// SPDX-License-Identifier: Apache-2.0
// Images as files (roadmap D6, algorithms §6.9): which images of the pages are content, writing
// them, and their places in reading order. Decoration stays out: images under 1 % of the page
// area or under 24 pixels on a side, page-filling backgrounds behind text, and an image repeated
// at the same place on at least half the pages (a logo). A kept image is written once, however
// often it is drawn, as p<page>-<n>.<ext> (the page of its first placement, n counting that page's
// files), and each placement gets an anchor line (kLineImage) that OrderLines orders with the text.
//
// The images are decoded and encoded on the conversion's threads when they are worth it (roadmap
// D4): each into a file of its own without a name yet, and the files are named, and counted, in
// page order afterwards, so the files, their names and the report are those of one thread.
#pragma once

#include "image/image.h"
#include "layout/convert.h"
#include "layout/page.h"
#include "pdf/document.h"

#include <string>
#include <vector>

namespace RayoPdf::Layout {

class PagePool;

struct ImageOutput {
    const Document* document = nullptr;
    std::string directory;   // created when the first file is written
    std::string linkPrefix;  // put before each file name in the Markdown link
    // The conversion's threads, when it has some (layout/page_pool.h), and the threads it may
    // use (ConvertOptions::pageWorkers: 0 automatic, 1 none): a document of one page has no pool,
    // and PlaceImages makes one when the images are worth it.
    PagePool* pool = nullptr;
    unsigned pageWorkers = 1;
};

// Decides, writes and anchors the images of `pages` (after BuildLines and header/footer removal,
// before OrderLines); counts report.images, report.imagesSkipped and report.imagesMaskDropped.
// An image that cannot be extracted keeps its anchor with the reason (ImageState::Failed). Does
// nothing without a document or a directory.
void PlaceImages(std::vector<PageData>& pages, const ImageOutput& output, ConvertReport& report);

// The pixels of placed image `image` of `page` (an entry of page.images: an XObject or an inline
// image, with its page-space box x0..y1 and placement matrix) as 8-bit gray for OCR, through
// Image::GrayXObject or Image::GrayInline with `maxSide`. Works whatever image.state is.
Image::Status ImageGray(const Document& document, const PageData& page, const ImageBox& image, Image::GrayImage& out,
                        uint32_t maxSide = 0);

} // namespace RayoPdf::Layout
