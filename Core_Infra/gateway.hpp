#pragma once
#include <optional>
#include "ring_buffer.hpp"
#include "orderbook.hpp"

// claim() a slot → get() the pointer to that slot → fill in the order data → publish() to signal it's ready. 
// The matching engine on the other side polls the buffer checking is_ready and calls release() when done.

enum class GatewayMode { LIVE, REPLAY };

class Gateway{ // a wrapper around the Orderbook object. It converts user facing ID (username + id) to internal orderbook id
private:
    int64_t gateway_id = 0;
    int64_t outbound_ringbuffer_read_p = 0; // used along with read_from_ring_buffer
    RingBuffer_inbound* ring_buffer_internal; // we use a raw pointer instead of unique pointer because the gateway shouldn't own the ringbuffer, it is a shared resource
    RingBuffer_outbound* ring_buffer_outbound;
    std::atomic<int64_t> internal_id_counter = 0;
    GatewayMode mode = GatewayMode::LIVE; // LIVE mints ids + uses wall-clock; REPLAY uses caller-supplied id + timestamp
    // Orthogonal to mode. If true, the engine skips writing outbound acks for events stamped by this gateway.
    // Default: LIVE→false (acks emitted), REPLAY→true (no acks; data feed doesn't need them and would overflow the outbound buffer).
    bool silent = false;
public:
    // `silent_override`: pass true/false to override the mode-based default for the silent flag.
    // Pass std::nullopt (the default) to use the mode-based default.
    Gateway(RingBuffer_inbound* buffer_inbound, RingBuffer_outbound* ring_buffer_outbound, int64_t id,
            GatewayMode mode = GatewayMode::LIVE,
            std::optional<bool> silent_override = std::nullopt);
    bool is_silent() const { return silent; }
    // out_op is optional — pass a non-null pointer to also receive which op (NEW/CANCEL/MODIFY) produced this ack.
    bool read_from_ring_buffer(Fill* fills, int64_t& fill_count, int64_t& order_id, bool& fulfilled, int64_t& remaining_qty,
                               Op* out_op = nullptr);
    // In LIVE mode external_id and external_ts are ignored. In REPLAY mode both must be >= 0.
    int64_t place_order_to_ring_buffer(int64_t price, int64_t volume, bool side, std::string username,
                                       int64_t external_id = -1, int64_t external_ts = -1);
    // In LIVE mode external_ts is ignored. In REPLAY mode it must be >= 0.
    void cancel_order_to_ring_buffer(int64_t order_id, int64_t external_ts = -1);
    // In-place size reduction. new_size must be a strictly smaller positive value than the order's current volume.
    // In LIVE mode external_ts is ignored. In REPLAY mode it must be >= 0.
    void modify_order_to_ring_buffer(int64_t order_id, int64_t new_size, int64_t external_ts = -1);
};

int64_t getCurrentTime();