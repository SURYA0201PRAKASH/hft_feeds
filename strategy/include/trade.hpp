#pragma once
#include <cstdint>
#include <string>
#include "order_intent.hpp"

struct Trade {
    std::string key;
    Side side;
    double price;
    double qty;
    std::int64_t ts_ms;
    double pos_after;
};
