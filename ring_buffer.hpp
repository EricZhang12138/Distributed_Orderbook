#pragma once
#include <unordered_map>
#include <vector>
#include <atomic>
#include <cstdint>
#include <thread>


// 64 bytes
struct alignas(64) Orderevent_inbound{
    int64_t price = 0;
    bool side = true;
    int64_t volume = 0;
    int64_t order_arrival_time = 0;
    int64_t internal_order_id = 0;
    int64_t gateway_id = 0;
    // is_ready is basically used to signal to the engine that this orderevent is ready for you to process
    std::atomic<bool> is_ready{false};   // this needs to be atomic because the matching engine and the gateway may access it at the same time
};
// the ring buffer also works as a sequencer, multiple requests coming but they are sequenced when they come in the ring buffer
struct RingBuffer_inbound{
    std::vector<Orderevent_inbound> buffer; // we use the actual Orderevent object here rather than pointers for cache friendliness
    int64_t size = 0;
    // since we are using the mask in place of modulo operation, the size of buffer has to be powers of 2
    int64_t mask = 0;
    std::atomic<int64_t> write_pointer = 0;  // this is the write_pointer and it constantly goes up

    RingBuffer_inbound(int64_t buffer_size);
    int64_t claim();
    Orderevent_inbound* get(int64_t write_p);
    void publish(int64_t write_p);
    void release(int64_t write_p);
};

struct RingBuffer_outbound{

};
