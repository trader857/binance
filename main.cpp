#include <iostream>
#include <thread>
#include <atomic>
#include <iomanip>
#include <csignal>
#include "io/binance_connector.hpp"
#include "io/mmap_buffer.hpp"
#include "io/ring_buffer_consumer.hpp"
#include "features/IcebergDetector.hpp"
#include "features/liquidity_tracker.hpp"
#include "core/ts_queue.hpp"

extern std::atomic<bool> stop_flag;
extern TSQueue<OrderBookUpdate> iceberg_queue;

// New queues for liquidity tracking
TSQueue<OrderBookUpdate> liquidity_queue;
TSQueue<TradeMessageBinary> trade_queue;

int main() {
    BinanceConnector connector;
    IcebergDetector iceberg_detector;

    // Initialize the liquidity tracker
    LiquidityTracker liquidity_tracker(
        10000.0, // buy bucket size
        10000.0, // sell bucket size
        5000.0,  // cancel bucket size
        30,      // depth_levels_track
        20,      // depth_levels_report
        0.01     // tick_size (price resolution)
    );
    liquidity_tracker.setTickSize(0.01); // Adjust tick size as needed

    // Print bucket-level statistics
    liquidity_tracker.setBuyBucketCallback([](bool is_buy, uint64_t duration_ns, double bucket_size, double ratio) {
        std::cout << (is_buy ? "[BUY BUCKET]" : "[SELL BUCKET]") << " $" << bucket_size
                  << " filled in " << (duration_ns / 1e6) << " ms, "
                  << "Buy/Sell ratio: " << std::setprecision(3) << ratio << std::endl;
    });

    liquidity_tracker.setSellBucketCallback([](bool is_buy, uint64_t duration_ns, double bucket_size, double ratio) {
        std::cout << (is_buy ? "[BUY BUCKET]" : "[SELL BUCKET]") << " $" << bucket_size
                  << " filled in " << (duration_ns / 1e6) << " ms, "
                  << "Sell/Buy ratio: " << std::setprecision(3) << ratio << std::endl;
    });

    liquidity_tracker.setCancelBuyBucketCallback([](bool is_buy, uint64_t duration_ns, double bucket_size, double ratio) {
        std::cout << (is_buy ? "[CANCEL BUY BUCKET]" : "[CANCEL SELL BUCKET]") << " $" << bucket_size
                  << " cancelled in " << (duration_ns / 1e6) << " ms, "
                  << "Cancel ratio: " << std::setprecision(3) << ratio << std::endl;
    });

    liquidity_tracker.setCancelSellBucketCallback([](bool is_buy, uint64_t duration_ns, double bucket_size, double ratio) {
        std::cout << (is_buy ? "[CANCEL BUY BUCKET]" : "[CANCEL SELL BUCKET]") << " $" << bucket_size
                  << " cancelled in " << (duration_ns / 1e6) << " ms, "
                  << "Cancel ratio: " << std::setprecision(3) << ratio << std::endl;
    });

    std::thread ws_thread([&connector]() {
        connector.start();
    });

    std::thread consumer_thread(consume_ring_buffer);

    std::thread iceberg_thread([&]() {
        while (true) {
            auto update_opt = iceberg_queue.pop();
            if (!update_opt.has_value())
                break;
            iceberg_detector.process_update(update_opt.value());
        }
    });

    // Add liquidity tracker thread
    std::thread liquidity_thread([&]() {
        while (true) {
            // Process order book updates
            auto update_opt = liquidity_queue.try_pop();
            if (update_opt.has_value()) {
                OrderBookUpdate& update = update_opt.value();
                std::vector<OrderBookLevel> bids, asks;
                for (const auto& bid : update.bids)
                    bids.push_back({bid.price, bid.quantity});
                for (const auto& ask : update.asks)
                    asks.push_back({ask.price, ask.quantity});
                liquidity_tracker.onOrderBookUpdate(update.timestamp_ns, bids, asks);
            }
            // Process trades
            auto trade_opt = trade_queue.try_pop();
            if (trade_opt.has_value()) {
                TradeMessageBinary& trade = trade_opt.value();
                std::cout << "[DEBUG] TradeMessage received. Price: " << trade.price
                          << ", Quantity: " << trade.quantity << ", IsBuy: " << trade.is_buy() << std::endl;
                liquidity_tracker.onTrade(trade);
            }
            // Exit condition
            if (liquidity_queue.is_closed() && liquidity_queue.empty() &&
                trade_queue.is_closed() && trade_queue.empty() &&
                stop_flag.load(std::memory_order_acquire)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::cout << "[Liquidity Tracker] Thread stopped" << std::endl;
    });

    std::cout << "Binance Processor started. Press Enter to stop...\n";
    std::cin.get();

    std::cout << "Stopping Binance Processor...\n";

    connector.stop();
    if (ws_thread.joinable()) ws_thread.join();
    stop_flag.store(true, std::memory_order_release);
    if (consumer_thread.joinable()) consumer_thread.join();

    iceberg_queue.close();
    liquidity_queue.close();
    trade_queue.close();

    if (iceberg_thread.joinable()) iceberg_thread.join();
    if (liquidity_thread.joinable()) liquidity_thread.join();

    std::cout << "Binance Processor stopped.\n";
    return 0;
}