// SPDX-License-Identifier: Apache-2.0
// OCR where the text layer fails (docs/development/ocr-protocol.md, docs/development/ocr_ml_decision.md):
// between interpretation and the layout stages, pages are routed (layout/ocr_route.h), the add-on
// is found and started only when a page needs it (layout/ocr_client.h), routed pages and regions
// are rendered from their images (layout/ocr_layer.h) and recognised, and the answers become an
// OCR text layer that the usual stages read. Fonts whose text is missing or wrong get it from their
// glyphs' shapes, and text drawn as vector outlines is rendered and recognised
// (layout/ocr_glyphs.h). Without an add-on nothing changes but the report: a page with no text
// keeps its "no extractable text" comment. When the add-on fails (it crashes, hangs past its
// timeout or answers garbage), it is killed, no further page of the document is sent, and each
// routed page left without text says so in its comment. The output depends only on the document,
// the options and the add-on's answers, never on timing or the number of workers.
#pragma once

#include "layout/convert.h"
#include "layout/ocr_options.h"
#include "layout/page.h"
#include "layout/pipeline.h"
#include "pdf/document.h"
#include "text/font.h"

#include <vector>

namespace RayoPdf::Layout {

// Routes and recognises `pages` (interpreted with `fonts`; `quality` per page, as Convert fills it)
// and adds the OCR counters to `report`: ocrPagesRouted, ocrPages, ocrFailures, ocrAddonMissing,
// ocrBrokenPages, ocrVectorTextPages, ocrGlyphFonts, ocrGlyphs. Sets quality[p].ocrFailed for
// routed pages left without text by a failing add-on. Does nothing with OcrMode::Off.
//
// Text read in an image region of a page with a text layer goes where the reader of the Markdown
// meets the image: with `imagesWritten`, it is the image's alternate text (ImageBox::ocr, used
// when the image has no /Alt), so the figure's words stay with the figure instead of entering the
// body; without images it is body text at the image's place (an OCR text layer).
void RecognisePages(const Document& document, Text::FontCache& fonts, const OcrOptions& options,
                    std::vector<PageData>& pages, std::vector<PageTextQuality>& quality, ConvertReport& report,
                    bool imagesWritten);

} // namespace RayoPdf::Layout
