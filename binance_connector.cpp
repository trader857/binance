#include "io/binance_connector.hpp"
#include <libwebsockets.h>
#include <iostream>
#include <string>
#include <cstring>
#include <iomanip>  // For std::fixed and std::setprecision
#include "core/serialization.hpp"
#include "io/mmap_buffer.hpp"
#include "core/ts_queue.hpp"
#include <vector>
#include <memory>
#include <algorithm>
#include <optional>
#include <stdexcept>

// External queue declarations
extern TSQueue<OrderBookUpdate> liquidity_queue;
extern TSQueue<TradeMessageBinary> trade_queue;
extern TSQueue<OrderBookUpdate> iceberg_queue;

// Memory-mapped buffer for efficient data storage
static MMapBuffer mmap_buffer(4096); // Size in bytes, adjust to your needs

// Define message type identifiers
enum MessageType : uint8_t {
    TYPE_TRADE = 0x01,
    TYPE_ORDERBOOK = 0x02
};

// WebSocket callback function
static int callback_ws(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len) {
    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            std::cout << "[WebSocket] Connected to Binance" << std::endl;
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE: {
            std::string json_str(reinterpret_cast<char*>(in), len);

            try {
                // Check if this is a trade message
                if (json_str.find("\"e\":\"trade\"") != std::string::npos) {
                    TradeMessageBinary trade_msg = Serialization::parse_trade_json(json_str);
                    trade_queue.push(trade_msg);
                    std::cout << "[DEBUG] Trade message received: Price = " << trade_msg.price
                              << ", Quantity = " << trade_msg.quantity
                              << ", IsBuy = " << trade_msg.is_buy() << std::endl;
                }
                // Check if this is an order book update
                else if (json_str.find("\"e\":\"depthUpdate\"") != std::string::npos) {
                    std::cout << "[DEBUG] Received depth update JSON: " << json_str << std::endl;

                    auto book_opt = Serialization::parse_orderbook_json(json_str);
                    if (!book_opt.has_value()) {
                        std::cerr << "[ERROR] Failed to parse depth update JSON: " << json_str << std::endl;
                    } else {
                        OrderBookUpdate book = book_opt.value();
                        liquidity_queue.push(book);
                        iceberg_queue.push(book);
                        std::cout << "[DEBUG] Parsed depth update and pushed to queues." << std::endl;
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "[Error] Failed to process WebSocket message: " << e.what() << std::endl;
            }

            break;
        }

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            std::cerr << "[WebSocket] Connection error" << std::endl;
            break;

        case LWS_CALLBACK_CLIENT_CLOSED:
            std::cout << "[WebSocket] Connection closed" << std::endl;
            break;

        default:
            break;
    }

    return 0;
}

// BinanceConnector class methods
void BinanceConnector::run() {
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));

    struct lws_protocols protocols[] = {
        { "ws", callback_ws, 0, 65536 },
        { NULL, NULL, 0, 0 }
    };
    
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    struct lws_context *context = lws_create_context(&info);
    if (!context) {
        std::cerr << "[WebSocket] Failed to create context" << std::endl;
        return;
    }

    struct lws_client_connect_info ccinfo = {};
    ccinfo.context = context;
    ccinfo.address = "stream.binance.us";
    ccinfo.port = 9443;
    ccinfo.path = "/ws/btcusdt@trade/btcusdt@depth50@100ms"; // Combined trade and depth streams
    ccinfo.host = ccinfo.address;
    ccinfo.origin = "origin";
    ccinfo.protocol = protocols[0].name;
    ccinfo.ssl_connection = LCCSCF_USE_SSL;

    if (!lws_client_connect_via_info(&ccinfo)) {
        std::cerr << "[WebSocket] Connection initiation failed" << std::endl;
        lws_context_destroy(context);
        return;
    }

    running = true;

    while (running) {
        lws_service(context, 100);
    }

    lws_context_destroy(context);
}

BinanceConnector::BinanceConnector() {
    running = false;
}

BinanceConnector::~BinanceConnector() {
    stop();
}

void BinanceConnector::start() {
    running = true;
    run();
}

void BinanceConnector::stop() {
    running = false;
}
