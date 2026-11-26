// SPDX-License-Identifier: Apache-2.0
#include "common/thread.h"

#include <cstdlib>

namespace RayoPdf {

#if defined(_WIN32)

Thread::Thread(Thread&& other) noexcept : thread_(std::move(other.thread_)) {}

Thread::~Thread() {
    if (thread_.joinable()) std::abort();
}

void Thread::Start(std::unique_ptr<Task> task) {
    thread_ = std::thread([task = std::move(task)] { task->Run(); });
}

void Thread::Join() {
    thread_.join();
}

#else

Thread::Thread(Thread&& other) noexcept : handle_(other.handle_), joinable_(other.joinable_) {
    other.joinable_ = false;
}

Thread::~Thread() {
    if (joinable_) std::abort();
}

void* Thread::Enter(void* task) {
    const std::unique_ptr<Task> owned(static_cast<Task*>(task));
    owned->Run();
    return nullptr;
}

void Thread::Start(std::unique_ptr<Task> task) {
    // Enter owns the task from here on.
    Task* const owned = task.release();
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) std::abort();
    const bool made = pthread_attr_setstacksize(&attributes, kThreadStackBytes) == 0 &&
                      pthread_create(&handle_, &attributes, &Thread::Enter, owned) == 0;
    pthread_attr_destroy(&attributes);
    if (!made) std::abort();
    joinable_ = true;
}

void Thread::Join() {
    if (!joinable_ || pthread_join(handle_, nullptr) != 0) std::abort();
    joinable_ = false;
}

#endif

} // namespace RayoPdf
