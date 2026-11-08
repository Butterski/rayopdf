// SPDX-License-Identifier: Apache-2.0
#include "common/file.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
// After windows.h, whose types it uses.
#include <shellapi.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace RayoPdf::File {

#ifdef _WIN32

namespace {

// `text` (UTF-8) as UTF-16; false when it is not UTF-8 or holds a NUL.
bool Widen(std::string_view text, std::wstring& out) {
    out.clear();
    if (text.empty()) return true;
    if (text.size() > 0x7FFFFFFF) return false;
    const int length = static_cast<int>(text.size());
    const int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (need <= 0) return false;
    out.resize(static_cast<size_t>(need));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, out.data(), need) != need) return false;
    return out.find(L'\0') == std::wstring::npos;
}

// `text` (UTF-16) as UTF-8; an unpaired surrogate becomes U+FFFD.
std::string Narrow(const wchar_t* text, size_t size) {
    std::string out;
    if (size == 0 || size > 0x7FFFFFFF) return out;
    const int length = static_cast<int>(size);
    const int need = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return out;
    out.resize(static_cast<size_t>(need));
    WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), need, nullptr, nullptr);
    return out;
}

int64_t Ticks(const FILETIME& time) {
    return static_cast<int64_t>((static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime);
}

bool WriteHandle(HANDLE handle, std::string_view content) {
    size_t done = 0;
    while (done < content.size()) {
        const size_t left = content.size() - done;
        const DWORD chunk = left > (DWORD(1) << 30) ? (DWORD(1) << 30) : static_cast<DWORD>(left);
        DWORD put = 0;
        if (!WriteFile(handle, content.data() + done, chunk, &put, nullptr) || put == 0) return false;
        done += put;
    }
    return true;
}

// Numbers the temporary files of this process, so threads writing beside each other never pick
// the same name.
std::atomic<uint32_t> gTemporaryCount{0};

} // namespace

InputFile::~InputFile() {
    Close();
}

void InputFile::Close() {
    if (mapped_ && data_) UnmapViewOfFile(data_);
    data_ = nullptr;
    size_ = 0;
    mapped_ = false;
    owned_.clear();
}

ReadStatus InputFile::Open(const std::string& path, uint64_t maximumBytes) {
    Close();
    std::wstring wide;
    if (path.empty() || !Widen(path, wide)) return ReadStatus::NotFound;
    // Backup semantics open a directory too, so that it is told apart from a missing file.
    const HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return ReadStatus::NotFound;
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        CloseHandle(file);
        return ReadStatus::NotRegular;
    }
    const uint64_t size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size > maximumBytes || size > SIZE_MAX / 2) {
        CloseHandle(file);
        return ReadStatus::TooLarge;
    }
    if (size == 0) {
        CloseHandle(file);
        data_ = owned_.data();
        return ReadStatus::Ok;
    }
    const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping) {
        void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(size));
        CloseHandle(mapping);
        if (view) {
            CloseHandle(file);
            data_ = static_cast<const char*>(view);
            size_ = static_cast<size_t>(size);
            mapped_ = true;
            return ReadStatus::Ok;
        }
    }
    owned_.resize(static_cast<size_t>(size));
    size_t done = 0;
    while (done < owned_.size()) {
        const size_t left = owned_.size() - done;
        const DWORD chunk = left > (DWORD(1) << 30) ? (DWORD(1) << 30) : static_cast<DWORD>(left);
        DWORD got = 0;
        if (!ReadFile(file, owned_.data() + done, chunk, &got, nullptr) || got == 0) break;
        done += got;
    }
    CloseHandle(file);
    if (done != owned_.size()) {
        owned_.clear();
        return ReadStatus::Failed;
    }
    data_ = owned_.data();
    size_ = owned_.size();
    return ReadStatus::Ok;
}

bool WriteAll(const std::string& path, std::string_view content) {
    if (path == "-") return WriteStandardOutput(content);
    std::wstring target;
    if (path.empty() || !Widen(path, target)) return false;
    // CREATE_NEW never opens a file someone else made; a name taken (by another process) moves on.
    for (int attempt = 0; attempt < 16; ++attempt) {
        const uint32_t count = gTemporaryCount.fetch_add(1, std::memory_order_relaxed);
        const std::wstring temp = target + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                  std::to_wstring(count);
        const HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            return false;
        }
        bool ok = WriteHandle(file, content);
        ok = CloseHandle(file) && ok;
        if (ok && MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
        DeleteFileW(temp.c_str());
        return false;
    }
    return false;
}

