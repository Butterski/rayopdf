// SPDX-License-Identifier: Apache-2.0
// Pixel work of the OCR add-on rayopdf-ocr (never linked into `rayopdf`): resampling an 8-bit gray
// image, cutting a rotated rectangle out of it, and turning gray pixels into the normalised
// three-channel float planes the PP-OCR networks read.
//
// Resize is separable: an axis that shrinks is box-filtered (every source pixel weighted by the
// share of it an output pixel covers, as an area resampler does), an axis that grows is bilinear
// with pixel centres aligned. Weights are computed once per axis and call; the work is linear in
// source plus output pixels. Everything is single-threaded and deterministic: the same input gives
// the same bytes on every run.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace RayoPdf::Ocr {

// 8-bit gray pixels, rows top to bottom, `width` bytes a row, no padding.
struct GrayView {
    const uint8_t* pixels = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
};

// A rectangle at any angle, in pixel coordinates of an image (x right, y down): its centre, the unit
// vector (ux, uy) along its width, and its size. The height runs along (-uy, ux).
struct RotatedRect {
    float cx = 0, cy = 0;
    float ux = 1, uy = 0;
    float width = 0, height = 0;
};

// Scratch buffers of Resize, kept by the caller so repeated calls do not allocate.
struct ResizeScratch {
    std::vector<uint32_t> first, count, offset;
    std::vector<float> weights;
    std::vector<uint32_t> firstY, countY, offsetY;
    std::vector<float> weightsY;
    std::vector<float> rows;  // horizontally resampled rows
};

// `source` resampled to width x height (both at least 1) into `out` (replaced).
void Resize(const GrayView& source, uint32_t width, uint32_t height, ResizeScratch& scratch, std::vector<uint8_t>& out);

// `rect` of `source` sampled bilinearly into width x height pixels along the rectangle's own axes
// (x along its width; the whole rectangle whatever the output size); samples outside the image
// repeat its nearest edge pixel.
void SampleRect(const GrayView& source, const RotatedRect& rect, uint32_t width, uint32_t height,
                std::vector<uint8_t>& out);

// `pixels` (width x height) rotated a quarter turn counter-clockwise into `out`, which becomes
// height x width: the image's top row becomes its left column, read bottom to top.
void RotateCounterClockwise(const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height,
                            std::vector<uint8_t>& out);

// Three planes (channel-major, as a 1x3xHxW tensor) of (gray / 255 - mean[c]) / deviation[c].
void Normalise(const uint8_t* pixels, size_t count, const float mean[3], const float deviation[3],
               std::vector<float>& planes);

} // namespace RayoPdf::Ocr
