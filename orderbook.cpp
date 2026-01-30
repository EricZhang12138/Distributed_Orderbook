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
        while(bids.rbegin() -> first >= price && volume > 0){
            Limit* highest_limit = bids.rbegin() -> second; // this is the Limit we will process for this step 
            while(highest_limit -> totalVolume > 0 && volume > 0){
                Order& current_order = *(highest_limit -> head);
                if (current_order.volume > volume){
                    current_order.volume -= volume; // decrement the volume of the order
                    current_order.parentlimit -> totalVolume -= volume;
                }else{ // if(current_order.volume <= volume)

                } 
                


            }
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