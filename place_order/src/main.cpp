// main.cpp (FULL)
// Includes fixes:
// 1) pnlFULL no longer crashes on std::stod("") (uses get_num_safe everywhere)
// 2) Trade FIFO realized PnL allocates BOTH open + close fees (Lot::fee_rem)
// 3) pnlFULL compiles (realAllJ/realJ defined)

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <chrono>
#include <unordered_map>
#include <csignal>
#include <vector>
#include <fstream>
#include <unordered_set>
#include <algorithm>
#include <cmath>

#include <zmq.hpp>
#include <nlohmann/json.hpp>

#include "bybit_demo_client.hpp"
#include "imb_momo_strategy.hpp"
// ---- Logging (avoid interleaved prints from multiple threads) ----
static std::mutex g_log_mtx;

static const char* LEDGER_PATH       = "executions_ledger.jsonl";
static const char* FUND_LEDGER_PATH  = "funding_ledger.jsonl";
static const char* TRADE_LEDGER_PATH = "trades_ledger.jsonl";

static std::mutex g_ledger_mtx;
static std::atomic<bool> g_auto{false};
static std::mutex g_exec_mtx;   // protect bybit API calls inside rx thread
static std::atomic<bool> g_print_sig{true};

static double get_num_safe(const nlohmann::json& v);

// ------------------------ Trade ledger (realized PnL) ------------------------
static std::unordered_set<std::string> g_seen_trade_ids;

static std::string dbl_to_str(double x) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.10f", x);
    return std::string(buf);
}
enum class OrdType { MKT, LMT };

static OrdType parse_ordtype_or_default_mkt(const std::string& s) {
    if (s == "lmt" || s == "LMT" || s == "limit" || s == "LIMIT") return OrdType::LMT;
    return OrdType::MKT; // default
}

static std::string make_trade_id(const std::string& close_execId,
                                 const std::string& open_execId,
                                 const std::string& symbol,
                                 double qty,
                                 double open_px,
                                 double close_px,
                                 long long ts_ms) {
    return close_execId + "|" + open_execId + "|" + symbol + "|" +
           dbl_to_str(qty) + "|" + dbl_to_str(open_px) + "|" + dbl_to_str(close_px) + "|" +
           std::to_string(ts_ms);
}

static void load_seen_trade_ids() {
    std::ifstream in(TRADE_LEDGER_PATH);
    if (!in.good()) return;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            auto j = nlohmann::json::parse(line);
            if (j.contains("tradeId") && j["tradeId"].is_string()) {
                g_seen_trade_ids.insert(j["tradeId"].get<std::string>());
            }
        } catch (...) {}
    }
}

static bool extract_ts_ms_any(const nlohmann::json& j, long long& ts_out) {
    if (!j.contains("ts_ms")) return false;
    try {
        if (j["ts_ms"].is_number_integer()) { ts_out = j["ts_ms"].get<long long>(); return true; }
        if (j["ts_ms"].is_number())         { ts_out = (long long)j["ts_ms"].get<double>(); return true; }
        if (j["ts_ms"].is_string())         { ts_out = std::stoll(j["ts_ms"].get<std::string>()); return true; }
    } catch (...) {}
    return false;
}

static double get_num_field(const nlohmann::json& j, const char* key) {
    if (!j.contains(key)) return 0.0;
    return get_num_safe(j.at(key));
}

enum class LotSide { LONG, SHORT };

struct Lot {
    LotSide side;
    double qty;          // remaining qty
    double px;           // entry price
    std::string execId;  // opening execId
    long long ts_ms;     // opening time
    double fee_rem;      // remaining OPEN fee to allocate when this lot is closed
};

static std::string lot_side_str(LotSide s) {
    return (s == LotSide::LONG) ? "LONG" : "SHORT";
}

static bool append_trade_event(const nlohmann::json& ev) {
    if (!ev.contains("tradeId") || !ev["tradeId"].is_string()) return false;
    const std::string tid = ev["tradeId"].get<std::string>();
    if (tid.empty()) return false;

    std::lock_guard<std::mutex> lk(g_ledger_mtx);

    if (g_seen_trade_ids.find(tid) != g_seen_trade_ids.end())
        return false;

    std::ofstream f(TRADE_LEDGER_PATH, std::ios::app);
    if (!f.good()) return false;

    f << ev.dump() << "\n";
    g_seen_trade_ids.insert(tid);
    return true;
}

// Read executions ledger lines for a symbol+category, sorted by ts_ms then execId
static std::vector<nlohmann::json> load_execs_from_ledger(const std::string& category,
                                                         const std::string& symbol) {
    std::vector<nlohmann::json> out;
    std::ifstream in(LEDGER_PATH);
    if (!in.good()) return out;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            auto j = nlohmann::json::parse(line);
            if (!j.contains("symbol") || !j["symbol"].is_string()) continue;
            if (j["symbol"].get<std::string>() != symbol) continue;

            if (j.contains("category") && j["category"].is_string()) {
                if (j["category"].get<std::string>() != category) continue;
            } else continue;

            if (!j.contains("execId") || !j["execId"].is_string()) continue;
            if (!j.contains("side")) continue;
            if (!j.contains("execQty")) continue;
            if (!j.contains("execPrice")) continue;
			// IMPORTANT: only real trade fills
			if (j.contains("execType") && j["execType"].is_string()) {
				if (j["execType"].get<std::string>() != "Trade") continue;
			} else {
				continue;
			}
            long long ts=0;
            if (!extract_ts_ms_any(j, ts)) continue;

            out.push_back(j);
        } catch (...) {}
    }

    std::sort(out.begin(), out.end(), [](const nlohmann::json& a, const nlohmann::json& b){
        long long ta=0, tb=0;
        extract_ts_ms_any(a, ta);
        extract_ts_ms_any(b, tb);
        if (ta != tb) return ta < tb;
        return a.value("execId", "") < b.value("execId", "");
    });

    return out;
}

// FIFO matching. Generates trade events on closes.
// Allocates BOTH close fee (from close fill) and open fee (stored in lot.fee_rem)
static void process_fill_and_emit_trades(const std::string& category,
                                        const std::string& symbol,
                                        const nlohmann::json& fill,
                                        std::vector<Lot>& lots,
                                        long long& closed_events,
                                        double& gross_realized_sum,
                                        double& net_realized_sum) {
    const std::string execId = fill.value("execId", "");
    long long ts=0; extract_ts_ms_any(fill, ts);

    const std::string side = fill.value("side", "");
    const double px  = get_num_field(fill, "execPrice");
    double qty       = get_num_field(fill, "execQty");
    const double fee_total = get_num_field(fill, "execFee"); // fee for THIS fill

    if (qty <= 0.0 || px <= 0.0) return;

    const bool isBuy  = (side == "Buy"  || side == "BUY"  || side == "buy");
    const bool isSell = (side == "Sell" || side == "SELL" || side == "sell");
    if (!isBuy && !isSell) return;

    const double fill_qty_total = qty; // keep original for fee splitting

    auto close_against = [&](LotSide against_side) {
        for (size_t i = 0; i < lots.size() && qty > 0.0; ) {
            if (lots[i].side != against_side) { i++; continue; }

            const double close_qty = std::min(qty, lots[i].qty);
            if (close_qty <= 0.0) { i++; continue; }

            const double open_px  = lots[i].px;
            const double close_px = px;

            double gross = 0.0;
            if (against_side == LotSide::LONG) {
                gross = (close_px - open_px) * close_qty;   // sell closes long
            } else {
                gross = (open_px - close_px) * close_qty;   // buy closes short
            }

            // CLOSE fee allocation from this fill
            double fee_close_alloc = 0.0;
            if (fee_total != 0.0 && fill_qty_total > 0.0) {
                fee_close_alloc = fee_total * (close_qty / fill_qty_total);
            }

            // OPEN fee allocation from the lot
            double fee_open_alloc = 0.0;
            if (lots[i].fee_rem != 0.0 && lots[i].qty > 0.0) {
                fee_open_alloc = lots[i].fee_rem * (close_qty / lots[i].qty);
            }

            const double net = gross - fee_close_alloc - fee_open_alloc;

            nlohmann::json ev;
            ev["ts_ms"] = ts;
            ev["category"] = category;
            ev["symbol"] = symbol;

            ev["close_execId"] = execId;
            ev["open_execId"]  = lots[i].execId;
            ev["side_closed"]  = lot_side_str(against_side);

            ev["qty"] = close_qty;
            ev["open_price"] = open_px;
            ev["close_price"] = close_px;

            ev["gross_realized"] = gross;
            ev["fee_close_alloc"] = fee_close_alloc;
            ev["fee_open_alloc"]  = fee_open_alloc;
            ev["net_realized"] = net;

            ev["tradeId"] = make_trade_id(execId, lots[i].execId, symbol, close_qty, open_px, close_px, ts);

            if (append_trade_event(ev)) {
                closed_events++;
                gross_realized_sum += gross;
                net_realized_sum   += net;
            }

            lots[i].fee_rem -= fee_open_alloc;
            lots[i].qty     -= close_qty;
            qty             -= close_qty;

            if (lots[i].qty <= 1e-12) {
                lots.erase(lots.begin() + (long long)i);
            } else {
                i++;
            }
        }
    };

    if (isBuy) {
        close_against(LotSide::SHORT);   // Buy closes shorts first
        if (qty > 1e-12) {
            Lot l;
            l.side = LotSide::LONG;
            l.qty = qty;
            l.px = px;
            l.execId = execId;
            l.ts_ms = ts;
            l.fee_rem = (fee_total != 0.0 && fill_qty_total > 0.0) ? (fee_total * (qty / fill_qty_total)) : 0.0;
            lots.push_back(l);
        }
    } else {
        close_against(LotSide::LONG);    // Sell closes longs first
        if (qty > 1e-12) {
            Lot l;
            l.side = LotSide::SHORT;
            l.qty = qty;
            l.px = px;
            l.execId = execId;
            l.ts_ms = ts;
            l.fee_rem = (fee_total != 0.0 && fill_qty_total > 0.0) ? (fee_total * (qty / fill_qty_total)) : 0.0;
            lots.push_back(l);
        }
    }
}

