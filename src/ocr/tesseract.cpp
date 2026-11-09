// SPDX-License-Identifier: Apache-2.0
#include "ocr/tesseract.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace RayoPdf::Ocr {
namespace {

constexpr size_t kTsvColumns = 12;

bool NextLine(std::string_view text, size_t& at, std::string_view& line) {
    if (at >= text.size()) return false;
    size_t end = text.find('\n', at);
    if (end == std::string_view::npos) end = text.size();
    line = text.substr(at, end - at);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    at = end + 1;
    return true;
}

bool ParseInt(std::string_view text, int64_t& value) {
    bool negative = false;
    size_t i = 0;
    if (!text.empty() && text[0] == '-') {
        negative = true;
        i = 1;
    }
    if (i == text.size() || text.size() - i > 9) return false;
    value = 0;
    for (; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + (text[i] - '0');
    }
    if (negative) value = -value;
    return true;
}

// A confidence such as "96.581345" or "-1", as a value 0..100 (negative counts as 0).
double ParseConfidence(std::string_view text) {
    double value = 0, scale = 0;
    for (char c : text) {
        if (c == '-') return 0;
        if (c == '.') {
            scale = 1;
            continue;
        }
        if (c < '0' || c > '9') break;
        if (scale > 0) {
            scale /= 10;
            value += (c - '0') * scale;
        } else {
            value = value * 10 + (c - '0');
        }
        if (value > 100) return 100;
    }
    return value;
}

bool Blank(std::string_view text) {
    for (char c : text) {
        if (c != ' ' && c != '\t') return false;
    }
    return true;
}

struct Word {
    int64_t key[4];
    int32_t x0, y0, x1, y1;
    double conf;
    std::string_view text;
};

void FinishLine(const std::vector<Word>& words, size_t begin, size_t end, std::vector<OcrProtocol::Line>& lines) {
    if (begin == end) return;
    OcrProtocol::Line line;
    line.x0 = words[begin].x0;
    line.y0 = words[begin].y0;
    line.x1 = words[begin].x1;
    line.y1 = words[begin].y1;
    double conf = 0;
    for (size_t i = begin; i < end; ++i) {
        const Word& w = words[i];
        if (w.x0 < line.x0) line.x0 = w.x0;
        if (w.y0 < line.y0) line.y0 = w.y0;
        if (w.x1 > line.x1) line.x1 = w.x1;
        if (w.y1 > line.y1) line.y1 = w.y1;
        conf += w.conf;
        if (i > begin) {
            const int32_t from = words[i - 1].x1;
            line.text.push_back(' ');
            line.chars.push_back({from, w.x0 > from ? w.x0 : from});
        }
        const size_t points = OcrProtocol::CodePoints(w.text);
        line.text.append(w.text);
        const int64_t span = int64_t(w.x1) - w.x0;
        for (size_t k = 0; k < points; ++k) {
            const int32_t a = int32_t(w.x0 + span * int64_t(k) / int64_t(points));
            const int32_t b = int32_t(w.x0 + span * int64_t(k + 1) / int64_t(points));
            line.chars.push_back({a, b});
        }
    }
    line.conf = ToConf(conf / double(end - begin) / 100.0);
    lines.push_back(std::move(line));
}

} // namespace

bool ParseTesseractTsv(std::string_view tsv, std::vector<OcrProtocol::Line>& lines) {
    lines.clear();
    size_t at = 0;
    std::string_view row;
    if (!NextLine(tsv, at, row) || row.substr(0, 6) != "level\t") return false;
    std::vector<Word> words;
    while (NextLine(tsv, at, row)) {
        std::string_view fields[kTsvColumns];
        size_t count = 0, start = 0;
        while (count < kTsvColumns) {
            size_t tab = count + 1 < kTsvColumns ? row.find('\t', start) : std::string_view::npos;
            if (tab == std::string_view::npos) tab = row.size();
            fields[count++] = row.substr(start, tab - start);
            if (tab >= row.size()) break;
            start = tab + 1;
        }
        int64_t level = 0;
        if (count < kTsvColumns || !ParseInt(fields[0], level) || level != 5) continue;
        Word word;
        int64_t left, top, width, height;
        bool ok = ParseInt(fields[6], left) && ParseInt(fields[7], top) && ParseInt(fields[8], width) &&
                  ParseInt(fields[9], height);
        for (int k = 0; k < 4 && ok; ++k) ok = ParseInt(fields[1 + k], word.key[k]);
        if (!ok || left < 0 || top < 0 || width < 0 || height < 0 || Blank(fields[11])) continue;
        if (OcrProtocol::CodePoints(fields[11]) == 0) continue;
        word.x0 = int32_t(left);
        word.y0 = int32_t(top);
        word.x1 = int32_t(left + width);
        word.y1 = int32_t(top + height);
        word.conf = ParseConfidence(fields[10]);
        word.text = fields[11];
        words.push_back(word);
    }
    size_t begin = 0;
    for (size_t i = 1; i <= words.size(); ++i) {
        const bool same = i < words.size() && words[i].key[0] == words[begin].key[0] &&
                          words[i].key[1] == words[begin].key[1] && words[i].key[2] == words[begin].key[2] &&
                          words[i].key[3] == words[begin].key[3];
        if (same) continue;
        FinishLine(words, begin, i, lines);
        if (lines.size() >= kMaxPageLines) break;
        begin = i;
    }
    return true;
}

