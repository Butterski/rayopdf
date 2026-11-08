// SPDX-License-Identifier: Apache-2.0
// Forked from rayomd-gui src/gfx/raster.h at 9fbbfaf (Apache-2.0, same author).
// The exact-area scanline rasterizer, in the manner of FreeType's smooth rasterizer, drawing
// anti-aliased ink onto an 8-bit gray canvas (rows top to bottom): glyph outlines and vector
// paths rendered for OCR (layout/ocr_glyphs.h).
//
// Coordinates are 24.8 fixed point relative to the canvas. A segment is clipped to the canvas in
// double precision before it is rounded: rows outside it are dropped, the part left of it becomes
// a vertical segment on its left edge (keeping its cover), the part right of it is dropped. Work
// goes in bands of 16 rows, each cell an int32 pair {cover, area}. At most kMaxSegments segments
// are taken per fill; further ones are dropped (Overflowed()).
#pragma once

#include "raster/path.h"

#include <cstdint>
#include <vector>

namespace RayoPdf::Raster {

enum class FillRule : uint8_t { NonZero, EvenOdd };

constexpr uint32_t kMaxCanvasSide = 1u << 20;
constexpr size_t kMaxSegments = size_t(1) << 21;

class Rasterizer {
public:
    // Fills `path`, mapped by `m` to canvas pixels (y down), onto `pixels` (`width` x `height`, no
    // row padding): each pixel moves towards `ink` by its coverage; with `darkenOnly` a pixel only
    // ever gets darker (glyphs drawn over one another).
    void Fill(const Path& path, const Matrix& m, FillRule rule, uint8_t* pixels, uint32_t width, uint32_t height,
              uint8_t ink = 0, bool darkenOnly = true);
    // True when the last Fill dropped segments over kMaxSegments.
    bool Overflowed() const { return overflowed_; }

private:
    struct Segment {
        int32_t x0, y0, x1, y1;  // y0 < y1, 24.8 relative to the canvas
        int32_t dir;             // +1 when the original ran downwards, else -1
    };
    struct Sink;

    void AddLine(double x0, double y0, double x1, double y1);
    void Push(double x0, double y0, double x1, double y1);
    void RenderBand(const Segment& s, int bandTop);
    void RowPiece(int32_t* row, int local, int32_t sx, int32_t sy, int32_t ex, int32_t ey, int32_t dir);
    void Cell(int32_t* row, int local, int32_t c, int32_t cover, int32_t area);

    int width_ = 0, height_ = 0;
    std::vector<Segment> segments_;
    std::vector<int32_t> cells_;
    std::vector<uint32_t> order_, bandStart_, active_;
    int32_t minCell_[16] = {}, maxCell_[16] = {};
    int32_t minY_ = 0, maxY_ = -1;
    bool overflowed_ = false;
};

} // namespace RayoPdf::Raster
