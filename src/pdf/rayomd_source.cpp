// SPDX-License-Identifier: Apache-2.0
// Forked from rayomd src/core/rayomd_pdf_source.cpp at ef106ca (Apache-2.0, same author).
// Changes: the reader only (RayoMD's writer side and its SHA-NI code dropped; SHA-256 from
// crypt/), results as views into the PDF instead of copies, and the cross-reference table's size
// bounded by the bytes it occupies before anything is allocated for it.
#include "pdf/rayomd_source.h"

#include "crypt/crypt.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace RayoPdf::RayomdSource {
namespace {

constexpr size_t kMaxObjects = 1000000;
constexpr size_t kMaxMetadataBytes = 16u * 1024u;
// The shortest cross-reference entry the scanner accepts: "1 0 n" and a separator.
constexpr size_t kMinXrefEntryBytes = 6;

bool CheckedAdd(size_t left, size_t right, size_t& result) {
    if (right > std::numeric_limits<size_t>::max() - left) return false;
    result = left + right;
    return true;
}

bool IsWhite(char ch) {
    return ch == '\0' || ch == '\t' || ch == '\n' || ch == '\f' || ch == '\r' || ch == ' ';
}

bool IsDelimiter(char ch) {
    return IsWhite(ch) || ch == '(' || ch == ')' || ch == '<' || ch == '>' || ch == '[' || ch == ']' ||
           ch == '{' || ch == '}' || ch == '/' || ch == '%';
}

class Scanner {
public:
    explicit Scanner(std::string_view input, size_t offset = 0) : input_(input), offset_(offset) {}

    size_t Offset() const { return offset_; }

    bool AtEnd() {
        SkipSpace();
        return offset_ == input_.size();
    }

    void SkipSpace() {
        for (;;) {
            while (offset_ < input_.size() && IsWhite(input_[offset_])) offset_++;
            if (offset_ >= input_.size() || input_[offset_] != '%') return;
            while (offset_ < input_.size() && input_[offset_] != '\r' && input_[offset_] != '\n') offset_++;
        }
    }

    bool Consume(std::string_view token) {
        SkipSpace();
        if (input_.substr(offset_, token.size()) != token) return false;
        size_t end = offset_ + token.size();
        if (!token.empty() && !IsDelimiter(token.back()) && end < input_.size() && !IsDelimiter(input_[end])) {
            return false;
        }
        offset_ = end;
        return true;
    }

    bool ReadUnsigned(size_t& value) {
        SkipSpace();
        size_t start = offset_;
        while (offset_ < input_.size() && input_[offset_] >= '0' && input_[offset_] <= '9') offset_++;
        if (start == offset_) return false;
        auto result = std::from_chars(input_.data() + start, input_.data() + offset_, value);
        return result.ec == std::errc{} && result.ptr == input_.data() + offset_;
    }

    bool ReadName(std::string_view& name) {
        SkipSpace();
        if (offset_ >= input_.size() || input_[offset_] != '/') return false;
        size_t start = ++offset_;
        while (offset_ < input_.size() && !IsDelimiter(input_[offset_])) offset_++;
        if (offset_ - start > 1024) return false;
        name = input_.substr(start, offset_ - start);
        return true;
    }

    bool ReadLiteral(std::string& value) {
        SkipSpace();
        if (offset_ >= input_.size() || input_[offset_] != '(') return false;
        offset_++;
        value.clear();
        unsigned depth = 1;
        while (offset_ < input_.size() && depth != 0) {
            char ch = input_[offset_++];
            if (ch == '\\') {
                if (offset_ >= input_.size()) return false;
                char escaped = input_[offset_++];
                if (escaped == 'n') {
                    value.push_back('\n');
                } else if (escaped == 'r') {
                    value.push_back('\r');
                } else if (escaped == 't') {
                    value.push_back('\t');
                } else if (escaped == 'b') {
                    value.push_back('\b');
                } else if (escaped == 'f') {
                    value.push_back('\f');
                } else if (escaped == '\r' || escaped == '\n') {
                    if (escaped == '\r' && offset_ < input_.size() && input_[offset_] == '\n') offset_++;
                } else {
                    value.push_back(escaped);
                }
            } else if (ch == '(') {
                if (++depth > 16) return false;
                value.push_back(ch);
            } else if (ch == ')') {
                depth--;
                if (depth != 0) value.push_back(ch);
            } else {
                value.push_back(ch);
            }
            if (value.size() > 64u * 1024u) return false;
        }
        return depth == 0;
    }

