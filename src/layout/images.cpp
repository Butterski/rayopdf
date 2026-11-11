// SPDX-License-Identifier: Apache-2.0
#include "layout/images.h"

#include "common/batch.h"
#include "common/file.h"
#include "common/minmax.h"
#include "image/image.h"
#include "layout/page_pool.h"
#include "pdf/content_lexer.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace RayoPdf::Layout {
namespace {

constexpr float kMinAreaShare = 0.01f;     // smaller images are decoration (bullets, rules, icons)
constexpr uint32_t kMinPixels = 24;        // ... and so are images this narrow or low
constexpr float kBackgroundShare = 0.9f;   // an image filling the page behind text is its background
constexpr float kSamePlace = 2.0f;         // points: a logo comes back at the same place
constexpr uint32_t kMaxImagesPerPage = 512; // placements written per page; more are skipped

// When the images of a document are made on threads. A thread is not free (its start, and the
// first touch of its buffers), so the images must be worth it: a thread for every
// kImageThreadPixels pixels of the images to write, as long as there are images left for them.
// A count given with --workers starts its threads at once.
constexpr uint64_t kImageThreadPixels = uint64_t(1) << 18;

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
    case Image::Status::WriteFailed: return "the file cannot be written";
    case Image::Status::Ok: break;
    }
    return "unknown";
}

// The image directory, made when the first file is to be written: once, by whichever thread
// comes first.
class ImageDirectory {
public:
    explicit ImageDirectory(const std::string& directory) : directory_(directory), prefix_(directory) {
        if (!prefix_.empty() && prefix_.back() != '/') prefix_.push_back('/');
    }

    // True when the directory is there.
    bool Ensure() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ready_ && !failed_) {
            ready_ = File::EnsureDirectory(directory_);
            failed_ = !ready_;
        }
        return ready_;
    }
    // It could not be made.
    bool Failed() {
        std::lock_guard<std::mutex> lock(mutex_);
        return failed_;
    }
    // As before the first Ensure, for a pass over the images that is made again; with `remove`
    // the directory, which that pass made, goes when it is empty.
    void Forget(bool remove) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ready_ && remove) File::RemoveEmptyDirectory(directory_);
        ready_ = failed_ = false;
    }
    // The path of the file `name` in it.
    std::string Path(std::string_view name) const {
        std::string path = prefix_;
        path += name;
        return path;
    }

private:
    const std::string& directory_;
    std::string prefix_;
    std::mutex mutex_;
    bool ready_ = false;
    bool failed_ = false;
};

// Every image is made under this name (File::StagedFile puts a suffix of its own after it) and
// renamed to p<page>-<n>.<ext> once the images before it are counted.
constexpr std::string_view kStagedName = "image";

// One image file in the making: encoded, on any thread, into a temporary file in the image
// directory, which PlaceImages names in page order.
struct ImageJob {
    uint32_t page = 0;  // pages[page].images[image]
    uint32_t image = 0;
    const char* failure = nullptr;  // once encoded: why there is no file
    const char* extension = "png";
    bool maskDropped = false;
    File::StagedFile file;  // once encoded: closed, without its name
};

// The file a PNG is made into while it is encoded (Image::Sink), opened when the encoder has
// its first byte (the image directory made then), so that no image-sized copy of the file is held.
class ImageFile final : public Image::Sink {
public:
    ImageFile(ImageDirectory& directory, File::StagedFile& file) : directory_(directory), file_(file) {}

    bool Open(const char*) override { return directory_.Ensure() && file_.Open(directory_.Path(kStagedName)); }
    bool Append(const uint8_t* data, size_t size) override {
        return file_.Write(std::string_view(reinterpret_cast<const char*>(data), size));
    }
    bool Patch(uint64_t offset, const uint8_t* data, size_t size) override {
        return file_.WriteAt(offset, std::string_view(reinterpret_cast<const char*>(data), size));
    }

private:
    ImageDirectory& directory_;
    File::StagedFile& file_;
};

