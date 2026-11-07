// SPDX-License-Identifier: Apache-2.0
// Core tests of src/common: the --serve --json request reader, the line reader, the batch record
// and the worker threads (codes 950-999).
#include "check.h"
#include "common/batch.h"
#include "common/file.h"
#include "common/serve.h"

#include <atomic>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

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

} // namespace

int main() {
    TestRequests();
    TestLineReader();
    TestRecords();
    TestWorkers();
    TestPaths();
    TestFiles();
    std::puts("test_common: all checks passed");
    return 0;
}
