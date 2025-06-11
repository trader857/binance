#include "core/serialization.hpp"
#include <cstring>
#include <stdexcept>
#include <chrono>  // ✅ Add this for timestamps
#include <nlohmann/json.hpp>
#include <iostream> // For error reporting

using json = nlohmann::json;

TradeMessageBinary Serialization::parse_trade_json(const std::string& json_str) {
    auto j = json::parse(json_str);

    TradeMessageBinary trade{};

    trade.event_time = (j.contains("E") && !j["E"].is_null()) ? j["E"].get<uint64_t>() : 0;
    trade.trade_id = (j.contains("t") && !j["t"].is_null()) ? j["t"].get<uint64_t>() : 0;

    // Price and quantity come as strings, parse to double
    trade.price = (j.contains("p") && !j["p"].is_null()) ? std::stod(j["p"].get<std::string>()) : 0.0;
    trade.quantity = (j.contains("q") && !j["q"].is_null()) ? std::stod(j["q"].get<std::string>()) : 0.0;

    trade.buyer_order_id = (j.contains("b") && !j["b"].is_null()) ? j["b"].get<uint64_t>() : 0;
    trade.seller_order_id = (j.contains("a") && !j["a"].is_null()) ? j["a"].get<uint64_t>() : 0;

    trade.trade_time = (j.contains("T") && !j["T"].is_null()) ? j["T"].get<uint64_t>() : 0;

    // ✅ FIX: Set proper timestamp in nanoseconds
    // Option 1: Use Binance trade_time (convert milliseconds to nanoseconds)
    if (trade.trade_time > 0) {
        trade.timestamp_ns = trade.trade_time * 1000000;  // Convert ms to ns
    } else {
        // Option 2: Use current system time as fallback
        auto now = std::chrono::high_resolution_clock::now();
        auto duration = now.time_since_epoch();
        trade.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    }

    trade.flags = 0;

    // Binance "m" is "is buyer maker" boolean
    bool is_buyer_maker = (j.contains("m") && !j["m"].is_null()) ? j["m"].get<bool>() : false;
    trade.set_is_buyer_maker(is_buyer_maker);

    // is_buy is typically the inverse of is_buyer_maker in Binance
    trade.set_is_buy(!is_buyer_maker);

    return trade;
}

std::vector<uint8_t> Serialization::serialize_trade(const TradeMessageBinary& trade) {
    std::vector<uint8_t> buffer(sizeof(TradeMessageBinary));
    std::memcpy(buffer.data(), &trade, sizeof(TradeMessageBinary));
    return buffer;
}

TradeMessageBinary Serialization::deserialize_trade(const uint8_t* data, size_t size) {
    if (size < sizeof(TradeMessageBinary)) {
        throw std::runtime_error("Buffer too small for TradeMessageBinary");
    }
    TradeMessageBinary trade;
    std::memcpy(&trade, data, sizeof(TradeMessageBinary));
    return trade;
}

// New functions for order book handling

std::optional<OrderBookUpdate> Serialization::parse_orderbook_json(const std::string& json_str) {
    try {
        auto j = json::parse(json_str);
        
        // Check if this is a depth update
        if (!j.contains("e") || j["e"] != "depthUpdate") {
            return std::nullopt;
        }
        
        OrderBookUpdate update{};
        
        // Set timestamps - both event time and local time
        uint64_t event_time = (j.contains("E") && !j["E"].is_null()) ? j["E"].get<uint64_t>() : 0;
        update.timestamp_ns = event_time * 1000000; // Convert ms to ns
        
        // If no event time from Binance, use local time
        if (update.timestamp_ns == 0) {
            auto now = std::chrono::high_resolution_clock::now();
            auto duration = now.time_since_epoch();
            update.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
        }
        
        // Get update ID (important for sequencing)
        update.last_update_id = (j.contains("u") && !j["u"].is_null()) ? j["u"].get<uint64_t>() : 0;
        
        // Parse bids - bids array has format [["price", "quantity"], ...]
        if (j.contains("b") && j["b"].is_array()) {
            for (const auto& bid : j["b"]) {
                if (bid.is_array() && bid.size() >= 2) {
                    double price = std::stod(bid[0].get<std::string>());
                    double quantity = std::stod(bid[1].get<std::string>());
                    
                    // Quantity of 0 means remove this price level - don't include it
                    if (quantity > 0) {
                        update.bids.push_back({price, quantity});
                    }
                }
            }
        }
        
        // Parse asks - asks array has format [["price", "quantity"], ...]
        if (j.contains("a") && j["a"].is_array()) {
            for (const auto& ask : j["a"]) {
                if (ask.is_array() && ask.size() >= 2) {
                    double price = std::stod(ask[0].get<std::string>());
                    double quantity = std::stod(ask[1].get<std::string>());
                    
                    // Quantity of 0 means remove this price level - don't include it
                    if (quantity > 0) {
                        update.asks.push_back({price, quantity});
                    }
                }
            }
        }
        
        return update;
    } catch (const std::exception& e) {
        std::cerr << "[Error] Failed to parse orderbook: " << e.what() << std::endl;
        return std::nullopt;
    }
}