Image::Status Encode(const Document& document, const PageData& page, const ImageBox& image, Image::Encoded& out,
                     Image::Sink* sink, Image::Parallel parallel) {
    if (!image.xobject.IsNull()) {
        return Image::EncodeXObject(document, image.xobject, image.resources, out, Image::kDefaultMaxPixels, sink,
                                    parallel);
    }
    // An inline image: its source, BI to EI, read again.
    ContentLexer lexer(std::string_view(page.inlineImages).substr(image.inlineData, image.inlineLength));
    std::string_view op;
    std::vector<PdfValue> operands;
    if (!lexer.Next(op, operands) || op != "BI" || operands.empty()) return Image::Status::Corrupt;
    return Image::EncodeInline(document, operands.back(), lexer.InlineImageData(), image.resources, out,
                               Image::kDefaultMaxPixels, sink, parallel);
}

// Makes the file of `job` (its page and image set), without its name: a PNG goes into the file
// while it is made; a JPEG or JPEG 2000 passed through is written whole. `encoded` is the
// calling thread's buffer.
void EncodeJob(const Document& document, const std::vector<PageData>& pages, ImageDirectory& directory,
               Image::Encoded& encoded, Image::Parallel parallel, ImageJob& job) {
    const PageData& page = pages[job.page];
    ImageFile sink(directory, job.file);
    const Image::Status status = Encode(document, page, page.images[job.image], encoded, &sink, parallel);
    job.failure = nullptr;
    if (status == Image::Status::WriteFailed && directory.Failed()) {
        job.failure = "the image directory cannot be created";
    } else if (status != Image::Status::Ok) {
        job.failure = Reason(status);
    } else if (encoded.streamed) {
        if (!job.file.Close()) job.failure = "the file cannot be written";
    } else if (!directory.Ensure()) {
        job.failure = "the image directory cannot be created";
    } else if (!job.file.Open(directory.Path(kStagedName)) || !job.file.Write(encoded.bytes) || !job.file.Close()) {
        job.failure = "the file cannot be written";
    }
    if (job.failure) job.file.Discard();
    job.extension = encoded.extension;
    job.maskDropped = encoded.maskDropped;
}

// The images PlaceImages writes, in the order it writes them: the first placement of every
// image that is not skipped. Returns their pixels.
uint64_t ListJobs(const std::vector<PageData>& pages, std::vector<std::pair<uint32_t, uint32_t>>& list) {
    std::unordered_set<uint64_t> seen;
    uint64_t pixels = 0;
    for (uint32_t p = 0; p < pages.size(); ++p) {
        const PageData& page = pages[p];
        for (uint32_t i = 0; i < page.images.size(); ++i) {
            const ImageBox& image = page.images[i];
            if (image.state == ImageState::Skipped || !seen.insert(image.identity).second) continue;
            list.emplace_back(p, i);
            pixels += uint64_t(image.width) * image.height;
        }
    }
    return pixels;
}

