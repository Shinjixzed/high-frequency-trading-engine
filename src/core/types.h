#pragma once

#include <cstdint>

namespace trading_engine {
    // Basic type aliases for better readability and type safety
    using Price = std::uint64_t; // Fixed-point price representation (scaled by 1e8)
    using Quantity = std::uint64_t; // Quantity in minimum tradable units
    using Value = std::uint64_t; // Notional value (scaled by 1e8)
    using OrderID = std::uint64_t; // Unique order identifier
    using SymbolID = std::uint32_t; // Symbol identifier
    using TradeID = std::uint64_t; // Trade identifier
    using Timestamp = std::uint64_t; // Hardware timestamp (TSC)

    // Trading-specific enums
    enum class Side : uint8_t {
        Buy = 0,
        Sell = 1
    };

    enum class OrderType : uint8_t {
        Market = 0,
        Limit = 1,
        Stop = 2,
        StopLimit = 3
    };

    enum class TimeInForce : uint8_t {
        Day = 0,
        Ioc = 1, // Immediate or Cancel
        Fok = 2, // Fill or Kill
        Gtc = 3 // Good Till Cancel
    };

    enum class OrderStatus : uint8_t {
        Incoming = 0,
        PartiallyFilled = 1,
        Filled = 2,
        Cancelled = 3,
        Rejected = 4
    };

    enum class MessageType : uint8_t {
        MarketDataIncremental = 1,
        MarketDataSnapshot = 2,
        NewOrder = 3,
        CancelOrder = 4,
        TradeReport = 5
    };

    enum class SignalType : uint8_t {
        None = 0,
        Buy = 1,
        Sell = 2
    };

    // Core data structures
    struct alignas(64) Order {
        OrderID orderID{};
        SymbolID symbolID{};
        Side side{};
        OrderType orderType{};
        TimeInForce TimeInForce{};
        Price price{};
        Quantity quantity{};
        Quantity filledQuantity{0};
        OrderStatus status{OrderStatus::Incoming};
        Timestamp timestamp{};

        // For intrusive containers
        Order *next{nullptr};
        Order *prev{nullptr};
    };

    struct alignas(32) MarketTick {
        SymbolID symbol_id;
        Price price;
        Quantity quantity;
        Side side;
        Timestamp timestamp;
        uint64_t sequence;
    };

    struct alignas(32) Trade {
        TradeID trade_id;
        OrderID buy_order_id;
        OrderID sell_order_id;
        SymbolID symbol_id;
        Price price;
        Quantity quantity;
        Timestamp timestamp;
        Side aggressor_side;
    };

    // Message headers for network protocols
    struct alignas(8) MessageHeader {
        MessageType message_type;
        uint8_t version;
        uint16_t length;
        uint32_t sequence_number;
    };

    struct alignas(32) MDIncrementalMessage {
        MessageHeader header;
        SymbolID symbol_id;
        Price price;
        Quantity quantity;
        Side side;
        Timestamp exchange_timestamp;
    };

    struct alignas(32) MDSnapshotMessage {
        MessageHeader header;
        SymbolID symbol_id;
        uint32_t num_levels;
        Timestamp exchange_timestamp;
        // Followed by array of price levels
    };

    // Constants
    constexpr std::size_t CacheLineSize = 64;
    constexpr Price PriceScale = 100000000ULL; // 1e8 for 8 decimal places
    constexpr uint32_t MaxSymbolCount = 10000;
    constexpr uint32_t DefaultQueueSize = 4096;

    // Utility functions
    constexpr Price to_scaled_price(const double price) noexcept {
        return static_cast<Price>(price * PriceScale);
    }

    constexpr double from_scaled_price(const Price price) noexcept {
        return static_cast<double>(price) / PriceScale;
    }

    constexpr Value calculate_notional(const Price price, Quantity quantity) noexcept {
        return static_cast<Value>(price) * quantity / PriceScale;
    }

    // Hardware timestamp functions
    inline std::uint64_t rdtsc() noexcept {
#ifdef _MSC_VER
        return __rdtsc();
#else
        uint32_t lo, hi;
        __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
        return (static_cast<std::uint64_t>(hi) << 32) | lo;
#endif
    }
}
