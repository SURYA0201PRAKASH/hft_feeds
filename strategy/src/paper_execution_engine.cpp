#include "paper_execution_engine.hpp"

PaperExecutionEngine::PaperExecutionEngine(Params p)
    : p_(p), wallet_(p.initial_cash) {}

void PaperExecutionEngine::on_market(const MarketState& s) {
    wallet_.mark(s.mid);
}

std::optional<Trade> PaperExecutionEngine::submit(
    const MarketState& s,
    const OrderIntent& oi)
{
    if (oi.qty <= 0.0) return std::nullopt;
    if (s.bid <= 0.0 || s.ask <= 0.0) return std::nullopt;

    if (oi.side == Side::Buy) {
        if (oi.price < s.ask) return std::nullopt;
        if (p_.require_cash && wallet_.snap().cash < oi.qty * s.ask)
            return std::nullopt;

        wallet_.on_fill_buy(oi.qty, s.ask);
        wallet_.mark(s.mid);

        Trade t{oi.key, Side::Buy, s.ask, oi.qty, oi.ts_ms, wallet_.snap().pos};
        trades_.push_back(t);
        return t;
    }

    if (!p_.allow_short && wallet_.snap().pos < oi.qty)
        return std::nullopt;

    if (oi.price > s.bid) return std::nullopt;

    wallet_.on_fill_sell(oi.qty, s.bid);
    wallet_.mark(s.mid);

    Trade t{oi.key, Side::Sell, s.bid, oi.qty, oi.ts_ms, wallet_.snap().pos};
    trades_.push_back(t);
    return t;
}
