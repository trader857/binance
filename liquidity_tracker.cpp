#include "features/liquidity_tracker.hpp"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

// Constructor
LiquidityTracker::LiquidityTracker(
    double buy_bucket_size_usd,
    double sell_bucket_size_usd,
    double cancel_bucket_size_usd,
    size_t depth_levels_track,
    size_t depth_levels_report,
    double tick_size)
    : buy_bucket_size_(buy_bucket_size_usd),
      sell_bucket_size_(sell_bucket_size_usd),
      cancel_bucket_size_(cancel_bucket_size_usd),
      depth_levels_track_(depth_levels_track),
      depth_levels_report_(depth_levels_report),
      tick_size_(tick_size),
      buy_accum_usd_(0.0),
      sell_accum_usd_(0.0),
      buy_bucket_buyflow_(0.0),
      buy_bucket_sellflow_(0.0),
      sell_bucket_sellflow_(0.0),
      sell_bucket_buyflow_(0.0),
      buy_start_ts_ns_(0),
      sell_start_ts_ns_(0),
      cancel_buy_accum_usd_(0.0),
      cancel_sell_accum_usd_(0.0),
      cancel_buy_bucket_total_(0.0),
      cancel_sell_bucket_total_(0.0),
      cancel_buy_start_ts_ns_(0),
      cancel_sell_start_ts_ns_(0) 
{}

// Destructor
LiquidityTracker::~LiquidityTracker() {}

// Resets all tracking counters
void LiquidityTracker::reset() {
    buy_accum_usd_ = sell_accum_usd_ = 0.0;
    buy_bucket_buyflow_ = buy_bucket_sellflow_ = 0.0;
    sell_bucket_sellflow_ = sell_bucket_buyflow_ = 0.0;
    buy_start_ts_ns_ = sell_start_ts_ns_ = 0;
    cancel_buy_accum_usd_ = cancel_sell_accum_usd_ = 0.0;
    cancel_buy_bucket_total_ = cancel_sell_bucket_total_ = 0.0;
    cancel_buy_start_ts_ns_ = cancel_sell_start_ts_ns_ = 0;
    last_bids_volume_.clear();
    last_asks_volume_.clear();
}

// Setters for callbacks
void LiquidityTracker::setBuyBucketCallback(BucketSpeedCallback cb) { buy_bucket_cb_ = cb; }
void LiquidityTracker::setSellBucketCallback(BucketSpeedCallback cb) { sell_bucket_cb_ = cb; }
void LiquidityTracker::setCancelBuyBucketCallback(CancelBucketCallback cb) { cancel_buy_cb_ = cb; }
void LiquidityTracker::setCancelSellBucketCallback(CancelBucketCallback cb) { cancel_sell_cb_ = cb; }
void LiquidityTracker::setLiquidityChangeCallback(LiquidityChangeCallback cb) { liquidity_change_cb_ = cb; }
void LiquidityTracker::setTickSize(double tick_size) { tick_size_ = tick_size; }

// Rounds a price to the nearest tick size
double LiquidityTracker::round_price(double price) const {
    if (tick_size_ <= 0.0) return price;
    return std::round(price / tick_size_) * tick_size_;
}

