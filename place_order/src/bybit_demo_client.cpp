#include "bybit_demo_client.hpp"

#include <nlohmann/json.hpp>
#include <openssl/hmac.h>
#include <curl/curl.h>

#include <chrono>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <cmath>

std::string BybitDemoClient::place_limit_order(
    const std::string& category,
    const std::string& symbol,
    const std::string& side,
    double qty,
    double price,
    const std::string& tif
) {
    nlohmann::json j;
    j["category"] = category;
    j["symbol"] = symbol;
    j["side"] = side;                // "Buy" / "Sell"
    j["orderType"] = "Limit";
    j["qty"] = std::to_string(qty);
    j["price"] = std::to_string(price);
    j["timeInForce"] = tif;          // "GTC" good default

    return post("/v5/order/create", j.dump());
}

static long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

static std::string hex_encode(const unsigned char* data, size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; i++)
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i];
    return oss.str();
}

static std::string hmac_sha256_hex(const std::string& key, const std::string& msg) {
    unsigned int outlen = 0;
    unsigned char out[EVP_MAX_MD_SIZE];
    HMAC(EVP_sha256(),
         key.data(), (int)key.size(),
         (const unsigned char*)msg.data(), (int)msg.size(),
         out, &outlen);
    return hex_encode(out, outlen);
}

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

BybitDemoClient::BybitDemoClient(std::string api_key, std::string api_secret)
: api_key_(std::move(api_key)), api_secret_(std::move(api_secret)) {}

bool BybitDemoClient::ready() const {
    return !api_key_.empty() && !api_secret_.empty();
}

std::string BybitDemoClient::place_market_order(
    const std::string& category,
    const std::string& symbol,
    const std::string& side,
    double qty
) {
    nlohmann::json j;
    j["category"] = category;      // "spot" / "linear"
    j["symbol"] = symbol;
    j["side"] = side;              // "Buy" / "Sell"
    j["orderType"] = "Market";
    j["qty"] = std::to_string(qty);

    return post("/v5/order/create", j.dump());
}

std::string BybitDemoClient::post(const std::string& path, const std::string& body_json) {
    const std::string base = "https://api-demo.bybit.com";
    const std::string url = base + path;

    const char* rw = std::getenv("BYBIT_RECV_WINDOW");
    const std::string recv_window = rw ? std::string(rw) : "5000";
    const std::string ts = std::to_string(now_ms());

    // V5 signature: ts + apiKey + recvWindow + body
    const std::string payload = ts + api_key_ + recv_window + body_json;
    const std::string sig = hmac_sha256_hex(api_secret_, payload);

    CURL* curl = curl_easy_init();
    if (!curl) return "{\"error\":\"curl init failed\"}";

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("X-BAPI-API-KEY: " + api_key_).c_str());
    headers = curl_slist_append(headers, ("X-BAPI-SIGN: " + sig).c_str());
    headers = curl_slist_append(headers, ("X-BAPI-TIMESTAMP: " + ts).c_str());
    headers = curl_slist_append(headers, ("X-BAPI-RECV-WINDOW: " + recv_window).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string resp;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_json.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    // TLS verify ON
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode rc = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        return std::string("{\"error\":\"curl perform failed\",\"msg\":\"") + curl_easy_strerror(rc) + "\"}";
    }
    return resp;
}

std::string BybitDemoClient::cancel_order(
    const std::string& category,
    const std::string& symbol,
    const std::string& orderId
) {
    nlohmann::json j;
    j["category"] = category;
    j["symbol"]   = symbol;
    j["orderId"]  = orderId;

    return post("/v5/order/cancel", j.dump());
}

/*std::string BybitDemoClient::get(const std::string& path, const std::string& query_string) {
    const std::string base = "https://api-demo.bybit.com";
    const std::string url  = base + path + (query_string.empty() ? "" : ("?" + query_string));

    const char* rw = std::getenv("BYBIT_RECV_WINDOW");
    const std::string recv_window = rw ? std::string(rw) : "10000"; // use 10s to reduce timestamp errors
    const std::string ts = std::to_string(now_ms());

    // V5 signature for GET: ts + apiKey + recvWindow + queryString
    // (Bybit docs confirm GET uses queryString, POST uses body) :contentReference[oaicite:0]{index=0}
    const std::string payload = ts + api_key_ + recv_window + query_string;
    const std::string sig = hmac_sha256_hex(api_secret_, payload);

    CURL* curl = curl_easy_init();
    if (!curl) return "{\"error\":\"curl init failed\"}";

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("X-BAPI-API-KEY: " + api_key_).c_str());
    headers = curl_slist_append(headers, ("X-BAPI-SIGN: " + sig).c_str());
    headers = curl_slist_append(headers, ("X-BAPI-TIMESTAMP: " + ts).c_str());
    headers = curl_slist_append(headers, ("X-BAPI-RECV-WINDOW: " + recv_window).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string resp;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode rc = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        return std::string("{\"error\":\"curl perform failed\",\"msg\":\"") + curl_easy_strerror(rc) + "\"}";
    }
    return resp;
} */

std::string BybitDemoClient::get_positions(const std::string& category, const std::string& symbol) {
    // GET /v5/position/list?category=linear&symbol=ETHUSDT :contentReference[oaicite:1]{index=1}
    std::string qs = "category=" + category + "&symbol=" + symbol;
    return get("/v5/position/list", qs);
}

