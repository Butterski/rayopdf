// SPDX-License-Identifier: Apache-2.0
#include "layout/ocr.h"

#include "layout/ocr_client.h"
#include "layout/ocr_glyphs.h"
#include "layout/ocr_layer.h"
#include "layout/ocr_route.h"

#include <memory>

namespace RayoPdf::Layout {

namespace {

// One canvas to the add-on and its lines into `page`: the non-space glyphs added, or the failure.
OcrStatus Recognise(OcrSession& session, const OcrCanvas& canvas, PageData& page, uint32_t& added) {
    OcrProtocol::Request request;
    request.op = OcrProtocol::Op::Page;
    request.width = canvas.width;
    request.height = canvas.height;
    request.dpi = static_cast<uint32_t>(canvas.scale * 72.0f + 0.5f);
    OcrProtocol::Response response;
    const OcrStatus status = session.Recognize(request, canvas.pixels, response);
    added = status == OcrStatus::Ok ? AddOcrGlyphs(canvas, response, page) : 0;
    return status;
}

// One region to the add-on, its text kept as the image's alternate text (ImageBox::ocr): the
// bytes kept, or the failure.
OcrStatus RecogniseAlt(OcrSession& session, const OcrCanvas& canvas, PageData& page, ImageBox& image, size_t& kept) {
    OcrProtocol::Request request;
    request.op = OcrProtocol::Op::Page;
    request.width = canvas.width;
    request.height = canvas.height;
    request.dpi = static_cast<uint32_t>(canvas.scale * 72.0f + 0.5f);
    OcrProtocol::Response response;
    const OcrStatus status = session.Recognize(request, canvas.pixels, response);
    kept = 0;
    if (status != OcrStatus::Ok || page.text.size() > (size_t(1) << 30)) return status;
    const size_t at = page.text.size();
    kept = AppendOcrAltText(response, page.text);
    image.ocr = static_cast<uint32_t>(at);
    image.ocrLength = static_cast<uint32_t>(kept);
    return status;
}

} // namespace

void RoutePage(const PageData& page, const PageTextQuality& quality, OcrMode mode, PageRoute& route) {
    PageSignals signals;
    MeasurePage(page, quality.glyphs, quality.unreliable, signals);
    route.broken = signals.broken && signals.visibleChars + signals.invisibleChars >= kPageMinChars;
    route.vectorText = signals.vectorText;
    PlanPage(page, signals, mode, route.plan);
    route.glyphShapes = signals.broken || signals.junk >= kGlyphRecoveryMinJunk;
}

void RecognisePages(const Document& document, Text::FontCache& fonts, const OcrOptions& options,
                    std::vector<PageData>& pages, std::vector<PageTextQuality>& quality,
                    const std::vector<PageRoute>& routes, ConvertReport& report, bool imagesWritten) {
    if (options.mode == OcrMode::Off) return;
    std::vector<uint32_t> glyphPages, vectorPages;
    bool any = false;
    for (size_t p = 0; p < pages.size(); ++p) {
        const PageRoute& route = routes[p];
        if (route.broken) report.ocrBrokenPages++;
        if (route.vectorText) report.ocrVectorTextPages++;
        any = any || route.plan.route != OcrRoute::None;
        if (route.plan.route == OcrRoute::Page) continue;
        if (route.glyphShapes) glyphPages.push_back(static_cast<uint32_t>(p));
        if (route.vectorText) vectorPages.push_back(static_cast<uint32_t>(p));
    }
    if (!any && glyphPages.empty() && vectorPages.empty()) return;

    std::unique_ptr<OcrSession> own;
    OcrSession* session = options.session;
    if (!session) {
        own.reset(new OcrSession(options));
        session = own.get();
    }
    bool unavailable = false;  // nothing more goes to the add-on in this document
    bool failed = false;       // ... because it failed
    // Glyph shapes and vector text, when an add-on runs: without one nothing changes, not even the
    // report (a missing or broken add-on is reported by the routed pages below).
    if ((!glyphPages.empty() || !vectorPages.empty()) && session->Ensure() == OcrStatus::Ok) {
        GlyphRecoveryStats glyphs;
        const OcrStatus status = RecoverGlyphShapes(document, fonts, *session, glyphPages, pages, quality, glyphs);
        report.ocrGlyphFonts += static_cast<int>(glyphs.fontsAccepted);
        report.ocrGlyphs += glyphs.usesReplaced;
        if (status != OcrStatus::Ok && status != OcrStatus::Refused) unavailable = failed = true;
        for (uint32_t p : vectorPages) {
            report.ocrPagesRouted++;
            if (unavailable) {
                report.ocrFailures++;
                continue;
            }
            uint32_t added = 0;
            const OcrStatus vector = RecogniseVectorPage(document, *session, pages[p], added);
            if (vector != OcrStatus::Ok) {
                report.ocrFailures++;
                if (vector != OcrStatus::Refused) unavailable = failed = true;
                continue;
            }
            if (added > 0) report.ocrPages++;
        }
    }
    if (!any) return;
    OcrCanvas canvas;
    for (size_t p = 0; p < pages.size(); ++p) {
        const OcrPlan& plan = routes[p].plan;
        if (plan.route == OcrRoute::None) continue;
        PageData& page = pages[p];
        const bool whole = plan.route == OcrRoute::Page;
        // Regions waiting for the text check are not routed until their pixels pass it.
        if (whole || !plan.checkText) report.ocrPagesRouted++;
        const auto pageFailed = [&] {
            report.ocrFailures++;
            if (whole && p < quality.size()) quality[p].ocrFailed = true;
        };
        if (unavailable) {
            if (failed && (whole || !plan.checkText)) pageFailed();
            continue;
        }
        const OcrStatus ready = session->Ensure();
        if (ready != OcrStatus::Ok) {
            unavailable = true;
            if (ready == OcrStatus::Missing) {
                if (whole || !plan.checkText) report.ocrAddonMissing = true;
            } else {
                failed = true;
                if (whole || !plan.checkText) pageFailed();
            }
            continue;
        }

        if (whole) {
            if (ComposeImages(document, page, plan.images, 0, 0, page.width, page.height, canvas) == 0) continue;
            const size_t before = page.glyphs.size();
            uint32_t added = 0;
            const OcrStatus status = Recognise(*session, canvas, page, added);
            if (status != OcrStatus::Ok) {
                pageFailed();
                if (status != OcrStatus::Refused) unavailable = failed = true;
                continue;
            }
            if (added == 0) continue;
            report.ocrPages++;
            if (p < quality.size()) quality[p].unreliable = 0;  // the OCR text is the page's text now
            if (plan.replaceInvisible) {
                for (size_t g = 0; g < before; ++g)
                    if (page.glyphs[g].flags & kGlyphInvisible) page.glyphs[g].flags |= kGlyphDropped;
            }
            continue;
        }

        bool routed = !plan.checkText, recognised = false, regionFailed = false;
        for (uint32_t index : plan.images) {
            const ImageBox& image = page.images[index];
            if (ComposeImages(document, page, {index}, image.x0, image.y0, image.x1, image.y1, canvas) == 0) continue;
            if (plan.checkText &&
                !LooksLikeText(reinterpret_cast<const uint8_t*>(canvas.pixels.data()), canvas.width, canvas.height))
                continue;
            if (!routed) {
                routed = true;
                report.ocrPagesRouted++;
            }
            uint32_t added = 0;
            size_t kept = 0;
            // A figure's text on a digital page is its alternate text; on a page without a text
            // layer (a scan with margins) the region's text is the page's.
            const OcrStatus status = imagesWritten && plan.textLayer
                                         ? RecogniseAlt(*session, canvas, page, page.images[index], kept)
                                         : Recognise(*session, canvas, page, added);
            if (status != OcrStatus::Ok) {
                regionFailed = true;
                if (status != OcrStatus::Refused) {
                    unavailable = failed = true;
                    break;
                }
                continue;
            }
            recognised = recognised || added > 0 || kept > 0;
        }
        if (recognised) report.ocrPages++;
        if (regionFailed) report.ocrFailures++;
    }
}

} // namespace RayoPdf::Layout
