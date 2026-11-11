// SPDX-License-Identifier: Apache-2.0
// The JPEG 2000 decoder of JPXDecode images (internal to the image module), written from ITU-T
// T.800 (ISO/IEC 15444-1): JP2 files (annex I boxes: image header, colour specification with
// enumerated or ICC colour, palette, component mapping, channel definition; other boxes skipped)
// and raw J2K codestreams. Codestream: SIZ, COD, COC, QCD, QCC, RGN, POC, PPM, PPT, TLM, PLM, PLT,
// CRG and COM; tiles and tile-parts; the five progression orders and progression changes; layers;
// precincts; SOP and EPH markers; code-blocks with every style of T.800 (bypass, reset, terminate
// each pass, vertically causal, predictable termination, segmentation symbols); EBCOT tier-1 with
// the MQ decoder; scalar quantization (none, derived, expounded) and ROI max-shift; the 5/3
// reversible and 9/7 irreversible inverse wavelet transforms; RCT and ICT; components of any size
// (subsampling) and precisions 1 to 16, signed or unsigned. Part 2 extensions (multiple component
// transforms, arbitrary wavelets, ...) and HTJ2K code-blocks fail as Unsupported.
//
// The samples match OpenJPEG 2.5 (`opj_decompress`), the reference decoder of RayoPDF's check:
// the same reconstruction (the midpoint of the last decoded interval), the same 9/7 lifting and
// ICT in single-precision floating point, rounded to nearest-even. The arithmetic is IEEE single
// precision, never contracted into fused multiply-adds (see jpx_dwt.cpp), so builds agree.
//
// What a PDF needs (ISO 32000-1 7.4.9 and 8.9.5): Decode returns the image's channels after the
// JP2 palette (unless Options::ignorePalette, for a PDF /ColorSpace that is Indexed), the colour
// channels in the order of the channel definitions' associations (the colour space's component
// order; without channel definitions, the codestream's), sYCC converted to sRGB, every channel at
// the image's full size (subsampled components are repeated), as 8-bit samples or, when a channel
// has more than 8 bits, big-endian 16-bit samples, each scaled from its precision to the full
// range (signed samples offset to unsigned) except with Options::ignorePalette, where the samples
// are the index values themselves. The opacity channel (a channel definition of type 1 or 2, or
// with SMaskInData and no channel definitions the channel after the colour channels) is appended
// last when Options::smaskInData is nonzero. Image tells what the JP2 header says about the
// colour; a PDF /ColorSpace, when present, overrides it (the caller's business).
//
// Reduced resolution (Options::reduce = r > 0) decodes only the first levels of the wavelet
// pyramid: the image comes out ceil(size / 2^r) large at a fraction of the work, for OCR and
// thumbnails.
//
// Every read is bounds-checked; buffers are sized from validated headers (at most `maxPixels`
// pixels at the decoded resolution; a tile's code-blocks and precincts at most twice its samples
// at full resolution and 4 Mi each). Work is bounded by the image size and the data size:
// packets are read until the data ends, a code-block has at most 128 segments, no recursion.
//
// With Options::threads the independent parts of a decode run on the conversion's threads: the
// code-blocks of a tile (tier-1), the lines of each wavelet pass, the component transform and
// the rows written to the image. Every part writes samples of its own and reads none that
// another writes meanwhile, so the samples are those of one thread.
#pragma once

#include "common/threads.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace RayoPdf::Image::Jpx {

enum class Result : uint8_t {
    Ok,
    Unsupported,  // a valid file this decoder does not read (Part 2 or HTJ2K features, > 16 bits)
    TooLarge,     // more than `maxPixels` pixels at the decoded resolution
    Corrupt,      // not JPEG 2000, or damaged so that no image can be made
};

// The colour space the JP2 header gives (the first colour specification box).
enum class Colour : uint8_t {
    Unspecified,  // a raw codestream or an unknown method: the channels are what the PDF says
    Gray,         // enumerated 17
    Srgb,         // enumerated 16; also 18 (sYCC), which Decode converts to sRGB
    Cmyk,         // enumerated 12
    Icc,          // an ICC profile (methods 2 and 3): Info::iccOffset / iccSize
    Other,        // another enumerated colour space (Info::enumerated has its number)
};