// Sum realized pnl from trades ledger in a time window
static nlohmann::json sum_realized_from_trade_ledger(const std::string& symbol, long long start_ms, long long end_ms) {
    std::ifstream in(TRADE_LEDGER_PATH);
    double gross = 0.0;
    double net = 0.0;
    long long count = 0;
    long long scanned = 0;

    if (!in.good()) {
        nlohmann::json err;
        err["error"] = "cannot_open_ledger";
        err["ledger_path"] = TRADE_LEDGER_PATH;
        return err;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        scanned++;
        try {
            auto j = nlohmann::json::parse(line);

            if (!j.contains("symbol") || !j["symbol"].is_string()) continue;
            if (j["symbol"].get<std::string>() != symbol) continue;

            long long ts=0;
            if (!extract_ts_ms_any(j, ts)) continue;
            if (ts < start_ms || ts > end_ms) continue;

            gross += j.value("gross_realized", 0.0);
            net   += j.value("net_realized", 0.0);
            count++;
        } catch (...) {}
    }

    nlohmann::json out;
    out["symbol"] = symbol;
    out["gross_realized"] = gross;
    out["net_realized"] = net;
    out["close_events"] = count;
    out["scanned_lines"] = scanned;
    out["ledger_path"] = TRADE_LEDGER_PATH;
    return out;
}

static nlohmann::json sum_realized_from_trade_ledger_all(const std::string& symbol) {
    std::ifstream in(TRADE_LEDGER_PATH);
    double gross = 0.0;
    double net = 0.0;
    long long count = 0;
    long long scanned = 0;

    if (!in.good()) {
        nlohmann::json err;
        err["error"] = "cannot_open_ledger";
        err["ledger_path"] = TRADE_LEDGER_PATH;
        return err;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        scanned++;
        try {
            auto j = nlohmann::json::parse(line);

            if (!j.contains("symbol") || !j["symbol"].is_string()) continue;
            if (j["symbol"].get<std::string>() != symbol) continue;

            gross += j.value("gross_realized", 0.0);
            net   += j.value("net_realized", 0.0);
            count++;
        } catch (...) {}
    }

    nlohmann::json out;
    out["symbol"] = symbol;
    out["gross_realized_all"] = gross;
    out["net_realized_all"] = net;
    out["close_events_all"] = count;
    out["scanned_lines"] = scanned;
    out["ledger_path"] = TRADE_LEDGER_PATH;
    return out;
}

static long long now_ms_local() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

static bool extract_ts_ms(const nlohmann::json& j, long long& ts_out) {
    if (!j.contains("ts_ms")) return false;
    try {
        if (j["ts_ms"].is_number_integer()) { ts_out = j["ts_ms"].get<long long>(); return true; }
        if (j["ts_ms"].is_number())         { ts_out = (long long)j["ts_ms"].get<double>(); return true; }
        if (j["ts_ms"].is_string())         { ts_out = std::stoll(j["ts_ms"].get<std::string>()); return true; }
    } catch (...) {}
    return false;
}

// Sum exec fees from execution ledger within a time window
static nlohmann::json sum_exec_fees_from_ledger(const std::string& symbol, long long start_ms, long long end_ms) {
    std::ifstream in(LEDGER_PATH);
    double fees = 0.0;
    long long count = 0;
    long long scanned = 0;

    if (!in.good()) {
        nlohmann::json err;
        err["error"] = "cannot_open_ledger";
        err["ledger_path"] = LEDGER_PATH;
        return err;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        scanned++;
        try {
            auto j = nlohmann::json::parse(line);

            if (j.contains("symbol") && j["symbol"].is_string()) {
                if (j["symbol"].get<std::string>() != symbol) continue;
            } else continue;

            long long ts=0;
            if (!extract_ts_ms(j, ts)) continue;
            if (ts < start_ms || ts > end_ms) continue;

            if (j.contains("execFee")) {
                fees += get_num_safe(j["execFee"]);
            }
            count++;
        } catch (...) {}
    }

    nlohmann::json out;
    out["symbol"] = symbol;
    out["fees"] = fees;
    out["exec_count"] = count;
    out["scanned_lines"] = scanned;
    out["ledger_path"] = LEDGER_PATH;
    return out;
}

// Sum funding from funding ledger within a time window
static nlohmann::json sum_funding_from_ledger(const std::string& symbol, long long start_ms, long long end_ms) {
    std::ifstream in(FUND_LEDGER_PATH);
    double funding = 0.0;
    long long count = 0;
    long long scanned = 0;

    if (!in.good()) {
        nlohmann::json err;
        err["error"] = "cannot_open_ledger";
        err["ledger_path"] = FUND_LEDGER_PATH;
        return err;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        scanned++;
        try {
            auto j = nlohmann::json::parse(line);

            if (j.contains("symbol") && j["symbol"].is_string()) {
                if (j["symbol"].get<std::string>() != symbol) continue;
            } else continue;

            long long ts=0;
            if (!extract_ts_ms(j, ts)) continue;
            if (ts < start_ms || ts > end_ms) continue;

            if (j.contains("funding")) {
                funding += get_num_safe(j["funding"]);
            }
            count++;
        } catch (...) {}
    }

    nlohmann::json out;
    out["symbol"] = symbol;
    out["funding"] = funding;
    out["event_count"] = count;
    out["scanned_lines"] = scanned;
    out["ledger_path"] = FUND_LEDGER_PATH;
    return out;
}

// ------------------------ Funding ledger (shared) ------------------------
static std::unordered_set<std::string> g_seen_fund_ids;

static void load_seen_funding_ids() {
    std::ifstream in(FUND_LEDGER_PATH);
    if (!in.good()) return;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            auto j = nlohmann::json::parse(line);
            if (j.contains("fundId") && j["fundId"].is_string()) {
                g_seen_fund_ids.insert(j["fundId"].get<std::string>());
            }
        } catch (...) {}
    }
}

