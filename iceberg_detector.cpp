#include "features/IcebergDetector.hpp"
#include <iostream>
#include <iomanip>

IcebergDetector::IcebergDetector() {}

IcebergDetector::~IcebergDetector() {}

void IcebergDetector::process_update(const OrderBookUpdate& update) {
    std::string symbol = "BTCUSDT"; // Default symbol since update doesn't have symbol field
    
    // Process bids
    for (const auto& bid : update.bids) {
        detect_iceberg(symbol, bid.price, bid.quantity, true);
    }
    
    // Process asks
    for (const auto& ask : update.asks) {
        detect_iceberg(symbol, ask.price, ask.quantity, false);
    }
}

void IcebergDetector::detect_iceberg(const std::string& symbol, double price, double quantity, bool is_bid) {
    auto& level_state = book_state_[symbol][price];

    // Simplified example logic:
    // If quantity decreased but order not fully removed, could be iceberg
    if (quantity < level_state.last_quantity && quantity > 0) {
        level_state.iceberg_counter++;
        if (level_state.iceberg_counter >= 3) {  // threshold to signal iceberg
            emit_iceberg_event(symbol, price, is_bid);
            level_state.iceberg_counter = 0;  // reset counter after detection
        }
    } else {
        level_state.iceberg_counter = 0;
    }

    level_state.last_quantity = quantity;
}

void IcebergDetector::emit_iceberg_event(const std::string& symbol, double price, bool is_bid) {
    std::cout << "[ICEBERG DETECTED] " << symbol << " " 
              << (is_bid ? "BID" : "ASK") << " at $" 
              << std::fixed << std::setprecision(2) << price << std::endl;
}