// The files of `jobs` made on the threads of `pool`, the document shared between them
// (Document::BeginShared). False when the document cannot be shared, or says that what the
// threads read might depend on their order: nothing is left of the pass then, and the caller
// makes the files one after another.
bool EncodeShared(const Document& document, const std::vector<PageData>& pages, const ImageOutput& output,
                  ImageDirectory& directory, PagePool& pool, ImageJob* jobs, size_t count) {
    File::FileInfo before;
    const bool existed = File::StatusOf(output.directory, before);
    if (!document.BeginShared()) return false;
    {
        std::vector<Image::Encoded> encoded(pool.Workers());
        pool.Run(count, [&](unsigned worker, size_t index) {
            if (!document.SharedIntact()) return;  // made again by the caller
            EncodeJob(document, pages, directory, encoded[worker], Image::Parallel{&pool, worker}, jobs[index]);
        });
    }
    if (document.EndShared()) return true;
    for (size_t index = 0; index < count; ++index) jobs[index].file.Discard();
    directory.Forget(!existed);
    return false;
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
    line.u0 = Fmin(line.u0, image.x0), line.u1 = Fmax(line.u1, image.x1);
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
            // Text read in it by OCR (layout/ocr.h) makes it content with an anchor of its own: the
            // text is its alternate text.
            if (image.ocrLength > 0) image.line = -1, small = false;
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

    // Threads: the conversion's pool, or a pool of this pass (a document of one page has none),
    // when the images are worth a thread or a thread count was asked for.
    const Document& document = *output.document;
    ImageDirectory directory(output.directory);
    PagePool* pool = nullptr;
    std::unique_ptr<PagePool> own;
    std::unique_ptr<ImageJob[]> jobs;  // the files made on threads, in page order
    if (output.pageWorkers != 1) {
        const bool asked = output.pageWorkers != 0;
        uint64_t pixels = 0;  // of every image kept: no fewer than the files have
        bool any = false;
        for (const PageData& page : pages) {
            for (const ImageBox& image : page.images) {
                if (image.state == ImageState::Skipped) continue;
                any = true;
                pixels += uint64_t(image.width) * image.height;
            }
        }
        if (any && (asked || pixels >= kImageThreadPixels)) {
            std::vector<std::pair<uint32_t, uint32_t>> list;
            pixels = ListJobs(pages, list);
            if (asked || pixels >= kImageThreadPixels) {
                pool = output.pool;
                if (!pool) {
                    const unsigned workers = asked ? Batch::WorkerCount(output.pageWorkers, SIZE_MAX)
                                                   : std::min(Batch::UsableCpus(), kDefaultPageWorkers);
                    if (workers > 1) {
                        own.reset(new PagePool(workers));
                        pool = own.get();
                    }
                }
            }
            if (pool) {
                // Threads for whole images here; an image starts more for its own parts (image.h).
                const uint64_t threads = asked ? pool->Workers() - 1
                                               : std::min<uint64_t>(list.size() - 1, pixels / kImageThreadPixels);
                pool->Start(static_cast<unsigned>(std::min<uint64_t>(threads, pool->Workers() - 1)));
                if (list.size() >= 2 && pool->Started() > 0) {
                    jobs.reset(new ImageJob[list.size()]);
                    for (size_t k = 0; k < list.size(); ++k) {
                        jobs[k].page = list[k].first;
                        jobs[k].image = list[k].second;
                    }
                    if (!EncodeShared(document, pages, output, directory, *pool, jobs.get(), list.size())) jobs.reset();
                }
            }
        }
    }

    // Files: one per distinct image, named after the page that first shows it.
    struct Written {
        uint32_t page;
        uint32_t image;
    };
    std::unordered_map<uint64_t, Written> files;
    Image::Encoded encoded;
    ImageJob single;  // the file in the making, when they are made one after another
    size_t next = 0;  // in `jobs`
    std::string name;
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
            ImageJob* job = &single;
            if (jobs) {
                job = &jobs[next++];
            } else {
                single.page = p;
                single.image = i;
                EncodeJob(document, pages, directory, encoded, Image::Parallel{pool, 0}, single);
            }
            if (!job->failure) {
                name = "p" + std::to_string(page.number) + "-" + std::to_string(count + 1) + "." + job->extension;
                if (!job->file.CommitAs(directory.Path(name))) job->failure = "the file cannot be written";
            }
            if (job->failure) {
                report.imagesSkipped++;
                SetName(page, image, ImageState::Failed, job->failure);
            } else {
                report.images++;
                if (job->maskDropped) report.imagesMaskDropped++;
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

Image::Status ImageGray(const Document& document, const PageData& page, const ImageBox& image, Image::GrayImage& out,
                        uint32_t maxSide) {
    if (!image.xobject.IsNull()) return Image::GrayXObject(document, image.xobject, image.resources, out, maxSide);
    out = Image::GrayImage();
    if (uint64_t(image.inlineData) + image.inlineLength > page.inlineImages.size()) return Image::Status::Corrupt;
    ContentLexer lexer(std::string_view(page.inlineImages).substr(image.inlineData, image.inlineLength));
    std::string_view op;
    std::vector<PdfValue> operands;
    if (!lexer.Next(op, operands) || op != "BI" || operands.empty()) return Image::Status::Corrupt;
    return Image::GrayInline(document, operands.back(), lexer.InlineImageData(), image.resources, out, maxSide);
}

} // namespace RayoPdf::Layout