#ifdef _WIN32

namespace {

std::wstring Wide(std::string_view text) {
    if (text.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), &out[0], n);
    return out;
}

// One argument quoted for CommandLineToArgvW.
void AppendQuoted(std::wstring& line, const std::wstring& arg) {
    line.push_back(L'"');
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') backslashes = backslashes * 2 + 1;
        line.append(backslashes, L'\\');
        backslashes = 0;
        line.push_back(c);
    }
    line.append(backslashes * 2, L'\\');
    line.push_back(L'"');
}

} // namespace

ProcessStatus RunProcess(const std::vector<std::string>& argv, std::string_view input, std::string& output) {
    output.clear();
    if (argv.empty()) return ProcessStatus::NotStarted;
    std::wstring line;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) line.push_back(L' ');
        AppendQuoted(line, Wide(argv[i]));
    }
    SetEnvironmentVariableW(L"OMP_THREAD_LIMIT", L"1");
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, &inherit, 0)) return ProcessStatus::NotStarted;
    if (!CreatePipe(&outRead, &outWrite, &inherit, 0)) {
        CloseHandle(inRead);
        CloseHandle(inWrite);
        return ProcessStatus::NotStarted;
    }
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    HANDLE null = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0,
                              nullptr);
    STARTUPINFOW startup;
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = inRead;
    startup.hStdOutput = outWrite;
    startup.hStdError = null;
    PROCESS_INFORMATION process;
    ZeroMemory(&process, sizeof(process));
    const BOOL started = CreateProcessW(nullptr, &line[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                        &startup, &process);
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (null != INVALID_HANDLE_VALUE) CloseHandle(null);
    if (!started) {
        CloseHandle(inWrite);
        CloseHandle(outRead);
        return ProcessStatus::NotStarted;
    }
    size_t done = 0;
    while (done < input.size()) {
        DWORD wrote = 0;
        const DWORD chunk = DWORD(input.size() - done > (1u << 20) ? (1u << 20) : input.size() - done);
        if (!WriteFile(inWrite, input.data() + done, chunk, &wrote, nullptr) || wrote == 0) break;
        done += wrote;
    }
    CloseHandle(inWrite);
    bool tooLarge = false;
    char buffer[65536];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(outRead, buffer, sizeof(buffer), &got, nullptr) || got == 0) break;
        if (output.size() + got > kMaxTesseractOutput) {
            tooLarge = true;
            TerminateProcess(process.hProcess, 1);
            break;
        }
        output.append(buffer, got);
    }
    CloseHandle(outRead);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    if (tooLarge) return ProcessStatus::TooLarge;
    return code == 0 ? ProcessStatus::Ok : ProcessStatus::Failed;
}

#else

