// SPDX-License-Identifier: Apache-2.0
// rayopdf-ocr: the OCR add-on of RayoPDF (docs/development/ocr-engine.md). The core starts it as
// `rayopdf-ocr --serve` and talks protocol 1 (docs/development/ocr-protocol.md) over its standard
// input and output; it is a separate executable so that the core binary stays free of the
// inference runtime and the models.
//
//   rayopdf-ocr --serve [OPTIONS]                        answer requests on stdin/stdout
//   rayopdf-ocr --image PAGE.pgm [--dpi N] [--repeat N] [OPTIONS]
//                                                        one page (binary PGM): the response header
//                                                        on stdout; with --repeat, the median time
//                                                        per run on stderr
//   rayopdf-ocr --verify [OPTIONS]                       check the model files' SHA-256
//   rayopdf-ocr --version
// Options: --models DIR (default: rayopdf-ocr-models beside the executable), --tier tiny|small|
// medium, --engine pp-ocrv6|tesseract, --tesseract PROGRAM (default: tesseract on PATH), --lang L
// (Tesseract's language, default eng), --det-side N (longest side of the detection input),
// --space-threshold P (space recovery, recognize.h; above 1 turns it off).
#include "common/file.h"
#include "common/ocr_protocol.h"
#include "ocr/addon/runtime.h"
#include "ocr/engine.h"
#include "ocr/models.h"
#include "ocr/server.h"
#include "ocr/tesseract.h"

#include "common/utf8.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace {

using namespace RayoPdf;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitModels = 3;
constexpr int kExitRuntime = 4;
constexpr int kExitBrokenFrame = 5;
constexpr int kExitUnwritable = 6;
constexpr int kExitImage = 7;
constexpr int kExitEngine = 8;
constexpr const char* kEngineName = "pp-ocrv6";
constexpr uint32_t kMaxRepeat = 1000;

int Fail(int code, const std::string& message) {
    File::WriteStandardError("rayopdf-ocr: " + message + "\n");
    return code;
}

// The folder of the running executable, '/' separated, or empty.
std::string ExecutableFolder() {
    std::string path;
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    if (n == 0 || n >= buffer.size()) return std::string();
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, buffer.data(), int(n), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return std::string();
    path.resize(size_t(bytes));
    WideCharToMultiByte(CP_UTF8, 0, buffer.data(), int(n), &path[0], bytes, nullptr, nullptr);
    std::replace(path.begin(), path.end(), '\\', '/');
#else
    std::vector<char> buffer(4096);
    const ssize_t n = readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (n <= 0 || size_t(n) >= buffer.size()) return std::string();
    path.assign(buffer.data(), size_t(n));
#endif
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool ReadUnsigned(const std::string& text, uint32_t limit, uint32_t& value) {
    if (text.empty() || text.size() > 9) return false;
    value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + uint32_t(c - '0');
    }
    return value <= limit;
}

// A binary PGM (P5, maxval 255; comments allowed in the header) into `pixels`.
bool ReadPgm(std::string_view data, std::vector<uint8_t>& pixels, uint32_t& width, uint32_t& height) {
    size_t at = 0;
    auto skip = [&]() {
        for (;;) {
            while (at < data.size() && (data[at] == ' ' || data[at] == '\t' || data[at] == '\r' || data[at] == '\n')) ++at;
            if (at < data.size() && data[at] == '#') {
                while (at < data.size() && data[at] != '\n') ++at;
                continue;
            }
            return;
        }
    };
    auto number = [&](uint32_t& value) {
        skip();
        value = 0;
        size_t digits = 0;
        while (at < data.size() && data[at] >= '0' && data[at] <= '9' && digits < 9) {
            value = value * 10 + uint32_t(data[at++] - '0');
            ++digits;
        }
        return digits > 0;
    };
    if (data.size() < 2 || data[0] != 'P' || data[1] != '5') return false;
    at = 2;
    uint32_t maxval = 0;
    if (!number(width) || !number(height) || !number(maxval) || maxval != 255 || width == 0 || height == 0) return false;
    if (uint64_t(width) * height > OcrProtocol::kMaxPixels) return false;
    if (at >= data.size()) return false;
    ++at;  // the one whitespace byte after maxval
    const size_t bytes = size_t(width) * height;
    if (data.size() - at < bytes) return false;
    pixels.assign(reinterpret_cast<const uint8_t*>(data.data()) + at, reinterpret_cast<const uint8_t*>(data.data()) + at + bytes);
    return true;
}

