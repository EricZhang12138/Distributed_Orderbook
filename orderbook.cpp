#include "orderbook.hpp"

// Returns current Wall Clock time in Nanoseconds
int64_t getCurrentTime() {
    // Get the current time point from the system wall clock
    auto now = std::chrono::system_clock::now();    
    // Convert to time since epoch (1970-01-01)
    auto duration = now.time_since_epoch();   
    // Cast to nanoseconds (returns an integer)
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}




orderResult Orderbook::match(int64_t price, int64_t volume, bool side, int64_t time, int64_t orderID){ // it also takes in the time when the order is placed. We don't want to calculate the time multiple times for different Fills in the same ask/bid because it slows down the system
    if (side == true){ // we add to asks map, meaning we want to sell
        // we first check whether there is matching price in the bids map
        orderResult res = {
            .orderID = orderID,
            .fulfilled = false,
            .remainingQty = volume
        };
        while(!bids.empty() && bids.rbegin() -> first >= price && volume > 0){
            std::unique_ptr<Limit>& highest_limit = bids.rbegin() -> second; // Look at the pointer in the map but don't copy it
            while(highest_limit -> count > 0 && volume > 0){
                Order& current_order = *(highest_limit -> head);
                if (current_order.volume > volume){
                    current_order.volume -= volume; // decrement the volume of the order
                    highest_limit -> totalVolume -= volume;
                    Fill f_1 = {
                        .price = current_order.price,
                        .volume = volume,
                        .timestamp = time
                    };
                    res.fulfilled = true;
                    res.remainingQty = 0;
                    res.trades.push_back(f_1);

                    return res; // this is when we return the result 

                }else{ // if(current_order.volume <= volume)
                    // update Limit
                    highest_limit -> count -= 1; // number of order decrements by 1
                    highest_limit -> totalVolume -= current_order.volume;
                    highest_limit -> head = highest_limit -> head -> next; // we move the pointer to the next Order
                    volume -= current_order.volume; // update the current volume count
                    res.remainingQty -= current_order.volume;
                    Fill f_1 = {
                        .price = current_order.price,
                        .volume = current_order.volume,
                        .timestamp = time
                    };
                    if (highest_limit -> head != nullptr){
                        highest_limit -> head -> prev = nullptr; 
                    }
                    global_map.erase(current_order.orderID);
                    delete &current_order;
                    res.trades.push_back(f_1);
                    //after all the processing if count == 0, then we need to erase this price level in the map
                    if (highest_limit -> count == 0){
                        // Method 2: Convert reverse iterator
                        auto it = std::next(bids.rbegin()).base();
                        bids.erase(it); // erase the entire entry 
                        break; // leave the while loop, and we don't want to check highest_limit -> count > 0 ever again bc it doesn't exist
                    }
                }
            }
        }
        if (volume > 0){ // if the volume is 0, we don't want to create a zombie order
            // now we have gone through all the Limit in the bids that satisfy the condition
            // we need to insert the remaining asks into the tree
            Order* new_order = new Order{price,volume,orderID, side}; 
            // using new Order() will require you to have your own constructor
            // but using new Order{} allows you to directly initialise those variables
            auto it = asks.find(price);
            if (it != asks.end()){ // price is in the asks map
                std::unique_ptr<Limit>& insert_limit = it -> second;
                insert_limit -> count ++;
                insert_limit -> tail -> next = new_order;
                new_order -> prev = insert_limit -> tail;
                insert_limit -> tail = new_order;
                insert_limit -> totalVolume += volume;
                new_order -> parentlimit = (it -> second).get();
            }else{// price is not in the asks map
                asks[price] = std::make_unique<Limit>();
                asks[price] -> count = 1;
                asks[price] -> head = new_order;
                asks[price] -> tail = new_order;
                asks[price] -> totalVolume = volume;
                new_order -> parentlimit = asks[price].get();
            }
            global_map[orderID] = new_order;
            return res;
        }
    }
}




int64_t Orderbook::placeOrder(int64_t price, int64_t volume, bool side){
    // generate timestamp 
    int64_t c_time = getCurrentTime();
    // increment the id counter
    id_counter ++;
    int64_t orderID = id_counter;

}