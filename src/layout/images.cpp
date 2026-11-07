// SPDX-License-Identifier: Apache-2.0
#include "layout/images.h"

#include "common/file.h"
#include "image/image.h"
#include "pdf/content_lexer.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace RayoPdf::Layout {
namespace {

constexpr float kMinAreaShare = 0.01f;     // smaller images are decoration (bullets, rules, icons)
constexpr uint32_t kMinPixels = 24;        // ... and so are images this narrow or low
constexpr float kBackgroundShare = 0.9f;   // an image filling the page behind text is its background
constexpr float kSamePlace = 2.0f;         // points: a logo comes back at the same place
constexpr uint32_t kMaxImagesPerPage = 512; // placements written per page; more are skipped

float Area(const ImageBox& b) { return (b.x1 - b.x0) * (b.y1 - b.y0); }

bool SamePlace(const ImageBox& a, const ImageBox& b) {
    return std::fabs(a.x0 - b.x0) <= kSamePlace && std::fabs(a.y0 - b.y0) <= kSamePlace &&
           std::fabs(a.x1 - b.x1) <= kSamePlace && std::fabs(a.y1 - b.y1) <= kSamePlace;
}

const char* Reason(Image::Status status) {
    switch (status) {
    case Image::Status::Unsupported: return "unsupported encoding";
    case Image::Status::Corrupt: return "corrupt image data";
    case Image::Status::TooLarge: return "too large";
    case Image::Status::Ok: break;
    }
    return "unknown";
}

Image::Status Encode(const Document& document, const PageData& page, const ImageBox& image, Image::Encoded& out) {
    if (!image.xobject.IsNull()) return Image::EncodeXObject(document, image.xobject, image.resources, out);
    // An inline image: its source, BI to EI, read again.
    ContentLexer lexer(std::string_view(page.inlineImages).substr(image.inlineData, image.inlineLength));
    std::string_view op;
    std::vector<PdfValue> operands;
    if (!lexer.Next(op, operands) || op != "BI" || operands.empty()) return Image::Status::Corrupt;
    return Image::EncodeInline(document, operands.back(), lexer.InlineImageData(), image.resources, out);
}

void SetName(PageData& page, ImageBox& image, ImageState state, std::string_view name) {
    image.state = state;
    image.name = static_cast<uint32_t>(page.text.size());
    image.nameLength = static_cast<uint32_t>(name.size());
    page.text.append(name);
}


constexpr uint32_t kMaxInlineChecks = 256;  // small images per page tested for a place in a line

// The text line an image sits in, as an inline image: its bottom on the line's baseline (within
// half an em), no taller than a few lines, and text within an em and a half of it on that line,
// before or after it. -1 when none.
int32_t InlineLine(const PageData& page, const ImageBox& image) {
    for (uint32_t li = 0; li < page.lines.size(); ++li) {
        const Line& line = page.lines[li];
        if (line.dir != 0 || line.count == 0 || (line.flags & kLineRemoved)) continue;
        const float em = line.size;
        if (std::fabs(image.y0 - line.baseline) > 0.5f * em || image.y1 - image.y0 > 10 * em) continue;
        if (image.x1 < line.u0 - 1.5f * em || image.x0 > line.u1 + 1.5f * em) continue;
        for (uint32_t k = 0; k < line.count; ++k) {
            const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
            if ((g.x1 <= image.x0 + 0.5f && image.x0 - g.x1 <= 1.5f * em) ||
                (g.x0 >= image.x1 - 0.5f && g.x0 - image.x1 <= 1.5f * em))
                return static_cast<int32_t>(li);
        }
    }
    return -1;
}

// Sets written inline image `index` into its line as a glyph whose text is its Markdown link
// (kGlyphImage). The line's glyphs move to the end of lineGlyphs with the image among them.
bool InsertInline(PageData& page, uint32_t index, std::string& link) {
    const ImageBox& image = page.images[index];
    const std::string_view target = page.ImageName(image);
    const bool angle = target.find_first_of(" ()<>") != std::string_view::npos;
    link = "![](";
    if (angle) link.push_back('<');
    for (char c : target) {
        if (angle && (c == '<' || c == '>'))
            link += c == '<' ? "%3C" : "%3E";
        else
            link.push_back(c);
    }
    if (angle) link.push_back('>');
    link.push_back(')');
    if (link.size() > UINT16_MAX || page.text.size() > (size_t(1) << 30)) return false;
    Line& line = page.lines[static_cast<size_t>(image.line)];
    Glyph g;
    g.x0 = image.x0, g.x1 = image.x1, g.y0 = image.y0, g.y1 = image.y1;
    g.u0 = image.x0, g.u1 = image.x1, g.vb = line.vb, g.vt = line.vt, g.v = line.baseline;
    g.size = line.size;
    g.text = static_cast<uint32_t>(page.text.size());
    g.textLength = static_cast<uint16_t>(link.size());
    g.flags = static_cast<uint16_t>(kGlyphImage | kGlyphSpaceBefore);
    page.text += link;
    const uint32_t glyph = static_cast<uint32_t>(page.glyphs.size());
    page.glyphs.push_back(g);
    const uint32_t first = static_cast<uint32_t>(page.lineGlyphs.size());
    const float middle = (image.x0 + image.x1) / 2;
    bool placed = false;
    for (uint32_t k = 0; k < line.count; ++k) {
        const uint32_t gi = page.lineGlyphs[line.first + k];
        if (!placed && page.glyphs[gi].u0 >= middle) {
            page.lineGlyphs.push_back(glyph);
            page.glyphs[gi].flags |= kGlyphSpaceBefore;
            placed = true;
        }
        page.lineGlyphs.push_back(gi);
    }
    if (!placed) page.lineGlyphs.push_back(glyph);
    page.glyphs[page.lineGlyphs[first]].flags &= static_cast<uint16_t>(~kGlyphSpaceBefore);
    line.first = first;
    line.count++;
    line.u0 = std::fmin(line.u0, image.x0), line.u1 = std::fmax(line.u1, image.x1);
    return true;
}

} // namespace

