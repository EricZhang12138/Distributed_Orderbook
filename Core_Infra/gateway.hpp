#pragma once
#include "ring_buffer.hpp"
#include "orderbook.hpp"

// claim() a slot → get() the pointer to that slot → fill in the order data → publish() to signal it's ready. 
// The matching engine on the other side polls the buffer checking is_ready and calls release() when done.

class Gateway{ // a wrapper around the Orderbook object. It converts user facing ID (username + id) to internal orderbook id 
private:
    int64_t gateway_id = 0;
    int64_t outbound_ringbuffer_read_p = 0; // used along with read_from_ring_buffer
    std::mutex id_lock; // for id_map
    std::unordered_map<std::string, int> id_map;
    RingBuffer_inbound* ring_buffer_internal; // we use a raw pointer instead of unique pointer because the gateway shouldn't own the ringbuffer, it is a shared resource
    RingBuffer_outbound* ring_buffer_outbound;
    std::atomic<int64_t> internal_id_counter = 0; 
public:
    Gateway(RingBuffer_inbound* buffer_inbound, RingBuffer_outbound* ring_buffer_outbound,int64_t id);
    bool read_from_ring_buffer(Fill* fills, int64_t& fill_count, int64_t& order_id, bool& fulfilled, int64_t& remaining_qty);
    void place_order_to_ring_buffer(int64_t price,int64_t volume, bool side, std::string username);
};

