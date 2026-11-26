// SPDX-License-Identifier: Apache-2.0
#include "layout/ocr_process.h"

#include <climits>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cstdlib>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char** environ;
#endif

namespace RayoPdf::Layout {

namespace {

// Milliseconds left until `deadline`, rounded up; 0 when it has passed.
int RemainingMs(OcrClock::time_point deadline) {
    const OcrClock::time_point now = OcrClock::now();
    if (deadline <= now) return 0;
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count() + 1;
    return ms > INT_MAX ? INT_MAX : static_cast<int>(ms);
}

std::string Directory(std::string path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos) return std::string();
    path.resize(slash == 0 ? 1 : slash);
    return path;
}

} // namespace

ChildProcess::~ChildProcess() {
    Kill();
}

#ifdef _WIN32

namespace {

std::wstring Wide(std::string_view text) {
    if (text.empty() || text.size() > INT_MAX) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], n);
    return out;
}

std::string Narrow(const wchar_t* text, int length) {
    if (length <= 0) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, &out[0], n, nullptr, nullptr);
    return out;
}

// One argument quoted the way CommandLineToArgvW and the C runtimes split a command line.
void AppendArgument(std::wstring& line, const std::wstring& arg) {
    if (!line.empty()) line.push_back(L' ');
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        line += arg;
        return;
    }
    line.push_back(L'"');
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        line.append(c == L'"' ? 2 * backslashes + 1 : backslashes, L'\\');
        backslashes = 0;
        line.push_back(c);
    }
    line.append(2 * backslashes, L'\\');
    line.push_back(L'"');
}

void CloseHandleIfOpen(void*& handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(static_cast<HANDLE>(handle));
    handle = nullptr;
}

// An overlapped read or write on `handle`, waited for until `deadline`. False on a failure or a
// timeout (`timedOut` set); a timed-out operation is cancelled first.
bool Transfer(HANDLE handle, HANDLE event, bool write, char* data, DWORD size, OcrClock::time_point deadline,
              DWORD& done, bool& timedOut) {
    done = 0;
    OVERLAPPED ov{};
    ov.hEvent = event;
    ResetEvent(event);
    const BOOL started = write ? WriteFile(handle, data, size, nullptr, &ov) : ReadFile(handle, data, size, nullptr, &ov);
    if (!started && GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(event, static_cast<DWORD>(RemainingMs(deadline))) != WAIT_OBJECT_0) {
        CancelIoEx(handle, &ov);
        GetOverlappedResult(handle, &ov, &done, TRUE);
        timedOut = true;
        return false;
    }
    return GetOverlappedResult(handle, &ov, &done, FALSE) != 0;
}

std::atomic<unsigned> pipeCounter{0};

} // namespace

bool ChildProcess::Start(const std::string& path, const std::vector<std::string>& args, bool keepStandardError) {
    Kill();
    timedOut_ = false;
    const unsigned serial = pipeCounter.fetch_add(1);
    const std::wstring base = L"\\\\.\\pipe\\rayopdf-ocr-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(serial);
    const std::wstring inName = base + L"-in", outName = base + L"-out";
    constexpr DWORD kBuffer = 64 * 1024;
    const DWORD mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
    input_ = CreateNamedPipeW(inName.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                              mode, 1, kBuffer, kBuffer, 0, nullptr);
    output_ = CreateNamedPipeW(outName.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                               mode, 1, kBuffer, kBuffer, 0, nullptr);
    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (input_ == INVALID_HANDLE_VALUE || output_ == INVALID_HANDLE_VALUE || event_ == nullptr) {
        CloseHandles();
        return false;
    }
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    void* childIn = CreateFileW(inName.c_str(), GENERIC_READ, 0, &inherit, OPEN_EXISTING, 0, nullptr);
    void* childOut = CreateFileW(outName.c_str(), GENERIC_WRITE, 0, &inherit, OPEN_EXISTING, 0, nullptr);
    void* childErr = nullptr;
    const HANDLE ownErr = GetStdHandle(STD_ERROR_HANDLE);
    if (keepStandardError && ownErr != nullptr && ownErr != INVALID_HANDLE_VALUE)
        DuplicateHandle(GetCurrentProcess(), ownErr, GetCurrentProcess(), reinterpret_cast<HANDLE*>(&childErr), 0, TRUE,
                        DUPLICATE_SAME_ACCESS);
    if (childErr == nullptr)
        childErr = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0,
                               nullptr);
    bool started = false;
    if (childIn != INVALID_HANDLE_VALUE && childOut != INVALID_HANDLE_VALUE && childErr != INVALID_HANDLE_VALUE) {
        // Only these three handles reach the child, whatever other threads make inheritable meanwhile.
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::string storage(size, '\0');
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(&storage[0]);
        HANDLE handles[3] = {childIn, childOut, childErr};
        if (size > 0 && InitializeProcThreadAttributeList(list, 1, 0, &size)) {
            if (UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof handles, nullptr,
                                          nullptr)) {
                STARTUPINFOEXW info{};
                info.StartupInfo.cb = sizeof info;
                info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
                info.StartupInfo.hStdInput = childIn;
                info.StartupInfo.hStdOutput = childOut;
                info.StartupInfo.hStdError = childErr;
                info.lpAttributeList = list;
                const std::wstring program = Wide(path);
                std::wstring line;
                AppendArgument(line, program);
                for (const std::string& arg : args) AppendArgument(line, Wide(arg));
                PROCESS_INFORMATION process{};
                if (!program.empty() &&
                    CreateProcessW(program.c_str(), &line[0], nullptr, nullptr, TRUE,
                                   EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
                                   &info.StartupInfo, &process)) {
                    CloseHandle(process.hThread);
                    process_ = process.hProcess;
                    started = true;
                }
            }
            DeleteProcThreadAttributeList(list);
        }
    }
    CloseHandleIfOpen(childIn);
    CloseHandleIfOpen(childOut);
    CloseHandleIfOpen(childErr);
    if (!started) CloseHandles();
    return started;
}

