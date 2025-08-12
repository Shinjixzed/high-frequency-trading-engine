#pragma once

#include "../core/types.h"
#include "../core/memory.h"
#include "../core/timing.h"
#include "../risk/risk_manager.h"

#include <unordered_map>
#include <vector>
#include <algorithm>
#include <map>
#include <functional>

namespace trading_engine {
    // Order matching engine
    class MatchingEngine {
        struct OrderEntry {
            Order order;
            OrderEntry *next{nullptr};
            OrderEntry *prev{nullptr};

            OrderEntry();

            explicit OrderEntry(const Order &o);
        };

        // Price level containing orders at the same price
        struct PriceLevel {
            Price price;
            Quantity total_quantity{0};
            uint32_t order_count{0};
            OrderEntry *first_order{nullptr};
            OrderEntry *last_order{nullptr};

            explicit PriceLevel(const Price p) : price(p) {
            }

            void add_order(OrderEntry *entry) {
                if (last_order == nullptr) {
                    first_order = last_order = entry;
                    entry->next = entry->prev = nullptr;
                } else {
                    last_order->next = entry;
                    entry->prev = last_order;
                    entry->next = nullptr;
                    last_order = entry;
                }

                total_quantity += entry->order.quantity;
                order_count++;
            }

            void remove_order(const OrderEntry *entry) {
                if (entry->prev) {
                    entry->prev->next = entry->next;
                } else {
                    first_order = entry->next;
                }

                if (entry->next) {
                    entry->next->prev = entry->prev;
                } else {
                    last_order = entry->prev;
                }

                total_quantity -= entry->order.quantity;
                order_count--;
            }

            [[nodiscard]] bool empty() const {
                return order_count == 0;
            }
        };

        // Order book sides
        using price_level_map = std::map<Price, std::unique_ptr<PriceLevel> >;

        price_level_map bid_levels; // Descending order (highest first)
        price_level_map ask_levels; // Ascending order (lowest first)

        // Order tracking
        std::unordered_map<OrderID, OrderEntry *> order_lookup;
        LockFreeMemoryPool<OrderEntry, 10000> order_pool;
        LockFreeMemoryPool<Trade, 1000> trade_pool;

        // Statistics
        std::atomic<std::uint64_t> total_orders_processed{0};
        std::atomic<std::uint64_t> total_trades_generated{0};
        std::atomic<std::uint64_t> total_volume_matched{0};

        // Callbacks
        std::function<void(const Trade &)> trade_callback;
        std::function<void(const Order &)> order_update_callback;

    public:
        struct MatchResult {
            std::vector<Trade *> trades;
            bool fully_matched{false};
            OrderEntry *remaining_order{nullptr};
        };

        void set_trade_callback(std::function<void(const Trade &)> callback) {
            trade_callback = std::move(callback);
        }

        void set_order_update_callback(std::function<void(const Order &)> callback) {
            order_update_callback = std::move(callback);
        }

        MatchResult process_order(const Order &incoming_order) {
            MEASURE_LATENCY(LatencyProfiler::Order_matching);

            total_orders_processed.fetch_add(1, std::memory_order_relaxed);

            MatchResult result;

            if (incoming_order.side == Side::Buy) {
                result = match_buy_order(incoming_order);
            } else {
                result = match_sell_order(incoming_order);
            }

            // Add remaining quantity to book if not fully matched
            if (!result.fully_matched && result.remaining_order != nullptr) {
                add_order_to_book(result.remaining_order);
            }

            return result;
        }

        bool cancel_order(const OrderID order_id) {
            const auto it = order_lookup.find(order_id);
            if (it == order_lookup.end()) {
                return false; // Order not found
            }

            OrderEntry *entry = it->second;
            remove_order_from_book(entry);

            // Update order status
            entry->order.status = OrderStatus::Cancelled;
            if (order_update_callback) {
                order_update_callback(entry->order);
            }

            order_pool.release(entry);
            order_lookup.erase(it);

            return true;
        }

        struct BookState {
            Price best_bid{0};
            Price best_ask{0};
            Quantity best_bid_qty{0};
            Quantity best_ask_qty{0};
            uint32_t bid_levels_count{0};
            uint32_t ask_levels_count{0};
        };

