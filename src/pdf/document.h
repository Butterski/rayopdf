// SPDX-License-Identifier: Apache-2.0
// The PDF object layer (ISO 32000-1 sections 7.5-7.7, roadmap M1): cross-reference tables and
// streams, object streams, hybrid files and /Prev chains (cycle-checked), a single-pass repair
// scan when the cross-reference data is unusable, decryption through the Standard security
// handler, decoded streams, and the page tree with inherited attributes.
//
// Objects are parsed on first use and live in the document's arena as long as it does; every
// value and string view returned stays valid until the Document goes. A Document is used by one
// thread at a time (const member functions fill caches).
//
// Hostile input: every loop is bounded (roadmap D8 / algorithms §1.5): value nesting 64,
// reference chains 32, /Prev chains and the page tree cycle-checked, a page tree at most 64 levels
// deep, every decoded stream at most `limit` bytes, the decoded bytes of a whole document at most
// kDocumentDecodeBudget. Every limit fails closed and is counted in the report.
#pragma once

#include "pdf/filters.h"
#include "pdf/value.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace RayoPdf {

// A rectangle in default user space, normalized so that x0 <= x1 and y0 <= y1.
struct PdfRect {
    double x0 = 0;
    double y0 = 0;
    double x1 = 0;
    double y1 = 0;

    double Width() const { return x1 - x0; }
    double Height() const { return y1 - y0; }
};

enum class OpenStatus : uint8_t {
    Ok,
    NotPdf,            // no "%PDF-" header in the first 1024 bytes and no object found by scanning
    Corrupt,           // no catalog with a readable page tree, even after repair
    PasswordRequired,  // encrypted, and neither the given password nor "" opens it
    Unsupported,       // encrypted with a handler or revision RayoPDF does not implement
    TooLarge,          // past a document limit (more than kMaxObjects objects)
};

// What reading the document met, for `--inspect`, `--report` and uncertainty comments. Counters
// grow as objects and streams are read.
struct DocumentReport {
    bool repaired = false;         // objects were located by scanning the file
    bool encrypted = false;
    bool linearized = false;
    int xrefSections = 0;          // cross-reference sections read through /Prev and /XRefStm
    uint32_t brokenObjects = 0;    // referenced objects that could not be parsed
    uint32_t salvagedStreams = 0;  // streams decoded only up to a corrupt or truncated point
    uint32_t failedStreams = 0;    // streams with no usable data
    uint32_t limitHits = 0;        // limits reached (stream size, decode budget, nesting, cycles)
};

class Document {
public:
    static constexpr uint32_t kMaxObjects = 8u << 20;
    static constexpr size_t kDefaultStreamLimit = size_t(256) << 20;
    static constexpr uint64_t kDocumentDecodeBudget = uint64_t(4) << 30;

    // Reads `bytes`, a whole PDF file that the caller keeps alive and unchanged while the
    // Document lives. `password` is tried as the user password, then the owner password.
    // `out` is set only for OpenStatus::Ok.
    static OpenStatus Open(std::string_view bytes, std::string_view password, std::unique_ptr<Document>& out);
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    std::string_view Bytes() const;
    // The header's version ("1.7"), or the catalog's /Version when that is later.
    std::string_view Version() const;
    const PdfValue& Trailer() const;
    // The catalog (/Root), resolved.
    const PdfValue& Catalog() const;
    // The /Info dictionary, resolved; null when absent.
    const PdfValue& Info() const;
    // One more than the highest object number known.
    uint32_t ObjectCount() const;
    const DocumentReport& Report() const;

    // Object `num` (any generation), parsed on first use; null when free, missing or unreadable.
    const PdfValue& Object(uint32_t num) const;
    // `value`, or the object a reference names (followed through up to 32 references); null for a
    // missing object or a generation that does not match.
    const PdfValue& Resolve(const PdfValue& value) const;
    // Entry `key` of the dictionary or stream `dict` (itself resolved first), resolved; null when
    // absent.
    const PdfValue& Get(const PdfValue& dict, std::string_view key) const;
    // A number, resolved; false for anything else or a non-finite value.
    bool Number(const PdfValue& value, double& out) const;
    // An array of four numbers, resolved and normalized; false for anything else.
    bool Rect(const PdfValue& value, PdfRect& out) const;

    // The data of `stream` (resolved) decrypted and run through every filter of /Filter with its
    // /DecodeParms, at most `limit` bytes, into `out` (replaced). Decoding stops before the first
    // image filter (Unsupported, `out` holding the data that filter would read).
    StreamStatus StreamData(const PdfValue& stream, std::string& out, size_t limit = kDefaultStreamLimit) const;

    class StreamReader;

    int PageCount() const;
    // Page `index` (0-based) as the page tree orders the leaves; null for an index out of range.
    const PdfValue& PageDict(int index) const;
    // Inherited attributes. MediaBox defaults to US Letter; CropBox to the MediaBox, clipped to
    // it; Rotate is normalized to 0, 90, 180 or 270; Resources is null when the page has none.
    PdfRect MediaBox(int index) const;
    PdfRect CropBox(int index) const;
    int Rotate(int index) const;
    const PdfValue& Resources(int index) const;
    // The page's content: its /Contents stream, or the streams of a /Contents array joined with
    // a newline, decoded into `out` (replaced). The worst status of its streams.
    StreamStatus PageContent(int index, std::string& out) const;

private:
    struct Impl;
    explicit Document(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// StreamData's output a piece at a time, for data too large to hold whole (an image's samples):
// the same bytes in order, the same status, and the same counts in the report and the decode
// budget, made once per Open whatever the passes. Only the last filter's output is never held
// whole (Filters::Chain); decrypted data and the output of the filters before it are.
//
// A stream is counted when a pass reaches its end, or by Finish (which reads it to the end). Meanwhile
// it holds back its limit from the budget: a decode that the stream's count could change (the
// budget nearly spent) first reads and counts every stream held back, in the order they were
// opened, so every count and limit is as if StreamData had read each stream whole when it was opened.
class Document::StreamReader {
public:
    StreamReader() = default;
    ~StreamReader();
    StreamReader(const StreamReader&) = delete;
    StreamReader& operator=(const StreamReader&) = delete;

    // StreamData(stream, ·, limit) begun: NotStream, Corrupt, Unsupported or TooLarge when it ends
    // at once (with nothing to read, counted as StreamData counts it), else Ok and the output follows.
    StreamStatus Open(const Document& document, const PdfValue& stream, size_t limit = Document::kDefaultStreamLimit);
    // The next piece (at least one byte), valid until the next call; false at the end of the output.
    bool Next(std::string_view& piece);
    // The output again from its first byte (a second pass reads the same bytes, counted once).
    void Restart();
    // Reads to the end of the output (counting the stream, once) and returns StreamData's status.
    StreamStatus Finish();
    // The bytes given since the start of this pass.
    uint64_t Size() const { return chain_.Size(); }

private:
    friend struct Document::Impl;

    // Counts the stream in the document as StreamData does, once.
    void Count(StreamStatus status, uint64_t size);

    Document::Impl* impl_ = nullptr;
    std::vector<Filters::Stage> stages_;
    std::string_view raw_;
    std::string decrypted_;
    Filters::Chain chain_;
    size_t limit_ = 0;
    StreamStatus start_ = StreamStatus::Ok;   // the status before the filters (decryption)
    StreamStatus status_ = StreamStatus::Ok;  // the status of a read that ended at once
    bool reading_ = false;  // Open found data to decode
    bool counted_ = false;
};

} // namespace RayoPdf
