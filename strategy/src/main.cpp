#include <csignal>
#include <iomanip>
#include <iostream>
#include <thread>
#include <unordered_map>

// threading
#include <condition_variable>
#include <mutex>
#include <queue>

#include <atomic>
#include <chrono>
#include <sstream>

#include "imbalance_taker.hpp"
#include "zmq_market_subscriber.hpp"
#include "paper_execution_engine.hpp"
#include "order_intent.hpp"

// ------------------- shutdown -------------------
static volatile std::sig_atomic_t g_stop = 0;
static void on_sigint(int) { g_stop = 1; }

// ------------------- market queue -------------------
static std::mutex g_mtx;
static std::condition_variable g_cv;
static std::queue<MarketState> g_q;
static constexpr size_t MAX_Q = 200000;

static void push_state(MarketState&& s) {
    std::unique_lock<std::mutex> lk(g_mtx);
    if (g_q.size() >= MAX_Q) g_q.pop();   // drop oldest
    g_q.push(std::move(s));
    lk.unlock();
    g_cv.notify_one();
}

static bool pop_state(MarketState& out) {
    std::unique_lock<std::mutex> lk(g_mtx);
    g_cv.wait(lk, [] { return g_stop || !g_q.empty(); });
    if (g_stop && g_q.empty()) return false;
    out = std::move(g_q.front());
    g_q.pop();
    return true;
}

// ------------------- current key (for manual input default) -------------------
static std::mutex g_key_mtx;
static std::string g_current_key;

static void set_current_key(const std::string& k) {
    std::lock_guard<std::mutex> lk(g_key_mtx);
    g_current_key = k;
}

static std::string get_current_key() {
    std::lock_guard<std::mutex> lk(g_key_mtx);
    return g_current_key;
}

// ------------------- latest market snapshot (per symbol key) -------------------
static std::mutex g_last_mtx;
static std::unordered_map<std::string, MarketState> g_last;

static void update_last(const MarketState& s) {
    std::lock_guard<std::mutex> lk(g_last_mtx);
    g_last[s.key()] = s;
}

static bool get_last(const std::string& key, MarketState& out) {
    std::lock_guard<std::mutex> lk(g_last_mtx);
    auto it = g_last.find(key);
    if (it == g_last.end()) return false;
    out = it->second;
    return true;
}

