#ifndef BINANCE_CONNECTOR_HPP
#define BINANCE_CONNECTOR_HPP

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>

struct BinanceTrade {
    double price;
    double quantity;
    bool is_buy;
    uint64_t timestamp;
};

struct BinanceLevel {
    double price;
    double quantity;
};

struct BinanceDepthUpdate {
    std::vector<BinanceLevel> bids;
    std::vector<BinanceLevel> asks;
    uint64_t timestamp;
};

class BinanceConnector {
public:
    BinanceConnector();
    ~BinanceConnector();

    void start();
    void stop();

    void set_trade_callback(std::function<void(const BinanceTrade&)> cb);
    void set_depth_callback(std::function<void(const BinanceDepthUpdate&)> cb);

private:
    std::thread ws_thread;
    std::atomic<bool> running;

    std::function<void(const BinanceTrade&)> trade_cb;
    std::function<void(const BinanceDepthUpdate&)> depth_cb;

    void run();
};

#endif // BINANCE_CONNECTOR_HPP
