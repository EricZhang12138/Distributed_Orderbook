#include "matching_engine.hpp"

void MatchingEngine::run(){
    while(true){
        Orderevent_inbound* event = ring_buffer_inbound->get(read_p);
        // if the order is ready
        if (event->is_ready.load(std::memory_order_acquire)){
            auto res  = orderbook.placeOrder(event->price, event->volume, event->side, event -> order_arrival_time, event->internal_order_id, event->gateway_id);
            ring_buffer_inbound->release(read_p);
            read_p++;

            //update the outbound ring buffer
            int64_t gid = res.second;
            RingBuffer_outbound* outbound = ring_buffer_outbound[gid];
            Orderevent_outbound* event_out = outbound->get(write_p_outbound[gid]);
            event_out->order_id = res.first.orderID;
            event_out->gateway_id = gid;
            event_out->fulfilled = res.first.fulfilled;
            event_out->remaining_qty = res.first.remainingQty;
            // Because Orderevent_outbound only has Fill fills[16] — a fixed array of 16. If res.first.trades has more than 16 fills, copying all of them would write past the end of the array and corrupt memory. So we cap 
            // it at 16 to stay within bounds.   
            event_out->fill_count = std::min((int64_t)res.first.trades.size(), (int64_t)16);
            for (int64_t i = 0; i < event_out->fill_count; i++){
                event_out->fills[i] = res.first.trades[i];
            }
            outbound->publish(write_p_outbound[gid]);
            write_p_outbound[gid]++;
        }
    }
}

MatchingEngine::MatchingEngine(RingBuffer_inbound* buffer_inbound, std::vector<RingBuffer_outbound*> buffer_outbound){
    ring_buffer_inbound = buffer_inbound;
    ring_buffer_outbound = buffer_outbound;
    write_p_outbound.resize(buffer_outbound.size(), 0);
}