static std::string make_fund_dedupe_key(const nlohmann::json& e, const std::string& symbol) {
    if (e.contains("id") && e["id"].is_string()) return e["id"].get<std::string>();
    if (e.contains("transId") && e["transId"].is_string()) return e["transId"].get<std::string>();
    if (e.contains("txnId") && e["txnId"].is_string()) return e["txnId"].get<std::string>();

    std::string ts;
    if (e.contains("execTime")) {
        if (e["execTime"].is_string()) ts = e["execTime"].get<std::string>();
        else if (e["execTime"].is_number()) ts = std::to_string((long long)e["execTime"].get<double>());
    } else if (e.contains("transactionTime")) {
        if (e["transactionTime"].is_string()) ts = e["transactionTime"].get<std::string>();
        else if (e["transactionTime"].is_number()) ts = std::to_string((long long)e["transactionTime"].get<double>());
    }

    std::string fund;
    if (e.contains("funding")) {
        if (e["funding"].is_string()) fund = e["funding"].get<std::string>();
        else if (e["funding"].is_number()) fund = std::to_string(e["funding"].get<double>());
    }

    std::string ccy;
    if (e.contains("currency") && e["currency"].is_string()) ccy = e["currency"].get<std::string>();
    if (ccy.empty() && e.contains("feeCurrency") && e["feeCurrency"].is_string()) ccy = e["feeCurrency"].get<std::string>();

    return ts + "|" + symbol + "|" + fund + "|" + ccy;
}

static bool append_funding_to_ledger(const nlohmann::json& e,
                                     const std::string& category,
                                     const std::string& symbol) {
    const std::string fundId = make_fund_dedupe_key(e, symbol);
    if (fundId.empty()) return false;

    std::lock_guard<std::mutex> lk(g_ledger_mtx);

    if (g_seen_fund_ids.find(fundId) != g_seen_fund_ids.end())
        return false;

    nlohmann::json out;
    out["category"] = category;
    out["symbol"] = symbol;
    out["fundId"] = fundId;

    if (e.contains("execTime")) out["ts_ms"] = e["execTime"];
    if (e.contains("transactionTime")) out["ts_ms"] = e["transactionTime"];
    if (e.contains("type")) out["type"] = e["type"];
    if (e.contains("funding")) out["funding"] = e["funding"];
    if (e.contains("currency")) out["currency"] = e["currency"];
    if (e.contains("feeCurrency")) out["currency"] = e["feeCurrency"];
    if (e.contains("cashFlow")) out["cashFlow"] = e["cashFlow"];
    if (e.contains("change")) out["change"] = e["change"];
    if (e.contains("amount")) out["amount"] = e["amount"];
    if (e.contains("walletBalance")) out["walletBalance"] = e["walletBalance"];
    if (e.contains("symbol")) out["symbol_api"] = e["symbol"];

    std::ofstream f(FUND_LEDGER_PATH, std::ios::app);
    if (!f.good()) return false;

    f << out.dump() << "\n";
    g_seen_fund_ids.insert(fundId);
    return true;
}

// ------------------------ Execution ledger (shared) ------------------------
static std::unordered_set<std::string> g_seen_exec_ids;

static void load_seen_exec_ids() {
    std::ifstream in(LEDGER_PATH);
    if (!in.good()) return;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            auto j = nlohmann::json::parse(line);
            if (j.contains("execId") && j["execId"].is_string()) {
                g_seen_exec_ids.insert(j["execId"].get<std::string>());
            }
        } catch (...) {}
    }
}

static bool append_execution_to_ledger(const nlohmann::json& e,
                                       const std::string& category,
                                       const std::string& symbol) {
    if (!e.contains("execId") || !e["execId"].is_string()) return false;
    const std::string execId = e["execId"].get<std::string>();
    if (execId.empty()) return false;

    std::lock_guard<std::mutex> lk(g_ledger_mtx);

    if (g_seen_exec_ids.find(execId) != g_seen_exec_ids.end())
        return false;

    nlohmann::json out;
    out["category"] = category;
    out["symbol"] = symbol;
    out["execId"] = execId;

    if (e.contains("execTime")) out["ts_ms"] = e["execTime"];
    if (e.contains("orderId")) out["orderId"] = e["orderId"];
    if (e.contains("side")) out["side"] = e["side"];
    if (e.contains("execPrice")) out["execPrice"] = e["execPrice"];
    if (e.contains("execQty")) out["execQty"] = e["execQty"];
    if (e.contains("execFee")) out["execFee"] = e["execFee"];
    if (e.contains("feeCurrency")) out["feeCurrency"] = e["feeCurrency"];
    if (e.contains("execType")) out["execType"] = e["execType"];
    if (e.contains("orderType")) out["orderType"] = e["orderType"];
    if (e.contains("seq")) out["seq"] = e["seq"];

    std::ofstream f(LEDGER_PATH, std::ios::app);
    if (!f.good()) return false;

    f << out.dump() << "\n";
    g_seen_exec_ids.insert(execId);
    return true;
}

// ------------------------ Utilities ------------------------
static std::string env_or(const char* k, const std::string& defv) {
    const char* v = std::getenv(k);
    return v ? std::string(v) : defv;
}

// Latest cached top-of-book (from ZMQ)
static std::mutex g_px_mtx;
static double g_last_bid = 0.0;
static double g_last_ask = 0.0;

// ---- Open order tracking (for cancel / killswitch) ----
struct OpenOrderInfo {
    std::string category;
    std::string symbol;
};

static std::mutex g_orders_mtx;
static std::unordered_map<std::string, OpenOrderInfo> g_open_orders; // orderId -> info

static void track_order(const std::string& orderId, const std::string& category, const std::string& symbol) {
    std::lock_guard<std::mutex> lk(g_orders_mtx);
    g_open_orders[orderId] = {category, symbol};
}

static void untrack_order(const std::string& orderId) {
    std::lock_guard<std::mutex> lk(g_orders_mtx);
    g_open_orders.erase(orderId);
}

static std::vector<std::pair<std::string, OpenOrderInfo>> snapshot_open_orders() {
    std::lock_guard<std::mutex> lk(g_orders_mtx);
    std::vector<std::pair<std::string, OpenOrderInfo>> v;
    v.reserve(g_open_orders.size());
    for (auto& kv : g_open_orders) v.push_back(kv);
    return v;
}

// ---- Ctrl+C kill flag ----
static std::atomic<bool> g_sigint{false};
static void on_sigint(int) { g_sigint.store(true); }

// Safe numeric parsing
static double get_num_safe(const nlohmann::json& v) {
    try {
        if (v.is_number_float() || v.is_number_integer() || v.is_number_unsigned())
            return v.get<double>();
        if (v.is_string()) {
            const auto s = v.get<std::string>();
            if (s.empty()) return 0.0;
            return std::stod(s);
        }
        return 0.0;
    } catch (...) {
        return 0.0;
    }
}

