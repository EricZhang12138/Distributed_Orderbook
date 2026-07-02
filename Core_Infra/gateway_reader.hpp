#pragma once
#include "gateway.hpp"
#include "ring_buffer.hpp"
#include "spsc_queue.hpp"
#include <atomic>
#include <thread>

// Plain copyable mirror of Orderevent_outbound, suitable for queue storage.
// Orderevent_outbound itself contains a std::atomic<bool> is_ready and so is
// not copyable — we reassemble the fields here.
struct Ack {
    Op op = Op::NEW;
    int64_t order_id = 0;
    bool fulfilled = false;
    int64_t remaining_qty = 0;
    int64_t fill_count = 0;
    Fill fills[16]{};
};

// Owns a thread that continuously drains a Gateway's outbound ring buffer
// into a caller-provided SpscQueue<Ack>. Decouples the engine's write rate
// from the consumer's processing rate.
//
// Overflow policy: drop-newest. If the queue is full when the reader tries to
// push, the ack is discarded and dropped_count_ is incremented. The reader
// itself never blocks the engine.
class GatewayReader {
public:
    GatewayReader(Gateway* gw, SpscQueue<Ack>* queue);
    ~GatewayReader();

    GatewayReader(const GatewayReader&) = delete;
    GatewayReader& operator=(const GatewayReader&) = delete;

    int64_t dropped_count() const { return dropped_count_.load(std::memory_order_relaxed); }

private:
    void run();

    Gateway* gw_;
    SpscQueue<Ack>* queue_;
    std::atomic<bool> stop_{false};
    std::atomic<int64_t> dropped_count_{0};
    std::thread thread_;
};
