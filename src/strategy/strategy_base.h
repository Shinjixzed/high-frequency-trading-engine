#pragma once

#include "../core/types.h"
#include "../core/memory.h"
#include "../core/queue.h"
#include "../core/timing.h"
#include "../market_data/order_book.h"

#include "strategy_interface.h"
#include <atomic>
#include <functional>
#include <cmath>

namespace trading_engine {
    enum class StrategySignal {
        None,
        Buy,
        Sell,
        CancelAll,
        ReducePosition
    };

    // Base strategy interface
    template<typename StrategyImpl>
    class StrategyBase : public IStrategy {
    protected:
        SymbolID symbol_id;
        SPSCQueue<MarketTick, 1024> tick_queue;
        SPSCQueue<Trade, 256> trade_queue;
        SPSCQueue<OrderBook<>::BookSnapshot, 128> snapshot_queue;

        // Strategy state
        struct StrategyState {
            Price last_price{0};
            int64_t position{0}; // Signed position
            uint64_t signal_count{0};
            Timestamp last_signal_time{0};
            std::atomic<bool> enabled{true};
        };

        StrategyState state;

        // Callbacks for order submission
        std::function<void(const Order &)> order_callback;
        std::function<void(OrderID)> cancel_callback;

    public:
        explicit StrategyBase(SymbolID symbol);

        ~StrategyBase() override = default;

        void set_order_callback(std::function<void(const Order &)> callback) {
            order_callback = std::move(callback);
        }

        void set_cancel_callback(std::function<void(OrderID)> callback) {
            cancel_callback = std::move(callback);
        }

        void on_market_data(const MarketTick &tick) override {
            if (!state.enabled.load(std::memory_order_acquire)) {
                return;
            }

            if (!tick_queue.try_push(tick)) {
                handle_data_overflow();
            }
        }

        void on_trade(const Trade &trade) override {
            if (!trade_queue.try_push(trade)) {
                handle_trade_overflow();
            }
        }

        void on_book_snapshot(const OrderBook<>::BookSnapshot &snapshot) override {
            if (!snapshot_queue.try_push(snapshot)) {
                // Snapshots are less critical, just drop
            }
        }

        void process_signals() override {
            MEASURE_LATENCY(LatencyProfiler::Strategy_signal);

            // Process market data updates
            MarketTick tick{};
            while (tick_queue.try_pop(tick)) {
                static_cast<StrategyImpl *>(this)->process_tick(tick);
            }

            // Process trade updates
            Trade trade{};
            while (trade_queue.try_pop(trade)) {
                static_cast<StrategyImpl *>(this)->process_trade(trade);
            }

            // Process book snapshots
            OrderBook<>::BookSnapshot
                    snapshot{};
            while (snapshot_queue.try_pop(snapshot)) {
                static_cast<StrategyImpl *>(this)->process_snapshot(snapshot);
            }
        }

        void enable() { state.enabled.store(true, std::memory_order_release); }
        void disable() { state.enabled.store(false, std::memory_order_release); }
        [[nodiscard]] bool is_enabled() const override { return state.enabled.load(std::memory_order_acquire); }

        [[nodiscard]] SymbolID get_symbol_id() const override { return symbol_id; }
        [[nodiscard]] std::int64_t get_position() const { return state.position; }
        [[nodiscard]] std::uint64_t get_signal_count() const { return state.signal_count; }

    protected:
        void submit_order(Side side, Price price, Quantity quantity, OrderType type = OrderType::Limit);

        void cancel_order(OrderID order_id) const;

        virtual void handle_data_overflow() {
            // Default implementation - log error
        }

        virtual void handle_trade_overflow() {
            // Default implementation - log error
        }

    private:
        static OrderID generate_order_id() {
            static std::atomic<OrderID> counter{1};
            return counter.fetch_add(1, std::memory_order_relaxed);
        }
    };

    template<typename StrategyImpl>
    StrategyBase<StrategyImpl>::StrategyBase(const SymbolID symbol)
        : symbol_id(symbol), tick_queue(), trade_queue(), snapshot_queue() {
    }

    template<typename StrategyImpl>
    void StrategyBase<StrategyImpl>::submit_order(
        const Side side, const Price price, const Quantity quantity,
        const OrderType type
    ) {
        if (!order_callback) {
            return;
        }

        Order order{
            .orderID = generate_order_id(),
            .symbolID = symbol_id,
            .side = side,
            .orderType = type,
            .TimeInForce = TimeInForce::Ioc,
            .price = price,
            .quantity = quantity,
            .filledQuantity = 0,
            .status = OrderStatus::Incoming,
            .timestamp = TimestampManager::get_hardware_timestamp()
        };

        order_callback(order);
        ++state.signal_count;
        state.last_signal_time = order.timestamp;
    }

    template<typename StrategyImpl>
    void StrategyBase<StrategyImpl>::cancel_order(const OrderID order_id) const {
        if (cancel_callback) {
            cancel_callback(order_id);
        }
    }

