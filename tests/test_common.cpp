// SPDX-License-Identifier: Apache-2.0
// Core tests of src/common: the --serve --json request reader, the line reader, the batch record
// and the worker threads (codes 950-999), the batch modes' file paths (codes 1680-1699) and the
// run-time choice of kernels (codes 1650-1679).
#include "check.h"
#include "common/batch.h"
#include "common/cpu.h"
#include "common/file.h"
#include "common/serve.h"

#include <atomic>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {

using namespace RayoPdf;

bool Parses(std::string_view line, Serve::Request& request) {
    std::string error;
    return Serve::ParseRequest(line, request, error);
}

bool Rejects(std::string_view line) {
    Serve::Request request;
    std::string error;
    return !Serve::ParseRequest(line, request, error) && !error.empty();
}

// common/cpu.h: forcing the baseline turns every extension off, so each dispatched kernel's
// comparison with its baseline path compares two paths; lifting it gives back the features found,
// the same on every call.
void TestCpuFeatures() {
    const uint32_t found = CpuFeatures();
    RAYOPDF_CHECK((found & ~uint32_t(kCpuSse42 | kCpuAvx2 | kCpuBmi2 | kCpuPclmul)) == 0, 1650);
    ForceCpuBaseline(true);
    RAYOPDF_CHECK(CpuFeatures() == 0, 1651);
    ForceCpuBaseline(false);
    RAYOPDF_CHECK(CpuFeatures() == found, 1652);
}

void TestRequests() {
    Serve::Request request;
    RAYOPDF_CHECK(Parses(R"({"id":"7","input":"in/a.pdf","output":"out/a.md","args":["--pages=2","--no-annotations"]})",
                         request),
                  950);
    RAYOPDF_CHECK(request.id == "\"7\"" && request.input == "in/a.pdf" && request.hasOutput &&
                      request.output == "out/a.md" && request.args.size() == 2 && request.args[1] == "--no-annotations",
                  951);
    RAYOPDF_CHECK(Parses(" { \"input\" : \"a.pdf\" , \"id\" : -42 } ", request), 952);
    RAYOPDF_CHECK(request.id == "-42" && !request.hasOutput && request.output.empty() && request.args.empty(), 953);
    RAYOPDF_CHECK(Parses(R"({"input":"d\u00e9j\u00e0\n\ud83d\ude00\/x.pdf","args":[]})", request), 954);
    RAYOPDF_CHECK(request.input == "d\xC3\xA9j\xC3\xA0\n\xF0\x9F\x98\x80/x.pdf", 955);
    RAYOPDF_CHECK(Parses("{\"input\":\"\xE2\x82\xAC.pdf\"}", request) && request.input == "\xE2\x82\xAC.pdf", 956);

    RAYOPDF_CHECK(Rejects(""), 957);
    RAYOPDF_CHECK(Rejects("[\"input\"]"), 958);
    RAYOPDF_CHECK(Rejects(R"({"input":"a.pdf","markdown":"x"})"), 959);
    RAYOPDF_CHECK(Rejects(R"({"input":"a.pdf","input":"b.pdf"})"), 960);
    RAYOPDF_CHECK(Rejects(R"({"output":"a.md"})"), 961);
    RAYOPDF_CHECK(Rejects(R"({"input":""})"), 962);
    RAYOPDF_CHECK(Rejects(R"({"input":"a.pdf","output":""})"), 963);
    RAYOPDF_CHECK(Rejects(R"({"id":1.5,"input":"a.pdf"})"), 964);
    RAYOPDF_CHECK(Rejects(R"({"id":null,"input":"a.pdf"})"), 965);
    RAYOPDF_CHECK(Rejects(R"({"input":"a.pdf","args":["--pages=1",2]})"), 966);
    RAYOPDF_CHECK(Rejects(R"({"input":"a.pdf"} x)"), 967);
    RAYOPDF_CHECK(Rejects("{\"input\":\"a\xFF.pdf\"}"), 968);
    RAYOPDF_CHECK(Rejects(R"({"input":"\ud800.pdf"})"), 969);
    RAYOPDF_CHECK(Rejects("{\"input\":\"a\tb.pdf\"}"), 970);
    RAYOPDF_CHECK(Rejects(R"({"input":"a\u0000.pdf"})"), 971);
    RAYOPDF_CHECK(Rejects(R"({"input":"a.pdf")"), 972);
    RAYOPDF_CHECK(Rejects(R"({"id":0123,"input":"a.pdf"})"), 973);
    RAYOPDF_CHECK(Rejects(R"({"id":12345678901234567890,"input":"a.pdf"})"), 974);
    std::string many = R"({"input":"a.pdf","args":[)";
    for (size_t i = 0; i <= Serve::kMaxArgs; ++i) many += i ? ",\"--no-annotations\"" : "\"--no-annotations\"";
    RAYOPDF_CHECK(Rejects(many + "]}"), 975);

    // The id read before a bad field stays, so the error answer carries it.
    std::string error;
    RAYOPDF_CHECK(!Serve::ParseRequest(R"({"id":"x9","input":3})", request, error) && request.id == "\"x9\"", 976);
}

