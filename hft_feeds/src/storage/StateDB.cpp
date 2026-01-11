#include "StateDB.hpp"
#include <iostream>
#include <chrono>

static void log_sqlite_err(sqlite3* db, const char* where) {
    std::cerr << "[StateDB] " << where << " sqlite_err="
              << (db ? sqlite3_errmsg(db) : "null-db") << "\n";
}

StateDB::StateDB(std::string db_path, int flush_ms, std::size_t max_queue)
    : db_path_(std::move(db_path))
    , flush_ms_(flush_ms)
    , max_queue_(max_queue)
{}

StateDB::~StateDB() {
    stop();
}

bool StateDB::start() {
    if (running_.exchange(true)) return true;

    if (!open_connection()) {
        running_ = false;
        return false;
    }
    if (!init_schema_and_pragmas()) {
        running_ = false;
        close_connection();
        return false;
    }
    if (!prepare_statements()) {
        running_ = false;
        close_connection();
        return false;
    }

    writer_ = std::thread(&StateDB::writer_loop, this);
    return true;
}

void StateDB::stop() {
    if (!running_.exchange(false)) return;

    cv_.notify_all();
    if (writer_.joinable()) writer_.join();

    finalize_statements();
    close_connection();
}

void StateDB::push(StateSnapshot s) {
    if (!running_) return;

    {
        std::lock_guard<std::mutex> lk(mtx_);

        // Drop oldest if queue is too large (protect memory / avoid stalls)
        if (q_.size() >= max_queue_) {
            q_.pop_front();
        }
        q_.push_back(std::move(s));
    }
    cv_.notify_one();
}

bool StateDB::open_connection() {
    int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        log_sqlite_err(db_, "sqlite3_open");
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    return true;
}

void StateDB::close_connection() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

static bool exec_sql(sqlite3* db, const std::string& sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "[StateDB] sqlite_exec failed: " << (err ? err : "") << "\n";
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool StateDB::init_schema_and_pragmas() {
    // Pragmas (WAL + speed sane defaults)
    if (!exec_sql(db_, "PRAGMA journal_mode=WAL;")) return false;
    if (!exec_sql(db_, "PRAGMA synchronous=NORMAL;")) return false;
    if (!exec_sql(db_, "PRAGMA temp_store=MEMORY;")) return false;
    if (!exec_sql(db_, "PRAGMA foreign_keys=ON;")) return false;
    if (!exec_sql(db_, "PRAGMA busy_timeout=2000;")) return false;

    // Schema: single table (industry standard)
    const char* create_sql =
        "CREATE TABLE IF NOT EXISTS market_state ("
        "  ts_ms INTEGER NOT NULL,"
        "  exchange TEXT NOT NULL,"
        "  instrument TEXT NOT NULL,"
        "  mid REAL NOT NULL,"
        "  spread REAL NOT NULL,"
        "  r1 REAL NOT NULL,"
        "  r5 REAL NOT NULL,"
        "  r10 REAL NOT NULL,"
        "  imbalance REAL NOT NULL,"
        "  cross_ex_signal REAL NOT NULL,"
        "  bid_v1 REAL NOT NULL, bid_v2 REAL NOT NULL, bid_v3 REAL NOT NULL, bid_v4 REAL NOT NULL, bid_v5 REAL NOT NULL,"
        "  ask_v1 REAL NOT NULL, ask_v2 REAL NOT NULL, ask_v3 REAL NOT NULL, ask_v4 REAL NOT NULL, ask_v5 REAL NOT NULL"
        ");";

    if (!exec_sql(db_, create_sql)) return false;

    // Indexes
    if (!exec_sql(db_, "CREATE INDEX IF NOT EXISTS idx_market_state_ts ON market_state(ts_ms);")) return false;
    if (!exec_sql(db_, "CREATE INDEX IF NOT EXISTS idx_market_state_key ON market_state(exchange, instrument, ts_ms);")) return false;

    return true;
}

bool StateDB::prepare_statements() {
    const char* ins =
        "INSERT INTO market_state ("
        " ts_ms, exchange, instrument, mid, spread, r1, r5, r10, imbalance, cross_ex_signal,"
        " bid_v1,bid_v2,bid_v3,bid_v4,bid_v5,"
        " ask_v1,ask_v2,ask_v3,ask_v4,ask_v5"
        ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";

    int rc = sqlite3_prepare_v2(db_, ins, -1, &stmt_insert_, nullptr);
    if (rc != SQLITE_OK) {
        log_sqlite_err(db_, "sqlite3_prepare_v2(insert)");
        stmt_insert_ = nullptr;
        return false;
    }
    return true;
}

void StateDB::finalize_statements() {
    if (stmt_insert_) {
        sqlite3_finalize(stmt_insert_);
        stmt_insert_ = nullptr;
    }
}

bool StateDB::insert_batch(const std::vector<StateSnapshot>& batch) {
    if (batch.empty()) return true;

    // One transaction per batch = fast
    if (!exec_sql(db_, "BEGIN IMMEDIATE TRANSACTION;")) return false;

    for (const auto& s : batch) {
        sqlite3_reset(stmt_insert_);
        sqlite3_clear_bindings(stmt_insert_);

        int idx = 1;
        sqlite3_bind_int64(stmt_insert_, idx++, static_cast<sqlite3_int64>(s.ts_ms));
        sqlite3_bind_text(stmt_insert_, idx++, s.exchange.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt_insert_, idx++, s.instrument.c_str(), -1, SQLITE_TRANSIENT);

        sqlite3_bind_double(stmt_insert_, idx++, s.mid);
        sqlite3_bind_double(stmt_insert_, idx++, s.spread);
        sqlite3_bind_double(stmt_insert_, idx++, s.r1);
        sqlite3_bind_double(stmt_insert_, idx++, s.r5);
        sqlite3_bind_double(stmt_insert_, idx++, s.r10);
        sqlite3_bind_double(stmt_insert_, idx++, s.imbalance);
        sqlite3_bind_double(stmt_insert_, idx++, s.cross_ex_signal);

        for (int i = 0; i < 5; ++i) sqlite3_bind_double(stmt_insert_, idx++, s.bid_vol[i]);
        for (int i = 0; i < 5; ++i) sqlite3_bind_double(stmt_insert_, idx++, s.ask_vol[i]);

        int rc = sqlite3_step(stmt_insert_);
        if (rc != SQLITE_DONE) {
            log_sqlite_err(db_, "sqlite3_step(insert)");
            exec_sql(db_, "ROLLBACK;");
            return false;
        }
    }

    if (!exec_sql(db_, "COMMIT;")) {
        exec_sql(db_, "ROLLBACK;");
        return false;
    }
    return true;
}

void StateDB::writer_loop() {
    std::vector<StateSnapshot> batch;
    batch.reserve(5000);

    while (running_) {
        // Wait for flush interval OR data
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait_for(lk, std::chrono::milliseconds(flush_ms_), [&]{
            return !running_ || !q_.empty();
        });

        if (!running_ && q_.empty())
            break;

        // Drain queue
        batch.clear();
        while (!q_.empty()) {
            batch.push_back(std::move(q_.front()));
            q_.pop_front();
        }
        lk.unlock();

        // Write batch
        if (!insert_batch(batch)) {
            std::cerr << "[StateDB] insert_batch failed (continuing)\n";
        }
    }

    // final flush on exit
    std::vector<StateSnapshot> tail;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        while (!q_.empty()) {
            tail.push_back(std::move(q_.front()));
            q_.pop_front();
        }
    }
    if (!tail.empty()) insert_batch(tail);
}