    // Mean reversion strategy implementation
    class MeanReversionStrategy final : public StrategyBase<MeanReversionStrategy> {
        struct Parameters {
            double lookback_period = 20.0;
            double entry_threshold = 2.0; // Standard deviations
            double exit_threshold = 0.5;
            Quantity max_position = 1000;
            double min_spread_bps = 5.0; // Minimum spread to trade
            uint64_t min_signal_interval_ns = 1000000; // 1ms minimum between signals
        };

        Parameters params;
        CircularBuffer<Price, 128> price_history;
        std::atomic<double> current_mean{0.0};
        std::atomic<double> current_std{0.0};

    public:
        explicit MeanReversionStrategy(SymbolID symbol) : StrategyBase(symbol), price_history() {
        }

        void set_parameters(const Parameters &new_params) {
            params = new_params;
        }

        void process_tick(const MarketTick &tick) {
            state.last_price = tick.price;
            price_history.push(tick.price);

            if (price_history.size() >= static_cast<size_t>(params.lookback_period)) {
                update_statistics();

                const double mean = current_mean.load(std::memory_order_relaxed);
                const double std_dev = current_std.load(std::memory_order_relaxed);

                if (std_dev > 0) {
                    const double z_score = (from_scaled_price(tick.price) - mean) / std_dev;

                    if (const StrategySignal signal = generate_signal(z_score); signal != StrategySignal::None) {
                        execute_signal(signal, tick.price);
                    }
                }
            }
        }

        void process_trade(const Trade &trade) {
            // Update our position if this trade affects us
            // In a real implementation, we'd track our order IDs
            update_position_from_trade(trade);
        }

        void process_snapshot(const OrderBook<>::BookSnapshot &snapshot) const {
            // Check if spread is wide enough to trade
            if (snapshot.best_ask_price > snapshot.best_bid_price) {
                const Price spread = snapshot.best_ask_price - snapshot.best_bid_price;

                if (const Price mid = (snapshot.best_ask_price + snapshot.best_bid_price) / 2; mid > 0) {
                    if (const double spread_bps = (from_scaled_price(spread) / from_scaled_price(mid)) * 10000.0;
                        spread_bps < params.min_spread_bps) {
                        // Spread too tight, don't trade
                        return;
                    }
                }
            }
        }

        void shutdown() override {
        }

    private:
        void update_statistics() {
            if (price_history.size() < 2) {
                return;
            }

            // Calculate mean
            double sum = 0.0;
            const size_t count = price_history.size();

            for (size_t i = 0; i < count; ++i) {
                sum += from_scaled_price(price_history[i]);
            }

            const double mean = sum / count;
            current_mean.store(mean, std::memory_order_relaxed);

            // Calculate standard deviation
            double variance_sum = 0.0;
            for (size_t i = 0; i < count; ++i) {
                const double diff = from_scaled_price(price_history[i]) - mean;
                variance_sum += diff * diff;
            }

            const double std_dev = std::sqrt(variance_sum / count);
            current_std.store(std_dev, std::memory_order_relaxed);
        }

        [[nodiscard]] StrategySignal generate_signal(const double z_score) const {
            // Check minimum time between signals
            if (const Timestamp current_time = TimestampManager::get_hardware_timestamp();
                current_time - state.last_signal_time < params.min_signal_interval_ns) {
                return StrategySignal::None;
            }

            if (state.position == 0) {
                // No position - look for entry signals
                if (z_score < -params.entry_threshold) {
                    return StrategySignal::Buy; // Price below mean, expect reversion up
                }
                if (z_score > params.entry_threshold) {
                    return StrategySignal::Sell; // Price above mean, expect reversion down
                }
            } else if (state.position > 0) {
                // Long position - look for exit signal
                if (z_score > -params.exit_threshold) {
                    return StrategySignal::Sell;
                }
            } else if (state.position < 0) {
                // Short position - look for exit signal
                if (z_score < params.exit_threshold) {
                    return StrategySignal::Buy;
                }
            }

            return StrategySignal::None;
        }

        void execute_signal(const StrategySignal signal, const Price current_price) {
            const Quantity order_size = calculate_order_size(signal);
            if (order_size == 0) {
                return;
            }

            switch (signal) {
                case StrategySignal::Buy:
                    submit_order(Side::Buy, current_price, order_size, OrderType::Limit);
                    break;

                case StrategySignal::Sell:
                    submit_order(Side::Sell, current_price, order_size, OrderType::Limit);
                    break;

                default:
                    break;
            }
        }