struct Chunks {
    std::vector<std::string> pieces;
    size_t next = 0;
};

size_t ReadChunk(void* context, char* buffer, size_t size) {
    Chunks& chunks = *static_cast<Chunks*>(context);
    if (chunks.next == chunks.pieces.size()) return 0;
    const std::string& piece = chunks.pieces[chunks.next++];
    const size_t take = piece.size() < size ? piece.size() : size;
    std::memcpy(buffer, piece.data(), take);
    return take;
}

void TestLineReader() {
    Chunks chunks{{"first\r", "\nsec", "ond\n", std::string(40, 'x') + "\nok\n", "last"}, 0};
    Serve::LineReader reader(ReadChunk, &chunks);
    std::string line;
    RAYOPDF_CHECK(reader.Next(line, 32) == Serve::LineReader::Status::Line && line == "first", 977);
    RAYOPDF_CHECK(reader.Next(line, 32) == Serve::LineReader::Status::Line && line == "second", 978);
    RAYOPDF_CHECK(reader.Next(line, 32) == Serve::LineReader::Status::TooLong && line.empty(), 979);
    RAYOPDF_CHECK(reader.Next(line, 32) == Serve::LineReader::Status::Line && line == "ok", 980);
    RAYOPDF_CHECK(reader.Next(line, 32) == Serve::LineReader::Status::Line && line == "last", 981);
    RAYOPDF_CHECK(reader.Next(line, 32) == Serve::LineReader::Status::End, 982);
}

void TestRecords() {
    std::string out;
    Batch::AppendJsonString(out, "a\"b\\c\n\x01\x7F\xC3\xA9\xFF");
    RAYOPDF_CHECK(out == "\"a\\\"b\\\\c\\n\\u0001\\u007f\xC3\xA9\\ufffd\"", 983);

    Batch::Record record;
    record.input = "in/a.pdf";
    record.output = "out/a.md";
    record.pages = 3;
    record.bytes = 120;
    record.milliseconds = 1.25;
    record.counters = ",\"glyphs\":9";
    out.clear();
    Batch::AppendRecord(out, record);
    RAYOPDF_CHECK(out == "{\"input\":\"in/a.pdf\",\"output\":\"out/a.md\",\"status\":\"ok\",\"code\":0,\"pages\":3,"
                         "\"bytes\":120,\"ms\":1.250,\"glyphs\":9}\n",
                  984);

    Batch::Record failed;
    failed.id = "17";
    failed.input = "x.pdf";
    failed.output = "-";
    failed.status = "error";
    failed.code = 11;
    failed.error = "x.pdf needs a password";
    out.clear();
    Batch::AppendRecord(out, failed);
    RAYOPDF_CHECK(out == "{\"id\":17,\"input\":\"x.pdf\",\"output\":\"-\",\"status\":\"error\",\"code\":11,"
                         "\"error\":\"x.pdf needs a password\",\"pages\":0,\"bytes\":0,\"ms\":0.000}\n",
                  985);

    Batch::Record answer;
    answer.input = "a.pdf";
    answer.output = "-";
    answer.hasMarkdown = true;
    answer.markdown = "# T\n";
    out.clear();
    Batch::AppendRecord(out, answer);
    RAYOPDF_CHECK(out.size() > 20 && out.compare(out.size() - 21, 21, ",\"markdown\":\"# T\\n\"}\n") == 0, 986);
}

