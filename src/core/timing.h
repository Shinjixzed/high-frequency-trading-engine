#pragma once

#include "types.h"
#include "memory.h"

#include <atomic>
#include <chrono>
#include <thread>
#ifdef _WIN32
#include <intrin.h>
#else
#include <x86intrin.h>
#include <cpuid.h>
#endif

namespace trading_engine {
    // Hardware timestamp manager
    class TimestampManager {
        inline static std::atomic<std::uint64_t> tsc_frequency{0};
        inline static std::atomic<bool> tsc_reliable{false};
        inline static std::atomic<bool> initialized{false};

    public:
        static void initialize() {
            if (initialized.load(std::memory_order_acquire)) {
                return;
            }

            calibrate_tsc();
            tsc_reliable.store(check_tsc_reliability(), std::memory_order_release);
            initialized.store(true, std::memory_order_release);
        }

        static FORCE_INLINE std::uint64_t get_hardware_timestamp() noexcept {
            if (LIKELY(tsc_reliable.load(std::memory_order_relaxed))) {
                return rdtsc();
            }
            return fallback_timestamp();
        }

        static FORCE_INLINE std::chrono::nanoseconds tsc_to_nanoseconds(const std::uint64_t tsc) noexcept {
            const std::uint64_t freq = tsc_frequency.load(std::memory_order_relaxed);
            return std::chrono::nanoseconds((tsc * 1000000000ULL) / freq);
        }

        static FORCE_INLINE double tsc_to_microseconds(const std::uint64_t tsc) noexcept {
            const std::uint64_t freq = tsc_frequency.load(std::memory_order_relaxed);
            return static_cast<double>(tsc) / (freq / 1000000.0);
        }

        static FORCE_INLINE double tsc_to_milliseconds(const std::uint64_t tsc) noexcept {
            const std::uint64_t freq = tsc_frequency.load(std::memory_order_relaxed);
            return static_cast<double>(tsc) / (freq / 1000.0);
        }

        static std::uint64_t get_frequency() noexcept {
            return tsc_frequency.load(std::memory_order_relaxed);
        }

        static bool is_reliable() noexcept {
            return tsc_reliable.load(std::memory_order_relaxed);
        }

    private:
        static void calibrate_tsc() {
            constexpr int num_samples = 10;
            constexpr auto sleep_duration = std::chrono::milliseconds(100);

            std::uint64_t total_frequency = 0;

            for (int i = 0; i < num_samples; ++i) {
                auto start_time = std::chrono::high_resolution_clock::now();
                const std::uint64_t start_tsc = rdtsc();

                std::this_thread::sleep_for(sleep_duration);

                const std::uint64_t end_tsc = rdtsc();
                auto end_time = std::chrono::high_resolution_clock::now();

                const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    end_time - start_time).count();

                const std::uint64_t tsc_diff = end_tsc - start_tsc;
                const std::uint64_t frequency = (tsc_diff * 1000000000ULL) / elapsed_ns;

                total_frequency += frequency;
            }

            tsc_frequency.store(total_frequency / num_samples, std::memory_order_release);
        }

        static bool check_tsc_reliability() {
            // Check if TSC is invariant and synchronized across cores
#ifdef _WIN32
            int cpu_info[4];
            __cpuid(cpu_info, 0x80000007);
            return (cpu_info[3] & (1 << 8)) != 0; // Invariant TSC bit
#else
            // Linux-specific checks could be added here
            return true; // Assume reliable for now
#endif
        }

