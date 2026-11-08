// SPDX-License-Identifier: Apache-2.0
// Internal interfaces of the JPEG 2000 decoder (src/image/jpx.h): the codestream decoder
// (jpx_codestream.cpp), tier-1 code-block decoding (jpx_t1.cpp) and the inverse wavelet and
// component transforms (jpx_dwt.cpp).
#pragma once

#include "image/jpx.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace RayoPdf::Image::Jpx::Internal {

constexpr int kMaxComponents = 256;
constexpr int kMaxLevels = 32;
constexpr int kMaxPrecision = 16;

// ------------------------------------------------------------------------------------- tier-1

// Code-block styles (COD/COC SPcod, T.800 table A.19).
constexpr uint8_t kStyleBypass = 0x01;
constexpr uint8_t kStyleReset = 0x02;
constexpr uint8_t kStyleTermAll = 0x04;
constexpr uint8_t kStyleCausal = 0x08;
constexpr uint8_t kStylePredictable = 0x10;
constexpr uint8_t kStyleSegmentation = 0x20;
constexpr uint8_t kStyleHt = 0x40;  // HTJ2K (T.814): not supported

// One code-block to decode. `data` holds the concatenated segments and has two writable bytes past
// its end (the decoder puts a 0xFF 0xFF marker after each segment, as OpenJPEG does, and restores
// the bytes).
struct CodeBlockJob {
    uint8_t* data = nullptr;
    const uint32_t* segmentLengths = nullptr;
    const uint32_t* segmentPasses = nullptr;
    int segments = 0;
    int width = 0;
    int height = 0;
    int orientation = 0;  // 0 LL, 1 HL, 2 LH, 3 HH
    int bitplanes = 0;    // the code-block's magnitude bit-planes (Mb + 1 - zero bit-planes)
    int roiShift = 0;
    uint8_t style = 0;
};

// Scratch buffers reused across code-blocks.
class T1Decoder {
public:
    // Decodes into `out` (width x height, row-major): the coefficients with one fractional bit
    // (twice the magnitude, reconstructed at the midpoint of the decoded interval), after the ROI
    // shift. False when the code-block has too many bit-planes to represent.
    bool Decode(const CodeBlockJob& job, int32_t* out);

private:
    std::vector<uint32_t> flags_;
};

// ------------------------------------------------------------------------------ transforms

// One resolution step of the inverse 2-D wavelet transform, in place on `data` (row stride
// `stride`): the region (width x height) holds the lower resolution's LL (lowWidth x lowHeight)
// at the top left, HL to its right, LH below and HH at the bottom right. xOdd/yOdd: the parity
// of the resolution's first column and row on its grid.
void Inverse53(int32_t* data, size_t stride, uint32_t width, uint32_t height, uint32_t lowWidth,
               uint32_t lowHeight, bool xOdd, bool yOdd, std::vector<int32_t>& scratch);
void Inverse97(float* data, size_t stride, uint32_t width, uint32_t height, uint32_t lowWidth,
               uint32_t lowHeight, bool xOdd, bool yOdd, std::vector<float>& scratch);

// Inverse reversible and irreversible component transforms on three equal-size planes.
void InverseRct(int32_t* c0, int32_t* c1, int32_t* c2, size_t count);
void InverseIct(float* c0, float* c1, float* c2, size_t count);

// DC level shift and clipping to [low, high], from integer or floating-point (rounded to nearest
// even as lrintf does) coefficients, into int32 samples.
void ShiftClamp(int32_t* data, size_t count, int32_t shift, int32_t low, int32_t high);
void ShiftClampFloat(const float* in, int32_t* out, size_t count, int32_t shift, int32_t low, int32_t high);

// ------------------------------------------------------------------------------ codestream

struct ComponentInfo {
    uint32_t dx = 1;
    uint32_t dy = 1;
    int precision = 8;
    bool isSigned = false;
    // At the decoded resolution: the component's sample area on its (reduced) grid.
    uint32_t x0 = 0;
    uint32_t y0 = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct CodestreamInfo {
    uint32_t x0 = 0;  // image area on the reference grid (SIZ)
    uint32_t y0 = 0;
    uint32_t x1 = 0;
    uint32_t y1 = 0;
    int levels = 0;   // the fewest decomposition levels of the main header's components
    bool reversible = false;
    int reduce = 0;   // applied by Decode
    std::vector<ComponentInfo> components;
};

// One decoded tile: per component, its samples (after the DC level shift and clipping) at the
// decoded resolution, `width` x `height` at (x0, y0) of the component's reduced grid.
struct TileComponent {
    const int32_t* samples = nullptr;
    size_t stride = 0;
    uint32_t x0 = 0;
    uint32_t y0 = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

class TileSink {
public:
    virtual ~TileSink() = default;
    // Called once before the tiles, with the components' sizes at the decoded resolution; a
    // status other than Ok stops decoding with it.
    virtual Result Begin(const CodestreamInfo& info) = 0;
    virtual void Tile(const TileComponent* components, int count) = 0;
};

// Reads the main header only.
Result ReadMainHeader(const uint8_t* data, size_t size, CodestreamInfo& info);

// Decodes the codestream tile by tile into `sink`. `reduce` is clamped to the main header's
// fewest levels; `maxPixels` bounds the image at the decoded resolution.
Result DecodeCodestream(const uint8_t* data, size_t size, int reduce, uint64_t maxPixels, TileSink& sink);

} // namespace RayoPdf::Image::Jpx::Internal
