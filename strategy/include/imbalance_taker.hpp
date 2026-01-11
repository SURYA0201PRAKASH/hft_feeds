#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

// Parsed message your MarketDataManager publishes
struct MarketState {
    std::string schema;
    std::string exchange;
    std::string instrument;
    std::int64_t ts_ms = 0;

    // top-of-book
    double bid = 0.0;
    double ask = 0.0;
    double mid = 0.0;
    double spread = 0.0;

    // returns
    double r1 = 0.0;
    double r5 = 0.0;
    double r10 = 0.0;

    // depth top-5 volumes
    std::array<double, 5> bid_vol{};
    std::array<double, 5> ask_vol{};

    // features
    double imbalance = 0.0;

    // Helper: key for maps
    std::string key() const { return exchange + "|" + instrument; }
};

class ImbalanceTaker {
public:
    ImbalanceTaker(double thresh = 0.6, int hold_ticks = 150)
        : thresh_(thresh), hold_ticks_(hold_ticks) {}

    // Returns:
    //  +1 => BUY (enter long / flip to long)
    //  -1 => SELL (enter short / flip to short)
    //   0 => HOLD / do nothing
    int on_state(const MarketState& s);

    int position() const { return state_; }

private:
    double thresh_;
    int hold_ticks_;
    int state_ = 0;      // +1 long, 0 flat, -1 short
    int ticks_left_ = 0; // countdown while in position
};
