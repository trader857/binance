#include "trade_bucket_speed.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <sstream>

// Helper function to format timestamp
std::string format_timestamp_bucket(uint64_t timestamp_ns) {
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

TradeBucketSpeed::TradeBucketSpeed() 
    : bucket_size_usd_(10000.0)
    , bucket_accum_usd_(0.0)
    , start_ts_ns_(0) {
}

TradeBucketSpeed::TradeBucketSpeed(double bucket_size_usd)
    : bucket_size_usd_(bucket_size_usd)
    , bucket_accum_usd_(0.0)
    , start_ts_ns_(0) {
}

void TradeBucketSpeed::processTrade(const TradeMessageBinary& trade) {
    // Calculate trade value in USD
    double trade_value_usd = trade.price * trade.quantity;
    
    // Initialize start time if this is the first trade
    if (start_ts_ns_ == 0) {
        start_ts_ns_ = trade.timestamp_ns;
    }
    
    // Accumulate the trade value
    bucket_accum_usd_ += trade_value_usd;
    
    // Check if bucket is full
    if (bucket_accum_usd_ >= bucket_size_usd_) {
        uint64_t duration_ns = trade.timestamp_ns - start_ts_ns_;
        
        // Call the callback if set
        if (callback_) {
            callback_(duration_ns, bucket_accum_usd_);
        } else {
            // Default output with timestamp
            std::cout << "[" << format_timestamp_bucket(trade.timestamp_ns) << "] "
                      << "[TRADE BUCKET] $" << std::fixed << std::setprecision(2) << bucket_accum_usd_
                      << " traded in " << std::setprecision(1) << (duration_ns / 1e6) << " ms"
                      << " (rate: $" << std::setprecision(0) << (bucket_accum_usd_ / (duration_ns / 1e9)) << "/s)"
                      << std::endl;
        }
        
        // Reset the bucket
        bucket_accum_usd_ = 0.0;
        start_ts_ns_ = 0;
    }
}

void TradeBucketSpeed::setCallback(const BucketCallback& callback) {
    callback_ = callback;
}