        [[nodiscard]] Quantity calculate_order_size(const StrategySignal signal) const {
            constexpr Quantity base_size = 100; // Base order size

            if (signal == StrategySignal::Buy) {
                // Don't exceed max position
                if (state.position >= static_cast<int64_t>(params.max_position)) {
                    return 0;
                }
                return std::min(base_size, params.max_position - static_cast<Quantity>(state.position));
            }
            if (signal == StrategySignal::Sell) {
                // Don't exceed max short position
                if (state.position <= -static_cast<int64_t>(params.max_position)) {
                    return 0;
                }

                if (state.position > 0) {
                    // Closing long position
                    return std::min(base_size, static_cast<Quantity>(state.position));
                }
                // Opening/increasing short position
                return std::min(base_size, params.max_position - static_cast<Quantity>(-state.position));
            }
            return 0;
        }

        void update_position_from_trade(const Trade &trade) {
            // Simplified position tracking
            // In reality, this would be more sophisticated
            if (trade.symbol_id == symbol_id) {
                // Assume we're always the aggressor for simplicity
                if (trade.aggressor_side == Side::Buy) {
                    state.position += static_cast<int64_t>(trade.quantity);
                } else {
                    state.position -= static_cast<int64_t>(trade.quantity);
                }
            }
        }
    };

    // Arbitrage strategy for cross-exchange opportunities
    class ArbitrageStrategy final : public StrategyBase<ArbitrageStrategy> {
        struct ArbitrageParams {
            double min_profit_bps = 10.0; // Minimum profit in basis points
            Quantity max_position = 500;
            uint64_t max_hold_time_ns = 5000000; // 5ms maximum hold time
        };

        ArbitrageParams params;

        // Track prices from different sources/exchanges
        std::atomic<Price> exchange_a_bid{0};
        std::atomic<Price> exchange_a_ask{0};
        std::atomic<Price> exchange_b_bid{0};
        std::atomic<Price> exchange_b_ask{0};

    public:
        explicit ArbitrageStrategy(SymbolID symbol) : StrategyBase(symbol) {
        }

        void set_exchange_a_prices(Price bid, Price ask) {
            exchange_a_bid.store(bid, std::memory_order_relaxed);
            exchange_a_ask.store(ask, std::memory_order_relaxed);
            check_arbitrage_opportunity();
        }

        void set_exchange_b_prices(Price bid, Price ask) {
            exchange_b_bid.store(bid, std::memory_order_relaxed);
            exchange_b_ask.store(ask, std::memory_order_relaxed);
            check_arbitrage_opportunity();
        }

        void process_tick(const MarketTick &tick) {
            // Update prices based on the tick source
            // This is simplified - real implementation would track exchange IDs
            state.last_price = tick.price;
        }

        void process_trade(const Trade &trade) {
            update_position_from_trade(trade);
        }

        static void process_snapshot(OrderBook<1000>::BookSnapshot & /*snapshot*/) {
            // Use snapshot data to update one of our exchange feeds
        }

        void shutdown() override {
        }

    private:
        void check_arbitrage_opportunity() {
            const Price a_bid = exchange_a_bid.load(std::memory_order_relaxed);
            const Price a_ask = exchange_a_ask.load(std::memory_order_relaxed);
            const Price b_bid = exchange_b_bid.load(std::memory_order_relaxed);
            const Price b_ask = exchange_b_ask.load(std::memory_order_relaxed);

            if (a_bid == 0 || a_ask == 0 || b_bid == 0 || b_ask == 0) {
                return; // Missing price data
            }

            // Check for arbitrage: buy low on one exchange, sell high on another
            if (a_bid > b_ask) {
                // Buy on B, sell on A
                double profit_bps = ((from_scaled_price(a_bid) - from_scaled_price(b_ask)) /
                                     from_scaled_price(b_ask)) * 10000.0;

                if (profit_bps >= params.min_profit_bps) {
                    execute_arbitrage(Side::Buy, b_ask, Side::Sell, a_bid);
                }
            } else if (b_bid > a_ask) {
                // Buy on A, sell on B
                double profit_bps = ((from_scaled_price(b_bid) - from_scaled_price(a_ask)) /
                                     from_scaled_price(a_ask)) * 10000.0;

                if (profit_bps >= params.min_profit_bps) {
                    execute_arbitrage(Side::Buy, a_ask, Side::Sell, b_bid);
                }
            }
        }

        void execute_arbitrage(Side buy_side, Price buy_price, Side sell_side, Price sell_price);

        void update_position_from_trade(const Trade &trade) {
            // Update position tracking
            if (trade.symbol_id == symbol_id) {
                if (trade.aggressor_side == Side::Buy) {
                    state.position += static_cast<int64_t>(trade.quantity);
                } else {
                    state.position -= static_cast<int64_t>(trade.quantity);
                }
            }
        }
    };

    inline void ArbitrageStrategy::execute_arbitrage(
        const Side buy_side, const Price buy_price, const Side sell_side,
        Price sell_price
    ) {
        const Quantity size = std::min(
            params.max_position,
            static_cast<Quantity>(params.max_position - std::abs(state.position))
        );

        if (size > 0) {
            // In a real implementation, these would go to different exchanges
            submit_order(buy_side, buy_price, size, OrderType::Limit);
            submit_order(sell_side, sell_price, size, OrderType::Limit);
        }
    }
}
