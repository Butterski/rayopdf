// SPDX-License-Identifier: Apache-2.0
#include "layout/convert.h"

#include "layout/interpreter.h"
#include "layout/pipeline.h"
#include "layout/structure.h"
#include "text/font.h"

#include <algorithm>
#include <vector>

namespace RayoPdf::Layout {

void Convert(const Document& document, const ConvertOptions& options, std::string& markdown, ConvertReport& report) {
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
    Text::FontCache fonts(document);
    for (int index = first - 1; index < last; ++index) {
        const size_t slot = static_cast<size_t>(index - (first - 1));
        PageStats stats;
        InterpretPage(document, fonts, index, options.annotations, pages[slot], stats);
        report.glyphs += stats.glyphs;
        report.unmappedGlyphs += stats.unmapped;
        report.guessedGlyphs += stats.guessed;
        quality[slot].glyphs = stats.glyphs;
        quality[slot].unreliable = stats.unmapped + stats.guessed;
    }
    report.pages = static_cast<int>(pages.size());
    std::vector<OutlineEntry> outline;
    ReadOutline(document, outline);
    ImageOutput images;
    images.document = &document;
    images.directory = options.imageDirectory;
    images.linkPrefix = options.imageLinkPrefix;
    Structure structure;
    if (options.structure && first <= last) ReadStructure(document, first - 1, last - 1, structure);
    ConvertPages(pages, quality, outline, document.Report().repaired, markdown, report, &images, &structure);
}

} // namespace RayoPdf::Layout
