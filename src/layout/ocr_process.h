// SPDX-License-Identifier: Apache-2.0
// A child process on pipes, as the OCR client (layout/ocr_client.h) runs the add-on: its standard
// input and output connected to this process, every read and write bounded by a deadline, and
// killed when it misbehaves. POSIX: a socket pair (writes to a dead child fail instead of raising
// SIGPIPE) and posix_spawn; Windows: two overlapped named pipes and CreateProcessW with an explicit
// list of inherited handles. Paths and arguments are UTF-8. Not thread-safe: one owner at a time.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace RayoPdf::Layout {

using OcrClock = std::chrono::steady_clock;

class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Starts `path` with `args` (after the program name). The child's standard error goes to the
    // null device unless `keepStandardError`. False when it cannot be started.
    bool Start(const std::string& path, const std::vector<std::string>& args, bool keepStandardError);
    bool Running() const;

    // Writes all of `data` before `deadline`. False on a timeout or when the child's input is gone.
    bool Write(std::string_view data, OcrClock::time_point deadline);
    // Reads what is there, up to `size` bytes, waiting until `deadline` for the first byte: how many
    // (at least 1), or 0 at the end of the child's output, on an error or a timeout (TimedOut()).
    size_t Read(char* buffer, size_t size, OcrClock::time_point deadline);
    bool TimedOut() const { return timedOut_; }

    // Closes the child's input, waits until `deadline` for it to exit, then kills it.
    void Finish(OcrClock::time_point deadline);
    // Kills the child at once and reaps it.
    void Kill();

private:
    void CloseHandles();

    bool timedOut_ = false;
#ifdef _WIN32
    void* process_ = nullptr;
    void* input_ = nullptr;   // the server end of the child's standard input (we write)
    void* output_ = nullptr;  // the server end of the child's standard output (we read)
    void* event_ = nullptr;   // overlapped I/O completion
#else
    int pid_ = -1;
    int socket_ = -1;
#endif
};

// The directory of this process's executable, '/'-separated, without a trailing '/'; empty when
// it cannot be found.
std::string ExecutableDirectory();

// The value of environment variable `name` (UTF-8), empty when unset.
std::string EnvironmentValue(const char* name);

// Whether `path` names an executable regular file.
bool IsExecutableFile(const std::string& path);

} // namespace RayoPdf::Layout
