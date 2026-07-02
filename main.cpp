#include "gateway.hpp"
#include "matching_engine.hpp"
#include "orderbook.hpp"
#include "ring_buffer.hpp"
#include <iostream>
#include <memory>

int main() {
    int num_gateways = 4; // 0..2 LIVE, 3 REPLAY

    RingBuffer_inbound inbound(1024);

    // create outbounds vector
    std::vector<std::unique_ptr<RingBuffer_outbound>> outbounds;
    for (int i = 0; i < num_gateways; i++)
        outbounds.push_back(std::make_unique<RingBuffer_outbound>(1024));

    std::vector<RingBuffer_outbound*> outbound_ptrs;
    for (auto& ob : outbounds) outbound_ptrs.push_back(ob.get());

    MatchingEngine engine(&inbound, outbound_ptrs);

    // create gateways: 0..2 LIVE, 3 REPLAY
    std::vector<std::unique_ptr<Gateway>> gateways;
    for (int i = 0; i < num_gateways - 1; i++)
        gateways.push_back(std::make_unique<Gateway>(&inbound, outbounds[i].get(), i, GatewayMode::LIVE));
    // REPLAY gateway with silent=false: the demo wants to observe acks (overrides the new REPLAY→silent default).
    gateways.push_back(std::make_unique<Gateway>(&inbound, outbounds[num_gateways - 1].get(), num_gateways - 1, GatewayMode::REPLAY, /*silent_override=*/false));

    std::thread engine_thread([&]{ engine.run(); });
    engine_thread.detach();

    std::vector<std::thread> gateway_threads;



    /*
    bool read_from_ring_buffer(Fill* fills, int64_t& fill_count, int64_t& order_id, bool& fulfilled, int64_t& remaining_qty);
    void place_order_to_ring_buffer(int64_t price,int64_t volume, bool side, std::string username);
    */
    gateway_threads.emplace_back([&]{
        gateways[0]->place_order_to_ring_buffer(100, 10, true, "alice");
        gateways[1]->place_order_to_ring_buffer(110,5,false,"Eric");

        Fill fills[16];
        int64_t fill_count, order_id, remaining_qty;
        bool fulfilled;
        //while (!gateways[0]->read_from_ring_buffer(fills, fill_count, order_id, fulfilled, remaining_qty)) {}
        while (!gateways[1]->read_from_ring_buffer(fills, fill_count, order_id, fulfilled, remaining_qty)) {}

        std::cout << "Order " << order_id
            << " | fulfilled: " << fulfilled
            << " | remaining: " << remaining_qty
            << " | fills: " << fill_count << "\n";
        for (int64_t i = 0; i < fill_count; i++) {
            std::cout << "  fill " << i << ": price=" << fills[i].price
                << " vol=" << fills[i].volume << "\n";
        }

        // --- Cancel round-trip test ---
        // Place a resting order that won't match (bid at 90, no asks <= 90), then cancel it.
        int64_t resting_id = gateways[2]->place_order_to_ring_buffer(90, 7, false, "carol");
        while (!gateways[2]->read_from_ring_buffer(fills, fill_count, order_id, fulfilled, remaining_qty)) {}
        std::cout << "Placed resting order id=" << resting_id
                  << " ack: fulfilled=" << fulfilled << " remaining=" << remaining_qty << "\n";

        gateways[2]->cancel_order_to_ring_buffer(resting_id);
        while (!gateways[2]->read_from_ring_buffer(fills, fill_count, order_id, fulfilled, remaining_qty)) {}
        std::cout << "Cancel ack for id=" << order_id
                  << " | success=" << fulfilled
                  << " | fill_count=" << fill_count << "\n";

        // Second cancel of the same id should fail (success=0).
        gateways[2]->cancel_order_to_ring_buffer(resting_id);
        while (!gateways[2]->read_from_ring_buffer(fills, fill_count, order_id, fulfilled, remaining_qty)) {}
        std::cout << "Re-cancel ack for id=" << order_id
                  << " | success=" << fulfilled << " (expected 0)\n";
    });

    // --- REPLAY-mode demo on gateway 3: place, modify, cancel ---
    gateway_threads.emplace_back([&]{
        // External id 88421, external timestamp 1700000000000000000 (arbitrary ns).
        int64_t returned = gateways[3]->place_order_to_ring_buffer(95, 10, false, "feed", 88421, 1700000000000000000LL);
        std::cout << "REPLAY place: returned id=" << returned << " (expected 88421)\n";

        Fill rfills[16];
        int64_t rfc, roid, rrq;
        bool rok;
        while (!gateways[3]->read_from_ring_buffer(rfills, rfc, roid, rok, rrq)) {}
        std::cout << "REPLAY place ack: id=" << roid << " fulfilled=" << rok << " remaining=" << rrq << "\n";

        // Modify the resting order from 10 down to 6.
        gateways[3]->modify_order_to_ring_buffer(88421, 6, 1700000000000500000LL);
        while (!gateways[3]->read_from_ring_buffer(rfills, rfc, roid, rok, rrq)) {}
        std::cout << "REPLAY modify ack: id=" << roid << " success=" << rok << " (expected 1)\n";

        // Try an invalid modify: size-up should fail.
        gateways[3]->modify_order_to_ring_buffer(88421, 100, 1700000000000600000LL);
        while (!gateways[3]->read_from_ring_buffer(rfills, rfc, roid, rok, rrq)) {}
        std::cout << "REPLAY modify-up ack: id=" << roid << " success=" << rok << " (expected 0)\n";

        gateways[3]->cancel_order_to_ring_buffer(88421, 1700000000001000000LL);
        while (!gateways[3]->read_from_ring_buffer(rfills, rfc, roid, rok, rrq)) {}
        std::cout << "REPLAY cancel ack: id=" << roid << " success=" << rok << "\n";
    });
    for (auto& t : gateway_threads) t.join();
}


/*
Why we use unique pointers for the gateway vector and the outbound vector

  The problem:
  When a std::vector grows (e.g. you emplace_back and it runs out of capacity), it allocates a bigger array and moves every existing element to the new array. But
  std::atomic and std::mutex have their move/copy constructors deleted — they literally cannot be moved. So the compiler rejects it.

  Why unique_ptr fixes it:
  With std::vector<std::unique_ptr<Gateway>>, the vector contains pointers, not Gateway objects. When the vector resizes, it moves the pointers around — which is just
  copying an address (a number). The actual Gateway object sits on the heap and never moves at all.
*/


/*
About threads 

❯ when we talk about the scope of the thread itself, what do we actually refer to? So without [&], the thread can only access its local variables (variables within       
  the stack of the thread) if we have [&] then we get to access variables outside ?                                                                                      
                                                                                                                                                                          
⏺ Yes, exactly. The [&] is about the lambda (the function you pass to the thread), not the thread itself.                                                                 
                                                                                                                                                                          
  - []{ ... } — the lambda can only see what's declared inside its own { } body                                                                                           
  - [&]{ ... } — the lambda can also see variables from the surrounding scope (by reference)
                                                                                                                                                                          
  int x = 5;                                                                                                                                                            

  auto f1 = []{ std::cout << x; };   // ❌ compile error: x not captured
  auto f2 = [&]{ std::cout << x; };  // ✅ works: x captured by reference

  So when you write [&]{ gateways[0]->place_order_to_ring_buffer(...); }, the lambda can see gateways because [&] captures it from main()'s scope. Without [&], it
  wouldn't know gateways exists.
*/