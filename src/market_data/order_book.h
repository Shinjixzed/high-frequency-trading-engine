#pragma once

#include "../core/types.h"
#include "../core/memory.h"
#include "../core/timing.h"

#include <unordered_map>
#include <vector>
#include <algorithm>
#include <shared_mutex>
#include <mutex>

namespace trading_engine {
    // High-performance order book implementation
    template<size_t MaxLevels = 1000>
    class OrderBook {
        struct Level {
            Price price;
            Quantity quantity;
            uint32_t order_count;
        };

        struct alignas(CacheLineSize) BookSide {
            std::array<Level, MaxLevels> levels;
            std::atomic<uint32_t> level_count{0};

            // Price-to-index mapping for O(1) lookup
            std::unordered_map<Price, uint16_t> price_to_index;
            mutable std::shared_mutex index_mutex;
        };

        BookSide bids; // Sorted descending (highest first)
        BookSide asks; // Sorted ascending (lowest first)

        alignas(CacheLineSize) std::atomic<std::uint64_t> version{0};
        alignas(CacheLineSize) std::atomic<Price> best_bid{0};
        alignas(CacheLineSize) std::atomic<Price> best_ask{UINT64_MAX};
        alignas(CacheLineSize) std::atomic<Quantity> best_bid_qty{0};
        alignas(CacheLineSize) std::atomic<Quantity> best_ask_qty{0};

    public:
        void update_level(Side side, Price price, Quantity quantity) noexcept {
            MEASURE_LATENCY(LatencyProfiler::Order_processing); {
                BookSide &book_side = (side == Side::Buy) ? bids : asks;
                std::unique_lock lock(book_side.index_mutex);

                auto it = book_side.price_to_index.find(price);
                if (it != book_side.price_to_index.end()) {
                    // Update existing level
                    uint16_t index = it->second;
                    if (quantity == 0) {
                        remove_level(book_side, index, price);
                    } else {
                        book_side.levels[index].quantity = quantity;
                    }
                } else if (quantity > 0) {
                    // Add new level
                    add_level(book_side, price, quantity);
                }
            }

            update_best_prices();
            version.fetch_add(1, std::memory_order_release);
        }

        struct BookSnapshot {
            Price best_bid_price;
            Price best_ask_price;
            Quantity best_bid_qty;
            Quantity best_ask_qty;
            std::uint64_t version;
            Timestamp timestamp;
        };

        BookSnapshot get_snapshot() const noexcept {
            return BookSnapshot{
                .best_bid_price = best_bid.load(std::memory_order_acquire),
                .best_ask_price = best_ask.load(std::memory_order_acquire),
                .best_bid_qty = best_bid_qty.load(std::memory_order_acquire),
                .best_ask_qty = best_ask_qty.load(std::memory_order_acquire),
                .version = version.load(std::memory_order_acquire),
                .timestamp = TimestampManager::get_hardware_timestamp()
            };
        }

        Price get_best_bid() const noexcept {
            return best_bid.load(std::memory_order_acquire);
        }

        Price get_best_ask() const noexcept {
            Price ask = best_ask.load(std::memory_order_acquire);
            return ask == UINT64_MAX ? 0 : ask;
        }

        Quantity get_bid_quantity(Price price) const noexcept {
            std::shared_lock lock(bids.index_mutex);
            auto it = bids.price_to_index.find(price);
            if (it != bids.price_to_index.end()) {
                return bids.levels[it->second].quantity;
            }
            return 0;
        }

        Quantity get_ask_quantity(Price price) const noexcept {
            std::shared_lock lock(asks.index_mutex);
            auto it = asks.price_to_index.find(price);
            if (it != asks.price_to_index.end()) {
                return asks.levels[it->second].quantity;
            }
            return 0;
        }

        // Get top N levels
        std::vector<Level> get_bid_levels(size_t depth = 10) const {
            std::shared_lock lock(bids.index_mutex);
            std::vector<Level> result;

            uint32_t count = std::min(static_cast<uint32_t>(depth),
                                      bids.level_count.load(std::memory_order_acquire));
            result.reserve(count);

            for (uint32_t i = 0; i < count; ++i) {
                result.push_back(bids.levels[i]);
            }

            return result;
        }

        std::vector<Level> get_ask_levels(size_t depth = 10) const {
            std::shared_lock lock(asks.index_mutex);
            std::vector<Level> result;

            uint32_t count = std::min(static_cast<uint32_t>(depth),
                                      asks.level_count.load(std::memory_order_acquire));
            result.reserve(count);

            for (uint32_t i = 0; i < count; ++i) {
                result.push_back(asks.levels[i]);
            }

            return result;
        }

        bool is_crossed() const noexcept {
            Price bid = best_bid.load(std::memory_order_acquire);
            Price ask = best_ask.load(std::memory_order_acquire);
            return bid > 0 && ask != UINT64_MAX && bid >= ask;
        }

        Price get_mid_price() const noexcept {
            Price bid = best_bid.load(std::memory_order_acquire);
            Price ask = best_ask.load(std::memory_order_acquire);

            if (bid > 0 && ask != UINT64_MAX) {
                return (bid + ask) / 2;
            }
            return 0;
        }

