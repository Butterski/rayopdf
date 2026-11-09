// SPDX-License-Identifier: Apache-2.0
// The threads that read the pages of one document at once (roadmap D4: pages in parallel, the
// output independent of the thread count). One pool serves every page-local phase of a document
// (layout/convert.cpp, layout/pipeline.cpp): its threads start once and wait between phases.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace RayoPdf::Layout {

class PagePool {
public:
    // `workers` (2 or more) threads: the calling thread of Run and workers - 1 of the pool's own,
    // started here.
    explicit PagePool(unsigned workers);
    ~PagePool();
    PagePool(const PagePool&) = delete;
    PagePool& operator=(const PagePool&) = delete;

    unsigned Workers() const { return workers_; }

    // `run(context, worker, index)` for every index below `count`, each once, taken in increasing
    // order by whichever thread is free; `worker` (below Workers(); 0 is the calling thread)
    // names the thread, for buffers of its own. Returns when every index is done.
    void Run(size_t count, void (*run)(void*, unsigned, size_t), void* context);

    template <class Job>
    void Run(size_t count, Job&& job) {
        using Function = std::remove_reference_t<Job>;
        Run(count, [](void* self, unsigned worker, size_t index) { (*static_cast<Function*>(self))(worker, index); },
            const_cast<void*>(static_cast<const void*>(&job)));
    }

private:
    // The pool's thread `worker` (1 on): each round's jobs, until the pool goes.
    void Serve(unsigned worker);
    // The round's indices left, taken one at a time.
    void Take(unsigned worker);

    const unsigned workers_;
    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable start_;  // a round began, or the pool goes
    std::condition_variable done_;   // the last thread of a round finished
    uint64_t round_ = 0;             // rounds begun (under mutex_)
    unsigned busy_ = 0;              // the pool's threads still in the round (under mutex_)
    bool closing_ = false;           // under mutex_
    // The round: set under mutex_ before round_ moves, read by the threads after they see it.
    void (*run_)(void*, unsigned, size_t) = nullptr;
    void* context_ = nullptr;
    size_t count_ = 0;
    std::atomic<size_t> next_{0};
};

} // namespace RayoPdf::Layout