// Wait a short time for first snapshot (so first order can fill immediately)
static bool wait_last(const std::string& key, MarketState& out, int max_wait_ms = 2000) {
    auto start = std::chrono::steady_clock::now();
    while (!g_stop) {
        if (get_last(key, out)) return true;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() > max_wait_ms)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// ------------------- execution queue (orders to execute) -------------------
static std::mutex g_exec_mtx;
static std::condition_variable g_exec_cv;
static std::queue<OrderIntent> g_exec_q;

static void push_exec(OrderIntent&& oi) {
    {
        std::lock_guard<std::mutex> lk(g_exec_mtx);
        g_exec_q.push(std::move(oi));
    }
    g_exec_cv.notify_one();
}

static bool pop_exec(OrderIntent& out) {
    std::unique_lock<std::mutex> lk(g_exec_mtx);
    g_exec_cv.wait(lk, [] { return g_stop || !g_exec_q.empty(); });
    if (g_stop && g_exec_q.empty()) return false;
    out = std::move(g_exec_q.front());
    g_exec_q.pop();
    return true;
}

// ------------------- throttle printing -------------------
static std::atomic<uint64_t> g_rx_print_ctr{0};

// ------------------- main -------------------
int main(int argc, char** argv) {
    std::signal(SIGINT, on_sigint);

    std::string endpoint = "tcp://127.0.0.1:5555";
    std::string filter   = "state.";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--endpoint" && i + 1 < argc) endpoint = argv[++i];
        else if (a == "--filter" && i + 1 < argc) filter = argv[++i];
    }

    std::cout << "SUB endpoint: " << endpoint << "\n";
    std::cout << "SUB filter  : " << filter << "\n";
    std::cout << "Press Ctrl+C to stop.\n\n";

    // ----------- ZMQ subscriber (receiver thread owns it) -----------
    ZmqMarketSubscriber sub(endpoint, filter);

    // ----------- manual input thread -----------
    std::thread input_thread([&]() {
        std::cout << "\nManual paper trading enabled.\n";
        std::cout << "Commands:\n";
        std::cout << "  b <qty>   => BUY market (fills at ASK)\n";
        std::cout << "  s <qty>   => SELL market (fills at BID)\n";
        std::cout << "Example: b 0.01\n\n";

        std::string line;
        while (!g_stop && std::getline(std::cin, line)) {
            if (line.empty()) continue;

            std::stringstream ss(line);
            char side;
            double qty;
            ss >> side >> qty;

            if (!ss || qty <= 0.0 || (side != 'b' && side != 's')) {
                std::cout << "[INPUT] Invalid. Use: b 0.01 or s 0.01\n";
                continue;
            }

            std::string key = get_current_key();
            int waited = 0;
            while (!g_stop && key.empty() && waited < 2000) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                waited += 50;
                key = get_current_key();
            }
            if (key.empty()) {
                std::cout << "[INPUT] Still no market key. Try again after RX starts.\n";
                continue;
            }

            OrderIntent oi;
            oi.key   = key;
            oi.qty   = qty;
            oi.ts_ms = 0;                 // will be set at execution time
            oi.side  = (side == 'b') ? Side::Buy : Side::Sell;
            oi.price = 0.0;               // will be set at execution time (bid/ask)

            push_exec(std::move(oi));
            std::cout << "[INPUT] queued " << (side == 'b' ? "BUY " : "SELL ")
                      << qty << " for " << key << "\n";
        }
    });

    // ----------- strategy worker thread (NO execution / NO wallet here) -----------
    std::thread worker([&]() {
        std::unordered_map<std::string, ImbalanceTaker> strat_map;

        std::cout.setf(std::ios::fixed);
        std::cout << std::setprecision(8);

        MarketState s;
        while (pop_state(s)) {
            set_current_key(s.key());
            update_last(s);

            auto& strat =
                strat_map.try_emplace(s.key(), ImbalanceTaker(0.6, 150)).first->second;

            int sig = strat.on_state(s);

            // Throttle printing so humans can type
            if (++g_rx_print_ctr % 50 == 0) {
                std::cout
                    << "[RX] " << s.exchange << " " << s.instrument
                    << " TS=" << s.ts_ms
                    << " BID=" << s.bid
                    << " ASK=" << s.ask
                    << " MID=" << s.mid
                    << " SPR=" << s.spread
                    << " r1=" << s.r1
                    << " r5=" << s.r5
                    << " r10=" << s.r10
                    << " IMB=" << s.imbalance;

                if (sig == +1) std::cout << " => BUY (pos=" << strat.position() << ")";
                else if (sig == -1) std::cout << " => SELL (pos=" << strat.position() << ")";
                else std::cout << " => HOLD (pos=" << strat.position() << ")";

                std::cout << "\n";
            }

            // (Optional later) If you want strategy-driven orders:
            // if (sig == +1) push_exec(OrderIntent{...});
            // if (sig == -1) push_exec(OrderIntent{...});
        }
    });

    // ----------- execution thread (ONLY place that owns PaperExecutionEngine) -----------
    std::thread exec_thread([&]() {
        PaperExecutionEngine::Params p;
        p.initial_cash = 10000.0;
        p.allow_short  = false;
        p.require_cash = true;

        PaperExecutionEngine exec(p);

        while (!g_stop) {
            OrderIntent oi;
            if (!pop_exec(oi)) break;

            MarketState s;
            if (!wait_last(oi.key, s, 2000)) {
                std::cout << "[EXEC] No market snapshot for key=" << oi.key << " (skipping)\n";
                continue;
            }

            // Marketable Day-1 behavior
            if (oi.side == Side::Buy) oi.price = s.ask;
            else                     oi.price = s.bid;

            oi.ts_ms = s.ts_ms;

            std::cout << "\n[ORDER] " << (oi.side == Side::Buy ? "BUY " : "SELL ")
                      << oi.qty << " @ " << oi.price
                      << " key=" << oi.key << "\n";

            exec.on_market(s);
            auto trade = exec.submit(s, oi);

            if (trade) {
                std::cout << "[FILL ] " << (trade->side == Side::Buy ? "BUY " : "SELL ")
                          << trade->qty << " @ " << trade->price
                          << " pos_after=" << trade->pos_after << "\n";
            } else {
                std::cout << "[NOFILL]\n";
            }

            const auto& w = exec.wallet().snap();
            std::cout << "[WALLET] cash=" << w.cash
                      << " pos=" << w.pos
                      << " avg=" << w.avg_entry
                      << " rPnL=" << w.realized_pnl
                      << " uPnL=" << w.unrealized_pnl
                      << "\n\n";
        }
    });

    // ----------- receiver loop (main thread) -----------
    while (!g_stop) {
        auto ms = sub.recv_one();
        if (!ms) continue;
        push_state(std::move(*ms));
    }

    // ----------- shutdown / join -----------
    g_stop = 1;
    g_cv.notify_all();
    g_exec_cv.notify_all();

    if (input_thread.joinable()) input_thread.join();
    if (worker.joinable()) worker.join();
    if (exec_thread.joinable()) exec_thread.join();

    std::cout << "\nStopping strategy.\n";
    return 0;
}
