#include <map>
#include <cstdint>
#include <chrono>



// forward declaration, Order needs Limit while Limit needs Order
struct Order; 
struct Limit;

struct Fill{
    int64_t price = 0;
    int64_t volume = 0;
    int64_t timestamp = 0;
};

struct orderResult{
    int64_t orderID = 0;
    bool fulfilled = true;
    int64_t remainingQty = 0;
    std::vector<Fill> trades;
};


// So normally if we use std::list<Order> what is happening is for every Order object it is wrapped by 
// a node object but then if I implement my own management class Limit then I can have direct access
// this is called intrusive linked list (using std::list is non intrusive linked list)

// a doubly linked list
struct Limit{
    Order* head = nullptr;
    Order* tail = nullptr;
    int64_t count = 0;   // number of orders
    int64_t totalVolume = 0; // total number of shares 
};

struct Order{
    int64_t price = 0;
    int64_t volume = 0;
    int64_t orderID = 0;
    bool side = true; // indicates the direction of the trade: true -> sell    false -> buy
    
    // doubly linked list
    Order* next = nullptr;
    Order* prev = nullptr; 
    Limit* parentlimit = nullptr; // this is to efficiently trace back to the limit. For example, after deletion, parentlimit.count - 1
};


class Orderbook{
private:
    int id_counter = 0; // this is the counter for the id of each order. It increments when 
    
    std::unordered_map<int64_t, Order*> global_map; //orderID -> Order* for efficient order cancelling, otherwise knowing orderID won't help you delete it
    std::map<int64_t, Limit*> bids;  // price -> list of orders
    std::map<int64_t, Limit*> asks;

    orderResult match(int64_t price, int64_t volume, bool side, int64_t time, int64_t orderID); // for the asks or bids on the Limit queues directly
    //void pop(int64_t price, int64_t volumne, bool side); 

public:
    int64_t placeOrder(int64_t price, int64_t volume, bool side); // returns the id of the order
    void cancel(int64_t order_id);

};


class gateway{ // a wrapper around the Orderbook object. It converts user facing ID (username + id) to internal orderbook id 

};