// ------------------------ main ------------------------
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

    // Bybit client (keys from env)
    BybitDemoClient bybit(
        env_or("BYBIT_API_KEY", ""),
        env_or("BYBIT_API_SECRET", "")
    );
	if (bybit.ready()) {
    bool ok = bybit.sync_time();
    std::cout << "[BYBIT] time sync " << (ok ? "OK" : "FAILED") << "\n";
	}
	ImbMomoParams sp;
	ImbMomoStrategy strat(sp);
	
    // Ledger warmup
    load_seen_exec_ids();
    load_seen_funding_ids();
    load_seen_trade_ids();

    std::cout << "[ledger] loaded tradeIds=" << g_seen_trade_ids.size()
              << " from " << TRADE_LEDGER_PATH << "\n";
    std::cout << "[ledger] loaded execIds=" << g_seen_exec_ids.size()
              << " from " << LEDGER_PATH << "\n";
    std::cout << "[ledger] loaded fundIds=" << g_seen_fund_ids.size()
              << " from " << FUND_LEDGER_PATH << "\n";

    if (!bybit.ready()) {
        std::cout << "[place_order] NOTE: set BYBIT_API_KEY and BYBIT_API_SECRET to enable orders.\n";
    }

    std::atomic<bool> running{true};
    std::signal(SIGINT, on_sigint);

    // Thread: ZMQ receive (cache bid/ask from JSON)
    std::thread rx([&](){
        while (running.load()) {
            zmq::message_t part1;
            if (!sub.recv(part1, zmq::recv_flags::none)) continue;

            std::string payload;

            // Handle both: [payload] OR [topic][payload]
            if (part1.more()) {
                zmq::message_t part2;
                if (!sub.recv(part2, zmq::recv_flags::none)) continue;
                payload = std::string(static_cast<char*>(part2.data()), part2.size());
            } else {
                payload = std::string(static_cast<char*>(part1.data()), part1.size());
            }
			// ✅ ADD HERE
			static int raw_prints = 0;
			if (raw_prints < 5) {
				std::cout << "\n[ZMQ RAW " << raw_prints << "] " << payload << "\n";
				raw_prints++;
			}
            try {
                auto j = nlohmann::json::parse(payload);

                static int k = 0;
                if (k < 2) {
                    std::cout << "[KEYS] top-level: ";
                    for (auto it = j.begin(); it != j.end(); ++it) std::cout << it.key() << " ";
                    std::cout << "\n";
                    k++;
                }

                auto& tob = j.at("top_of_book");
                double bid = get_num_safe(tob.at("bid"));
                double ask = get_num_safe(tob.at("ask"));

                {
                    std::lock_guard<std::mutex> lk(g_px_mtx);
                    g_last_bid = bid;
                    g_last_ask = ask;
                }
				MarketTick t;
				t.exchange = j.value("exchange", "");
				t.instrument = j.value("instrument", "");
				t.ts_ms = j.value("ts_ms", 0LL);

				auto &tob2 = j.at("top_of_book");
				t.bid = get_num_safe(tob2.at("bid"));
				t.ask = get_num_safe(tob2.at("ask"));
				t.mid = get_num_safe(tob2.at("mid"));
				t.spread = get_num_safe(tob2.at("spread"));

				auto &ret = j.at("returns");
				t.r1 = get_num_safe(ret.at("r1"));
				t.r5 = get_num_safe(ret.at("r5"));
				t.r10 = get_num_safe(ret.at("r10"));

				auto &dep = j.at("depth");
				if (dep.contains("bid_vol") && dep["bid_vol"].is_array())
    				for (auto &x : dep["bid_vol"]) t.bid_vol.push_back(get_num_safe(x));
				if (dep.contains("ask_vol") && dep["ask_vol"].is_array())
    				for (auto &x : dep["ask_vol"]) t.ask_vol.push_back(get_num_safe(x));

				auto sig = strat.on_tick(t);
				if (sig.has_value() && g_print_sig.load() && !g_auto.load()) {
					std::lock_guard<std::mutex> lk(g_log_mtx);
    				std::string s = (sig->side == Side::Buy) ? "BUY" : "SELL";
    				std::cout << "[STRAT] "
              				<< (sig->type == StrategySignal::Type::Enter ? "ENTER " : "EXIT ")
              				<< s
              				<< " mid=" << t.mid
              				<< " spr=" << t.spread
              				<< " r1=" << t.r1
              				<< " r5=" << t.r5
              				<< " imb=" << sig->imbalance
              				<< " tp=" << sig->tp_px
              				<< " sl=" << sig->sl_px
              				<< " reason=" << sig->reason
              				<< "\n";
				}
				if (sig.has_value() && g_auto.load() && bybit.ready()) {
    std::lock_guard<std::mutex> api_lk(g_exec_mtx);

    std::string category = env_or("STRAT_CATEGORY", "linear");
    std::string symbol   = env_or("STRAT_SYMBOL", "ETHUSDT");
    double qty           = std::stod(env_or("STRAT_QTY", "0.01"));

    if (sig->type == StrategySignal::Type::Enter) {
        std::string side = (sig->side == Side::Buy) ? "Buy" : "Sell";

        {
            std::lock_guard<std::mutex> log_lk(g_log_mtx);
            std::cout << "[AUTO] ENTER placing MARKET " << side
                      << " " << symbol << " qty=" << qty << "\n";
        }

        std::string resp = bybit.place_market_order(category, symbol, side, qty);

        {
            std::lock_guard<std::mutex> log_lk(g_log_mtx);
            std::cout << "[AUTO] resp=" << resp << "\n";
        }
    } else {
        {
            std::lock_guard<std::mutex> log_lk(g_log_mtx);
            std::cout << "[AUTO] EXIT reduce-only close " << symbol << "\n";
        }

        std::string resp = bybit.close_position_market_reduce_only(category, symbol);

        {
            std::lock_guard<std::mutex> log_lk(g_log_mtx);
            std::cout << "[AUTO] resp=" << resp << "\n";
        }
    }
}

            } catch (const std::exception& e) {
                std::cout << "[ERR] " << e.what() << "\n";
            }
        }
    });

    // Main thread: user commands
