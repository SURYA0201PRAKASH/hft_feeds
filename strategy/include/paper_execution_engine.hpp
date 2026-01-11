#pragma once
#include <optional>
#include <vector>

#include "order_intent.hpp"
#include "trade.hpp"
#include "virtual_wallet.hpp"
#include "imbalance_taker.hpp"   // MUST define MarketState

class PaperExecutionEngine {
public:
    struct Params {
        double initial_cash;
        bool allow_short;
        bool require_cash;

        Params(double cash = 10000.0, bool allow = false, bool require = true)
            : initial_cash(cash), allow_short(allow), require_cash(require) {}
    };

    PaperExecutionEngine();                 // default constructor
    explicit PaperExecutionEngine(Params p); // NO default argument

    void on_market(const MarketState& s);
    std::optional<Trade> submit(const MarketState& s, const OrderIntent& oi);

    const VirtualWallet& wallet() const { return wallet_; }
    const std::vector<Trade>& trades() const { return trades_; }

private:
    Params p_;
    VirtualWallet wallet_;
    std::vector<Trade> trades_;
};