ProcessStatus RunProcess(const std::vector<std::string>& argv, std::string_view input, std::string& output) {
    output.clear();
    if (argv.empty()) return ProcessStatus::NotStarted;
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    std::vector<char*> env;
    static char kThreads[] = "OMP_THREAD_LIMIT=1";
    for (char** e = environ; e && *e; ++e) {
        if (std::strncmp(*e, "OMP_THREAD_LIMIT=", 17) != 0) env.push_back(*e);
    }
    env.push_back(kThreads);
    env.push_back(nullptr);

    int in[2], out[2];
    if (pipe2(in, O_CLOEXEC) != 0) return ProcessStatus::NotStarted;
    if (pipe2(out, O_CLOEXEC) != 0) {
        close(in[0]);
        close(in[1]);
        return ProcessStatus::NotStarted;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[0], 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    const int spawned = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), env.data());
    posix_spawn_file_actions_destroy(&actions);
    close(in[0]);
    close(out[1]);
    if (spawned != 0) {
        close(in[1]);
        close(out[0]);
        return ProcessStatus::NotStarted;
    }
    size_t done = 0;
    while (done < input.size()) {
        const ssize_t n = write(in[1], input.data() + done, input.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;  // the child is gone (EPIPE: SIGPIPE is ignored by rayopdf-ocr)
        done += size_t(n);
    }
    close(in[1]);
    bool tooLarge = false;
    char buffer[65536];
    for (;;) {
        const ssize_t n = read(out[0], buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (output.size() + size_t(n) > kMaxTesseractOutput) {
            tooLarge = true;
            kill(pid, SIGKILL);
            break;
        }
        output.append(buffer, size_t(n));
    }
    close(out[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (tooLarge) return ProcessStatus::TooLarge;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? ProcessStatus::Ok : ProcessStatus::Failed;
}

#endif

bool TesseractEngine::Probe(std::string& version) {
    if (RunProcess({program_, "--version"}, {}, output_) != ProcessStatus::Ok) return false;
    size_t end = output_.find('\n');
    version = output_.substr(0, end == std::string::npos ? output_.size() : end);
    if (!version.empty() && version.back() == '\r') version.pop_back();
    return !version.empty();
}

EngineStatus TesseractEngine::Run(const GrayView& image, uint32_t dpi, const char* psm,
                                  std::vector<OcrProtocol::Line>& lines) {
    lines.clear();
    if (image.width == 0 || image.height == 0) return EngineStatus::Ok;
    input_ = "P5\n" + std::to_string(image.width) + " " + std::to_string(image.height) + "\n255\n";
    input_.append(reinterpret_cast<const char*>(image.pixels), size_t(image.width) * image.height);
    std::vector<std::string> argv = {program_, "stdin", "stdout", "-l", language_, "--psm", psm};
    if (dpi > 0) {
        argv.push_back("--dpi");
        argv.push_back(std::to_string(dpi));
    }
    argv.push_back("tsv");
    if (RunProcess(argv, input_, output_) != ProcessStatus::Ok) return EngineStatus::Failed;
    if (!ParseTesseractTsv(output_, lines)) return EngineStatus::Failed;
    return EngineStatus::Ok;
}

EngineStatus TesseractEngine::Page(const GrayView& page, uint32_t dpi, std::vector<OcrProtocol::Line>& lines) {
    return Run(page, dpi, "3", lines);
}

EngineStatus TesseractEngine::Line(const GrayView& image, OcrProtocol::Line& line) {
    line = OcrProtocol::Line();
    line.x1 = int32_t(image.width);
    line.y1 = int32_t(image.height);
    const EngineStatus status = Run(image, 0, "7", lines_);
    if (status != EngineStatus::Ok) return status;
    double conf = 0;
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (i) {
            line.text.push_back(' ');
            const int32_t from = line.chars.empty() ? 0 : line.chars.back().x1;
            line.chars.push_back({from, from});
        }
        line.text += lines_[i].text;
        line.chars.insert(line.chars.end(), lines_[i].chars.begin(), lines_[i].chars.end());
        conf += lines_[i].conf;
    }
    if (!lines_.empty()) line.conf = uint16_t(conf / double(lines_.size()) + 0.5);
    for (OcrProtocol::CharSpan& span : line.chars) {
        if (span.x0 > line.x1) span.x0 = line.x1;
        if (span.x1 > line.x1) span.x1 = line.x1;
        if (span.x1 < span.x0) span.x1 = span.x0;
    }
    return EngineStatus::Ok;
}

EngineStatus TesseractEngine::Lines(const std::vector<GrayView>& images, std::vector<OcrProtocol::Line>& lines) {
    lines.resize(images.size());
    for (size_t i = 0; i < images.size(); ++i) {
        if (Line(images[i], lines[i]) != EngineStatus::Ok) return EngineStatus::Failed;
    }
    return EngineStatus::Ok;
}

} // namespace RayoPdf::Ocr