bool ChildProcess::Running() const {
    return process_ != nullptr;
}

bool ChildProcess::Write(std::string_view data, OcrClock::time_point deadline) {
    if (input_ == nullptr) return false;
    while (!data.empty()) {
        const DWORD size = static_cast<DWORD>(data.size() < (1u << 20) ? data.size() : (1u << 20));
        DWORD done = 0;
        if (!Transfer(static_cast<HANDLE>(input_), static_cast<HANDLE>(event_), true, const_cast<char*>(data.data()),
                      size, deadline, done, timedOut_) ||
            done == 0) {
            return false;
        }
        data.remove_prefix(done);
    }
    return true;
}

size_t ChildProcess::Read(char* buffer, size_t size, OcrClock::time_point deadline) {
    if (output_ == nullptr || size == 0) return 0;
    DWORD done = 0;
    const DWORD want = static_cast<DWORD>(size < (1u << 20) ? size : (1u << 20));
    if (!Transfer(static_cast<HANDLE>(output_), static_cast<HANDLE>(event_), false, buffer, want, deadline, done,
                  timedOut_)) {
        return 0;
    }
    return done;
}

void ChildProcess::Finish(OcrClock::time_point deadline) {
    CloseHandleIfOpen(input_);
    if (process_ != nullptr &&
        WaitForSingleObject(static_cast<HANDLE>(process_), static_cast<DWORD>(RemainingMs(deadline))) == WAIT_OBJECT_0) {
        CloseHandles();
        return;
    }
    Kill();
}

void ChildProcess::Kill() {
    if (process_ != nullptr) {
        TerminateProcess(static_cast<HANDLE>(process_), 1);
        WaitForSingleObject(static_cast<HANDLE>(process_), 10000);
    }
    CloseHandles();
}

void ChildProcess::CloseHandles() {
    CloseHandleIfOpen(input_);
    CloseHandleIfOpen(output_);
    CloseHandleIfOpen(event_);
    CloseHandleIfOpen(process_);
}

std::string ExecutableDirectory() {
    std::wstring buffer(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, &buffer[0], static_cast<DWORD>(buffer.size()));
    if (n == 0 || n >= buffer.size()) return std::string();
    std::string path = Narrow(buffer.data(), static_cast<int>(n));
    for (char& c : path)
        if (c == '\\') c = '/';
    return Directory(path);
}

std::string EnvironmentValue(const char* name) {
    const std::wstring wide = Wide(name);
    if (wide.empty()) return std::string();
    const DWORD n = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
    if (n == 0) return std::string();
    std::wstring value(n, L'\0');
    const DWORD got = GetEnvironmentVariableW(wide.c_str(), &value[0], n);
    if (got == 0 || got >= n) return std::string();
    return Narrow(value.data(), static_cast<int>(got));
}

bool IsExecutableFile(const std::string& path) {
    const std::wstring wide = Wide(path);
    if (wide.empty()) return false;
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

#else  // POSIX

bool ChildProcess::Start(const std::string& path, const std::vector<std::string>& args, bool keepStandardError) {
    Kill();
    timedOut_ = false;
    int fds[2];
#ifdef __APPLE__
    // macOS has no SOCK_CLOEXEC: the child closes every descriptor its file actions do not name
    // (POSIX_SPAWN_CLOEXEC_DEFAULT below), which also keeps these two out of the add-ons that
    // other batch workers start at the same time.
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return false;
#else
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) return false;
#endif
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attributes);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 0);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    if (!keepStandardError) posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
#ifdef __APPLE__
    else posix_spawn_file_actions_addinherit_np(&actions, 2);
