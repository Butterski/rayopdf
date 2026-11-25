// SPDX-License-Identifier: Apache-2.0
// Derived from RayoMD src/common/batch_report.cpp at ef106ca (Apache-2.0, same author).
#include "common/batch.h"

#include "common/utf8.h"

#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

namespace RayoPdf::Batch {

namespace {

void AppendNumber(std::string& out, const char* key, uint64_t value) {
    out += ",\"";
    out += key;
    out += "\":";
    char buf[20];
    out.append(buf, std::to_chars(buf, buf + sizeof buf, value).ptr);
}

} // namespace

void AppendJsonString(std::string& out, std::string_view text) {
    static const char kHex[] = "0123456789abcdef";
    out.push_back('"');
    size_t at = 0;
    while (at < text.size()) {
        const unsigned char byte = static_cast<unsigned char>(text[at]);
        if (byte >= 0x80) {
            uint32_t codePoint = 0;
            size_t length = 0;
            Utf8::Decode(text, at, codePoint, length);
            // Decode reads a malformed byte as U+FFFD of length 1; U+FFFD itself is 3 bytes long.
            if (length == 1) {
                out += "\\ufffd";
            } else {
                out.append(text.data() + at, length);
            }
            at += length;
            continue;
        }
        switch (byte) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (byte < 0x20 || byte == 0x7F) {
                    out += "\\u00";
                    out.push_back(kHex[byte >> 4]);
                    out.push_back(kHex[byte & 15]);
                } else {
                    out.push_back(static_cast<char>(byte));
                }
        }
        ++at;
    }
    out.push_back('"');
}

void AppendRecord(std::string& out, const Record& record) {
    out.reserve(out.size() + 256 + record.id.size() + record.input.size() + record.output.size() +
                record.error.size() + record.counters.size() + record.markdown.size() + record.json.size() +
                record.chunks.size() + record.html.size());
    out.push_back('{');
    if (!record.id.empty()) {
        out += "\"id\":";
        out += record.id;
        out.push_back(',');
    }
    out += "\"input\":";
    AppendJsonString(out, record.input);
    out += ",\"output\":";
    AppendJsonString(out, record.output);
    out += ",\"status\":\"";
    out += record.status;
    out.push_back('"');
    AppendNumber(out, "code", static_cast<uint64_t>(record.code < 0 ? 0 : record.code));
    if (!record.error.empty()) {
        out += ",\"error\":";
        AppendJsonString(out, record.error);
    }
    AppendNumber(out, "pages", record.pages);
    AppendNumber(out, "bytes", record.bytes);
    char ms[48];
    const double milliseconds = record.milliseconds >= 0 && record.milliseconds < 1e15 ? record.milliseconds : 0;
    std::snprintf(ms, sizeof ms, ",\"ms\":%.3f", milliseconds);
    out += ms;
    out += record.counters;
    if (record.hasJson) {
        out += ",\"json\":";
        out += record.json;
    } else if (record.hasChunks) {
        out += ",\"chunks\":";
        out += record.chunks;
    } else if (record.hasHtml) {
        out += ",\"html\":";
        AppendJsonString(out, record.html);
    } else if (record.hasMarkdown) {
        out += ",\"markdown\":";
        AppendJsonString(out, record.markdown);
    }
    out += "}\n";
}

unsigned UsableCpus() {
#ifdef _WIN32
    DWORD_PTR process = 0;
    DWORD_PTR system = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process != 0) {
        unsigned count = 0;
        for (; process != 0; process &= process - 1) ++count;
        return count;
    }
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0 && CPU_COUNT(&set) > 0) return static_cast<unsigned>(CPU_COUNT(&set));
#endif
    const unsigned cpus = std::thread::hardware_concurrency();
    return cpus == 0 ? 1 : cpus;
}

unsigned WorkerCount(unsigned workers, size_t jobs) {
    if (workers == 0) workers = UsableCpus() < kDefaultMaxWorkers ? UsableCpus() : kDefaultMaxWorkers;
    if (workers > kMaxWorkers) workers = kMaxWorkers;
    if (jobs < workers) workers = static_cast<unsigned>(jobs);
    return workers == 0 ? 1 : workers;
}

void RunJobs(size_t count, unsigned workers, void (*run)(void*, unsigned, size_t), void* context) {
    if (count == 0) return;
    workers = WorkerCount(workers, count);
    if (workers == 1) {
        for (size_t index = 0; index < count; ++index) run(context, 0, index);
        return;
    }
    std::atomic<size_t> next{0};
    const auto work = [&](unsigned worker) {
        for (size_t index; (index = next.fetch_add(1, std::memory_order_relaxed)) < count;) run(context, worker, index);
    };
    // The calling thread is worker 0: one thread fewer to start, and its heap is the warm one.
    std::vector<std::thread> threads;
    threads.reserve(workers - 1);
    for (unsigned id = 1; id < workers; ++id) threads.emplace_back(work, id);
    work(0);
    for (std::thread& thread : threads) thread.join();
}

OrderedOutput::~OrderedOutput() {
    Close();
}

bool OrderedOutput::Open(const std::string& path, bool flushEachLine) {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = stream_.Open(path);
    flushEachLine_ = flushEachLine;
    failed_ = false;
    next_ = 0;
    waiting_.clear();
    buffer_.clear();
    lastWrite_ = std::chrono::steady_clock::now();
    return open_;
}

void OrderedOutput::Put(size_t index, std::string line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_ || index < next_) return;
    if (index != next_) {
        waiting_.emplace(index, std::move(line));
        return;
    }
    buffer_ += line;
    ++next_;
    for (auto it = waiting_.begin(); it != waiting_.end() && it->first == next_; it = waiting_.erase(it)) {
        buffer_ += it->second;
        ++next_;
    }
    if (flushEachLine_ || buffer_.size() >= 64 * 1024) {
        FlushLocked();
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - lastWrite_ >= std::chrono::seconds(1)) FlushLocked();
}

void OrderedOutput::FlushLocked() {
    if (!buffer_.empty() && !failed_ && !stream_.Write(buffer_)) failed_ = true;
    buffer_.clear();
    lastWrite_ = std::chrono::steady_clock::now();
}

bool OrderedOutput::Healthy() {
    std::lock_guard<std::mutex> lock(mutex_);
    return !failed_;
}

bool OrderedOutput::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) return false;
    FlushLocked();
    if (!waiting_.empty()) failed_ = true;
    waiting_.clear();
    if (!stream_.Close()) failed_ = true;
    open_ = false;
    return !failed_;
}

} // namespace RayoPdf::Batch
