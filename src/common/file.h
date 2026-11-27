// SPDX-License-Identifier: Apache-2.0
// Files, folders and the standard streams through the OS, without iostreams or <filesystem>
// (roadmap D1): POSIX on Linux, the Win32 API on Windows. Paths are UTF-8 everywhere; on Windows
// they are converted to UTF-16 for the system, and '/' and '\' both separate folders. Every
// function may be called from several threads at once.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace RayoPdf::File {

enum class ReadStatus : uint8_t {
    Ok,
    NotFound,    // no file at the path, or no permission to open it
    NotRegular,  // a directory, FIFO, device or socket
    TooLarge,    // larger than the caller's maximum
    Failed,      // the open file could not be mapped or read
};

// Files up to this size are read into a caller's buffer rather than mapped, when the caller keeps
// one (InputFile::Open): below it the mapping's fixed costs (mmap, munmap, page faults) exceed
// the copy's; above it a mapping touches only the pages the conversion reads.
constexpr uint64_t kBufferedReadBytes = uint64_t(256) << 10;

// A read-only view of a whole regular file: memory-mapped, or read into memory where mapping is
// not possible. The bytes stay valid and unchanged while the object lives (a file truncated by
// another process meanwhile can fault a mapped read; RayoPDF reads its inputs once, start to end).
class InputFile {
public:
    InputFile() = default;
    ~InputFile();
    InputFile(const InputFile&) = delete;
    InputFile& operator=(const InputFile&) = delete;

    // Opens `path` (UTF-8). A FIFO or device is refused before anything can wait on it. With
    // `buffer`, a file of at most kBufferedReadBytes is read into it instead: a caller that keeps
    // the buffer from file to file (a batch worker) makes and removes no mapping for each one, and
    // touches the buffer's memory once. The buffer only grows; Bytes() views its start, so it must
    // stay unchanged while they are used.
    ReadStatus Open(const std::string& path, uint64_t maximumBytes, std::string* buffer = nullptr);
    std::string_view Bytes() const { return std::string_view(data_, size_); }
    // Releases the file (unmaps it) early; Bytes() is empty afterwards. Windows deletes no file
    // while it is mapped.
    void Close();

private:
    const char* data_ = nullptr;
    size_t size_ = 0;
    bool mapped_ = false;
    std::string owned_;
};

// Creates or replaces the file at `path` (UTF-8) with `content`: written to a temporary file in the
// same directory, then renamed over the target, so a failed write never leaves a partial file. "-"
// writes to standard output. False when any step fails.
bool WriteAll(const std::string& path, std::string_view content);

// Creates the directory `path` (one level; its parent must exist) unless a directory is there
// already. False when it cannot, or when something else has the name.
bool EnsureDirectory(const std::string& path);

// Deletes the empty directory at `path`. False when it cannot.
bool RemoveEmptyDirectory(const std::string& path);

enum class FileType : uint8_t { None, Regular, Directory, Other };

struct FileInfo {
    FileType type = FileType::None;  // None: nothing there, or it cannot be examined
    uint64_t size = 0;
    int64_t modified = 0;            // last modification, in the system's units: compare only
};

// What is at `path`, symbolic links followed. False (and type None) when nothing can be found.
bool StatusOf(const std::string& path, FileInfo& info);

struct DirectoryEntry {
    std::string name;   // UTF-8, without the folder
    FileType type = FileType::None;  // of the target, for a symbolic link
    bool link = false;  // a symbolic link (or, on Windows, another reparse point)
};

// The entries of the directory `path` except "." and "..", in the order the system lists them.
// False when the directory cannot be read.
bool ListDirectory(const std::string& path, std::vector<DirectoryEntry>& entries);

// `path` made absolute against the working directory, with "." and ".." segments resolved
// lexically and '/' as the separator ("/a/b" on Linux, "C:/a/b" on Windows).
std::string AbsolutePath(const std::string& path);

// A file written from start to end in pieces (a report): created or truncated by Open, "-" for
// standard output. Unlike WriteAll, what was written stays if the process stops halfway.
class OutputStream {
public:
    OutputStream() = default;
    ~OutputStream();
    OutputStream(const OutputStream&) = delete;
    OutputStream& operator=(const OutputStream&) = delete;

    bool Open(const std::string& path);
    bool IsOpen() const { return open_; }
    bool Write(std::string_view content);
    // False when the file could not be closed (or was not open).
    bool Close();

private:
    bool open_ = false;
    bool standardOutput_ = false;
    intptr_t handle_ = -1;  // a file descriptor, or a Windows HANDLE
};

// A file written in pieces, bytes already written set again in place (WriteAt), that appears under
// its name only when CommitAs succeeds: as WriteAll, a temporary file beside it, renamed. Without a
// CommitAs (or when it fails) the temporary file is removed. A file whose name is known only later
// (an image made on a thread, named in page order) is closed with Close before CommitAs.
class StagedFile {
public:
    StagedFile() = default;
    ~StagedFile();
    StagedFile(const StagedFile&) = delete;
    StagedFile& operator=(const StagedFile&) = delete;

    bool Open(const std::string& path);
    bool Write(std::string_view content);
    bool WriteAt(uint64_t offset, std::string_view content);
    // Ends the writing and keeps the temporary file, closed, for CommitAs. False (and the file
    // removed) when it cannot be closed.
    bool Close();
    // Names the file `path` (Open's path, or another in the same directory); after Close too.
    bool CommitAs(const std::string& path);
    // Removes the temporary file, as the destructor does.
    void Discard();

private:
    std::string path_;
    std::string temp_;
    intptr_t handle_ = -1;  // a file descriptor, or a Windows HANDLE, while the file is open
    bool staged_ = false;   // the temporary file exists
};

// The standard streams as bytes: no text-mode translation and no stdio buffer. A write is one
// system call for the whole text (repeated only when the system takes part of it), so the lines of
// several threads do not mix. ReadStandardInput puts into `got` what the input has, up to `size`
// bytes, as soon as it has any (never waiting for a full buffer), and 0 at its end; false (and
// `got` 0) when the read fails, which is not the end of the input.
bool WriteStandardOutput(std::string_view content);
bool WriteStandardError(std::string_view content);
bool ReadStandardInput(char* buffer, size_t size, size_t& got);

// The command line's arguments as UTF-8: `argv` as given on Linux; on Windows the wide command line,
// as `argv` holds only the system's ANSI code page. Puts the standard streams into binary mode there.
std::vector<std::string> Arguments(int argc, char** argv);

} // namespace RayoPdf::File
