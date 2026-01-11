#include "MarketDataManager.hpp"
#include <iostream>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <chrono>

// NOTE: unchanged serializer (we still use Quote + OrderBook + StateSnapshot)
static std::string serialize_snapshot(
    const std::string& ex,
    const Quote& q,
    const OrderBook& ob,
    const StateSnapshot& s)
{
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss << std::setprecision(8);

    oss << "{";
    oss << "\"schema\":\"market_state_v1\",";
    oss << "\"exchange\":\"" << ex << "\",";
    oss << "\"instrument\":\"" << q.instrument << "\",";
    oss << "\"ts_ms\":" << q.ts_ms << ",";

    oss << "\"book_meta\":{";
    oss << "\"bid_levels\":" << ob.bids.size() << ",";
    oss << "\"ask_levels\":" << ob.asks.size();
    oss << "},";

    oss << "\"top_of_book\":{";
    oss << "\"bid\":" << q.bid << ",";
    oss << "\"ask\":" << q.ask << ",";
    oss << "\"mid\":" << s.mid << ",";
    oss << "\"spread\":" << s.spread;
    oss << "},";

    oss << "\"returns\":{";
    oss << "\"r1\":" << s.r1 << ",";
    oss << "\"r5\":" << s.r5 << ",";
    oss << "\"r10\":" << s.r10;
    oss << "},";

    oss << "\"depth\":{";
    oss << "\"bid_vol\":["
        << s.bid_vol[0] << "," << s.bid_vol[1] << ","
        << s.bid_vol[2] << "," << s.bid_vol[3] << ","
        << s.bid_vol[4] << "],";
    oss << "\"ask_vol\":["
        << s.ask_vol[0] << "," << s.ask_vol[1] << ","
        << s.ask_vol[2] << "," << s.ask_vol[3] << ","
        << s.ask_vol[4] << "]";
    oss << "},";

    oss << "\"features\":{";
    //SuPr moving it to strategy oss << "\"imbalance\":" << s.imbalance;
    oss << "}";

    oss << "}";

    return oss.str();
}

MarketDataManager::MarketDataManager(
    ExchangeChoice choice,
    const std::vector<std::string>& instruments,
    int orderBookDepth,
    int orderBookPollFrequencyInMs)
    : order_book_depth_(orderBookDepth),
      snapshot_freq_ms_(orderBookPollFrequencyInMs)
{
    zmq_pub_ = std::make_unique<ZmqPublisher>("tcp://*:5555");

    for (const auto& ins : instruments) {

        // ================= BINANCE =================
        if (choice == ExchangeChoice::Binance || choice == ExchangeChoice::Both) {
            auto f = std::make_unique<BinanceL2Feed>(ins, order_book_depth_);

            // ✅ state-only update (NO publish, NO DB push, NO cout)
            f->on_quote = [this](const Quote& q, const OrderBook& ob) {
                const std::string ex = "binance";
                MarketKey key{ex, q.instrument};

                std::lock_guard<std::mutex> lock(state_mtx_);

                // keep latest quote + book for snapshot thread
                last_quote_[key] = q;
                last_ob_[key]    = ob;

                // ---- MID & SPREAD ----
                double mid    = 0.5 * (q.bid + q.ask);
                double spread = q.ask - q.bid;

                auto& state = state_[key];
                auto& hist  = price_history_[key];

                state.mid    = mid;
                state.spread = spread;

                // ---- PRICE HISTORY ----
                hist.emplace_back(q.ts_ms, mid);
                while (!hist.empty() && q.ts_ms - hist.front().first > 15000) {
                    hist.pop_front();
                }

                // ---- RETURNS ----
                state.r1 = state.r5 = state.r10 = 0.0;

                for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
                    double dt = (q.ts_ms - it->first) / 1000.0;
                    if (dt >= 1.0  && state.r1  == 0.0) state.r1  = std::log(mid / it->second);
                    if (dt >= 5.0  && state.r5  == 0.0) state.r5  = std::log(mid / it->second);
                    if (dt >= 10.0 && state.r10 == 0.0) state.r10 = std::log(mid / it->second);
                }

                // ---- TOP-5 BID/ASK VOLUMES ----
                int i = 0;
                for (const auto& [px, qty] : ob.bids) {
                    if (i >= 5) break;
                    state.bid_vol[i++] = qty;
                }
                for (; i < 5; ++i) state.bid_vol[i] = 0.0;

                i = 0;
                for (const auto& [px, qty] : ob.asks) {
                    if (i >= 5) break;
                    state.ask_vol[i++] = qty;
                }
                for (; i < 5; ++i) state.ask_vol[i] = 0.0;

                // ---- IMBALANCE ----
                double bid_sum = 0.0, ask_sum = 0.0;
                for (int k = 0; k < 5; ++k) {
                    bid_sum += state.bid_vol[k];
                    ask_sum += state.ask_vol[k];
                }
                const double eps = 1e-9;
                //SuPr moving it to strategy state.imbalance = (bid_sum - ask_sum) / (bid_sum + ask_sum + eps);

                state.cross_ex_signal = 0.0;
            };

            feeds_.push_back(std::move(f));
        }

        // ================= BYBIT =================
        if (choice == ExchangeChoice::Bybit || choice == ExchangeChoice::Both) {
            auto f = std::make_unique<BybitL2Feed>(ins, order_book_depth_);

            // ✅ state-only update (NO publish, NO DB push, NO cout)
            f->on_quote = [this](const Quote& q, const OrderBook& ob) {
                const std::string ex = "bybit";
                MarketKey key{ex, q.instrument};

                std::lock_guard<std::mutex> lock(state_mtx_);

                last_quote_[key] = q;
                last_ob_[key]    = ob;

                double mid    = 0.5 * (q.bid + q.ask);
                double spread = q.ask - q.bid;

                auto& state = state_[key];
                auto& hist  = price_history_[key];

                state.mid    = mid;
                state.spread = spread;

                hist.emplace_back(q.ts_ms, mid);
                while (!hist.empty() && q.ts_ms - hist.front().first > 15000) {
                    hist.pop_front();
                }

                state.r1 = state.r5 = state.r10 = 0.0;

                for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
                    double dt = (q.ts_ms - it->first) / 1000.0;
                    if (dt >= 1.0  && state.r1  == 0.0) state.r1  = std::log(mid / it->second);
                    if (dt >= 5.0  && state.r5  == 0.0) state.r5  = std::log(mid / it->second);
                    if (dt >= 10.0 && state.r10 == 0.0) state.r10 = std::log(mid / it->second);
                }

                int i = 0;
                for (const auto& [px, qty] : ob.bids) {
                    if (i >= 5) break;
                    state.bid_vol[i++] = qty;
                }
                for (; i < 5; ++i) state.bid_vol[i] = 0.0;

                i = 0;
                for (const auto& [px, qty] : ob.asks) {
                    if (i >= 5) break;
                    state.ask_vol[i++] = qty;
                }
                for (; i < 5; ++i) state.ask_vol[i] = 0.0;

                double bid_sum = 0.0, ask_sum = 0.0;
                for (int k = 0; k < 5; ++k) {
                    bid_sum += state.bid_vol[k];
                    ask_sum += state.ask_vol[k];
                }
                const double eps = 1e-9;
                //SuPr moving it to strategy state.imbalance = (bid_sum - ask_sum) / (bid_sum + ask_sum + eps);

                state.cross_ex_signal = 0.0;
            };

            feeds_.push_back(std::move(f));
        }
    }
}

