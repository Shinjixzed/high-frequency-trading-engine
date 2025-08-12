#pragma once

#include <atomic>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "../core/memory.h"
#include "../core/queue.h"
#include "../core/timing.h"
#include "../core/types.h"
#include "../market_data/gateway.h"
#include "../market_data/order_book.h"
#include "../matching/matching_engine.h"
#include "../risk/risk_manager.h"
#include "../strategy/strategy_base.h"
#include "../strategy/strategy_interface.h"

namespace trading_engine {
    // Main trading engine orchestrator
    class TradingEngine {
    private:
        // Core components
        std::unique_ptr<OrderBookManager> order_book_manager;
        std::unique_ptr<MarketDataGateway> market_data_gateway;
        std::unique_ptr<RiskManager> risk_manager;
        std::unique_ptr<MatchingEngine> matching_engine;

        // Strategy management
        std::vector<std::unique_ptr<IStrategy> > strategies;

        // Threading
        std::vector<std::thread> worker_threads;
        std::atomic<bool> engine_running{false};
        std::atomic<bool> stopped{false};

        // Message queues for different components
        SPSCQueue<Order, 4096> incoming_orders;
        SPSCQueue<Order, 1024> risk_approved_orders;
        MPSCQueue<Trade, 2048> trade_notifications;

        // Statistics and monitoring
        std::atomic<uint64_t> orders_received{0};
        std::atomic<uint64_t> orders_processed{0};
        std::atomic<uint64_t> orders_rejected{0};
        std::atomic<uint64_t> trades_executed{0};

        // Performance tracking
        std::chrono::steady_clock::time_point start_time;

    public:
        TradingEngine() {
            initialize_components();
        }

        ~TradingEngine() {
            stop();
        }

        bool start() {
            if (engine_running.load(std::memory_order_acquire)) {
                return false; // Already running
            }

            start_time = std::chrono::steady_clock::now();
            engine_running.store(true, std::memory_order_release);

            // Initialize subsystems
            TimestampManager::initialize();
            LatencyProfiler::initialize();
            NUMAAllocator::instance().initialize(std::thread::hardware_concurrency());

            // Start components
            if (!market_data_gateway->start()) {
                return false;
            }

            // Start worker threads
            start_worker_threads();

            return true;
        }

        void stop() {
            if (stopped.exchange(true, std::memory_order_acq_rel)) {
                return; // Already stopped
            }

            std::cout << "Stopping trading engine..." << std::endl;

            // 1. Stop new events from entering the system
            std::cout << "Stopping market data gateway..." << std::endl;
            if (market_data_gateway) {
                market_data_gateway->stop();
            }

            // 2. Signal worker threads to stop their loops
            std::cout << "Signaling worker threads to stop..." << std::endl;
            engine_running.store(false, std::memory_order_release);

            // 3. Wait for all worker threads to complete and exit
            std::cout << "Joining worker threads..." << std::endl;
            for (size_t i = 0; i < worker_threads.size(); ++i) {
                if (worker_threads[i].joinable()) {
                    std::cout << "Joining thread " << i << "..." << std::endl;
                    worker_threads[i].join();
                    std::cout << "Thread " << i << " joined." << std::endl;
                }
            }
            worker_threads.clear();
            std::cout << "All worker threads joined." << std::endl;

            // 4. Stop strategies
            for (auto &strategy: strategies) {
                strategy->shutdown();
            }

            std::cout << "Trading engine stopped." << std::endl;
        }

        // Order submission interface
        bool submit_order(const Order &order) {
            orders_received.fetch_add(1, std::memory_order_relaxed);

            if (!incoming_orders.try_push(order)) {
                // Queue full - this is a critical error in production
                return false;
            }

            return true;
        }

        // Strategy management
        void add_mean_reversion_strategy(SymbolID symbol_id) {
            auto strategy = std::make_unique<MeanReversionStrategy>(symbol_id);

            // Set up callbacks
            strategy->set_order_callback([this](const Order &order) {
                submit_order(order);
            });

            strategy->set_cancel_callback([this](OrderID order_id) {
                cancel_order(order_id);
            });

            strategies.push_back(std::move(strategy));

            // Subscribe to market data for this symbol
            market_data_gateway->subscribe_symbol(symbol_id);
        }

