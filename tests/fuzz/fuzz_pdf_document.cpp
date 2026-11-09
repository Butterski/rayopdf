// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of Document::Open and everything a reader then asks of a document: every page's
// attributes and content, every object, every stream decoded. Page content is also checked against
// its definition: StreamData of each /Contents piece joined with newlines, on a second open of the
// same bytes, gives the same bytes, status and report (an abort otherwise).
#include "pdf/document.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

namespace {

using namespace RayoPdf;

// PageContent as it is defined: each piece by StreamData, at most the stream limit in all.
StreamStatus JoinedContent(const Document& doc, int index, std::string& out) {
    out.clear();
    if (index < 0 || index >= doc.PageCount()) return StreamStatus::NotStream;
    const PdfValue& contents = doc.Get(doc.PageDict(index), "Contents");
    if (contents.IsNull()) return StreamStatus::Ok;
    if (!contents.IsArray()) return doc.StreamData(contents, out);
    const auto rank = [](StreamStatus s) {
        switch (s) {
            case StreamStatus::Ok: return 0;
            case StreamStatus::Salvaged: return 1;
            case StreamStatus::Unsupported: return 2;
            case StreamStatus::TooLarge: return 3;
            case StreamStatus::Corrupt: return 4;
            case StreamStatus::NotStream: return 5;
        }
        return 5;
    };
    StreamStatus status = StreamStatus::Ok;
    bool first = true;
    std::string piece;
    for (const PdfValue& item : contents.Array()) {
        if (out.size() >= Document::kDefaultStreamLimit) {
            if (rank(StreamStatus::TooLarge) > rank(status)) status = StreamStatus::TooLarge;
            break;
        }
        const StreamStatus s = doc.StreamData(item, piece, Document::kDefaultStreamLimit - out.size());
        if (rank(s) > rank(status)) status = s;
        if (!first) out.push_back('\n');
        first = false;
        out.append(piece);
    }
    return status;
}

void PageAttributes(const Document& doc, int index) {
    doc.PageDict(index);
    doc.MediaBox(index);
    doc.CropBox(index);
    doc.Rotate(index);
    doc.Resources(index);
}

bool SameReport(const DocumentReport& a, const DocumentReport& b) {
    return a.repaired == b.repaired && a.encrypted == b.encrypted && a.linearized == b.linearized &&
           a.xrefSections == b.xrefSections && a.brokenObjects == b.brokenObjects &&
           a.salvagedStreams == b.salvagedStreams && a.failedStreams == b.failedStreams;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);
    std::unique_ptr<Document> doc;
    if (Document::Open(bytes, "", doc) != OpenStatus::Ok) return 0;
    std::unique_ptr<Document> reference;
    if (Document::Open(bytes, "", reference) != OpenStatus::Ok) std::abort();
    // Bounded so that one input stays fast: objects and pages past these are what the first
    // ones already exercise.
    const int pages = doc->PageCount() < 256 ? doc->PageCount() : 256;
    std::string joined;
    for (int i = 0; i < pages; i++) {
        PageAttributes(*doc, i);
        std::string_view content;
        const StreamStatus status = doc->PageContent(i, content);
        PageAttributes(*reference, i);
        const StreamStatus expected = JoinedContent(*reference, i, joined);
        if (status != expected || content != joined) std::abort();
    }
    // limitHits differs only by the limit test PageContent counts and the reference does not.
    if (!SameReport(doc->Report(), reference->Report())) std::abort();
    std::string out;
    const uint32_t objects = doc->ObjectCount() < 4096 ? doc->ObjectCount() : 4096;
    for (uint32_t num = 0; num < objects; num++) {
        const PdfValue& value = doc->Object(num);
        if (value.IsStream()) doc->StreamData(value, out, size_t(1) << 20);
        PdfRect rect;
        doc->Rect(value, rect);
        double number = 0;
        doc->Number(value, number);
        doc->Get(value, "Type");
    }
    doc->Version();
    doc->Info();
    doc->Catalog();
    doc->Trailer();
    return 0;
}
