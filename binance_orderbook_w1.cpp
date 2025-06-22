#include <iostream>
#include <map>
#include <vector>
#include <string>
#include <jsoncpp/json/json.h>
#include <thread>
#include <mutex>
#include <iomanip>
#include <cmath>
#include <libwebsockets.h>
#include <atomic>
#include <chrono>
#include <curl/curl.h>
#include <algorithm>
#include <memory>
#include <unistd.h>
#include <chrono>
#include <ctime>
#include <deque>

// Helper function for libcurl to write response data to a string
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* s) {
    size_t newLength = size * nmemb;
    try {
        s->append((char*)contents, newLength);
    } catch (const std::bad_alloc& e) {
        std::cerr << "Memory allocation error in WriteCallback: " << e.what() << std::endl;
        return 0;
    }
    return newLength;
}

class BinanceOrderBook {
private:
    // Order book data
    std::map<double, std::pair<double, std::string>> bids;  // price -> {quantity, source}
    std::map<double, std::pair<double, std::string>> asks;  // price -> {quantity, source}
    std::mutex orderbook_mutex;
    std::string user_login = "trader857ok";

    // Trade data storage
    struct Trade {
        uint64_t id = 0; // Default to 0 to indicate an empty slot
        double price;
        double quantity;
        bool isBuyerMaker;  // false = buy trade (market buy), true = sell trade (market sell)
        std::chrono::system_clock::time_point timestamp;
        std::string time_str;
    };
    
    // --- Ring Buffer Implementation for Recent Trades ---
    std::vector<Trade> recent_trades;
    size_t trade_head = 0; // Points to the next slot to be written in the ring buffer
    const size_t max_trades_to_store = 20;
    
    // Volume tracking
    double cumulative_buy_volume_btc = 0.0;    // BTC volume
    double cumulative_sell_volume_btc = 0.0;   // BTC volume
    double cumulative_buy_volume_usd = 0.0;    // USD volume (price * quantity)
    double cumulative_sell_volume_usd = 0.0;   // USD volume (price * quantity)
    
    // NEW: Cached metrics structure for separation
    struct OrderBookMetrics {
        double best_bid = 0.0;
        double best_ask = 0.0;
        double spread = 0.0;
        double imbalance_2_levels = 0.0;
        double imbalance_10_levels = 0.0;
        double imbalance_20_levels = 0.0;
        double imbalance_all_levels = 0.0;
        double total_ask_liquidity = 0.0;
        double total_bid_liquidity = 0.0;
        std::string interpretation_2;
        std::string interpretation_10;
        std::string interpretation_20;
        std::string interpretation_all;
        std::chrono::system_clock::time_point last_updated;
    };
    
    OrderBookMetrics cached_metrics;
    mutable std::mutex metrics_mutex;
    bool auto_print_enabled = true;  // NEW: Toggle for printing
    
    // Calculate order book imbalance based on USD volume with limited levels
    double calculate_orderbook_imbalance(int levels = 10) {
        std::lock_guard<std::mutex> lock(orderbook_mutex);
        
        double total_ask_volume_usd = 0.0;
        double total_bid_volume_usd = 0.0;
        
        // Sum up USD values on ask side (limited to specified levels)
        int count = 0;
        for (auto it = asks.begin(); it != asks.end() && count < levels; ++it, ++count) {
            double usd_value = it->first * it->second.first;  // price * quantity
            total_ask_volume_usd += usd_value;
        }
        
        // Sum up USD values on bid side (limited to specified levels)
        count = 0;
        for (auto it = bids.rbegin(); it != bids.rend() && count < levels; ++it, ++count) {
            double usd_value = it->first * it->second.first;  // price * quantity
            total_bid_volume_usd += usd_value;
        }
        
        // Calculate imbalance
        double total_volume_usd = total_ask_volume_usd + total_bid_volume_usd;
        if (total_volume_usd > 0) {
            return (total_ask_volume_usd - total_bid_volume_usd) / total_volume_usd;
        }
        
        return 0.0;  // Default to balanced if no orders
    }

    // Helper function to interpret imbalance value
    std::string interpret_imbalance(double imbalance) {
        if (imbalance > 0.20) {
            return " (Strong Buying Pressure)";
        } else if (imbalance > 0.05) {
            return " (Moderate Buying Pressure)";
        } else if (imbalance < -0.20) {
            return " (Strong Selling Pressure)";
        } else if (imbalance < -0.05) {
            return " (Moderate Selling Pressure)";
        } else {
            return " (Neutral)";
        }
    }

