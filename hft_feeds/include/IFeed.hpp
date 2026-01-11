#pragma once
#include "Quote.hpp"
#include <functional>
#include <string>

class OrderBook;

class IFeed {
public:
    virtual ~IFeed() = default;

    // Blocking loop: connect WS, maintain orderbook, emit L1 quotes
    virtual void run() = 0;

    // Called whenever we have a new L1 quote
    std::function<void(const Quote&, const OrderBook&)> on_quote;
};
