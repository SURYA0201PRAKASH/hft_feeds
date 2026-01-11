#pragma once
#include <string>

class BybitDemoClient {
public:
    BybitDemoClient(std::string api_key, std::string api_secret);

    bool ready() const;

    // Returns response JSON string
    std::string place_market_order(
        const std::string& category,  // "spot" or "linear"
        const std::string& symbol,    // e.g., "ETHUSDC"
        const std::string& side,      // "Buy" or "Sell"
        double qty
    );
	std::string place_limit_order(
    const std::string& category,
    const std::string& symbol,
    const std::string& side,
    double qty,
    double price,
    const std::string& tif = "GTC"
);
private:
    std::string api_key_;
    std::string api_secret_;

    std::string post(const std::string& path, const std::string& body_json);
};