    // NEW: Separate calculation function (no printing) - ALWAYS RUNS
    void calculate_all_metrics() {
        std::lock_guard<std::mutex> ob_lock(orderbook_mutex);
        std::lock_guard<std::mutex> metrics_lock(metrics_mutex);
        
        auto now = std::chrono::system_clock::now();
        
        // Calculate basic metrics
        if (!bids.empty()) {
            cached_metrics.best_bid = bids.rbegin()->first;
        }
        if (!asks.empty()) {
            cached_metrics.best_ask = asks.begin()->first;
        }
        
        if (cached_metrics.best_bid > 0 && cached_metrics.best_ask > 0) {
            cached_metrics.spread = cached_metrics.best_ask - cached_metrics.best_bid;
        }
        
        // Calculate imbalance metrics (if enabled)
        if (imbalance_calculation_enabled) {
            // NOTE on data copy:
            // The order book (asks/bids) is copied here intentionally. This is a crucial optimization
            // for concurrency. By creating a copy, we can release the `orderbook_mutex` very quickly,
            // allowing the high-frequency WebSocket thread to continue processing updates without being
            // blocked by these potentially lengthy calculations. This prevents data backlog and keeps
            // the order book data fresh. The trade-off is a small amount of memory and CPU for the copy
            // in exchange for much better overall application responsiveness and data accuracy.
            std::vector<std::pair<double, double>> ask_copy, bid_copy;
            
            for (auto it = asks.begin(); it != asks.end(); ++it)
                ask_copy.emplace_back(it->first, it->second.first);
                
            for (auto it = bids.begin(); it != bids.end(); ++it)
                bid_copy.emplace_back(it->first, it->second.first);
            
            // Calculate metrics from copied data
            double ask_volume_2 = 0.0, bid_volume_2 = 0.0;
            double ask_volume_10 = 0.0, bid_volume_10 = 0.0;
            double ask_volume_20 = 0.0, bid_volume_20 = 0.0;
            double ask_volume_all = 0.0, bid_volume_all = 0.0;
            
            // Process ask data (ascending order - lowest to highest)
            int count = 0;
            for (const auto& [price, quantity] : ask_copy) {
                double usd_value = price * quantity;
                if (count < 2) ask_volume_2 += usd_value;
                if (count < 10) ask_volume_10 += usd_value;
                if (count < 20) ask_volume_20 += usd_value;
                ask_volume_all += usd_value;
                count++;
            }
            
            // Process bid data (descending order - highest to lowest)
            count = 0;
            for (auto it = bid_copy.rbegin(); it != bid_copy.rend(); ++it, ++count) {
                double usd_value = it->first * it->second;
                if (count < 2) bid_volume_2 += usd_value;
                if (count < 10) bid_volume_10 += usd_value;
                if (count < 20) bid_volume_20 += usd_value;
                bid_volume_all += usd_value;
            }
            
            // Calculate imbalances
            auto calc_imb = [](double ask, double bid) {
                double total = ask + bid;
                return total > 0 ? (bid - ask) / total : 0.0;
            };
            
            cached_metrics.imbalance_2_levels = calc_imb(ask_volume_2, bid_volume_2);
            cached_metrics.imbalance_10_levels = calc_imb(ask_volume_10, bid_volume_10);
            cached_metrics.imbalance_20_levels = calc_imb(ask_volume_20, bid_volume_20);
            cached_metrics.imbalance_all_levels = calc_imb(ask_volume_all, bid_volume_all);
            
            cached_metrics.total_ask_liquidity = ask_volume_all;
            cached_metrics.total_bid_liquidity = bid_volume_all;
            
            // Generate interpretations
            cached_metrics.interpretation_2 = interpret_imbalance(cached_metrics.imbalance_2_levels);
            cached_metrics.interpretation_10 = interpret_imbalance(cached_metrics.imbalance_10_levels);
            cached_metrics.interpretation_20 = interpret_imbalance(cached_metrics.imbalance_20_levels);
            cached_metrics.interpretation_all = interpret_imbalance(cached_metrics.imbalance_all_levels);
        }
        
        cached_metrics.last_updated = now;
    }
        
    // Time-windowed volumes (e.g., 1-minute window)
    struct TimeWindowedVolume {
        double buy_volume_btc = 0.0;     // BTC quantity
        double sell_volume_btc = 0.0;    // BTC quantity
        double buy_volume_usd = 0.0;     // USD value (price * quantity)
        double sell_volume_usd = 0.0;    // USD value (price * quantity)
        std::chrono::system_clock::time_point start_time;
    };
    
    std::vector<TimeWindowedVolume> volume_windows;  // Multiple time windows (1m, 5m, 15m)
    std::chrono::seconds window_duration = std::chrono::seconds(300);
    std::mutex trades_mutex;

    // Configuration
    double tick_size = 0.0100;
    std::vector<double> available_tick_sizes = {0.001, 0.01, 0.1, 1.0, 10.0, 100.0};
    const std::string symbol = "btcusdt";
    
    // WebSocket handling
    struct lws_context *context = nullptr;
    struct lws *wsi = nullptr;
    std::string ws_buffer;
    std::atomic<uint64_t> last_update_id{0};
    
    // Threading
    std::atomic<bool> is_running{false};
    std::thread ws_thread;
    std::thread api_thread;
    
    // Singleton instance for WebSocket callback
    static BinanceOrderBook* instance;
    static struct lws_protocols protocols[];

    // Round price to current tick size
    double round_to_tick_size(double price) {
        if (std::abs(tick_size) < 1e-9) return price;
        return std::round(price / tick_size) * tick_size;
    }
    
    // Update time-windowed volume data
    void update_time_windows(double buy_vol_btc, double sell_vol_btc, 
                         double buy_vol_usd, double sell_vol_usd,
                         const std::chrono::system_clock::time_point& timestamp) {
        auto now = timestamp;
        
        if (volume_windows.empty() || 
            now - volume_windows.back().start_time > window_duration) {
            
            // Add new window
            TimeWindowedVolume new_window;
            new_window.start_time = now;
            new_window.buy_volume_btc = buy_vol_btc;
            new_window.sell_volume_btc = sell_vol_btc;
            new_window.buy_volume_usd = buy_vol_usd;
            new_window.sell_volume_usd = sell_vol_usd;
            volume_windows.push_back(new_window);
        } else {
            // Update existing window
            volume_windows.back().buy_volume_btc += buy_vol_btc;
            volume_windows.back().sell_volume_btc += sell_vol_btc;
            volume_windows.back().buy_volume_usd += buy_vol_usd;
            volume_windows.back().sell_volume_usd += sell_vol_usd;
        }
    }

