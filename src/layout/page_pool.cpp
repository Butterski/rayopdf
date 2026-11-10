// SPDX-License-Identifier: Apache-2.0
#include "layout/page_pool.h"

namespace RayoPdf::Layout {

PagePool::PagePool(unsigned workers) : workers_(workers < 2 ? 2 : workers) {
    threads_.reserve(workers_ - 1);
}

PagePool::~PagePool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
    }
    start_.notify_all();
    for (std::thread& thread : threads_) thread.join();
}

void PagePool::Start(unsigned threads) {
    if (threads > workers_ - 1) threads = workers_ - 1;
    if (threads <= threads_.size()) return;
    // Under the lock a new thread cannot miss the round that is running (it serves the rounds
    // after `seen`), and the round waits for it.
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t seen = active_ ? round_ - 1 : round_;
    while (threads_.size() < threads) {
        const unsigned worker = static_cast<unsigned>(threads_.size()) + 1;
        if (active_) busy_++;
        threads_.emplace_back(&PagePool::Serve, this, worker, seen);
    }
}

void PagePool::Run(size_t count, void (*run)(void*, unsigned, size_t), void* context) {
    if (count == 0) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        run_ = run;
        context_ = context;
        count_ = count;
        next_.store(0, std::memory_order_relaxed);
        busy_ = static_cast<unsigned>(threads_.size());
        active_ = true;
        round_++;
    }
    start_.notify_all();
    Take(0);
    // The pool's threads may still run their last index (and read the round) until busy_ is 0.
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [this] { return busy_ == 0; });
    active_ = false;
}

void PagePool::Take(unsigned worker) {
    for (size_t index; (index = next_.fetch_add(1, std::memory_order_relaxed)) < count_;) run_(context_, worker, index);
}

void PagePool::Serve(unsigned worker, uint64_t seen) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        start_.wait(lock, [&] { return closing_ || round_ != seen; });
        if (closing_) return;
        seen = round_;
        lock.unlock();
        Take(worker);
        lock.lock();
        if (--busy_ == 0) done_.notify_one();
    }
}

} // namespace RayoPdf::Layout