        BookState get_book_state() const {
            BookState state;

            if (!bid_levels.empty()) {
                const auto it = bid_levels.rbegin(); // Highest bid (reverse iterator)
                state.best_bid = it->first;
                state.best_bid_qty = it->second->total_quantity;
            }

            if (!ask_levels.empty()) {
                auto it = ask_levels.begin(); // Lowest ask
                state.best_ask = it->first;
                state.best_ask_qty = it->second->total_quantity;
            }

            state.bid_levels_count = static_cast<uint32_t>(bid_levels.size());
            state.ask_levels_count = static_cast<uint32_t>(ask_levels.size());

            return state;
        }

        struct MatchingStats {
            std::uint64_t total_orders;
            std::uint64_t total_trades;
            std::uint64_t total_volume;
            double match_rate;
            double average_fill_size;
        };

        MatchingStats get_statistics() const {
            const std::uint64_t orders = total_orders_processed.load(std::memory_order_relaxed);
            const std::uint64_t trades = total_trades_generated.load(std::memory_order_relaxed);
            const std::uint64_t volume = total_volume_matched.load(std::memory_order_relaxed);

            return MatchingStats{
                .total_orders = orders,
                .total_trades = trades,
                .total_volume = volume,
                .match_rate = orders > 0 ? static_cast<double>(trades) / orders : 0.0,
                .average_fill_size = trades > 0 ? static_cast<double>(volume) / trades : 0.0
            };
        }

    private:
        MatchResult match_buy_order(const Order &buy_order) {
            MatchResult result;
            result.fully_matched = false;

            Quantity remaining_qty = buy_order.quantity;
            Order working_order = buy_order;

            // Match against ask levels (lowest price first)
            auto ask_it = ask_levels.begin();
            while (ask_it != ask_levels.end() && ask_it->first <= buy_order.price && remaining_qty > 0) {
                PriceLevel *level = ask_it->second.get();
                const Price level_price = ask_it->first;

                // Match against orders at this price level (FIFO)
                OrderEntry *sell_order = level->first_order;
                while (sell_order != nullptr && remaining_qty > 0) {
                    OrderEntry *next_order = sell_order->next; // Save before potential removal

                    Quantity trade_qty = std::min(remaining_qty, sell_order->order.quantity);

                    // Create trade
                    if (Trade *trade = create_trade(working_order, sell_order->order, level_price, trade_qty)) {
                        result.trades.push_back(trade);

                        // Update quantities
                        remaining_qty -= trade_qty;
                        sell_order->order.quantity -= trade_qty;
                        sell_order->order.filledQuantity += trade_qty;
                        level->total_quantity -= trade_qty;

                        total_volume_matched.fetch_add(trade_qty, std::memory_order_relaxed);

                        // Update order status
                        if (sell_order->order.quantity == 0) {
                            sell_order->order.status = OrderStatus::Filled;
                            level->remove_order(sell_order);
                            order_lookup.erase(sell_order->order.orderID);
                            order_pool.release(sell_order);
                        } else {
                            sell_order->order.status = OrderStatus::PartiallyFilled;
                        }

                        // Notify callbacks
                        if (trade_callback) {
                            trade_callback(*trade);
                        }
                        if (order_update_callback) {
                            order_update_callback(sell_order->order);
                        }
                    }

                    sell_order = next_order;
                }

                // Remove empty price levels
                if (level->empty()) {
                    ask_it = ask_levels.erase(ask_it);
                } else {
                    ++ask_it;
                }
            }

            result.fully_matched = (remaining_qty == 0);

            // Create remaining order entry if not fully matched
            if (!result.fully_matched) {
                if (OrderEntry *entry = order_pool.acquire()) {
                    working_order.quantity = remaining_qty;
                    working_order.status = OrderStatus::Incoming;
                    entry->order = working_order;
                    result.remaining_order = entry;
                }
            }

            return result;
        }

