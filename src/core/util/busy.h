#pragma once

#include <atomic>

namespace oc {

// Process-wide "work in flight" counter used by headless runs (--screenshot/--script) to wait until
// background indexing, image decoding, layout measurement etc. have settled.
class Busy {
public:
    static void begin() { s_count.fetch_add(1, std::memory_order_relaxed); }
    static void end() { s_count.fetch_sub(1, std::memory_order_relaxed); }
    static bool idle() { return s_count.load(std::memory_order_relaxed) <= 0; }

    // RAII guard
    struct Scope {
        Scope() { begin(); }
        ~Scope() { end(); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };

private:
    static inline std::atomic<int> s_count{0};
};

} // namespace oc
