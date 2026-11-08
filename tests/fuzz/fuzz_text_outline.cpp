// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of glyph-shape recovery's parsers (src/text/outline.h, src/raster/): the input
// read as an sfnt with 'glyf' outlines, as a CFF font with Type 2 charstrings and as a Type 1
// program, the first glyphs of each drawn by the rasterizer; and the input's tail read as a path of
// raw coordinates, filled through a matrix built from its first bytes.
#include "raster/raster.h"
#include "text/outline.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

using namespace RayoPdf;
using namespace RayoPdf::Text::Program;

namespace {

constexpr uint32_t kSide = 64;
constexpr uint32_t kGlyphs = 64;

void Draw(Raster::Rasterizer& raster, const Raster::Path& path, const Raster::Matrix& m, uint8_t* pixels) {
    raster.Fill(path, m, Raster::FillRule::NonZero, pixels, kSide, kSide);
    raster.Fill(path, m, Raster::FillRule::EvenOdd, pixels, kSide, kSide, 128, false);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    static uint8_t pixels[kSide * kSide];
    Raster::Rasterizer raster;
    Raster::Path path;
    // Glyph space (1000 or unitsPerEm units) to the canvas, y down.
    const Raster::Matrix toCanvas{0.05, 0, 0, -0.05, 8, 56};

    Sfnt sfnt;
    GlyfOutlines glyf;
    if (ReadSfnt(input, sfnt) && ReadGlyfOutlines(sfnt, glyf)) {
        for (uint32_t g = 0; g < glyf.glyphCount && g < kGlyphs; ++g) {
            path.Clear();
            GlyfOutline(glyf, g, toCanvas, path);
            Draw(raster, path, Raster::Matrix(), pixels);
        }
    }
    CffOutlines cff;
    if (ReadCffOutlines(!sfnt.cff.empty() ? sfnt.cff : input, cff)) {
        for (uint32_t g = 0; g < cff.charStrings.size() && g < kGlyphs; ++g) {
            path.Clear();
            CffGlyphOutline(cff, g, CffGlyphMatrix(cff, g), path);
            Draw(raster, path, toCanvas, pixels);
        }
    }
    Type1Outlines type1;
    if (ReadType1Outlines(input, type1)) {
        for (size_t g = 0; g < type1.charStrings.size() && g < kGlyphs; ++g) {
            path.Clear();
            Type1GlyphOutline(type1, type1.charStrings[g].first, toCanvas, path);
            Draw(raster, path, Raster::Matrix(), pixels);
        }
    }

    // A path of raw floats: each 9-byte record is a verb and two coordinates.
    if (size >= 48) {
        double m[6];
        for (int i = 0; i < 6; ++i) {
            float v;
            std::memcpy(&v, data + 4 * i, 4);
            m[i] = v;
        }
        path.Clear();
        for (size_t at = 24; at + 9 <= size; at += 9) {
            float x, y;
            std::memcpy(&x, data + at + 1, 4);
            std::memcpy(&y, data + at + 5, 4);
            switch (data[at] % 5) {
            case 0: path.MoveTo(x, y); break;
            case 1: path.LineTo(x, y); break;
            case 2: path.QuadTo(x, y, y, x); break;
            case 3: path.CubicTo(x, y, y, x, x * 0.5f, y * 0.5f); break;
            default: path.Close(); break;
            }
        }
        Draw(raster, path, Raster::Matrix{m[0], m[1], m[2], m[3], m[4], m[5]}, pixels);
    }
    return 0;
}
