#pragma once
#include "gateway.hpp"
#include <fstream>
#include <iosfwd>
#include <string>
#include <unordered_map>

// Streams a LOBSTER message CSV into a REPLAY gateway, one row at a time.
//
// Each input row maps to exactly one of place/cancel/modify on the gateway
// (or is skipped for unsupported types — execution rows are skipped because
// the matcher generates fills itself when processing the matching incoming
// type-1 order).
//
// Zero intermediate buffering: the adapter blocks inside gateway.claim() if
// the engine can't keep up. That's the desired backpressure shape — the
// inbound ring buffer is the only queue.
//
// Expected to be paired with a Gateway constructed in REPLAY mode, which
// defaults to silent=true so the engine doesn't waste cycles writing acks
// nobody reads.
class LobsterReplayAdapter {
public:
    explicit LobsterReplayAdapter(Gateway* gw);

    // Opens the file at `path` and streams it to completion. Returns false if
    // the file could not be opened; true after successful streaming.
    bool process_file(const std::string& path);

    // Streams any istream to completion. Useful for in-memory tests.
    void process_stream(std::istream& in);

    int64_t rows_processed() const { return rows_processed_; }
    int64_t rows_skipped()   const { return rows_skipped_; }
    int64_t rows_failed()    const { return rows_failed_; }

private:
    struct Row {
        double  time_seconds;
        int     type;
        int64_t order_id;
        int64_t size;
        int64_t price;
        int     direction;   // +1 = buy, -1 = sell (LOBSTER convention)
    };

    // Returns false if the row is malformed.
    bool parse_row(const std::string& line, Row& out);
    void dispatch(const Row& row);

    Gateway* gw_;
    // For type 2 (partial cancel): LOBSTER reports the delta cancelled, our
    // gateway's modify takes the new total size — so we track current sizes.
    std::unordered_map<int64_t, int64_t> live_sizes_;
    int64_t rows_processed_ = 0;
    int64_t rows_skipped_   = 0;
    int64_t rows_failed_    = 0;
};
