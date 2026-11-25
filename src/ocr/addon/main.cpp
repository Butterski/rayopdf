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
// Options: --models DIR (default: rayopdf-ocr-models beside the executable), --tier tiny|small,
// --first-pass tiny|none (default tiny: tiny's recogniser reads every line first, the tier's reads
// again what tiny reads doubtfully; engine.h), --scripts on|off (the script pack in DIR/scripts/,
// default on when present), --orient on|off (the orientation classifier in DIR/orient/, default on
// when present), --det-side N (longest side of the detection input), --space-threshold P (space
// recovery, recognize.h; above 1 turns it off).
#include "common/file.h"
#include "common/ocr_protocol.h"
#include "ocr/addon/runtime.h"
#include "ocr/engine.h"
#include "ocr/models.h"
#include "ocr/server.h"

#include "common/utf8.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
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
    std::string firstPass = "tiny";
    bool scripts = true, orient = true;
    bool stats = false;
    uint32_t dpi = 0;
    uint32_t repeat = 0;
    uint32_t detSide = Ocr::kDefaultDetMaxSide;
    float spaceThreshold = Ocr::kDefaultSpaceThreshold;
};

const char kUsage[] =
    "usage: rayopdf-ocr --serve | --image PAGE.pgm [--dpi N] [--repeat N] | --verify | --version\n"
    "       [--models DIR] [--tier tiny|small] [--first-pass tiny|none] [--scripts on|off]\n"
    "       [--orient on|off] [--det-side N] [--space-threshold P] [--stats]\n";