#endif
    // The child starts with SIGPIPE at its default and no signal blocked, whatever this process set.
    sigset_t none, pipe;
    sigemptyset(&none);
    sigemptyset(&pipe);
    sigaddset(&pipe, SIGPIPE);
    posix_spawnattr_setsigmask(&attributes, &none);
    posix_spawnattr_setsigdefault(&attributes, &pipe);
#ifdef __APPLE__
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_CLOEXEC_DEFAULT);
#else
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
#endif
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(path.c_str()));
    for (const std::string& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int status = posix_spawn(&pid, path.c_str(), &actions, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(fds[1]);
    if (status != 0) {
        close(fds[0]);
        return false;
    }
    pid_ = static_cast<int>(pid);
    socket_ = fds[0];
    const int flags = fcntl(socket_, F_GETFL);
    if (flags < 0 || fcntl(socket_, F_SETFL, flags | O_NONBLOCK) != 0) {
        Kill();
        return false;
    }
#ifdef __APPLE__
    // A write to an add-on that has gone fails with EPIPE instead of raising SIGPIPE (macOS has
    // no MSG_NOSIGNAL); and no other program this process starts inherits the socket.
    const int on = 1;
    if (setsockopt(socket_, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on) != 0 || fcntl(socket_, F_SETFD, FD_CLOEXEC) != 0) {
        Kill();
        return false;
    }
#endif
    return true;
}

bool ChildProcess::Running() const {
    return pid_ > 0;
}

bool ChildProcess::Write(std::string_view data, OcrClock::time_point deadline) {
    if (socket_ < 0) return false;
    while (!data.empty()) {
#ifdef __APPLE__
        const ssize_t n = send(socket_, data.data(), data.size(), 0);
#else
        const ssize_t n = send(socket_, data.data(), data.size(), MSG_NOSIGNAL);
#endif
        if (n > 0) {
            data.remove_prefix(static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const int wait = RemainingMs(deadline);
            pollfd p{socket_, POLLOUT, 0};
            const int ready = wait > 0 ? poll(&p, 1, wait) : 0;
            if (ready == 0) {
                timedOut_ = true;
                return false;
            }
            if (ready < 0 && errno != EINTR) return false;
            continue;
        }
        return false;
    }
    return true;
}

size_t ChildProcess::Read(char* buffer, size_t size, OcrClock::time_point deadline) {
    if (socket_ < 0 || size == 0) return 0;
    for (;;) {
        const ssize_t n = recv(socket_, buffer, size, 0);
        if (n >= 0) return static_cast<size_t>(n);
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return 0;
        const int wait = RemainingMs(deadline);
        pollfd p{socket_, POLLIN, 0};
        const int ready = wait > 0 ? poll(&p, 1, wait) : 0;
        if (ready == 0) {
            timedOut_ = true;
            return 0;
        }
        if (ready < 0 && errno != EINTR) return 0;
    }
}

void ChildProcess::Finish(OcrClock::time_point deadline) {
    if (socket_ >= 0) shutdown(socket_, SHUT_WR);
    // Waits for the exit in steps of a millisecond, at most until the deadline.
    while (pid_ > 0) {
        int status = 0;
        const pid_t done = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
        if (done == static_cast<pid_t>(pid_) || (done < 0 && errno != EINTR)) {
            pid_ = -1;
            break;
        }
        if (RemainingMs(deadline) == 0) break;
        const timespec step{0, 1000000};
        nanosleep(&step, nullptr);
    }
    Kill();
}

void ChildProcess::Kill() {
    if (pid_ > 0) {
        kill(static_cast<pid_t>(pid_), SIGKILL);
        int status = 0;
        while (waitpid(static_cast<pid_t>(pid_), &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
    }
    CloseHandles();
}

void ChildProcess::CloseHandles() {
    if (socket_ >= 0) close(socket_);
    socket_ = -1;
}

std::string ExecutableDirectory() {
#ifdef __APPLE__
    char buffer[4096];
    uint32_t size = sizeof buffer;
    if (_NSGetExecutablePath(buffer, &size) != 0) return std::string();
    char resolved[PATH_MAX];
    if (!realpath(buffer, resolved)) return std::string();
    return Directory(std::string(resolved));
#else
    char buffer[4096];
    const ssize_t n = readlink("/proc/self/exe", buffer, sizeof buffer);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof buffer) return std::string();
    return Directory(std::string(buffer, static_cast<size_t>(n)));
#endif
}

std::string EnvironmentValue(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

bool IsExecutableFile(const std::string& path) {
    struct stat info;
    return !path.empty() && stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && access(path.c_str(), X_OK) == 0;
}

#endif

} // namespace RayoPdf::Layout