        bool cancel_order(const OrderID order_id) const {
            return matching_engine->cancel_order(order_id);
        }

        // Market data subscription
        void subscribe_symbol(SymbolID symbol_id) const {
            market_data_gateway->subscribe_symbol(symbol_id);
        }

        void unsubscribe_symbol(SymbolID symbol_id) const {
            market_data_gateway->unsubscribe_symbol(symbol_id);
        }

        // Statistics and monitoring
        struct EngineStats {
            uint64_t orders_received;
            uint64_t orders_processed;
            uint64_t orders_rejected;
            uint64_t trades_executed;
            double order_processing_rate;
            double uptime_seconds;
            MarketDataGateway::GatewayStats market_data_stats;
            MatchingEngine::MatchingStats matching_stats;
        };

        [[nodiscard]] EngineStats get_statistics() const {
            auto now = std::chrono::steady_clock::now();
            auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - start_time);

            uint64_t processed = orders_processed.load(std::memory_order_relaxed);
            double processing_rate = uptime.count() > 0 ? static_cast<double>(processed) / uptime.count() : 0.0;

            return EngineStats{
                .orders_received = orders_received.load(std::memory_order_relaxed),
                .orders_processed = processed,
                .orders_rejected = orders_rejected.load(std::memory_order_relaxed),
                .trades_executed = trades_executed.load(std::memory_order_relaxed),
                .order_processing_rate = processing_rate,
                .uptime_seconds = static_cast<double>(uptime.count()),
                .market_data_stats = market_data_gateway->get_statistics(),
                .matching_stats = matching_engine->get_statistics()
            };
        }

        [[nodiscard]] OrderBook<1000> *get_order_book(SymbolID symbol_id) const {
            return order_book_manager->get_order_book(symbol_id);
        }

        [[nodiscard]] RiskManager::PositionInfo get_position_info(SymbolID symbol_id) const {
            return risk_manager->get_position_info(symbol_id);
        }

    private:
        void initialize_components() {
            // Create core components
            order_book_manager = std::make_unique<OrderBookManager>();
            market_data_gateway = std::make_unique<MarketDataGateway>(order_book_manager.get());
            risk_manager = std::make_unique<RiskManager>();
            matching_engine = std::make_unique<MatchingEngine>();

            // Set up callbacks
            setup_callbacks();
        }

        void setup_callbacks() const {
            // Market data callbacks
            market_data_gateway->set_tick_callback([this](const MarketTick &tick) {
                on_market_tick(tick);
            });

            market_data_gateway->set_snapshot_callback([this](SymbolID symbol_id,
                                                              const OrderBook<>::BookSnapshot &snapshot) {
                    on_book_snapshot(symbol_id, snapshot);
                }
            );

            // Matching engine callbacks
            matching_engine->set_trade_callback([this](const Trade &trade) {
                on_trade_executed(trade);
            });

            matching_engine->set_order_update_callback([this](const Order &order) {
                on_order_update(order);
            });
        }

        void start_worker_threads() {
            try {
                // Order processing thread
                worker_threads.emplace_back(&TradingEngine::order_processing_loop, this);

                // Risk management thread
                worker_threads.emplace_back(&TradingEngine::risk_processing_loop, this);

                // Strategy processing thread
                worker_threads.emplace_back(&TradingEngine::strategy_processing_loop, this);

                // Trade notification thread
                worker_threads.emplace_back(&TradingEngine::trade_notification_loop, this);
            } catch (const std::exception &e) {
                std::cerr << "Exception creating worker threads: " << e.what() << std::endl;
                engine_running.store(false, std::memory_order_release);
            } catch (...) {
                std::cerr << "Unknown exception creating worker threads" << std::endl;
                engine_running.store(false, std::memory_order_release);
            }
        }