void TestWorkers() {
    RAYOPDF_CHECK(Batch::WorkerCount(4, 2) == 2 && Batch::WorkerCount(4, 100) == 4 && Batch::WorkerCount(0, 0) == 1 &&
                      Batch::WorkerCount(500, 1000) == Batch::kMaxWorkers,
                  987);
    const unsigned automatic = Batch::WorkerCount(0, 1000);
    RAYOPDF_CHECK(automatic >= 1 && automatic <= Batch::kDefaultMaxWorkers, 988);

    constexpr size_t kJobs = 1000;
    for (unsigned workers : {1u, 4u}) {
        std::vector<std::atomic<int>> seen(kJobs);
        std::atomic<bool> badWorker{false};
        Batch::RunJobs(kJobs, workers, [&](unsigned worker, size_t index) {
            if (worker >= workers) badWorker = true;
            seen[index].fetch_add(1);
        });
        bool once = true;
        for (const std::atomic<int>& count : seen) once = once && count.load() == 1;
        RAYOPDF_CHECK(once, 989);
        RAYOPDF_CHECK(!badWorker, 990);
    }
    // The calling thread is worker 0, and the only one it is.
    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<bool> callerIsZero{true};
    Batch::RunJobs(kJobs, 4, [&](unsigned worker, size_t) {
        if ((std::this_thread::get_id() == caller) != (worker == 0)) callerIsZero = false;
    });
    RAYOPDF_CHECK(callerIsZero, 1680);
}

void TestPaths() {
    const std::string here = File::AbsolutePath(".");
    const std::string path = File::AbsolutePath("a/./b/../c");
    RAYOPDF_CHECK(!here.empty() && (here[0] == '/' || (here.size() >= 3 && here[1] == ':' && here[2] == '/')), 991);
    const std::string expected = here.back() == '/' ? here + "a/c" : here + "/a/c";
    RAYOPDF_CHECK(path == expected, 992);
    RAYOPDF_CHECK(path.find('\\') == std::string::npos, 993);
}

// The file layer end to end in a temporary directory: write, list, examine, map, remove.
void TestFiles() {
    std::string folder;
    RAYOPDF_CHECK(File::MakeTemporaryDirectory("rayopdf-test-common-", folder), 994);
    const std::string name = folder + "/d\xC3\xA9j\xC3\xA0.md";
    RAYOPDF_CHECK(File::WriteAll(name, "first") && File::WriteAll(name, "second text"), 995);
    RAYOPDF_CHECK(File::EnsureDirectory(folder + "/sub") && File::EnsureDirectory(folder + "/sub"), 996);
    std::vector<File::DirectoryEntry> entries;
    RAYOPDF_CHECK(File::ListDirectory(folder, entries) && entries.size() == 2, 997);
    bool file = false, directory = false;
    for (const File::DirectoryEntry& entry : entries) {
        file = file || (entry.name == "d\xC3\xA9j\xC3\xA0.md" && entry.type == File::FileType::Regular);
        directory = directory || (entry.name == "sub" && entry.type == File::FileType::Directory);
    }
    File::FileInfo info;
    File::InputFile input;
    RAYOPDF_CHECK(file && directory && File::StatusOf(name, info) && info.type == File::FileType::Regular &&
                      info.size == 11 && input.Open(name, 1024) == File::ReadStatus::Ok &&
                      input.Bytes() == "second text" && input.Open(folder, 1024) == File::ReadStatus::NotRegular &&
                      input.Open(folder + "/missing", 1024) == File::ReadStatus::NotFound,
                  998);
    input.Open(folder + "/missing", 1024);  // unmaps the file, so Windows lets it go
    RAYOPDF_CHECK(File::RemoveFile(name) && File::RemoveEmptyDirectory(folder + "/sub") &&
                      File::RemoveEmptyDirectory(folder) && !File::StatusOf(folder, info),
                  999);
}