std::cout << "\nCommands:\n"
          << "  px\n"
          << "  buy         <category> <symbol> <qty>  mkt|lmt\n"
          << "  sell        <category> <symbol> <qty>  mkt|lmt\n"
          << "  cancel      <category> <symbol> <orderId>\n"
          << "  close       <category> <symbol>\n"
          << "  pos         <category> <symbol>\n"
          << "  pnl         <category> <symbol>\n"
          << "  pnlw        <category> <symbol> <minutes>\n"
          << "  fundw       <category> <symbol> <minutes>\n"
          << "  pnlx        <category> <symbol> <minutes>\n"
          << "  sync_exec   <category> <symbol> <minutes>\n"
          << "  sync_fund   <category> <symbol> <minutes>\n"
          << "  pnlwL       <symbol> <minutes>\n"
          << "  fundwL      <symbol> <minutes>\n"
          << "  pnlxL       <category> <symbol> <minutes>\n"
          << "  reconcile   <category> <symbol> <minutes>\n"
          << "  sync_trades <category> <symbol>\n"
          << "  realwL      <symbol> <minutes>\n"
          << "  lotsL       <category> <symbol>\n"
          << "  pnlFULL     <category> <symbol> <minutes>\n"
          << "  auto on|off\n"
          << "  sig on|off\n"
          << "  close_short <category> <symbol> <qty>  mkt|lmt   (reduce SHORT)\n"
          << "  close_long  <category> <symbol> <qty>  mkt|lmt   (reduce LONG)\n"
          << "  quit\n\n";

    std::string cmd;
    while (std::cin >> cmd) {
        if (cmd == "quit") break;
        if (g_sigint.load()) break;

        // ---- px ----
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
		if (cmd == "auto") {
			std::string v; std::cin >> v;
			g_auto.store(v == "on");
			std::cout << "[AUTO] " << (g_auto.load() ? "ON" : "OFF") << "\n";
			continue;
		}
		if (cmd == "sig") {
    std::string v; std::cin >> v;
    g_print_sig.store(v == "on");
    std::lock_guard<std::mutex> lk(g_log_mtx);
    std::cout << "[SIGPRINT] " << (g_print_sig.load() ? "ON" : "OFF") << "\n";
    continue;
}
        // ---- cancel ----
        if (cmd == "cancel") {
            std::string category, symbol, orderId;
            std::cin >> category >> symbol >> orderId;

            if (!bybit.ready()) {
                std::cout << "Set BYBIT_API_KEY and BYBIT_API_SECRET first.\n";
                continue;
            }

            std::string resp = bybit.cancel_order(category, symbol, orderId);
            std::cout << resp << "\n";

            try {
                auto j = nlohmann::json::parse(resp);
                if (j.contains("retCode") && j["retCode"].is_number() && j["retCode"].get<int>() == 0) {
                    untrack_order(orderId);
                }
            } catch (...) {}

            continue;
        }

        // ---- close ----
        if (cmd == "close") {
            std::string category, symbol;
            std::cin >> category >> symbol;

            if (!bybit.ready()) {
                std::cout << "Set BYBIT_API_KEY and BYBIT_API_SECRET first.\n";
                continue;
            }

            std::string resp = bybit.close_position_market_reduce_only(category, symbol);
            std::cout << resp << "\n";
            continue;
        }

        // ---- pos ----
        if (cmd == "pos") {
            std::string category, symbol;
            std::cin >> category >> symbol;

            if (!bybit.ready()) {
                std::cout << "Set BYBIT_API_KEY and BYBIT_API_SECRET first.\n";
                continue;
            }

            std::string resp = bybit.get_positions(category, symbol);
            std::cout << resp << "\n";
            continue;
        }

        // ---- pnl ----
        if (cmd == "pnl") {
            std::string category, symbol;
            std::cin >> category >> symbol;

            if (!bybit.ready()) {
                std::cout << "Set BYBIT_API_KEY and BYBIT_API_SECRET first.\n";
                continue;
            }

            std::string resp = bybit.get_positions(category, symbol);

            try {
                auto j = nlohmann::json::parse(resp);
                if (!j.contains("retCode") || j["retCode"].get<int>() != 0) {
                    std::cout << resp << "\n";
                    continue;
                }

                auto &list = j["result"]["list"];
                if (!list.is_array() || list.empty()) {
                    std::cout << "{\"error\":\"no position data\"}\n";
                    continue;
                }

                auto &p = list[0];

                std::string sym  = p.value("symbol", symbol);
                std::string side = p.value("side", "");
                double size = get_num_safe(p.value("size", "0"));
                double avg  = get_num_safe(p.value("avgPrice", "0"));
                double mark = get_num_safe(p.value("markPrice", "0"));
                double u    = get_num_safe(p.value("unrealisedPnl", "0"));
                double r    = get_num_safe(p.value("cumRealisedPnl", "0"));

                nlohmann::json out;
                out["symbol"] = sym;
                out["side"]   = side;
                out["size"]   = size;
                out["avgPrice"] = avg;
                out["markPrice"] = mark;
                out["uPnL"]    = u;
                out["cumRealisedPnl"] = r;

                std::cout << out.dump() << "\n";
            } catch (...) {
                std::cout << resp << "\n";
            }
            continue;
        }
		if (cmd == "close_short" || cmd == "close_long") {
    std::string category, symbol;
    double qty = 0.0;
    std::string ord = "mkt";
    std::cin >> category >> symbol >> qty;

    // optional token
    if (std::cin.peek() == ' ') {
        while (std::cin.peek() == ' ') std::cin.get();
        if (std::cin.peek() != '\n' && std::cin.peek() != '\r') std::cin >> ord;
    }
    OrdType ot = parse_ordtype_or_default_mkt(ord);

    if (!bybit.ready()) { std::cout << "Set BYBIT_API_KEY and BYBIT_API_SECRET first.\n"; continue; }

    double bid=0.0, ask=0.0;
    { std::lock_guard<std::mutex> lk(g_px_mtx); bid=g_last_bid; ask=g_last_ask; }
    if (bid <= 0.0 || ask <= 0.0) { std::cout << "No bid/ask yet. Run px.\n"; continue; }

    std::string side = (cmd == "close_short") ? "Buy" : "Sell";

    std::string resp;
    if (ot == OrdType::MKT) {
        resp = bybit.place_market_order(category, symbol, side, qty, true /*reduceOnly*/);
    } else {
        double px = (side == "Buy") ? ask : bid;
        resp = bybit.place_limit_order(category, symbol, side, qty, px, "GTC", true /*reduceOnly*/);
    }

    std::cout << resp << "\n";
    continue;
}
        // ---- pnlw (execution fees, paginated) ----
        if (cmd == "pnlw") {
            std::string category, symbol;
            int minutes;
            std::cin >> category >> symbol >> minutes;

            if (!bybit.ready()) {
                std::cout << "Set BYBIT_API_KEY and BYBIT_API_SECRET first.\n";
                continue;
            }

            long long now = now_ms_local();
            long long start = now - (long long)minutes * 60 * 1000;

            double fee_sum = 0.0;
            long long exec_count = 0;
            int pages = 0;

            std::string cursor;

            while (true) {
                std::string resp = bybit.get_executions(category, symbol, start, cursor);

                try {
                    auto j = nlohmann::json::parse(resp);
                    if (!j.contains("retCode") || j["retCode"].get<int>() != 0) {
                        std::cout << resp << "\n";
                        break;
                    }

                    if (j.contains("result") && j["result"].contains("list") && j["result"]["list"].is_array()) {
                        for (auto &e : j["result"]["list"]) {
                            if (e.contains("execFee")) fee_sum += get_num_safe(e["execFee"]);
                            exec_count++;
                        }
                    }

                    std::string next;
                    if (j.contains("result") && j["result"].contains("nextPageCursor") && j["result"]["nextPageCursor"].is_string()) {
                        next = j["result"]["nextPageCursor"].get<std::string>();
                    }

                    pages++;
                    if (next.empty()) break;
                    cursor = next;

                    if (pages > 50) break;
                } catch (...) {
                    std::cout << resp << "\n";
                    break;
                }
            }

            nlohmann::json out;
            out["symbol"] = symbol;
            out["window_minutes"] = minutes;
            out["pages"] = pages;
            out["exec_count"] = exec_count;
            out["fees"] = fee_sum;

            std::cout << out.dump() << "\n";
            continue;
        }

        // ---- fundw (funding, paginated) ----
        if (cmd == "fundw") {
            std::string category, symbol;
            int minutes;
            std::cin >> category >> symbol >> minutes;

            if (!bybit.ready()) {
                std::cout << "Set BYBIT_API_KEY and BYBIT_API_SECRET first.\n";
                continue;
            }

            long long now = now_ms_local();
            long long start = now - (long long)minutes * 60 * 1000;

            double funding_sum = 0.0;
            long long event_count = 0;
            int pages = 0;

            std::string cursor;

            while (true) {
                std::string resp = bybit.get_transaction_log(category, symbol, start, now, cursor, "SETTLEMENT");

                try {
                    auto j = nlohmann::json::parse(resp);
                    if (!j.contains("retCode") || j["retCode"].get<int>() != 0) {
                        std::cout << resp << "\n";
                        break;
                    }

                    if (j.contains("result") && j["result"].contains("list") && j["result"]["list"].is_array()) {
                        for (auto &e : j["result"]["list"]) {
                            if (e.contains("symbol") && e["symbol"].is_string()) {
                                if (e["symbol"].get<std::string>() != symbol) continue;
                            }

                            if (e.contains("funding")) {
                                funding_sum += get_num_safe(e["funding"]);
                                event_count++;
                            }
                        }
                    }

                    std::string next;
                    if (j.contains("result") && j["result"].contains("nextPageCursor") && j["result"]["nextPageCursor"].is_string()) {
                        next = j["result"]["nextPageCursor"].get<std::string>();
                    }

                    pages++;
                    if (next.empty()) break;
                    cursor = next;

                    if (pages > 50) break;
                } catch (...) {
                    std::cout << resp << "\n";
                    break;
                }
            }

            nlohmann::json out;
            out["symbol"] = symbol;
            out["window_minutes"] = minutes;
            out["pages"] = pages;
            out["event_count"] = event_count;
            out["funding"] = funding_sum;

            std::cout << out.dump() << "\n";
            continue;
        }

        // ---- pnlx (unified view) ----
        if (cmd == "pnlx") {
            std::string category, symbol;
            int minutes;
            std::cin >> category >> symbol >> minutes;

            if (!bybit.ready()) {
                std::cout << "Set BYBIT_API_KEY and BYBIT_API_SECRET first.\n";
                continue;
            }

            long long now = now_ms_local();
            long long start = now - (long long)minutes * 60 * 1000;

            // 1) Position snapshot
            double uPnL = 0.0, cumRealisedPnl = 0.0, size = 0.0, avg = 0.0, mark = 0.0;
            std::string side;

            {
                std::string resp = bybit.get_positions(category, symbol);
                try {
                    auto j = nlohmann::json::parse(resp);
                    if (!j.contains("retCode") || j["retCode"].get<int>() != 0) {
                        std::cout << resp << "\n";
                        continue;
                    }
                    auto &list = j["result"]["list"];
                    if (!list.is_array() || list.empty()) {
                        nlohmann::json out;
                        out["symbol"] = symbol;
                        out["error"] = "no position data";
                        std::cout << out.dump() << "\n";
                        continue;
                    }

                    auto &p = list[0];
                    side = p.value("side", "");
                    size = get_num_safe(p.value("size", "0"));
                    avg  = get_num_safe(p.value("avgPrice", "0"));
                    mark = get_num_safe(p.value("markPrice", "0"));
                    uPnL = get_num_safe(p.value("unrealisedPnl", "0"));
                    cumRealisedPnl = get_num_safe(p.value("cumRealisedPnl", "0"));
                } catch (...) {
                    std::cout << resp << "\n";
                    continue;
                }
            }

            // 2) Fees window
            double fees = 0.0;
            long long exec_count = 0;
            int fee_pages = 0;
            {
                std::string cursor;
                while (true) {
                    std::string resp = bybit.get_executions(category, symbol, start, cursor);
                    try {
                        auto j = nlohmann::json::parse(resp);
                        if (!j.contains("retCode") || j["retCode"].get<int>() != 0) break;

                        if (j.contains("result") && j["result"].contains("list") && j["result"]["list"].is_array()) {
                            for (auto &e : j["result"]["list"]) {
                                if (e.contains("execFee")) fees += get_num_safe(e["execFee"]);
                                exec_count++;
                            }
                        }

                        std::string next;
                        if (j.contains("result") && j["result"].contains("nextPageCursor") && j["result"]["nextPageCursor"].is_string()) {
                            next = j["result"]["nextPageCursor"].get<std::string>();
                        }

                        fee_pages++;
                        if (next.empty()) break;
                        cursor = next;
                        if (fee_pages > 50) break;
                    } catch (...) { break; }
                }
            }

            // 3) Funding window
            double funding = 0.0;
            long long funding_count = 0;
            int fund_pages = 0;
            {
                std::string cursor;
                while (true) {
                    std::string resp = bybit.get_transaction_log(category, symbol, start, now, cursor, "SETTLEMENT");
                    try {
                        auto j = nlohmann::json::parse(resp);
                        if (!j.contains("retCode") || j["retCode"].get<int>() != 0) break;

                        if (j.contains("result") && j["result"].contains("list") && j["result"]["list"].is_array()) {
                            for (auto &e : j["result"]["list"]) {
                                if (e.contains("symbol") && e["symbol"].is_string()) {
                                    if (e["symbol"].get<std::string>() != symbol) continue;
                                }
                                if (e.contains("funding")) {
                                    funding += get_num_safe(e["funding"]);
                                    funding_count++;
                                }
                            }
                        }

                        std::string next;
                        if (j.contains("result") && j["result"].contains("nextPageCursor") && j["result"]["nextPageCursor"].is_string()) {
                            next = j["result"]["nextPageCursor"].get<std::string>();
                        }

                        fund_pages++;
                        if (next.empty()) break;
                        cursor = next;
                        if (fund_pages > 50) break;
                    } catch (...) { break; }
                }
            }

            double net_window_cashflow = fees + funding;
            double net_mixed_view = uPnL + cumRealisedPnl + fees + funding;

            nlohmann::json out;
            out["symbol"] = symbol;
            out["side"] = side;
            out["size"] = size;
            out["avgPrice"] = avg;
            out["markPrice"] = mark;
            out["uPnL"] = uPnL;
            out["cumRealisedPnl"] = cumRealisedPnl;
            out["window_minutes"] = minutes;

            out["fees"] = fees;
            out["fees_exec_count"] = exec_count;
            out["fees_pages"] = fee_pages;

            out["funding"] = funding;
            out["funding_event_count"] = funding_count;
            out["funding_pages"] = fund_pages;

            out["net_window_cashflow"] = net_window_cashflow;
            out["net_mixed_view"] = net_mixed_view;

            std::cout << out.dump() << "\n";
            continue;
        }

        // ---- sync_exec ----
        if (cmd == "sync_exec") {
            std::string category, symbol;
            int minutes;
            std::cin >> category >> symbol >> minutes;

            if (!bybit.ready()) {
                std::cout << "Set BYBIT_API_KEY and BYBIT_API_SECRET first.\n";
                continue;
            }

            long long now = now_ms_local();
            long long start = now - (long long)minutes * 60 * 1000;

            std::string cursor;
            int pages = 0;
            long long fetched = 0;
            long long appended = 0;
            long long dupes = 0;

            while (true) {
                std::string resp = bybit.get_executions(category, symbol, start, cursor);

                try {
                    auto j = nlohmann::json::parse(resp);
                    if (!j.contains("retCode") || j["retCode"].get<int>() != 0) {
                        std::cout << resp << "\n";
                        break;
                    }

                    if (j.contains("result") && j["result"].contains("list") && j["result"]["list"].is_array()) {
                        for (auto &e : j["result"]["list"]) {
                            fetched++;
                            bool ok = append_execution_to_ledger(e, category, symbol);
                            if (ok) appended++;
                            else dupes++;
                        }
                    }

                    std::string next;
                    if (j.contains("result") && j["result"].contains("nextPageCursor") && j["result"]["nextPageCursor"].is_string()) {
                        next = j["result"]["nextPageCursor"].get<std::string>();
                    }

                    pages++;
                    if (next.empty()) break;
                    cursor = next;

                    if (pages > 50) break;
                } catch (...) {
                    std::cout << resp << "\n";
                    break;
                }
            }

            nlohmann::json out;
            out["symbol"] = symbol;
            out["window_minutes"] = minutes;
            out["pages"] = pages;
            out["fetched"] = fetched;
            out["appended"] = appended;
            out["duplicates_or_skipped"] = dupes;
            out["ledger_path"] = LEDGER_PATH;
            out["seen_exec_ids"] = (long long)g_seen_exec_ids.size();

            std::cout << out.dump() << "\n";
            continue;
        }

        // ---- sync_fund ----
        if (cmd == "sync_fund") {
            std::string category, symbol;
            int minutes;
            std::cin >> category >> symbol >> minutes;

            if (!bybit.ready()) {
                std::cout << "Set BYBIT_API_KEY and BYBIT_API_SECRET first.\n";
                continue;
            }

            long long now = now_ms_local();
            long long start = now - (long long)minutes * 60 * 1000;

            std::string cursor;
            int pages = 0;
            long long fetched = 0;
            long long appended = 0;
            long long dupes = 0;

            while (true) {
                std::string resp = bybit.get_transaction_log(category, symbol, start, now, cursor, "SETTLEMENT");

                try {
                    auto j = nlohmann::json::parse(resp);
                    if (!j.contains("retCode") || !j["retCode"].is_number() || j["retCode"].get<int>() != 0) {
                        std::cout << resp << "\n";
                        break;
                    }

                    if (j.contains("result") && j["result"].contains("list") && j["result"]["list"].is_array()) {
                        for (auto &e : j["result"]["list"]) {
                            if (e.contains("symbol") && e["symbol"].is_string()) {
                                if (e["symbol"].get<std::string>() != symbol) continue;
                            }
                            if (!e.contains("funding")) continue;

                            fetched++;
                            bool ok = append_funding_to_ledger(e, category, symbol);
                            if (ok) appended++;
                            else dupes++;
                        }
                    }

                    std::string next;
                    if (j.contains("result") && j["result"].contains("nextPageCursor") && j["result"]["nextPageCursor"].is_string()) {
                        next = j["result"]["nextPageCursor"].get<std::string>();
                    }

                    pages++;
                    if (next.empty()) break;
                    cursor = next;

                    if (pages > 50) break;
                } catch (...) {
                    std::cout << resp << "\n";
                    break;
                }
            }

            nlohmann::json out;
            out["symbol"] = symbol;
            out["window_minutes"] = minutes;
            out["pages"] = pages;
            out["fetched"] = fetched;
            out["appended"] = appended;
            out["duplicates_or_skipped"] = dupes;
            out["ledger_path"] = FUND_LEDGER_PATH;
            out["seen_fund_ids"] = (long long)g_seen_fund_ids.size();

            std::cout << out.dump() << "\n";
            continue;
        }

        if (cmd == "pnlwL") {
            std::string symbol; int minutes;
            std::cin >> symbol >> minutes;

            long long end_ms = now_ms_local();
            long long start_ms = end_ms - (long long)minutes * 60 * 1000;

            auto out = sum_exec_fees_from_ledger(symbol, start_ms, end_ms);
            out["window_minutes"] = minutes;
            std::cout << out.dump() << "\n";
            continue;
        }

        if (cmd == "fundwL") {
            std::string symbol; int minutes;
            std::cin >> symbol >> minutes;

            long long end_ms = now_ms_local();
            long long start_ms = end_ms - (long long)minutes * 60 * 1000;

            auto out = sum_funding_from_ledger(symbol, start_ms, end_ms);
            out["window_minutes"] = minutes;
            std::cout << out.dump() << "\n";
            continue;
        }

        if (cmd == "pnlxL") {
            std::string category, symbol;
            int minutes;
            std::cin >> category >> symbol >> minutes;

            if (!bybit.ready()) {
                std::cout << "Set BYBIT_API_KEY and BYBIT_API_SECRET first.\n";
                continue;
            }

            long long end_ms = now_ms_local();
            long long start_ms = end_ms - (long long)minutes * 60 * 1000;

            double uPnL = 0.0, cumRealisedPnl = 0.0, size = 0.0, avg = 0.0, mark = 0.0;
            std::string side;

            {
                std::string resp = bybit.get_positions(category, symbol);
                try {
                    auto j = nlohmann::json::parse(resp);
                    if (!j.contains("retCode") || j["retCode"].get<int>() != 0) {
                        std::cout << resp << "\n";
                        continue;
                    }
                    auto &list = j["result"]["list"];
                    if (!list.is_array() || list.empty()) {
                        nlohmann::json out;
                        out["symbol"] = symbol;
                        out["error"] = "no position data";
                        std::cout << out.dump() << "\n";
                        continue;
                    }

                    auto &p = list[0];
                    side = p.value("side", "");
                    size = get_num_safe(p.value("size", "0"));
                    avg  = get_num_safe(p.value("avgPrice", "0"));
                    mark = get_num_safe(p.value("markPrice", "0"));
                    uPnL = get_num_safe(p.value("unrealisedPnl", "0"));
                    cumRealisedPnl = get_num_safe(p.value("cumRealisedPnl", "0"));
                } catch (...) {
                    std::cout << resp << "\n";
                    continue;
                }
            }

            auto feesJ = sum_exec_fees_from_ledger(symbol, start_ms, end_ms);
            auto fundJ = sum_funding_from_ledger(symbol, start_ms, end_ms);

            double fees = feesJ.value("fees", 0.0);
            double funding = fundJ.value("funding", 0.0);

            double net_window_cashflow = fees + funding;
            double net_mixed_view = uPnL + cumRealisedPnl + fees + funding;

            nlohmann::json out;
            out["symbol"] = symbol;
            out["side"] = side;
            out["size"] = size;
            out["avgPrice"] = avg;
            out["markPrice"] = mark;
            out["uPnL"] = uPnL;
            out["cumRealisedPnl"] = cumRealisedPnl;

            out["window_minutes"] = minutes;

            out["fees_ledger"] = fees;
            out["fees_ledger_exec_count"] = feesJ.value("exec_count", 0);

            out["funding_ledger"] = funding;
            out["funding_ledger_event_count"] = fundJ.value("event_count", 0);

            out["net_window_cashflow_ledger"] = net_window_cashflow;
            out["net_mixed_view_ledger"] = net_mixed_view;

            std::cout << out.dump() << "\n";
            continue;
        }

        // =========================
        // ======= YOUR TAIL =======
        // =========================

        if (cmd == "reconcile") {
            std::string category, symbol;
            int minutes;
            std::cin >> category >> symbol >> minutes;

            if (!bybit.ready()) {
                std::cout << "Set BYBIT_API_KEY and BYBIT_API_SECRET first.\n";
                continue;
            }

            long long end_ms = now_ms_local();
            long long start_ms = end_ms - (long long)minutes * 60 * 1000;

            double api_fees = 0.0;
            long long api_exec_count = 0;
            int api_fee_pages = 0;
            {
                std::string cursor;
                while (true) {
                    std::string resp = bybit.get_executions(category, symbol, start_ms, cursor);
                    try {
                        auto j = nlohmann::json::parse(resp);
                        if (!j.contains("retCode") || j["retCode"].get<int>() != 0) break;

                        if (j.contains("result") && j["result"].contains("list") && j["result"]["list"].is_array()) {
                            for (auto &e : j["result"]["list"]) {
                                if (e.contains("execFee")) api_fees += get_num_safe(e["execFee"]);
                                api_exec_count++;
                            }
                        }

                        std::string next;
                        if (j.contains("result") && j["result"].contains("nextPageCursor") && j["result"]["nextPageCursor"].is_string())
                            next = j["result"]["nextPageCursor"].get<std::string>();

                        api_fee_pages++;
                        if (next.empty()) break;
                        cursor = next;

                        if (api_fee_pages > 50) break;
                    } catch (...) { break; }
                }
            }

            double api_funding = 0.0;
            long long api_fund_count = 0;
            int api_fund_pages = 0;
            {
                std::string cursor;
                while (true) {
                    std::string resp = bybit.get_transaction_log(category, symbol, start_ms, end_ms, cursor, "SETTLEMENT");
                    try {
                        auto j = nlohmann::json::parse(resp);
                        if (!j.contains("retCode") || j["retCode"].get<int>() != 0) break;

                        if (j.contains("result") && j["result"].contains("list") && j["result"]["list"].is_array()) {
                            for (auto &e : j["result"]["list"]) {
                                if (e.contains("symbol") && e["symbol"].is_string()) {
                                    if (e["symbol"].get<std::string>() != symbol) continue;
                                }
                                if (e.contains("funding")) {
                                    api_funding += get_num_safe(e["funding"]);
                                    api_fund_count++;
                                }
                            }
                        }

                        std::string next;
                        if (j.contains("result") && j["result"].contains("nextPageCursor") && j["result"]["nextPageCursor"].is_string())
                            next = j["result"]["nextPageCursor"].get<std::string>();

                        api_fund_pages++;
                        if (next.empty()) break;
                        cursor = next;

                        if (api_fund_pages > 50) break;
                    } catch (...) { break; }
                }
            }

            auto ledFees = sum_exec_fees_from_ledger(symbol, start_ms, end_ms);
            auto ledFund = sum_funding_from_ledger(symbol, start_ms, end_ms);

            double led_fees = ledFees.value("fees", 0.0);
            long long led_exec_count = ledFees.value("exec_count", 0);

            double led_funding = ledFund.value("funding", 0.0);
            long long led_fund_count = ledFund.value("event_count", 0);

            nlohmann::json out;
            out["symbol"] = symbol;
            out["window_minutes"] = minutes;

            out["api_fees"] = api_fees;
            out["api_exec_count"] = api_exec_count;
            out["api_fee_pages"] = api_fee_pages;

            out["ledger_fees"] = led_fees;
            out["ledger_exec_count"] = led_exec_count;

            out["delta_fees"] = (led_fees - api_fees);
            out["delta_exec_count"] = (led_exec_count - api_exec_count);

            out["api_funding"] = api_funding;
            out["api_funding_count"] = api_fund_count;
            out["api_funding_pages"] = api_fund_pages;

            out["ledger_funding"] = led_funding;
            out["ledger_funding_count"] = led_fund_count;

            out["delta_funding"] = (led_funding - api_funding);
            out["delta_funding_count"] = (led_fund_count - api_fund_count);

            out["ok"] = (std::abs(out["delta_fees"].get<double>()) < 1e-9) &&
                        (std::abs(out["delta_funding"].get<double>()) < 1e-9) &&
                        (out["delta_exec_count"].get<long long>() == 0) &&
                        (out["delta_funding_count"].get<long long>() == 0);

            std::cout << out.dump() << "\n";
            continue;
        }

        // ---- sync_trades ----
        if (cmd == "sync_trades") {
            std::string category, symbol;
            std::cin >> category >> symbol;

            auto execs = load_execs_from_ledger(category, symbol);

            std::vector<Lot> lots;
            long long closed_events = 0;
            double gross_sum = 0.0;
            double net_sum = 0.0;

            for (auto& e : execs) {
                process_fill_and_emit_trades(category, symbol, e, lots, closed_events, gross_sum, net_sum);
            }

            double long_qty = 0.0, short_qty = 0.0;
            double long_notional = 0.0, short_notional = 0.0;
            for (auto& l : lots) {
                if (l.side == LotSide::LONG)  { long_qty += l.qty;  long_notional += l.qty * l.px; }
                if (l.side == LotSide::SHORT) { short_qty += l.qty; short_notional += l.qty * l.px; }
            }

            nlohmann::json out;
            out["symbol"] = symbol;
            out["category"] = category;
            out["execs_loaded"] = (long long)execs.size();

            out["trade_close_events_appended_or_existing"] = closed_events;
            out["gross_realized_sum_new_appends"] = gross_sum;
            out["net_realized_sum_new_appends"] = net_sum;

            out["open_long_qty"] = long_qty;
            out["open_short_qty"] = short_qty;
            out["open_long_avg"] = (long_qty > 0 ? long_notional / long_qty : 0.0);
            out["open_short_avg"] = (short_qty > 0 ? short_notional / short_qty : 0.0);

            out["trade_ledger_path"] = TRADE_LEDGER_PATH;
            out["seen_trade_ids"] = (long long)g_seen_trade_ids.size();

            std::cout << out.dump() << "\n";
            continue;
        }

        // ---- realwL ----
        if (cmd == "realwL") {
            std::string symbol; int minutes;
            std::cin >> symbol >> minutes;

            long long end_ms = now_ms_local();
            long long start_ms = end_ms - (long long)minutes * 60 * 1000;

            auto out = sum_realized_from_trade_ledger(symbol, start_ms, end_ms);
            out["window_minutes"] = minutes;
            std::cout << out.dump() << "\n";
            continue;
        }

        // ---- lotsL ----
        if (cmd == "lotsL") {
            std::string category, symbol;
            std::cin >> category >> symbol;

            auto execs = load_execs_from_ledger(category, symbol);
            std::vector<Lot> lots;

            long long dummy_closed = 0;
            double dummy_gross = 0.0, dummy_net = 0.0;

            for (auto& e : execs) {
                process_fill_and_emit_trades(category, symbol, e, lots, dummy_closed, dummy_gross, dummy_net);
            }

            nlohmann::json out;
            out["symbol"] = symbol;
            out["category"] = category;
            out["execs_loaded"] = (long long)execs.size();

            nlohmann::json arr = nlohmann::json::array();
            for (auto& l : lots) {
                nlohmann::json j;
                j["side"] = lot_side_str(l.side);
                j["qty"] = l.qty;
                j["px"] = l.px;
                j["open_execId"] = l.execId;
                j["open_ts_ms"] = l.ts_ms;
                j["open_fee_rem"] = l.fee_rem;
                arr.push_back(j);
            }
            out["open_lots"] = arr;
            std::cout << out.dump() << "\n";
            continue;
        }

        // ---- pnlFULL ----
        if (cmd == "pnlFULL") {
            std::string category, symbol;
            int minutes;
            std::cin >> category >> symbol >> minutes;

            if (!bybit.ready()) {
                std::cout << "Set BYBIT_API_KEY and BYBIT_API_SECRET first.\n";
                continue;
            }

            long long end_ms = now_ms_local();
            long long start_ms = end_ms - (long long)minutes * 60 * 1000;

            // --- Live position snapshot (API) ---
            double uPnL = 0.0, cumRealisedPnl_api = 0.0, size = 0.0, avg = 0.0, mark = 0.0;
            std::string side;

            {
                std::string resp = bybit.get_positions(category, symbol);
                try {
                    auto j = nlohmann::json::parse(resp);
                    if (!j.contains("retCode") || j["retCode"].get<int>() != 0) {
                        std::cout << resp << "\n";
                        continue;
                    }
                    auto &list = j["result"]["list"];
                    if (!list.is_array() || list.empty()) {
                        nlohmann::json out;
                        out["symbol"] = symbol;
                        out["error"] = "no position data";
                        std::cout << out.dump() << "\n";
                        continue;
                    }
                    auto &p = list[0];

                    side = p.value("side", "");
                    size = get_num_safe(p.value("size", "0"));
                    avg  = get_num_safe(p.value("avgPrice", "0"));
                    mark = get_num_safe(p.value("markPrice", "0"));
                    uPnL = get_num_safe(p.value("unrealisedPnl", "0"));
                    cumRealisedPnl_api = get_num_safe(p.value("cumRealisedPnl", "0"));
                } catch (...) {
                    std::cout << resp << "\n";
                    continue;
                }
            }

            // --- Ledger components ---
            auto feesJ    = sum_exec_fees_from_ledger(symbol, start_ms, end_ms);
            auto fundJ    = sum_funding_from_ledger(symbol, start_ms, end_ms);
            auto realJ    = sum_realized_from_trade_ledger(symbol, start_ms, end_ms);
            auto realAllJ = sum_realized_from_trade_ledger_all(symbol);

            double fees = feesJ.value("fees", 0.0);
            double funding = fundJ.value("funding", 0.0);
            double realized_net_window = realJ.value("net_realized", 0.0);
            double realized_net_all    = realAllJ.value("net_realized_all", 0.0);

            double net_window_cashflow = fees + funding;
            double net_total_ledger = realized_net_all + uPnL + net_window_cashflow;

            // --- Reconcile realized vs API ---
            double delta_realized = cumRealisedPnl_api - realized_net_all;
            const double tol = 1e-6;
            bool ok_realized = (std::abs(delta_realized) < tol);

            nlohmann::json out;
            out["symbol"] = symbol;
            out["category"] = category;
            out["window_minutes"] = minutes;

            out["side"] = side;
            out["size"] = size;
            out["avgPrice"] = avg;
            out["markPrice"] = mark;

            out["uPnL_api"] = uPnL;
            out["cumRealisedPnl_api"] = cumRealisedPnl_api;

            out["fees_ledger_window"] = fees;
            out["funding_ledger_window"] = funding;

            out["realized_ledger_net_window"] = realized_net_window;
            out["realized_ledger_net_all"] = realized_net_all;
            out["ledger_trade_close_events_all"] = realAllJ.value("close_events_all", 0);

            out["net_window_cashflow_ledger"] = net_window_cashflow;
            out["net_total_ledger"] = net_total_ledger;

            out["delta_realized_api_minus_ledger_all"] = delta_realized;
            out["ok_realized"] = ok_realized;

            std::cout << out.dump() << "\n";
            continue;
        }

        // ---- buy/sell (default path) ----
        std::string category, symbol;
        double qty = 0.0;
		std::string ord = "mkt"; // default
        std::cin >> category >> symbol >> qty;
		// try read optional token without breaking next command
		if (std::cin.peek() == ' ') {
    		// eat spaces
    		while (std::cin.peek() == ' ') std::cin.get();
    		// if next is not newline, read token
    		if (std::cin.peek() != '\n' && std::cin.peek() != '\r') {
        		std::cin >> ord;
    		}
		}
		OrdType ot = parse_ordtype_or_default_mkt(ord);
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
            std::string resp;
            if (ot == OrdType::MKT) {
                resp = bybit.place_market_order(category, symbol, "Buy", qty, false /*reduceOnly*/);
            } else {
                // lmt: use best ask
                resp = bybit.place_limit_order(category, symbol, "Buy", qty, ask, "GTC", false /*reduceOnly*/);
            }
            std::cout << resp << "\n";
            try {
                auto j = nlohmann::json::parse(resp);
                if (j.contains("retCode") && j["retCode"].is_number() && j["retCode"].get<int>() == 0) {
                    auto oid = j["result"]["orderId"].get<std::string>();
                    track_order(oid, category, symbol);
                    std::cout << "[TRACK] orderId=" << oid << "\n";
                }
            } catch (...) {}
        } else if (cmd == "sell") {
            std::string resp;
            if (ot == OrdType::MKT) {
                resp = bybit.place_market_order(category, symbol, "Sell", qty, false /*reduceOnly*/);
            } else {
                // lmt: use best bid
                resp = bybit.place_limit_order(category, symbol, "Sell", qty, bid, "GTC", false /*reduceOnly*/);
            }
            std::cout << resp << "\n";
            try {
                auto j = nlohmann::json::parse(resp);
                if (j.contains("retCode") && j["retCode"].is_number() && j["retCode"].get<int>() == 0) {
                    auto oid = j["result"]["orderId"].get<std::string>();
                    track_order(oid, category, symbol);
                    std::cout << "[TRACK] orderId=" << oid << "\n";
                }
            } catch (...) {}
        } else {
            std::cout << "Unknown command.\n";
        }
    }

    // Kill-switch cancel-all before exit
    if (bybit.ready()) {
        auto open = snapshot_open_orders();
        for (auto &it : open) {
            const std::string &oid = it.first;
            const auto &info = it.second;
            std::cout << "[KILL] cancel orderId=" << oid << "\n";
            std::cout << bybit.cancel_order(info.category, info.symbol, oid) << "\n";
        }
    }

    running = false;
    try { sub.close(); } catch (...) {}
    if (rx.joinable()) rx.join();
    return 0;
}
