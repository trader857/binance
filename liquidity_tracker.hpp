#pragma once

#include <cstdint>
#include <map>
#include <vector>
#include <deque>
#include <chrono>
#include <functional>
#include <mutex>
#include <atomic>
#include "core/serialization.hpp"

struct OrderBookLevel {
    double price;
    double volume;
};

struct LiquidityChange {
    double price;
    double volume_delta;
    uint64_t timestamp_ns;
    bool is_bid;
};

class LiquidityTracker {
public:
    using BucketSpeedCallback = std::function<void(bool is_buy, uint64_t duration_ns, double bucket_size, double flow_ratio)>;
    using CancelBucketCallback = std::function<void(bool is_buy, uint64_t duration_ns, double bucket_size, double cancel_ratio)>;
    using LiquidityChangeCallback = std::function<void(const LiquidityChange& change)>;

    LiquidityTracker(double buy_bucket_size_usd = 1000000.0,
                     double sell_bucket_size_usd = 1000000.0,
                     double cancel_bucket_size_usd = 500000.0,
                     size_t depth_levels_track = 30,
                     size_t depth_levels_report = 20,
                     double tick_size = 0.01);

    ~LiquidityTracker();

    void onOrderBookUpdate(
        uint64_t timestamp_ns,
        const std::vector<OrderBookLevel>& bids,
        const std::vector<OrderBookLevel>& asks);

    void onTrade(const TradeMessageBinary& trade);

    void setBuyBucketCallback(BucketSpeedCallback cb);
    void setSellBucketCallback(BucketSpeedCallback cb);
    void setCancelBuyBucketCallback(CancelBucketCallback cb);
    void setCancelSellBucketCallback(CancelBucketCallback cb);
    void setLiquidityChangeCallback(LiquidityChangeCallback cb);

    void setTickSize(double tick_size);

    void reset();

    // For testing: direct cancel volume simulation
    void processCancelVolume(bool is_buy, double cancel_volume, uint64_t ts_ns);

private:
    double round_price(double price) const;

    // Config
    double buy_bucket_size_;
    double sell_bucket_size_;
    double cancel_bucket_size_;
    size_t depth_levels_track_;
    size_t depth_levels_report_;
    double tick_size_;

    // State
    std::map<double, double> last_bids_volume_;
    std::map<double, double> last_asks_volume_;

    // Buy/Sell bucket tracking
    double buy_accum_usd_;
    double sell_accum_usd_;
    double buy_bucket_buyflow_;
    double buy_bucket_sellflow_;
    double sell_bucket_sellflow_;
    double sell_bucket_buyflow_;
    uint64_t buy_start_ts_ns_;
    uint64_t sell_start_ts_ns_;

    // Cancel bucket tracking
    double cancel_buy_accum_usd_;
    double cancel_sell_accum_usd_;
    double cancel_buy_bucket_total_;
    double cancel_sell_bucket_total_;
    uint64_t cancel_buy_start_ts_ns_;
    uint64_t cancel_sell_start_ts_ns_;

    // Callbacks
    BucketSpeedCallback buy_bucket_cb_;
    BucketSpeedCallback sell_bucket_cb_;
    CancelBucketCallback cancel_buy_cb_;
    CancelBucketCallback cancel_sell_cb_;
    LiquidityChangeCallback liquidity_change_cb_;

    void detectLiquidityChanges(
        uint64_t timestamp_ns,
        const std::map<double, double>& prev_bids,
        const std::map<double, double>& prev_asks);

    void processCancelVolumeInternal(bool is_buy, double cancel_volume, uint64_t timestamp_ns);
};
