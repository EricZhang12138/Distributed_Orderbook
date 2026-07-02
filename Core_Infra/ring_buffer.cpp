#include "ring_buffer.hpp"

RingBuffer_inbound::RingBuffer_inbound(int64_t buffer_size): size(buffer_size), mask(buffer_size-1){
    buffer = std::make_unique<Orderevent_inbound[]>(size);
}

// Multi-producer claim. Returns the sequence number of the slot the caller now owns.
//
// Protocol (Disruptor-style):
//   1. Read the current write_pointer to pick a target slot.
//   2. Spin until that slot is empty (the consumer has released it).
//   3. Atomically advance write_pointer (CAS) to claim the slot.
//   4. On CAS failure, another producer beat us — retry with the new target.
int64_t RingBuffer_inbound::claim() {
    while (true) {
        int64_t target_seq = write_pointer.load(std::memory_order_relaxed);

        // Backpressure: wait for the consumer to release this slot.
        // Acquire pairs with the consumer's release-store of is_ready = false,
        // ordering the consumer's reads of the previous occupant before our writes.
        while (buffer[target_seq & mask].is_ready.load(std::memory_order_acquire)) {
            target_seq = write_pointer.load(std::memory_order_relaxed);
        }

        // compare_exchange_weak writes the live value back into target_seq on
        // failure, so the next iteration naturally re-peeks the new slot.
        const bool claimed = write_pointer.compare_exchange_weak(
            target_seq, target_seq + 1,
            std::memory_order_relaxed,
            std::memory_order_relaxed);

        if (claimed) {
            return target_seq;
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