        MatchResult match_sell_order(const Order &sell_order) {
            MatchResult result;
            result.fully_matched = false;

            Quantity remaining_qty = sell_order.quantity;
            Order working_order = sell_order;

            // Match against bid levels (highest price first)
            auto bid_it = bid_levels.rbegin();
            while (bid_it != bid_levels.rend() && bid_it->first >= sell_order.price && remaining_qty > 0) {
                PriceLevel *level = bid_it->second.get();
                const Price level_price = bid_it->first;

                // Match against orders at this price level (FIFO)
                OrderEntry *buy_order = level->first_order;
                while (buy_order != nullptr && remaining_qty > 0) {
                    OrderEntry *next_order = buy_order->next;

                    const Quantity trade_qty = std::min(remaining_qty, buy_order->order.quantity);

                    // Create trade
                    if (Trade *trade = create_trade(buy_order->order, working_order, level_price, trade_qty)) {
                        result.trades.push_back(trade);

                        // Update quantities
                        remaining_qty -= trade_qty;
                        buy_order->order.quantity -= trade_qty;
                        buy_order->order.filledQuantity += trade_qty;
                        level->total_quantity -= trade_qty;

                        total_volume_matched.fetch_add(trade_qty, std::memory_order_relaxed);

                        // Update order status
                        if (buy_order->order.quantity == 0) {
                            buy_order->order.status = OrderStatus::Filled;
                            level->remove_order(buy_order);
                            order_lookup.erase(buy_order->order.orderID);
                            order_pool.release(buy_order);
                        } else {
                            buy_order->order.status = OrderStatus::PartiallyFilled;
                        }

                        // Notify callbacks
                        if (trade_callback) {
                            trade_callback(*trade);
                        }
                        if (order_update_callback) {
                            order_update_callback(buy_order->order);
                        }
                    }

                    buy_order = next_order;
                }

                // Remove empty price levels
                if (level->empty()) {
                    // Convert reverse iterator to forward iterator for erase
                    const auto forward_it = std::next(bid_it).base();
                    bid_levels.erase(forward_it);
                    bid_it = bid_levels.rbegin(); // Restart from beginning
                } else {
                    ++bid_it;
                }
            }

            result.fully_matched = (remaining_qty == 0);

            // Create remaining order entry if not fully matched
            if (!result.fully_matched) {
                if (OrderEntry *entry = order_pool.acquire()) {
                    working_order.quantity = remaining_qty;
                    working_order.status = OrderStatus::Incoming;
                    entry->order = working_order;
                    result.remaining_order = entry;
                }
            }

            return result;
        }

        void add_order_to_book(OrderEntry *entry) {
            Price price = entry->order.price;

            if (entry->order.side == Side::Buy) {
                auto it = bid_levels.find(price);
                if (it == bid_levels.end()) {
                    auto level = std::make_unique<PriceLevel>(price);
                    level->add_order(entry);
                    bid_levels[price] = std::move(level);
                } else {
                    it->second->add_order(entry);
                }
            } else {
                if (const auto it = ask_levels.find(price); it == ask_levels.end()) {
                    auto level = std::make_unique<PriceLevel>(price);
                    level->add_order(entry);
                    ask_levels[price] = std::move(level);
                } else {
                    it->second->add_order(entry);
                }
            }

            order_lookup[entry->order.orderID] = entry;
        }

        void remove_order_from_book(const OrderEntry *entry) {
            Price price = entry->order.price;

            if (entry->order.side == Side::Buy) {
                if (const auto it = bid_levels.find(price); it != bid_levels.end()) {
                    it->second->remove_order(entry);
                    if (it->second->empty()) {
                        bid_levels.erase(it);
                    }
                }
            } else {
                auto it = ask_levels.find(price);
                if (it != ask_levels.end()) {
                    it->second->remove_order(entry);
                    if (it->second->empty()) {
                        ask_levels.erase(it);
                    }
                }
            }
        }

        Trade *create_trade(const Order &buy_order, const Order &sell_order,
                            Price trade_price, Quantity trade_qty) {
            Trade *trade = trade_pool.acquire();
            if (!trade) {
                return nullptr;
            }

            static std::atomic<TradeID> trade_id_generator{1};

            *trade = Trade{
                .trade_id = trade_id_generator.fetch_add(1, std::memory_order_relaxed),
                .buy_order_id = buy_order.orderID,
                .sell_order_id = sell_order.orderID,
                .symbol_id = buy_order.symbolID,
                .price = trade_price,
                .quantity = trade_qty,
                .timestamp = TimestampManager::get_hardware_timestamp(),
                .aggressor_side = determine_aggressor_side(buy_order, sell_order)
            };

            total_trades_generated.fetch_add(1, std::memory_order_relaxed);

            return trade;
        }

        static Side determine_aggressor_side(const Order &buy_order, const Order &sell_order) {
            // The more recent order is typically the aggressor
            return (buy_order.timestamp > sell_order.timestamp) ? Side::Buy : Side::Sell;
        }
    };

    inline MatchingEngine::OrderEntry::OrderEntry() = default;

    inline MatchingEngine::OrderEntry::OrderEntry(const Order &o) : order(o) {
    }
}
