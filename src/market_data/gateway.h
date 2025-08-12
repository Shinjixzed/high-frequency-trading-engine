#pragma once

#include "../core/types.h"
#include "../core/memory.h"
#include "../core/queue.h"
#include "../core/timing.h"
#include "../market_data/order_book.h"

#include <thread>
#include <functional>
#include <unordered_map>

namespace trading_engine {
    // Market data feed handler
    class MarketDataGateway {
        struct alignas(CacheLineSize) SymbolProcessor {
            SPSCQueue<MarketTick, 4096> tick_queue;
            std::atomic<uint64_t> sequence_number{0};
            std::atomic<uint64_t> messages_processed{0};
            std::atomic<uint64_t> messages_dropped{0};
            std::thread processor_thread;
            std::atomic<bool> running{false};
        };

        std::unordered_map<SymbolID, std::unique_ptr<SymbolProcessor> > processors;
        OrderBookManager *order_book_manager;

        // Network receiver (placeholder for actual implementation)
        std::thread receiver_thread;
        std::atomic<bool> gateway_running{false};

        // Callbacks for processed data
        std::function<void(const MarketTick &)> tick_callback;
        std::function<void(SymbolID, const OrderBook<1000>::BookSnapshot &)> snapshot_callback;

        // Statistics
        std::atomic<uint64_t> total_messages_received{0};
        std::atomic<uint64_t> total_messages_processed{0};
        std::atomic<uint64_t> total_parsing_errors{0};

    public:
        explicit MarketDataGateway(OrderBookManager *book_manager)
            : order_book_manager(book_manager) {
        }

        ~MarketDataGateway() {
            stop();
        }

        void set_tick_callback(std::function<void(const MarketTick &)> callback) {
            tick_callback = std::move(callback);
        }

        void set_snapshot_callback(std::function<void(SymbolID, const OrderBook<1000>::BookSnapshot &)> callback) {
            snapshot_callback = std::move(callback);
        }

        bool start() {
            if (gateway_running.load(std::memory_order_acquire)) {
                return false; // Already running
            }

            gateway_running.store(true, std::memory_order_release);

            // Start receiver thread
            receiver_thread = std::thread(&MarketDataGateway::receiver_loop, this);

            return true;
        }

        void stop() {
            gateway_running.store(false, std::memory_order_release);

            // Stop all symbol processors
            for (auto &[symbol_id, processor]: processors) {
                processor->running.store(false, std::memory_order_release);
                if (processor->processor_thread.joinable()) {
                    processor->processor_thread.join();
                }
            }

            if (receiver_thread.joinable()) {
                receiver_thread.join();
            }
        }

        void subscribe_symbol(SymbolID symbol_id) {
            auto processor = std::make_unique<SymbolProcessor>();
            processor->running.store(true, std::memory_order_release);

            // Store the processor in the map first to ensure stable memory location
            SymbolProcessor *processor_ptr = processor.get();
            processors[symbol_id] = std::move(processor);

            // Start processing thread for this symbol using the stable pointer
            processor_ptr->processor_thread = std::thread(&MarketDataGateway::symbol_processor_loop, this, symbol_id,
                                                          processor_ptr);
        }

        void unsubscribe_symbol(SymbolID symbol_id) {
            if (auto it = processors.find(symbol_id); it != processors.end()) {
                it->second->running.store(false, std::memory_order_release);
                if (it->second->processor_thread.joinable()) {
                    it->second->processor_thread.join();
                }
                processors.erase(it);
            }
        }

        // Statistics
        struct GatewayStats {
            uint64_t total_messages_received;
            uint64_t total_messages_processed;
            uint64_t total_parsing_errors;
            uint64_t active_symbols;
            double processing_rate_per_second;
        };

        GatewayStats get_statistics() const {
            return GatewayStats{
                .total_messages_received = total_messages_received.load(std::memory_order_relaxed),
                .total_messages_processed = total_messages_processed.load(std::memory_order_relaxed),
                .total_parsing_errors = total_parsing_errors.load(std::memory_order_relaxed),
                .active_symbols = processors.size(),
                .processing_rate_per_second = calculate_processing_rate()
            };
        }

