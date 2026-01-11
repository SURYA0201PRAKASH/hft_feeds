#pragma once
#include "IFeed.hpp"
#include "OrderBook.hpp"
#include <string>

class BinanceL2Feed : public IFeed {
public:
    explicit BinanceL2Feed(std::string instrument, int depth);

    void run() override;

private:
    std::string instrument_;    // e.g. "ETHUSDT"
	int depth_ = 20;
};