// Scripts with at least 20 entries in the dictionary, in a fixed order.
std::vector<std::string> Scripts(const Ocr::Dictionary& dictionary) {
    struct Range {
        const char* name;
        uint32_t first, last;
    };
    static const Range kRanges[] = {
        {"latin", 0x41, 0x24F},     {"greek", 0x370, 0x3FF},      {"cyrillic", 0x400, 0x4FF},
        {"hebrew", 0x590, 0x5FF},   {"arabic", 0x600, 0x6FF},     {"devanagari", 0x900, 0x97F},
        {"thai", 0xE00, 0xE7F},     {"kana", 0x3040, 0x30FF},     {"han", 0x4E00, 0x9FFF},
        {"hangul", 0xAC00, 0xD7AF},
    };
    constexpr size_t kCount = sizeof(kRanges) / sizeof(kRanges[0]);
    uint32_t counts[kCount] = {};
    for (uint32_t cls = 1; cls < dictionary.Classes(); ++cls) {
        const std::string_view entry = dictionary.Entry(cls);
        uint32_t codePoint;
        size_t length;
        if (!Utf8::Decode(entry, 0, codePoint, length)) continue;
        for (size_t i = 0; i < kCount; ++i) {
            if (codePoint >= kRanges[i].first && codePoint <= kRanges[i].last) ++counts[i];
        }
    }
    std::vector<std::string> scripts;
    for (size_t i = 0; i < kCount; ++i) {
        if (counts[i] >= 20) scripts.push_back(kRanges[i].name);
    }
    return scripts;
}

size_t ReadInput(void*, char* buffer, size_t size) {
    return File::ReadStandardInput(buffer, size);
}

bool WriteOutput(void*, std::string_view bytes) {
    return File::WriteStandardOutput(bytes);
}

struct Options {
    bool serve = false, verify = false, version = false;
    std::string image;
    std::string models;
    std::string tier = RAYOPDF_OCR_TIER;
    std::string engine = kEngineName;
    std::string tesseract = "tesseract";
    std::string language = "eng";
    uint32_t dpi = 0;
    uint32_t repeat = 0;
    uint32_t detSide = Ocr::kDefaultDetMaxSide;
    float spaceThreshold = Ocr::kDefaultSpaceThreshold;
};

const char kUsage[] =
    "usage: rayopdf-ocr --serve | --image PAGE.pgm [--dpi N] [--repeat N] | --verify | --version\n"
    "       [--models DIR] [--tier tiny|small|medium] [--engine pp-ocrv6|tesseract]\n"
    "       [--tesseract PROGRAM] [--lang L] [--det-side N] [--space-threshold P]\n";

bool ParseOptions(const std::vector<std::string>& args, Options& options, std::string& error) {
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        const bool hasValue = i + 1 < args.size();
        if (a == "--serve") {
            options.serve = true;
        } else if (a == "--verify") {
            options.verify = true;
        } else if (a == "--version") {
            options.version = true;
        } else if (a == "--image" && hasValue) {
            options.image = args[++i];
        } else if (a == "--models" && hasValue) {
            options.models = args[++i];
        } else if (a == "--tier" && hasValue) {
            options.tier = args[++i];
            if (options.tier != "tiny" && options.tier != "small" && options.tier != "medium") {
                error = "unknown tier " + options.tier;
                return false;
            }
        } else if (a == "--engine" && hasValue) {
            options.engine = args[++i];
            if (options.engine != kEngineName && options.engine != "tesseract") {
                error = "unknown engine " + options.engine;
                return false;
            }
        } else if (a == "--tesseract" && hasValue) {
            options.tesseract = args[++i];
        } else if (a == "--lang" && hasValue) {
            options.language = args[++i];
        } else if (a == "--dpi" && hasValue) {
            if (!ReadUnsigned(args[++i], OcrProtocol::kMaxDpi, options.dpi)) {
                error = "bad --dpi";
                return false;
            }
        } else if (a == "--repeat" && hasValue) {
            if (!ReadUnsigned(args[++i], kMaxRepeat, options.repeat)) {
                error = "bad --repeat";
                return false;
            }
        } else if (a == "--det-side" && hasValue) {
            if (!ReadUnsigned(args[++i], Ocr::kMaxDetSide, options.detSide) || options.detSide < 32) {
                error = "bad --det-side (32 to 4096)";
                return false;
            }
        } else if (a == "--space-threshold" && hasValue) {
            if (!Ocr::ParseDecimal(args[++i], options.spaceThreshold) || options.spaceThreshold > 2) {
                error = "bad --space-threshold (0 to 2; above 1 turns space recovery off)";
                return false;
            }
        } else {
            error = "unknown or incomplete argument " + a;
            return false;
        }
    }
    const int modes = int(options.serve) + int(options.verify) + int(options.version) + int(!options.image.empty());
    if (modes != 1) {
        error = "give exactly one of --serve, --image, --verify, --version";
        return false;
    }
    return true;
}

