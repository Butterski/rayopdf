// SPDX-License-Identifier: Apache-2.0
// The layout inspector (`--format=inspect`, roadmap D6): one HTML5 file with a style block and no
// script that draws every page as SVG in PDF points: each line's text where it stands (turned lines
// turned, lines removed as running headers or footers grey and struck through), each block's boxes
// (the JSON output's, BlockBoxes) coloured by type and numbered in reading order, with a tooltip
// of its type and the start of its Markdown, a polyline through the blocks' centres in output
// order, the boxes of table cells and images. For looking at what the layout stages decided.
#pragma once

#include "layout/markdown_writer.h"
#include "layout/page.h"
#include "layout/semantics.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace RayoPdf::Layout {

// Lines drawn on one page at most; a note says how many more there were.
constexpr uint32_t kInspectMaxLines = 20000;

// Appends the inspector page of `pages` to `out`: `written` are the blocks WriteMarkdown wrote
// (with `written`) into `markdown`, in output order; block k (from 1) is written[k - 1].
void WriteInspect(const std::vector<PageData>& pages, const BlockList& blocks, std::string_view markdown,
                  const std::vector<WrittenBlock>& written, std::string& out);

} // namespace RayoPdf::Layout
