#pragma once

#include "../core/types.h"
#include "../core/memory.h"
#include "../core/timing.h"

#include <atomic>
#include <unordered_map>
#include <chrono>
#include <shared_mutex>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <tuple>

namespace trading_engine {
    class RiskManager {
    public:
        enum class RiskResult {
            approved,
            rejected_position_limit,
            rejected_notional_limit,
            rejected_rate_limit,
            rejected_loss_limit,
            rejected_order_size,
            rejected_price_limit
        };

    private:
        struct alignas(CacheLineSize) RiskLimits {
            std::atomic<Quantity> max_position{1000000};
            std::atomic<Value> max_notional{10000000 * PriceScale};
            std::atomic<std::uint32_t> max_orders_per_second{1000};
            std::atomic<Value> max_loss_per_day{100000 * PriceScale};
            std::atomic<Quantity> max_order_size{100000};
            std::atomic<Price> max_price_deviation{to_scaled_price(10.0)}; // 10$ from reference

            // Custom assignment to copy atomic values
            void copy_from(const RiskLimits &other) {
                max_position.store(other.max_position.load());
                max_notional.store(other.max_notional.load());
                max_orders_per_second.store(other.max_orders_per_second.load());
                max_loss_per_day.store(other.max_loss_per_day.load());
                max_order_size.store(other.max_order_size.load());
                max_price_deviation.store(other.max_price_deviation.load());
            }
        };

        struct alignas(CacheLineSize) PositionTracker {
            std::atomic<std::int64_t> current_position{0}; // Signed for long/short
            std::atomic<Value> current_notional{0};
            std::atomic<std::int64_t> realized_pnl{0}; // Signed PnL
            std::atomic<std::uint32_t> order_count_today{0};
            std::atomic<Price> vwap{0}; // Volume weighted average price
            std::atomic<Quantity> total_volume{0};
        };

        RiskLimits global_limits;
        std::unordered_map<SymbolID, PositionTracker> positions;
        std::unordered_map<SymbolID, RiskLimits> symbol_limits;
        mutable std::shared_mutex positions_mutex;

        // Rate limiting using token bucket
        struct RateLimiter {
            std::atomic<std::uint32_t> tokens{1000};
            std::atomic<std::uint64_t> last_refill_time{};
            std::uint32_t refill_rate; // tokens per second
            std::uint32_t bucket_size;

            RateLimiter() : refill_rate(1000), bucket_size(1000) {
                last_refill_time.store(TimestampManager::get_hardware_timestamp());
            }

            RateLimiter(std::uint32_t rate, std::uint32_t size);

            // Make it movable and copyable
            RateLimiter(const RateLimiter &other)
                : tokens(other.tokens.load()),
                  last_refill_time(other.last_refill_time.load()),
                  refill_rate(other.refill_rate),
                  bucket_size(other.bucket_size) {
            }

            RateLimiter &operator=(const RateLimiter &other) {
                if (this != &other) {
                    tokens.store(other.tokens.load());
                    last_refill_time.store(other.last_refill_time.load());
                    refill_rate = other.refill_rate;
                    bucket_size = other.bucket_size;
                }
                return *this;
            }

            RateLimiter(RateLimiter &&other) noexcept
                : tokens(other.tokens.load()),
                  last_refill_time(other.last_refill_time.load()),
                  refill_rate(other.refill_rate),
                  bucket_size(other.bucket_size) {
            }

            RateLimiter &operator=(RateLimiter &&other) noexcept {
                if (this != &other) {
                    tokens.store(other.tokens.load());
                    last_refill_time.store(other.last_refill_time.load());
                    refill_rate = other.refill_rate;
                    bucket_size = other.bucket_size;
                }
                return *this;
            }
        };

        RateLimiter global_rate_limiter;
        std::unordered_map<SymbolID, RateLimiter> symbol_rate_limiters;

        // Reference prices for price deviation checks
        std::unordered_map<SymbolID, std::atomic<Price> > reference_prices;

    public:
        RiskManager() = default;

        void initialize() {
            // Initialize with default limits
            global_limits.max_position.store(1000000, std::memory_order_relaxed);
            global_limits.max_notional.store(10000000 * PriceScale, std::memory_order_relaxed);
            global_limits.max_orders_per_second.store(1000, std::memory_order_relaxed);
            global_limits.max_loss_per_day.store(100000 * PriceScale, std::memory_order_relaxed);
            global_limits.max_order_size.store(100000, std::memory_order_relaxed);
        }

