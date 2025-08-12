#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>
#include <windows.h>

#include "engine/trading_engine.h"

using namespace trading_engine;

// Global engine instance for signal handling
std::unique_ptr<TradingEngine> g_engine;
std::atomic<bool> g_shutdown_requested{false};

void print_statistics(const TradingEngine &engine) {
    const auto stats = engine.get_statistics();

    std::cout << "\n=== Trading Engine Statistics ===\n";
    std::cout << "Uptime: " << stats.uptime_seconds << " seconds\n";
    std::cout << "Orders Received: " << stats.orders_received << "\n";
    std::cout << "Orders Processed: " << stats.orders_processed << "\n";
    std::cout << "Orders Rejected: " << stats.orders_rejected << "\n";
    std::cout << "Trades Executed: " << stats.trades_executed << "\n";
    std::cout << "Processing Rate: " << stats.order_processing_rate << " orders/sec\n";

    std::cout << "\n--- Market Data Stats ---\n";
    std::cout << "Messages Received: " << stats.market_data_stats.total_messages_received << "\n";
    std::cout << "Messages Processed: " << stats.market_data_stats.total_messages_processed << "\n";
    std::cout << "Parsing Errors: " << stats.market_data_stats.total_parsing_errors << "\n";
    std::cout << "Active Symbols: " << stats.market_data_stats.active_symbols << "\n";
    std::cout << "Processing Rate: " << stats.market_data_stats.processing_rate_per_second << " msg/sec\n";

    std::cout << "\n--- Matching Engine Stats ---\n";
    std::cout << "Total Orders: " << stats.matching_stats.total_orders << "\n";
    std::cout << "Total Trades: " << stats.matching_stats.total_trades << "\n";
    std::cout << "Total Volume: " << stats.matching_stats.total_volume << "\n";
    std::cout << "Match Rate: " << (stats.matching_stats.match_rate * 100.0) << "%\n";
    std::cout << "Average Fill Size: " << stats.matching_stats.average_fill_size << "\n";

    // Show latency profiles
    std::cout << "\n--- Latency Profiles ---\n";
    const auto order_latency = LatencyProfiler::get_stats(LatencyProfiler::Order_processing);
    const auto md_latency = LatencyProfiler::get_stats(LatencyProfiler::Market_data_processing);
    const auto matching_latency = LatencyProfiler::get_stats(LatencyProfiler::Order_matching);
    const auto risk_latency = LatencyProfiler::get_stats(LatencyProfiler::Risk_check);
    const auto strategy_latency = LatencyProfiler::get_stats(LatencyProfiler::Strategy_signal);

    std::cout << "Order Processing - Avg: " << order_latency.avg_latency_us
            << "μs, Max: " << order_latency.max_latency_us << "μs, Samples: "
            << order_latency.sample_count << "\n";

    std::cout << "Market Data - Avg: " << md_latency.avg_latency_us
            << "μs, Max: " << md_latency.max_latency_us << "μs, Samples: "
            << md_latency.sample_count << "\n";

    std::cout << "Order Matching - Avg: " << matching_latency.avg_latency_us
            << "μs, Max: " << matching_latency.max_latency_us << "μs, Samples: "
            << matching_latency.sample_count << "\n";

    std::cout << "Risk Checks - Avg: " << risk_latency.avg_latency_us
            << "μs, Max: " << risk_latency.max_latency_us << "μs, Samples: "
            << risk_latency.sample_count << "\n";

    std::cout << "Strategy Signals - Avg: " << strategy_latency.avg_latency_us
            << "μs, Max: " << strategy_latency.max_latency_us << "μs, Samples: "
            << strategy_latency.sample_count << "\n";

    std::cout << "================================\n\n";
}

void signal_handler(int signal) {
    std::cout << "\nReceived signal " << signal << ", requesting shutdown...\n";
    g_shutdown_requested.store(true);
}

int main() {
    // Set console output to UTF-8 for better character support
    SetConsoleOutputCP(CP_UTF8);

    // Set up signal handling for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        g_engine = std::make_unique<TradingEngine>();
        std::cout << "Trading engine created successfully." << std::endl;

        // Start the full trading engine
        if (!g_engine->start()) {
            std::cerr << "Failed to start trading engine!" << std::endl;
            return 1;
        }
        std::cout << "Trading engine started successfully." << std::endl;

        // Add a mean reversion strategy (this will automatically subscribe to the symbol)
        SymbolID test_symbol = 1;
        g_engine->add_mean_reversion_strategy(test_symbol);
        std::cout << "Added mean reversion strategy for symbol " << test_symbol << std::endl;

        // Submit some test orders to demonstrate the system
        std::cout << "\nSubmitting test orders..." << std::endl;
        for (int i = 0; i < 5; ++i) {
            Order buy_order{};
            buy_order.orderID = 2 * i + 1;
            buy_order.symbolID = test_symbol;
            buy_order.side = Side::Buy;
            buy_order.orderType = OrderType::Limit;
            buy_order.quantity = 100;
            buy_order.price = 10000 + i * 10; // Prices from 100.00 to 100.40
            buy_order.timestamp = TimestampManager::get_hardware_timestamp();

            Order sell_order{};
            sell_order.orderID = i * 2 + 2;
            sell_order.symbolID = test_symbol;
            sell_order.side = Side::Sell;
            sell_order.orderType = OrderType::Limit;
            sell_order.quantity = 100;
            sell_order.price = 10100 + i * 10; // Prices from 101.00 to 101.40
            sell_order.timestamp = TimestampManager::get_hardware_timestamp();

            if (g_engine->submit_order(buy_order)) {
                std::cout << "Submitted buy order " << buy_order.orderID
                        << " at price " << (static_cast<double>(buy_order.price) / 100.0) << std::endl;
            }

            if (g_engine->submit_order(sell_order)) {
                std::cout << "Submitted sell order " << sell_order.orderID
                        << " at price " << (static_cast<double>(sell_order.price) / 100.0) << std::endl;
            }

            // Small delay between orders
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        std::cout << "\nTrading engine is running..." << std::endl;
        std::cout << "- Market data gateway is generating synthetic market data" << std::endl;
        std::cout << "- Mean reversion strategy is analyzing price movements" << std::endl;
        std::cout << "- Order matching engine is processing orders" << std::endl;
        std::cout << "- Risk manager is monitoring positions" << std::endl;
        std::cout << "\nPress Ctrl+C to stop and view final statistics." << std::endl;

        // Main loop - print statistics every 3 seconds
        auto last_stats_time = std::chrono::steady_clock::now();
        int stats_counter = 0;

        while (!g_shutdown_requested.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            if (auto now = std::chrono::steady_clock::now();
                std::chrono::duration_cast<std::chrono::seconds>(now - last_stats_time).count() >= 3) {
                stats_counter++;
                std::cout << "\n--- Stats Update #" << stats_counter << " ---" << std::endl;
                print_statistics(*g_engine);
                last_stats_time = now;
            }
        }

        std::cout << "\nShutdown requested, stopping engine..." << std::endl;
        g_engine->stop();
        std::cout << "Engine stopped." << std::endl;

        // Final statistics
        std::cout << "\n=== FINAL STATISTICS ===" << std::endl;
        print_statistics(*g_engine);

        g_engine.reset();
        std::cout << "TradingEngine destroyed successfully." << std::endl;
    } catch (const std::exception &e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Unknown exception caught" << std::endl;
        return 1;
    }

    std::cout << "Trading Engine demonstration completed successfully." << std::endl;
    return 0;
}