// Processes a trade update
void LiquidityTracker::onTrade(const TradeMessageBinary& trade) {
    double notional = trade.price * trade.quantity;
    uint64_t ts_ns = trade.timestamp_ns;

    std::cout << "[DEBUG] onTrade: Processing trade message - Price: " << trade.price
              << ", Quantity: " << trade.quantity
              << ", Notional: " << notional
              << ", IsBuy: " << trade.is_buy() << std::endl;

    if (trade.is_buy()) {
        if (buy_start_ts_ns_ == 0) buy_start_ts_ns_ = ts_ns;
        buy_accum_usd_ += notional;
        buy_bucket_buyflow_ += notional;

        if (buy_accum_usd_ >= buy_bucket_size_) {
            double ratio = (buy_bucket_sellflow_ > 0.0)
                ? buy_bucket_buyflow_ / buy_bucket_sellflow_
                : (buy_bucket_buyflow_ > 0.0 ? 1000.0 : 1.0);

            if (buy_bucket_cb_) {
                buy_bucket_cb_(true, ts_ns - buy_start_ts_ns_, buy_bucket_size_, ratio);
                std::cout << "[DEBUG] Buy bucket callback triggered. Ratio: " << ratio << std::endl;
            }

            buy_accum_usd_ = 0.0;
            buy_bucket_buyflow_ = 0.0;
            buy_bucket_sellflow_ = 0.0;
            buy_start_ts_ns_ = 0;
        }
    } else {
        if (sell_start_ts_ns_ == 0) sell_start_ts_ns_ = ts_ns;
        sell_accum_usd_ += notional;
        sell_bucket_sellflow_ += notional;

        if (sell_accum_usd_ >= sell_bucket_size_) {
            double ratio = (sell_bucket_buyflow_ > 0.0)
                ? sell_bucket_sellflow_ / sell_bucket_buyflow_
                : (sell_bucket_sellflow_ > 0.0 ? 1000.0 : 1.0);

            if (sell_bucket_cb_) {
                sell_bucket_cb_(false, ts_ns - sell_start_ts_ns_, sell_bucket_size_, ratio);
                std::cout << "[DEBUG] Sell bucket callback triggered. Ratio: " << ratio << std::endl;
            }

            sell_accum_usd_ = 0.0;
            sell_bucket_sellflow_ = 0.0;
            sell_bucket_buyflow_ = 0.0;
            sell_start_ts_ns_ = 0;
        }
    }

    if (trade.is_buy()) {
        sell_bucket_buyflow_ += notional;
    } else {
        buy_bucket_sellflow_ += notional;
    }
}

// Processes an order book update
void LiquidityTracker::onOrderBookUpdate(
    uint64_t timestamp_ns,
    const std::vector<OrderBookLevel>& bids,
    const std::vector<OrderBookLevel>& asks)
{
    std::cout << "[DEBUG] onOrderBookUpdate called. Timestamp: " << timestamp_ns
              << ", Bids size: " << bids.size()
              << ", Asks size: " << asks.size() << std::endl;

    std::map<double, double> prev_bids = last_bids_volume_;
    std::map<double, double> prev_asks = last_asks_volume_;
    last_bids_volume_.clear();
    last_asks_volume_.clear();

    for (size_t i = 0; i < std::min(depth_levels_report_, bids.size()); ++i) {
        double rounded_price = round_price(bids[i].price);
        last_bids_volume_[rounded_price] += bids[i].volume;
    }
    for (size_t i = 0; i < std::min(depth_levels_report_, asks.size()); ++i) {
        double rounded_price = round_price(asks[i].price);
        last_asks_volume_[rounded_price] += asks[i].volume;
    }

    detectLiquidityChanges(timestamp_ns, prev_bids, prev_asks);

    // Debug: Print updated order book
    std::cout << "[DEBUG] Updated Bids:" << std::endl;
    for (const auto& bid : last_bids_volume_) {
        std::cout << "Price: " << bid.first << ", Volume: " << bid.second << std::endl;
    }

    std::cout << "[DEBUG] Updated Asks:" << std::endl;
    for (const auto& ask : last_asks_volume_) {
        std::cout << "Price: " << ask.first << ", Volume: " << ask.second << std::endl;
    }
}

// Detects liquidity changes between previous and current order books
void LiquidityTracker::detectLiquidityChanges(
    uint64_t timestamp_ns,
    const std::map<double, double>& prev_bids,
    const std::map<double, double>& prev_asks)
{
    for (const auto& entry : last_bids_volume_) {
        auto price = entry.first;
        auto volume = entry.second;
        double prev_vol = prev_bids.count(price) ? prev_bids.at(price) : 0.0;
        double delta = volume - prev_vol;
        if (delta != 0.0 && liquidity_change_cb_) {
            LiquidityChange change{price, delta, timestamp_ns, true};
            liquidity_change_cb_(change);
        }
    }

    for (const auto& entry : last_asks_volume_) {
        auto price = entry.first;
        auto volume = entry.second;
        double prev_vol = prev_asks.count(price) ? prev_asks.at(price) : 0.0;
        double delta = volume - prev_vol;
        if (delta != 0.0 && liquidity_change_cb_) {
            LiquidityChange change{price, delta, timestamp_ns, false};
            liquidity_change_cb_(change);
        }
    }
}
