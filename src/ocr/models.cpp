// SPDX-License-Identifier: Apache-2.0
#include "ocr/models.h"

#include "common/file.h"
#include "crypt/crypt.h"

namespace RayoPdf::Ocr {
namespace {

constexpr uint64_t kMaxTextFile = uint64_t(16) << 20;     // models.txt, config.txt, dict.txt
constexpr uint64_t kMaxModelFile = uint64_t(1) << 30;     // one network
constexpr size_t kMaxManifestFiles = 64;

bool IsHex(std::string_view text) {
    if (text.size() != 64) return false;
    for (char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// The next line of `text` from `at` (without '\n' and a '\r' before it).
bool NextLine(std::string_view text, size_t& at, std::string_view& line) {
    if (at >= text.size()) return false;
    size_t end = text.find('\n', at);
    if (end == std::string_view::npos) end = text.size();
    line = text.substr(at, end - at);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    at = end + 1;
    return true;
}

// Splits `line` at its first space.
void SplitWord(std::string_view line, std::string_view& first, std::string_view& rest) {
    const size_t space = line.find(' ');
    if (space == std::string_view::npos) {
        first = line;
        rest = {};
        return;
    }
    first = line.substr(0, space);
    rest = line.substr(space + 1);
}

bool ParseUnsigned(std::string_view text, uint64_t limit, uint64_t& value) {
    if (text.empty() || text.size() > 19) return false;
    value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + uint64_t(c - '0');
    }
    return value <= limit;
}

std::string Join(const std::string& directory, std::string_view name) {
    std::string path = directory;
    path += '/';
    path.append(name);
    return path;
}

bool ReadText(const std::string& path, std::string& text) {
    File::InputFile file;
    if (file.Open(path, kMaxTextFile) != File::ReadStatus::Ok) return false;
    text.assign(file.Bytes());
    return true;
}

} // namespace

const char* ModelStatusText(ModelStatus status) {
    switch (status) {
    case ModelStatus::Ok: return "ok";
    case ModelStatus::Missing: return "missing or unreadable file";
    case ModelStatus::BadManifest: return "malformed models.txt";
    case ModelStatus::BadConfig: return "malformed config.txt";
    case ModelStatus::BadDictionary: return "malformed dict.txt";
    case ModelStatus::SizeMismatch: return "file size differs from models.txt";
    case ModelStatus::HashMismatch: return "SHA-256 differs from models.txt";
    }
    return "?";
}

bool ParseHex(std::string_view text, uint32_t& value) {
    if (text.empty() || text.size() > 6) return false;
    value = 0;
    for (char c : text) {
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (digit < 0) return false;
        value = value * 16 + uint32_t(digit);
    }
    return value <= 0x10FFFF;
}

bool ParseDecimal(std::string_view text, float& value) {
    uint64_t whole = 0, fraction = 0, scale = 1;
    size_t i = 0;
    if (text.empty()) return false;
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
        whole = whole * 10 + uint64_t(text[i] - '0');
        if (whole > 1000) return false;
    }
    if (i == 0) return false;
    if (i < text.size()) {
        if (text[i] != '.' || i + 1 == text.size()) return false;
        for (++i; i < text.size(); ++i) {
            if (text[i] < '0' || text[i] > '9' || scale >= 100000000) return false;
            fraction = fraction * 10 + uint64_t(text[i] - '0');
            scale *= 10;
        }
    }
    value = float(double(whole) + double(fraction) / double(scale));
    return true;
}

bool ParseManifest(std::string_view text, std::vector<ModelFile>& files) {
    files.clear();
    size_t at = 0;
    std::string_view line;
    while (NextLine(text, at, line)) {
        if (line.empty()) continue;
        std::string_view hash, rest, size, name;
        SplitWord(line, hash, rest);
        SplitWord(rest, size, name);
        ModelFile file;
        if (!IsHex(hash) || !ParseUnsigned(size, kMaxModelFile, file.bytes) || name.empty() || name.size() > 64) {
            return false;
        }
        for (char c : name) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
            if (!ok) return false;
        }
        for (const ModelFile& other : files) {
            if (other.name == name) return false;
        }
        if (files.size() >= kMaxManifestFiles) return false;
        file.sha256.assign(hash);
        file.name.assign(name);
        files.push_back(std::move(file));
    }
    return !files.empty();
}

