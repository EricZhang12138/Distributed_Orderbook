#include "orderbook.hpp"
#include "gateway.hpp"

class matching_engine{
    Orderbook orderbook;
    RingBuffer_inbound* ring_buffer;
    int64_t read_p = 0;
    void run();
};