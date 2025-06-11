#pragma once

#include <string>
#include <map>
#include <unordered_map>
#include <iostream>
#include "core/serialization.hpp"  // For OrderBookUpdate

// Structure to track price level state
struct IcebergLevelState {
    double last_quantity = 0.0;
    int iceberg_counter = 0;
};

class IcebergDetector {
public:
    IcebergDetector();
    ~IcebergDetector();

    // Process an order book update
    void process_update(const OrderBookUpdate& update);

private:
    // Map of symbol -> price -> state
    std::unordered_map<std::string, std::map<double, IcebergLevelState>> book_state_;
    
    // Detect iceberg patterns at a specific price level
    void detect_iceberg(const std::string& symbol, double price, double quantity, bool is_bid);
    
    // Emit an iceberg detection event
    void emit_iceberg_event(const std::string& symbol, double price, bool is_bid);
};