StagedFile::~StagedFile() { Discard(); }

void StagedFile::Discard() {
    if (handle_ == -1) return;
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
    handle_ = -1;
    std::wstring temp;
    if (Widen(temp_, temp)) DeleteFileW(temp.c_str());
}

bool StagedFile::Open(const std::string& path) {
    Discard();
    std::wstring target;
    if (path.empty() || path == "-" || !Widen(path, target)) return false;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const uint32_t count = gTemporaryCount.fetch_add(1, std::memory_order_relaxed);
        const std::string temp =
            path + ".tmp-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(count);
        std::wstring wide;
        if (!Widen(temp, wide)) return false;
        const HANDLE file = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            return false;
        }
        handle_ = reinterpret_cast<intptr_t>(file);
        path_ = path;
        temp_ = temp;
        return true;
    }
    return false;
}

bool StagedFile::Write(std::string_view content) {
    return handle_ != -1 && WriteHandle(reinterpret_cast<HANDLE>(handle_), content);
}

bool StagedFile::WriteAt(uint64_t offset, std::string_view content) {
    if (handle_ == -1 || content.size() > (DWORD(1) << 30)) return false;
    const HANDLE file = reinterpret_cast<HANDLE>(handle_);
    OVERLAPPED at{};
    at.Offset = static_cast<DWORD>(offset);
    at.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD put = 0;
    const bool ok = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &put, &at) &&
                    put == content.size();
    // A positioned write moves the file pointer of a synchronous handle: back to the end.
    LARGE_INTEGER zero{};
    return SetFilePointerEx(file, zero, nullptr, FILE_END) && ok;
}

