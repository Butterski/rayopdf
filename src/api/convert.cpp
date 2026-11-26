// SPDX-License-Identifier: Apache-2.0
#include "api/convert.h"

#include "common/batch.h"

#include <charconv>
#include <cstdint>

#ifndef RAYOPDF_VERSION
#define RAYOPDF_VERSION "0.0.0"
#endif

namespace RayoPdf::Api {

namespace {

Layout::ConvertOptions LayoutOptions(const Options& options) {
    Layout::ConvertOptions out;
    out.firstPage = options.firstPage;
    out.lastPage = options.lastPage;
    out.annotations = options.annotations;
    out.structure = options.structure;
    out.exact = options.exact;
    out.imageDirectory = options.imageDirectory;
    out.imageLinkPrefix = options.imageLinkPrefix;
    switch (options.ocr) {
        case Ocr::Off: out.ocr.mode = Layout::OcrMode::Off; break;
        case Ocr::Auto: out.ocr.mode = Layout::OcrMode::Auto; break;
        case Ocr::Force: out.ocr.mode = Layout::OcrMode::Force; break;
    }
    out.ocr.addon = options.ocrAddon;
    out.math = options.math == Math::Latex ? Layout::MathMode::Latex : Layout::MathMode::Unicode;
    switch (options.format) {
        case Format::Markdown: out.format = Layout::OutputFormat::Markdown; break;
        case Format::Json: out.format = Layout::OutputFormat::Json; break;
        case Format::Chunks: out.format = Layout::OutputFormat::Chunks; break;
        case Format::Inspect: out.format = Layout::OutputFormat::Inspect; break;
        case Format::Text: out.format = Layout::OutputFormat::Text; break;
    }
    out.chunkChars = options.chunkChars;
    out.jsonDetail = options.jsonDetail == JsonDetail::Words   ? Layout::JsonDetail::Words
                     : options.jsonDetail == JsonDetail::Lines ? Layout::JsonDetail::Lines
                                                               : Layout::JsonDetail::Blocks;
    out.pageMarkers = options.pageMarkers;
    out.frontMatter = options.frontMatter;
    out.furniture = options.furniture == Furniture::Keep ? Layout::Furniture::Keep : Layout::Furniture::Drop;
    out.plain = options.plain;
    out.tables = options.tables == Tables::Html        ? Layout::TableFormat::Html
                 : options.tables == Tables::Pipe      ? Layout::TableFormat::Pipe
                 : options.tables == Tables::PipeLatex ? Layout::TableFormat::PipeLatex
                                                       : Layout::TableFormat::Auto;
    out.quotes = options.quotes == Quotes::Ascii ? Layout::Quotes::Ascii : Layout::Quotes::Keep;
    out.smallCaps = options.smallCaps == SmallCaps::Keep ? Layout::SmallCaps::Keep : Layout::SmallCaps::Lower;
    out.figureText = options.figureText == FigureText::Drop ? Layout::FigureText::Drop : Layout::FigureText::Keep;
    out.pageWorkers = options.workers;
    return out;
}

// Opens and converts the PDF in `bytes`, which `name` names in errors.
Result Run(const std::string& name, std::string_view bytes, std::string_view password,
           const Layout::ConvertOptions& options, std::string& output) {
    Result result;
    std::unique_ptr<Document> document;
    result.status = OpenDocument(name, bytes, password, document, result.error);
    if (result.status != Status::Ok) return result;
    Layout::ConvertReport report;
    Layout::Convert(*document, options, output, report);
    result.pages = report.pages;
    std::string counters;
    AppendCounters(counters, report, document->Report());
    result.report = "{";
    result.report.append(counters, 1, std::string::npos);  // without its leading comma
    result.report += '}';
    return result;
}

} // namespace

Status ReadInput(const std::string& path, File::InputFile& file, std::string& error, std::string* buffer) {
    switch (file.Open(path, kMaxInputBytes, buffer)) {
        case File::ReadStatus::Ok: return Status::Ok;
        case File::ReadStatus::NotFound: error = "cannot open " + path; break;
        case File::ReadStatus::NotRegular: error = path + " is not a regular file"; break;
        case File::ReadStatus::TooLarge: error = path + " is too large"; break;
        case File::ReadStatus::Failed: error = "cannot read " + path; break;
    }
    return Status::Unreadable;
}

Status OpenDocument(const std::string& path, std::string_view bytes, std::string_view password,
                    std::unique_ptr<Document>& document, std::string& error) {
    switch (Document::Open(bytes, password, document)) {
        case OpenStatus::Ok: return Status::Ok;
        case OpenStatus::NotPdf: error = path + " is not a PDF file"; return Status::Corrupt;
        case OpenStatus::Corrupt: error = path + " is corrupt beyond repair"; return Status::Corrupt;
        case OpenStatus::TooLarge: error = path + " exceeds a document limit"; return Status::Corrupt;
        case OpenStatus::PasswordRequired:
            error = path + " needs a password (--password=PW)";
            return Status::PasswordRequired;
        case OpenStatus::Unsupported: break;
    }
    error = path + " uses an unsupported encryption";
    return Status::UnsupportedEncryption;
}

void AppendField(std::string& out, const char* key, uint64_t value) {
    if (out.size() > 1) out.push_back(',');
    out += '"';
    out += key;
    out += "\":";
    char buf[20];
    out.append(buf, std::to_chars(buf, buf + sizeof buf, value).ptr);
}

void AppendField(std::string& out, const char* key, bool value) {
    if (out.size() > 1) out.push_back(',');
    out += '"';
    out += key;
    out += value ? "\":true" : "\":false";
}

void AppendField(std::string& out, const char* key, std::string_view value) {
    if (out.size() > 1) out.push_back(',');
    out += '"';
    out += key;
    out += "\":";
    Batch::AppendJsonString(out, value);
}

void AppendCounters(std::string& out, const Layout::ConvertReport& r, const DocumentReport& d) {
    out += ",";
    AppendField(out, "pages_without_text", static_cast<uint64_t>(r.pagesWithoutText));
    AppendField(out, "glyphs", r.glyphs);
    AppendField(out, "unmapped_glyphs", r.unmappedGlyphs);
    AppendField(out, "guessed_glyphs", r.guessedGlyphs);
    AppendField(out, "hidden_glyphs", r.hiddenGlyphs);
    AppendField(out, "headings", static_cast<uint64_t>(r.headings));
    AppendField(out, "paragraphs", static_cast<uint64_t>(r.paragraphs));
    AppendField(out, "list_items", static_cast<uint64_t>(r.listItems));
    AppendField(out, "code_blocks", static_cast<uint64_t>(r.codeBlocks));
    AppendField(out, "tables", static_cast<uint64_t>(r.tables));
    AppendField(out, "links", static_cast<uint64_t>(r.links));
    AppendField(out, "footnotes", static_cast<uint64_t>(r.footnotes));
    AppendField(out, "images", static_cast<uint64_t>(r.images));
    AppendField(out, "images_skipped", static_cast<uint64_t>(r.imagesSkipped));
    AppendField(out, "images_mask_dropped", static_cast<uint64_t>(r.imagesMaskDropped));
    AppendField(out, "header_footer_lines", static_cast<uint64_t>(r.headerFooterLines));
    AppendField(out, "content_errors", static_cast<uint64_t>(r.contentErrors));
    AppendField(out, "structure", std::string_view(Layout::StructureUseName(r.structure)));
    AppendField(out, "structure_elements", static_cast<uint64_t>(r.structureElements));
    AppendField(out, "exact", std::string_view(Layout::ExactUseName(r.exact)));
    AppendField(out, "ocr_pages_routed", static_cast<uint64_t>(r.ocrPagesRouted));
    AppendField(out, "ocr_pages", static_cast<uint64_t>(r.ocrPages));
    AppendField(out, "ocr_failures", static_cast<uint64_t>(r.ocrFailures));
    AppendField(out, "ocr_addon_missing", r.ocrAddonMissing);
    AppendField(out, "ocr_broken_pages", static_cast<uint64_t>(r.ocrBrokenPages));
    AppendField(out, "ocr_vector_text_pages", static_cast<uint64_t>(r.ocrVectorTextPages));
    AppendField(out, "ocr_glyph_fonts", static_cast<uint64_t>(r.ocrGlyphFonts));
    AppendField(out, "ocr_glyphs", r.ocrGlyphs);
    AppendField(out, "ocr_tables", static_cast<uint64_t>(r.ocrTables));
    AppendField(out, "repaired", d.repaired);
    AppendField(out, "encrypted", d.encrypted);
    AppendField(out, "broken_objects", static_cast<uint64_t>(d.brokenObjects));
    AppendField(out, "salvaged_streams", static_cast<uint64_t>(d.salvagedStreams));
    AppendField(out, "failed_streams", static_cast<uint64_t>(d.failedStreams));
    AppendField(out, "limit_hits", static_cast<uint64_t>(d.limitHits));
}

Result ConvertMemory(std::string_view pdf, const Options& options, std::string& output) {
    output.clear();
    return Run("the input", pdf, options.password, LayoutOptions(options), output);
}

Result ConvertMemory(std::string_view pdf, std::string_view password, const Layout::ConvertOptions& options,
                     std::string& output) {
    output.clear();
    return Run("the input", pdf, password, options, output);
}

Result ConvertFile(const std::string& path, const Options& options, std::string& output) {
    output.clear();
    File::InputFile file;
    Result result;
    result.status = ReadInput(path, file, result.error);
    if (result.status != Status::Ok) return result;
    return Run(path, file.Bytes(), options.password, LayoutOptions(options), output);
}

const char* Version() {
    return RAYOPDF_VERSION;
}

} // namespace RayoPdf::Api
