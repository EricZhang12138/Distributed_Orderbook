#include "lobster_replay_adapter.hpp"
#include <cstdlib>
#include <istream>

LobsterReplayAdapter::LobsterReplayAdapter(Gateway* gw) : gw_(gw) {}

bool LobsterReplayAdapter::process_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return false;
    process_stream(file);
    return true;
}

void LobsterReplayAdapter::process_stream(std::istream& in) {
    std::string line;   // allocated once, getline reuses the buffer
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        Row row;
        if (!parse_row(line, row)) { rows_failed_++; continue; }
        dispatch(row);
    }
}

// Zero-allocation parser: strto* on the c_str with pointer arithmetic. No
// substrings, no stringstream. Returns false if any field is malformed.
bool LobsterReplayAdapter::parse_row(const std::string& line, Row& out) {
    const char* p = line.c_str();
    char* end = nullptr;

    out.time_seconds = std::strtod(p, &end);
    if (end == p || *end != ',') return false;
    p = end + 1;

    out.type = static_cast<int>(std::strtol(p, &end, 10));
    if (end == p || *end != ',') return false;
    p = end + 1;

    out.order_id = std::strtoll(p, &end, 10);
    if (end == p || *end != ',') return false;
    p = end + 1;

    out.size = std::strtoll(p, &end, 10);
    if (end == p || *end != ',') return false;
    p = end + 1;

    out.price = std::strtoll(p, &end, 10);
    if (end == p || *end != ',') return false;
    p = end + 1;

    out.direction = static_cast<int>(std::strtol(p, &end, 10));
    if (end == p) return false;
    // Trailing characters after direction are ok (Windows line endings, etc.) —
    // we don't validate them.
    return true;
}

void LobsterReplayAdapter::dispatch(const Row& row) {
    const int64_t ts_ns = static_cast<int64_t>(row.time_seconds * 1e9);
    // LOBSTER direction:  1 = buy  →  our convention side = false (rests on bid tree)
    //                    -1 = sell →  our convention side = true  (rests on ask tree)
    const bool side = (row.direction == -1);

    switch (row.type) {
        case 1: {  // Submission of a new limit order
            gw_->place_order_to_ring_buffer(row.price, row.size, side, "",
                                            row.order_id, ts_ns);
            live_sizes_[row.order_id] = row.size;
            rows_processed_++;
            break;
        }
        case 2: {  // Partial cancellation — size column is the *delta* removed
            auto it = live_sizes_.find(row.order_id);
            if (it == live_sizes_.end()) { rows_failed_++; return; }
            const int64_t new_size = it->second - row.size;
            if (new_size <= 0) { rows_failed_++; return; }  // type 3 should have been used
            gw_->modify_order_to_ring_buffer(row.order_id, new_size, ts_ns);
            it->second = new_size;
            rows_processed_++;
            break;
        }
        case 3: {  // Full deletion
            gw_->cancel_order_to_ring_buffer(row.order_id, ts_ns);
            live_sizes_.erase(row.order_id);
            rows_processed_++;
            break;
        }
        case 4: case 5: case 6: case 7:
            // 4 = visible execution: skipped because our matcher generates the fill
            //     itself when it processes the matching incoming type-1 order.
            // 5 = hidden execution: not in our book.
            // 6 = cross trade: not modeled.
            // 7 = trading halt: not modeled.
            rows_skipped_++;
            break;
        default:
            rows_failed_++;
            break;
    }
}
