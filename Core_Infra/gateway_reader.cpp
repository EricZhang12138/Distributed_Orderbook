#include "gateway_reader.hpp"

GatewayReader::GatewayReader(Gateway* gw, SpscQueue<Ack>* queue)
    : gw_(gw), queue_(queue), thread_([this]{ run(); }) {}

GatewayReader::~GatewayReader() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

// Tight loop: poll the gateway for an ack, if one arrives, push it into the
// queue. If the queue is full, drop the ack and count it.
//
// This thread should be the *only* reader of the gateway's outbound ring buffer.
// If the consumer also calls gw_->read_from_ring_buffer, the two will race on
// the outbound read pointer.
void GatewayReader::run() {
    Ack ack;
    Op op = Op::NEW;
    int64_t order_id = 0;
    bool fulfilled = false;
    int64_t remaining_qty = 0;
    int64_t fill_count = 0;

    while (!stop_.load(std::memory_order_acquire)) {
        if (gw_->read_from_ring_buffer(ack.fills, fill_count, order_id,
                                       fulfilled, remaining_qty, &op)) {
            ack.op = op;
            ack.order_id = order_id;
            ack.fulfilled = fulfilled;
            ack.remaining_qty = remaining_qty;
            ack.fill_count = fill_count;
            if (!queue_->try_push(ack)) {
                dropped_count_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // No yield/sleep here — pinned-core deployments want max responsiveness.
        // Add std::this_thread::yield() if running on a shared core.
    }
}
