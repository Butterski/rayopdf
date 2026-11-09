// SPDX-License-Identifier: Apache-2.0
// Derived from RayoMD src/cli/no_unwind.cpp at ef106ca (Apache-2.0, same author).
// The Linux release command line's C++ exception runtime: a throw ends the process.
// RayoPDF is built without exceptions and catches nothing (roadmap D1), so what libstdc++ throws
// (std::bad_alloc, std::length_error, std::system_error of a thread that cannot start) ended it
// before, after a search of the stack for a handler that is not there; libstdc++'s verbose
// terminate handler then printed the type, demangled, and its what(). That search linked libgcc's
// unwinder, the personality routines, the emergency exception pool and the demangler: these
// definitions take their place, so those archive members are not linked, and no_unwind.ld drops
// the unwind tables only they read. A throw prints what the verbose handler printed and aborts.
// In the static build these also stand for what glibc's libc.a calls of the unwinder (stdio's
// cleanup regions, a thread's exit or cancellation): RayoPDF neither exits nor cancels a thread
// (its threads return from their function), and nothing unwinds.
#include <cxxabi.h>
#include <unwind.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <typeinfo>

namespace {

[[noreturn]] void Fail(const char* message) {
    std::fputs(message, stderr);
    std::abort();
}

// A type name as the demangler wrote it, for the names libstdc++ throws: "St<length><name>" is
// "std::<name>"; any other name is written as the ABI has it.
void PutTypeName(const char* name) {
    if (name[0] == 'S' && name[1] == 't' && name[2] >= '1' && name[2] <= '9') {
        size_t length = 0;
        const char* at = name + 2;
        for (int digits = 0; digits < 3 && *at >= '0' && *at <= '9'; ++digits, ++at) {
            length = length * 10 + static_cast<size_t>(*at - '0');
        }
        if (std::strlen(at) == length) {
            std::fputs("std::", stderr);
            std::fputs(at, stderr);
            return;
        }
    }
    std::fputs(name, stderr);
}

constexpr const char* kNoException = "terminate called without an active exception\n";

} // namespace

namespace __cxxabiv1 {

extern "C" void* __cxa_allocate_exception(std::size_t size) noexcept {
    void* const object = std::malloc(size);
    if (object == nullptr) Fail("terminate called after throwing an instance of 'std::bad_alloc'\n");
    return object;
}

extern "C" void __cxa_free_exception(void* object) noexcept {
    std::free(object);
}

// Every type libstdc++ throws derives from std::exception, at the start of the object.
extern "C" void __cxa_throw(void* object, std::type_info* type, void (*)(void*)) {
    std::fputs("terminate called after throwing an instance of '", stderr);
    PutTypeName(type->name());
    std::fputs("'\n  what():  ", stderr);
    std::fputs(static_cast<const std::exception*>(object)->what(), stderr);
    Fail("\n");
}

extern "C" void __cxa_rethrow() {
    Fail(kNoException);
}

extern "C" void* __cxa_begin_catch(void*) noexcept {
    Fail(kNoException);
}

extern "C" void __cxa_end_catch() {
    Fail(kNoException);
}

extern "C" _Unwind_Reason_Code __gxx_personality_v0(int, _Unwind_Action, _Unwind_Exception_Class, _Unwind_Exception*,
                                                    _Unwind_Context*) {
    Fail(kNoException);
}

extern "C" void __cxa_call_unexpected(void*) {
    Fail(kNoException);
}

extern "C" void* __cxa_get_exception_ptr(void*) noexcept {
    Fail(kNoException);
}

// No exception is ever in flight or caught.
extern "C" std::type_info* __cxa_current_exception_type() noexcept {
    return nullptr;
}

} // namespace __cxxabiv1

namespace std {

bool uncaught_exception() noexcept {
    return false;
}

int uncaught_exceptions() noexcept {
    return 0;
}

} // namespace std

namespace __gnu_cxx {

// std::terminate's handler (a joinable std::thread destroyed): no exception is ever active.
void __verbose_terminate_handler() {
    Fail(kNoException);
}

} // namespace __gnu_cxx

extern "C" void _Unwind_Resume(_Unwind_Exception*) {
    Fail(kNoException);
}

extern "C" _Unwind_Reason_Code __gcc_personality_v0(int, _Unwind_Action, _Unwind_Exception_Class, _Unwind_Exception*,
                                                    _Unwind_Context*) {
    Fail(kNoException);
}

extern "C" _Unwind_Reason_Code _Unwind_ForcedUnwind(_Unwind_Exception*, _Unwind_Stop_Fn, void*) {
    Fail("rayopdf: a thread exited or was cancelled, which needs an unwinder\n");
}

extern "C" _Unwind_Word _Unwind_GetCFA(_Unwind_Context*) {
    Fail("rayopdf: a thread exited or was cancelled, which needs an unwinder\n");
}

// As libstdc++'s operator new: the new-handler is called while malloc fails; without one, null
// (libstdc++'s catches the std::bad_alloc of the throwing form, which would end the process here).
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (size == 0) size = 1;
    for (;;) {
        if (void* memory = std::malloc(size)) return memory;
        const std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) return nullptr;
        handler();
    }
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}
