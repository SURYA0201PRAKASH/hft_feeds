#include "BybitL2Feed.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <chrono>

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
namespace ssl       = net::ssl;
using tcp           = net::ip::tcp;
using json          = nlohmann::json;

BybitL2Feed::BybitL2Feed(std::string instrument, int depth)
    : instrument_(std::move(instrument)), depth_(depth)
{}

static double j_to_double(const json& v) {
    if (v.is_number())  return v.get<double>();
    if (v.is_string())  return std::stod(v.get<std::string>());
    return 0.0;
}
static inline uint64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}
void BybitL2Feed::run() {
    try {
        const std::string host   = "stream.bybit.com";
        const std::string port   = "443";
        const std::string target = "/v5/public/spot";

        net::io_context ioc;

        ssl::context ctx(ssl::context::tlsv12_client);
        ctx.set_default_verify_paths();
        ctx.set_verify_mode(ssl::verify_peer);

        tcp::resolver resolver(ioc);
        auto const results = resolver.resolve(host, port);

        // --- same transport chain as your working code ---
        beast::ssl_stream<beast::tcp_stream> tls_stream(ioc, ctx);
        beast::get_lowest_layer(tls_stream).connect(results);
        SSL_set_tlsext_host_name(tls_stream.native_handle(), host.c_str());
        tls_stream.handshake(ssl::stream_base::client);

        websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(std::move(tls_stream));
        ws.handshake(host, target);

        std::cout << "[BYBIT ] Connected websocket\n";

        // Subscribe to orderbook + ticker for this instrument
		int sub_depth = 50;
		if (depth_ <= 1)        sub_depth = 1;
		else if (depth_ <= 50)  sub_depth = 50;
		else if (depth_ <= 200) sub_depth = 200;
		else                    sub_depth = 1000;
        json sub = {
            {"op","subscribe"},
            {"args", json::array({
                "orderbook." + std::to_string(sub_depth) + "." + instrument_,
                "tickers."      + instrument_
            })}
        };

        ws.write(net::buffer(sub.dump()));

        OrderBook ob;          // from your project
        double   spot = 0.0;   // lastPrice from ticker

        beast::flat_buffer buffer;

        while (true) {
            buffer.consume(buffer.size());
            ws.read(buffer);

            std::string text = beast::buffers_to_string(buffer.data());

            json msg;
            try {
                msg = json::parse(text);
            } catch (...) {
                continue;
            }

            if (!msg.contains("topic"))
                continue;

            std::string topic = msg["topic"].get<std::string>();

            // ------------------ 1) Ticker (L1 + spot) ------------------
            if (topic == "tickers." + instrument_) {
                if (!msg.contains("data")) continue;
                const auto& data = msg["data"];

                json t;
                if (data.is_array() && !data.empty())
                    t = data[0];
                else if (data.is_object())
                    t = data;
                else
                    continue;

                if (t.contains("lastPrice"))
                    spot = j_to_double(t["lastPrice"]);

                double bid = ob.best_bid();
                double ask = ob.best_ask();

                if (t.contains("bid1Price"))
                    bid = j_to_double(t["bid1Price"]);
                if (t.contains("ask1Price"))
                    ask = j_to_double(t["ask1Price"]);

                if (on_quote) {
                    Quote q;
                    q.exchange   = "bybit";
                    q.instrument = instrument_;
                    q.bid        = bid;
                    q.ask        = ask;
                    q.spot       = spot;

                    auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now());
                    q.ts_ms = now.time_since_epoch().count();

                    on_quote(q,ob);
                }

                continue;
            }

            // ------------------ 2) Orderbook (snapshot/delta) ---------
            if (topic == "orderbook.50." + instrument_) {
                if (!msg.contains("data")) continue;
                const json& data = msg["data"];

                // data may be object OR array[0] depending on Bybit
                const json* lob = nullptr;
                if (data.is_object()) {
                    lob = &data;
                } else if (data.is_array() && !data.empty()) {
                    lob = &data[0];
                } else {
                    continue;
                }

                const json& d = *lob;

                std::string type = msg.value("type", "snapshot");

                std::vector<std::pair<double,double>> bid_lvls;
                std::vector<std::pair<double,double>> ask_lvls;

                if (d.contains("b") && d["b"].is_array()) {
                    for (const auto& lvl : d["b"]) {
                        if (!lvl.is_array() || lvl.size() < 2) continue;
                        double px  = j_to_double(lvl[0]);
                        double qty = j_to_double(lvl[1]);
                        bid_lvls.emplace_back(px, qty);
                    }
                }

                if (d.contains("a") && d["a"].is_array()) {
                    for (const auto& lvl : d["a"]) {
                        if (!lvl.is_array() || lvl.size() < 2) continue;
                        double px  = j_to_double(lvl[0]);
                        double qty = j_to_double(lvl[1]);
                        ask_lvls.emplace_back(px, qty);
                    }
                }

                if (type == "snapshot")
                    ob.apply_snapshot(bid_lvls, ask_lvls);
                else
                    ob.apply_delta(bid_lvls, ask_lvls);

                // we rely on the ticker branch to emit quotes,
                // but we *could* emit here too if you want.
                continue;
            }
        }

    } catch (const std::exception& ex) {
        std::cerr << "[BybitL2Feed] Error (" << instrument_ << "): "
                  << ex.what() << std::endl;
    }
}
