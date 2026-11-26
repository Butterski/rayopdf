// SPDX-License-Identifier: Apache-2.0
// A joinable thread for the page pool, the batch workers and the serve workers. On POSIX it gets
// a stack of kThreadStackBytes: the recursion caps (forms, XY cut, syntax nesting, structure
// depth) were sized on glibc's 8 MiB default, and macOS gives a new thread 512 KiB. On Windows it
// is a std::thread as before.
#pragma once

#include <cstddef>
#include <memory>
#include <utility>

#if defined(_WIN32)
#include <thread>
#else
#include <pthread.h>
#endif

namespace RayoPdf {

constexpr size_t kThreadStackBytes = size_t(8) << 20;

class Thread {
public:
    // Runs `function()` on a new thread; ends the process when no thread can be made (as a
    // std::thread does without exceptions).
    template <class Function>
    explicit Thread(Function function) {
        Start(std::make_unique<Body<Function>>(std::move(function)));
    }
    Thread(Thread&& other) noexcept;
    Thread& operator=(Thread&&) = delete;
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    // A thread not joined ends the process, as a joinable std::thread's destructor does.
    ~Thread();

    void Join();

private:
    struct Task {
        virtual ~Task() = default;
        virtual void Run() = 0;
    };
    template <class Function>
    struct Body final : Task {
        explicit Body(Function&& f) : function(std::move(f)) {}
        void Run() override { function(); }
        Function function;
    };

    void Start(std::unique_ptr<Task> task);

#if defined(_WIN32)
    std::thread thread_;
#else
    static void* Enter(void* task);
    pthread_t handle_{};
    bool joinable_ = false;
#endif
};

} // namespace RayoPdf
