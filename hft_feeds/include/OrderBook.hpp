#pragma once
#include <map>
#include <vector>
#include <functional>
#include <utility>

class OrderBook {
public:
    // Bids: highest price first
    std::map<double, double, std::greater<double>> bids;
    // Asks: lowest price first
    std::map<double, double> asks;

    void clear() {
        bids.clear();
        asks.clear();
    }

    // Apply full snapshot
    void apply_snapshot(const std::vector<std::pair<double, double>>& bid_lvls,
                        const std::vector<std::pair<double, double>>& ask_lvls)
    {
        clear();
        for (const auto& [px, qty] : bid_lvls) {
            if (qty > 0) bids[px] = qty;
        }
        for (const auto& [px, qty] : ask_lvls) {
            if (qty > 0) asks[px] = qty;
        }
    }

    // Apply deltas (updates on top of existing book)
    void apply_delta(const std::vector<std::pair<double, double>>& bid_lvls,
                     const std::vector<std::pair<double, double>>& ask_lvls)
    {
        for (const auto& [px, qty] : bid_lvls) {
            if (qty <= 0) {
                bids.erase(px);
            } else {
                bids[px] = qty;
            }
        }
        for (const auto& [px, qty] : ask_lvls) {
            if (qty <= 0) {
                asks.erase(px);
            } else {
                asks[px] = qty;
            }
        }
    }

    double best_bid() const {
        return bids.empty() ? 0.0 : bids.begin()->first;
    }

    double best_ask() const {
        return asks.empty() ? 0.0 : asks.begin()->first;
    }
};
