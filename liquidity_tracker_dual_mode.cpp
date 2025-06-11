#include "liquidity_tracker.hpp"
#include <iostream>
#include <iomanip>
#include <cmath>
#include <chrono>
#include <sstream>

// Helper function to format timestamp
std::string format_timestamp(uint64_t timestamp_ns) {
    auto timestamp_ms = timestamp_ns / 1000000;
    auto time_t_sec = timestamp_ms / 1000;
    auto ms_part = timestamp_ms % 1000;
    
    std::time_t time = static_cast<std::time_t>(time_t_sec);
    std::tm* tm_utc = std::gmtime(&time);
    
    std::stringstream ss;
    ss << std::put_time(tm_utc, "%Y-%m-%d %H:%M:%S");
    ss << "." << std::setfill('0') << std::setw(3) << ms_part;
    return ss.str();
}

LiquidityTracker::LiquidityTracker(double buy_bucket_size_usd,
                                   double sell_bucket_size_usd,
                                   double cancel_bucket_size_usd,
                                   size_t depth_levels_track,
                                   size_t depth_levels_report,
                                   double tick_size)
    : buy_bucket_size_(buy_bucket_size_usd)
    , sell_bucket_size_(sell_bucket_size_usd)
    , cancel_bucket_size_(cancel_bucket_size_usd)
    , depth_levels_track_(depth_levels_track)
    , depth_levels_report_(depth_levels_report)
    , tick_size_(tick_size)
    // Trade-based buckets
    , buy_accum_usd_(0.0)
    , sell_accum_usd_(0.0)
    , buy_bucket_buyflow_(0.0)
    , buy_bucket_sellflow_(0.0)
    , sell_bucket_sellflow_(0.0)
    , sell_bucket_buyflow_(0.0)
    , buy_start_ts_ns_(0)
    , sell_start_ts_ns_(0)
    // Order flow buckets (new)
    , order_buy_accum_usd_(0.0)
    , order_sell_accum_usd_(0.0)
    , order_buy_start_ts_ns_(0)
    , order_sell_start_ts_ns_(0)
    // Cancel buckets
    , cancel_buy_accum_usd_(0.0)
    , cancel_sell_accum_usd_(0.0)
    , cancel_buy_bucket_total_(0.0)
    , cancel_sell_bucket_total_(0.0)
    , cancel_buy_start_ts_ns_(0)
    , cancel_sell_start_ts_ns_(0)
{
}

LiquidityTracker::~LiquidityTracker() {
}

// MODE 1: Order Book Liquidity Changes (Order Flow)
void LiquidityTracker::onOrderBookUpdate(
    uint64_t timestamp_ns,
    const std::vector<OrderBookLevel>& bids,
    const std::vector<OrderBookLevel>& asks) {
    
    // Store previous state for change detection
    std::map<double, double> prev_bids = last_bids_volume_;
    std::map<double, double> prev_asks = last_asks_volume_;
    
    // Update current state
    last_bids_volume_.clear();
    last_asks_volume_.clear();
    
    // Process bids (buy side)
    for (size_t i = 0; i < std::min(bids.size(), depth_levels_track_); ++i) {
        double rounded_price = round_price(bids[i].price);
        last_bids_volume_[rounded_price] = bids[i].volume;
    }
    
    // Process asks (sell side)
    for (size_t i = 0; i < std::min(asks.size(), depth_levels_track_); ++i) {
        double rounded_price = round_price(asks[i].price);
        last_asks_volume_[rounded_price] = asks[i].volume;
    }
    
    // Detect order flow changes and cancellations
    detectLiquidityChanges(timestamp_ns, prev_bids, prev_asks);
}

