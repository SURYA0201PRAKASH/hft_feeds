#include "virtual_wallet.hpp"

VirtualWallet::VirtualWallet(double initial_cash) {
    s_.cash = initial_cash;
}

void VirtualWallet::mark(double mid) {
    if (s_.pos != 0.0)
        s_.unrealized_pnl = s_.pos * (mid - s_.avg_entry);
    else
        s_.unrealized_pnl = 0.0;
}

void VirtualWallet::on_fill_buy(double qty, double price) {
    s_.cash -= qty * price;

    double new_pos = s_.pos + qty;
    if (s_.pos == 0.0)
        s_.avg_entry = price;
    else
        s_.avg_entry = (s_.pos * s_.avg_entry + qty * price) / new_pos;

    s_.pos = new_pos;
}

void VirtualWallet::on_fill_sell(double qty, double price) {
    s_.cash += qty * price;
    s_.realized_pnl += qty * (price - s_.avg_entry);
    s_.pos -= qty;

    if (s_.pos == 0.0)
        s_.avg_entry = 0.0;
}