// "on" or "off" into `value`.
bool ReadSwitch(const std::string& text, bool& value) {
    if (text != "on" && text != "off") return false;
    value = text == "on";
    return true;
}

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
        } else if (a == "--stats") {
            options.stats = true;
        } else if (a == "--image" && hasValue) {
            options.image = args[++i];
        } else if (a == "--models" && hasValue) {
            options.models = args[++i];
        } else if (a == "--tier" && hasValue) {
            options.tier = args[++i];
            if (options.tier != "tiny" && options.tier != "small") {
                error = "unknown tier " + options.tier;
                return false;
            }
        } else if (a == "--first-pass" && hasValue) {
            options.firstPass = args[++i];
            if (options.firstPass != "tiny" && options.firstPass != "none") {
                error = "unknown first pass " + options.firstPass;
                return false;
            }
        } else if ((a == "--scripts" || a == "--orient") && hasValue) {
            if (!ReadSwitch(args[++i], a == "--scripts" ? options.scripts : options.orient)) {
                error = a + " takes on or off";
                return false;
            }
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

// Runs one page through `engine` (`repeat` times when timing) and prints the response header;
// with --stats, what the engine did on its first run (Ocr::EngineStats) on standard error.
int RunImage(const Options& options, Ocr::PpOcrEngine& engine) {
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
        if (run == 0 && options.stats) {
            const Ocr::EngineStats& stats = engine.stats;
            char line[256];
            std::snprintf(line, sizeof line,
                          "stats {\"recognitions\":%llu,\"script_recognitions\":%llu,\"classifications\":%llu,"
                          "\"turned\":%u,\"second_looks\":%u,\"flip\":%.4f}\n",
                          (unsigned long long)stats.recognitions, (unsigned long long)stats.scriptRecognitions,
                          (unsigned long long)stats.classifications, stats.turnedPages, stats.secondLooks,
                          double(stats.lastFlip));
            File::WriteStandardError(line);
        }
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

// Appends `set`'s files the engine reads to the handshake, each named `prefix` + file name, with
// its SHA-256 from models.txt: config.txt, dict.txt, then the runtime's network files (the
// detector's only when `detector`).
bool HandshakeModels(const Ocr::ModelSet& set, const std::string& prefix, bool detector,
                     OcrProtocol::Handshake& handshake, std::string& detail) {
    for (const char* name : {"config.txt", "dict.txt"}) {
        handshake.models.emplace_back(prefix + name, std::string(Ocr::ManifestHash(set, name)));
    }
    for (size_t i = 0; i < Ocr::kRuntimeFileCount; ++i) {
        const std::string_view name = Ocr::kRuntimeFiles[i];
        if (!detector && name.substr(0, 4) == "det.") continue;
        const std::string_view hash = Ocr::ManifestHash(set, name);
        if (hash.empty()) {
            detail = set.directory + ": models.txt lacks " + std::string(name);
            return false;
        }
        handshake.models.emplace_back(prefix + std::string(name), std::string(hash));
    }
    return true;
}

// Appends every file of `set`'s manifest to the handshake, named `prefix` + file name (the script
// pack and the classifier: their manifests hold one runtime's files).
void HandshakeAll(const Ocr::ModelSet& set, const std::string& prefix, OcrProtocol::Handshake& handshake) {
    for (const Ocr::ModelFile& file : set.files) handshake.models.emplace_back(prefix + file.name, file.sha256);
}

// The script pack's folders, in the order their recognisers are tried (docker/ocr_models.py).
const char* const kPackScripts[] = {"cyrillic", "hangul", "arabic", "devanagari", "kannada", "tamil"};

// An optional folder (the classifier, a recogniser of the pack): loaded when present and readable by
// this runtime; a broken one is reported on standard error and left out.
bool LoadOptional(const std::string& directory, Ocr::Networks networks, Ocr::ModelSet& set) {
    std::string detail;
    File::FileInfo info;
    if (!File::StatusOf(directory + "/models.txt", info)) return false;
    const Ocr::ModelStatus status = Ocr::LoadModelSet(directory, set, detail);
    if (status != Ocr::ModelStatus::Ok) {
        File::WriteStandardError("rayopdf-ocr: " + detail + ": " + Ocr::ModelStatusText(status) + "; left out\n");
        return false;
    }
    return Ocr::RuntimeReads(set, networks);
}

// The script pack: recognisers loaded on first use (a page that needs none loads none).
class FolderScripts final : public Ocr::ScriptPack {
public:
    struct Entry {
        Ocr::ModelSet set;
        std::unique_ptr<Ocr::Runtime> runtime;
        Ocr::Recognizer recognizer;
        bool tried = false;
    };
    std::vector<std::unique_ptr<Entry>> entries;

    size_t Count() const override { return entries.size(); }
    const Ocr::Recognizer* Get(size_t index) override {
        Entry& entry = *entries[index];
        if (!entry.tried) {
            entry.tried = true;
            std::string detail;
            entry.runtime = Ocr::CreateRuntime(entry.set, Ocr::Networks::RecognizeOnly, detail);
            if (entry.runtime) {
                entry.recognizer = {entry.runtime.get(), &entry.set.dictionary, entry.set.visualOrder, entry.set.spaceGap,
                                    entry.set.lettersFirst, entry.set.lettersLast};
            } else {
                File::WriteStandardError("rayopdf-ocr: cannot load " + detail + "; script left out\n");
            }
        }
        return entry.runtime ? &entry.recognizer : nullptr;
    }
};

} // namespace

int main(int argc, char** argv) {
#ifndef _WIN32
    // The core's pipe may close early: report EPIPE, do not die.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    const std::vector<std::string> args = File::Arguments(argc, argv);
    Options options;
    std::string error;
    if (!ParseOptions(args, options, error)) return Fail(kExitUsage, error + "\n" + kUsage);

    // Tiny's recogniser reads first unless tiny is the tier itself or the first pass is off.
    const bool firstPass = options.firstPass == "tiny" && options.tier != "tiny";
    if (options.version) {
        File::WriteStandardOutput(std::string("rayopdf-ocr ") + RAYOPDF_VERSION + " (" + kEngineName + " " +
                                  options.tier + (firstPass ? " with a tiny first pass" : "") + ", " +
                                  Ocr::kRuntimeName + " " + Ocr::kRuntimeVersion + ")\n");
        return kExitOk;
    }

    OcrProtocol::Handshake handshake;
    handshake.version = RAYOPDF_VERSION;
    handshake.protocol = OcrProtocol::kProtocol;
    std::unique_ptr<Ocr::Runtime> runtime, firstRuntime, orientRuntime;
    Ocr::ModelSet models, firstModels, orientModels;
    FolderScripts scripts;
    std::string root = options.models;
    if (root.empty()) {
        root = ExecutableFolder();
        if (root.empty()) return Fail(kExitModels, "cannot find the executable's folder; give --models");
        root += "/rayopdf-ocr-models";
    }
    std::string detail;
    Ocr::ModelStatus status = Ocr::LoadModelSet(root + "/" + options.tier, models, detail);
    if (status == Ocr::ModelStatus::Ok && firstPass) status = Ocr::LoadModelSet(root + "/tiny", firstModels, detail);
    if (status == Ocr::ModelStatus::Ok && !models.detectConfig) status = Ocr::ModelStatus::BadConfig;
    if (status != Ocr::ModelStatus::Ok) return Fail(kExitModels, detail + ": " + Ocr::ModelStatusText(status));
    const bool orient = options.orient && LoadOptional(root + "/orient", Ocr::Networks::Classify, orientModels);
    if (options.scripts) {
        for (const char* name : kPackScripts) {
            std::unique_ptr<FolderScripts::Entry> entry(new FolderScripts::Entry());
            if (LoadOptional(root + "/scripts/" + name, Ocr::Networks::RecognizeOnly, entry->set) &&
                entry->set.script == name) {
                scripts.entries.push_back(std::move(entry));
            }
        }
    }
    if (options.verify) {
        std::vector<const Ocr::ModelSet*> sets = {&models};
        if (firstPass) sets.push_back(&firstModels);
        if (orient) sets.push_back(&orientModels);
        for (const std::unique_ptr<FolderScripts::Entry>& entry : scripts.entries) sets.push_back(&entry->set);
        std::string said;
        for (const Ocr::ModelSet* set : sets) {
            const Ocr::ModelStatus verified = Ocr::VerifyModelSet(*set, detail);
            if (verified != Ocr::ModelStatus::Ok) return Fail(kExitModels, detail + ": " + Ocr::ModelStatusText(verified));
            said += set->directory + ": " + std::to_string(set->files.size()) + " files verified\n";
        }
        File::WriteStandardOutput(said);
        return kExitOk;
    }
    handshake.engine = kEngineName;
    handshake.tier = options.tier;
    handshake.runtime = Ocr::kRuntimeName;
    // The files the engine reads: the tier's, then the first pass's (its recogniser alone), named
    // "tiny/<file>".
    if (!HandshakeModels(models, "", true, handshake, detail) ||
        (firstPass && !HandshakeModels(firstModels, "tiny/", false, handshake, detail))) {
        return Fail(kExitModels, detail);
    }
    handshake.scripts = Scripts(models.dictionary);
    // Then the classifier's ("orient/<file>") and the script pack's ("scripts/<script>/<file>"),
    // whose scripts join the list.
    if (orient) HandshakeAll(orientModels, "orient/", handshake);
    for (const std::unique_ptr<FolderScripts::Entry>& entry : scripts.entries) {
        HandshakeAll(entry->set, "scripts/" + entry->set.script + "/", handshake);
        if (std::find(handshake.scripts.begin(), handshake.scripts.end(), entry->set.script) == handshake.scripts.end()) {
            handshake.scripts.push_back(entry->set.script);
        }
    }
    runtime = Ocr::CreateRuntime(models, Ocr::Networks::DetectAndRecognize, detail);
    if (!runtime) return Fail(kExitRuntime, "cannot load the networks: " + detail);
    Ocr::PpOcrSettings settings;
    settings.detect = models.detect;
    settings.detMaxSide = options.detSide;
    settings.spaceThreshold = options.spaceThreshold;
    std::unique_ptr<Ocr::PpOcrEngine> engine;
    if (firstPass) {
        firstRuntime = Ocr::CreateRuntime(firstModels, Ocr::Networks::RecognizeOnly, detail);
        if (!firstRuntime) return Fail(kExitRuntime, "cannot load the networks: " + detail);
        engine.reset(new Ocr::PpOcrEngine(*runtime, models.dictionary, *firstRuntime, firstModels.dictionary, settings));
    } else {
        engine.reset(new Ocr::PpOcrEngine(*runtime, models.dictionary, settings));
    }
    if (orient) {
        orientRuntime = Ocr::CreateRuntime(orientModels, Ocr::Networks::Classify, detail);
        if (!orientRuntime) return Fail(kExitRuntime, "cannot load the networks: " + detail);
        engine->SetOrientation(orientRuntime.get());
    }
    if (scripts.Count()) engine->SetScripts(&scripts);

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