    // Fetch initial order book snapshot from REST API
    void fetch_api_snapshot() {
        CURL* curl;
        CURLcode res;
        std::string readBuffer;

        curl = curl_easy_init();
        if (curl) {
            std::string url = "https://api.binance.us/api/v3/depth?symbol=BTCUSDC&limit=50";
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);

            res = curl_easy_perform(curl);

            long http_code = 0;
            if (res == CURLE_OK) {
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            }

            if (res != CURLE_OK) {
                std::cerr << "API curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
            } else if (http_code != 200) {
                std::cerr << "API request failed with HTTP code: " << http_code << std::endl;
            } else {
                process_api_snapshot(readBuffer);
            }
            curl_easy_cleanup(curl);
        } else {
            std::cerr << "Failed to initialize libcurl for API call" << std::endl;
        }
    }

    // FIXED: Process the initial snapshot - NO MORE DEADLOCK
    void process_api_snapshot(const std::string& message) {
        try {
            Json::Value root;
            Json::CharReaderBuilder readerBuilder;
            std::unique_ptr<Json::CharReader> const jsonReader(readerBuilder.newCharReader());
            std::string errs;

            if (!jsonReader->parse(message.c_str(), message.c_str() + message.length(), &root, &errs)) {
                std::cerr << "Failed to parse API JSON: " << errs << std::endl;
                return;
            }

            if (root.isMember("lastUpdateId") && root.isMember("bids") && root.isMember("asks")) {
                // FIXED: Scope the lock properly to avoid deadlock
                {
                    std::lock_guard<std::mutex> lock(orderbook_mutex);
                    
                    // Update the last update ID
                    uint64_t snapshot_update_id = root["lastUpdateId"].asUInt64();
                    last_update_id.store(snapshot_update_id);
                    std::cout << "Received order book snapshot with lastUpdateId: " << snapshot_update_id << std::endl;
                    
                    // Clear existing data
                    bids.clear();
                    asks.clear();

                    // Process bids
                    const Json::Value& bids_json = root["bids"];
                    for (const auto& bid : bids_json) {
                        double price = std::stod(bid[0].asString());
                        double quantity = std::stod(bid[1].asString());
                        price = round_to_tick_size(price);
                        if (quantity > 0) bids[price] = std::make_pair(quantity, "API");
                    }

                    // Process asks
                    const Json::Value& asks_json = root["asks"];
                    for (const auto& ask : asks_json) {
                        double price = std::stod(ask[0].asString());
                        double quantity = std::stod(ask[1].asString());
                        price = round_to_tick_size(price);
                        if (quantity > 0) asks[price] = std::make_pair(quantity, "API");
                    }
                } // Lock is released here!
                
                // Print the order book AFTER releasing the lock
                print_orderbook();
            }
        } catch (const std::exception& e) {
            std::cerr << "Error processing API snapshot: " << e.what() << std::endl;
        }
    }

    // Message router to handle different types of WebSocket messages
    void process_ws_message(const std::string& message) {
        try {
            Json::Value root;
            Json::CharReaderBuilder readerBuilder;
            std::unique_ptr<Json::CharReader> const jsonReader(readerBuilder.newCharReader());
            std::string errs;

            if (!jsonReader->parse(message.c_str(), message.c_str() + message.length(), &root, &errs)) {
                std::cerr << "Failed to parse WebSocket JSON: " << errs << std::endl;
                return;
            }

            // Route message based on event type
            if (root.isMember("e")) {
                std::string event_type = root["e"].asString();
                
                if (event_type == "depthUpdate") {
                    process_ws_update(message);
                } else if (event_type == "trade") {
                    process_trade_message(message);
                } else {
                    std::cerr << "Unknown event type: " << event_type << std::endl;
                }
            } else {
                std::cerr << "Message missing event type field" << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in message router: " << e.what() << std::endl;
        }
    }

    // FIXED: Process WebSocket diff updates - NO MORE DEADLOCK
    void process_ws_update(const std::string& message) {
        try {
            if (message.length() < 2) {
                return;
            }
            
            Json::Value root;
            Json::CharReaderBuilder readerBuilder;
            std::unique_ptr<Json::CharReader> const jsonReader(readerBuilder.newCharReader());
            std::string errs;

            if (!jsonReader->parse(message.c_str(), message.c_str() + message.length(), &root, &errs)) {
                std::cerr << "Failed to parse WebSocket JSON: " << errs << std::endl;
                return;
            }

            // Ensure this is a depth update message with the right fields
            if (root.isMember("e") && root["e"].asString() == "depthUpdate" && 
                root.isMember("u") && root.isMember("b") && root.isMember("a")) {
                
                // Get the update ID and first/last update IDs
                uint64_t update_id = root["u"].asUInt64();
                uint64_t first_update_id = root["U"].asUInt64();
                
                // Check if this update is valid for our current state
                uint64_t current_last_id = last_update_id.load();
                
                if (update_id <= current_last_id) {
                    // This is an old update, skip it
                    return;
                }
                
                if (first_update_id <= current_last_id + 1) {
                    // FIXED: Scope the lock properly to avoid deadlock
                    {
                        std::lock_guard<std::mutex> lock(orderbook_mutex);
                        
                        // Process bids updates
                        const Json::Value& bids_json = root["b"];
                        for (const auto& bid : bids_json) {
                            double price = std::stod(bid[0].asString());
                            double quantity = std::stod(bid[1].asString());
                            price = round_to_tick_size(price);
                            
                            if (quantity > 0) {
                                bids[price] = std::make_pair(quantity, "WS");
                            } else {
                                bids.erase(price);
                            }
                        }
                        
                        // Process asks updates
                        const Json::Value& asks_json = root["a"];
                        for (const auto& ask : asks_json) {
                            double price = std::stod(ask[0].asString());
                            double quantity = std::stod(ask[1].asString());
                            price = round_to_tick_size(price);
                            
                            if (quantity > 0) {
                                asks[price] = std::make_pair(quantity, "WS");
                            } else {
                                asks.erase(price);
                            }
                        }
                        
                        // Update our last update ID
                        last_update_id.store(update_id);
                    } // Lock is released here!
                    
                    // Print the updated order book AFTER releasing the lock
                    print_orderbook();
                } else {
                    // Out of sync, need to re-fetch the snapshot
                    std::cout << "Order book out of sync. Fetching new snapshot..." << std::endl;
                    fetch_api_snapshot();
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error processing WebSocket update: " << e.what() << std::endl;
        }
    }
    
    // Process trade message from WebSocket
    void process_trade_message(const std::string& message) {
        try {
            Json::Value root;
            Json::CharReaderBuilder readerBuilder;
            std::unique_ptr<Json::CharReader> const jsonReader(readerBuilder.newCharReader());
            std::string errs;

            if (!jsonReader->parse(message.c_str(), message.c_str() + message.length(), &root, &errs)) {
                std::cerr << "Failed to parse trade JSON: " << errs << std::endl;
                return;
            }
            
            // Ensure this is a trade message
            if (root.isMember("e") && root["e"].asString() == "trade") {
                std::lock_guard<std::mutex> lock(trades_mutex);
                
                // Extract trade data
                uint64_t trade_id = root["t"].asUInt64();  // Trade ID
                double price = std::stod(root["p"].asString());  // Price
                double quantity = std::stod(root["q"].asString());  // Quantity
                bool is_buyer_maker = root["m"].asBool();  // Is buyer maker
                
                // Calculate USD value
                double usd_value = price * quantity;
                
                // Convert timestamp to readable time
                uint64_t timestamp_ms = root["T"].asUInt64();
                auto trade_time = std::chrono::system_clock::time_point(
                    std::chrono::milliseconds(timestamp_ms));
                    
                auto time_t_timestamp = std::chrono::system_clock::to_time_t(trade_time);
                std::tm* tm_time = std::localtime(&time_t_timestamp);  // Pass the time_t pointer
                char time_buffer[32];
                std::strftime(time_buffer, sizeof(time_buffer), "%H:%M:%S", tm_time);
                std::string time_str = time_buffer;
                
                // Update volume statistics
                if (!is_buyer_maker) {  // Market buy
                    cumulative_buy_volume_btc += quantity;
                    cumulative_buy_volume_usd += usd_value;
                    update_time_windows(quantity, 0.0, usd_value, 0.0, trade_time);
                } else {  // Market sell
                    cumulative_sell_volume_btc += quantity;
                    cumulative_sell_volume_usd += usd_value;
                    update_time_windows(0.0, quantity, 0.0, usd_value, trade_time);
                }
                
                // Add to recent trades using the ring buffer
                recent_trades[trade_head] = Trade{trade_id, price, quantity, is_buyer_maker, trade_time, time_str};
                trade_head = (trade_head + 1) % max_trades_to_store;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error processing trade message: " << e.what() << std::endl;
        }
    }
 
    // Display recent trades and volume data
	
    void print_trades_and_volumes() {
        std::lock_guard<std::mutex> lock(trades_mutex);
        
        std::cout << "\n--- RECENT TRADES ---" << std::endl;
        std::cout << std::setw(10) << "Time" << " | " 
                  << std::setw(10) << "Price" << " | " 
                  << std::setw(10) << "Quantity" << " | "
                  << std::setw(12) << "USD Value" << " | " 
                  << "Side" << std::endl;
        std::cout << "----------------------------------------------------------------------" << std::endl;
        
        int count = 0;
        // Iterate from newest to oldest in the ring buffer
        for (size_t i = 0; i < max_trades_to_store; ++i) {
            if (count >= 50) break; // Limit to 50, though max_trades_to_store is smaller

            const auto& trade = recent_trades[(trade_head - 1 - i + max_trades_to_store) % max_trades_to_store];

            // Skip empty slots if the buffer isn't full yet
            if (trade.id == 0) {
                continue;
            }
            
            double usd_value = trade.price * trade.quantity;
            std::cout << std::setw(10) << trade.time_str << " | "
                      << std::setprecision(get_precision_for_tick_size()) << std::setw(10) << trade.price << " | "
                      << std::setprecision(5) << std::setw(10) << trade.quantity << " | "
                      << std::fixed << std::setprecision(2) << std::setw(12) << usd_value << " | "
                      << (trade.isBuyerMaker ? "SELL" : "BUY") << std::endl;
            count++;
        }
        
        std::cout << "\n--- VOLUME METRICS ---" << std::endl;
        // BTC volume display
        std::cout << "Total Buy Volume (BTC): " << std::fixed << std::setprecision(5) 
                  << cumulative_buy_volume_btc << " BTC" << std::endl;
        std::cout << "Total Sell Volume (BTC): " << std::fixed << std::setprecision(5) 
                  << cumulative_sell_volume_btc << " BTC" << std::endl;
        
        // USD volume display - prominently shown 
        std::cout << "\n--- USD TRADING VOLUME ---" << std::endl;
        std::cout << "Total Buy Volume (USD): $" << std::fixed << std::setprecision(2) 
                  << cumulative_buy_volume_usd << std::endl;
        std::cout << "Total Sell Volume (USD): $" << std::fixed << std::setprecision(2) 
                  << cumulative_sell_volume_usd << std::endl;
        
        // Calculate buy/sell ratio in USD value
        double usd_ratio = (cumulative_sell_volume_usd > 0) ? 
                          (cumulative_buy_volume_usd / cumulative_sell_volume_usd) : 
                          (cumulative_buy_volume_usd > 0 ? 999.99 : 0.0);
                          
        std::cout << "Buy/Sell USD Ratio: " << std::fixed << std::setprecision(2) << usd_ratio << std::endl;
        
        // Display recent window volumes
        if (!volume_windows.empty()) {
            auto& latest_window = volume_windows.back();
            std::cout << "\n--- LAST MINUTE ACTIVITY ---" << std::endl;
            std::cout << "Buy Volume: " << std::fixed << std::setprecision(5) 
                      << latest_window.buy_volume_btc << " BTC  ($" 
                      << std::fixed << std::setprecision(2) << latest_window.buy_volume_usd << ")" << std::endl;
            std::cout << "Sell Volume: " << std::fixed << std::setprecision(5) 
                      << latest_window.sell_volume_btc << " BTC  ($" 
                      << std::fixed << std::setprecision(2) << latest_window.sell_volume_usd << ")" << std::endl;
        }
    }
    
    // MODIFIED: Display the order book - NOW WITH CALCULATION/PRINTING SEPARATION
    void print_orderbook() {
        // ALWAYS calculate metrics first (regardless of print setting)
        calculate_all_metrics();
        
        // Only print if auto-print is enabled
        if (!auto_print_enabled) {
            return;  // Skip printing but keep calculations
        }
        
        // Find expected price range to filter out obviously bad data
        double best_bid = 0, best_ask = 0;
        
        if (!bids.empty()) {
            best_bid = bids.rbegin()->first; // Highest bid price
        }
        
        if (!asks.empty()) {
            best_ask = asks.begin()->first; // Lowest ask price
        }
        
        // Remove obviously wrong bid prices (more than 5% away from best bid)
        if (best_bid > 0) {
            auto it = bids.begin();
            while (it != bids.end()) {
                if (it->first < best_bid * 0.95) {
                    it = bids.erase(it);
                } else {
                    ++it;
                }
            }
        }
        
        std::cout << "\033[2J\033[1;1H"; // Clear screen and move cursor to top-left
        std::cout << "=== BTC/USDT Order Book (Tick Size: " << std::fixed 
                << std::setprecision(get_precision_for_tick_size()) << tick_size 
                << ", Last Update ID: " << last_update_id.load() << ") ===" << std::endl;
        
        // Add current date and time in UTC with specified format
        auto now = std::chrono::system_clock::now();
        std::time_t now_c = std::chrono::system_clock::to_time_t(now);
        std::tm* now_tm = std::localtime(&now_c);  // Use localtime instead of gmtime

        char time_str[26];
        std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", now_tm);

        // Print time and user information
        std::cout << "Current Date and Time (UTC - YYYY-MM-DD HH:MM:SS formatted): " << time_str << std::endl;
        std::cout << "Current User's Login: " << user_login << std::endl;
        std::cout << std::fixed;

        const int max_levels_to_print = 30;
        
        // Print spread information USING CACHED DATA
        std::cout << "\n--- SPREAD ---" << std::endl;
        {
            std::lock_guard<std::mutex> metrics_lock(metrics_mutex);
            if (cached_metrics.best_bid > 0 && cached_metrics.best_ask > 0) {
                std::cout << "Best Bid: " << std::setprecision(get_precision_for_tick_size()) << cached_metrics.best_bid
                        << " | Best Ask: " << std::setprecision(get_precision_for_tick_size()) << cached_metrics.best_ask
                        << " | Spread: " << std::setprecision(get_precision_for_tick_size()) << cached_metrics.spread << std::endl;
            } else {
                std::cout << "Spread not available (one or both sides of the book might be empty)." << std::endl;
            }
        }
                
        // Print asks (sell orders) - ASCENDING ORDER (low to high)
		
     	//* comment out up to this line to eliminate print orderbook
		
        std::cout << "\n--- ASKS --- (Lowest to Highest " << max_levels_to_print << ")" << std::endl;
        std::cout << std::setw(15) << "Price" << " | " 
                  << std::setw(15) << "Quantity" << " | "
                  << std::setw(15) << "USD Value" << " | " 
                  << "Source" << std::endl;
        std::cout << "----------------------------------------------------------------------" << std::endl;
        
        int ask_count = 0;
        for (auto it = asks.begin(); it != asks.end() && ask_count < max_levels_to_print; ++it) {
            double usd_value = it->first * it->second.first; // Price * Quantity = USD Value
            std::cout << std::setprecision(get_precision_for_tick_size()) << std::setw(15) << it->first
                    << " | "
                    << std::setprecision(5) << std::setw(15) << it->second.first
                    << " | "
                    << std::fixed << std::setprecision(2) << std::setw(15) << usd_value
                    << " | " << it->second.second << std::endl;
            ask_count++;
        }
        
        // Print bids (buy orders) - DESCENDING ORDER (high to low)
        std::cout << "\n--- BIDS --- (Highest to Lowest " << max_levels_to_print << ")" << std::endl;
        std::cout << std::setw(15) << "Price" << " | " 
                  << std::setw(15) << "Quantity" << " | "
                  << std::setw(15) << "USD Value" << " | " 
                  << "Source" << std::endl;
        std::cout << "----------------------------------------------------------------------" << std::endl;
        
        int bid_count = 0;
        // Using REVERSE iterator to show from highest to lowest
        for (auto it = bids.rbegin(); it != bids.rend() && bid_count < max_levels_to_print; ++it) {
            double usd_value = it->first * it->second.first; // Price * Quantity = USD Value
            std::cout << std::setprecision(get_precision_for_tick_size()) << std::setw(15) << it->first
                    << " | "
                    << std::setprecision(5) << std::setw(15) << it->second.first
                    << " | "
                    << std::fixed << std::setprecision(2) << std::setw(15) << usd_value
                    << " | " << it->second.second << std::endl;
            bid_count++;
        }
         //*/                  
		 
        // Print imbalance USING CACHED DATA
        if (imbalance_calculation_enabled) {
            std::cout << "\n--- ORDER BOOK IMBALANCE ---" << std::endl;
            std::lock_guard<std::mutex> metrics_lock(metrics_mutex);
            std::cout << "Top 2 Levels: " << std::fixed << std::setprecision(4) << cached_metrics.imbalance_2_levels 
                      << cached_metrics.interpretation_2 << std::endl;
            std::cout << "Top 10 Levels: " << std::fixed << std::setprecision(4) << cached_metrics.imbalance_10_levels 
                      << cached_metrics.interpretation_10 << std::endl;
            std::cout << "Top 20 Levels: " << std::fixed << std::setprecision(4) << cached_metrics.imbalance_20_levels 
                      << cached_metrics.interpretation_20 << std::endl;
            std::cout << "All Levels: " << std::fixed << std::setprecision(4) << cached_metrics.imbalance_all_levels 
                      << cached_metrics.interpretation_all << std::endl;
            
            std::cout << "Total Ask Liquidity: $" << std::fixed << std::setprecision(2) 
                      << cached_metrics.total_ask_liquidity << std::endl;
            std::cout << "Total Bid Liquidity: $" << std::fixed << std::setprecision(2) 
                      << cached_metrics.total_bid_liquidity << std::endl;
        }
        
        // Add trade information and volume metrics
        print_trades_and_volumes();
        std::cout << "\nCommands: 't <size>' to change tick size, 'p' toggle print, 'd' display once, 's' spread, 'l' list sizes, 'q' quit" << std::endl;
    }

public:
    // Toggle for imbalance calculation
    bool imbalance_calculation_enabled = true;

    void disable_imbalance_calculation() {
        imbalance_calculation_enabled = false;
    }
    
    void enable_imbalance_calculation() {
        imbalance_calculation_enabled = true;
    }

    // NEW: Control printing
    void enable_auto_print() { auto_print_enabled = true; }
    void disable_auto_print() { auto_print_enabled = false; }
    bool is_auto_print_enabled() const { return auto_print_enabled; }

    // NEW: Force a one-time display
    void force_display() {
        bool original_state = auto_print_enabled;
        auto_print_enabled = true;
        print_orderbook();
        auto_print_enabled = original_state;
    }

    // NEW: Access calculated data without printing
    OrderBookMetrics get_current_metrics() {
        calculate_all_metrics(); // Ensure fresh data
        std::lock_guard<std::mutex> lock(metrics_mutex);
        return cached_metrics;
    }

    // NEW: Get specific metrics quickly
    double get_current_spread() {
        auto metrics = get_current_metrics();
        return metrics.spread;
    }

    std::pair<double, double> get_best_bid_ask() {
        auto metrics = get_current_metrics();
        return {metrics.best_bid, metrics.best_ask};
    }

    BinanceOrderBook() {
        instance = this;
        curl_global_init(CURL_GLOBAL_DEFAULT);
        recent_trades.resize(max_trades_to_store); // Pre-allocate the ring buffer
    }
    
    ~BinanceOrderBook() {
        stop();
        curl_global_cleanup();
    }
    
    // Get appropriate precision for display based on tick size
    int get_precision_for_tick_size() const {
        if (std::abs(tick_size) < 1e-9) return 3;
        if (tick_size == 0.001) return 3;
        if (tick_size == 0.01) return 2;
        if (tick_size == 0.1) return 1;
        if (tick_size == 1.0 || tick_size == 10.0|| tick_size == 100.0) return 0;
        if (tick_size < 1.0) {
            return std::max(0, static_cast<int>(std::ceil(-std::log10(tick_size))));
        }
        return 0;
    }
    
    // Change the tick size
    void set_tick_size(double new_tick_size) {
        std::lock_guard<std::mutex> lock(orderbook_mutex);
        bool valid = false;
        for (double size : available_tick_sizes) {
            if (std::abs(new_tick_size - size) < 1e-6) {
                valid = true;
                break;
            }
        }
        
        if (valid) {
            tick_size = new_tick_size;
            std::cout << "Tick size set to: " << std::fixed 
                      << std::setprecision(get_precision_for_tick_size()) << tick_size << std::endl;
            
            // Re-aggregate the order book with the new tick size
            std::map<double, std::pair<double, std::string>> new_bids, new_asks;
            
            for (const auto& [price, qty_source] : bids) {
                double rounded_price = round_to_tick_size(price);
                // If a price level already exists, we add the quantity and keep the new source
                if (new_bids.count(rounded_price) > 0) {
                    new_bids[rounded_price].first += qty_source.first;
                } else {
                    new_bids[rounded_price] = qty_source;
                }
            }
            
            for (const auto& [price, qty_source] : asks) {
                double rounded_price = round_to_tick_size(price);
                // If a price level already exists, we add the quantity and keep the new source
                if (new_asks.count(rounded_price) > 0) {
                    new_asks[rounded_price].first += qty_source.first;
                } else {
                    new_asks[rounded_price] = qty_source;
                }
            }
            
            bids = std::move(new_bids);
            asks = std::move(new_asks);
            
            print_orderbook();
        } else {
            std::cout << "Invalid tick size. Available options: ";
            for (size_t i = 0; i < available_tick_sizes.size(); ++i) {
                std::cout << available_tick_sizes[i] << (i < available_tick_sizes.size() - 1 ? ", " : "");
            }
            std::cout << std::endl;
        }
    }
    
    // Get current tick size
    double get_tick_size() const {
        return tick_size;
    }
    
    // List available tick sizes
    void list_available_tick_sizes() const {
        std::cout << "Available tick sizes: ";
        for (size_t i = 0; i < available_tick_sizes.size(); ++i) {
            std::cout << available_tick_sizes[i] << (i < available_tick_sizes.size() - 1 ? ", " : "");
        }
        std::cout << std::endl;
    }
    
    // WebSocket callback - UNCHANGED FROM YOUR WORKING VERSION
    static int callback_binance(struct lws *wsi_in, enum lws_callback_reasons reason,
                               void *user, void *in, size_t len) {
        if (!instance) {
            return 0;
        }
        
        switch (reason) {
            case LWS_CALLBACK_CLIENT_ESTABLISHED:
                std::cout << "WebSocket connection established" << std::endl;
                break;

            case LWS_CALLBACK_CLIENT_RECEIVE:
                try {
                    if (!in || len == 0) {
                        return 0;
                    }
                    
                    instance->ws_buffer.append(static_cast<char*>(in), len);
                    
                    if (lws_is_final_fragment(wsi_in)) {
                        if (!instance->ws_buffer.empty()) {
                            // Use the message router instead of directly calling process_ws_update
                            instance->process_ws_message(instance->ws_buffer);
                            instance->ws_buffer.clear();
                        }
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Exception in CLIENT_RECEIVE: " << e.what() << std::endl;
                    instance->ws_buffer.clear();
                }
                break;
               
            case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
                std::cerr << "WebSocket connection error" << std::endl;
                instance->is_running.store(false);
                break;
                
            case LWS_CALLBACK_CLOSED:
                std::cout << "WebSocket connection closed" << std::endl;
                instance->is_running.store(false);
                break;
                
            default:
                break;
        }
        return 0;
    }
    
    // Start the order book service - UNCHANGED FROM YOUR WORKING VERSION
    void start() {
        if (is_running.load()) return;
        is_running.store(true);
        
        // First fetch the snapshot via REST API
        fetch_api_snapshot();
        
        // Start WebSocket thread
        ws_thread = std::thread([this]() {
            lws_set_log_level(LLL_ERR | LLL_WARN, NULL);

            struct lws_context_creation_info info;
            memset(&info, 0, sizeof(info));
            info.port = CONTEXT_PORT_NO_LISTEN;
            info.protocols = BinanceOrderBook::protocols;
            info.gid = -1;
            info.uid = -1;
            info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
            
            const char* ca_path = "/etc/ssl/certs/ca-certificates.crt";
            if (access(ca_path, F_OK) != -1) {
                info.ssl_ca_filepath = ca_path;
            }
            
            context = lws_create_context(&info);
            if (!context) {
                std::cerr << "Failed to create WebSocket context." << std::endl;
                is_running.store(false);
                return;
            }
            
            struct lws_client_connect_info ccinfo;
            memset(&ccinfo, 0, sizeof(ccinfo));
            ccinfo.context = context;
            ccinfo.address = "stream.binance.us";
            ccinfo.port = 9443;
            // UNCHANGED - Your working WebSocket path
            ccinfo.path = "/ws/btcusdc@depth@100ms/btcusdc@trade";
            ccinfo.host = "stream.binance.us";
            ccinfo.origin = "stream.binance.us";
            ccinfo.protocol = "binance-websocket";
            ccinfo.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;

            std::cout << "Connecting to WebSocket..." << std::endl;
            wsi = lws_client_connect_via_info(&ccinfo);
            if (!wsi) {
                std::cerr << "Failed to connect to WebSocket." << std::endl;
                if (context) lws_context_destroy(context);
                context = nullptr;
                is_running.store(false);
                return;
            }
            
            // Service loop
            while (is_running.load()) {
                int n = lws_service(context, 30);
                if (n < 0) {
                    std::cerr << "WebSocket service error. Reconnecting..." << std::endl;
                    // Try to reconnect
                    if (wsi) wsi = nullptr;
                    wsi = lws_client_connect_via_info(&ccinfo);
                    if (!wsi) {
                        std::cerr << "Failed to reconnect." << std::endl;
                        break;
                    }
                }
            }
            
            if (context) {
                lws_context_destroy(context);
                context = nullptr;
            }
            wsi = nullptr;
        });
        
        // Start API thread for periodic snapshots
        api_thread = std::thread([this]() {
            // Wait for initial connection
            std::this_thread::sleep_for(std::chrono::seconds(2));
            
            while (is_running.load()) {
                // Fetch a new snapshot every 30 seconds to ensure sync
                for (int i = 0; i < 300 && is_running.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                
                if (is_running.load()) {
                    fetch_api_snapshot();
                }
            }
        });
    }
    
    // Stop the order book service - UNCHANGED
    void stop() {
        if (!is_running.load()) return;
        
        std::cout << "Stopping order book service..." << std::endl;
        is_running.store(false);
        
        if (context) {
            lws_cancel_service(context);
        }
        
        if (ws_thread.joinable()) {
            ws_thread.join();
        }
        
        if (api_thread.joinable()) {
            api_thread.join();
        }
        
        std::cout << "Order book service stopped." << std::endl;
    }
};

// Static member initialization - UNCHANGED
BinanceOrderBook* BinanceOrderBook::instance = nullptr;
struct lws_protocols BinanceOrderBook::protocols[] = {
    {
        "binance-websocket",
        BinanceOrderBook::callback_binance,
        0,
        16384,
        0, NULL, 0
    },
    { NULL, NULL, 0, 0, 0, NULL, 0 }
};

int main() {
    try {
        BinanceOrderBook orderbook;
		orderbook.enable_imbalance_calculation();
        
        std::cout << "Starting BTC/USDC OrderBook with API and WebSocket integration." << std::endl;
        orderbook.list_available_tick_sizes();
        std::cout << "Current tick size: " << std::fixed 
                  << std::setprecision(orderbook.get_precision_for_tick_size()) 
                  << orderbook.get_tick_size() << std::endl;
        
        orderbook.start();
        
        std::string command;
        while (true) {
            std::cout << "\nEnter command (t <size>/i/p/d/s/m/l/q): ";  // NEW COMMANDS ADDED
            if (!std::getline(std::cin, command)) {
                if (std::cin.eof()) {
                    std::cout << "EOF detected, quitting." << std::endl;
                } else {
                    std::cerr << "Input error, quitting." << std::endl;
                }
                break;
            }
            
            if (command == "q" || command == "quit") {
                break;
            } else if (command == "l" || command == "list") {
                orderbook.list_available_tick_sizes();
            } else if (command == "i" || command == "imbalance") {
                // Toggle imbalance calculation
                if (orderbook.imbalance_calculation_enabled) {
                    orderbook.disable_imbalance_calculation();
                    std::cout << "Order book imbalance display: DISABLED" << std::endl;
                } else {
                    orderbook.enable_imbalance_calculation();
                    std::cout << "Order book imbalance display: ENABLED" << std::endl;
                }
            } else if (command == "p" || command == "print") {
                // NEW: Toggle auto-print functionality
                if (orderbook.is_auto_print_enabled()) {
                    orderbook.disable_auto_print();
                    std::cout << "Auto-print: DISABLED (calculations continue)" << std::endl;
                } else {
                    orderbook.enable_auto_print();
                    std::cout << "Auto-print: ENABLED" << std::endl;
                }
            } else if (command == "d" || command == "display") {
                // NEW: Force display once regardless of auto-print setting
                orderbook.force_display();
            } else if (command == "s" || command == "spread") {
                // NEW: Show current spread without full display
                double spread = orderbook.get_current_spread();
                auto bid_ask = orderbook.get_best_bid_ask();
                std::cout << "Current Best Bid: " << std::fixed 
                          << std::setprecision(orderbook.get_precision_for_tick_size()) 
                          << bid_ask.first << std::endl;
                std::cout << "Current Best Ask: " << std::fixed 
                          << std::setprecision(orderbook.get_precision_for_tick_size()) 
                          << bid_ask.second << std::endl;
                std::cout << "Current Spread: " << std::fixed 
                          << std::setprecision(orderbook.get_precision_for_tick_size()) 
                          << spread << std::endl;
            } else if (command == "m" || command == "metrics") {
                // NEW: Show current metrics without full display
                auto metrics = orderbook.get_current_metrics();
                std::cout << "\n--- CURRENT METRICS ---" << std::endl;
                std::cout << "Best Bid: " << std::fixed 
                          << std::setprecision(orderbook.get_precision_for_tick_size()) 
                          << metrics.best_bid << std::endl;
                std::cout << "Best Ask: " << std::fixed 
                          << std::setprecision(orderbook.get_precision_for_tick_size()) 
                          << metrics.best_ask << std::endl;
                std::cout << "Spread: " << std::fixed 
                          << std::setprecision(orderbook.get_precision_for_tick_size()) 
                          << metrics.spread << std::endl;
                
                if (orderbook.imbalance_calculation_enabled) {
                    std::cout << "\n--- IMBALANCE METRICS ---" << std::endl;
                    std::cout << "Top 5 Levels: " << std::fixed << std::setprecision(4) 
                              << metrics.imbalance_2_levels 
                              << metrics.interpretation_2 << std::endl;
                    std::cout << "Top 10 Levels: " << metrics.imbalance_10_levels 
                              << metrics.interpretation_10 << std::endl;
                    std::cout << "Top 20 Levels: " << metrics.imbalance_20_levels 
                              << metrics.interpretation_20 << std::endl;
                    std::cout << "All Levels: " << metrics.imbalance_all_levels 
                              << metrics.interpretation_all << std::endl;
                    std::cout << "Total Ask Liquidity: $" << std::fixed << std::setprecision(2) 
                              << metrics.total_ask_liquidity << std::endl;
                    std::cout << "Total Bid Liquidity: $" << std::fixed << std::setprecision(2) 
                              << metrics.total_bid_liquidity << std::endl;
                }
            } else if (command.rfind("t ", 0) == 0) {
                try {
                    if (command.length() > 2) {
                        double new_tick_size = std::stod(command.substr(2));
                        orderbook.set_tick_size(new_tick_size);
                    } else {
                        std::cout << "Invalid tick size format. Use: t <number>" << std::endl;
                    }
                } catch (const std::invalid_argument& ia) {
                    std::cerr << "Invalid number for tick size: " << ia.what() << std::endl;
                } catch (const std::out_of_range& oor) {
                    std::cerr << "Number out of range for tick size: " << oor.what() << std::endl;
                }
            } else if (!command.empty()) {
                std::cout << "Unknown command. Available commands:" << std::endl;
                std::cout << "  t <size> - Set tick size (e.g., t 0.1)" << std::endl;
                std::cout << "  i        - Toggle imbalance calculation" << std::endl;
                std::cout << "  p        - Toggle auto-print (calculations continue)" << std::endl;
                std::cout << "  d        - Force display once" << std::endl;
                std::cout << "  s        - Show current spread and best bid/ask" << std::endl;
                std::cout << "  m        - Show current metrics summary" << std::endl;
                std::cout << "  l        - List available tick sizes" << std::endl;
                std::cout << "  q        - Quit" << std::endl;
            }
        }
        
        std::cout << "Main loop finished. Stopping order book..." << std::endl;
        orderbook.stop();
        std::cout << "Orderbook stopped. Exiting application." << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}