#include "orderbook.hpp"
#include "gateway.hpp"

class MatchingEngine{
    MatchingEngine(RingBuffer_inbound* buffer_inbound, std::vector<RingBuffer_outbound*> buffer_outbound);
    Orderbook orderbook;
    RingBuffer_inbound* ring_buffer_inbound;
    int64_t read_p = 0;
    std::vector<int64_t> write_p_outbound; // one write pointer per gateway
    // we need a "map" of gateway_id to outbound ring buffers
    // in this case, given the gateway_id increments from 0 onwards, we use a vector to represent that
    std::vector<RingBuffer_outbound*> ring_buffer_outbound;
    void run();
};