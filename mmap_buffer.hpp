#pragma once
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <stdexcept>

class MMapBuffer {
public:
    explicit MMapBuffer(size_t capacity);
    // Add constructor with read mode flag
    explicit MMapBuffer(size_t capacity, bool read_only);
    ~MMapBuffer();

    // Write raw data into ring buffer
    // Returns number of bytes actually written (0 if no space)
    size_t write(const uint8_t* data, size_t len);

    // Read raw data from ring buffer
    // Returns number of bytes actually read (0 if empty)
    size_t read(uint8_t* out, size_t max_len);
    
    // Check if this buffer is in read-only mode
    bool is_read_only() const { return read_only_; }

private:
    uint8_t* buffer_;
    size_t capacity_;
    std::atomic<size_t> head_;
    std::atomic<size_t> tail_;
    bool read_only_ = false;  // Default to write mode
};
