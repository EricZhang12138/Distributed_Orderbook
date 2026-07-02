#include "matching_engine.hpp"

void MatchingEngine::run(){
    while(true){
        Orderevent_inbound* event = ring_buffer_inbound->get(read_p);
        if (!event->is_ready.load(std::memory_order_acquire)) continue;

        int64_t gid = event->gateway_id;
        const bool silent = event->silent;   // copy out before we release the inbound slot

        if (silent) {
            // Run the orderbook side-effect, skip the outbound write entirely.
            // The orderbook still gets updated; no ack lands on the gateway's outbound buffer.
            if (event->op == Op::NEW){
                orderbook.placeOrder(event->price, event->volume, event->side, event->order_arrival_time, event->internal_order_id, gid);
            } else if (event->op == Op::CANCEL){
                orderbook.cancel(event->internal_order_id);
            } else { // Op::MODIFY
                orderbook.modify(event->internal_order_id, event->volume);
            }
            ring_buffer_inbound->release(read_p);
            read_p++;
            continue;   // do NOT touch write_p_outbound[gid] — silent gateways never advance it
        }

        RingBuffer_outbound* outbound = ring_buffer_outbound[gid];
        Orderevent_outbound* event_out = outbound->get(write_p_outbound[gid]);

        event_out->op = event->op;   // stamp every ack with the op that produced it
        if (event->op == Op::NEW){
            auto res = orderbook.placeOrder(event->price, event->volume, event->side, event->order_arrival_time, event->internal_order_id, gid);
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
        } else if (event->op == Op::CANCEL){
            bool ok = orderbook.cancel(event->internal_order_id);
            event_out->order_id = event->internal_order_id;
            event_out->gateway_id = gid;
            event_out->fulfilled = ok;   // reused: "did the cancel succeed?"
            event_out->remaining_qty = 0;
            event_out->fill_count = 0;
        } else { // Op::MODIFY — volume field carries the new size
            bool ok = orderbook.modify(event->internal_order_id, event->volume);
            event_out->order_id = event->internal_order_id;
            event_out->gateway_id = gid;
            event_out->fulfilled = ok;   // reused: "did the modify succeed?"
            event_out->remaining_qty = 0;
            event_out->fill_count = 0;
        }

        ring_buffer_inbound->release(read_p);
        read_p++;
        outbound->publish(write_p_outbound[gid]);
        write_p_outbound[gid]++;
    }
}

MatchingEngine::MatchingEngine(RingBuffer_inbound* buffer_inbound, std::vector<RingBuffer_outbound*> buffer_outbound){
    ring_buffer_inbound = buffer_inbound;
    ring_buffer_outbound = buffer_outbound;
    write_p_outbound.resize(buffer_outbound.size(), 0);
}

