#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

// Single-producer single-consumer bounded ring buffer.
//
// Simpler than the MPSC inbound ring: only one writer and one reader, so no CAS
// is needed — each side is the sole owner of its pointer. The two pointers are
// atomic only to communicate progress across the producer/consumer threads.
//
// Capacity must be a power of two (bitmask wrap, same trick as RingBuffer_inbound).
//
// Overflow policy: try_push returns false when full. Caller decides whether to
// drop, retry, or count the miss. We never overwrite an unread slot.
template <typename T>
class SpscQueue {
public:
    explicit SpscQueue(int64_t capacity)
        : capacity_(capacity), mask_(capacity - 1),
          buffer_(std::make_unique<T[]>(capacity)) {}

    // Producer side. Returns false if the queue is full.
    bool try_push(const T& value) {
        const int64_t head = head_.load(std::memory_order_relaxed);
        const int64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= capacity_) return false;  // full
        buffer_[head & mask_] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false if the queue is empty.
    bool try_pop(T& out) {
        const int64_t tail = tail_.load(std::memory_order_relaxed);
        const int64_t head = head_.load(std::memory_order_acquire);
        if (tail >= head) return false;  // empty
        out = buffer_[tail & mask_];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    int64_t capacity() const { return capacity_; }

private:
    const int64_t capacity_;
    const int64_t mask_;
    std::unique_ptr<T[]> buffer_;
    // head = producer's write pointer, tail = consumer's read pointer.
    // Aligned to dodge false sharing between producer and consumer.
    alignas(64) std::atomic<int64_t> head_{0};
    alignas(64) std::atomic<int64_t> tail_{0};
};
