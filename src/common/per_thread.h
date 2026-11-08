// SPDX-License-Identifier: Apache-2.0
// Working storage kept by each thread until the thread ends: PerThread<T>() is the calling
// thread's T, made (value-initialized) at its first use.
//
// Not a `thread_local` object on Windows: MinGW-w64's GCC keeps thread_local objects in emulated
// TLS, and at a thread's end winpthreads runs its key destructors in the order the keys were made.
// The emulated-TLS key is made first (at the first thread_local access, before the first
// destructor is registered), so the TLS blocks are freed before libstdc++ runs the objects'
// destructors: a thread_local std::vector is destroyed after its memory went back to the heap and
// frees whatever the heap wrote there (a crash at thread exit, seen in 2 of 3 runs of the Windows
// layout_ocr test). There the object lives behind a pthread key of its own, whose destructor
// deletes it while it is still whole. Elsewhere (glibc) it is a plain thread_local.
#pragma once

#if defined(_WIN32)
#include <pthread.h>

#include <cstdlib>
#endif

namespace RayoPdf {

template <typename T>
T& PerThread() {
#if defined(_WIN32)
    static const pthread_key_t key = [] {
        pthread_key_t made;
        if (pthread_key_create(&made, [](void* object) { delete static_cast<T*>(object); }) != 0) std::abort();
        return made;
    }();
    void* object = pthread_getspecific(key);
    if (!object) {
        object = new T();
        if (pthread_setspecific(key, object) != 0) std::abort();
    }
    return *static_cast<T*>(object);
#else
    thread_local T object{};
    return object;
#endif
}

} // namespace RayoPdf
