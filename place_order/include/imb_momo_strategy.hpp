#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

struct MarketTick {
    std::string exchange;
    std::string instrument;
    std::int64_t ts_ms = 0;

    double bid = 0.0;
    double ask = 0.0;
    double mid = 0.0;
    double spread = 0.0;

    double r1 = 0.0;
    double r5 = 0.0;
    double r10 = 0.0;

    std::vector<double> bid_vol;
    std::vector<double> ask_vol;
};

enum class Side { Buy, Sell };

struct StrategySignal {
    enum class Type { Enter, Exit } type = Type::Enter;
    Side side = Side::Buy;

    double entry_px = 0.0;
    double tp_px = 0.0;
    double sl_px = 0.0;

    double imbalance = 0.0;
    std::string reason;
    std::int64_t ts_ms = 0;
};

struct ImbMomoParams {
    double imb_threshold = 0.20;
    double r1_threshold  = 0.00002;

    double max_spread = 0.05;
    double max_abs_r1 = 0.00050;

    double tp_spread_mult = 2.0;
    double sl_spread_mult = 3.0;

    std::int64_t max_hold_ms = 30'000;
    std::int64_t cooldown_ms = 2'000;
};

class ImbMomoStrategy {
public:
    explicit ImbMomoStrategy(ImbMomoParams p = {});
    std::optional<StrategySignal> on_tick(const MarketTick& t);

private:
    ImbMomoParams p_;

    bool in_pos_ = false;
    std::optional<Side> pos_side_;

    double entry_px_ = 0.0;
    double tp_px_ = 0.0;
    double sl_px_ = 0.0;

    std::int64_t entry_ts_ms_ = 0;
    std::int64_t last_exit_ts_ms_ = 0;

    static double sum(const std::vector<double>& v);
    static double calc_imbalance(double bid_sum, double ask_sum);
};
