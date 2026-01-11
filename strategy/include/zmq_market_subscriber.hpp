#pragma once

#include <optional>
#include <string>

#include <zmq.hpp>
#include <nlohmann/json.hpp>

#include "imbalance_taker.hpp" // for MarketState (you already defined it there)

// This module ONLY does: connect -> subscribe -> recv -> parse -> return MarketState
class ZmqMarketSubscriber {
public:
    ZmqMarketSubscriber(std::string endpoint, std::string topic_filter);

    // Blocking receive:
    // Returns MarketState when a valid JSON payload arrives.
    // Returns std::nullopt if message is malformed (keeps running).
    std::optional<MarketState> recv_one(std::string* out_topic = nullptr);

private:
    std::optional<MarketState> parse_market_state_json(const std::string& payload);

private:
    std::string endpoint_;
    std::string topic_filter_;

    zmq::context_t ctx_;
    zmq::socket_t  sub_;
};
