#include "ring_buffer.hpp"

RingBuffer_inbound::RingBuffer_inbound(int64_t buffer_size): size(buffer_size), mask(buffer_size-1){
    buffer = std::make_unique<Orderevent_inbound[]>(size);
}

int64_t RingBuffer_inbound::claim(){    // the gateway is trying to claim a ticket here
    for (;;) {
        int64_t w = write_pointer.load(std::memory_order_relaxed);
        // Backpressure: spin while the target slot still holds an unconsumed event.
        // Acquire pairs with the consumer's release-store of is_ready=false.
        while (buffer[w & mask].is_ready.load(std::memory_order_acquire)) {
            w = write_pointer.load(std::memory_order_relaxed);
        }
        // CAS instead of fetch_add: if another producer advanced write_pointer
        // between our peek and our claim, retry against the new target slot.
        if (write_pointer.compare_exchange_weak(
                w, w + 1,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return w;
        }
    }
}

Orderevent_inbound* RingBuffer_inbound::get(int64_t write_p){
    return &buffer[write_p & mask];
}

void RingBuffer_inbound::publish(int64_t write_p){ // publish the orderevent, telling the matching engine that we are ready 
    buffer[write_p & mask].is_ready.store(true, std::memory_order_release);
}

void RingBuffer_inbound::release(int64_t write_p){ // matching engine has done all the work and reset the value to false
    buffer[write_p & mask].is_ready.store(false, std::memory_order_release);
}


RingBuffer_outbound::RingBuffer_outbound(int64_t buffer_size): size(buffer_size), mask(buffer_size-1){
    buffer = std::make_unique<Orderevent_outbound[]>(size);
}

Orderevent_outbound* RingBuffer_outbound::get(int64_t write_p){
    return &buffer[write_p & mask];
}

void RingBuffer_outbound::publish(int64_t write_p){
    buffer[write_p & mask].is_ready.store(true, std::memory_order_release);
    return;
}

void RingBuffer_outbound::release(int64_t write_p){
    buffer[write_p & mask].is_ready.store(false, std::memory_order_release);
}