    // A hex string, such as the file identifiers of a PDF/A trailer: hex digits and white
    // space between '<' and '>'.
    bool ReadHexString() {
        SkipSpace();
        if (offset_ + 1 >= input_.size() || input_[offset_] != '<' || input_[offset_ + 1] == '<') return false;
        size_t at = offset_ + 1;
        size_t digits = 0;
        for (; at < input_.size() && input_[at] != '>'; at++) {
            const char ch = input_[at];
            if (IsWhite(ch)) continue;
            const bool hex = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f');
            if (!hex || ++digits > 1024) return false;
        }
        if (at >= input_.size()) return false;
        offset_ = at + 1;
        return true;
    }

private:
    std::string_view input_;
    size_t offset_ = 0;
};

bool ReadReference(Scanner& scanner, size_t& objectId) {
    size_t generation = 0;
    return scanner.ReadUnsigned(objectId) && objectId > 0 && objectId <= kMaxObjects &&
           scanner.ReadUnsigned(generation) && generation == 0 && scanner.Consume("R");
}

struct DictionaryEntry {
    std::string_view name;
    std::string_view value;
};

struct DictionaryView {
    std::vector<DictionaryEntry> entries;
    size_t endOffset = 0;
};

bool SkipPdfValue(std::string_view input, Scanner& scanner, unsigned depth, size_t& tokens);

bool SkipPdfDictionary(std::string_view input, Scanner& scanner, unsigned depth, size_t& tokens,
                       std::vector<DictionaryEntry>* entries) {
    if (depth > 16 || !scanner.Consume("<<")) return false;
    std::vector<std::string_view> names;
    while (tokens++ < 4096) {
        Scanner end = scanner;
        if (end.Consume(">>")) {
            scanner = end;
            return true;
        }
        std::string_view name;
        if (!scanner.ReadName(name) || names.size() >= 128 ||
            std::find(names.begin(), names.end(), name) != names.end()) {
            return false;
        }
        names.push_back(name);
        scanner.SkipSpace();
        size_t valueStart = scanner.Offset();
        if (!SkipPdfValue(input, scanner, depth + 1, tokens)) return false;
        if (entries) entries->push_back({name, input.substr(valueStart, scanner.Offset() - valueStart)});
    }
    return false;
}

bool SkipPdfValue(std::string_view input, Scanner& scanner, unsigned depth, size_t& tokens) {
    if (depth > 16 || tokens++ >= 4096) return false;
    Scanner compound = scanner;
    if (compound.Consume("<<")) return SkipPdfDictionary(input, scanner, depth, tokens, nullptr);
    compound = scanner;
    if (compound.Consume("[")) {
        scanner = compound;
        while (tokens++ < 4096) {
            Scanner end = scanner;
            if (end.Consume("]")) {
                scanner = end;
                return true;
            }
            if (!SkipPdfValue(input, scanner, depth + 1, tokens)) return false;
        }
        return false;
    }
    std::string literal;
    compound = scanner;
    if (compound.ReadLiteral(literal)) {
        scanner = compound;
        return true;
    }
    compound = scanner;
    if (compound.ReadHexString()) {
        scanner = compound;
        return true;
    }
    std::string_view name;
    compound = scanner;
    if (compound.ReadName(name)) {
        scanner = compound;
        return true;
    }
    size_t number = 0;
    compound = scanner;
    if (compound.ReadUnsigned(number)) {
        Scanner reference = compound;
        size_t generation = 0;
        if (reference.ReadUnsigned(generation) && generation == 0 && reference.Consume("R")) compound = reference;
        scanner = compound;
        return true;
    }
    for (std::string_view keyword : {std::string_view("true"), std::string_view("false"), std::string_view("null")}) {
        compound = scanner;
        if (compound.Consume(keyword)) {
            scanner = compound;
            return true;
        }
    }
    return false;
}

bool ParseDictionary(std::string_view input, DictionaryView& result) {
    result.entries.clear();
    result.endOffset = 0;
    Scanner scanner(input);
    size_t tokens = 0;
    if (!SkipPdfDictionary(input, scanner, 0, tokens, &result.entries)) return false;
    result.endOffset = scanner.Offset();
    return true;
}

const std::string_view* DictionaryValue(const DictionaryView& dictionary, std::string_view key) {
    for (const auto& entry : dictionary.entries) {
        if (entry.name == key) return &entry.value;
    }
    return nullptr;
}

bool ValueReference(std::string_view value, size_t& objectId) {
    Scanner scanner(value);
    return ReadReference(scanner, objectId) && scanner.AtEnd();
}

bool ValueUnsigned(std::string_view value, size_t& number) {
    Scanner scanner(value);
    return scanner.ReadUnsigned(number) && scanner.AtEnd();
}

bool ValueName(std::string_view value, std::string_view expected) {
    Scanner scanner(value);
    std::string_view name;
    return scanner.ReadName(name) && name == expected && scanner.AtEnd();
}

bool ValueLiteral(std::string_view value, std::string_view expected) {
    Scanner scanner(value);
    std::string literal;
    return scanner.ReadLiteral(literal) && literal == expected && scanner.AtEnd();
}

bool ValueDictionary(std::string_view value, DictionaryView& dictionary) {
    if (!ParseDictionary(value, dictionary)) return false;
    Scanner trailing(value, dictionary.endOffset);
    return trailing.AtEnd();
}

bool ValueSingleReferenceArray(std::string_view value, size_t& objectId) {
    Scanner scanner(value);
    return scanner.Consume("[") && ReadReference(scanner, objectId) && scanner.Consume("]") && scanner.AtEnd();
}

bool ValueSourceNameArray(std::string_view value, size_t& objectId) {
    Scanner scanner(value);
    std::string literal;
    return scanner.Consume("[") && scanner.ReadLiteral(literal) && literal == "source.md" &&
           ReadReference(scanner, objectId) && scanner.Consume("]") && scanner.AtEnd();
}

bool DictionaryObject(std::string_view object, DictionaryView& dictionary) {
    if (!ParseDictionary(object, dictionary)) return false;
    Scanner trailing(object, dictionary.endOffset);
    return trailing.Consume("endobj") && trailing.AtEnd();
}

bool StreamObject(std::string_view object, size_t maximum, DictionaryView& dictionary, std::string_view& data) {
    if (!ParseDictionary(object, dictionary)) return false;
    const std::string_view* lengthValue = DictionaryValue(dictionary, "Length");
    size_t length = 0;
    if (!lengthValue || !ValueUnsigned(*lengthValue, length) || length > maximum) return false;
    constexpr std::string_view prefix = "\nstream\n";
    constexpr std::string_view suffix = "\nendstream\nendobj";
    if (object.substr(dictionary.endOffset, prefix.size()) != prefix) return false;
    size_t start = 0, end = 0;
    if (!CheckedAdd(dictionary.endOffset, prefix.size(), start) || !CheckedAdd(start, length, end) ||
        end > object.size() || object.substr(end, suffix.size()) != suffix) {
        return false;
    }
    Scanner trailing(object, end + suffix.size());
    if (!trailing.AtEnd()) return false;
    data = object.substr(start, length);
    return true;
}

struct XrefTable {
    std::vector<size_t> offsets;
    size_t xrefOffset = 0;
    size_t rootId = 0;
};

bool ParseXref(std::string_view pdf, XrefTable& result) {
    size_t tailStart = pdf.size() > 4096 ? pdf.size() - 4096 : 0;
    size_t marker = pdf.rfind("startxref", std::string_view::npos);
    if (marker == std::string_view::npos || marker < tailStart) return false;
    Scanner startScanner(pdf, marker + 9);
    if (!startScanner.ReadUnsigned(result.xrefOffset) || result.xrefOffset >= pdf.size()) return false;
    size_t eof = startScanner.Offset();
    while (eof < pdf.size() && IsWhite(pdf[eof])) eof++;
    if (pdf.substr(eof, 5) != "%%EOF") return false;
    eof += 5;
    while (eof < pdf.size() && IsWhite(pdf[eof])) eof++;
    if (eof != pdf.size()) return false;

    Scanner scanner(pdf, result.xrefOffset);
    size_t first = 0, count = 0;
    if (!scanner.Consume("xref") || !scanner.ReadUnsigned(first) || first != 0 || !scanner.ReadUnsigned(count) ||
        count < 2 || count > kMaxObjects + 1 || count > (pdf.size() - scanner.Offset()) / kMinXrefEntryBytes) {
        return false;
    }
    result.offsets.assign(count, 0);
    size_t previousOffset = 0;
    for (size_t id = 0; id < count; id++) {
        size_t offset = 0, generation = 0;
        if (!scanner.ReadUnsigned(offset) || !scanner.ReadUnsigned(generation)) return false;
        scanner.SkipSpace();
        bool free = false;
        if (scanner.Consume("f")) {
            free = true;
        } else if (!scanner.Consume("n")) {
            return false;
        }
        if (id == 0) {
            if (!free || offset != 0 || generation != 65535) return false;
        } else {
            if (free || generation != 0 || offset == 0 || offset <= previousOffset || offset >= result.xrefOffset) {
                return false;
            }
            result.offsets[id] = offset;
            previousOffset = offset;
        }
    }
    if (!scanner.Consume("trailer")) return false;
    std::string_view trailer = pdf.substr(scanner.Offset(), marker - scanner.Offset());
    DictionaryView dictionary;
    if (!ParseDictionary(trailer, dictionary)) return false;
    Scanner trailing(trailer, dictionary.endOffset);
    const std::string_view* sizeValue = DictionaryValue(dictionary, "Size");
    const std::string_view* rootValue = DictionaryValue(dictionary, "Root");
    size_t declaredSize = 0;
    if (!trailing.AtEnd() || !sizeValue || !ValueUnsigned(*sizeValue, declaredSize) || declaredSize != count ||
        !rootValue || !ValueReference(*rootValue, result.rootId) || result.rootId >= result.offsets.size()) {
        return false;
    }
    return !DictionaryValue(dictionary, "Prev") && !DictionaryValue(dictionary, "XRefStm") &&
           !DictionaryValue(dictionary, "Encrypt");
}

bool ObjectBody(std::string_view pdf, const XrefTable& xref, size_t objectId, std::string_view& body) {
    if (objectId == 0 || objectId >= xref.offsets.size()) return false;
    size_t offset = xref.offsets[objectId];
    if (offset == 0 || offset >= xref.xrefOffset) return false;
    Scanner scanner(pdf, offset);
    size_t parsedId = 0, generation = 0;
    if (!scanner.ReadUnsigned(parsedId) || parsedId != objectId || !scanner.ReadUnsigned(generation) ||
        generation != 0 || !scanner.Consume("obj")) {
        return false;
    }
    size_t end = objectId + 1 < xref.offsets.size() ? xref.offsets[objectId + 1] : xref.xrefOffset;
    if (scanner.Offset() >= end) return false;
    body = pdf.substr(scanner.Offset(), end - scanner.Offset());
    return true;
}

// The value of the XMP attribute rayomd:<name>="...", which must occur exactly once.
bool ExtractAttribute(std::string_view xml, std::string_view name, std::string_view& value) {
    char buffer[48];
    constexpr std::string_view kPrefix = "rayomd:";
    if (kPrefix.size() + name.size() + 2 > sizeof buffer) return false;
    std::memcpy(buffer, kPrefix.data(), kPrefix.size());
    std::memcpy(buffer + kPrefix.size(), name.data(), name.size());
    std::memcpy(buffer + kPrefix.size() + name.size(), "=\"", 2);
    const std::string_view prefix(buffer, kPrefix.size() + name.size() + 2);
    size_t start = xml.find(prefix);
    if (start == std::string_view::npos || xml.find(prefix, start + prefix.size()) != std::string_view::npos) {
        return false;
    }
    start += prefix.size();
    size_t end = xml.find('"', start);
    if (end == std::string_view::npos || end - start > 1024) return false;
    value = xml.substr(start, end - start);
    return value.find('&') == std::string_view::npos && value.find('<') == std::string_view::npos;
}

bool ParseSize(std::string_view text, size_t& value) {
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool CatalogSourceReferences(const DictionaryView& catalog, size_t& fileSpecId) {
    const std::string_view* namesValue = DictionaryValue(catalog, "Names");
    const std::string_view* associatedValue = DictionaryValue(catalog, "AF");
    if (!namesValue || !associatedValue) return false;
    DictionaryView names, embeddedFiles;
    if (!ValueDictionary(*namesValue, names)) return false;
    const std::string_view* embeddedValue = DictionaryValue(names, "EmbeddedFiles");
    if (!embeddedValue || !ValueDictionary(*embeddedValue, embeddedFiles)) return false;
    const std::string_view* sourceNames = DictionaryValue(embeddedFiles, "Names");
    size_t namedId = 0, associatedId = 0;
    return sourceNames && ValueSourceNameArray(*sourceNames, namedId) &&
           ValueSingleReferenceArray(*associatedValue, associatedId) && namedId == associatedId &&
           (fileSpecId = namedId) != 0;
}

bool FileSpecSourceReference(const DictionaryView& fileSpec, size_t& sourceId) {
    const std::string_view* type = DictionaryValue(fileSpec, "Type");
    const std::string_view* fileName = DictionaryValue(fileSpec, "F");
    const std::string_view* unicodeName = DictionaryValue(fileSpec, "UF");
    const std::string_view* relationship = DictionaryValue(fileSpec, "AFRelationship");
    const std::string_view* embeddedValue = DictionaryValue(fileSpec, "EF");
    if (!type || !ValueName(*type, "Filespec") || !fileName || !ValueLiteral(*fileName, "source.md") ||
        !unicodeName || !ValueLiteral(*unicodeName, "source.md") || !relationship ||
        !ValueName(*relationship, "Source") || !embeddedValue) {
        return false;
    }
    DictionaryView embedded;
    if (!ValueDictionary(*embeddedValue, embedded)) return false;
    const std::string_view* fileRef = DictionaryValue(embedded, "F");
    const std::string_view* unicodeRef = DictionaryValue(embedded, "UF");
    size_t fileId = 0, unicodeId = 0;
    return fileRef && unicodeRef && ValueReference(*fileRef, fileId) && ValueReference(*unicodeRef, unicodeId) &&
           fileId == unicodeId && (sourceId = fileId) != 0;
}

Status ReadProfile(std::string_view pdf, Result& result) {
    if (pdf.size() > kMaxPdfBytes) return Status::LimitExceeded;
    if (pdf.size() < 16 || pdf.substr(0, 5) != "%PDF-") return Status::CorruptPdf;
    XrefTable xref;
    if (!ParseXref(pdf, xref)) return Status::CorruptPdf;
    std::string_view catalogObject;
    DictionaryView catalog;
    if (!ObjectBody(pdf, xref, xref.rootId, catalogObject) || !DictionaryObject(catalogObject, catalog)) {
        return Status::CorruptPdf;
    }
    const std::string_view* catalogType = DictionaryValue(catalog, "Type");
    if (!catalogType || !ValueName(*catalogType, "Catalog")) return Status::CorruptPdf;
    size_t metadataId = 0;
    const std::string_view* metadataValue = DictionaryValue(catalog, "Metadata");
    if (!metadataValue || !ValueReference(*metadataValue, metadataId)) return Status::NotReversible;

    std::string_view metadataObject, xml;
    DictionaryView metadata;
    if (!ObjectBody(pdf, xref, metadataId, metadataObject) ||
        !StreamObject(metadataObject, kMaxMetadataBytes, metadata, xml)) {
        return Status::NotReversible;
    }
    const std::string_view* metadataType = DictionaryValue(metadata, "Type");
    const std::string_view* metadataSubtype = DictionaryValue(metadata, "Subtype");
    if (!metadataType || !ValueName(*metadataType, "Metadata") || !metadataSubtype ||
        !ValueName(*metadataSubtype, "XML") || DictionaryValue(metadata, "Filter") ||
        xml.find("<!DOCTYPE") != std::string_view::npos || xml.find("<!ENTITY") != std::string_view::npos) {
        return Status::NotReversible;
    }
    std::string_view encoding, lengthText, digest, attachment;
    if (!ExtractAttribute(xml, "profile", result.info.profile)) return Status::NotReversible;
    if (result.info.profile != "rayomd-source/1") return Status::UnsupportedProfile;
    // PDF 2.0, or PDF 1.7 in a PDF/A-3 or PDF/UA-1 export, which cannot be a PDF 2.0 file.
    const std::string_view header = pdf.substr(0, 8);
    const bool archival = header == "%PDF-1.7" && (xml.find(" pdfaid:part=\"3\"") != std::string_view::npos ||
                                                   xml.find(" pdfuaid:part=\"1\"") != std::string_view::npos);
    if ((header != "%PDF-2.0" && !archival) || !ExtractAttribute(xml, "producer", result.info.producerVersion) ||
        !ExtractAttribute(xml, "encoding", encoding) || !ExtractAttribute(xml, "length", lengthText) ||
        !ExtractAttribute(xml, "sha256", digest) || !ExtractAttribute(xml, "attachment", attachment)) {
        return Status::CorruptPdf;
    }
    if (encoding != "UTF-8") return Status::UnsupportedProfile;
    if (attachment != "source.md" || digest.size() != 64 || !ParseSize(lengthText, result.info.sourceBytes)) {
        return Status::CorruptPdf;
    }
    if (result.info.sourceBytes > kMaxSourceBytes) return Status::LimitExceeded;

    size_t fileSpecId = 0;
    if (!CatalogSourceReferences(catalog, fileSpecId)) return Status::CorruptPdf;
    std::string_view fileSpecObject;
    DictionaryView fileSpec;
    size_t sourceId = 0;
    if (!ObjectBody(pdf, xref, fileSpecId, fileSpecObject) || !DictionaryObject(fileSpecObject, fileSpec) ||
        !FileSpecSourceReference(fileSpec, sourceId)) {
        return Status::CorruptPdf;
    }
    std::string_view sourceObject, source;
    DictionaryView sourceDictionary;
    if (!ObjectBody(pdf, xref, sourceId, sourceObject) ||
        !StreamObject(sourceObject, kMaxSourceBytes, sourceDictionary, source)) {
        return Status::CorruptPdf;
    }
    const std::string_view* sourceType = DictionaryValue(sourceDictionary, "Type");
    const std::string_view* sourceSubtype = DictionaryValue(sourceDictionary, "Subtype");
    const std::string_view* paramsValue = DictionaryValue(sourceDictionary, "Params");
    DictionaryView params;
    const std::string_view* declaredSizeValue = nullptr;
    size_t declaredSourceSize = 0;
    if (!sourceType || !ValueName(*sourceType, "EmbeddedFile") || !sourceSubtype ||
        !ValueName(*sourceSubtype, "text#2Fmarkdown") || DictionaryValue(sourceDictionary, "Filter") ||
        !paramsValue || !ValueDictionary(*paramsValue, params) ||
        !(declaredSizeValue = DictionaryValue(params, "Size")) ||
        !ValueUnsigned(*declaredSizeValue, declaredSourceSize) || declaredSourceSize != result.info.sourceBytes ||
        source.size() != result.info.sourceBytes) {
        return Status::CorruptPdf;
    }
    char actual[64];
    Sha256Hex(source, actual);
    if (std::string_view(actual, sizeof actual) != digest) return Status::IntegrityMismatch;
    if (!IsValidUtf8(source)) return Status::InvalidUtf8;
    result.source = source;
    return Status::Ok;
}

} // namespace

bool IsValidUtf8(std::string_view text) {
    size_t i = 0;
    while (i < text.size()) {
        unsigned char first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) continue;
        unsigned count = 0;
        uint32_t codepoint = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            count = 1;
            codepoint = first & 0x1f;
        } else if (first >= 0xe0 && first <= 0xef) {
            count = 2;
            codepoint = first & 0x0f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 3;
            codepoint = first & 0x07;
        } else {
            return false;
        }
        if (count > text.size() - i) return false;
        for (unsigned j = 0; j < count; j++) {
            unsigned char next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if ((count == 2 && codepoint < 0x800) || (count == 3 && codepoint < 0x10000) ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) {
            return false;
        }
    }
    return true;
}

void Sha256Hex(std::string_view data, char out[64]) {
    uint8_t digest[32];
    Crypt::Sha256(data, digest);
    static constexpr char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        out[2 * i] = kHex[digest[i] >> 4];
        out[2 * i + 1] = kHex[digest[i] & 15];
    }
}

void Read(std::string_view pdf, Result& result) {
    result = Result();
    result.status = ReadProfile(pdf, result);
    if (result.status != Status::Ok) result.source = {};
}

} // namespace RayoPdf::RayomdSource
