// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of the pages read on several threads (roadmap D4: the output never depends on
// the number of threads): the input as a PDF file converted once on one thread and once with two
// to four page threads started at once (the document in shared mode, layout/convert.cpp), each
// from a Document of its own. The Markdown, the conversion's report and the document's report
// must be equal; a difference ends the run as a crash, both records on standard error.
// Seeds: `python3 tools/fuzz.py seeds` (real PDFs, as fuzz_layout_document).

#include "layout/convert.h"
#include "pdf/document.h"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

namespace {

using namespace RayoPdf;

void Field(std::string& out, const char* name, uint64_t value) {
    char text[64];
    std::snprintf(text, sizeof text, " %s=%" PRIu64, name, value);
    out += text;
}

// Every counter of the two reports, as `--report` prints them.
std::string Record(const Layout::ConvertReport& r, const DocumentReport& d) {
    std::string out;
    Field(out, "pages", static_cast<uint64_t>(r.pages));
    Field(out, "pages_without_text", static_cast<uint64_t>(r.pagesWithoutText));
    Field(out, "glyphs", r.glyphs);
    Field(out, "unmapped_glyphs", r.unmappedGlyphs);
    Field(out, "guessed_glyphs", r.guessedGlyphs);
    Field(out, "hidden_glyphs", r.hiddenGlyphs);
    Field(out, "headings", static_cast<uint64_t>(r.headings));
    Field(out, "paragraphs", static_cast<uint64_t>(r.paragraphs));
    Field(out, "list_items", static_cast<uint64_t>(r.listItems));
    Field(out, "code_blocks", static_cast<uint64_t>(r.codeBlocks));
    Field(out, "tables", static_cast<uint64_t>(r.tables));
    Field(out, "links", static_cast<uint64_t>(r.links));
    Field(out, "footnotes", static_cast<uint64_t>(r.footnotes));
    Field(out, "images", static_cast<uint64_t>(r.images));
    Field(out, "images_skipped", static_cast<uint64_t>(r.imagesSkipped));
    Field(out, "images_mask_dropped", static_cast<uint64_t>(r.imagesMaskDropped));
    Field(out, "header_footer_lines", static_cast<uint64_t>(r.headerFooterLines));
    Field(out, "content_errors", static_cast<uint64_t>(r.contentErrors));
    Field(out, "structure", static_cast<uint64_t>(r.structure));
    Field(out, "structure_elements", r.structureElements);
    Field(out, "exact", static_cast<uint64_t>(r.exact));
    Field(out, "ocr_pages_routed", static_cast<uint64_t>(r.ocrPagesRouted));
    Field(out, "ocr_pages", static_cast<uint64_t>(r.ocrPages));
    Field(out, "ocr_failures", static_cast<uint64_t>(r.ocrFailures));
    Field(out, "ocr_addon_missing", r.ocrAddonMissing);
    Field(out, "ocr_broken_pages", static_cast<uint64_t>(r.ocrBrokenPages));
    Field(out, "ocr_vector_text_pages", static_cast<uint64_t>(r.ocrVectorTextPages));
    Field(out, "ocr_glyph_fonts", static_cast<uint64_t>(r.ocrGlyphFonts));
    Field(out, "ocr_glyphs", r.ocrGlyphs);
    Field(out, "repaired", d.repaired);
    Field(out, "encrypted", d.encrypted);
    Field(out, "linearized", d.linearized);
    Field(out, "xref_sections", static_cast<uint64_t>(d.xrefSections));
    Field(out, "broken_objects", d.brokenObjects);
    Field(out, "salvaged_streams", d.salvagedStreams);
    Field(out, "failed_streams", d.failedStreams);
    Field(out, "limit_hits", d.limitHits);
    return out;
}

// One conversion of `bytes` with `workers` page threads; false when the file does not open.
bool Convert(std::string_view bytes, unsigned workers, std::string& markdown, std::string& record) {
    std::unique_ptr<Document> document;
    if (Document::Open(bytes, "", document) != OpenStatus::Ok) return false;
    Layout::ConvertOptions options;
    options.pageWorkers = workers;
    Layout::ConvertReport report;
    Layout::Convert(*document, options, markdown, report);
    record = Record(report, document->Report());
    return true;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);
    std::string one, oneRecord;
    if (!Convert(bytes, 1, one, oneRecord)) return 0;
    // Two to four threads, by the input: a count that is asked for starts its threads at once.
    const unsigned workers = 2 + static_cast<unsigned>(size % 3);
    std::string several, severalRecord;
    if (!Convert(bytes, workers, several, severalRecord)) std::abort();
    if (one != several || oneRecord != severalRecord) {
        std::fprintf(stderr, "fuzz_layout_pages: %u page threads differ from one\n one:    %s\n several:%s\n", workers,
                     oneRecord.c_str(), severalRecord.c_str());
        if (one != several) {
            size_t at = 0;
            while (at < one.size() && at < several.size() && one[at] == several[at]) ++at;
            std::fprintf(stderr, " Markdown: %zu and %zu bytes, first difference at byte %zu\n", one.size(),
                         several.size(), at);
        }
        std::abort();
    }
    return 0;
}