        double get_spread_bps() const noexcept {
            Price bid = best_bid.load(std::memory_order_acquire);
            Price ask = best_ask.load(std::memory_order_acquire);

            if (bid > 0 && ask != UINT64_MAX) {
                Price mid = (bid + ask) / 2;
                return (static_cast<double>(ask - bid) / mid) * 10000.0; // Basis points
            }
            return 0.0;
        }

    private:
        void add_level(BookSide &book_side, Price price, Quantity quantity) {
            uint32_t current_count = book_side.level_count.load(std::memory_order_acquire);

            if (UNLIKELY(current_count >= MaxLevels)) {
                return; // Book is full
            }

            // Find insertion point to maintain sorted order
            uint32_t insert_index = 0;
            bool is_bid_side = (&book_side == &bids);

            for (uint32_t i = 0; i < current_count; ++i) {
                if (is_bid_side) {
                    // Bids: descending order (highest first)
                    if (price > book_side.levels[i].price) {
                        insert_index = i;
                        break;
                    }
                    insert_index = i + 1;
                } else {
                    // Asks: ascending order (lowest first)
                    if (price < book_side.levels[i].price) {
                        insert_index = i;
                        break;
                    }
                    insert_index = i + 1;
                }
            }

            // Shift elements to make room
            for (uint32_t i = current_count; i > insert_index; --i) {
                book_side.levels[i] = book_side.levels[i - 1];

                // Update index mapping
                Price shifted_price = book_side.levels[i].price;
                book_side.price_to_index[shifted_price] = i;
            }

            // Insert new level
            book_side.levels[insert_index] = Level{
                .price = price,
                .quantity = quantity,
                .order_count = 1
            };

            book_side.price_to_index[price] = insert_index;
            book_side.level_count.store(current_count + 1, std::memory_order_release);
        }

        void remove_level(BookSide &book_side, uint16_t index, Price price) {
            uint32_t current_count = book_side.level_count.load(std::memory_order_acquire);

            if (index >= current_count) {
                return;
            }

            // Shift elements to fill the gap
            for (uint32_t i = index; i < current_count - 1; ++i) {
                book_side.levels[i] = book_side.levels[i + 1];

                // Update index mapping
                Price shifted_price = book_side.levels[i].price;
                book_side.price_to_index[shifted_price] = i;
            }

            book_side.price_to_index.erase(price);
            book_side.level_count.store(current_count - 1, std::memory_order_release);
        }

        void update_best_prices() noexcept {
            // Update best bid
            uint32_t bid_count = bids.level_count.load(std::memory_order_acquire);
            if (bid_count > 0) {
                best_bid.store(bids.levels[0].price, std::memory_order_release);
                best_bid_qty.store(bids.levels[0].quantity, std::memory_order_release);
            } else {
                best_bid.store(0, std::memory_order_release);
                best_bid_qty.store(0, std::memory_order_release);
            }

            // Update best ask
            uint32_t ask_count = asks.level_count.load(std::memory_order_acquire);
            if (ask_count > 0) {
                best_ask.store(asks.levels[0].price, std::memory_order_release);
                best_ask_qty.store(asks.levels[0].quantity, std::memory_order_release);
            } else {
                best_ask.store(UINT64_MAX, std::memory_order_release);
                best_ask_qty.store(0, std::memory_order_release);
            }
        }
    };

    // Order book manager for multiple symbols
    class OrderBookManager {
        std::unordered_map<SymbolID, std::unique_ptr<OrderBook<> > > order_books;
        mutable std::shared_mutex books_mutex;

    public:
        OrderBook<> *get_order_book(SymbolID symbol_id) {
            std::shared_lock lock(books_mutex);

            auto it = order_books.find(symbol_id);
            if (it != order_books.end()) {
                return it->second.get();
            }

            return nullptr;
        }

        OrderBook<> *get_or_create_order_book(SymbolID symbol_id) {
            // Try shared lock first
            {
                std::shared_lock lock(books_mutex);
                auto it = order_books.find(symbol_id);
                if (it != order_books.end()) {
                    return it->second.get();
                }
            }

            // Need to create new order book
            std::unique_lock lock(books_mutex);

            // Double-check in case another thread created it
            auto it = order_books.find(symbol_id);
            if (it != order_books.end()) {
                return it->second.get();
            }

            auto book = std::make_unique<OrderBook<> >();
            OrderBook<> *book_ptr = book.get();
            order_books[symbol_id] = std::move(book);

            return book_ptr;
        }

        void process_market_data(const MarketTick &tick) {
            if (OrderBook<> *book = get_or_create_order_book(tick.symbol_id)) {
                book->update_level(tick.side, tick.price, tick.quantity);
            }
        }

        std::vector<SymbolID> get_active_symbols() const {
            std::shared_lock lock(books_mutex);
            std::vector<SymbolID> symbols;
            symbols.reserve(order_books.size());

            for (const auto &[symbol_id, book]: order_books) {
                symbols.push_back(symbol_id);
            }

            return symbols;
        }

        size_t get_book_count() const {
            std::shared_lock lock(books_mutex);
            return order_books.size();
        }
    };
}