// Runs one page through `engine` (`repeat` times when timing) and prints the response header.
int RunImage(const Options& options, Ocr::Engine& engine) {
    File::InputFile file;
    if (file.Open(options.image, OcrProtocol::kMaxFrameBytes) != File::ReadStatus::Ok) {
        return Fail(kExitImage, "cannot read " + options.image);
    }
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
    if (!ReadPgm(file.Bytes(), pixels, width, height)) return Fail(kExitImage, options.image + ": not a binary 8-bit PGM");
    OcrProtocol::Response response;
    response.id = 1;
    std::vector<double> times;
    const uint32_t runs = options.repeat ? options.repeat : 1;
    for (uint32_t run = 0; run < runs; ++run) {
        const auto start = std::chrono::steady_clock::now();
        if (engine.Page(Ocr::GrayView{pixels.data(), width, height}, options.dpi, response.lines) !=
            Ocr::EngineStatus::Ok) {
            return Fail(kExitEngine, "the engine failed");
        }
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    std::string frame;
    OcrProtocol::WriteResponse(response, frame);
    if (!File::WriteStandardOutput(std::string_view(frame).substr(4))) return kExitUnwritable;
    if (options.repeat) {
        std::sort(times.begin(), times.end());
        const double median = times[times.size() / 2];
        File::WriteStandardError("ms " + std::to_string(median) + " min " + std::to_string(times.front()) + "\n");
    }
    return kExitOk;
}

} // namespace

int main(int argc, char** argv) {
#ifndef _WIN32
    // A request's pipe to tesseract, or the core, may close early: report EPIPE, do not die.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    const std::vector<std::string> args = File::Arguments(argc, argv);
    Options options;
    std::string error;
    if (!ParseOptions(args, options, error)) return Fail(kExitUsage, error + "\n" + kUsage);

    if (options.version) {
        File::WriteStandardOutput(std::string("rayopdf-ocr ") + RAYOPDF_VERSION + " (" + kEngineName + " " +
                                  options.tier + ", " + Ocr::kRuntimeName + " " + Ocr::kRuntimeVersion + ")\n");
        return kExitOk;
    }

    OcrProtocol::Handshake handshake;
    handshake.version = RAYOPDF_VERSION;
    handshake.protocol = OcrProtocol::kProtocol;
    std::unique_ptr<Ocr::Engine> engine;
    std::unique_ptr<Ocr::Runtime> runtime;
    Ocr::ModelSet models;
    if (options.engine == "tesseract") {
        std::unique_ptr<Ocr::TesseractEngine> tesseract(new Ocr::TesseractEngine(options.tesseract, options.language));
        std::string version;
        if (!tesseract->Probe(version)) return Fail(kExitRuntime, "cannot run " + options.tesseract + " --version");
        handshake.engine = "tesseract";
        handshake.tier = options.language;
        handshake.runtime = version;
        handshake.scripts.push_back("latin");
        engine = std::move(tesseract);
    } else {
        std::string folder = options.models;
        if (folder.empty()) {
            folder = ExecutableFolder();
            if (folder.empty()) return Fail(kExitModels, "cannot find the executable's folder; give --models");
            folder += "/rayopdf-ocr-models";
        }
        folder += "/" + options.tier;
        std::string detail;
        const Ocr::ModelStatus status = Ocr::LoadModelSet(folder, models, detail);
        if (status != Ocr::ModelStatus::Ok) {
            return Fail(kExitModels, detail + ": " + Ocr::ModelStatusText(status));
        }
        if (options.verify) {
            const Ocr::ModelStatus verified = Ocr::VerifyModelSet(models, detail);
            if (verified != Ocr::ModelStatus::Ok) return Fail(kExitModels, detail + ": " + Ocr::ModelStatusText(verified));
            File::WriteStandardOutput(folder + ": " + std::to_string(models.files.size()) + " files verified\n");
            return kExitOk;
        }
        handshake.engine = kEngineName;
        handshake.tier = options.tier;
        handshake.runtime = Ocr::kRuntimeName;
        for (const char* name : {"config.txt", "dict.txt"}) {
            handshake.models.emplace_back(name, std::string(Ocr::ManifestHash(models, name)));
        }
        for (size_t i = 0; i < Ocr::kRuntimeFileCount; ++i) {
            const std::string_view hash = Ocr::ManifestHash(models, Ocr::kRuntimeFiles[i]);
            if (hash.empty()) return Fail(kExitModels, folder + ": models.txt lacks " + Ocr::kRuntimeFiles[i]);
            handshake.models.emplace_back(Ocr::kRuntimeFiles[i], std::string(hash));
        }
        handshake.scripts = Scripts(models.dictionary);
        runtime = Ocr::CreateRuntime(models, detail);
        if (!runtime) return Fail(kExitRuntime, "cannot load the networks: " + detail);
        Ocr::PpOcrSettings settings;
        settings.detect = models.detect;
        settings.detMaxSide = options.detSide;
        settings.spaceThreshold = options.spaceThreshold;
        engine.reset(new Ocr::PpOcrEngine(*runtime, models.dictionary, settings));
    }
    if (options.verify) return Fail(kExitUsage, "--verify checks model files; the tesseract engine has none");

    if (!options.image.empty()) return RunImage(options, *engine);

    Ocr::ServerIo io;
    io.read = ReadInput;
    io.write = WriteOutput;
    switch (Ocr::Serve(io, handshake, *engine)) {
    case Ocr::ServeStatus::Quit:
    case Ocr::ServeStatus::End:
        return kExitOk;
    case Ocr::ServeStatus::BrokenFrame:
        return kExitBrokenFrame;
    case Ocr::ServeStatus::WriteFailed:
        return kExitUnwritable;
    }
    return kExitOk;
}
