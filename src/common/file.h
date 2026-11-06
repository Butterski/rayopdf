// SPDX-License-Identifier: Apache-2.0
// Whole-file input and output through the OS, without iostreams (roadmap D1).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace RayoPdf::File {

enum class ReadStatus : uint8_t {
    Ok,
    NotFound,    // no file at the path, or no permission to open it
    NotRegular,  // a directory, FIFO, device or socket
    TooLarge,    // larger than the caller's maximum
    Failed,      // the open file could not be mapped or read
};

// A read-only view of a whole regular file: memory-mapped, or read into memory where mapping is
// not possible. The bytes stay valid and unchanged while the object lives (a file truncated by
// another process meanwhile can fault a mapped read; RayoPDF reads its inputs once, start to end).
class InputFile {
public:
    InputFile() = default;
    ~InputFile();
    InputFile(const InputFile&) = delete;
    InputFile& operator=(const InputFile&) = delete;

    // Opens `path` (UTF-8). A FIFO or device is refused before anything can wait on it.
    ReadStatus Open(const std::string& path, uint64_t maximumBytes);
    std::string_view Bytes() const { return std::string_view(data_, size_); }

private:
    void Close();

    const char* data_ = nullptr;
    size_t size_ = 0;
    bool mapped_ = false;
    std::string owned_;
};

// Creates or replaces the file at `path` (UTF-8) with `content`: written to a temporary file in the
// same directory, then renamed over the target, so a failed write never leaves a partial file. "-"
// writes to standard output. False when any step fails.
bool WriteAll(const std::string& path, std::string_view content);

} // namespace RayoPdf::File