bool StagedFile::Commit() {
    if (handle_ == -1) return false;
    bool ok = CloseHandle(reinterpret_cast<HANDLE>(handle_)) != 0;
    handle_ = -1;
    std::wstring temp, target;
    if (!Widen(temp_, temp) || !Widen(path_, target)) return false;
    if (ok && MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileW(temp.c_str());
    return false;
}

bool EnsureDirectory(const std::string& path) {
    std::wstring wide;
    if (path.empty() || !Widen(path, wide)) return false;
    if (CreateDirectoryW(wide.c_str(), nullptr)) return true;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool MakeTemporaryDirectory(std::string_view prefix, std::string& path) {
    wchar_t base[MAX_PATH + 1];
    const DWORD length = GetTempPathW(MAX_PATH + 1, base);
    if (length == 0 || length > MAX_PATH) return false;
    std::string folder = Narrow(base, length);
    for (char& c : folder) {
        if (c == '\\') c = '/';
    }
    if (folder.empty() || folder.back() != '/') folder += '/';
    folder.append(prefix.data(), prefix.size());
    for (int attempt = 0; attempt < 64; ++attempt) {
        const uint32_t count = gTemporaryCount.fetch_add(1, std::memory_order_relaxed);
        const std::string candidate =
            folder + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(count);
        std::wstring wide;
        if (!Widen(candidate, wide)) return false;
        if (CreateDirectoryW(wide.c_str(), nullptr)) {
            path = candidate;
            return true;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    return false;
}

bool RemoveFile(const std::string& path) {
    std::wstring wide;
    return !path.empty() && Widen(path, wide) && DeleteFileW(wide.c_str());
}

bool RemoveEmptyDirectory(const std::string& path) {
    std::wstring wide;
    return !path.empty() && Widen(path, wide) && RemoveDirectoryW(wide.c_str());
}

bool StatusOf(const std::string& path, FileInfo& info) {
    info = FileInfo();
    std::wstring wide;
    if (path.empty() || !Widen(path, wide)) return false;
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &data)) return false;
    info.type = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? FileType::Directory
                : (data.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) ? FileType::Other
                                                                    : FileType::Regular;
    info.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    info.modified = Ticks(data.ftLastWriteTime);
    return true;
}

bool ListDirectory(const std::string& path, std::vector<DirectoryEntry>& entries) {
    entries.clear();
    std::wstring pattern;
    if (path.empty() || !Widen(path, pattern)) return false;
    if (pattern.back() != L'/' && pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW found;
    const HANDLE search = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr,
                                           FIND_FIRST_EX_LARGE_FETCH);
    if (search == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        const wchar_t* name = found.cFileName;
        if (name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0))) continue;
        DirectoryEntry entry;
        entry.name = Narrow(name, wcslen(name));
        entry.type = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? FileType::Directory : FileType::Regular;
        entry.link = (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        entries.push_back(std::move(entry));
    } while (FindNextFileW(search, &found));
    const bool complete = GetLastError() == ERROR_NO_MORE_FILES;
    FindClose(search);
    return complete;
}

std::string AbsolutePath(const std::string& path) {
    std::wstring wide;
    if (!Widen(path.empty() ? std::string(".") : path, wide)) wide = L".";
    std::wstring full(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 4; ++attempt) {
        const DWORD need = GetFullPathNameW(wide.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
        if (need == 0) return path;
        if (need < full.size()) {
            full.resize(need);
            break;
        }
        full.resize(need + 1);
    }
    std::string out = Narrow(full.data(), full.size());
    for (char& c : out) {
        if (c == '\\') c = '/';
    }
    // "C:/" keeps its slash; any other trailing one goes.
    if (out.size() > 3 && out.back() == '/') out.pop_back();
    return out;
}

OutputStream::~OutputStream() {
    Close();
}

bool OutputStream::Open(const std::string& path) {
    Close();
    if (path == "-") {
        standardOutput_ = true;
        open_ = true;
        return true;
    }
    std::wstring wide;
    if (path.empty() || !Widen(path, wide)) return false;
    const HANDLE file = CreateFileW(wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    handle_ = reinterpret_cast<intptr_t>(file);
    standardOutput_ = false;
    open_ = true;
    return true;
}

bool OutputStream::Write(std::string_view content) {
    if (!open_) return false;
    if (standardOutput_) return WriteStandardOutput(content);
    return WriteHandle(reinterpret_cast<HANDLE>(handle_), content);
}

bool OutputStream::Close() {
    if (!open_) return false;
    open_ = false;
    if (standardOutput_) return true;
    const bool ok = CloseHandle(reinterpret_cast<HANDLE>(handle_)) != 0;
    handle_ = -1;
    return ok;
}

bool WriteStandardOutput(std::string_view content) {
    return WriteHandle(GetStdHandle(STD_OUTPUT_HANDLE), content);
}

bool WriteStandardError(std::string_view content) {
    return WriteHandle(GetStdHandle(STD_ERROR_HANDLE), content);
}

size_t ReadStandardInput(char* buffer, size_t size) {
    const DWORD chunk = size > (DWORD(1) << 30) ? (DWORD(1) << 30) : static_cast<DWORD>(size);
    DWORD got = 0;
    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, chunk, &got, nullptr)) return 0;
    return got;
}

std::vector<std::string> Arguments(int argc, char** argv) {
    // stdio's streams too ("--help", messages): bytes as written, no CR before LF.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    std::vector<std::string> out;
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!wide) {
        for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
        return out;
    }
    out.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) out.push_back(Narrow(wide[i], wcslen(wide[i])));
    LocalFree(wide);
    return out;
}

#else  // POSIX

namespace {

bool WriteFd(int fd, std::string_view content) {
    size_t done = 0;
    while (done < content.size()) {
        const ssize_t put = write(fd, content.data() + done, content.size() - done);
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0) return false;
        done += static_cast<size_t>(put);
    }
    return true;
}

// The process's umask, read once: umask(2) can only be read by setting it, which is not safe while
// other threads create files.
mode_t Umask() {
    static const mode_t mask = [] {
        const mode_t value = umask(0);
        umask(value);
        return value;
    }();
    return mask;
}

FileType TypeOf(mode_t mode) {
    return S_ISREG(mode) ? FileType::Regular : S_ISDIR(mode) ? FileType::Directory : FileType::Other;
}

int64_t Nanoseconds(const struct stat& info) {
    return static_cast<int64_t>(info.st_mtim.tv_sec) * 1000000000 + info.st_mtim.tv_nsec;
}

} // namespace

InputFile::~InputFile() {
    Close();
}

void InputFile::Close() {
    if (mapped_ && data_) munmap(const_cast<char*>(data_), size_);
    data_ = nullptr;
    size_ = 0;
    mapped_ = false;
    owned_.clear();
}

