#pragma once

#include "../core/types.h"
#include "../market_data/order_book.h"

namespace trading_engine {
    class IStrategy {
    public:
        virtual ~IStrategy() = default;

        virtual void process_signals() = 0;

        virtual bool is_enabled() const = 0;

        virtual SymbolID get_symbol_id() const = 0;

        virtual void on_market_data(const MarketTick &tick) = 0;

        virtual void on_book_snapshot(const OrderBook<>::BookSnapshot &snapshot) = 0;

        virtual void on_trade(const Trade &trade) = 0;

        virtual void shutdown() = 0;
    };
}