        void process_raw_message(const uint8_t *data, size_t length) {
            MEASURE_LATENCY(LatencyProfiler::Market_data_processing);

            total_messages_received.fetch_add(1, std::memory_order_relaxed);

            if (UNLIKELY(length < sizeof(MessageHeader))) {
                total_parsing_errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            const auto header = reinterpret_cast<const MessageHeader *>(data);

            switch (header->message_type) {
                case MessageType::MarketDataIncremental:
                    if (length >= sizeof(MDIncrementalMessage)) {
                        process_incremental_update(
                            reinterpret_cast<const MDIncrementalMessage *>(data));
                    } else {
                        total_parsing_errors.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;

                case MessageType::MarketDataSnapshot:
                    if (length >= sizeof(MDSnapshotMessage)) {
                        process_snapshot_update(
                            reinterpret_cast<const MDSnapshotMessage *>(data));
                    } else {
                        total_parsing_errors.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;

                default:
                    total_parsing_errors.fetch_add(1, std::memory_order_relaxed);
                    break;
            }
        }

    private:
        void receiver_loop() {
            while (gateway_running.load(std::memory_order_acquire)) {
                // Simulate receiving market data
                std::this_thread::sleep_for(std::chrono::microseconds(100));

                // Generate synthetic market data for testing
                generate_synthetic_data();
            }
        }

        void symbol_processor_loop(SymbolID /*symbol_id*/, SymbolProcessor *processor) {
            MarketTick tick{};

            while (processor->running.load(std::memory_order_acquire)) {
                if (processor->tick_queue.try_pop(tick)) {
                    // Process the tick
                    process_tick(tick);
                    processor->messages_processed.fetch_add(1, std::memory_order_relaxed);
                    total_messages_processed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    // No data available, yield
                    std::this_thread::yield();
                }
            }
        }

        void process_incremental_update(const MDIncrementalMessage *msg) {
            const SymbolID symbol_id = msg->symbol_id;

            auto it = processors.find(symbol_id);
            if (it == processors.end()) {
                // Symbol not subscribed
                return;
            }

            SymbolProcessor *processor = it->second.get();

            MarketTick tick{
                .symbol_id = symbol_id,
                .price = msg->price,
                .quantity = msg->quantity,
                .side = msg->side,
                .timestamp = TimestampManager::get_hardware_timestamp(),
                .sequence = processor->sequence_number.fetch_add(1, std::memory_order_relaxed)
            };

            if (!processor->tick_queue.try_push(tick)) {
                // Queue overflow
                processor->messages_dropped.fetch_add(1, std::memory_order_relaxed);
                handle_queue_overflow(symbol_id);
            }
        }

        void process_snapshot_update(const MDSnapshotMessage *msg) const {
            // Snapshot processing would rebuild the entire order book
            // This is a simplified version
            const SymbolID symbol_id = msg->symbol_id;

            if (order_book_manager) {
                if (const OrderBook<> *book = order_book_manager->get_or_create_order_book(symbol_id);
                    book && snapshot_callback) {
                    const auto snapshot = book->get_snapshot();
                    snapshot_callback(symbol_id, snapshot);
                }
            }
        }

        void process_tick(const MarketTick &tick) const {
            // Update order book
            if (order_book_manager) {
                order_book_manager->process_market_data(tick);
            }

            // Notify callback
            if (tick_callback) {
                tick_callback(tick);
            }
        }

        void handle_queue_overflow(SymbolID /*symbol_id*/) {
            // Handle queue overflow, e.g., log warning or drop messages
            // In production, this could be more sophisticated
            // std::cerr << "Warning: Market data queue overflow for symbol." << std::endl;
        }

        void generate_synthetic_data() {
            // Generate test data for development
            static std::atomic<uint32_t> counter{0};

            if (processors.empty()) {
                return;
            }

            SymbolID symbol_id = 1; // Test symbol
            auto it = processors.find(symbol_id);
            if (it == processors.end()) {
                return;
            }

            const uint32_t count = counter.fetch_add(1, std::memory_order_relaxed);

            MDIncrementalMessage synthetic_msg{
                .header = {
                    .message_type = MessageType::MarketDataIncremental,
                    .version = 1,
                    .length = sizeof(MDIncrementalMessage),
                    .sequence_number = count
                },
                .symbol_id = symbol_id,
                .price = to_scaled_price(100.0 + (count % 100) * 0.01),
                .quantity = 1000 + (count % 5000),
                .side = (count % 2) ? Side::Buy : Side::Sell,
                .exchange_timestamp = TimestampManager::get_hardware_timestamp()
            };

            process_incremental_update(&synthetic_msg);
        }

        double calculate_processing_rate() const {
            // Simple rate calculation - in production this would be more sophisticated
            static auto last_time = std::chrono::steady_clock::now();
            static uint64_t last_count = 0;

            const auto now = std::chrono::steady_clock::now();
            uint64_t current_count = total_messages_processed.load(std::memory_order_relaxed);

            if (auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time);
                duration.count() > 1000) {
                // Update every second
                const double rate = 1000.0 * (current_count - last_count) / duration.count();
                last_time = now;
                last_count = current_count;
                return rate;
            }

            return 0.0;
        }
    };
}