// MODE 2: Trade-Based Liquidity Consumption (Actual Execution)
void LiquidityTracker::onTrade(const TradeMessageBinary& trade) {
    double trade_value_usd = trade.price * trade.quantity;
    bool is_buy = trade.is_buy();
    
    std::cout << "[" << format_timestamp(trade.timestamp_ns) << "] "
              << "[TRADE EXECUTION] " << (is_buy ? "BUY" : "SELL") << " $" 
              << std::fixed << std::setprecision(2) << trade_value_usd 
              << " at $" << std::setprecision(2) << trade.price << std::endl;
    
    // MODE 2: Track actual trade execution buckets
    if (is_buy) {
        // BUY trade consumes ASK liquidity
        if (buy_start_ts_ns_ == 0) {
            buy_start_ts_ns_ = trade.timestamp_ns;
        }
        
        buy_accum_usd_ += trade_value_usd;
        buy_bucket_buyflow_ += trade_value_usd;
        
        // Check if trade-based buy bucket is full
        if (buy_accum_usd_ >= buy_bucket_size_) {
            uint64_t duration_ns = trade.timestamp_ns - buy_start_ts_ns_;
            double flow_ratio = buy_bucket_buyflow_ / (buy_bucket_buyflow_ + buy_bucket_sellflow_);
            
            if (buy_bucket_cb_) {
                buy_bucket_cb_(true, duration_ns, buy_bucket_size_, flow_ratio);
            }
            
            // Reset trade-based buy bucket
            buy_accum_usd_ = 0.0;
            buy_bucket_buyflow_ = 0.0;
            buy_bucket_sellflow_ = 0.0;
            buy_start_ts_ns_ = 0;
        }
    } else {
        // SELL trade consumes BID liquidity
        if (sell_start_ts_ns_ == 0) {
            sell_start_ts_ns_ = trade.timestamp_ns;
        }
        
        sell_accum_usd_ += trade_value_usd;
        sell_bucket_sellflow_ += trade_value_usd;
        
        // Check if trade-based sell bucket is full
        if (sell_accum_usd_ >= sell_bucket_size_) {
            uint64_t duration_ns = trade.timestamp_ns - sell_start_ts_ns_;
            double flow_ratio = sell_bucket_sellflow_ / (sell_bucket_sellflow_ + sell_bucket_buyflow_);
            
            if (sell_bucket_cb_) {
                sell_bucket_cb_(false, duration_ns, sell_bucket_size_, flow_ratio);
            }
            
            // Reset trade-based sell bucket
            sell_accum_usd_ = 0.0;
            sell_bucket_sellflow_ = 0.0;
            sell_bucket_buyflow_ = 0.0;
            sell_start_ts_ns_ = 0;
        }
    }
}

void LiquidityTracker::setBuyBucketCallback(BucketSpeedCallback cb) {
    buy_bucket_cb_ = cb;
}

void LiquidityTracker::setSellBucketCallback(BucketSpeedCallback cb) {
    sell_bucket_cb_ = cb;
}

void LiquidityTracker::setCancelBuyBucketCallback(CancelBucketCallback cb) {
    cancel_buy_cb_ = cb;
}

void LiquidityTracker::setCancelSellBucketCallback(CancelBucketCallback cb) {
    cancel_sell_cb_ = cb;
}

void LiquidityTracker::setLiquidityChangeCallback(LiquidityChangeCallback cb) {
    liquidity_change_cb_ = cb;
}

// NEW: Callbacks for order flow buckets
void LiquidityTracker::setOrderFlowBuyCallback(BucketSpeedCallback cb) {
    order_flow_buy_cb_ = cb;
}

void LiquidityTracker::setOrderFlowSellCallback(BucketSpeedCallback cb) {
    order_flow_sell_cb_ = cb;
}

void LiquidityTracker::setTickSize(double tick_size) {
    tick_size_ = tick_size;
}

void LiquidityTracker::reset() {
    // Trade-based buckets
    buy_accum_usd_ = 0.0;
    sell_accum_usd_ = 0.0;
    buy_bucket_buyflow_ = 0.0;
    buy_bucket_sellflow_ = 0.0;
    sell_bucket_sellflow_ = 0.0;
    sell_bucket_buyflow_ = 0.0;
    buy_start_ts_ns_ = 0;
    sell_start_ts_ns_ = 0;
    
    // Order flow buckets
    order_buy_accum_usd_ = 0.0;
    order_sell_accum_usd_ = 0.0;
    order_buy_start_ts_ns_ = 0;
    order_sell_start_ts_ns_ = 0;
    
    // Cancel buckets
    cancel_buy_accum_usd_ = 0.0;
    cancel_sell_accum_usd_ = 0.0;
    cancel_buy_bucket_total_ = 0.0;
    cancel_sell_bucket_total_ = 0.0;
    cancel_buy_start_ts_ns_ = 0;
    cancel_sell_start_ts_ns_ = 0;
    
    last_bids_volume_.clear();
    last_asks_volume_.clear();
}

