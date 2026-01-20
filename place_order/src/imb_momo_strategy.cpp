#include "imb_momo_strategy.hpp"
#include <cmath>

ImbMomoStrategy::ImbMomoStrategy(ImbMomoParams p) : p_(p) {}

double ImbMomoStrategy::sum(const std::vector<double>& v) {
    double s = 0.0; for (double x : v) s += x; return s;
}

double ImbMomoStrategy::calc_imbalance(double bid_sum, double ask_sum) {
    double d = bid_sum + ask_sum;
    if (d <= 0.0) return 0.0;
    return (bid_sum - ask_sum) / d;
}

std::optional<StrategySignal> ImbMomoStrategy::on_tick(const MarketTick& t) {
    if (t.bid <= 0.0 || t.ask <= 0.0 || t.mid <= 0.0) return std::nullopt;
    if (t.spread <= 0.0 || t.spread > p_.max_spread) return std::nullopt;
    if (std::abs(t.r1) > p_.max_abs_r1) return std::nullopt;

    double bv = sum(t.bid_vol);
    double av = sum(t.ask_vol);
    double imb = calc_imbalance(bv, av);

    // cooldown after exit
    if (!in_pos_ && last_exit_ts_ms_ > 0 && (t.ts_ms - last_exit_ts_ms_ < p_.cooldown_ms))
        return std::nullopt;

    // FLAT -> ENTER
    if (!in_pos_) {
        bool long_ok  = (imb > +p_.imb_threshold) && (t.r1 > +p_.r1_threshold) && (t.r5 >= 0.0);
        bool short_ok = (imb < -p_.imb_threshold) && (t.r1 < -p_.r1_threshold) && (t.r5 <= 0.0);
        if (!long_ok && !short_ok) return std::nullopt;

        StrategySignal sig;
        sig.type = StrategySignal::Type::Enter;
        sig.ts_ms = t.ts_ms;
        sig.imbalance = imb;

        if (long_ok) {
            sig.side = Side::Buy;
            sig.entry_px = t.ask;
            sig.tp_px = sig.entry_px + p_.tp_spread_mult * t.spread;
            sig.sl_px = sig.entry_px - p_.sl_spread_mult * t.spread;
            sig.reason = "ENTER_LONG (imb+ & r1+)";
        } else {
            sig.side = Side::Sell;
            sig.entry_px = t.bid;
            sig.tp_px = sig.entry_px - p_.tp_spread_mult * t.spread;
            sig.sl_px = sig.entry_px + p_.sl_spread_mult * t.spread;
            sig.reason = "ENTER_SHORT (imb- & r1-)";
        }

        // assume filled for now (today goal = get real pnl quickly)
        in_pos_ = true;
        pos_side_ = sig.side;
        entry_px_ = sig.entry_px;
        tp_px_ = sig.tp_px;
        sl_px_ = sig.sl_px;
        entry_ts_ms_ = t.ts_ms;

        return sig;
    }

    // IN POSITION -> EXIT
    Side s = *pos_side_;
    bool hit_tp=false, hit_sl=false;

    if (s == Side::Buy)  { hit_tp = (t.mid >= tp_px_); hit_sl = (t.mid <= sl_px_); }
    if (s == Side::Sell) { hit_tp = (t.mid <= tp_px_); hit_sl = (t.mid >= sl_px_); }

    bool time_stop = (p_.max_hold_ms > 0) && ((t.ts_ms - entry_ts_ms_) >= p_.max_hold_ms);

    if (hit_tp || hit_sl || time_stop) {
        StrategySignal sig;
        sig.type = StrategySignal::Type::Exit;
        sig.side = (s == Side::Buy) ? Side::Sell : Side::Buy; // to close
        sig.ts_ms = t.ts_ms;
        sig.imbalance = imb;
        sig.entry_px = entry_px_;
        sig.tp_px = tp_px_;
        sig.sl_px = sl_px_;
        sig.reason = hit_tp ? "EXIT_TP" : (hit_sl ? "EXIT_SL" : "EXIT_TIME");

        in_pos_ = false;
        pos_side_.reset();
        entry_px_ = tp_px_ = sl_px_ = 0.0;
        entry_ts_ms_ = 0;
        last_exit_ts_ms_ = t.ts_ms;

        return sig;
    }

    return std::nullopt;
}
