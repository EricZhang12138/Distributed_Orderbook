#pragma once
#include <unordered_map>
#include <vector>
#include <atomic>
#include <cstdint>
#include <thread>
class gateway{ // a wrapper around the Orderbook object. It converts user facing ID (username + id) to internal orderbook id 
    std::unordered_map<std::string, int> id_map;
    RingBuffer* ring_buffer; // we use a raw pointer instead of unique pointer because the gateway shouldn't own the ringbuffer, it is a shared resource
    gateway(RingBuffer* buffer): ring_buffer(buffer){};
    
};



// currently the ring buffer does not 
struct alignas(64) Orderevent{
    int64_t price = 0;
    bool side = true;
    int64_t volume = 0;
    int64_t timestamp = 0;
    int64_t internal_order_id = 0;
    // is_ready is basically used to signal to the engine that this orderevent is ready for you to process
    std::atomic<bool> is_ready{false};   // this needs to be atomic because the matching engine and the gateway may access it at the same time

};
// the ring buffer also works as a sequencer, multiple requests coming but they are sequenced when they come in the ring buffer
struct RingBuffer{
    std::vector<Orderevent> buffer; // we use the actual Orderevent object here rather than pointers for cache friendliness
    int64_t size = 0;
    // since we are using the mask in place of modulo operation, the size of buffer has to be powers of 2
    int64_t mask = 0;
    std::atomic<int64_t> write_pointer = 0;  // this is the write_pointer and it constantly goes up

    RingBuffer(int64_t buffer_size): size(buffer_size), mask(buffer_size-1){
        buffer.resize(size);
    }
    int64_t claim(){    // the gateway is trying to claim a ticket here
        return write_pointer.fetch_add(1, std::memory_order_relaxed);
    }
    Orderevent* get(int64_t write_p){
        return &buffer[write_p & mask];
    }
    void publish(int64_t write_p){ // publish the orderevent, telling the matching engine that we are ready 
        buffer[write_p & mask].is_ready.store(true, std::memory_order_release);
    }
    void release(int64_t write_p){ // matching engine has done all the work and reset the value to false
        buffer[write_p & mask].is_ready.store(false, std::memory_order_relaxed);
    }
};

