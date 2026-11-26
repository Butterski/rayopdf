// SPDX-License-Identifier: Apache-2.0
// The threads that read the pages of one document at once (roadmap D4: pages in parallel, the
// output independent of the thread count). One pool serves every page-local phase of a document
// (layout/convert.cpp, layout/pipeline.cpp) and its images (layout/images.cpp, and through
// common/threads.h the image module): its threads start once and wait between rounds.
#pragma once

#include "common/thread.h"
#include "common/threads.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>
#include <vector>

namespace RayoPdf::Layout {

class PagePool final : public Threads {
public:
    // Up to `workers` (2 or more) threads: the thread that makes the pool and workers - 1 of the
    // pool's own, which Start starts: a thread costs more than a small page does (its start, and
    // the first touch of its buffers), so the caller starts them when it has seen the work.
    explicit PagePool(unsigned workers);
    ~PagePool();
    PagePool(const PagePool&) = delete;
    PagePool& operator=(const PagePool&) = delete;

    unsigned Workers() const override { return workers_; }
    unsigned Started() const override { return started_.load(std::memory_order_relaxed); }
    void Start(unsigned threads) override;
    void Run(unsigned caller, size_t count, void (*run)(void*, unsigned, size_t), void* context) override;
    using Threads::Run;

    // A round of the thread that made the pool (worker 0).
    void Run(size_t count, void (*run)(void*, unsigned, size_t), void* context) { Run(0u, count, run, context); }

    template <class Job>
    void Run(size_t count, Job&& job) {
        using Function = std::remove_reference_t<Job>;
        Run(0u, count,
            [](void* self, unsigned worker, size_t index) { (*static_cast<Function*>(self))(worker, index); },
            const_cast<void*>(static_cast<const void*>(&job)));
    }

private:
    // One Run: its indices, taken one at a time by its caller and by the pool's threads.
    struct Round {
        void (*run)(void*, unsigned, size_t);
        void* context;
        size_t count;
        std::atomic<size_t> next{0};
        unsigned helpers = 0;  // the pool's threads inside it (under mutex_)
    };

    // The pool's thread `worker` (1 on): the indices of the open rounds, until the pool goes.
    void Serve(unsigned worker);
    // The outermost open round with indices left; null when there is none (under mutex_).
    Round* Open() const;
    static void Take(Round& round, unsigned worker);

    const unsigned workers_;
    std::mutex mutex_;
    std::condition_variable start_;  // a round opened, or the pool goes
    std::condition_variable done_;   // a round's last helper left it
    std::vector<Thread> threads_;  // under mutex_
    std::atomic<unsigned> started_{0};  // threads_.size()
    std::vector<Round*> rounds_;        // the open rounds, outermost first (under mutex_)
    bool closing_ = false;              // under mutex_
};

} // namespace RayoPdf::Layout
