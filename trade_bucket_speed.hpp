#pragma once

#include <cstdint>
#include <functional>

// TradeMessageBinary struct definition (if not already defined elsewhere)
struct TradeMessageBinary {
    double price;
    double quantity;
    uint64_t timestamp_ns;
};

// Callback type for bucket notifications
using BucketCallback = std::function<void(uint64_t duration_ns, double bucket_value)>;

class TradeBucketSpeed {
public:
    // Constructors
    TradeBucketSpeed();
    TradeBucketSpeed(double bucket_size_usd);

    // Process trade and update bucket
    void processTrade(const TradeMessageBinary& trade);

    // Set callback for bucket filled notifications
    void setCallback(const BucketCallback& callback);

private:
    double bucket_size_usd_;         // Size of the bucket in USD
    double bucket_accum_usd_;        // Accumulated USD value in the current bucket
    uint64_t start_ts_ns_;           // Start time of the current bucket in nanoseconds
    BucketCallback callback_;        // Callback for bucket notifications
};