struct Info {
    uint32_t width = 0;   // the image area at full resolution (SIZ)
    uint32_t height = 0;
    int components = 0;   // codestream components
    int precision = 0;    // the largest component precision
    int levels = 0;       // the fewest decomposition levels in the main header: the largest reduce
    bool jp2 = false;     // a JP2 file (else a raw codestream)
    bool reversible = false;   // the main header's wavelet is 5/3
    bool palette = false;      // the JP2 header has a palette (pclr) and component mapping
    bool channelDefinitions = false;
    Colour colour = Colour::Unspecified;
    uint32_t enumerated = 0;   // the enumerated colour space number (0 without one)
    size_t iccOffset = 0;      // the ICC profile's bytes within the data (iccSize 0: none)
    size_t iccSize = 0;
};

struct Options {
    uint64_t maxPixels = uint64_t(1) << 28;
    int reduce = 0;              // resolution levels to drop (clamped to Info::levels)
    bool ignorePalette = false;  // keep the palette indices (a PDF Indexed /ColorSpace)
    int smaskInData = 0;         // the image dictionary's /SMaskInData: 0 drops opacity
    // The components of the PDF /ColorSpace (0 without one): the colour channels Decode keeps,
    // the next one being the opacity channel with SMaskInData and no channel definitions.
    int pdfComponents = 0;
    // The conversion's threads (common/threads.h) with the calling thread's number among them;
    // null: the calling thread alone.
    Threads* threads = nullptr;
    unsigned worker = 0;
    // What is split while threads run: a wavelet pass, a component transform or a tile's rows of
    // at least `splitSamples` samples, into parts of `partLines` lines (a multiple of 16; the
    // component transform: 1024 samples a line). A part then takes 0.05 to 0.2 ms, many times
    // what handing it to a thread costs. The tests lower both to split their small images.
    uint64_t splitSamples = uint64_t(1) << 17;
    uint32_t partLines = 64;
};

struct Image {
    Info info;
    uint32_t width = 0;          // pixels at the decoded resolution
    uint32_t height = 0;
    int reduce = 0;              // the resolution levels dropped
    int channels = 0;            // samples per pixel: colourChannels, plus 1 with opacity
    int colourChannels = 0;
    bool opacity = false;        // the last channel is opacity (only with smaskInData)
    bool premultiplied = false;  // the colour channels are premultiplied by it
    int bitsPerComponent = 8;    // 8, or 16 (big-endian)
    int precision = 0;           // the largest channel precision before scaling (index bits)
    Colour colour = Colour::Unspecified;  // after any sYCC conversion
    std::string samples;         // interleaved, rows top to bottom, no row padding
};

// A codestream component as decoded (after the inverse colour transform, the DC level shift and
// clipping to its precision): `width` x `height` samples at the decoded resolution, whose top
// left sample is at (x0, y0) of the component's (reduced) sampling grid.
struct Component {
    uint32_t x0 = 0;
    uint32_t y0 = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t dx = 1;  // subsampling (XRsiz, YRsiz)
    uint32_t dy = 1;
    int precision = 0;
    bool isSigned = false;
    std::vector<int32_t> samples;
};

// Reads the JP2 boxes and the main header: the checks the decoder makes before it decodes.
Result ReadInfo(const uint8_t* data, size_t size, Info& info);

// The largest number of levels to drop so that both sides stay at least `minSide` pixels (0 when
// the image is smaller): Options::reduce for an image needed at about that size.
int ReduceFor(const Info& info, uint32_t minSide);

// Decodes the image for a PDF (see the top comment) into `image` (replaced; samples empty on
// failure).
Result Decode(const uint8_t* data, size_t size, const Options& options, Image& image);

// Decodes the codestream's components as they are, before the JP2 palette, channel definitions or
// colour conversion: the samples OpenJPEG's library returns with its palette and channel
// definitions switched off. For the reference check and the tests.
Result DecodeComponents(const uint8_t* data, size_t size, const Options& options, Info& info,
                        std::vector<Component>& components);

} // namespace RayoPdf::Image::Jpx
