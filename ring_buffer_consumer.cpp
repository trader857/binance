#include "io/mmap_buffer.hpp"
#include "core/ts_queue.hpp"
#include "core/serialization.hpp"
#include <atomic>
#include <thread>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <chrono>
#include <sstream>

// Import external variables
extern std::atomic<bool> stop_flag;
extern TSQueue<OrderBookUpdate> iceberg_queue;
extern TSQueue<OrderBookUpdate> liquidity_queue;
extern TSQueue<TradeMessageBinary> trade_queue;

// Use the same message type identifiers as in binance_connector.cpp
enum MessageType : uint8_t {
    TYPE_TRADE = 0x01,
    TYPE_ORDERBOOK = 0x02
};

// Helper function to format timestamp
std::string format_timestamp_consumer(uint64_t timestamp_ns) {
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

void consume_ring_buffer() {
    MMapBuffer buffer(4096, false); // Open existing buffer in read mode
    
    // Allocate a buffer large enough for any message type
    constexpr size_t MAX_MESSAGE_SIZE = 8192; // Adjust based on expected order book size
    std::vector<uint8_t> data_buffer(MAX_MESSAGE_SIZE);
    
    while (!stop_flag.load(std::memory_order_acquire)) {
        // First, try to read the message header (type + length)
        constexpr size_t HEADER_SIZE = 5;
        size_t header_bytes_read = buffer.read(data_buffer.data(), HEADER_SIZE);
        
        if (header_bytes_read == HEADER_SIZE) {
            MessageType msg_type = static_cast<MessageType>(data_buffer[0]);
            uint32_t msg_length;
            std::memcpy(&msg_length, data_buffer.data() + 1, sizeof(uint32_t));
            
            // Make sure our buffer is large enough
            if (msg_length > data_buffer.size()) {
                data_buffer.resize(msg_length);
            }
            
            // Read the rest of the message
            size_t body_bytes_read = buffer.read(data_buffer.data(), msg_length);
            
            if (body_bytes_read == msg_length) {
                // Process based on message type
                switch (msg_type) {
                    case TYPE_TRADE: {
                        if (msg_length == sizeof(TradeMessageBinary)) {
                            TradeMessageBinary trade = Serialization::deserialize_trade(
                                data_buffer.data(), msg_length);
                            
                            // Push to trade queue for liquidity tracking
                            trade_queue.push(trade);
                            
                            // Enhanced output with timestamp and dollar values
                            double trade_value_usd = trade.price * trade.quantity;
                            std::cout << "[" << format_timestamp_consumer(trade.timestamp_ns) << "] "
                                      << "[Consumer] Processed trade: " << trade.trade_id
                                      << ", price: $" << std::fixed << std::setprecision(2) << trade.price
                                      << ", quantity: " << std::setprecision(4) << trade.quantity
                                      << ", value: $" << std::setprecision(2) << trade_value_usd
                                      << ", side: " << (trade.is_buy() ? "BUY" : "SELL")
                                      << std::endl;
                        } else {
                            std::cerr << "[Consumer] Invalid trade message size: " << msg_length << std::endl;
                        }
                        break;
                    }
                    
                    case TYPE_ORDERBOOK: {
                        try {
                            OrderBookUpdate book = Serialization::deserialize_orderbook(
                                data_buffer.data(), msg_length);
                            
                            // Push to both queues that need order book data
                            iceberg_queue.push(book);
                            liquidity_queue.push(book);
                            
                            // Calculate total volume in USD for best bid/ask
                            double best_bid_value = 0.0;
                            double best_ask_value = 0.0;
                            
                            if (!book.bids.empty()) {
                                best_bid_value = book.bids[0].price * book.bids[0].quantity;
                            }
                            if (!book.asks.empty()) {
                                best_ask_value = book.asks[0].price * book.asks[0].quantity;
                            }
                            
                            std::cout << "[" << format_timestamp_consumer(book.timestamp_ns) << "] "
                                      << "[Consumer] Processed orderbook update: " << book.last_update_id
                                      << ", bids: " << book.bids.size()
                                      << ", asks: " << book.asks.size()
                                      << ", best bid value: $" << std::fixed << std::setprecision(2) << best_bid_value
                                      << ", best ask value: $" << std::setprecision(2) << best_ask_value
                                      << std::endl;
                        } catch (const std::exception& e) {
                            std::cerr << "[Consumer] Error deserializing order book: " << e.what() << std::endl;
                        }
                        break;
                    }
                    
                    default:
                        std::cerr << "[Consumer] Unknown message type: " << static_cast<int>(msg_type) << std::endl;
                        break;
                }
            } else {
                std::cerr << "[Consumer] Incomplete message body read: expected " << msg_length 
                          << ", got " << body_bytes_read << std::endl;
            }
        } else if (header_bytes_read > 0) {
            std::cerr << "[Consumer] Incomplete header read: " << header_bytes_read << " bytes" << std::endl;
        }
        
        // Sleep a bit to avoid spinning unnecessarily
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    std::cout << "[Consumer] Ring buffer consumer thread exiting" << std::endl;
}
