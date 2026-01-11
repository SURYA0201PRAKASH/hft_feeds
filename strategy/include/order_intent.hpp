#pragma once
#include <cstdint>
#include <string>

enum class Side { Buy, Sell };

struct OrderIntent {
    std::string key;       // exchange|symbol
    Side side;
    double price;          // limit price
    double qty;            // base qty (ETH)
    std::int64_t ts_ms;
};
