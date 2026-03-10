#include "ring_buffer.hpp"

RingBuffer_inbound::RingBuffer_inbound(int64_t buffer_size): size(buffer_size), mask(buffer_size-1){
    buffer.resize(size);
}

int64_t RingBuffer_inbound::claim(){    // the gateway is trying to claim a ticket here
    return write_pointer.fetch_add(1, std::memory_order_relaxed);
}

Orderevent_inbound* RingBuffer_inbound::get(int64_t write_p){
    return &buffer[write_p & mask];
}

void RingBuffer_inbound::publish(int64_t write_p){ // publish the orderevent, telling the matching engine that we are ready 
    buffer[write_p & mask].is_ready.store(true, std::memory_order_release);
}

void RingBuffer_inbound::release(int64_t write_p){ // matching engine has done all the work and reset the value to false
    buffer[write_p & mask].is_ready.store(false, std::memory_order_relaxed);
}


RingBuffer_outbound::RingBuffer_outbound(int64_t buffer_size): size(buffer_size), mask(buffer_size-1){
    buffer.resize(size);
}

Orderevent_outbound* RingBuffer_outbound::get(int64_t write_p){
    return &buffer[write_p & mask];
}

void RingBuffer_outbound::publish(int64_t write_p){
    buffer[write_p & mask].is_ready.store(true, std::memory_order_release);
    return;
}

void RingBuffer_outbound::release(int64_t write_p){
    buffer[write_p & mask].is_ready.store(false, std::memory_order_relaxed);
}