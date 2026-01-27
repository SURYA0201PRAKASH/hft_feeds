#pragma once
#include <string>
#include <atomic>

class BybitDemoClient {
public:
    BybitDemoClient(std::string api_key, std::string api_secret);

    bool ready() const;

    // ---- TIME SYNC (FIX for retCode=10002) ----
    // Call once at startup (and optionally every 30-60s)
    bool sync_time();
    long long now_ms_adjusted() const;

    // Returns response JSON string
    std::string place_market_order(
        const std::string& category,  // "spot" or "linear"
        const std::string& symbol,    // e.g., "ETHUSDT"
        const std::string& side,      // "Buy" or "Sell"
        double qty,
		bool reduceOnly = false
    );

    std::string place_limit_order(
        const std::string& category,
        const std::string& symbol,
        const std::string& side,
        double qty,
        double price,
        const std::string& tif = "GTC",
		bool reduceOnly = false
    );

    // Cancel an order by orderId
    std::string cancel_order(
        const std::string& category,
        const std::string& symbol,
        const std::string& orderId
    );

    std::string get_positions(const std::string& category, const std::string& symbol);

    // Returns +size for long, -size for short, 0 if flat (linear futures)
    double get_position_size_linear(const std::string& symbol);

    // Closes current position using reduce-only MARKET order
    std::string close_position_market_reduce_only(const std::string& category, const std::string& symbol);

    std::string get_executions(
        const std::string& category,
        const std::string& symbol,
        long long startTimeMs
    );

    std::string get_executions(
        const std::string& category,
        const std::string& symbol,
        long long startTimeMs,
        const std::string& cursor
    );

    std::string get(const std::string& path, const std::string& query_string);

    std::string get_transaction_log(
        const std::string& category,
        const std::string& symbol,
        long long startTimeMs,
        long long endTimeMs,
        const std::string& cursor,
        const std::string& type = "SETTLEMENT"
    );

    std::string get_transaction_log(
        const std::string& category,
        const std::string& symbol,
        long long startTimeMs,
        long long endTimeMs,
        const std::string& type = "SETTLEMENT"
    );

private:
    std::string api_key_;
    std::string api_secret_;

    // server_ms - local_ms (if local clock is ahead, offset will be negative)
    std::atomic<long long> time_offset_ms_{0};
    std::atomic<long long> last_sync_local_ms_{0};

    std::string post(const std::string& path, const std::string& body_json);

    // Public endpoint call without signature (for /v5/market/time)
    std::string get_public(const std::string& path, const std::string& query_string);
};
