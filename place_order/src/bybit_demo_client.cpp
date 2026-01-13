#include "bybit_demo_client.hpp"

#include <nlohmann/json.hpp>
#include <openssl/hmac.h>
#include <curl/curl.h>

#include <chrono>
#include <sstream>
#include <iomanip>
#include <cstdlib>

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
