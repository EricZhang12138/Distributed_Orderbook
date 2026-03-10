#include "gateway.hpp"
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

Gateway::Gateway(RingBuffer_inbound* buffer, int64_t id): ring_buffer_internal(buffer), gateway_id(id){};

void Gateway::place_order_to_ring_buffer(int64_t price, int64_t volume, bool side, std::string username){
    // increment the count for internal_id_counter
    int64_t my_id = internal_id_counter.fetch_add(1, std::memory_order_relaxed); 
    int64_t write_pointer = ring_buffer_internal ->claim();
    Orderevent_inbound* ord_event = ring_buffer_internal -> get(write_pointer);
    //update the orderevent with the current order
    ord_event -> internal_order_id = my_id;
    ord_event -> price = price;
    ord_event -> volume = volume;
    ord_event -> order_arrival_time = getCurrentTime();
    ord_event ->side = side;
    ord_event ->gateway_id = gateway_id;
    // publish to notify that we are ready 
    ring_buffer_internal -> publish(write_pointer);
}
