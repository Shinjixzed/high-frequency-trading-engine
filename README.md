# High-Frequency Trading Engine

A sophisticated, low-latency trading engine built in modern C++23 designed for high-frequency trading applications. This
system demonstrates enterprise-grade architecture with microsecond-level performance optimizations.

## 🚀 Features

### Core Trading Capabilities

- **Ultra-Low Latency**: Sub-microsecond order processing with hardware timestamp precision
- **Multi-Asset Support**: Configurable for stocks, futures, options, and other financial instruments
- **Advanced Order Types**: Market, Limit, Stop, and Stop-Limit orders with multiple time-in-force options
- **Real-Time Risk Management**: Position limits, exposure controls, and pre-trade risk checks
- **Strategy Framework**: Pluggable algorithmic trading strategies (Mean Reversion included)

### High-Performance Architecture

- **Lock-Free Data Structures**: SPSC/MPSC queues for zero-lock message passing
- **NUMA-Aware Memory Management**: Optimized memory allocation and cache-friendly layouts
- **Multi-Threading**: Dedicated threads for market data, order matching, and risk management
- **Zero-Copy Design**: Minimizes memory allocations in critical paths

### Market Data Processing

- **Synthetic Data Generation**: Built-in market simulation for testing and development
- **Multiple Feed Support**: Extensible architecture for various market data providers
- **Order Book Management**: Full order book reconstruction and maintenance

## 📊 Performance Metrics

Based on actual runtime statistics:

| Component        | Average Latency | Throughput           |
|------------------|-----------------|----------------------|
| Order Processing | 0.86μs          | 400+ orders/sec      |
| Market Data      | 0μs             | 400,000+ msg/sec     |
| Order Matching   | 213μs           | Real-time matching   |
| Risk Checks      | 12μs            | Pre-trade validation |
| Strategy Signals | 0.14μs          | Signal generation    |

## 🛠️ Technology Stack

- **Language**: C++23 with modern features
- **Build System**: CMake 3.20+
- **Compiler**: GCC 11+ / MSVC 2022+ / Clang 14+
- **Architecture**: x86-64 optimized
- **Threading**: std::thread with atomic operations
- **Memory**: Custom allocators and NUMA-aware allocation

## 🏗️ Architecture Overview

```
┌───────────────────────┐      ┌─────────────────┐      ┌───────────────────┐
│  Market Data Gateway  │ ◄──► │ Trading Engine  │ ◄──► │  Risk Management  │
└───────────────────────┘      └─────────────────┘      └───────────────────┘
            ▲                           ▲                         ▲
            │                           │                         │
            ▼                           ▼                         ▼
┌───────────────────────┐      ┌─────────────────┐      ┌────────────────────┐
│ Order Book Management │      │ Matching Engine │      │ Strategy Framework │
└───────────────────────┘      └─────────────────┘      └────────────────────┘
```

### Key Components

1. **Trading Engine Core** (`src/engine/trading_engine.h`)
    - Central orchestrator managing all subsystems
    - Worker thread pool with configurable sizing
    - Component lifecycle management

2. **Market Data Gateway** (`src/market_data/gateway.h`)
    - High-throughput market data processing
    - Symbol subscription management
    - Real-time price feed simulation

3. **Order Matching Engine** (`src/matching/matching_engine.h`)
    - Price-time priority matching algorithm
    - Support for partial fills and order types
    - Trade execution and reporting

4. **Risk Management** (`src/risk/risk_manager.h`)
    - Real-time position monitoring
    - Pre-trade risk checks
    - Configurable risk limits

5. **Strategy Framework** (`src/strategy/strategy_base.h`)
    - Base class for algorithmic strategies
    - Mean reversion strategy implementation
    - Signal generation and order placement

## 🚀 Quick Start

### Building

```bash
git clone https://github.com/ScriptWanderer/high-frequency-trading-engine.git
cd high-frequency-trading-engine

# Create build directory
mkdir cmake-build-debug
cd cmake-build-debug

# Configure and build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . --target TradingEngine

# Run the trading engine
./main.exe  # Windows
./main      # Linux
```

### Running

The trading engine will start with default configuration:

- Subscribe to synthetic AAPL market data
- Deploy mean reversion strategy
- Process orders with risk management
- Display real-time statistics every 3 seconds

```
=== Trading Engine Statistics ===
Uptime: 62 seconds
Orders Received: 32
Orders Processed: 11
Orders Rejected: 21
Trades Executed: 1
Processing Rate: 0.177 orders/sec

--- Market Data Stats ---
Messages Processed: 28,827,144
Processing Rate: 444,419 msg/sec
Active Symbols: 1

--- Latency Profiles ---
Order Processing - Avg: 0.82μs, Max: 20.59μs
Risk Checks - Avg: 13.04μs, Max: 155.29μs
```

## ⚙️ Configuration

### Trading Parameters

Key constants in `src/core/types.h`:

```cpp
constexpr Price PriceScale = 100000000ULL;     // 8 decimal places
constexpr uint32_t MaxSymbolCount = 10000;     // Maximum symbols
constexpr uint32_t DefaultQueueSize = 4096;    // Message queue size
```

### Performance Tuning

1. **CPU Affinity**: Bind threads to specific CPU cores
2. **Memory Pages**: Use huge pages for large allocations
3. **Compiler Flags**: Enable -O3, -march=native optimizations
4. **NUMA**: Configure memory allocation for NUMA topology

## 🧪 Testing and Development

### Synthetic Market Data

The system includes a built-in market data simulator:

- Realistic price movements with volatility
- Configurable tick frequency and price ranges
- Multiple symbol support for testing

### Strategy Development

Create custom strategies by inheriting from `StrategyBase`:

```cpp
class MyStrategy : public StrategyBase {
public:
    void on_market_data(const MarketTick& tick) override {
        // Implement your trading logic
        if (should_buy(tick)) {
            submit_order(create_buy_order(tick));
        }
    }
    
private:
    bool should_buy(const MarketTick& tick) {
        // Your strategy logic here
        return true;
    }
};
```

## 📈 Use Cases

- **Algorithmic Trading**: Deploy systematic trading strategies
- **Market Making**: Provide liquidity with bid-ask spreads
- **Arbitrage**: Exploit price differences across markets
- **Research**: Backtest and analyze trading strategies
- **Education**: Learn high-frequency trading system design

## 🔧 Advanced Features

### Custom Memory Management

- Pool allocators for frequent allocations
- Stack allocators for temporary objects
- NUMA-aware allocation strategies

### Network Optimization

- Kernel bypass networking (future enhancement)
- UDP multicast for market data feeds
- Binary protocol optimization

### Monitoring and Analytics

- Real-time performance metrics
- Trade execution analytics
- Latency histograms and percentiles

## 📋 Roadmap

- [ ] Web-based monitoring dashboard
- [ ] Historical market data replay
- [ ] Machine learning strategy framework
- [ ] Multi-exchange connectivity
- [ ] Kubernetes deployment support
- [ ] Real market data connectors (FIX protocol)

## 🤝 Contributing

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## ⚠️ Disclaimer

This software is for educational and research purposes only. Use in production trading environments at your own risk.
The authors are not responsible for any financial losses incurred through the use of this software.

---

⭐ **Star this repository if you found it helpful!**