        RiskResult check_order(const Order &order) noexcept {
            MEASURE_LATENCY(LatencyProfiler::Risk_check);

            // Global rate limiting check
            if (!check_rate_limit(global_rate_limiter)) {
                return RiskResult::rejected_rate_limit;
            }

            // Symbol-specific rate limiting
            if (auto &symbol_limiter = get_or_create_symbol_limiter(order.symbolID);
                !check_rate_limit(symbol_limiter)) {
                return RiskResult::rejected_rate_limit;
            }

            // Order size check
            if (order.quantity > global_limits.max_order_size.load(std::memory_order_relaxed)) {
                return RiskResult::rejected_order_size;
            }

            // Price deviation check
            if (!check_price_deviation(order)) {
                return RiskResult::rejected_price_limit;
            }

            // Position and notional checks
            std::shared_lock lock(positions_mutex);
            const PositionTracker &position = positions[order.symbolID];

            // Calculate new position
            const std::int64_t position_change = order.side == Side::Buy
                                                     ? static_cast<std::int64_t>(order.quantity)
                                                     : -static_cast<std::int64_t>(order.quantity);
            const std::int64_t new_position = position.current_position.load(std::memory_order_relaxed) +
                                              position_change;

            // Position limit check
            if (static_cast<std::uint64_t>(std::abs(new_position)) > global_limits.max_position.load(
                    std::memory_order_relaxed)) {
                return RiskResult::rejected_position_limit;
            }

            // Notional limit check
            const Value order_notional = calculate_notional(order.price, order.quantity);
            const Value current_notional = position.current_notional.load(std::memory_order_relaxed);

            // For position increases, check if we exceed notional limits
            if ((new_position > 0 && position_change > 0) || (new_position < 0 && position_change < 0)) {
                if (const Value new_notional = current_notional + order_notional;
                    new_notional > global_limits.max_notional.load(std::memory_order_relaxed)) {
                    return RiskResult::rejected_notional_limit;
                }
            }

            // Loss limit check
            const std::int64_t current_pnl = position.realized_pnl.load(std::memory_order_relaxed);
            if (current_pnl < -static_cast<std::int64_t>(global_limits.max_loss_per_day.
                    load(std::memory_order_relaxed))) {
                return RiskResult::rejected_loss_limit;
            }

            return RiskResult::approved;
        }

        void update_position(const Trade &trade) noexcept {
            std::unique_lock lock(positions_mutex);
            PositionTracker &position = positions[trade.symbol_id];

            // Determine position change based on which side we were on
            std::int64_t position_change = 0;
            Value notional_change = 0;

            // This is simplified - in reality, we'd need to track which orders we own
            // For now, assume we're always the aggressor
            if (trade.aggressor_side == Side::Buy) {
                position_change = static_cast<std::int64_t>(trade.quantity);
                notional_change = calculate_notional(trade.price, trade.quantity);
            } else {
                position_change = -static_cast<std::int64_t>(trade.quantity);
                notional_change = calculate_notional(trade.price, trade.quantity);
            }

            // Update position
            const std::int64_t old_position = position.current_position.fetch_add(
                position_change, std::memory_order_relaxed);

            // Update VWAP
            update_vwap(position, trade.price, trade.quantity);

            // Calculate and update PnL if position is being reduced
            if ((old_position > 0 && position_change < 0) || (old_position < 0 && position_change > 0)) {
                const std::int64_t pnl_change = calculate_pnl_change(position, trade.price, std::abs(position_change));
                position.realized_pnl.fetch_add(pnl_change, std::memory_order_relaxed);
            }

            // Update notional (only for position increases)
            if ((old_position >= 0 && position_change > 0) || (old_position <= 0 && position_change < 0)) {
                position.current_notional.fetch_add(notional_change, std::memory_order_relaxed);
            } else {
                // Position reduction - reduce notional proportionally
                if (const Value current_notional = position.current_notional.load(std::memory_order_relaxed);
                    current_notional > 0) {
                    const Value notional_reduction =
                            notional_change * current_notional / (std::abs(old_position) * trade.price / PriceScale);
                    position.current_notional.fetch_sub(
                        std::min(notional_reduction, current_notional), std::memory_order_relaxed);
                }
            }
        }

        void update_reference_price(SymbolID symbol_id, Price price) {
            reference_prices[symbol_id].store(price, std::memory_order_relaxed);
        }

        void set_global_limits(const RiskLimits &limits) {
            global_limits.copy_from(limits);
        }

        void set_symbol_limits(SymbolID symbol_id, const RiskLimits &limits) {
            symbol_limits[symbol_id].copy_from(limits);
        }

        struct PositionInfo {
            std::int64_t position;
            Value notional;
            std::int64_t pnl;
            Price vwap;
            std::uint32_t order_count;
        };

