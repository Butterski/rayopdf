// SPDX-License-Identifier: Apache-2.0
// Images as files (roadmap D6, algorithms §6.9): which images of the pages are content, writing
// them, and their places in reading order. Decoration stays out: images under 1 % of the page
// area or under 24 pixels on a side, page-filling backgrounds behind text, and an image repeated
// at the same place on at least half the pages (a logo). A kept image is written once, however
// often it is drawn, as p<page>-<n>.<ext> (the page of its first placement, n counting that page's
// files), and each placement gets an anchor line (kLineImage) that OrderLines orders with the text.
#pragma once

#include "layout/convert.h"
#include "layout/page.h"
#include "pdf/document.h"

#include <string>
#include <vector>

namespace RayoPdf::Layout {

struct ImageOutput {
    const Document* document = nullptr;
    std::string directory;   // created when the first file is written
    std::string linkPrefix;  // put before each file name in the Markdown link
};

// Decides, writes and anchors the images of `pages` (after BuildLines and header/footer removal,
// before OrderLines); counts report.images, report.imagesSkipped and report.imagesMaskDropped.
// An image that cannot be extracted keeps its anchor with the reason (ImageState::Failed). Does
// nothing without a document or a directory.
void PlaceImages(std::vector<PageData>& pages, const ImageOutput& output, ConvertReport& report);

} // namespace RayoPdf::Layout