// A batch worker's input buffer: small files read into it, from its start, the buffer kept from
// file to file; larger files mapped as before. Temporary files take the mode a new file gets.
void TestBufferedInput() {
    std::string folder;
    RAYOPDF_CHECK(File::MakeTemporaryDirectory("rayopdf-test-common-", folder), 1681);
    const std::string small = folder + "/small.pdf";
    const std::string smaller = folder + "/smaller.pdf";
    const std::string large = folder + "/large.pdf";
    const std::string big(static_cast<size_t>(File::kBufferedReadBytes) + 1, 'x');
    RAYOPDF_CHECK(File::WriteAll(small, "%PDF-1.7 small") && File::WriteAll(smaller, "%PDF-1") &&
                      File::WriteAll(large, big),
                  1682);
    std::string buffer;
    File::InputFile input;
    RAYOPDF_CHECK(input.Open(small, 1024, &buffer) == File::ReadStatus::Ok && input.Bytes() == "%PDF-1.7 small" &&
                      input.Bytes().data() == buffer.data(),
                  1683);
    input.Close();
    RAYOPDF_CHECK(input.Open(smaller, 1024, &buffer) == File::ReadStatus::Ok && input.Bytes() == "%PDF-1" &&
                      input.Bytes().data() == buffer.data() && buffer.size() == 14,
                  1684);
    input.Close();
    RAYOPDF_CHECK(input.Open(large, uint64_t(1) << 30, &buffer) == File::ReadStatus::Ok && input.Bytes() == big &&
                      buffer.size() == 14,
                  1685);
    input.Close();
    RAYOPDF_CHECK(input.Open(folder, 1024, &buffer) == File::ReadStatus::NotRegular &&
                      input.Open(folder + "/missing", 1024, &buffer) == File::ReadStatus::NotFound &&
                      input.Open(small, 4, &buffer) == File::ReadStatus::TooLarge,
                  1686);
#ifndef _WIN32
    const mode_t mask = umask(0);
    umask(mask);
    File::StagedFile staged;
    RAYOPDF_CHECK(staged.Open(folder + "/staged.png") && staged.Write("png") && staged.Commit(), 1687);
    struct stat status;
    RAYOPDF_CHECK(stat(small.c_str(), &status) == 0 && (status.st_mode & 0777) == (0666 & ~mask), 1688);
    RAYOPDF_CHECK(stat((folder + "/staged.png").c_str(), &status) == 0 && (status.st_mode & 0777) == (0666 & ~mask),
                  1689);
    RAYOPDF_CHECK(File::RemoveFile(folder + "/staged.png"), 1690);
#endif
    std::vector<File::DirectoryEntry> entries;
    RAYOPDF_CHECK(File::ListDirectory(folder, entries) && entries.size() == 3, 1691);  // no temporary file left
    RAYOPDF_CHECK(File::RemoveFile(small) && File::RemoveFile(smaller) && File::RemoveFile(large) &&
                      File::RemoveEmptyDirectory(folder),
                  1692);
}

// A staged file named only after it was written (an image made on a page thread, named in page
// order): Close keeps it under its temporary name, CommitAs gives it its name, Discard removes it.
void TestStagedNames() {
    std::string folder;
    RAYOPDF_CHECK(File::MakeTemporaryDirectory("rayopdf-test-staged-", folder), 1693);
    File::StagedFile closed;
    File::StagedFile open;
    File::StagedFile dropped;
    File::StagedFile droppedOpen;
    RAYOPDF_CHECK(closed.Open(folder + "/image.tmp-0") && closed.Write("closed ") && closed.WriteAt(0, "C") &&
                      closed.Close() && open.Open(folder + "/image.tmp-1") && open.Write("open") &&
                      dropped.Open(folder + "/image.tmp-2") && dropped.Write("dropped") && dropped.Close() &&
                      droppedOpen.Open(folder + "/image.tmp-3") && droppedOpen.Write("dropped"),
                  1694);
    std::vector<File::DirectoryEntry> staged;
    RAYOPDF_CHECK(File::ListDirectory(folder, staged) && staged.size() == 4, 1695);  // four temporary files
    dropped.Discard();
    droppedOpen.Discard();
    RAYOPDF_CHECK(closed.CommitAs(folder + "/b.png") && open.CommitAs(folder + "/a.png"), 1696);
    File::InputFile input;
    RAYOPDF_CHECK(input.Open(folder + "/b.png", 1024) == File::ReadStatus::Ok && input.Bytes() == "Closed ", 1697);
    RAYOPDF_CHECK(input.Open(folder + "/a.png", 1024) == File::ReadStatus::Ok && input.Bytes() == "open", 1698);
    input.Open(folder + "/missing", 1024);  // unmaps the file, so Windows lets it go
    std::vector<File::DirectoryEntry> named;
    RAYOPDF_CHECK(File::ListDirectory(folder, named) && named.size() == 2 && File::RemoveFile(folder + "/a.png") &&
                      File::RemoveFile(folder + "/b.png") && File::RemoveEmptyDirectory(folder),
                  1699);
}

} // namespace

int main() {
    TestCpuFeatures();
    TestRequests();
    TestLineReader();
    TestRecords();
    TestWorkers();
    TestPaths();
    TestBufferedInput();
    TestFiles();
    TestStagedNames();
    std::puts("test_common: all checks passed");
    return 0;
}