ReadStatus InputFile::Open(const std::string& path, uint64_t maximumBytes) {
    Close();
    // Non-blocking, so a FIFO cannot stall the open; fstat then decides before any read.
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) return ReadStatus::NotFound;
    struct stat info;
    if (fstat(fd, &info) != 0) {
        close(fd);
        return ReadStatus::Failed;
    }
    if (!S_ISREG(info.st_mode)) {
        close(fd);
        return ReadStatus::NotRegular;
    }
    const uint64_t size = static_cast<uint64_t>(info.st_size);
    if (size > maximumBytes || size > SIZE_MAX / 2) {
        close(fd);
        return ReadStatus::TooLarge;
    }
    if (size == 0) {
        close(fd);
        data_ = owned_.data();
        return ReadStatus::Ok;
    }
    void* map = mmap(nullptr, static_cast<size_t>(size), PROT_READ, MAP_PRIVATE, fd, 0);
    if (map != MAP_FAILED) {
        close(fd);
        data_ = static_cast<const char*>(map);
        size_ = static_cast<size_t>(size);
        mapped_ = true;
        return ReadStatus::Ok;
    }
    // Some file systems cannot be mapped: read the file instead.
    const int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    owned_.resize(static_cast<size_t>(size));
    size_t done = 0;
    while (done < owned_.size()) {
        const ssize_t got = read(fd, owned_.data() + done, owned_.size() - done);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        done += static_cast<size_t>(got);
    }
    close(fd);
    if (done != owned_.size()) {
        owned_.clear();
        return ReadStatus::Failed;
    }
    data_ = owned_.data();
    size_ = owned_.size();
    return ReadStatus::Ok;
}

bool WriteAll(const std::string& path, std::string_view content) {
    if (path == "-") return WriteFd(STDOUT_FILENO, content);
    std::string temp = path + ".tmp-XXXXXX";
    const int fd = mkostemp(temp.data(), O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = WriteFd(fd, content);
    // mkostemp creates 0600; give the result the usual permissions under the umask.
    ok = fchmod(fd, 0666 & ~Umask()) == 0 && ok;
    ok = close(fd) == 0 && ok;
    if (ok && rename(temp.c_str(), path.c_str()) == 0) return true;
    unlink(temp.c_str());
    return false;
}

StagedFile::~StagedFile() { Discard(); }

void StagedFile::Discard() {
    if (handle_ == -1) return;
    close(static_cast<int>(handle_));
    handle_ = -1;
    unlink(temp_.c_str());
}

bool StagedFile::Open(const std::string& path) {
    Discard();
    if (path.empty() || path == "-") return false;
    std::string temp = path + ".tmp-XXXXXX";
    const int fd = mkostemp(temp.data(), O_CLOEXEC);
    if (fd < 0) return false;
    handle_ = fd;
    path_ = path;
    temp_ = temp;
    return true;
}

bool StagedFile::Write(std::string_view content) {
    return handle_ != -1 && WriteFd(static_cast<int>(handle_), content);
}

bool StagedFile::WriteAt(uint64_t offset, std::string_view content) {
    if (handle_ == -1) return false;
    size_t done = 0;
    while (done < content.size()) {
        const ssize_t put = pwrite(static_cast<int>(handle_), content.data() + done, content.size() - done,
                                   static_cast<off_t>(offset + done));
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0) return false;
        done += static_cast<size_t>(put);
    }
    return true;
}

bool StagedFile::Commit() {
    if (handle_ == -1) return false;
    const int fd = static_cast<int>(handle_);
    handle_ = -1;
    // mkostemp creates 0600; give the result the usual permissions under the umask (as WriteAll).
    bool ok = fchmod(fd, 0666 & ~Umask()) == 0;
    ok = close(fd) == 0 && ok;
    if (ok && rename(temp_.c_str(), path_.c_str()) == 0) return true;
    unlink(temp_.c_str());
    return false;
}

