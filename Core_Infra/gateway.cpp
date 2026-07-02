#include "gateway.hpp"
#include <cassert>
//         Gateway Thread                 Matching Engine Thread                                                                                                                      
//  ──────────────────────────────    ──────────────────────────────                                                                                                            
//  claim()  → slot = 5                                                                                                                                                           
//  get(5)   → fill event data                                                                                                                                                  
//  publish(5) → is_ready = true                                                                                                                                                  
//                                    poll slot 5 → is_ready == true                                                                                                            
//                                    placeOrder(...)
//                                    release(5) → is_ready = false
//                                    read_p++
//  claim()  → slot = 6
//  ...

int64_t getCurrentTime() {
    // Get the current time point from the system wall clock
    auto now = std::chrono::system_clock::now();    
    // Convert to time since epoch (1970-01-01)
    auto duration = now.time_since_epoch();   
    // Cast to nanoseconds (returns an integer)
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

Gateway::Gateway(RingBuffer_inbound* buffer, RingBuffer_outbound* buffer_outbound, int64_t id, GatewayMode m,
                 std::optional<bool> silent_override)
    : ring_buffer_internal(buffer), ring_buffer_outbound(buffer_outbound), gateway_id(id), mode(m) {
    // Default: REPLAY is silent (data feeds don't need acks), LIVE is noisy.
    // Caller can override either way.
    silent = silent_override.value_or(m == GatewayMode::REPLAY);
}

int64_t Gateway::place_order_to_ring_buffer(int64_t price, int64_t volume, bool side, std::string username,
                                            int64_t external_id, int64_t external_ts){
    int64_t order_id;
    int64_t arrival_ts;
    if (mode == GatewayMode::LIVE){
        // mint id + stamp wall-clock; ignore external args
        order_id = internal_id_counter.fetch_add(1, std::memory_order_relaxed);
        arrival_ts = getCurrentTime();
    } else { // REPLAY
        assert(external_id >= 0 && "REPLAY gateway requires a non-negative external_id");
        assert(external_ts >= 0 && "REPLAY gateway requires a non-negative external_ts");
        order_id = external_id;
        arrival_ts = external_ts;
    }

    int64_t write_pointer = ring_buffer_internal->claim();
    Orderevent_inbound* ord_event = ring_buffer_internal->get(write_pointer);
    ord_event->op = Op::NEW;
    ord_event->internal_order_id = order_id;
    ord_event->price = price;
    ord_event->volume = volume;
    ord_event->order_arrival_time = arrival_ts;
    ord_event->side = side;
    ord_event->gateway_id = gateway_id;
    ord_event->silent = silent;
    ring_buffer_internal->publish(write_pointer);
    return order_id; // caller can later cancel by this id
}

void Gateway::cancel_order_to_ring_buffer(int64_t order_id, int64_t external_ts){
    int64_t ts;
    if (mode == GatewayMode::LIVE){
        ts = getCurrentTime();
    } else { // REPLAY
        assert(external_ts >= 0 && "REPLAY gateway requires a non-negative external_ts");
        ts = external_ts;
    }
    int64_t write_pointer = ring_buffer_internal->claim();
    Orderevent_inbound* ord_event = ring_buffer_internal->get(write_pointer);
    ord_event->op = Op::CANCEL;
    ord_event->internal_order_id = order_id;
    ord_event->gateway_id = gateway_id;
    ord_event->order_arrival_time = ts;
    ord_event->silent = silent;
    // price/volume/side unused for CANCEL
    ring_buffer_internal->publish(write_pointer);
}

void Gateway::modify_order_to_ring_buffer(int64_t order_id, int64_t new_size, int64_t external_ts){
    int64_t ts;
    if (mode == GatewayMode::LIVE){
        ts = getCurrentTime();
    } else { // REPLAY
        assert(external_ts >= 0 && "REPLAY gateway requires a non-negative external_ts");
        ts = external_ts;
    }
    int64_t write_pointer = ring_buffer_internal->claim();
    Orderevent_inbound* ord_event = ring_buffer_internal->get(write_pointer);
    ord_event->op = Op::MODIFY;
    ord_event->internal_order_id = order_id;
    ord_event->volume = new_size;       // MODIFY uses volume to carry the NEW size
    ord_event->gateway_id = gateway_id;
    ord_event->order_arrival_time = ts;
    ord_event->silent = silent;
    // price/side unused for MODIFY
    ring_buffer_internal->publish(write_pointer);
}

bool Gateway::read_from_ring_buffer(Fill* fills, int64_t& fill_count, int64_t& order_id, bool& fulfilled, int64_t& remaining_qty,
                                    Op* out_op){
    Orderevent_outbound* order = ring_buffer_outbound->get(outbound_ringbuffer_read_p);
    if (order->is_ready.load(std::memory_order_acquire)){
        for (int64_t i = 0; i < order->fill_count; i++) {
            fills[i] = order->fills[i];
        }
        fill_count = order -> fill_count;
        order_id = order -> order_id;
        fulfilled = order -> fulfilled;
        remaining_qty = order -> remaining_qty;
        if (out_op) *out_op = order->op;
        ring_buffer_outbound -> release(outbound_ringbuffer_read_p);
        outbound_ringbuffer_read_p +=1;
        return true;
    }else{
        return false;
    }
}