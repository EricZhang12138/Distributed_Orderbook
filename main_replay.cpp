#include "gateway.hpp"
#include "lobster_replay_adapter.hpp"
#include "matching_engine.hpp"
#include "orderbook.hpp"
#include "ring_buffer.hpp"
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <lobster_message_csv_path>\n";
        return 1;
    }
    const std::string path = argv[1];

    // Heap-allocate and intentionally leak: MatchingEngine::run() has no stop
    // flag, so we can't join the engine thread. Detaching and then destroying
    // the stack-allocated buffers as main() returns is a use-after-free. The
    // process is exiting anyway, so leaking is the right move.
    auto* inbound  = new RingBuffer_inbound(1024);
    auto* outbound = new RingBuffer_outbound(64);   // silent=true → unused
    std::vector<RingBuffer_outbound*> outbound_ptrs{ outbound };

    auto* engine = new MatchingEngine(inbound, outbound_ptrs);
    auto* gw     = new Gateway(inbound, outbound, 0, GatewayMode::REPLAY);  // silent=true by default

    std::thread engine_thread([engine]{ engine->run(); });
    engine_thread.detach();

    LobsterReplayAdapter adapter(gw);

    const auto t0 = std::chrono::steady_clock::now();
    if (!adapter.process_file(path)) {
        std::cerr << "failed to open " << path << "\n";
        return 2;
    }
    // After process_file returns, every row has been written to the inbound
    // buffer (the adapter blocks inside claim() if the engine can't keep up,
    // so completion implies all rows are queued). The engine still needs a
    // brief moment to drain the last <= buffer_size events. Wait until every
    // inbound slot's is_ready flag is false — that's the consumer-released
    // state.
    auto all_released = [&]{
        for (int64_t i = 0; i < inbound->size; i++) {
            if (inbound->get(i)->is_ready.load(std::memory_order_acquire)) return false;
        }
        return true;
    };
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!all_released() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    std::cout << "rows_processed: " << adapter.rows_processed() << "\n"
              << "rows_skipped:   " << adapter.rows_skipped()   << "\n"
              << "rows_failed:    " << adapter.rows_failed()    << "\n"
              << "elapsed:        " << secs << " s\n";
    if (secs > 0) {
        std::cout << "throughput:     "
                  << static_cast<int64_t>((adapter.rows_processed() + adapter.rows_skipped()) / secs)
                  << " rows/s\n";
    }
    // Deliberately leaking inbound/outbound/engine/gw — see comment above.
    return 0;
}