bool EnsureDirectory(const std::string& path) {
    if (path.empty()) return false;
    if (mkdir(path.c_str(), 0777) == 0) return true;
    if (errno != EEXIST) return false;
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool MakeTemporaryDirectory(std::string_view prefix, std::string& path) {
    const char* base = std::getenv("TMPDIR");
    std::string pattern = base && *base ? base : "/tmp";
    if (pattern.back() != '/') pattern += '/';
    pattern.append(prefix.data(), prefix.size());
    pattern += "XXXXXX";
    if (!mkdtemp(pattern.data())) return false;
    path = pattern;
    return true;
}

bool RemoveFile(const std::string& path) {
    return !path.empty() && unlink(path.c_str()) == 0;
}

bool RemoveEmptyDirectory(const std::string& path) {
    return !path.empty() && rmdir(path.c_str()) == 0;
}

bool StatusOf(const std::string& path, FileInfo& info) {
    info = FileInfo();
    struct stat status;
    if (path.empty() || stat(path.c_str(), &status) != 0) return false;
    info.type = TypeOf(status.st_mode);
    info.size = static_cast<uint64_t>(status.st_size);
    info.modified = Nanoseconds(status);
    return true;
}

bool ListDirectory(const std::string& path, std::vector<DirectoryEntry>& entries) {
    entries.clear();
    if (path.empty()) return false;
    DIR* directory = opendir(path.c_str());
    if (!directory) return false;
    const int fd = dirfd(directory);
    for (;;) {
        errno = 0;
        const dirent* item = readdir(directory);
        if (!item) break;
        const char* name = item->d_name;
        if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
        DirectoryEntry entry;
        entry.name = name;
        switch (item->d_type) {
            case DT_REG: entry.type = FileType::Regular; break;
            case DT_DIR: entry.type = FileType::Directory; break;
            case DT_LNK:
            case DT_UNKNOWN: {
                struct stat status;
                if (fstatat(fd, name, &status, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(status.st_mode)) {
                    entry.link = true;
                    if (fstatat(fd, name, &status, 0) != 0) break;  // a dangling link: type None
                }
                entry.type = TypeOf(status.st_mode);
                break;
            }
            default: entry.type = FileType::Other; break;
        }
        entries.push_back(std::move(entry));
    }
    const bool complete = errno == 0;
    closedir(directory);
    return complete;
}

std::string AbsolutePath(const std::string& path) {
    std::string full = path;
    if (path.empty() || path[0] != '/') {
        char cwd[4096];
        full = (getcwd(cwd, sizeof cwd) ? std::string(cwd) : std::string(".")) + "/" + path;
    }
    std::string out;
    size_t at = 0;
    while (at <= full.size()) {
        size_t next = full.find('/', at);
        if (next == std::string::npos) next = full.size();
        const std::string_view part(full.data() + at, next - at);
        if (part == "..") {
            const size_t cut = out.rfind('/');
            out.resize(cut == std::string::npos ? 0 : cut);
        } else if (!part.empty() && part != ".") {
            out += '/';
            out += part;
        }
        at = next + 1;
    }
    return out.empty() ? "/" : out;
}

OutputStream::~OutputStream() {
    Close();
}

bool OutputStream::Open(const std::string& path) {
    Close();
    if (path == "-") {
        handle_ = STDOUT_FILENO;
        standardOutput_ = true;
        open_ = true;
        return true;
    }
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOCTTY, 0666);
    if (fd < 0) return false;
    handle_ = fd;
    standardOutput_ = false;
    open_ = true;
    return true;
}

bool OutputStream::Write(std::string_view content) {
    return open_ && WriteFd(static_cast<int>(handle_), content);
}

bool OutputStream::Close() {
    if (!open_) return false;
    open_ = false;
    if (standardOutput_) return true;
    const bool ok = close(static_cast<int>(handle_)) == 0;
    handle_ = -1;
    return ok;
}

bool WriteStandardOutput(std::string_view content) {
    return WriteFd(STDOUT_FILENO, content);
}

bool WriteStandardError(std::string_view content) {
    return WriteFd(STDERR_FILENO, content);
}

size_t ReadStandardInput(char* buffer, size_t size) {
    for (;;) {
        const ssize_t got = read(STDIN_FILENO, buffer, size);
        if (got >= 0) return static_cast<size_t>(got);
        if (errno != EINTR) return 0;
    }
}

std::vector<std::string> Arguments(int argc, char** argv) {
    std::vector<std::string> out;
    out.reserve(static_cast<size_t>(argc > 0 ? argc : 0));
    for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
    return out;
}

#endif

} // namespace RayoPdf::File
