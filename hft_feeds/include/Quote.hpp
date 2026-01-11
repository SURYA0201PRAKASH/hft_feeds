#pragma once
#include <string>

struct Quote {
    std::string exchange;    // "binance" or "bybit"
    std::string instrument;  // "ETHUSDT"
    double bid = 0.0;        // best bid
    double ask = 0.0;        // best ask
    double spot = 0.0;       // last traded price / spot
    long long ts_ms = 0;     // local timestamp in ms
};
