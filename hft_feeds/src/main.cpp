#include "MarketDataManager.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <nlohmann/json.hpp>
using json = nlohmann::json;


static std::vector<std::string> parse_instruments(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        auto pos = line.find(',', start);
        if (pos == std::string::npos) {
            auto token = line.substr(start);
            if (!token.empty()) out.push_back(token);
            break;
        }
        auto token = line.substr(start, pos - start);
        if (!token.empty()) out.push_back(token);
        start = pos + 1;
    }
    return out;
}

static ExchangeChoice parse_exchange(const std::string& ex) {
    if (ex == "binance") return ExchangeChoice::Binance;
    if (ex == "bybit")  return ExchangeChoice::Bybit;
    return ExchangeChoice::Both;
}

static std::string to_lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static ExchangeChoice parse_exchanges_array_or_fallback(const json& j) {
    // New format: "exchanges": [...]
    if (j.contains("exchanges") && j["exchanges"].is_array()) {
        bool has_binance = false;
        bool has_bybit   = false;

        for (const auto& v : j["exchanges"]) {
            if (!v.is_string()) continue;
            auto ex = to_lower(v.get<std::string>());

            if (ex == "binance") has_binance = true;
            if (ex == "bybit")   has_bybit   = true;
        }

        if (has_binance && has_bybit) return ExchangeChoice::Both;
        if (has_binance)             return ExchangeChoice::Binance;
        if (has_bybit)               return ExchangeChoice::Bybit;

        // If array exists but had no valid values, fall back
    }

    // Old format fallback: "exchange": "both/binance/bybit"
    std::string exchange_str = to_lower(j.value("exchange", "both"));
    if (exchange_str == "binance") return ExchangeChoice::Binance;
    if (exchange_str == "bybit")   return ExchangeChoice::Bybit;
    return ExchangeChoice::Both;
}

int main() {
    /*std::cout << "Select exchange:\n"
              << "1. Binance\n"
              << "2. Bybit\n"
              << "3. Both\n"
              << "> ";

    int choice = 0;
    std::cin >> choice;

    ExchangeChoice sel;
    if (choice == 1) sel = ExchangeChoice::Binance;
    else if (choice == 2) sel = ExchangeChoice::Bybit;
    else sel = ExchangeChoice::Both;

    std::cout << "Enter instruments (comma separated, e.g. ETHUSDT,BTCUSDT):\n> ";
    std::string ins_line;
    std::cin >> ins_line;

    auto instruments = parse_instruments(ins_line);*/
	    // ---------- Open config file ----------
    std::ifstream cfg("../config.json");
    if (!cfg.is_open()) {
        std::cerr << "Failed to open config.json\n";
        return 1;
    }

    // ---------- Parse JSON ----------
    json j;
    cfg >> j;
	int orderbook_depth = j.value("orderBookDepth", 20);
	int orderbook_poll_ms = j.value("orderBookPollFrequencyInMs", 50);
    // ---------- Read exchange ----------
    ExchangeChoice sel = parse_exchanges_array_or_fallback(j);

    // ---------- Read instruments ----------
    std::vector<std::string> instruments;
    for (const auto& ins : j["instruments"]) {
        instruments.push_back(ins.get<std::string>());
    }
	
	// ---------- Start market data ----------
    MarketDataManager mgr(sel, instruments, orderbook_depth, orderbook_poll_ms);
    mgr.start_all();
    mgr.join_all();

    return 0;
}
