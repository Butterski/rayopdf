// SPDX-License-Identifier: Apache-2.0
// File functions only the tests use: a fresh temporary folder and the removal of a file, on
// POSIX and on Windows (paths UTF-8, as in common/file.h).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <stdlib.h>
#include <unistd.h>
#endif

namespace RayoPdf::TestFiles {

#ifdef _WIN32

// `text` (UTF-8) as UTF-16; false when it is not UTF-8 or holds a NUL.
inline bool Widen(std::string_view text, std::wstring& out) {
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

// A new, empty directory under GetTempPath whose name starts with `prefix`; its path in `path`.
// False when none can be made.
inline bool MakeTemporaryDirectory(std::string_view prefix, std::string& path) {
    static std::atomic<uint32_t> counter{0};
    wchar_t base[MAX_PATH + 1];
    const DWORD length = GetTempPathW(MAX_PATH + 1, base);
    if (length == 0 || length > MAX_PATH) return false;
    const int need = WideCharToMultiByte(CP_UTF8, 0, base, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return false;
    std::string folder(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, base, static_cast<int>(length), folder.data(), need, nullptr, nullptr);
    for (char& c : folder) {
        if (c == '\\') c = '/';
    }
    if (folder.empty() || folder.back() != '/') folder += '/';
    folder.append(prefix.data(), prefix.size());
    for (int attempt = 0; attempt < 64; ++attempt) {
        const uint32_t count = counter.fetch_add(1, std::memory_order_relaxed);
        const std::string candidate = folder + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(count);
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

// Deletes the file at `path`. False when it cannot.
inline bool RemoveFile(const std::string& path) {
    std::wstring wide;
    return !path.empty() && Widen(path, wide) && DeleteFileW(wide.c_str());
}

#else  // POSIX

// A new, empty directory under $TMPDIR (or /tmp) whose name starts with `prefix`; its path in
// `path`. False when none can be made.
inline bool MakeTemporaryDirectory(std::string_view prefix, std::string& path) {
    const char* base = std::getenv("TMPDIR");
    std::string pattern = base && *base ? base : "/tmp";
    if (pattern.back() != '/') pattern += '/';
    pattern.append(prefix.data(), prefix.size());
    pattern += "XXXXXX";
    if (!mkdtemp(pattern.data())) return false;
    path = pattern;
    return true;
}

// Deletes the file at `path`. False when it cannot.
inline bool RemoveFile(const std::string& path) { return !path.empty() && unlink(path.c_str()) == 0; }

#endif

} // namespace RayoPdf::TestFiles
