#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <chrono>

#include <zmq.hpp>
#include <nlohmann/json.hpp>

#include "bybit_demo_client.hpp"

static std::string env_or(const char* k, const std::string& defv) {
    const char* v = std::getenv(k);
    return v ? std::string(v) : defv;
}

// Latest cached top-of-book (from ZMQ)
static std::mutex g_px_mtx;
static double g_last_bid = 0.0;
static double g_last_ask = 0.0;

static double get_num_safe(const nlohmann::json& v) {
    try {
        if (v.is_number_float() || v.is_number_integer() || v.is_number_unsigned())
            return v.get<double>();
        if (v.is_string())
            return std::stod(v.get<std::string>());
        // null / object / array
        return 0.0;
    } catch (...) {
        return 0.0;
    }
}

int main() {
    // ZMQ params
    std::string addr  = env_or("ZMQ_ADDR",  "tcp://127.0.0.1:5555");
    std::string topic = env_or("ZMQ_TOPIC", ""); // "" = subscribe all

    zmq::context_t ctx(1);
    zmq::socket_t sub(ctx, zmq::socket_type::sub);
    sub.connect(addr);
    sub.set(zmq::sockopt::subscribe, topic);

    std::cout << "[place_order] SUB connected to " << addr
              << " topic='" << topic << "'\n";

    // Bybit demo client (keys from env)
    BybitDemoClient bybit(
        env_or("BYBIT_API_KEY", ""),
        env_or("BYBIT_API_SECRET", "")
    );

    if (!bybit.ready()) {
        std::cout << "[place_order] NOTE: set BYBIT_API_KEY and BYBIT_API_SECRET to enable orders.\n";
    }

    std::atomic<bool> running{true};

    // Thread: ZMQ receive (cache bid/ask from JSON)
    std::thread rx([&](){
        int printed = 0;      // debug: print first 3 msgs
        int cache_printed = 0;

        while (running.load()) {
            zmq::message_t part1;
            if (!sub.recv(part1, zmq::recv_flags::none)) continue;

            std::string payload;

            // Handle both: [payload] OR [topic][payload]
            if (part1.more()) {
                zmq::message_t part2;
                if (!sub.recv(part2, zmq::recv_flags::none)) continue;

                std::string topic_s(static_cast<char*>(part1.data()), part1.size());
                payload = std::string(static_cast<char*>(part2.data()), part2.size());
            } else {
                payload = std::string(static_cast<char*>(part1.data()), part1.size());
            }

            try {
                auto j = nlohmann::json::parse(payload);

// Print keys once so we know what we actually parsed
static int k = 0;
if (k < 2) {
    std::cout << "[KEYS] top-level: ";
    for (auto it = j.begin(); it != j.end(); ++it) std::cout << it.key() << " ";
    std::cout << "\n";
    k++;
}

// Direct extraction (no continues hiding the problem)
auto& tob = j.at("top_of_book");

double bid = get_num_safe(tob.at("bid"));
double ask = get_num_safe(tob.at("ask"));

//std::cout << "[EXTRACT] bid=" << bid << " ask=" << ask << "\n";

{
    std::lock_guard<std::mutex> lk(g_px_mtx);
    g_last_bid = bid;
    g_last_ask = ask;
}


            } catch (const std::exception& e) {
                std::cout << "[ERR] " << e.what() << "\n";
            }
        }
    });

    // Main thread: user commands
    std::cout << "\nCommands:\n"
              << "  px\n"
              << "  buy  <category> <symbol> <qty>\n"
              << "  sell <category> <symbol> <qty>\n"
              << "  quit\n\n";

    std::string cmd;
    while (std::cin >> cmd) {
        if (cmd == "quit") break;

        if (cmd == "px") {
    double bid=0.0, ask=0.0;
    {
        std::lock_guard<std::mutex> lk(g_px_mtx);
        bid = g_last_bid;
        ask = g_last_ask;
        std::cout << "[PXREAD] g_last_bid=" << g_last_bid
                  << " g_last_ask=" << g_last_ask
                  << " (addr bid=" << (void*)&g_last_bid
                  << " ask=" << (void*)&g_last_ask << ")\n";
    }
    std::cout << "bid=" << bid << " ask=" << ask << "\n";
    continue;
}

        std::string category, symbol;
        double qty = 0.0;
        std::cin >> category >> symbol >> qty;

        if (!bybit.ready()) {
            std::cout << "Set BYBIT_API_KEY and BYBIT_API_SECRET first.\n";
            continue;
        }

        double bid=0.0, ask=0.0;
        { std::lock_guard<std::mutex> lk(g_px_mtx); bid=g_last_bid; ask=g_last_ask; }

        if (bid <= 0.0 || ask <= 0.0) {
            std::cout << "No bid/ask yet. Wait for ZMQ ticks then run: px\n";
            continue;
        }

        if (cmd == "buy") {
            // LIMIT Buy @ best ask
            std::cout << bybit.place_limit_order(category, symbol, "Buy", qty, ask) << "\n";
        } else if (cmd == "sell") {
            // LIMIT Sell @ best bid
            std::cout << bybit.place_limit_order(category, symbol, "Sell", qty, bid) << "\n";
        } else {
            std::cout << "Unknown command.\n";
        }
    }

    running = false;
    try { sub.close(); } catch (...) {}
    if (rx.joinable()) rx.join();
    return 0;
}
