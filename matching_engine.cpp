#include "matching_engine.hpp"

void matching_engine::run(){
    while(true){
        Orderevent_inbound* event = ring_buffer->get(read_p);
        // if the order is ready
        if (event->is_ready.load(std::memory_order_acquire)){
            auto res  = orderbook.placeOrder(event->price, event->volume, event->side, event -> order_arrival_time, event->internal_order_id, event->gateway_id);
            ring_buffer->release(read_p);
            read_p++;
        }
    }
}