        PositionInfo get_position_info(SymbolID symbol_id) const {
            std::shared_lock lock(positions_mutex);

            const auto it = positions.find(symbol_id);
            if (it == positions.end()) {
                return PositionInfo{0, 0, 0, 0, 0};
            }

            const PositionTracker &pos = it->second;
            return PositionInfo{
                .position = pos.current_position.load(std::memory_order_relaxed),
                .notional = pos.current_notional.load(std::memory_order_relaxed),
                .pnl = pos.realized_pnl.load(std::memory_order_relaxed),
                .vwap = pos.vwap.load(std::memory_order_relaxed),
                .order_count = pos.order_count_today.load(std::memory_order_relaxed)
            };
        }

        struct RiskStats {
            std::uint32_t total_orders_checked;
            std::uint32_t orders_approved;
            std::uint32_t orders_rejected;
            double approval_rate;
        };

        static RiskStats get_statistics() {
            // Implementation would track these metrics
            return RiskStats{0, 0, 0, 0.0};
        }

    private:
        static bool check_rate_limit(RateLimiter &limiter) noexcept {
            // Refill tokens based on elapsed time
            const std::uint64_t current_time = TimestampManager::get_hardware_timestamp();
            const std::uint64_t last_time = limiter.last_refill_time.load(std::memory_order_relaxed);

            if (current_time > last_time) {
                const std::uint64_t elapsed_ns = TimestampManager::tsc_to_nanoseconds(current_time - last_time).count();
                const auto tokens_to_add = static_cast<std::uint32_t>(elapsed_ns * limiter.refill_rate / 1000000000ULL);

                if (tokens_to_add > 0) {
                    const std::uint32_t current_tokens = limiter.tokens.load(std::memory_order_relaxed);
                    const std::uint32_t new_tokens = std::min(current_tokens + tokens_to_add, limiter.bucket_size);
                    limiter.tokens.store(new_tokens, std::memory_order_relaxed);
                    limiter.last_refill_time.store(current_time, std::memory_order_relaxed);
                }
            }

            // Try to consume a token
            std::uint32_t current_tokens = limiter.tokens.load(std::memory_order_relaxed);
            while (current_tokens > 0) {
                if (limiter.tokens.compare_exchange_weak(
                    current_tokens, current_tokens - 1, std::memory_order_relaxed)) {
                    return true;
                }
            }

            return false;
        }

        RateLimiter &get_or_create_symbol_limiter(SymbolID symbol_id) {
            const auto it = symbol_rate_limiters.find(symbol_id);
            if (it == symbol_rate_limiters.end()) {
                const auto result = symbol_rate_limiters.emplace(
                    std::piecewise_construct,
                    std::forward_as_tuple(symbol_id),
                    std::forward_as_tuple(100, 100)
                ); // Per-symbol limits
                return result.first->second;
            }
            return it->second;
        }

        bool check_price_deviation(const Order &order) const {
            const auto it = reference_prices.find(order.symbolID);
            if (it == reference_prices.end()) {
                return true; // No reference price set, allow order
            }

            const Price ref_price = it->second.load(std::memory_order_relaxed);
            if (ref_price == 0) {
                return true; // No valid reference price
            }

            const Price max_deviation = global_limits.max_price_deviation.load(std::memory_order_relaxed);
            const Price price_diff = (order.price > ref_price) ? (order.price - ref_price) : (ref_price - order.price);

            return price_diff <= max_deviation;
        }

        static void update_vwap(PositionTracker &position, Price price, Quantity quantity) {
            const Quantity old_volume = position.total_volume.fetch_add(quantity, std::memory_order_relaxed);
            const Price old_vwap = position.vwap.load(std::memory_order_relaxed);

            // Calculate new VWAP: (old_vwap * old_volume + price * quantity) / (old_volume + quantity)
            const Value old_total_value = old_vwap * old_volume / PriceScale;
            const Value new_value = price * quantity / PriceScale;
            const auto new_vwap = (old_total_value + new_value) * PriceScale / (old_volume + quantity);

            position.vwap.store(new_vwap, std::memory_order_relaxed);
        }

        static std::int64_t calculate_pnl_change(const PositionTracker &position, Price exit_price, Quantity quantity) {
            Price entry_vwap = position.vwap.load(std::memory_order_relaxed);
            if (entry_vwap == 0) {
                return 0;
            }

            // PnL = (exit_price - entry_price) * quantity for long positions
            // For short positions, it's (entry_price - exit_price) * quantity
            const std::int64_t current_pos = position.current_position.load(std::memory_order_relaxed);

            if (current_pos > 0) {
                // Long position being reduced
                return static_cast<std::int64_t>((exit_price - entry_vwap) * quantity / PriceScale);
            }
            // Short position being reduced
            return static_cast<std::int64_t>((entry_vwap - exit_price) * quantity / PriceScale);
        }
    };

    inline RiskManager::RateLimiter::RateLimiter(std::uint32_t rate, std::uint32_t size) : refill_rate(rate),
        bucket_size(size) {
        last_refill_time.store(TimestampManager::get_hardware_timestamp());
    }
}
