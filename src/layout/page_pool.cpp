// SPDX-License-Identifier: Apache-2.0
#include "layout/page_pool.h"

namespace RayoPdf::Layout {

PagePool::PagePool(unsigned workers) : workers_(workers < 2 ? 2 : workers) {
    threads_.reserve(workers_ - 1);
    rounds_.reserve(4);
}

PagePool::~PagePool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
    }
    start_.notify_all();
    for (Thread& thread : threads_) thread.Join();
}

void PagePool::Start(unsigned threads) {
    if (threads > workers_ - 1) threads = workers_ - 1;
    if (threads <= started_.load(std::memory_order_relaxed)) return;
    // A new thread looks for open rounds under the lock, so it joins the rounds that run.
    std::lock_guard<std::mutex> lock(mutex_);
    while (threads_.size() < threads) {
        const unsigned worker = static_cast<unsigned>(threads_.size()) + 1;
        threads_.emplace_back([this, worker] { Serve(worker); });
    }
    started_.store(static_cast<unsigned>(threads_.size()), std::memory_order_relaxed);
}

void PagePool::Run(unsigned caller, size_t count, void (*run)(void*, unsigned, size_t), void* context) {
    if (count == 0) return;
    Round round{run, context, count};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rounds_.push_back(&round);
    }
    start_.notify_all();
    Take(round, caller);
    // Closed to new helpers; those inside may still run their last index (and read the round).
    std::unique_lock<std::mutex> lock(mutex_);
    for (size_t i = rounds_.size(); i-- > 0;) {
        if (rounds_[i] == &round) {
            rounds_.erase(rounds_.begin() + static_cast<ptrdiff_t>(i));
            break;
        }
    }
    done_.wait(lock, [&round] { return round.helpers == 0; });
}

void PagePool::Take(Round& round, unsigned worker) {
    for (size_t index; (index = round.next.fetch_add(1, std::memory_order_relaxed)) < round.count;) {
        round.run(round.context, worker, index);
    }
}

PagePool::Round* PagePool::Open() const {
    for (Round* round : rounds_) {
        if (round->next.load(std::memory_order_relaxed) < round->count) return round;
    }
    return nullptr;
}

void PagePool::Serve(unsigned worker) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        Round* round = nullptr;
        start_.wait(lock, [&] { return closing_ || (round = Open()) != nullptr; });
        if (closing_) return;
        round->helpers++;
        lock.unlock();
        Take(*round, worker);
        lock.lock();
        if (--round->helpers == 0) done_.notify_all();
    }
}

} // namespace RayoPdf::Layout