bool ParseConfig(std::string_view text, ModelSet& set) {
    size_t at = 0;
    std::string_view line;
    bool thresh = false, box = false, unclip = false;
    while (NextLine(text, at, line)) {
        if (line.empty()) continue;
        std::string_view key, value;
        SplitWord(line, key, value);
        if (key == "model") {
            if (value.empty() || value.size() > 64) return false;
            set.model.assign(value);
        } else if (key == "det_thresh") {
            if (!ParseDecimal(value, set.detect.thresh) || set.detect.thresh >= 1) return false;
            thresh = true;
        } else if (key == "det_box_thresh") {
            if (!ParseDecimal(value, set.detect.boxThresh) || set.detect.boxThresh >= 1) return false;
            box = true;
        } else if (key == "det_unclip_ratio") {
            if (!ParseDecimal(value, set.detect.unclipRatio) || set.detect.unclipRatio > 10) return false;
            unclip = true;
        } else if (key == "rec_height") {
            if (value != "48") return false;
        } else if (key == "script") {
            if (value.empty() || value.size() > 32) return false;
            for (char c : value) {
                if (c < 'a' || c > 'z') return false;
            }
            set.script.assign(value);
        } else if (key == "order") {
            if (value != "logical" && value != "visual") return false;
            set.visualOrder = value == "visual";
        } else if (key == "letters") {
            // "XXXX-YYYY": the code points of the script's letters, hexadecimal.
            const size_t dash = value.find('-');
            if (dash == std::string_view::npos || !ParseHex(value.substr(0, dash), set.lettersFirst) ||
                !ParseHex(value.substr(dash + 1), set.lettersLast) || set.lettersFirst > set.lettersLast) {
                return false;
            }
        } else if (key == "space_gap") {
            if (!ParseDecimal(value, set.spaceGap) || set.spaceGap > 4) return false;
        }
    }
    // The detection thresholds come all together (a tier) or not at all (a recogniser of the script
    // pack, the orientation classifier).
    set.detectConfig = thresh && box && unclip;
    return (set.detectConfig || (!thresh && !box && !unclip)) && !set.model.empty();
}

ModelStatus LoadModelSet(const std::string& directory, ModelSet& set, std::string& detail) {
    set = ModelSet();
    set.directory = directory;
    std::string text;
    detail = Join(directory, "models.txt");
    if (!ReadText(detail, text)) return ModelStatus::Missing;
    if (!ParseManifest(text, set.files)) return ModelStatus::BadManifest;
    for (const ModelFile& file : set.files) {
        detail = Join(directory, file.name);
        File::FileInfo info;
        if (!File::StatusOf(detail, info) || info.type != File::FileType::Regular) return ModelStatus::Missing;
        if (info.size != file.bytes) return ModelStatus::SizeMismatch;
    }
    detail = Join(directory, "config.txt");
    if (ManifestHash(set, "config.txt").empty() || !ReadText(detail, text)) return ModelStatus::Missing;
    if (!ParseConfig(text, set)) return ModelStatus::BadConfig;
    detail = Join(directory, "dict.txt");
    if (ManifestHash(set, "dict.txt").empty() || !ReadText(detail, text)) return ModelStatus::Missing;
    if (!set.dictionary.Load(text)) return ModelStatus::BadDictionary;
    detail.clear();
    return ModelStatus::Ok;
}

ModelStatus VerifyModelSet(const ModelSet& set, std::string& detail) {
    static const char kHex[] = "0123456789abcdef";
    for (const ModelFile& file : set.files) {
        detail = Join(set.directory, file.name);
        File::InputFile input;
        if (input.Open(detail, kMaxModelFile) != File::ReadStatus::Ok) return ModelStatus::Missing;
        uint8_t digest[32];
        Crypt::Sha256(input.Bytes(), digest);
        std::string hex;
        for (uint8_t b : digest) {
            hex.push_back(kHex[b >> 4]);
            hex.push_back(kHex[b & 15]);
        }
        if (hex != file.sha256) return ModelStatus::HashMismatch;
    }
    detail.clear();
    return ModelStatus::Ok;
}

std::string_view ManifestHash(const ModelSet& set, std::string_view name) {
    for (const ModelFile& file : set.files) {
        if (file.name == name) return file.sha256;
    }
    return {};
}

} // namespace RayoPdf::Ocr