        void order_processing_loop() {
            try {
                Order order;

                while (engine_running.load(std::memory_order_acquire)) {
                    if (risk_approved_orders.try_pop(order)) {
                        MEASURE_LATENCY_BLOCK(LatencyProfiler::Order_processing, {
                                              // Process the order through matching engine
                                              auto result = matching_engine->process_order(order);
                                              orders_processed.fetch_add(1, std::memory_order_relaxed);

                                              // Notify about trades
                                              for (Trade* trade : result.trades) {
                                              if (!trade_notifications.try_push(*trade)) {
                                              // Handle trade notification overflow
                                              }
                                              }
                                              });
                    } else {
                        std::this_thread::yield();
                    }
                }
            } catch (const std::exception &e) {
                std::cerr << "Exception in order_processing_loop: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Unknown exception in order_processing_loop" << std::endl;
            }
        }

        void risk_processing_loop() {
            try {
                Order order;

                while (engine_running.load(std::memory_order_acquire)) {
                    if (incoming_orders.try_pop(order)) {
                        // Check risk limits
                        auto risk_result = risk_manager->check_order(order);

                        if (risk_result == RiskManager::RiskResult::approved) {
                            if (!risk_approved_orders.try_push(order)) {
                                // Risk approved queue full - critical error
                                orders_rejected.fetch_add(1, std::memory_order_relaxed);
                            }
                        } else {
                            // Order rejected by risk management
                            orders_rejected.fetch_add(1, std::memory_order_relaxed);

                            // Update order status and notify
                            Order rejected_order = order;
                            rejected_order.status = OrderStatus::Rejected;
                            on_order_update(rejected_order);
                        }
                    } else {
                        std::this_thread::yield();
                    }
                }
            } catch (const std::exception &e) {
                std::cerr << "Exception in risk_processing_loop: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Unknown exception in risk_processing_loop" << std::endl;
            }
        }

        void strategy_processing_loop() const {
            try {
                while (engine_running.load(std::memory_order_acquire)) {
                    // Process all strategies
                    for (auto &strategy: strategies) {
                        if (strategy->is_enabled()) {
                            strategy->process_signals();
                        }
                    }

                    // Small sleep to prevent excessive CPU usage
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            } catch (const std::exception &e) {
                std::cerr << "Exception in strategy_processing_loop: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Unknown exception in strategy_processing_loop" << std::endl;
            }
        }

        void trade_notification_loop() {
            try {
                Trade trade{};

                while (engine_running.load(std::memory_order_acquire)) {
                    if (trade_notifications.try_pop(trade)) {
                        // Update risk manager with trade information
                        risk_manager->update_position(trade);

                        // Update reference prices for risk checks
                        risk_manager->update_reference_price(trade.symbol_id, trade.price);

                        trades_executed.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        std::this_thread::yield();
                    }
                }
            } catch (const std::exception &e) {
                std::cerr << "Exception in trade_notification_loop: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Unknown exception in trade_notification_loop" << std::endl;
            }
        }

        void on_market_tick(const MarketTick &tick) const {
            // Forward to strategies
            for (auto &strategy: strategies) {
                if (strategy->get_symbol_id() == tick.symbol_id) {
                    strategy->on_market_data(tick);
                }
            }
        }

        void on_book_snapshot(SymbolID symbol_id, const OrderBook<1000>::BookSnapshot &snapshot) const {
            // Forward to strategies
            for (auto &strategy: strategies) {
                if (strategy->get_symbol_id() == symbol_id) {
                    strategy->on_book_snapshot(snapshot);
                }
            }
        }

        void on_trade_executed(const Trade &trade) const {
            // Forward to strategies
            for (auto &strategy: strategies) {
                if (strategy->get_symbol_id() == trade.symbol_id) {
                    strategy->on_trade(trade);
                }
            }
        }

        static void on_order_update([[maybe_unused]] const Order &order) {
            // Log order updates, send to external systems, etc.
            // This could be extended to notify external order management systems
        }
    };
}
