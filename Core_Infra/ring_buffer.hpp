#pragma once
#include <unordered_map>
#include <memory>
#include <atomic>
#include <cstdint>
#include <thread>
#include "orderbook.hpp"

enum class Op : uint8_t { NEW = 0, CANCEL = 1, MODIFY = 2 };

// 64 bytes
struct alignas(64) Orderevent_inbound{
    Op op = Op::NEW;            // distinguishes new orders from cancels
    int64_t price = 0;
    bool side = true;
    int64_t volume = 0;
    int64_t order_arrival_time = 0;
    int64_t internal_order_id = 0;
    int64_t gateway_id = 0;
    bool silent = false;        // if true, engine skips the outbound ack write for this event
    // is_ready is basically used to signal to the engine that this orderevent is ready for you to process
    std::atomic<bool> is_ready{false};   // this needs to be atomic because the matching engine and the gateway may access it at the same time
};
// the ring buffer also works as a sequencer, multiple requests coming but they are sequenced when they come in the ring buffer
struct RingBuffer_inbound{
    std::unique_ptr<Orderevent_inbound[]> buffer; // we use the actual Orderevent object here rather than pointers for cache friendliness
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


struct alignas(64) Orderevent_outbound{
      Op op = Op::NEW;           // which op produced this ack — disambiguates the meaning of `fulfilled`
      int64_t order_id = 0;
      int64_t gateway_id = 0;
      bool fulfilled = false;    // NEW: fully filled? CANCEL/MODIFY: succeeded?
      int64_t remaining_qty = 0;
      Fill fills[16]; // 16 fills max each time
      int64_t fill_count = 0;
      std::atomic<bool> is_ready{false};
  };

struct RingBuffer_outbound{
    std::unique_ptr<Orderevent_outbound[]> buffer;
    int64_t size = 0;
    int64_t mask = 0;
    RingBuffer_outbound(int64_t buffer_size);
    Orderevent_outbound* get(int64_t write_p);
    void publish(int64_t write_p); // set is_ready = true for the user to pick up
    void release(int64_t write_p);  // set is_ready = false to free up the space
};
