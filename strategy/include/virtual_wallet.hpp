#pragma once

struct WalletSnapshot {
    double cash = 0.0;
    double pos = 0.0;
    double avg_entry = 0.0;
    double realized_pnl = 0.0;
    double unrealized_pnl = 0.0;
};

class VirtualWallet {
public:
    explicit VirtualWallet(double initial_cash);

    void mark(double mid);
    void on_fill_buy(double qty, double price);
    void on_fill_sell(double qty, double price);

    const WalletSnapshot& snap() const { return s_; }

private:
    WalletSnapshot s_;
};