void MarketDataManager::start_all() {
    state_db_.start();

    // start feed threads
    threads_.reserve(feeds_.size());
    for (auto& feed : feeds_) {
        threads_.emplace_back([ptr = feed.get()](){
            ptr->run();
        });
    }

    // ✅ start snapshot sampler thread
    running_ = true;
    snapshot_thread_ = std::thread([this](){
        snapshot_loop();
    });
}

void MarketDataManager::join_all() {
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }

    // stop snapshot thread
    running_ = false;
    if (snapshot_thread_.joinable())
        snapshot_thread_.join();

    state_db_.stop();
}

void MarketDataManager::snapshot_loop() {
    using namespace std::chrono;

    const auto interval = milliseconds(snapshot_freq_ms_);

    while (running_) {
        auto t0 = steady_clock::now();

        // Copy under lock (keep lock short)
        std::vector<std::pair<MarketKey, StateVector>> states_copy;
        std::vector<std::pair<MarketKey, Quote>> quotes_copy;
        std::vector<std::pair<MarketKey, OrderBook>> obs_copy;

        {
            std::lock_guard<std::mutex> lock(state_mtx_);

            states_copy.reserve(state_.size());
            for (const auto& kv : state_) states_copy.push_back(kv);

            quotes_copy.reserve(last_quote_.size());
            for (const auto& kv : last_quote_) quotes_copy.push_back(kv);

            obs_copy.reserve(last_ob_.size());
            for (const auto& kv : last_ob_) obs_copy.push_back(kv);
        }

        // Build lookup maps (local)
        std::unordered_map<MarketKey, Quote, MarketKeyHash> qmap;
        std::unordered_map<MarketKey, OrderBook, MarketKeyHash> obmap;

        for (auto& kv : quotes_copy) qmap.emplace(kv.first, std::move(kv.second));
        for (auto& kv : obs_copy)    obmap.emplace(kv.first, std::move(kv.second));

        // Emit snapshots on fixed clock
        for (const auto& [key, st] : states_copy) {
            auto qit  = qmap.find(key);
            auto obit = obmap.find(key);
            if (qit == qmap.end() || obit == obmap.end())
                continue;

            Quote q = qit->second;          // copy
            const OrderBook& ob = obit->second;

            // overwrite ts to sampling time (this is what you want)
            q.ts_ms = duration_cast<milliseconds>(
                system_clock::now().time_since_epoch()
            ).count();

            StateSnapshot snap;
            snap.exchange   = key.exchange;
            snap.instrument = key.instrument;
            snap.ts_ms      = q.ts_ms;

            snap.mid    = st.mid;
            snap.spread = st.spread;
            snap.r1     = st.r1;
            snap.r5     = st.r5;
            snap.r10    = st.r10;

            //SuPr moving it to strategy snap.imbalance       = st.imbalance;
            snap.cross_ex_signal = st.cross_ex_signal;

            for (int i = 0; i < 5; ++i) {
                snap.bid_vol[i] = st.bid_vol[i];
                snap.ask_vol[i] = st.ask_vol[i];
            }

            const std::string topic = "state." + key.exchange + "." + key.instrument;
            const std::string payload = serialize_snapshot(key.exchange, q, ob, snap);

            {
                std::lock_guard<std::mutex> lock(zmq_pub_mtx_);
                zmq_pub_->publish(topic, payload);
            }

            state_db_.push(std::move(snap));
        }

        std::this_thread::sleep_until(t0 + interval);
    }
}
