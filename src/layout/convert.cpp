// SPDX-License-Identifier: Apache-2.0
#include "layout/convert.h"

#include "common/batch.h"
#include "layout/exact.h"
#include "layout/interpreter.h"
#include "layout/ocr.h"
#include "layout/page_pool.h"
#include "layout/pipeline.h"
#include "layout/structure.h"
#include "text/font.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace RayoPdf::Layout {

namespace {

// The threads for `pages` pages (ConvertOptions::pageWorkers): the CPUs are counted only for a
// document of several pages.
unsigned PageWorkers(unsigned asked, size_t pages) {
    if (pages < 2) return 1;
    if (asked == 0) asked = std::min(Batch::UsableCpus(), kDefaultPageWorkers);
    return Batch::WorkerCount(asked, pages);
}

// One page's counters, into the report and the page's quality.
void Tally(const PageStats& stats, PageTextQuality& quality, ConvertReport& report) {
    report.glyphs += stats.glyphs;
    report.unmappedGlyphs += stats.unmapped;
    report.guessedGlyphs += stats.guessed;
    quality.glyphs = stats.glyphs;
    quality.unreliable = stats.unmapped + stats.guessed;
}

// Pages `first` (0-based) on into `pages` (and their counters into `quality` and `report`), on the
// threads of `pool` sharing the document and the fonts when the document allows it
// (Document::BeginShared): what a thread reads cannot depend on the others, unless the document
// says it might have, and then the pages are read again on this thread in page order (roadmap D4:
// the output never depends on the number of threads). True: read; false: left to the caller.
bool InterpretShared(const Document& document, Text::FontCache& fonts, bool links, int first,
                     std::vector<PageData>& pages, std::vector<PageTextQuality>& quality, ConvertReport& report,
                     PagePool& pool) {
    if (!document.BeginShared()) return false;
    const size_t count = pages.size();
    std::vector<PageStats> stats(count);
    {
        std::vector<std::unique_ptr<Text::FontCache>> caches;  // workers 1 on; worker 0 uses `fonts`
        caches.reserve(pool.Workers() - 1);
        for (unsigned worker = 1; worker < pool.Workers(); ++worker) caches.emplace_back(new Text::FontCache(fonts));
        pool.Run(count, [&](unsigned worker, size_t slot) {
            if (!document.SharedIntact()) return;  // read again by the caller
            Text::FontCache& cache = worker == 0 ? fonts : *caches[worker - 1];
            InterpretPage(document, cache, first + static_cast<int>(slot), links, pages[slot], stats[slot]);
        });
    }
    if (!document.EndShared()) {
        fonts.Clear();
        return false;
    }
    for (size_t slot = 0; slot < count; ++slot) Tally(stats[slot], quality[slot], report);
    return true;
}

// The pages of `document` to Markdown (D4 steps 2-8).
void ConvertDocument(const Document& document, const ConvertOptions& options, std::string& markdown, ConvertReport& report) {
    markdown.clear();
    report = ConvertReport();
    const int count = document.PageCount();
    const int first = std::max(1, options.firstPage);
    const int last = options.lastPage <= 0 ? count : std::min(count, options.lastPage);
    std::vector<PageData> pages;
    std::vector<PageTextQuality> quality;
    if (first <= last) {
        pages.resize(static_cast<size_t>(last - first + 1));
        quality.resize(pages.size());
    }
    const unsigned workers = PageWorkers(options.pageWorkers, pages.size());
    std::unique_ptr<PagePool> pool;  // one document's threads, from the first page-local phase to the last
    if (workers > 1) pool.reset(new PagePool(workers));
    Text::FontCache fonts(document);
    if (!pool || !InterpretShared(document, fonts, options.annotations, first - 1, pages, quality, report, *pool)) {
        for (int index = first - 1; index < last; ++index) {
            const size_t slot = static_cast<size_t>(index - (first - 1));
            PageStats stats;
            InterpretPage(document, fonts, index, options.annotations, pages[slot], stats);
            Tally(stats, quality[slot], report);
        }
    }
    // The pages' content is read: its buffer (this thread's) goes back to the heap for what follows.
    if (options.oneShot) Document::ReleasePageContent();
    report.pages = static_cast<int>(pages.size());
    RecognisePages(document, fonts, options.ocr, pages, quality, report, !options.imageDirectory.empty());
    std::vector<OutlineEntry> outline;
    ReadOutline(document, outline);
    ImageOutput images;
    images.document = &document;
    images.directory = options.imageDirectory;
    images.linkPrefix = options.imageLinkPrefix;
    Structure structure;
    if (options.structure && first <= last) ReadStructure(document, first - 1, last - 1, structure);
    ConvertPages(pages, quality, outline, document.Report().repaired, markdown, report, &images, &structure, options.math,
                 pool.get());
}

} // namespace

void Convert(const Document& document, const ConvertOptions& options, std::string& markdown, ConvertReport& report) {
    std::string_view source;
    ExactUse use = options.exact ? FindExactSource(document, source) : ExactUse::Off;
    if (use == ExactUse::Used) {
        const int count = document.PageCount();
        if (options.firstPage > 1 || (options.lastPage > 0 && options.lastPage < count)) use = ExactUse::Pages;
    }
    if (use == ExactUse::Used) {
        // The check converts the pages without writing images or running OCR, with Unicode math;
        // when the caller asked for none of these, that conversion is also the fallback.
        ConvertOptions check = options;
        check.imageDirectory.clear();
        check.imageLinkPrefix.clear();
        check.ocr.mode = OcrMode::Off;
        check.math = MathMode::Unicode;
        ConvertDocument(document, check, markdown, report);
        ExactMatch match;
        if (SourceMatchesPages(source, markdown, match)) {
            markdown.assign(source.data(), source.size());
            report.exact = ExactUse::Used;
            return;
        }
        use = ExactUse::Mismatch;
        if (options.imageDirectory.empty() && options.ocr.mode == OcrMode::Off && options.math == MathMode::Unicode) {
            report.exact = use;
            return;
        }
    }
    ConvertDocument(document, options, markdown, report);
    report.exact = use;
}

} // namespace RayoPdf::Layout