void LiquidityTracker::processCancelVolume(bool is_buy, double cancel_volume, uint64_t ts_ns) {
    processCancelVolumeInternal(is_buy, cancel_volume, ts_ns);
}

double LiquidityTracker::round_price(double price) const {
    if (tick_size_ <= 0.0) return price;
    return std::round(price / tick_size_) * tick_size_;
}

// DUAL MODE: Detect both order flow changes AND cancellations
void LiquidityTracker::detectLiquidityChanges(
    uint64_t timestamp_ns,
    const std::map<double, double>& prev_bids,
    const std::map<double, double>& prev_asks) {
    
    // MODE 1: Track order flow changes (additions/removals)
    double total_bid_additions = 0.0;
    double total_bid_removals = 0.0;
    double total_ask_additions = 0.0;
    double total_ask_removals = 0.0;
    
    // Analyze bid changes
    for (const auto& [price, volume] : last_bids_volume_) {
        auto prev_it = prev_bids.find(price);
        double prev_volume = (prev_it != prev_bids.end()) ? prev_it->second : 0.0;
        
        if (std::abs(volume - prev_volume) > 1e-8) {
            double volume_delta = volume - prev_volume;
            double value_delta = volume_delta * price;
            
            if (volume_delta > 0) {
                // Order addition
                total_bid_additions += value_delta;
                std::cout << "[" << format_timestamp(timestamp_ns) << "] "
                          << "[ORDER FLOW] BID ADD $" << std::fixed << std::setprecision(2) << value_delta
                          << " at $" << std::setprecision(2) << price << std::endl;
            } else {
                // Order removal/cancellation
                total_bid_removals += std::abs(value_delta);
                
                // Large removals might be cancellations
                if (volume_delta < -prev_volume * 0.3 && prev_volume > 0) {
                    std::cout << "[" << format_timestamp(timestamp_ns) << "] "
                              << "[CANCEL DETECTED] BID at $" << std::fixed << std::setprecision(2) << price
                              << ", cancelled: $" << std::setprecision(2) << std::abs(value_delta) << std::endl;
                    processCancelVolumeInternal(true, std::abs(value_delta), timestamp_ns);
                } else {
                    std::cout << "[" << format_timestamp(timestamp_ns) << "] "
                              << "[ORDER FLOW] BID REMOVE $" << std::fixed << std::setprecision(2) << std::abs(value_delta)
                              << " at $" << std::setprecision(2) << price << std::endl;
                }
            }
            
            // Notify about liquidity changes
            if (liquidity_change_cb_) {
                LiquidityChange change{price, volume_delta, timestamp_ns, true};
                liquidity_change_cb_(change);
            }
        }
    }
    
    // Analyze ask changes
    for (const auto& [price, volume] : last_asks_volume_) {
        auto prev_it = prev_asks.find(price);
        double prev_volume = (prev_it != prev_asks.end()) ? prev_it->second : 0.0;
        
        if (std::abs(volume - prev_volume) > 1e-8) {
            double volume_delta = volume - prev_volume;
            double value_delta = volume_delta * price;
            
            if (volume_delta > 0) {
                // Order addition
                total_ask_additions += value_delta;
                std::cout << "[" << format_timestamp(timestamp_ns) << "] "
                          << "[ORDER FLOW] ASK ADD $" << std::fixed << std::setprecision(2) << value_delta
                          << " at $" << std::setprecision(2) << price << std::endl;
            } else {
                // Order removal/cancellation
                total_ask_removals += std::abs(value_delta);
                
                // Large removals might be cancellations
                if (volume_delta < -prev_volume * 0.3 && prev_volume > 0) {
                    std::cout << "[" << format_timestamp(timestamp_ns) << "] "
                              << "[CANCEL DETECTED] ASK at $" << std::fixed << std::setprecision(2) << price
                              << ", cancelled: $" << std::setprecision(2) << std::abs(value_delta) << std::endl;
                    processCancelVolumeInternal(false, std::abs(value_delta), timestamp_ns);
                } else {
                    std::cout << "[" << format_timestamp(timestamp_ns) << "] "
                              << "[ORDER FLOW] ASK REMOVE $" << std::fixed << std::setprecision(2) << std::abs(value_delta)
                              << " at $" << std::setprecision(2) << price << std::endl;
                }
            }
            
            // Notify about liquidity changes
            if (liquidity_change_cb_) {
                LiquidityChange change{price, volume_delta, timestamp_ns, false};
                liquidity_change_cb_(change);
            }
        }
    }
    
    // MODE 1: Track order flow buckets (separate from trade buckets)
    if (total_bid_additions > 0) {
        if (order_buy_start_ts_ns_ == 0) {
            order_buy_start_ts_ns_ = timestamp_ns;
        }
        order_buy_accum_usd_ += total_bid_additions;
        
        if (order_buy_accum_usd_ >= buy_bucket_size_) {
            uint64_t duration_ns = timestamp_ns - order_buy_start_ts_ns_;
            if (order_flow_buy_cb_) {
                order_flow_buy_cb_(true, duration_ns, buy_bucket_size_, 1.0);
            }
            order_buy_accum_usd_ = 0.0;
            order_buy_start_ts_ns_ = 0;
        }
    }
    
    if (total_ask_additions > 0) {
        if (order_sell_start_ts_ns_ == 0) {
            order_sell_start_ts_ns_ = timestamp_ns;
        }
        order_sell_accum_usd_ += total_ask_additions;
        
        if (order_sell_accum_usd_ >= sell_bucket_size_) {
            uint64_t duration_ns = timestamp_ns - order_sell_start_ts_ns_;
            if (order_flow_sell_cb_) {
                order_flow_sell_cb_(false, duration_ns, sell_bucket_size_, 1.0);
            }
            order_sell_accum_usd_ = 0.0;
            order_sell_start_ts_ns_ = 0;
        }
    }
}

