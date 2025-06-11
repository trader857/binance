#include "io/mmap_buffer.hpp"
#include <algorithm> // std::min
#include <cstring>   // memcpy

MMapBuffer::MMapBuffer(size_t capacity)
    : capacity_(capacity), head_(0), tail_(0), read_only_(false) {
    buffer_ = new uint8_t[capacity_];
    if (!buffer_) throw std::runtime_error("Failed to allocate ring buffer");
}

MMapBuffer::MMapBuffer(size_t capacity, bool read_only)
    : capacity_(capacity), head_(0), tail_(0), read_only_(read_only) {
    buffer_ = new uint8_t[capacity_];
    if (!buffer_) throw std::runtime_error("Failed to allocate ring buffer");
}

MMapBuffer::~MMapBuffer() {
    delete[] buffer_;
}

size_t MMapBuffer::write(const uint8_t* data, size_t len) {
    // Prevent writes if in read-only mode
    if (read_only_) {
        return 0;
    }
    
    size_t head = head_.load(std::memory_order_relaxed);
    size_t tail = tail_.load(std::memory_order_acquire);

    size_t space = (tail + capacity_ - head - 1) % capacity_;
    size_t to_write = std::min(len, space);

    size_t first_chunk = std::min(to_write, capacity_ - (head % capacity_));
    std::memcpy(buffer_ + (head % capacity_), data, first_chunk);

    size_t second_chunk = to_write - first_chunk;
    if (second_chunk > 0) {
        std::memcpy(buffer_, data + first_chunk, second_chunk);
    }

    head_.store((head + to_write) % capacity_, std::memory_order_release);
    return to_write;
}

size_t MMapBuffer::read(uint8_t* out, size_t max_len) {
    size_t head = head_.load(std::memory_order_acquire);
    size_t tail = tail_.load(std::memory_order_relaxed);

    size_t available = (head + capacity_ - tail) % capacity_;
    size_t to_read = std::min(max_len, available);

    size_t first_chunk = std::min(to_read, capacity_ - (tail % capacity_));
    std::memcpy(out, buffer_ + (tail % capacity_), first_chunk);

    size_t second_chunk = to_read - first_chunk;
    if (second_chunk > 0) {
        std::memcpy(out + first_chunk, buffer_, second_chunk);
    }

    tail_.store((tail + to_read) % capacity_, std::memory_order_release);
    return to_read;
}
