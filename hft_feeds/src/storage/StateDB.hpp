#pragma once
#include <sqlite3.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct StateSnapshot {
    std::string exchange;
    std::string instrument;
    std::uint64_t ts_ms{0};

    double mid{0.0};
    double spread{0.0};
    double r1{0.0}, r5{0.0}, r10{0.0};
    double imbalance{0.0};
    double cross_ex_signal{0.0};

    double bid_vol[5]{0,0,0,0,0};
    double ask_vol[5]{0,0,0,0,0};
};

class StateDB {
public:
    explicit StateDB(std::string db_path,
                     int flush_ms = 200,
                     std::size_t max_queue = 50000);
    ~StateDB();

    StateDB(const StateDB&) = delete;
    StateDB& operator=(const StateDB&) = delete;

    // B2 producer API (called from MarketDataManager threads)
    void push(StateSnapshot s);

    // Start/stop writer thread
    bool start();
    void stop();

private:
    bool open_connection();
    void close_connection();
    bool init_schema_and_pragmas();

    bool prepare_statements();
    void finalize_statements();

    void writer_loop();
    bool insert_batch(const std::vector<StateSnapshot>& batch);

private:
    std::string db_path_;
    int flush_ms_;
    std::size_t max_queue_;

    sqlite3* db_{nullptr};
    sqlite3_stmt* stmt_insert_{nullptr};

    std::atomic<bool> running_{false};
    std::thread writer_;

    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<StateSnapshot> q_;
};