std::vector<uint8_t> Serialization::serialize_orderbook(const OrderBookUpdate& book) {
    // Calculate buffer size: header + all price levels
    size_t bid_size = book.bids.size() * sizeof(PriceLevel);
    size_t ask_size = book.asks.size() * sizeof(PriceLevel);
    size_t header_size = sizeof(uint64_t) * 2 + sizeof(uint32_t) * 2; // timestamp, last_update_id, bid_count, ask_count
    
    std::vector<uint8_t> buffer(header_size + bid_size + ask_size);
    uint8_t* ptr = buffer.data();
    
    // Write header
    std::memcpy(ptr, &book.timestamp_ns, sizeof(uint64_t));
    ptr += sizeof(uint64_t);
    
    std::memcpy(ptr, &book.last_update_id, sizeof(uint64_t));
    ptr += sizeof(uint64_t);
    
    uint32_t bid_count = static_cast<uint32_t>(book.bids.size());
    std::memcpy(ptr, &bid_count, sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    
    uint32_t ask_count = static_cast<uint32_t>(book.asks.size());
    std::memcpy(ptr, &ask_count, sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    
    // Write bids
    for (const auto& bid : book.bids) {
        std::memcpy(ptr, &bid, sizeof(PriceLevel));
        ptr += sizeof(PriceLevel);
    }
    
    // Write asks
    for (const auto& ask : book.asks) {
        std::memcpy(ptr, &ask, sizeof(PriceLevel));
        ptr += sizeof(PriceLevel);
    }
    
    return buffer;
}

OrderBookUpdate Serialization::deserialize_orderbook(const uint8_t* data, size_t size) {
    if (size < sizeof(uint64_t) * 2 + sizeof(uint32_t) * 2) {
        throw std::runtime_error("Buffer too small for OrderBookUpdate header");
    }
    
    OrderBookUpdate book;
    const uint8_t* ptr = data;
    
    // Read header
    std::memcpy(&book.timestamp_ns, ptr, sizeof(uint64_t));
    ptr += sizeof(uint64_t);
    
    std::memcpy(&book.last_update_id, ptr, sizeof(uint64_t));
    ptr += sizeof(uint64_t);
    
    uint32_t bid_count;
    std::memcpy(&bid_count, ptr, sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    
    uint32_t ask_count;
    std::memcpy(&ask_count, ptr, sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    
    // Validate buffer size
    size_t expected_size = sizeof(uint64_t) * 2 + sizeof(uint32_t) * 2 + 
                          (bid_count + ask_count) * sizeof(PriceLevel);
    if (size < expected_size) {
        throw std::runtime_error("Buffer too small for OrderBookUpdate data");
    }
    
    // Read bids
    book.bids.resize(bid_count);
    for (uint32_t i = 0; i < bid_count; ++i) {
        std::memcpy(&book.bids[i], ptr, sizeof(PriceLevel));
        ptr += sizeof(PriceLevel);
    }
    
    // Read asks
    book.asks.resize(ask_count);
    for (uint32_t i = 0; i < ask_count; ++i) {
        std::memcpy(&book.asks[i], ptr, sizeof(PriceLevel));
        ptr += sizeof(PriceLevel);
    }
    
    return book;
}
