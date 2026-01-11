#pragma once

#include "IFeed.hpp"
#include "BinanceL2Feed.hpp"
#include "BybitL2Feed.hpp"
#include "../src/core/ZmqPublisher.hpp"
#include <memory>
#include <thread>
#include <vector>
#include <string>
#include <unordered_map>
#include <deque>
#include <utility>
#include <cstdint>
#include "StateDB.hpp"
#include <mutex>
#include <atomic>

/* ================= Exchange Choice ================= */

enum class ExchangeChoice {
    Binance,
    Bybit,
    Both
};

/* ================= Key: (exchange, instrument) ================= */

struct MarketKey {
    std::string exchange;    // "binance", "bybit"
    std::string instrument;  // "ETHUSDT"

    bool operator==(const MarketKey& o) const {
        return exchange == o.exchange && instrument == o.instrument;
    }
};

struct MarketKeyHash {
    std::size_t operator()(const MarketKey& k) const noexcept {
        std::size_t h1 = std::hash<std::string>{}(k.exchange);
        std::size_t h2 = std::hash<std::string>{}(k.instrument);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

/* ================= RL-Ready State Vector ================= */

struct StateVector {
    // Price
    double mid = 0.0;

    // Returns
    double r1  = 0.0;
    double r5  = 0.0;
    double r10 = 0.0;

    // Liquidity
    double spread    = 0.0;
    //SuPr moving it to strategy double imbalance = 0.0;

    // Depth (top-5)
    double bid_vol[5] = {0,0,0,0,0};
    double ask_vol[5] = {0,0,0,0,0};

    // Cross-exchange signal
    double cross_ex_signal = 0.0;
};

/* ================= MarketDataManager ================= */

class MarketDataManager {
public:
    MarketDataManager(
        ExchangeChoice choice,
        const std::vector<std::string>& instruments,
        int orderBookDepth,
        int orderBookPollFrequencyInMs
    );

    void start_all();
    void join_all();

    // ZMQ
    std::unique_ptr<ZmqPublisher> zmq_pub_;
    std::mutex zmq_pub_mtx_;

private:
    void snapshot_loop();

private:
    std::vector<std::unique_ptr<IFeed>> feeds_;
    std::vector<std::thread> threads_;

    // --- shared state (written by feed threads, read by snapshot thread) ---
    std::mutex state_mtx_;

    std::unordered_map<MarketKey, StateVector, MarketKeyHash> state_;
    std::unordered_map<
        MarketKey,
        std::deque<std::pair<uint64_t, double>>,
        MarketKeyHash
    > price_history_;

    // Keep the latest quote + orderbook so serializer still works
    std::unordered_map<MarketKey, Quote, MarketKeyHash> last_quote_;
    std::unordered_map<MarketKey, OrderBook, MarketKeyHash> last_ob_;

    StateDB state_db_{"market_state.db"};

    int order_book_depth_ = 20;
    int snapshot_freq_ms_ = 50;

    std::thread snapshot_thread_;
    std::atomic<bool> running_{false};
};
