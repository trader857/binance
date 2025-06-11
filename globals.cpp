#include <atomic>
#include "core/ts_queue.hpp"
#include "core/serialization.hpp"

// Global variables used across multiple files
std::atomic<bool> stop_flag(false);
TSQueue<OrderBookUpdate> iceberg_queue;
