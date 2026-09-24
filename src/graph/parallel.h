#pragma once

// A small fork/join helper for the graph's hot loops (many-body force, link rasterisation, label
// images): a persistent pool of std::threads that the calling thread joins while a job runs.
// Plain std::mutex/std::condition_variable synchronisation, so ThreadSanitizer sees every
// happens-before edge (QtConcurrent's waits are invisible to it with an uninstrumented Qt).

#include <functional>

namespace oc::graph {

// Calls fn(0) … fn(count − 1), distributed dynamically over the pool and the calling thread, and
// returns once every call has finished. fn must be thread-safe. One job runs at a time (concurrent
// callers wait); a call made from inside a job runs inline.
void parallelFor(int count, const std::function<void(int index)>& fn);

// Threads taking part in a parallelFor() job (pool threads + the caller).
int parallelWorkers();

} // namespace oc::graph
