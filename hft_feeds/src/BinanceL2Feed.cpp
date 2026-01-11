#include "BinanceL2Feed.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>          // <-- needed for beast::ssl_stream
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ssl.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <chrono>
#include <map>
#include <cctype>

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
namespace ssl       = net::ssl;
using tcp           = net::ip::tcp;
using json          = nlohmann::json;

BinanceL2Feed::BinanceL2Feed(std::string instrument, int depth)
    : instrument_(std::move(instrument)), depth_(depth)
{}

// lower-case helper
static std::string to_lower_copy(const std::string& s) {
    std::string out = s;
    for (auto& c : out) c = static_cast<char>(std::tolower(c));
    return out;
}

void BinanceL2Feed::run() {
    try {
        // ------------------------------------------------------------
        // IO + SSL context
        // ------------------------------------------------------------
        net::io_context ioc;
        ssl::context ctx(ssl::context::tlsv12_client);
        ctx.set_default_verify_paths();
        ctx.set_verify_mode(ssl::verify_peer);

        const std::string host = "stream.binance.com";
        const std::string port = "9443";
        const std::string path = "/ws";

        tcp::resolver resolver(ioc);
        auto const results = resolver.resolve(host, port);

        // ------------------------------------------------------------
        // TLS stack: beast::ssl_stream<beast::tcp_stream>
        // (same as your working standalone code)
        // ------------------------------------------------------------
        beast::ssl_stream<beast::tcp_stream> tls_stream(ioc, ctx);
        beast::get_lowest_layer(tls_stream).connect(results);
        SSL_set_tlsext_host_name(tls_stream.native_handle(), host.c_str());
        tls_stream.handshake(ssl::stream_base::client);

        websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(
            std::move(tls_stream));
        ws.handshake(host, path);

        // ------------------------------------------------------------
        // Subscribe: ticker + depth20 + bookTicker
        // (instrument_ but lower-cased, like your sample "ethusdt")
        // ------------------------------------------------------------
        std::string sym_lc = to_lower_copy(instrument_);
		int sub_depth = 20;
		if (depth_ <= 5)       sub_depth = 5;
		else if (depth_ <= 10) sub_depth = 10;
		else                   sub_depth = 20;
        json sub = {
            {"method", "SUBSCRIBE"},
            {"params", json::array({
                sym_lc + "@ticker",         // 24hrTicker (spot)
                sym_lc + "@depth" + std::to_string(sub_depth) + "@100ms",  // L2 updates
                sym_lc + "@bookTicker"      // best bid / ask
            })},
            {"id", 1}
        };

        ws.write(net::buffer(sub.dump()));

        beast::flat_buffer buffer;
        OrderBook ob;          // your shared orderbook class
        double spot     = 0.0; // last traded price from 24hrTicker
        double best_bid = 0.0; // from bookTicker
        double best_ask = 0.0; // from bookTicker

        // ------------------------------------------------------------
        // Main read loop – mirrors your working example
        // ------------------------------------------------------------
        while (true) {
            buffer.consume(buffer.size());
            ws.read(buffer);

            std::string text = beast::buffers_to_string(buffer.data());

            json msg = json::parse(text, nullptr, false);
            if (msg.is_discarded())
                continue;
            // --------------------------------------------------------
            // 1) 24hrTicker event: real spot price
            //    { "e": "24hrTicker", "c": "3163.25", ... }
            // --------------------------------------------------------
            if (msg.contains("e") && msg["e"] == "24hrTicker") {
                if (msg.contains("c")) {
                    // "c" is a string for spot
                    spot = std::stod(msg["c"].get<std::string>());
                }
                // don't 'continue'; we still want to emit a Quote below
            }

            // --------------------------------------------------------
            // 2) bookTicker: best bid / ask
            //    { "s": "ETHUSDT", "b": "3163.10", "a": "3163.20", ... }
            // depthUpdate ALSO has "b" and "a", but those are arrays AND
            // always come with "e": "depthUpdate", so we exclude those.
            // --------------------------------------------------------
            if (!msg.contains("e") && msg.contains("b") && msg.contains("a")) {
                try {
                    best_bid = std::stod(msg["b"].get<std::string>());
                    best_ask = std::stod(msg["a"].get<std::string>());
                } catch (...) {
                    // ignore malformed frames
                }
            }

            // --------------------------------------------------------
            // 3) depthUpdate: optional L2 maintenance via your OrderBook
            //    { "e": "depthUpdate", "b": [...], "a": [...] }
            // --------------------------------------------------------
            if (msg.contains("e") && msg["e"] == "depthUpdate") {
                std::vector<std::pair<double,double>> bid_lvls;
                std::vector<std::pair<double,double>> ask_lvls;

                if (msg.contains("b") && msg["b"].is_array()) {
                    for (const auto& lvl : msg["b"]) {
                        if (lvl.size() >= 2) {
                            double px  = std::stod(lvl[0].get<std::string>());
                            double qty = std::stod(lvl[1].get<std::string>());
                            bid_lvls.emplace_back(px, qty);
                        }
                    }
                }

                if (msg.contains("a") && msg["a"].is_array()) {
                    for (const auto& lvl : msg["a"]) {
                        if (lvl.size() >= 2) {
                            double px  = std::stod(lvl[0].get<std::string>());
                            double qty = std::stod(lvl[1].get<std::string>());
                            ask_lvls.emplace_back(px, qty);
                        }
                    }
                }

                // Treat each depth20 as a fresh snapshot of top 20
                ob.apply_snapshot(bid_lvls, ask_lvls);
            }
			
			// --------------------------------------------------------
			// Binance depth20@100ms SNAPSHOT
			// Format:
			// {
			//   "lastUpdateId": 123,
			//   "bids": [[px, qty], ...],
			//   "asks": [[px, qty], ...]
			// }
			// --------------------------------------------------------
			if (msg.contains("lastUpdateId") &&
			    msg.contains("bids") && msg["bids"].is_array() &&
			    msg.contains("asks") && msg["asks"].is_array())
			{
			    std::cout << "[BINANCE depth20 snapshot]" << std::endl;

			    std::vector<std::pair<double,double>> bid_lvls;
			    std::vector<std::pair<double,double>> ask_lvls;

 			   for (const auto& lvl : msg["bids"]) {
			        if (lvl.size() >= 2) {
			            double px  = std::stod(lvl[0].get<std::string>());
			            double qty = std::stod(lvl[1].get<std::string>());
 			           bid_lvls.emplace_back(px, qty);
 			       }
			    }

 			   for (const auto& lvl : msg["asks"]) {
			        if (lvl.size() >= 2) {
			            double px  = std::stod(lvl[0].get<std::string>());
			            double qty = std::stod(lvl[1].get<std::string>());
			            ask_lvls.emplace_back(px, qty);
			        }
			    }

			    // FULL rebuild (snapshot semantics)
 			   ob.apply_snapshot(bid_lvls, ask_lvls);
			}

            // --------------------------------------------------------
            // Emit Quote on every message using the latest values
            // (spot from 24hrTicker, bid/ask from bookTicker)
            // --------------------------------------------------------
            if (on_quote) {
                Quote q;
                q.exchange   = "binance";
                q.instrument = instrument_;
                q.bid        = best_bid;  // from bookTicker
                q.ask        = best_ask;  // from bookTicker
                q.spot       = spot;

                auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now());
                q.ts_ms = now.time_since_epoch().count();

                on_quote(q,ob);
            }
        }

    } catch (const std::exception& ex) {
        std::cerr << "[BinanceL2Feed] Error (" << instrument_ << "): "
                  << ex.what() << std::endl;
    }
}


