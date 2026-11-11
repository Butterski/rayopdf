// SPDX-License-Identifier: Apache-2.0
// The threads one conversion may use, as the modules below layout see them: the image module
// decodes and encodes on them (image/image.h). layout/page_pool.h is the implementation.
#pragma once

#include <cstddef>
#include <type_traits>

namespace RayoPdf {

class Threads {
public:
    // The most threads that work at once, the thread that owns the pool included (2 or more).
    virtual unsigned Workers() const = 0;

    // The pool's own threads that run (below Workers()).
    virtual unsigned Started() const = 0;

    // Starts the pool's threads until `threads` of them run (at most Workers() - 1). A thread
    // costs its start and the first touch of its buffers, so a caller asks for the threads its
    // work is worth. From any thread; threads started during a round join it.
    virtual void Start(unsigned threads) = 0;

    // `run(context, worker, index)` for every index below `count`, each once, taken in
    // increasing order by the calling thread and by the pool's threads that have nothing else to
    // do; `worker` (below Workers()) names the thread that runs an index, for buffers of its
    // own. `caller` is the calling thread's own number: 0 for the thread that owns the pool,
    // inside a job the job's `worker`. Returns when every index is done. A job may call Run
    // itself (a round inside a round): the pool's threads take the indices of the outermost
    // round that has any left.
    virtual void Run(unsigned caller, size_t count, void (*run)(void*, unsigned, size_t), void* context) = 0;

    template <class Job>
    void Run(unsigned caller, size_t count, Job&& job) {
        using Function = std::remove_reference_t<Job>;
        Run(caller, count,
            [](void* self, unsigned worker, size_t index) { (*static_cast<Function*>(self))(worker, index); },
            const_cast<void*>(static_cast<const void*>(&job)));
    }

protected:
    ~Threads() = default;
};

} // namespace RayoPdf