void LiquidityTracker::processCancelVolumeInternal(bool is_buy, double cancel_volume, uint64_t timestamp_ns) {
    if (is_buy) {
        if (cancel_buy_start_ts_ns_ == 0) {
            cancel_buy_start_ts_ns_ = timestamp_ns;
        }
        
        cancel_buy_accum_usd_ += cancel_volume;
        cancel_buy_bucket_total_ += cancel_volume;
        
        if (cancel_buy_accum_usd_ >= cancel_bucket_size_) {
            uint64_t duration_ns = timestamp_ns - cancel_buy_start_ts_ns_;
            double cancel_ratio = cancel_buy_bucket_total_ / cancel_bucket_size_;
            
            if (cancel_buy_cb_) {
                cancel_buy_cb_(true, duration_ns, cancel_bucket_size_, cancel_ratio);
            }
            
            // Reset cancel buy bucket
            cancel_buy_accum_usd_ = 0.0;
            cancel_buy_bucket_total_ = 0.0;
            cancel_buy_start_ts_ns_ = 0;
        }
    } else {
        if (cancel_sell_start_ts_ns_ == 0) {
            cancel_sell_start_ts_ns_ = timestamp_ns;
        }
        
        cancel_sell_accum_usd_ += cancel_volume;
        cancel_sell_bucket_total_ += cancel_volume;
        
        if (cancel_sell_accum_usd_ >= cancel_bucket_size_) {
            uint64_t duration_ns = timestamp_ns - cancel_sell_start_ts_ns_;
            double cancel_ratio = cancel_sell_bucket_total_ / cancel_bucket_size_;
            
            if (cancel_sell_cb_) {
                cancel_sell_cb_(false, duration_ns, cancel_bucket_size_, cancel_ratio);
            }
            
            // Reset cancel sell bucket
            cancel_sell_accum_usd_ = 0.0;
            cancel_sell_bucket_total_ = 0.0;
            cancel_sell_start_ts_ns_ = 0;
        }
    }
}