double BybitDemoClient::get_position_size_linear(const std::string& symbol) {
    std::string resp = get_positions("linear", symbol);

    try {
        auto j = nlohmann::json::parse(resp);
        if (!j.contains("retCode") || j["retCode"].get<int>() != 0) return 0.0;
        if (!j.contains("result") || !j["result"].contains("list")) return 0.0;

        double net = 0.0;
        for (auto& p : j["result"]["list"]) {
            if (!p.contains("symbol")) continue;
            if (p["symbol"].get<std::string>() != symbol) continue;

            std::string side = p.value("side", ""); // "Buy" or "Sell"
            double size = 0.0;

            if (p.contains("size")) {
                if (p["size"].is_string()) size = std::stod(p["size"].get<std::string>());
                else if (p["size"].is_number()) size = p["size"].get<double>();
            }

            if (side == "Buy") net += size;       // long
            else if (side == "Sell") net -= size; // short
        }
        return net;
    } catch (...) {
        return 0.0;
    }
}

std::string BybitDemoClient::close_position_market_reduce_only(const std::string& category,
                                                               const std::string& symbol) {
    if (category != "linear") {
        return R"({"error":"close supports only category=linear (USDT Perpetual) for now"})";
    }

    double pos = get_position_size_linear(symbol);
    if (std::abs(pos) < 1e-12) {
        return R"({"ok":true,"msg":"Already flat"})";
    }

    // opposite side to close
    std::string side = (pos > 0.0) ? "Sell" : "Buy";
    double qty = std::abs(pos);

    nlohmann::json j;
    j["category"]   = category;
    j["symbol"]     = symbol;
    j["side"]       = side;
    j["orderType"]  = "Market";
    j["qty"]        = std::to_string(qty);
    j["reduceOnly"] = true; // prevents flipping to opposite position :contentReference[oaicite:2]{index=2}

    // If hedge mode requires positionIdx, allow via env var
    if (const char* pidx = std::getenv("BYBIT_POSITION_IDX")) {
        j["positionIdx"] = std::stoi(pidx);
    }

    return post("/v5/order/create", j.dump());
}

std::string BybitDemoClient::get(const std::string& path, const std::string& query_string) {
    const std::string base = "https://api-demo.bybit.com";
    const std::string url  = base + path + (query_string.empty() ? "" : ("?" + query_string));

    const char* rw = std::getenv("BYBIT_RECV_WINDOW");
    const std::string recv_window = rw ? std::string(rw) : "10000";
    const std::string ts = std::to_string(now_ms());

    // V5 signature for GET: ts + apiKey + recvWindow + queryString
    const std::string payload = ts + api_key_ + recv_window + query_string;
    const std::string sig = hmac_sha256_hex(api_secret_, payload);

    CURL* curl = curl_easy_init();
    if (!curl) return "{\"error\":\"curl init failed\"}";

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("X-BAPI-API-KEY: " + api_key_).c_str());
    headers = curl_slist_append(headers, ("X-BAPI-SIGN: " + sig).c_str());
    headers = curl_slist_append(headers, ("X-BAPI-TIMESTAMP: " + ts).c_str());
    headers = curl_slist_append(headers, ("X-BAPI-RECV-WINDOW: " + recv_window).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string resp;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // IMPORTANT: GET + capture response
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode rc = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        return std::string("{\"error\":\"curl perform failed\",\"msg\":\"") + curl_easy_strerror(rc) + "\"}";
    }
    if (http_code < 200 || http_code >= 300) {
        return std::string("{\"error\":\"http\",\"code\":") + std::to_string(http_code) +
               ",\"resp\":\"" + resp + "\"}";
    }
    return resp;
}

std::string BybitDemoClient::get_executions(const std::string& category,
                                            const std::string& symbol,
                                            long long startTimeMs,
                                            const std::string& cursor) {
    std::string qs = "category=" + category +
                     "&symbol=" + symbol +
                     "&startTime=" + std::to_string(startTimeMs);

    if (!cursor.empty()) qs += "&cursor=" + cursor;

    return get("/v5/execution/list", qs);
}

std::string BybitDemoClient::get_executions(const std::string& category,
                                            const std::string& symbol,
                                            long long startTimeMs) {
    return get_executions(category, symbol, startTimeMs, "");
}

std::string BybitDemoClient::get_transaction_log(const std::string& category,
                                                 const std::string& symbol,
                                                 long long startTimeMs,
                                                 long long endTimeMs,
                                                 const std::string& cursor,
                                                 const std::string& type) {
    // Unified account transaction log (funding is in `funding`, type=SETTLEMENT) :contentReference[oaicite:1]{index=1}
    std::string qs = "accountType=UNIFIED"
                     "&category=" + category +
                     "&type=" + type +
                     "&startTime=" + std::to_string(startTimeMs) +
                     "&endTime=" + std::to_string(endTimeMs) +
                     "&limit=50";

    // Some Bybit accounts accept symbol filter; if ignored, we filter client-side in main.cpp.
    if (!symbol.empty()) qs += "&symbol=" + symbol;

    if (!cursor.empty()) qs += "&cursor=" + cursor;

    return get("/v5/account/transaction-log", qs);
}

std::string BybitDemoClient::get_transaction_log(const std::string& category,
                                                 const std::string& symbol,
                                                 long long startTimeMs,
                                                 long long endTimeMs,
                                                 const std::string& type) {
    return get_transaction_log(category, symbol, startTimeMs, endTimeMs, "", type);
}



