#include "zmq_market_subscriber.hpp"

using json = nlohmann::json;

ZmqMarketSubscriber::ZmqMarketSubscriber(std::string endpoint, std::string topic_filter)
    : endpoint_(std::move(endpoint)),
      topic_filter_(std::move(topic_filter)),
      ctx_(1),
      sub_(ctx_, zmq::socket_type::sub)
{
    // Helpful options (same as your current code)
    sub_.set(zmq::sockopt::rcvhwm, 100000);
    sub_.set(zmq::sockopt::linger, 0);

    sub_.connect(endpoint_);
    sub_.set(zmq::sockopt::subscribe, topic_filter_);
}

std::optional<MarketState> ZmqMarketSubscriber::recv_one(std::string* out_topic) {
    zmq::message_t topic_msg;
    zmq::message_t payload_msg;

    auto res = sub_.recv(topic_msg, zmq::recv_flags::none);
    if (!res.has_value()) return std::nullopt;

    std::string topic(static_cast<char*>(topic_msg.data()), topic_msg.size());

    bool more = sub_.get(zmq::sockopt::rcvmore);

    std::string payload;
    if (more) {
        if (!sub_.recv(payload_msg, zmq::recv_flags::none)) return std::nullopt;
        payload.assign(static_cast<char*>(payload_msg.data()), payload_msg.size());
    } else {
        // fallback: single-frame message
        payload = topic;
        topic.clear();
    }

    if (out_topic) *out_topic = topic;

    return parse_market_state_json(payload);
}

std::optional<MarketState> ZmqMarketSubscriber::parse_market_state_json(const std::string& payload) {
    try {
        json j = json::parse(payload);

        MarketState s;
        s.schema     = j.value("schema", "");
        s.exchange   = j.value("exchange", "");
        s.instrument = j.value("instrument", "");
        s.ts_ms      = j.value("ts_ms", static_cast<std::int64_t>(0));

        if (j.contains("top_of_book")) {
            const auto& tob = j["top_of_book"];
            s.bid    = tob.value("bid", 0.0);
            s.ask    = tob.value("ask", 0.0);
            s.mid    = tob.value("mid", 0.0);
            s.spread = tob.value("spread", 0.0);
        }

        if (j.contains("returns")) {
            const auto& r = j["returns"];
            s.r1  = r.value("r1", 0.0);
            s.r5  = r.value("r5", 0.0);
            s.r10 = r.value("r10", 0.0);
        }

        if (j.contains("depth")) {
            const auto& d = j["depth"];
            if (d.contains("bid_vol") && d["bid_vol"].is_array()) {
                for (size_t i = 0; i < 5 && i < d["bid_vol"].size(); ++i)
                    s.bid_vol[i] = d["bid_vol"][i].get<double>();
            }
            if (d.contains("ask_vol") && d["ask_vol"].is_array()) {
                for (size_t i = 0; i < 5 && i < d["ask_vol"].size(); ++i)
                    s.ask_vol[i] = d["ask_vol"][i].get<double>();
            }
        }

        // compute imbalance like you did
        double bid_sum = 0.0, ask_sum = 0.0;
        for (int i = 0; i < 5; ++i) {
            bid_sum += s.bid_vol[i];
            ask_sum += s.ask_vol[i];
        }
        s.imbalance = (bid_sum - ask_sum) / (bid_sum + ask_sum + 1e-9);

        // sanity
        if (s.exchange.empty() || s.instrument.empty()) return std::nullopt;

        return s;
    } catch (...) {
        return std::nullopt;
    }
}
