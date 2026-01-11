#pragma once
#include "IFeed.hpp"
#include "OrderBook.hpp"
#include <string>

class BybitL2Feed : public IFeed {
public:
    explicit BybitL2Feed(std::string instrument, int depth);

    void run() override;

private:
    std::string instrument_;   // e.g. "ETHUSDT"
	int depth_ = 20;
};