void PlaceImages(std::vector<PageData>& pages, const ImageOutput& output, ConvertReport& report) {
    if (!output.document || output.directory.empty()) return;
    const size_t n = pages.size();

    // Decoration: small images and page backgrounds behind text.
    for (PageData& page : pages) {
        const float pageArea = page.width * page.height;
        bool text = false;
        for (const Line& line : page.lines)
            if (line.count > 0 && !(line.flags & kLineRemoved)) text = true;
        uint32_t kept = 0, inlineChecks = 0;
        for (size_t i = 0; i < page.images.size(); ++i) {
            ImageBox& image = page.images[i];
            const bool tooFewPixels = (image.width > 0 && image.width < kMinPixels) ||
                                      (image.height > 0 && image.height < kMinPixels);
            bool small = Area(image) < kMinAreaShare * pageArea || tooFewPixels;
            // An image set in a line of text (an icon in a sentence) is content, however small.
            if (small && !tooFewPixels && inlineChecks < kMaxInlineChecks) {
                inlineChecks++;
                image.line = InlineLine(page, image);
                if (image.line >= 0) small = false;
            }
            const bool background = text && Area(image) >= kBackgroundShare * pageArea;
            // The same image drawn again at the same place on the page adds nothing.
            bool again = false;
            for (size_t j = 0; j < i && !again && j < 256; ++j)
                again = page.images[j].identity == image.identity && SamePlace(page.images[j], image);
            if (small || background || again || kept >= kMaxImagesPerPage)
                image.state = ImageState::Skipped;
            else
                kept++;
        }
    }
    // Logos: an image at the same place on at least half the pages (at least two).
    if (n >= 2) {
        struct Seen {
            const ImageBox* first;
            uint32_t pages;
            int lastPage;
        };
        std::unordered_map<uint64_t, Seen> seen;
        for (size_t p = 0; p < n; ++p) {
            for (const ImageBox& image : pages[p].images) {
                if (image.state != ImageState::Found) continue;
                auto it = seen.find(image.identity);
                if (it == seen.end()) {
                    seen.emplace(image.identity, Seen{&image, 1, static_cast<int>(p)});
                } else if (it->second.lastPage != static_cast<int>(p) && SamePlace(*it->second.first, image)) {
                    it->second.pages++;
                    it->second.lastPage = static_cast<int>(p);
                }
            }
        }
        const uint32_t threshold = static_cast<uint32_t>(std::max<size_t>(2, (n + 1) / 2));
        for (PageData& page : pages)
            for (ImageBox& image : page.images) {
                if (image.state != ImageState::Found) continue;
                const auto it = seen.find(image.identity);
                if (it != seen.end() && it->second.pages >= threshold && SamePlace(*it->second.first, image))
                    image.state = ImageState::Skipped;
            }
    }

    // Files: one per distinct image, named after the page that first shows it.
    struct Written {
        uint32_t page;
        uint32_t image;
    };
    std::unordered_map<uint64_t, Written> files;
    bool directoryReady = false, directoryFailed = false;
    Image::Encoded encoded;
    std::string name, path;
    for (uint32_t p = 0; p < n; ++p) {
        PageData& page = pages[p];
        int count = 0;
        for (uint32_t i = 0; i < page.images.size(); ++i) {
            ImageBox& image = page.images[i];
            if (image.state == ImageState::Skipped) {
                report.imagesSkipped++;
                continue;
            }
            const auto it = files.find(image.identity);
            if (it != files.end()) {
                const PageData& from = pages[it->second.page];
                const ImageBox& written = from.images[it->second.image];
                SetName(page, image, written.state, from.ImageName(written));
                continue;
            }
            const Image::Status status = Encode(*output.document, page, image, encoded);
            const char* failure = nullptr;
            if (status != Image::Status::Ok) {
                failure = Reason(status);
            } else {
                if (!directoryReady && !directoryFailed) {
                    directoryReady = File::EnsureDirectory(output.directory);
                    directoryFailed = !directoryReady;
                }
                name = "p" + std::to_string(page.number) + "-" + std::to_string(count + 1) + "." + encoded.extension;
                path = output.directory;
                if (!path.empty() && path.back() != '/') path.push_back('/');
                path += name;
                if (directoryFailed)
                    failure = "the image directory cannot be created";
                else if (!File::WriteAll(path, encoded.bytes))
                    failure = "the file cannot be written";
            }
            if (failure) {
                report.imagesSkipped++;
                SetName(page, image, ImageState::Failed, failure);
            } else {
                report.images++;
                if (encoded.maskDropped) report.imagesMaskDropped++;
                count++;
                SetName(page, image, ImageState::Written, output.linkPrefix + name);
            }
            files.emplace(image.identity, Written{p, i});
        }
    }

    // Anchors: one line per placement, ordered with the text; an inline image becomes a glyph of
    // its line instead, at its place along the baseline.
    std::string link;
    for (PageData& page : pages) {
        for (uint32_t i = 0; i < page.images.size(); ++i) {
            const ImageBox& image = page.images[i];
            if (image.state != ImageState::Written && image.state != ImageState::Failed) continue;
            if (image.state == ImageState::Written && image.line >= 0 && InsertInline(page, i, link)) continue;
            Line anchor;
            anchor.u0 = image.x0, anchor.u1 = image.x1, anchor.vb = image.y0, anchor.vt = image.y1;
            anchor.baseline = image.y0;
            anchor.first = i;
            anchor.flags = kLineImage;
            page.lines.push_back(anchor);
        }
    }
}

} // namespace RayoPdf::Layout