        static std::uint64_t fallback_timestamp() {
            auto now = std::chrono::high_resolution_clock::now();
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch()).count();
        }
    };

    // Latency measurement utilities
    class LatencyProfiler {
        struct LatencyStats {
            std::atomic<std::uint64_t> total_samples;
            std::atomic<std::uint64_t> total_latency;
            std::atomic<std::uint64_t> min_latency;
            std::atomic<std::uint64_t> max_latency;

            LatencyStats() : total_samples(0), total_latency(0), min_latency(UINT64_MAX), max_latency(0) {
            }
        };

        static constexpr size_t Max_profiles = 32;
        inline static std::array<LatencyStats, Max_profiles> profiles;
        inline static std::atomic<size_t> profile_count{0};

    public:
        enum ProfileID : size_t {
            Order_processing = 0,
            Market_data_processing = 1,
            Order_matching = 2,
            Risk_check = 3,
            Strategy_signal = 4,
            Trade_reporting = 5
        };

        static void record(ProfileID id, std::uint64_t latency_tsc) {
            if (UNLIKELY(id >= profile_count.load(std::memory_order_relaxed))) {
                return;
            }

            LatencyStats &stats = profiles[id];

            stats.total_samples.fetch_add(1, std::memory_order_relaxed);
            stats.total_latency.fetch_add(latency_tsc, std::memory_order_relaxed);

            // Update min
            std::uint64_t current_min = stats.min_latency.load(std::memory_order_relaxed);
            while (latency_tsc < current_min) {
                if (stats.min_latency.compare_exchange_weak(
                    current_min, latency_tsc, std::memory_order_relaxed)) {
                    break;
                }
            }

            // Update max
            std::uint64_t current_max = stats.max_latency.load(std::memory_order_relaxed);
            while (latency_tsc > current_max) {
                if (stats.max_latency.compare_exchange_weak(
                    current_max, latency_tsc, std::memory_order_relaxed)) {
                    break;
                }
            }
        }

        struct ProfileResults {
            std::uint64_t sample_count;
            double avg_latency_us;
            double min_latency_us;
            double max_latency_us;
        };

        static ProfileResults get_stats(ProfileID id) {
            if (id >= profile_count.load(std::memory_order_relaxed)) {
                return {};
            }

            const LatencyStats &stats = profiles[id];
            const std::uint64_t samples = stats.total_samples.load(std::memory_order_relaxed);
            const std::uint64_t total = stats.total_latency.load(std::memory_order_relaxed);
            const std::uint64_t min_lat = stats.min_latency.load(std::memory_order_relaxed);
            const std::uint64_t max_lat = stats.max_latency.load(std::memory_order_relaxed);

            return ProfileResults{
                .sample_count = samples,
                .avg_latency_us = samples > 0 ? TimestampManager::tsc_to_microseconds(total / samples) : 0.0,
                .min_latency_us = min_lat != UINT64_MAX ? TimestampManager::tsc_to_microseconds(min_lat) : 0.0,
                .max_latency_us = TimestampManager::tsc_to_microseconds(max_lat)
            };
        }

        static void reset(ProfileID id) {
            if (id >= profile_count.load(std::memory_order_relaxed)) {
                return;
            }

            LatencyStats &stats = profiles[id];
            stats.total_samples.store(0, std::memory_order_relaxed);
            stats.total_latency.store(0, std::memory_order_relaxed);
            stats.min_latency.store(UINT64_MAX, std::memory_order_relaxed);
            stats.max_latency.store(0, std::memory_order_relaxed);
        }

        static void initialize() {
            profile_count.store(6, std::memory_order_release); // Number of predefined profiles
        }
    };

    // RAII latency measurement
    class ScopedLatencyMeasure {
        LatencyProfiler::ProfileID profile_id;
        std::uint64_t start_tsc;

    public:
        explicit ScopedLatencyMeasure(LatencyProfiler::ProfileID id)
            : profile_id(id), start_tsc(TimestampManager::get_hardware_timestamp()) {
        }

        ~ScopedLatencyMeasure() {
            const std::uint64_t end_tsc = TimestampManager::get_hardware_timestamp();
            LatencyProfiler::record(profile_id, end_tsc - start_tsc);
        }
    };

    // Latency measurement macro
#define MEASURE_LATENCY(profile_id) ScopedLatencyMeasure _latency_measure(profile_id)
#define MEASURE_LATENCY_BLOCK(profile_id, code) \
    do { \
        std::uint64_t start_tsc = TimestampManager::get_hardware_timestamp(); \
        code; \
        std::uint64_t end_tsc = TimestampManager::get_hardware_timestamp(); \
        LatencyProfiler::record(profile_id, end_tsc - start_tsc); \
    } while(0)

    // High-frequency timer for periodic tasks
    class HighFrequencyTimer {
        std::atomic<bool> running{false};
        std::thread timer_thread;

    public:
        template<typename Func>
        void start(std::chrono::nanoseconds interval, Func &&callback) {
            running.store(true, std::memory_order_release);

            timer_thread = std::thread([this, interval, callback = std::forward<Func>(callback)]() {
                std::uint64_t next_wakeup = TimestampManager::get_hardware_timestamp();
                const std::uint64_t interval_tsc = interval.count() * TimestampManager::get_frequency() / 1000000000ULL;

                while (running.load(std::memory_order_acquire)) {
                    next_wakeup += interval_tsc;

                    // Busy wait for precise timing
                    while (TimestampManager::get_hardware_timestamp() < next_wakeup) {
#ifdef _WIN32
                        YieldProcessor(); // Windows equivalent of _mm_pause
#else
                        _mm_pause(); // CPU hint for spin loops
#endif
                    }

                    callback();
                }
            });
        }

        void stop() {
            running.store(false, std::memory_order_release);
            if (timer_thread.joinable()) {
                timer_thread.join();
            }
        }

        ~HighFrequencyTimer() {
            stop();
        }
    };
}
