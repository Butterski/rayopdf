// SPDX-License-Identifier: Apache-2.0
// Derived from RayoMD src/common/batch_report.h at ef106ca (Apache-2.0, same author).
// What the batch modes share (--batch, --serve --json, and --convert --report): the JSON record of
// one document, the worker threads, and output written in input order whatever thread finishes
// first, so that records and files never depend on the number of workers.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>

#include "common/file.h"

namespace RayoPdf::Batch {

// Workers: --workers=N takes 1 to kMaxWorkers; without it a batch uses one per CPU the process may
// run on, at most kDefaultMaxWorkers.
constexpr unsigned kMaxWorkers = 64;
constexpr unsigned kDefaultMaxWorkers = 6;

// One document: a line of the report, and the answer to a --serve --json request. The shape of
// RayoMD's protocol-1 record, with RayoPDF's counters in `counters`.
struct Record {
    std::string id;              // JSON text (a string with its quotes, or an integer); empty for none
    std::string input;           // UTF-8, as given
    std::string output;          // UTF-8, as given ("-" when the Markdown is in `markdown`)
    const char* status = "ok";   // "ok", "skipped" or "error"
    int code = 0;                // the exit code this document alone would give
    std::string error;           // why it failed, for "error"
    uint64_t pages = 0;          // pages converted
    uint64_t bytes = 0;          // Markdown bytes written
    double milliseconds = 0.0;   // reading, converting and writing
    std::string counters;        // further members, JSON text without braces: ,"name":value...
    bool hasMarkdown = false;    // --serve --json without an output: the Markdown itself
    std::string markdown;
    bool hasJson = false;        // the same with --format=json: the JSON object, written as it is
    std::string json;            // one JSON object, without its line break
    bool hasChunks = false;      // the same with --format=chunks: the chunks as a JSON array, as it is
    std::string chunks;
};

// Appends `text` as a JSON string, its quotes included: quotes, backslashes and control characters
// escaped, and bytes that are not UTF-8 (possible in Linux file names) as U+FFFD.
void AppendJsonString(std::string& out, std::string_view text);

// Appends `record` to `out` as one line, its line break included:
// {"id":..,"input":..,"output":..,"status":..,"code":..,"error":..,"pages":..,"bytes":..,"ms":..,
//  <counters>,"markdown":..}; "id", "error" and "markdown" only when there are, and "json" (the
//  object as it is) or "chunks" (the array as it is) instead of "markdown" when there is one.
void AppendRecord(std::string& out, const Record& record);

// The CPUs this process may run on: its affinity mask, which taskset or a container narrows, where
// std::thread::hardware_concurrency counts the machine's; at least 1.
unsigned UsableCpus();

// `workers` as asked (0: automatic) and bounded by the jobs there are; at least 1.
unsigned WorkerCount(unsigned workers, size_t jobs);

// `run(context, worker, index)` for every index below `count` on `workers` threads (see
// WorkerCount), each index once, taken in increasing order; `worker` (below the worker count) names
// the thread, for buffers of its own. The calling thread is worker 0; workers - 1 more are started.
void RunJobs(size_t count, unsigned workers, void (*run)(void*, unsigned, size_t), void* context);

template <class Run>
void RunJobs(size_t count, unsigned workers, Run&& run) {
    using Function = std::remove_reference_t<Run>;
    RunJobs(count, workers,
            [](void* self, unsigned worker, size_t index) { (*static_cast<Function*>(self))(worker, index); },
            const_cast<void*>(static_cast<const void*>(&run)));
}

// Lines handed in out of order (by index, each index once, from 0 up without gaps), written in index
// order to a file or standard output: the line the output waits for is written by the thread that
// hands it in, with every later one already there. A report flushes what it holds at least once a
// second and every 64 KiB; a session (`flushEachLine`) writes each line at once.
class OrderedOutput {
public:
    OrderedOutput() = default;
    OrderedOutput(const OrderedOutput&) = delete;
    OrderedOutput& operator=(const OrderedOutput&) = delete;
    ~OrderedOutput();

    // `path` in UTF-8, or "-" for standard output. False when the file cannot be created.
    bool Open(const std::string& path, bool flushEachLine);
    bool IsOpen() const { return open_; }
    void Put(size_t index, std::string line);
    // Whether every write so far succeeded.
    bool Healthy();
    // Writes what is held and closes; false when a write failed, a line is still missing, or there
    // was nothing open.
    bool Close();

private:
    void FlushLocked();

    std::mutex mutex_;
    File::OutputStream stream_;
    bool open_ = false;
    bool flushEachLine_ = false;
    bool failed_ = false;
    size_t next_ = 0;
    std::map<size_t, std::string> waiting_;
    std::string buffer_;
    std::chrono::steady_clock::time_point lastWrite_;
};

} // namespace RayoPdf::Batch
