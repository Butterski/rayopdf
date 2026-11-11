// SPDX-License-Identifier: Apache-2.0
// Image XObjects and inline images to file bytes (image.h). Choices the header leaves open:
// - /ImageMask stencils come out as opaque gray: black where the stencil paints, white elsewhere
//   (the fill colour is the content stream's, unknown here; black on white reads in any viewer).
// - A data stream shorter than the image keeps the rows present: the height shrinks to them (a
//   last partial row is completed with zero bytes); no complete or partial row at all is Corrupt.
//   CCITT and JBIG2 data are the exception: their decoders' output is read as the filter's byte
//   stream in rows of the image's own (as poppler does, whatever /Columns or the JBIG2 page width
//   says), what a damaged stream decoded before the damage is kept, and missing rows are white,
//   as poppler shows them.
// - Flate-family data (Flate, LZW, RunLength, ASCII85, ASCIIHex, predictors) is decoded a row at
//   a time as the PNG is written, and its soft or stencil mask alongside: only a few rows, the
//   inflate window and the compressor's window are held, whatever the image's size. A decision
//   that needs every row first (alpha opaque everywhere, gray that fewer bits hold) reads the data
//   once more; the PNG is begun with the declared height and begun again when the data ends
//   before it. DCT, JPX, CCITT and JBIG2 samples come whole from their decoders.
// - 16-bit samples are rounded to 8 bits; CMYK becomes RGB by the naive formula
//   R = 255 (1 - C)(1 - K); CalGray and CalRGB are taken as DeviceGray and DeviceRGB; Lab goes
//   through XYZ to sRGB with its white point mapped onto D65 by scaling.
// - Separation and DeviceN: a tint transform of type 0 (sampled, one input) or 2 (exponential),
//   or an array of them, is evaluated into the alternate space. Otherwise colorants named Cyan,
//   Magenta, Yellow and Black are composed as CMYK, and anything else is gray from the summed
//   tint (1 is black).
// - Soft masks (/SMask) and stencil masks (/Mask stream) of another size are resampled to the
//   image's by nearest neighbour; a mask that cannot be decoded is dropped, the image written
//   opaque and Encoded::maskDropped set. /Matte is ignored. Alpha that is opaque everywhere is
//   left out of the PNG.
// - A JPEG stored as it is (image.h) is written from its SOI marker on. A decoded JPEG's samples
//   go through the PDF colour space and /Decode like any 8-bit samples; a colour space of another
//   number of components than the JPEG's gives way to the JPEG's own (gray, RGB, CMYK). A JPEG
//   that should be decoded but cannot be (arithmetic coding, 12-bit, damaged, more than 64 Mi
//   pixels) is stored after all.
// - JPX: decoded samples of 8 or 16 bits through the PDF colour space, or without one (or with
//   one of another number of components) the JP2 header's (gray, sRGB, CMYK), else the device
//   space of as many channels; an Indexed space reads the raw indices (the JP2 palette is not
//   applied). A JPX image the decoder cannot read is stored (from the JP2 signature on; a raw
//   codestream gets a minimal JP2 wrapper so that it is a .jp2), a mask that hides part of it
//   counted as dropped.
#include "image/image.h"

#include "common/minmax.h"
#include "image/ccitt.h"
#include "image/jbig2.h"
#include "image/jpeg.h"
#include "image/jpx.h"
#include "image/png.h"
#include "pdf/filters.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace RayoPdf::Image {
namespace {

constexpr int kMaxComponents = 32;        // DeviceN colorants (ISO 32000-1 Annex C)
constexpr int kMaxSpaceDepth = 8;          // named colour spaces, bases and alternates
constexpr uint32_t kMaxDimension = 0x7FFFFFFFu;
constexpr uint32_t kMaxFunctionSamples = 1u << 16;  // sampled function: samples x outputs
constexpr size_t kMaxFilters = 8;
constexpr size_t kPassThroughLimit = Document::kDefaultStreamLimit;

// ---------------------------------------------------------------- arithmetic

inline double Clamp01(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
inline uint8_t To8(double x) { return static_cast<uint8_t>(Clamp01(x) * 255 + 0.5); }

// x^n for x >= 0 without libm (roadmap D1): the integer part of n by squaring, the fraction by
// its first 24 binary digits, each a repeated square root of x.
double Power(double x, double n) {
    if (!(x > 0)) return n == 0 ? 1 : 0;
    if (n < 0) return 1 / Power(x, -n);
    if (n > 1024) n = 1024;
    uint32_t whole = static_cast<uint32_t>(n);
    double fraction = n - whole;
    double result = 1;
    double square = x;
    for (; whole > 0; whole >>= 1) {
        if (whole & 1) result *= square;
        square *= square;
    }
    double root = x;
    for (int bit = 0; bit < 24 && fraction > 0; bit++) {
        root = std::sqrt(root);
        fraction *= 2;
        if (fraction >= 1) {
            result *= root;
            fraction -= 1;
        }
    }
    return result;
}

// Linear light 0..1 in 4096 steps to sRGB-encoded 8 bits (IEC 61966-2-1).
constexpr int kGammaSteps = 4096;
const uint8_t* SrgbTable() {
    static const std::vector<uint8_t> table = [] {
        std::vector<uint8_t> t(kGammaSteps + 1);
        for (int i = 0; i <= kGammaSteps; i++) {
            const double x = static_cast<double>(i) / kGammaSteps;
            t[static_cast<size_t>(i)] = To8(x <= 0.0031308 ? 12.92 * x : 1.055 * Power(x, 1 / 2.4) - 0.055);
        }
        return t;
    }();
    return table.data();
}

inline uint8_t EncodeSrgb(double linear) {
    return SrgbTable()[static_cast<int>(Clamp01(linear) * kGammaSteps + 0.5)];
}

// ---------------------------------------------------------------- functions

// One part of a tint transform: a type 0 (sampled) or type 2 (exponential) function of one input
// (ISO 32000-1 section 7.10.2-7.10.3).
struct FunctionPart {
    int type = 2;
    int outputs = 0;
    double domain[2] = {0, 1};
    bool hasRange = false;
    double range[2 * kMaxComponents] = {};
    // type 2
    double c0[kMaxComponents] = {};
    double c1[kMaxComponents] = {};
    double exponent = 1;
    // type 0: `size` samples of `outputs` decoded values each
    uint32_t size = 0;
    double encode[2] = {0, 0};
    std::vector<float> samples;
};

void Evaluate(const FunctionPart& f, double x, double* out) {
    x = Fmin(Fmax(x, f.domain[0]), f.domain[1]);
    if (f.type == 2) {
        const double t = Power(x, f.exponent);
        for (int j = 0; j < f.outputs; j++) out[j] = f.c0[j] + t * (f.c1[j] - f.c0[j]);
    } else {
        const double span = f.domain[1] - f.domain[0];
        double e = span > 0 ? f.encode[0] + (x - f.domain[0]) * (f.encode[1] - f.encode[0]) / span : f.encode[0];
        e = Fmin(Fmax(e, 0.0), static_cast<double>(f.size - 1));
        const uint32_t i0 = static_cast<uint32_t>(e);
        const uint32_t i1 = i0 + 1 < f.size ? i0 + 1 : i0;
        const double t = e - i0;
        const size_t m = static_cast<size_t>(f.outputs);
        for (size_t j = 0; j < m; j++) {
            const double a = f.samples[i0 * m + j];
            const double b = f.samples[i1 * m + j];
            out[j] = a + t * (b - a);
        }
    }
    if (f.hasRange) {
        for (int j = 0; j < f.outputs; j++) out[j] = Fmin(Fmax(out[j], f.range[2 * j]), f.range[2 * j + 1]);
    }
}

// Numbers of an array into `out`; false unless it holds exactly `count` of them.
bool Numbers(const Document& doc, const PdfValue& value, double* out, size_t count) {
    const PdfValue& array = doc.Resolve(value);
    if (!array.IsArray() || array.Array().size() != count) return false;
    for (size_t i = 0; i < count; i++) {
        if (!doc.Number(array.Array()[i], out[i])) return false;
    }
    return true;
}

// The numbers of an array of even length 2..2 * kMaxComponents; their count, 0 for anything else.
size_t NumberPairs(const Document& doc, const PdfValue& value, double* out) {
    const PdfValue& array = doc.Resolve(value);
    const size_t n = array.Array().size();
    if (n == 0 || n % 2 != 0 || n > 2 * kMaxComponents) return 0;
    return Numbers(doc, array, out, n) ? n : 0;
}

bool ParseFunctionPart(const Document& doc, const PdfValue& value, FunctionPart& f) {
    const PdfValue& dict = doc.Resolve(value);
    if (!dict.IsDict()) return false;
    double domain[2];
    if (!Numbers(doc, doc.Get(dict, "Domain"), domain, 2) || !(domain[0] <= domain[1])) return false;
    f.domain[0] = domain[0];
    f.domain[1] = domain[1];
    const size_t rangeCount = NumberPairs(doc, doc.Get(dict, "Range"), f.range);
    f.hasRange = rangeCount > 0;
    const int64_t type = doc.Get(dict, "FunctionType").Int(-1);
    if (type == 2) {
        f.type = 2;
        const PdfValue& c0 = doc.Get(dict, "C0");
        const PdfValue& c1 = doc.Get(dict, "C1");
        const size_t n = c0.IsNull() ? (c1.IsNull() ? 1 : c1.Array().size()) : c0.Array().size();
        if (n == 0 || n > kMaxComponents) return false;
        f.outputs = static_cast<int>(n);
        if (c0.IsNull()) std::fill(f.c0, f.c0 + n, 0.0);
        else if (!Numbers(doc, c0, f.c0, n)) return false;
        if (c1.IsNull()) std::fill(f.c1, f.c1 + n, 1.0);
        else if (!Numbers(doc, c1, f.c1, n)) return false;
        if (!doc.Number(doc.Get(dict, "N"), f.exponent)) return false;
        if (f.hasRange && rangeCount != 2 * n) f.hasRange = false;
        // A fractional exponent is defined for non-negative inputs only.
        if (f.exponent != std::floor(f.exponent) && f.domain[0] < 0) f.domain[0] = 0;
        if (f.domain[0] > f.domain[1]) return false;
        return true;
    }
    if (type != 0 || !dict.IsStream() || rangeCount == 0) return false;
    f.type = 0;
    f.outputs = static_cast<int>(rangeCount / 2);
    double size = 0;
    const PdfValue& sizes = doc.Get(dict, "Size");
    if (!Numbers(doc, sizes, &size, 1) || !(size >= 1) || size > kMaxFunctionSamples) return false;
    f.size = static_cast<uint32_t>(size);
    const int64_t bps = doc.Get(dict, "BitsPerSample").Int(0);
    if (bps != 1 && bps != 2 && bps != 4 && bps != 8 && bps != 12 && bps != 16 && bps != 24 && bps != 32) return false;
    const uint64_t count = uint64_t(f.size) * static_cast<uint64_t>(f.outputs);
    if (count > kMaxFunctionSamples) return false;
    if (!Numbers(doc, doc.Get(dict, "Encode"), f.encode, 2)) {
        f.encode[0] = 0;
        f.encode[1] = f.size - 1.0;
    }
    double decode[2 * kMaxComponents];
    if (NumberPairs(doc, doc.Get(dict, "Decode"), decode) != rangeCount) std::copy(f.range, f.range + rangeCount, decode);
    std::string data;
    const size_t bytes = static_cast<size_t>((count * static_cast<uint64_t>(bps) + 7) / 8);
    const StreamStatus status = doc.StreamData(dict, data, bytes);
    if (status == StreamStatus::Corrupt || status == StreamStatus::NotStream || status == StreamStatus::Unsupported ||
        data.size() < bytes) {
        return false;
    }
    f.samples.resize(static_cast<size_t>(count));
    const double maxSample = static_cast<double>((uint64_t(1) << bps) - 1);
    const uint8_t* in = reinterpret_cast<const uint8_t*>(data.data());
    uint64_t bit = 0;
    for (size_t i = 0; i < f.samples.size(); i++, bit += static_cast<uint64_t>(bps)) {
        uint64_t sample = 0;
        for (int64_t k = 0; k < bps; k++) {
            const uint64_t at = bit + static_cast<uint64_t>(k);
            sample = sample << 1 | ((in[at >> 3] >> (7 - (at & 7))) & 1);
        }
        const size_t j = i % static_cast<size_t>(f.outputs);
        f.samples[i] = static_cast<float>(decode[2 * j] + sample * (decode[2 * j + 1] - decode[2 * j]) / maxSample);
    }
    return true;
}

// A tint transform of one input: one function, or an array of one-output functions. False for
// anything else (type 3 and 4 functions included).
bool ParseTint(const Document& doc, const PdfValue& value, std::vector<FunctionPart>& parts, int& outputs) {
    parts.clear();
    outputs = 0;
    const PdfValue& resolved = doc.Resolve(value);
    const PdfValue* items = &resolved;
    size_t count = 1;
    if (resolved.IsArray()) {
        items = resolved.Array().begin();
        count = resolved.Array().size();
        if (count == 0 || count > kMaxComponents) return false;
    }
    parts.resize(count);
    for (size_t i = 0; i < count; i++) {
        if (!ParseFunctionPart(doc, items[i], parts[i])) return false;
        if (count > 1 && parts[i].outputs != 1) return false;
        outputs += parts[i].outputs;
    }
    return outputs <= kMaxComponents;
}

// ---------------------------------------------------------------- colour spaces

enum class Family : uint8_t {
    Gray,
    Rgb,
    Cmyk,
    Lab,
    Indexed,
    Separation,  // Separation and DeviceN
};

struct Space {
    Family family = Family::Gray;
    int components = 1;  // per sample
    int channels = 1;    // of the output: 1 gray or 3 RGB
    double range[4] = {-100, 100, -100, 100};  // Lab a* and b*
    // Indexed
    int hival = 0;
    std::vector<uint8_t> palette;  // (hival + 1) * channels
    // Separation and DeviceN: the tint transform into `alt`, when usable; else `process` maps
    // each colorant to C, M, Y or K (-1 for none), when every colorant is one of them.
    std::vector<FunctionPart> tint;
    std::unique_ptr<Space> alt;
    bool useProcess = false;
    int8_t process[kMaxComponents] = {};
};

void Convert(const Space& space, const double* v, uint8_t* out);

void ConvertLab(const Space& space, const double* v, uint8_t* out) {
    const double l = Fmin(Fmax(v[0], 0.0), 100.0);
    const double a = Fmin(Fmax(v[1], space.range[0]), space.range[1]);
    const double b = Fmin(Fmax(v[2], space.range[2]), space.range[3]);
    const double fy = (l + 16) / 116;
    const double f[3] = {fy + a / 500, fy, fy - b / 200};
    double xyz[3];
    constexpr double kD65[3] = {0.95047, 1.0, 1.08883};
    for (int i = 0; i < 3; i++) {
        const double t = f[i];
        xyz[i] = kD65[i] * (t > 6.0 / 29 ? t * t * t : 3 * (6.0 / 29) * (6.0 / 29) * (t - 4.0 / 29));
    }
    out[0] = EncodeSrgb(3.2404542 * xyz[0] - 1.5371385 * xyz[1] - 0.4985314 * xyz[2]);
    out[1] = EncodeSrgb(-0.9692660 * xyz[0] + 1.8760108 * xyz[1] + 0.0415560 * xyz[2]);
    out[2] = EncodeSrgb(0.0556434 * xyz[0] - 0.2040259 * xyz[1] + 1.0572252 * xyz[2]);
}

void ConvertCmyk(double c, double m, double y, double k, uint8_t* out) {
    const double white = 1 - Clamp01(k);
    out[0] = To8((1 - Clamp01(c)) * white);
    out[1] = To8((1 - Clamp01(m)) * white);
    out[2] = To8((1 - Clamp01(y)) * white);
}

// `v` holds one value per component in the space's own range (an index for Indexed).
void Convert(const Space& space, const double* v, uint8_t* out) {
    switch (space.family) {
    case Family::Gray: out[0] = To8(v[0]); return;
    case Family::Rgb:
        out[0] = To8(v[0]);
        out[1] = To8(v[1]);
        out[2] = To8(v[2]);
        return;
    case Family::Cmyk: ConvertCmyk(v[0], v[1], v[2], v[3], out); return;
    case Family::Lab: ConvertLab(space, v, out); return;
    case Family::Indexed: {
        const double index = Fmin(Fmax(std::floor(v[0] + 0.5), 0.0), static_cast<double>(space.hival));
        const size_t at = static_cast<size_t>(index) * static_cast<size_t>(space.channels);
        std::memcpy(out, space.palette.data() + at, static_cast<size_t>(space.channels));
        return;
    }
    case Family::Separation: break;
    }
    if (space.alt) {
        double alt[kMaxComponents];
        int at = 0;
        for (const FunctionPart& part : space.tint) {
            Evaluate(part, v[0], alt + at);
            at += part.outputs;
        }
        Convert(*space.alt, alt, out);
        return;
    }
    if (space.useProcess) {
        double cmyk[4] = {0, 0, 0, 0};
        for (int i = 0; i < space.components; i++) {
            if (space.process[i] >= 0) cmyk[space.process[i]] = Fmax(cmyk[space.process[i]], Clamp01(v[i]));
        }
        ConvertCmyk(cmyk[0], cmyk[1], cmyk[2], cmyk[3], out);
        return;
    }
    double sum = 0;
    for (int i = 0; i < space.components; i++) sum += Clamp01(v[i]);
    out[0] = To8(1 - Fmin(sum, 1.0));
}

struct SpaceContext {
    const Document& doc;
    const PdfValue& resources;
    bool inlineImage;
};

Status ParseSpace(const SpaceContext& ctx, const PdfValue& value, int depth, Space& out);

void SetDevice(Space& out, Family family) {
    out.family = family;
    out.components = family == Family::Gray ? 1 : family == Family::Cmyk ? 4 : 3;
    out.channels = family == Family::Gray ? 1 : 3;
}

Status ParseNamedSpace(const SpaceContext& ctx, std::string_view name, int depth, Space& out) {
    if (name == "DeviceGray" || name == "CalGray" || (ctx.inlineImage && name == "G")) {
        SetDevice(out, Family::Gray);
        return Status::Ok;
    }
    if (name == "DeviceRGB" || name == "CalRGB" || (ctx.inlineImage && name == "RGB")) {
        SetDevice(out, Family::Rgb);
        return Status::Ok;
    }
    if (name == "DeviceCMYK" || (ctx.inlineImage && name == "CMYK")) {
        SetDevice(out, Family::Cmyk);
        return Status::Ok;
    }
    if (name == "Pattern") return Status::Unsupported;
    const PdfValue& named = ctx.doc.Get(ctx.doc.Get(ctx.resources, "ColorSpace"), name);
    if (named.IsNull()) return Status::Corrupt;
    return ParseSpace(ctx, named, depth + 1, out);
}

Status ParseIndexed(const SpaceContext& ctx, PdfSpan<PdfValue> items, int depth, Space& out) {
    if (items.size() < 4) return Status::Corrupt;
    Space base;
    const Status status = ParseSpace(ctx, items[1], depth + 1, base);
    if (status != Status::Ok) return status;
    if (base.family == Family::Indexed) return Status::Corrupt;
    const int64_t hival = ctx.doc.Resolve(items[2]).Int(-1);
    if (hival < 0) return Status::Corrupt;
    out.family = Family::Indexed;
    out.components = 1;
    out.hival = static_cast<int>(std::min<int64_t>(hival, 255));
    out.channels = base.channels;
    const size_t baseN = static_cast<size_t>(base.components);
    const size_t needed = static_cast<size_t>(out.hival + 1) * baseN;
    std::string stored;
    std::string_view lookup;
    const PdfValue& table = ctx.doc.Resolve(items[3]);
    if (table.IsString()) {
        lookup = table.String();
    } else if (table.IsStream()) {
        const StreamStatus s = ctx.doc.StreamData(table, stored, needed);
        if (s == StreamStatus::Corrupt || s == StreamStatus::NotStream || s == StreamStatus::Unsupported) {
            return Status::Corrupt;
        }
        lookup = stored;
    } else {
        return Status::Corrupt;
    }
    if (lookup.empty()) return Status::Corrupt;
    // A short table leaves the entries past it black (zero components), as viewers do.
    out.palette.assign(static_cast<size_t>(out.hival + 1) * static_cast<size_t>(out.channels), 0);
    for (size_t i = 0; i <= static_cast<size_t>(out.hival); i++) {
        double v[kMaxComponents];
        for (size_t j = 0; j < baseN; j++) {
            const size_t at = i * baseN + j;
            const double byte = at < lookup.size() ? static_cast<uint8_t>(lookup[at]) : 0;
            if (base.family == Family::Lab) {
                v[j] = j == 0 ? byte * 100 / 255 : base.range[2 * (j - 1)] + byte * (base.range[2 * j - 1] - base.range[2 * (j - 1)]) / 255;
            } else {
                v[j] = byte / 255;
            }
        }
        Convert(base, v, out.palette.data() + i * static_cast<size_t>(out.channels));
    }
    return Status::Ok;
}

int ProcessColorant(std::string_view name) {
    if (name == "Cyan") return 0;
    if (name == "Magenta") return 1;
    if (name == "Yellow") return 2;
    if (name == "Black") return 3;
    if (name == "None") return -1;
    return -2;
}

Status ParseSeparation(const SpaceContext& ctx, PdfSpan<PdfValue> items, bool deviceN, int depth, Space& out) {
    if (items.size() < 4) return Status::Corrupt;
    out.family = Family::Separation;
    out.channels = 1;
    const PdfValue& names = ctx.doc.Resolve(items[1]);
    const PdfValue* nameItems = &names;
    size_t n = 1;
    if (deviceN) {
        n = names.Array().size();
        nameItems = names.Array().begin();
        if (n == 0) return Status::Corrupt;
        if (n > kMaxComponents) return Status::Unsupported;
    }
    out.components = static_cast<int>(n);
    if (n == 1) {
        auto alt = std::make_unique<Space>();
        int outputs = 0;
        if (ParseSpace(ctx, items[2], depth + 1, *alt) == Status::Ok && alt->family != Family::Indexed &&
            alt->family != Family::Separation && ParseTint(ctx.doc, items[3], out.tint, outputs) &&
            outputs == alt->components) {
            out.channels = alt->channels;
            out.alt = std::move(alt);
            return Status::Ok;
        }
        out.tint.clear();
    }
    bool process = true;
    bool colour = false;
    for (size_t i = 0; i < n; i++) {
        const int p = ProcessColorant(ctx.doc.Resolve(nameItems[i]).Name());
        if (p == -2) process = false;
        out.process[i] = static_cast<int8_t>(p < 0 ? -1 : p);
        if (p >= 0 && p < 3) colour = true;
    }
    // Black alone composes to the gray the summed tint gives.
    if (process && colour) {
        out.useProcess = true;
        out.channels = 3;
    }
    return Status::Ok;
}

Status ParseSpace(const SpaceContext& ctx, const PdfValue& value, int depth, Space& out) {
    if (depth > kMaxSpaceDepth) return Status::Corrupt;
    const PdfValue& cs = ctx.doc.Resolve(value);
    if (cs.IsName()) return ParseNamedSpace(ctx, cs.Name(), depth, out);
    if (!cs.IsArray() || cs.Array().empty()) return Status::Corrupt;
    const PdfSpan<PdfValue> items = cs.Array();
    const std::string_view family = ctx.doc.Resolve(items[0]).Name();
    if (items.size() == 1) return ParseNamedSpace(ctx, family, depth, out);
    if (family == "CalGray") {
        SetDevice(out, Family::Gray);
        return Status::Ok;
    }
    if (family == "CalRGB") {
        SetDevice(out, Family::Rgb);
        return Status::Ok;
    }
    if (family == "Lab") {
        SetDevice(out, Family::Lab);
        double range[4];
        if (Numbers(ctx.doc, ctx.doc.Get(items[1], "Range"), range, 4) && range[0] <= range[1] && range[2] <= range[3]) {
            std::copy(range, range + 4, out.range);
        }
        return Status::Ok;
    }
    if (family == "ICCBased") {
        const PdfValue& stream = ctx.doc.Resolve(items[1]);
        const int64_t n = ctx.doc.Get(stream, "N").Int(0);
        const PdfValue& alternate = ctx.doc.Get(stream, "Alternate");
        if (!alternate.IsNull()) {
            Space alt;
            if (ParseSpace(ctx, alternate, depth + 1, alt) == Status::Ok && alt.components == n &&
                alt.family != Family::Indexed && alt.family != Family::Separation) {
                out = std::move(alt);
                return Status::Ok;
            }
        }
        if (n == 1) SetDevice(out, Family::Gray);
        else if (n == 3) SetDevice(out, Family::Rgb);
        else if (n == 4) SetDevice(out, Family::Cmyk);
        else return Status::Unsupported;
        return Status::Ok;
    }
    if (family == "Indexed" || (ctx.inlineImage && family == "I")) return ParseIndexed(ctx, items, depth, out);
    if (family == "Separation") return ParseSeparation(ctx, items, false, depth, out);
    if (family == "DeviceN") return ParseSeparation(ctx, items, true, depth, out);
    if (family == "Pattern") return Status::Unsupported;
    if (family == "DeviceGray" || family == "DeviceRGB" || family == "DeviceCMYK" || family == "G" || family == "RGB" ||
        family == "CMYK") {
        return ParseNamedSpace(ctx, family, depth, out);
    }
    return Status::Unsupported;
}

// ---------------------------------------------------------------- image data

// Where an image's dictionary and data come from: an XObject stream, or an inline image.
struct Source {
    const Document& doc;
    const PdfValue& dict;
    const PdfValue& resources;
    bool inlineImage = false;
    std::string_view inlineData;
    // The data before its image filter when already read (a DCT image's JPEG), else empty.
    std::string_view encoded;
    // The conversion's threads for this image's work (image.h), or none.
    Parallel parallel;

    // Entry `full`, or for an inline image its abbreviation `brief`.
    const PdfValue& Entry(std::string_view full, std::string_view brief) const {
        const PdfValue& v = doc.Get(dict, full);
        if (!v.IsNull() || !inlineImage || brief.empty()) return v;
        return doc.Get(dict, brief);
    }
};

// The first filter of /Filter that is not decoded on the way to the image's samples (an image
// filter, Brotli or an unknown name); Kind::Flate when there is none.
Filters::Kind StopFilter(const Source& src) {
    const PdfValue& filter = src.Entry("Filter", "F");
    const PdfValue* items = &filter;
    size_t n = filter.IsName() ? 1 : 0;
    if (filter.IsArray()) {
        items = filter.Array().begin();
        n = filter.Array().size();
    }
    for (size_t i = 0; i < n; i++) {
        const Filters::Kind kind = Filters::KindOf(src.doc.Resolve(items[i]).Name());
        switch (kind) {
        case Filters::Kind::Flate:
        case Filters::Kind::Lzw:
        case Filters::Kind::Ascii85:
        case Filters::Kind::AsciiHex:
        case Filters::Kind::RunLength:
        case Filters::Kind::Crypt: continue;
        default: return kind;
        }
    }
    return Filters::Kind::Flate;
}

// The general-purpose filters of an inline image as stages, with their parameters; false past
// kMaxFilters.
bool InlineStages(const Source& src, Filters::Stage* stages, size_t& count) {
    const PdfValue& filter = src.Entry("Filter", "F");
    const PdfValue& parms = src.Entry("DecodeParms", "DP");
    const PdfValue* items = &filter;
    size_t n = filter.IsName() ? 1 : 0;
    if (filter.IsArray()) {
        items = filter.Array().begin();
        n = filter.Array().size();
    }
    if (n > kMaxFilters) return false;
    count = n;
    for (size_t i = 0; i < n; i++) {
        Filters::Stage& stage = stages[i];
        stage = Filters::Stage();
        stage.kind = Filters::KindOf(src.doc.Resolve(items[i]).Name());
        const PdfValue& p = parms.IsArray() ? (i < parms.Array().size() ? src.doc.Resolve(parms.Array()[i]) : parms)
                                            : parms;
        if (!p.IsDict()) continue;
        const auto number = [&](std::string_view key, int fallback) {
            const int64_t x = src.doc.Get(p, key).Int(fallback);
            return x < -1000000000 || x > 1000000000 ? fallback : static_cast<int>(x);
        };
        stage.params.predictor = number("Predictor", 1);
        stage.params.colors = number("Colors", 1);
        stage.params.bitsPerComponent = number("BitsPerComponent", 8);
        stage.params.columns = number("Columns", 1);
        stage.params.earlyChange = number("EarlyChange", 1);
    }
    return true;
}

// An inline image's data run through its general-purpose filters (up to the first image filter),
// at most `limit` bytes.
StreamStatus InlineData(const Source& src, std::string& out, size_t limit) {
    out.clear();
    Filters::Stage stages[kMaxFilters];
    size_t count = 0;
    if (!InlineStages(src, stages, count)) return StreamStatus::Unsupported;
    Filters::Chain chain;
    chain.Start(stages, count, src.inlineData, limit);
    std::string_view piece;
    while (chain.Next(piece)) out.append(piece.data(), piece.size());
    return chain.Status();
}

StreamStatus ReadData(const Source& src, std::string& out, size_t limit) {
    if (src.inlineImage) return InlineData(src, out, limit);
    return src.doc.StreamData(src.dict, out, limit);
}

bool Usable(StreamStatus status, const std::string& data) {
    return status != StreamStatus::Corrupt && status != StreamStatus::NotStream && !data.empty();
}

uint32_t Be16(const uint8_t* p) { return static_cast<uint32_t>(p[0]) << 8 | p[1]; }
uint32_t Be32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | Be16(p + 2); }

void PutBe32(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>(v >> 24));
    out.push_back(static_cast<char>(v >> 16));
    out.push_back(static_cast<char>(v >> 8));
    out.push_back(static_cast<char>(v));
}

// The size in a JPEG's first SOF marker segment; false when there is none in the data.
bool JpegSize(std::string_view jpeg, uint32_t& width, uint32_t& height) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(jpeg.data());
    size_t at = 2;
    while (at + 4 <= jpeg.size()) {
        if (p[at] != 0xFF) return false;
        const uint8_t marker = p[at + 1];
        if (marker == 0xFF) {
            at++;
            continue;
        }
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            at += 2;
            continue;
        }
        const size_t length = Be16(p + at + 2);
        const bool sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (sof) {
            if (at + 9 > jpeg.size()) return false;
            height = Be16(p + at + 5);
            width = Be16(p + at + 7);
            return true;
        }
        if (length < 2 || marker == 0xDA || marker == 0xD9) return false;
        at += 2 + length;
    }
    return false;
}

// A DCT image's JPEG from its SOI marker on: writers occasionally put bytes before it, which
// viewers skip and a file must not start with. Empty when the first 1024 bytes have none.
std::string_view JpegStart(std::string_view data) {
    const size_t soi = data.substr(0, 1024).find("\xFF\xD8");
    return soi == std::string_view::npos ? std::string_view() : data.substr(soi);
}

Status PassJpeg(std::string_view data, uint32_t width, uint32_t height, Encoded& out) {
    data = JpegStart(data);
    if (data.empty()) return Status::Corrupt;
    uint32_t w = 0, h = 0;
    if (JpegSize(data, w, h) && w > 0 && h > 0) {
        width = w;
        height = h;
    }
    out.bytes.assign(data.data(), data.size());
    out.extension = "jpg";
    out.width = width;
    out.height = height;
    return Status::Ok;
}

// A JP2 file (ISO/IEC 15444-1 Annex I) as stored, or a raw codestream wrapped in the boxes a .jp2
// needs: signature, file type, header (image header, bit depths when they differ, colour) and the
// codestream.
Status PassJpx(std::string_view data, uint32_t width, uint32_t height, Encoded& out) {
    static constexpr char kSignature[12] = {0, 0, 0, 12, 'j', 'P', ' ', ' ', '\r', '\n', '\x87', '\n'};
    out.extension = "jp2";
    out.width = width;
    out.height = height;
    if (data.size() >= 12 && std::memcmp(data.data(), kSignature, 12) == 0) {
        out.bytes.assign(data.data(), data.size());
        return Status::Ok;
    }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
    // SOC, then SIZ: Lsiz Rsiz Xsiz Ysiz XOsiz YOsiz XTsiz YTsiz XTOsiz YTOsiz Csiz, then per
    // component Ssiz XRsiz YRsiz.
    if (data.size() < 42 || Be16(p) != 0xFF4F || Be16(p + 2) != 0xFF51) return Status::Corrupt;
    const uint32_t xsiz = Be32(p + 8), ysiz = Be32(p + 12), xo = Be32(p + 16), yo = Be32(p + 20);
    const uint32_t components = Be16(p + 40);
    if (xsiz <= xo || ysiz <= yo || components == 0 || components > 16384 || data.size() < 42 + 3 * size_t(components)) {
        return Status::Corrupt;
    }
    out.width = xsiz - xo;
    out.height = ysiz - yo;
    bool same = true;
    for (uint32_t c = 1; c < components; c++) same = same && p[42 + 3 * c] == p[42];
    out.bytes.assign(kSignature, sizeof kSignature);
    out.bytes.append("\0\0\0\x14" "ftypjp2 \0\0\0\0jp2 ", 20);
    const uint32_t bpcc = same ? 0 : 8 + components;
    PutBe32(out.bytes, 8 + 22 + bpcc + 15);
    out.bytes += "jp2h";
    PutBe32(out.bytes, 22);
    out.bytes += "ihdr";
    PutBe32(out.bytes, out.height);
    PutBe32(out.bytes, out.width);
    out.bytes.push_back(static_cast<char>(components >> 8));
    out.bytes.push_back(static_cast<char>(components));
    out.bytes.push_back(static_cast<char>(same ? p[42] : 0xFF));
    out.bytes.append("\x07\0\0", 3);  // wavelet compression, colour space known, no IPR
    if (!same) {
        PutBe32(out.bytes, bpcc);
        out.bytes += "bpcc";
        for (uint32_t c = 0; c < components; c++) out.bytes.push_back(static_cast<char>(p[42 + 3 * c]));
    }
    PutBe32(out.bytes, 15);
    out.bytes += "colr";
    out.bytes.append("\x01\0\0", 3);  // enumerated colour space
    PutBe32(out.bytes, components == 4 ? 12 : components < 3 ? 17 : 16);  // CMYK, gray, sRGB
    PutBe32(out.bytes, static_cast<uint32_t>(std::min<size_t>(data.size() + 8, 0xFFFFFFFFu)));
    out.bytes += "jp2c";
    out.bytes.append(data.data(), data.size());
    return Status::Ok;
}

// ---------------------------------------------------------------- samples to pixels

struct Layout {
    uint32_t width = 0;
    uint32_t height = 0;
    int bpc = 8;
    int components = 1;
    size_t rowBytes = 0;
};

// Width, height and the bits per component of `src`; Corrupt, TooLarge or Ok.
Status ReadLayout(const Source& src, bool mask, int components, uint64_t maxPixels, Layout& out) {
    const int64_t w = src.Entry("Width", "W").Int(0);
    const int64_t h = src.Entry("Height", "H").Int(0);
    if (w <= 0 || h <= 0 || w > kMaxDimension || h > kMaxDimension) return Status::Corrupt;
    if (uint64_t(w) * uint64_t(h) > maxPixels) return Status::TooLarge;
    int64_t bpc = mask ? 1 : src.Entry("BitsPerComponent", "BPC").Int(0);
    if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16) return Status::Corrupt;
    out.width = static_cast<uint32_t>(w);
    out.height = static_cast<uint32_t>(h);
    out.bpc = static_cast<int>(bpc);
    out.components = components;
    const uint64_t rowBits = uint64_t(w) * static_cast<uint64_t>(components) * static_cast<uint64_t>(bpc);
    const uint64_t rowBytes = (rowBits + 7) / 8;
    // The samples come through the document's stream limit whole or not at all.
    if (rowBytes > Document::kDefaultStreamLimit / uint64_t(h)) return Status::TooLarge;
    out.rowBytes = static_cast<size_t>(rowBytes);
    return Status::Ok;
}

// The /DecodeParms entry of the filter StopFilter names (resolved): the entry at its index of a
// /DecodeParms array, or a /DecodeParms dictionary; null when there is none.
const PdfValue& StopParms(const Source& src) {
    static const PdfValue kNone{};
    const PdfValue& filter = src.Entry("Filter", "F");
    const PdfValue& parms = src.Entry("DecodeParms", "DP");
    const PdfValue* items = &filter;
    size_t n = filter.IsName() ? 1 : 0;
    if (filter.IsArray()) {
        items = filter.Array().begin();
        n = filter.Array().size();
    }
    for (size_t i = 0; i < n; i++) {
        switch (Filters::KindOf(src.doc.Resolve(items[i]).Name())) {
        case Filters::Kind::Flate:
        case Filters::Kind::Lzw:
        case Filters::Kind::Ascii85:
        case Filters::Kind::AsciiHex:
        case Filters::Kind::RunLength:
        case Filters::Kind::Crypt: continue;
        default:
            if (parms.IsArray()) return i < parms.Array().size() ? src.doc.Resolve(parms.Array()[i]) : kNone;
            return parms.IsDict() ? parms : kNone;
        }
    }
    return kNone;
}

bool Bitonal(Filters::Kind stop) { return stop == Filters::Kind::Ccitt || stop == Filters::Kind::Jbig2; }

Ccitt::Params CcittParams(const Document& doc, const PdfValue& p) {
    Ccitt::Params c;
    if (!p.IsDict()) return c;
    const auto number = [&](std::string_view key, int64_t fallback, int64_t low, int64_t high) {
        const int64_t v = doc.Get(p, key).Int(fallback);
        return v < low ? low : v > high ? high : v;
    };
    c.k = static_cast<int32_t>(number("K", 0, -1000000, 1000000));
    c.endOfLine = doc.Get(p, "EndOfLine").Bool(false);
    c.encodedByteAlign = doc.Get(p, "EncodedByteAlign").Bool(false);
    c.columns = static_cast<uint32_t>(number("Columns", 1728, 0, kMaxDimension));
    c.rows = static_cast<uint32_t>(number("Rows", 0, 0, kMaxDimension));
    c.endOfBlock = doc.Get(p, "EndOfBlock").Bool(true);
    c.blackIs1 = doc.Get(p, "BlackIs1").Bool(false);
    c.damagedRowsBeforeError = static_cast<uint32_t>(number("DamagedRowsBeforeError", 0, 0, kMaxDimension));
    return c;
}

// The rows of a CCITT or JBIG2 image (1 bit a pixel, `layout`) through its decoder, whole rows of
// layout.rowBytes. What a damaged stream decoded before the damage is kept, and rows the data
// lacks are white (in the filter's polarity), as poppler shows them.
Status DecodeBitonal(const Source& src, Filters::Kind stop, const Layout& layout, std::string& data, uint32_t& rows) {
    std::string encoded;
    const StreamStatus read = ReadData(src, encoded, kPassThroughLimit);
    if (!Usable(read, encoded)) return read == StreamStatus::TooLarge ? Status::TooLarge : Status::Corrupt;
    if (read == StreamStatus::TooLarge) return Status::TooLarge;
    std::string decoded;
    uint8_t white = 0xFF;
    if (stop == Filters::Kind::Ccitt) {
        const Ccitt::Params params = CcittParams(src.doc, StopParms(src));
        Ccitt::Info info;
        // The filter's rows are /Columns wide, which may pass /Width: its output is bounded by
        // those rows, /Height of them, and the general pixel limit.
        const uint64_t maxPixels =
            std::min<uint64_t>(kDefaultMaxPixels, uint64_t(std::max(params.columns, layout.width)) * layout.height);
        const Ccitt::Result result = Ccitt::Decode(reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size(), params,
                                                   layout.height, maxPixels, decoded, info);
        if (result == Ccitt::Result::InvalidParams) return Status::Corrupt;
        if (result != Ccitt::Result::Ok && info.rows == 0) {
            return result == Ccitt::Result::Unsupported ? Status::Unsupported
                   : result == Ccitt::Result::TooLarge  ? Status::TooLarge
                                                        : Status::Corrupt;
        }
        white = params.blackIs1 ? 0x00 : 0xFF;
    } else {
        // /JBIG2Globals: a stream of the segments shared by the document's JBIG2 images.
        std::string globals;
        const PdfValue& globalStream = src.doc.Get(StopParms(src), "JBIG2Globals");
        if (globalStream.IsStream()) {
            const StreamStatus g = src.doc.StreamData(globalStream, globals, kPassThroughLimit);
            if (g == StreamStatus::Corrupt || g == StreamStatus::NotStream) globals.clear();
        }
        // A page a little larger than /Width x /Height is seen (one column more); more is no image.
        const uint64_t maxPixels = std::min<uint64_t>(kDefaultMaxPixels, 4 * uint64_t(layout.width) * layout.height + (1u << 20));
        Jbig2::Info info;
        const Jbig2::Result result =
            Jbig2::Decode(reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size(),
                          reinterpret_cast<const uint8_t*>(globals.data()), globals.size(), maxPixels, info, decoded);
        if (result != Jbig2::Result::Ok) {
            return result == Jbig2::Result::Unsupported ? Status::Unsupported
                   : result == Jbig2::Result::TooLarge  ? Status::TooLarge
                                                        : Status::Corrupt;
        }
    }
    // The filter's output is a byte stream that the image reads as rows of its own (as poppler
    // does, whatever the filter's row length: /Columns, or the JBIG2 page's width).
    if (decoded.empty()) return Status::Corrupt;
    data.swap(decoded);
    data.resize(layout.rowBytes * layout.height, static_cast<char>(white));
    rows = layout.height;
    return Status::Ok;
}

// The rows of an image's samples, top to bottom, one at a time: decoded from its data a piece at a
// time (Flate-family data: no whole copy of it held), or from samples a decoder made whole (DCT,
// JPX, CCITT, JBIG2). The rows present are those the data holds, at most the image's height, a
// last partial one completed with zero bytes; how many is known once a pass has read them all.
class SampleRows {
public:
    // Samples made whole: `rows` rows of `rowBytes` bytes.
    void SetWhole(std::string data, size_t rowBytes, uint32_t rows) {
        whole_ = std::move(data);
        streamed_ = false;
        rowBytes_ = rowBytes;
        height_ = rows;
        known_ = true;
        rows_ = rows;
        next_ = 0;
    }

    // The data of `src` (no image filter) in rows of `layout`: Unsupported, TooLarge or Corrupt
    // when there are none (as the data read whole gave them), else Ok.
    Status Open(const Source& src, const Layout& layout) {
        streamed_ = true;
        inline_ = src.inlineImage;
        rowBytes_ = layout.rowBytes;
        height_ = layout.height;
        known_ = false;
        rows_ = 0;
        next_ = 0;
        ended_ = false;
        piece_ = std::string_view();
        // Slack past the samples, so that data a little longer than needed is no limit hit.
        const size_t limit = layout.rowBytes * layout.height + 4096;
        StreamStatus open = StreamStatus::Ok;
        if (inline_) {
            Filters::Stage stages[kMaxFilters];
            size_t count = 0;
            if (!InlineStages(src, stages, count)) return Status::Unsupported;
            chain_.Start(stages, count, src.inlineData, limit);
        } else {
            open = reader_.Open(src.doc, src.dict, limit);
        }
        if (open == StreamStatus::Unsupported) return Status::Unsupported;
        if (open != StreamStatus::Ok) return open == StreamStatus::TooLarge ? Status::TooLarge : Status::Corrupt;
        if (!NextPiece()) {
            const StreamStatus read = inline_ ? chain_.Status() : reader_.Finish();
            if (read == StreamStatus::Unsupported) return Status::Unsupported;
            return read == StreamStatus::TooLarge ? Status::TooLarge : Status::Corrupt;
        }
        rowBuffers_[0].resize(rowBytes_);
        rowBuffers_[1].resize(rowBytes_);
        return Status::Ok;
    }

    // The next row, valid until the call after next; null past the rows present.
    const uint8_t* Next() {
        if (!streamed_) {
            if (next_ >= rows_) return nullptr;
            return reinterpret_cast<const uint8_t*>(whole_.data()) + size_t(next_++) * rowBytes_;
        }
        if (ended_ || next_ >= height_) return End();
        uint8_t* row = rowBuffers_[next_ & 1].data();
        size_t fill = 0;
        while (fill < rowBytes_) {
            if (piece_.empty() && !NextPiece()) break;
            const size_t take = std::min(rowBytes_ - fill, piece_.size());
            std::memcpy(row + fill, piece_.data(), take);
            fill += take;
            piece_.remove_prefix(take);
        }
        if (fill == 0) return End();
        if (fill < rowBytes_) {
            std::memset(row + fill, 0, rowBytes_ - fill);
            ended_ = true;
        }
        next_++;
        return row;
    }

    // From the first row again (a second pass over the data).
    void Rewind() {
        next_ = 0;
        if (!streamed_) return;
        ended_ = false;
        piece_ = std::string_view();
        if (inline_) chain_.Restart();
        else reader_.Restart();
    }

    bool RowsKnown() const { return known_; }
    uint32_t Rows() const { return rows_; }

private:
    bool NextPiece() { return inline_ ? chain_.Next(piece_) : reader_.Next(piece_); }

    const uint8_t* End() {
        ended_ = true;
        if (!known_) {
            known_ = true;
            rows_ = next_;
        }
        return nullptr;
    }

    std::string whole_;
    bool streamed_ = false;
    bool inline_ = false;
    Document::StreamReader reader_;  // an XObject's data, counted in the document's report
    Filters::Chain chain_;           // an inline image's data
    std::string_view piece_;         // decoded data not yet in a row
    std::vector<uint8_t> rowBuffers_[2];
    size_t rowBytes_ = 0;
    uint32_t height_ = 0;
    uint32_t next_ = 0;   // rows given in this pass
    uint32_t rows_ = 0;   // rows present, once known
    bool known_ = false;
    bool ended_ = false;
};

// The samples of `layout` as rows (SampleRows): CCITT and JBIG2 data through their decoders
// (DecodeBitonal), other data decoded as it is read.
Status OpenSamples(const Source& src, const Layout& layout, SampleRows& rows) {
    const Filters::Kind stop = StopFilter(src);
    if (!Bitonal(stop)) return rows.Open(src, layout);
    std::string data;
    uint32_t count = 0;
    const Status status = DecodeBitonal(src, stop, layout, data, count);
    if (status == Status::Ok) rows.SetWhole(std::move(data), layout.rowBytes, count);
    return status;
}

// The samples of one row as codes of at most 8 bits: 16-bit samples rounded to 8.
void UnpackRow(const uint8_t* row, size_t samples, int bpc, uint8_t* codes) {
    switch (bpc) {
    case 8: std::memcpy(codes, row, samples); return;
    case 16:
        for (size_t i = 0; i < samples; i++) codes[i] = static_cast<uint8_t>((Be16(row + 2 * i) * 255 + 32767) / 65535);
        return;
    default: {
        const unsigned mask = (1u << bpc) - 1;
        const size_t perByte = static_cast<size_t>(8 / bpc);
        for (size_t i = 0; i < samples; i++) {
            const unsigned shift = static_cast<unsigned>(8 - bpc * static_cast<int>(i % perByte + 1));
            codes[i] = static_cast<uint8_t>((row[i / perByte] >> shift) & mask);
        }
        return;
    }
    }
}

// The value of each code of each component under /Decode (or the space's default decode).
struct DecodeMap {
    int codeBits = 8;
    double low[kMaxComponents];
    double high[kMaxComponents];
    bool identity = true;  // [0 1] per component of a [0, 1] space, 8 bits

    double Value(int component, unsigned code) const {
        const double maxCode = static_cast<double>((1u << codeBits) - 1);
        return low[component] + code * (high[component] - low[component]) / maxCode;
    }
};

// `useDecode` false ignores /Decode (JPX images, ISO 32000-1 Table 89).
void MakeDecodeMap(const Source& src, const Space& space, int bpc, DecodeMap& map, bool useDecode = true) {
    map.codeBits = bpc > 8 ? 8 : bpc;
    const int n = space.components;
    for (int i = 0; i < n; i++) {
        map.low[i] = 0;
        map.high[i] = 1;
    }
    if (space.family == Family::Lab) {
        map.high[0] = 100;
        map.low[1] = space.range[0];
        map.high[1] = space.range[1];
        map.low[2] = space.range[2];
        map.high[2] = space.range[3];
    } else if (space.family == Family::Indexed) {
        map.high[0] = static_cast<double>((1u << map.codeBits) - 1);
    }
    double decode[2 * kMaxComponents];
    if (useDecode && NumberPairs(src.doc, src.Entry("Decode", "D"), decode) == static_cast<size_t>(2 * n)) {
        for (int i = 0; i < n; i++) {
            map.low[i] = decode[2 * i];
            map.high[i] = decode[2 * i + 1];
        }
        // A 16-bit Indexed image's /Decode is in 16-bit index units; its codes are 8-bit.
        if (space.family == Family::Indexed && bpc == 16) {
            map.low[0] /= 257;
            map.high[0] /= 257;
        }
    }
    map.identity = map.codeBits == 8 && (space.family == Family::Gray || space.family == Family::Rgb);
    for (int i = 0; i < n && map.identity; i++) map.identity = map.low[i] == 0 && map.high[i] == 1;
}

// Rows of samples to rows of 8-bit pixels of a colour space under /Decode, one at a time: a code's
// colour from a table made once, Lab and DeviceN a pixel at a time with a run of equal samples
// converted once; 8-bit gray and RGB samples under the default /Decode are their own pixels. Or a
// stencil's bits as gray.
class Converter {
public:
    void Init(const Layout& layout, const Space& space, const DecodeMap& map) {
        layout_ = layout;
        space_ = &space;
        n_ = static_cast<size_t>(space.components);
        ch_ = static_cast<size_t>(space.channels);
        if (map.identity && layout.bpc == 8) {
            mode_ = Mode::AsIs;
            return;
        }
        mode_ = n_ == 1 ? Mode::Single
                : space.family == Family::Rgb || space.family == Family::Cmyk ? Mode::Components
                                                                               : Mode::Values;
        Prepare();
        codes_.resize(size_t(layout.width) * n_);
        codeCount_ = size_t(1) << map.codeBits;
        if (mode_ == Mode::Single) {
            // Every code's colour once.
            lut_.resize(codeCount_ * ch_);
            for (size_t code = 0; code < codeCount_; code++) {
                const double v = map.Value(0, static_cast<unsigned>(code));
                Convert(space, &v, lut_.data() + code * ch_);
            }
        } else if (mode_ == Mode::Components) {
            // Per component, each code's 8-bit value.
            lut_.resize(n_ * codeCount_);
            for (size_t c = 0; c < n_; c++) {
                for (size_t code = 0; code < codeCount_; code++) {
                    lut_[c * codeCount_ + code] = To8(map.Value(static_cast<int>(c), static_cast<unsigned>(code)));
                }
            }
        } else {
            values_.resize(n_ * codeCount_);
            for (size_t c = 0; c < n_; c++) {
                for (size_t code = 0; code < codeCount_; code++) {
                    values_[c * codeCount_ + code] = map.Value(static_cast<int>(c), static_cast<unsigned>(code));
                }
            }
            last_.assign(n_, 0);
        }
    }

    // A stencil's bits as gray: 0 where the bit is `painted`, 255 elsewhere.
    void InitStencil(const Layout& layout, uint8_t painted) {
        layout_ = layout;
        ch_ = 1;
        mode_ = Mode::Stencil;
        painted_ = painted;
        Prepare();
    }

    // Samples that are their own pixels.
    void InitAsIs() { mode_ = Mode::AsIs; }

    // The pixels of a row of samples: the row itself, or a row of the converter's that stays valid
    // until the call after next.
    const uint8_t* Row(const uint8_t* row) {
        if (mode_ == Mode::AsIs) return row;
        parity_ ^= 1;
        uint8_t* target = rows_[parity_].data();
        const size_t width = layout_.width;
        switch (mode_) {
        case Mode::Stencil:
            for (size_t x = 0; x < width; x++) {
                const uint8_t bit = (row[x >> 3] >> (7 - (x & 7))) & 1;
                target[x] = bit == painted_ ? 0 : 255;
            }
            break;
        case Mode::Single:
            UnpackRow(row, width, layout_.bpc, codes_.data());
            if (ch_ == 1) {
                for (size_t x = 0; x < width; x++) target[x] = lut_[codes_[x]];
            } else {
                for (size_t x = 0; x < width; x++) std::memcpy(target + 3 * x, lut_.data() + 3 * codes_[x], 3);
            }
            break;
        case Mode::Components: {
            UnpackRow(row, width * n_, layout_.bpc, codes_.data());
            const uint8_t* code = codes_.data();
            const size_t count = codeCount_;
            uint8_t* out = target;
            if (n_ == 3) {
                for (size_t x = 0; x < width; x++, code += 3, out += 3) {
                    out[0] = lut_[code[0]];
                    out[1] = lut_[count + code[1]];
                    out[2] = lut_[2 * count + code[2]];
                }
            } else {
                for (size_t x = 0; x < width; x++, code += 4, out += 3) {
                    const unsigned white = 255u - lut_[3 * count + code[3]];
                    out[0] = static_cast<uint8_t>(((255u - lut_[code[0]]) * white + 127) / 255);
                    out[1] = static_cast<uint8_t>(((255u - lut_[count + code[1]]) * white + 127) / 255);
                    out[2] = static_cast<uint8_t>(((255u - lut_[2 * count + code[2]]) * white + 127) / 255);
                }
            }
            break;
        }
        default:
            UnpackRow(row, width * n_, layout_.bpc, codes_.data());
            for (size_t x = 0; x < width; x++) {
                const uint8_t* code = codes_.data() + x * n_;
                if (!haveLast_ || std::memcmp(code, last_.data(), n_) != 0) {
                    double v[kMaxComponents];
                    for (size_t c = 0; c < n_; c++) v[c] = values_[c * codeCount_ + code[c]];
                    Convert(*space_, v, lastPixel_);
                    std::memcpy(last_.data(), code, n_);
                    haveLast_ = true;
                }
                std::memcpy(target + x * ch_, lastPixel_, ch_);
            }
            break;
        }
        return target;
    }

private:
    enum class Mode : uint8_t { AsIs, Stencil, Single, Components, Values };

    void Prepare() {
        rows_[0].resize(size_t(layout_.width) * ch_);
        rows_[1].resize(size_t(layout_.width) * ch_);
    }

    Mode mode_ = Mode::AsIs;
    Layout layout_;
    const Space* space_ = nullptr;
    size_t n_ = 1;
    size_t ch_ = 1;
    size_t codeCount_ = 0;
    uint8_t painted_ = 0;
    std::vector<uint8_t> codes_;
    std::vector<uint8_t> lut_;
    std::vector<double> values_;
    std::vector<uint8_t> last_;
    uint8_t lastPixel_[3] = {0, 0, 0};
    bool haveLast_ = false;
    std::vector<uint8_t> rows_[2];
    unsigned parity_ = 0;
};

// An image's pixels a row at a time, top to bottom: its samples (SampleRows) through its
// Converter, or as stored when packed; a soft mask in colour gives its luminance. What
// DecodeSampled and DecodeStencil decode, so that no image-sized buffer of samples or pixels is
// held for Flate-family data.
struct ImageRows {
    SampleRows samples;
    Layout layout;
    Space space;
    Converter converter;
    uint32_t width = 0;  // 0: nothing decoded
    uint32_t declaredHeight = 0;
    int channels = 1;
    // Gray kept as stored, `packedBits` (1, 2 or 4) bits a sample: only when the caller allows it
    // (SampleRequest::allowPacked) and the samples are a PNG's gray of that depth, or
    // (`packedInvert`) its complement. 0: 8-bit pixels.
    int packedBits = 0;
    bool packedInvert = false;
    bool luminance = false;  // RGB pixels given as their luminance (a soft mask in colour)
    // A /Mask colour key (ISO 32000-1 8.9.6.4): per component the range of samples as stored
    // (before /Decode) where the image is transparent.
    bool keyed = false;
    double key[2 * kMaxComponents] = {};

    // The next row's pixels (width x channels bytes; packed: the stored row), valid until the call
    // after next; null past the rows the data holds.
    const uint8_t* Next() {
        current = samples.Next();
        if (!current || packedBits != 0) return current;
        const uint8_t* pixels = converter.Row(current);
        if (!luminance) return pixels;
        parity ^= 1;
        std::vector<uint8_t>& gray = grayRows[parity];
        gray.resize(width);
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t* p = pixels + 3 * size_t(x);
            gray[x] = static_cast<uint8_t>((p[0] * 77u + p[1] * 150u + p[2] * 29u) >> 8);
        }
        return gray.data();
    }

    // The colour key's alpha of the row Next gave last: 0 where every sample lies in its
    // component's range, else 255.
    const uint8_t* KeyAlpha() {
        keyRow.resize(width);
        const size_t n = static_cast<size_t>(layout.components);
        const unsigned sampleMask = layout.bpc == 16 ? 0xFFFFu : (1u << layout.bpc) - 1;
        for (size_t x = 0; x < width; x++) {
            bool inside = true;
            for (size_t c = 0; c < n && inside; c++) {
                const size_t i = x * n + c;
                unsigned sample;
                if (layout.bpc == 16) {
                    sample = Be16(current + 2 * i);
                } else if (layout.bpc == 8) {
                    sample = current[i];
                } else {
                    const size_t bit = i * static_cast<size_t>(layout.bpc);
                    sample = (current[bit >> 3] >> (8 - static_cast<size_t>(layout.bpc) - (bit & 7))) & sampleMask;
                }
                inside = sample >= key[2 * c] && sample <= key[2 * c + 1];
            }
            keyRow[x] = inside ? 0 : 255;
        }
        return keyRow.data();
    }

    // From the first row again.
    void Rewind() { samples.Rewind(); }
    bool RowsKnown() const { return samples.RowsKnown(); }
    uint32_t Rows() const { return samples.Rows(); }

    const uint8_t* current = nullptr;  // the samples of the row Next gave last
    std::vector<uint8_t> grayRows[2];
    unsigned parity = 0;
    std::vector<uint8_t> keyRow;
};

// A stencil (ImageMask) as gray: 0 where it paints, 255 elsewhere. With `packed`, kept as its
// bits (gray bit * 255, complemented where 1 paints).
Status DecodeStencil(const Source& src, uint64_t maxPixels, ImageRows& out, bool packed = false) {
    const Filters::Kind stop = StopFilter(src);
    if (stop != Filters::Kind::Flate && !Bitonal(stop)) return Status::Unsupported;
    Layout& layout = out.layout;
    Status status = ReadLayout(src, true, 1, maxPixels, layout);
    if (status != Status::Ok) return status;
    if ((status = OpenSamples(src, layout, out.samples)) != Status::Ok) return status;
    // /Decode [0 1] (the default): 0 paints. [1 0]: 1 paints.
    double decode[2];
    const bool inverted = Numbers(src.doc, src.Entry("Decode", "D"), decode, 2) && decode[0] > decode[1];
    out.width = layout.width;
    out.declaredHeight = layout.height;
    out.channels = 1;
    if (packed) {
        out.packedBits = 1;
        out.packedInvert = inverted;
        return Status::Ok;
    }
    out.converter.InitStencil(layout, inverted ? 1 : 0);
    return Status::Ok;
}

// The /Mask colour key of `src` for `layout`'s components into `key` (two numbers a component);
// false without such a /Mask array.
bool ColourKey(const Source& src, const Layout& layout, double* key) {
    const PdfValue& mask = src.doc.Get(src.dict, "Mask");
    return mask.IsArray() && NumberPairs(src.doc, mask, key) == 2 * static_cast<size_t>(layout.components);
}

// A DCT image's samples through the JPEG decoder (jpeg.h): the layout is the JPEG's (8 bits, its
// components), and a colour space of another number of components gives way to the device space
// of the JPEG's, as viewers do.
Status DecodeJpegSamples(const Source& src, uint64_t maxPixels, Space& space, Layout& layout, std::string& samples) {
    std::string data;
    std::string_view jpeg = src.encoded;
    if (jpeg.empty()) {
        const StreamStatus read = ReadData(src, data, kPassThroughLimit);
        if (!Usable(read, data)) return read == StreamStatus::TooLarge ? Status::TooLarge : Status::Corrupt;
        if (read == StreamStatus::TooLarge) return Status::TooLarge;
        jpeg = JpegStart(data);
        if (jpeg.empty()) return Status::Corrupt;
    }
    Jpeg::Info info;
    // Its samples within the stream limit that bounds every other image's (4 components each).
    const uint64_t limit = std::min<uint64_t>(maxPixels, Document::kDefaultStreamLimit / 4);
    switch (Jpeg::Decode(reinterpret_cast<const uint8_t*>(jpeg.data()), jpeg.size(), limit, info, samples)) {
    case Jpeg::Result::Ok: break;
    case Jpeg::Result::Unsupported: return Status::Unsupported;
    case Jpeg::Result::TooLarge: return Status::TooLarge;
    case Jpeg::Result::Corrupt: return Status::Corrupt;
    }
    if (info.components != space.components) {
        space = Space();
        SetDevice(space, info.components == 1 ? Family::Gray : info.components == 3 ? Family::Rgb : Family::Cmyk);
    }
    layout.width = info.width;
    layout.height = info.height;
    layout.bpc = 8;
    layout.components = info.components;
    layout.rowBytes = size_t(info.width) * static_cast<size_t>(info.components);
    return Status::Ok;
}

// What a sampled image's decode is asked for, besides the samples.
struct SampleRequest {
    uint64_t maxPixels = kDefaultMaxPixels;
    bool softMask = false;                // a /SMask: DeviceGray when it has no /ColorSpace
    bool colourKey = false;               // alpha from a /Mask colour key (ImageRows::KeyAlpha)
    ImageRows* inDataAlpha = nullptr;     // given a JPX opacity channel (/SMaskInData)
    uint32_t longerSide = 0;  // JPX: decode at a reduced resolution down to about this (0: full)
    bool allowPacked = false; // gray of 1, 2 or 4 bits that is a PNG's may stay packed (ImageRows)
};

// A JPX image's samples through the JPEG 2000 decoder (jpx.h): the layout is the decoded
// image's (8 or 16 bits, its colour channels). Without a /ColorSpace, or with one of another
// number of components, the JP2 header's colour space or the device space of as many channels
// stands in. An opacity channel (/SMaskInData) goes to `alpha` (premultiplied colour divided by
// it). `fullWidth` x `fullHeight`: the image's size before any reduced-resolution decoding.
Status DecodeJpxSamples(const Source& src, const SampleRequest& req, bool spaceGiven, Space& space, Layout& layout,
                        std::string& samples, uint32_t& fullWidth, uint32_t& fullHeight) {
    std::string data;
    std::string_view jpx = src.encoded;
    if (jpx.empty()) {
        const StreamStatus read = ReadData(src, data, kPassThroughLimit);
        if (!Usable(read, data)) return read == StreamStatus::TooLarge ? Status::TooLarge : Status::Corrupt;
        if (read == StreamStatus::TooLarge) return Status::TooLarge;
        jpx = data;
    }
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(jpx.data());
    Jpx::Options options;
    options.maxPixels = std::min<uint64_t>(req.maxPixels, Document::kDefaultStreamLimit / 4);
    options.ignorePalette = spaceGiven && space.family == Family::Indexed;
    options.pdfComponents = spaceGiven ? space.components : 0;
    options.threads = src.parallel.threads;
    options.worker = src.parallel.worker;
    if (req.inDataAlpha && !src.inlineImage) {
        const int64_t inData = src.doc.Get(src.dict, "SMaskInData").Int(0);
        options.smaskInData = inData == 1 || inData == 2 ? static_cast<int>(inData) : 0;
    }
    if (req.longerSide > 0) {
        // The most levels to drop with the longer side still at least `longerSide`.
        Jpx::Info info;
        if (Jpx::ReadInfo(bytes, jpx.size(), info) == Jpx::Result::Ok) {
            const uint64_t longer = std::max(info.width, info.height);
            while (options.reduce < info.levels &&
                   ((longer + (uint64_t(2) << options.reduce) - 1) >> (options.reduce + 1)) >= req.longerSide) {
                options.reduce++;
            }
        }
    }
    Jpx::Image image;
    switch (Jpx::Decode(bytes, jpx.size(), options, image)) {
    case Jpx::Result::Ok: break;
    case Jpx::Result::Unsupported: return Status::Unsupported;
    case Jpx::Result::TooLarge: return Status::TooLarge;
    case Jpx::Result::Corrupt: return Status::Corrupt;
    }
    const int n = image.colourChannels;
    if (!spaceGiven || n != space.components) {
        if (n != 1 && n != 3 && n != 4) return Status::Unsupported;
        space = Space();
        SetDevice(space, n == 1 ? Family::Gray : n == 3 ? Family::Rgb : Family::Cmyk);
    }
    fullWidth = image.info.width;
    fullHeight = image.info.height;
    const size_t depth = image.bitsPerComponent == 16 ? 2 : 1;
    layout.width = image.width;
    layout.height = image.height;
    layout.bpc = image.bitsPerComponent;
    layout.components = n;
    layout.rowBytes = size_t(image.width) * static_cast<size_t>(n) * depth;
    if (!image.opacity || !req.inDataAlpha) {
        if (image.channels != n) return Status::Corrupt;
        samples.swap(image.samples);
        return Status::Ok;
    }
    // The opacity channel, last, apart: 8 bits of alpha, and the colour without it.
    const size_t pixels = size_t(image.width) * image.height;
    const size_t in = (static_cast<size_t>(n) + 1) * depth, colour = static_cast<size_t>(n) * depth;
    if (image.samples.size() < pixels * in) return Status::Corrupt;
    std::string opacityPlane(pixels, '\0');
    samples.resize(pixels * colour);
    const uint8_t* source = reinterpret_cast<const uint8_t*>(image.samples.data());
    uint8_t* target = reinterpret_cast<uint8_t*>(&samples[0]);
    uint8_t* opacity = reinterpret_cast<uint8_t*>(&opacityPlane[0]);
    const uint32_t full = depth == 2 ? 65535u : 255u;
    for (size_t i = 0; i < pixels; i++, source += in, target += colour) {
        std::memcpy(target, source, colour);
        const uint32_t a = depth == 2 ? (uint32_t(source[colour]) << 8 | source[colour + 1]) : source[colour];
        opacity[i] = source[colour];  // the high byte
        if (!image.premultiplied || a == 0 || a == full) continue;
        for (int c = 0; c < n; c++) {
            if (depth == 2) {
                const uint32_t v = uint32_t(target[2 * c]) << 8 | target[2 * c + 1];
                const uint32_t u = std::min<uint32_t>(full, (v * full + a / 2) / a);
                target[2 * c] = static_cast<uint8_t>(u >> 8);
                target[2 * c + 1] = static_cast<uint8_t>(u);
            } else {
                target[c] = static_cast<uint8_t>(std::min<uint32_t>(full, (target[c] * full + a / 2) / a));
            }
        }
    }
    ImageRows& alpha = *req.inDataAlpha;
    Layout alphaLayout;
    alphaLayout.width = image.width;
    alphaLayout.height = image.height;
    alphaLayout.rowBytes = image.width;
    alpha.layout = alphaLayout;
    alpha.width = image.width;
    alpha.declaredHeight = image.height;
    alpha.channels = 1;
    alpha.samples.SetWhole(std::move(opacityPlane), image.width, image.height);
    alpha.converter.InitAsIs();
    return Status::Ok;
}

// The pixels of a sampled image (no stencil; Flate-family, CCITT, JBIG2, DCT or JPX data):
// colour space, samples, /Decode (not for JPX), made a row at a time. `fullWidth` x `fullHeight`
// (when given): the image's size before a reduced-resolution JPX decode.
Status DecodeSampled(const Source& src, const SampleRequest& req, ImageRows& out, uint32_t* fullWidth = nullptr,
                     uint32_t* fullHeight = nullptr) {
    const Filters::Kind stop = StopFilter(src);
    const bool jpx = stop == Filters::Kind::Jpx;
    if (stop != Filters::Kind::Flate && stop != Filters::Kind::Dct && !jpx && !Bitonal(stop)) return Status::Unsupported;
    Space& space = out.space;
    space = Space();
    const SpaceContext ctx{src.doc, src.resources, src.inlineImage};
    const PdfValue& csValue = src.Entry("ColorSpace", "CS");
    Status status = Status::Ok;
    if (csValue.IsNull()) {
        // JPEG and JPEG 2000 data say what they hold, and fax and JBIG2 data are gray; other
        // samples need the space.
        if (!req.softMask && stop != Filters::Kind::Dct && !jpx && !Bitonal(stop)) return Status::Corrupt;
        SetDevice(space, Family::Gray);
    } else if ((status = ParseSpace(ctx, csValue, 0, space)) != Status::Ok) {
        return status;
    }
    Layout& layout = out.layout;
    if (stop == Filters::Kind::Dct) {
        std::string data;
        if ((status = DecodeJpegSamples(src, req.maxPixels, space, layout, data)) != Status::Ok) return status;
        out.samples.SetWhole(std::move(data), layout.rowBytes, layout.height);
    } else if (jpx) {
        std::string data;
        uint32_t width = 0, height = 0;
        status = DecodeJpxSamples(src, req, !csValue.IsNull() || req.softMask, space, layout, data, width, height);
        if (status != Status::Ok) return status;
        if (fullWidth) *fullWidth = width;
        if (fullHeight) *fullHeight = height;
        out.samples.SetWhole(std::move(data), layout.rowBytes, layout.height);
    } else {
        // CCITT and JBIG2 decode to one bit a pixel of one component, whatever /BitsPerComponent says.
        if (Bitonal(stop) && space.components != 1) return Status::Corrupt;
        if ((status = ReadLayout(src, Bitonal(stop), space.components, req.maxPixels, layout)) != Status::Ok) return status;
        if ((status = OpenSamples(src, layout, out.samples)) != Status::Ok) return status;
    }
    out.keyed = req.colourKey && ColourKey(src, layout, out.key);
    out.width = layout.width;
    out.declaredHeight = layout.height;
    out.channels = space.channels;
    DecodeMap map;
    MakeDecodeMap(src, space, layout.bpc, map, !jpx);
    if (req.allowPacked && stop != Filters::Kind::Dct && !jpx && space.family == Family::Gray &&
        space.components == 1 && space.channels == 1 && (layout.bpc == 1 || layout.bpc == 2 || layout.bpc == 4) &&
        !out.keyed) {
        // The samples are a PNG's gray when every code's colour, as Converter makes it, is
        // code * 255 / (2^bits - 1), or that of the complemented code: then they stay packed.
        const unsigned top = (1u << layout.bpc) - 1, scale = 255u / top;
        bool same = true, complement = true;
        for (unsigned code = 0; code <= top; code++) {
            const double v = map.Value(0, code);
            uint8_t colour = 0;
            Convert(space, &v, &colour);
            same = same && colour == code * scale;
            complement = complement && colour == (top - code) * scale;
        }
        if (same || complement) {
            out.packedBits = layout.bpc;
            out.packedInvert = !same;
            return Status::Ok;
        }
    }
    out.converter.Init(layout, space, map);
    return Status::Ok;
}

// The alpha of an image's rows from its soft or stencil mask (gray, at the mask's own size),
// resampled to the image's width and declared height by nearest neighbour (`invert` for a
// stencil, whose 0, painted, is opaque; rows the mask's data lacks are opaque). Rows are asked
// for top to bottom and the mask's rows read as they are needed, so that no whole alpha plane,
// nor an image with it interleaved, is held.
class AlphaRows {
public:
    AlphaRows(const ImageRows& image, ImageRows& mask, bool invert)
        : width_(image.width), declaredHeight_(image.declaredHeight), mask_(mask), invert_(invert) {
        columns_.resize(width_);
        for (uint32_t x = 0; x < width_; x++) {
            columns_[x] = static_cast<uint32_t>(uint64_t(x) * mask_.width / width_);
        }
    }

    // Row y's alpha, the image's width in bytes, into `out`; y no lower than the row before.
    void Row(uint32_t y, uint8_t* out) {
        const uint32_t my = static_cast<uint32_t>(uint64_t(y) * mask_.declaredHeight / declaredHeight_);
        while (fetched_ <= my && !ended_) {
            current_ = mask_.Next();
            if (current_) fetched_++;
            else ended_ = true;
        }
        if (fetched_ <= my) {
            std::fill(out, out + width_, uint8_t(255));
            return;
        }
        const size_t ch = static_cast<size_t>(mask_.channels);
        for (uint32_t x = 0; x < width_; x++) {
            const uint8_t v = current_[size_t(columns_[x]) * ch];
            out[x] = invert_ ? static_cast<uint8_t>(255 - v) : v;
        }
    }

    // From the mask's first row again.
    void Rewind() {
        mask_.Rewind();
        current_ = nullptr;
        fetched_ = 0;
        ended_ = false;
    }

private:
    uint32_t width_;
    uint32_t declaredHeight_;
    ImageRows& mask_;
    bool invert_;
    std::vector<uint32_t> columns_;
    const uint8_t* current_ = nullptr;  // the mask's row fetched_ - 1
    uint32_t fetched_ = 0;
    bool ended_ = false;
};

enum class MaskKind : uint8_t {
    None,
    Soft,       // /SMask: gray, the alpha itself
    Stencil,    // /Mask stream: a stencil as DecodeStencil gives it, 0 where the image shows
    ColourKey,  // /Mask array: applied while the image's samples are decoded
};

// The mask of `src` (an /SMask stream before a /Mask, as ISO 32000-1 11.6.5.3 orders them),
// decoded at its own size into `mask`; `decoded` false when its data cannot be decoded.
MaskKind ReadMask(const Source& src, uint64_t maxPixels, ImageRows& mask, bool& decoded) {
    decoded = false;
    if (src.inlineImage) return MaskKind::None;  // inline images have neither (ISO 32000-1 Table 93)
    const PdfValue& smask = src.doc.Get(src.dict, "SMask");
    if (smask.IsStream()) {
        const Source maskSrc{src.doc, smask, src.resources, false, {}, {}, src.parallel};
        SampleRequest req;
        req.maxPixels = maxPixels;
        req.softMask = true;
        decoded = DecodeSampled(maskSrc, req, mask) == Status::Ok;
        if (decoded && mask.channels == 3) {  // not the DeviceGray it must be: its luminance
            mask.luminance = true;
            mask.channels = 1;
        }
        return MaskKind::Soft;
    }
    const PdfValue& stencil = src.doc.Get(src.dict, "Mask");
    if (stencil.IsStream()) {
        const Source maskSrc{src.doc, stencil, src.resources, false, {}, {}, src.parallel};
        decoded = DecodeStencil(maskSrc, maxPixels, mask) == Status::Ok;
        return MaskKind::Stencil;
    }
    if (stencil.IsArray()) {
        decoded = true;
        return MaskKind::ColourKey;
    }
    return MaskKind::None;
}

// Whether a decoded soft or stencil mask hides any part of its image; the mask is read from its
// first row again after.
bool Transparent(ImageRows& mask, MaskKind kind) {
    const uint8_t opaque = kind == MaskKind::Stencil ? 0 : 255;
    bool transparent = false;
    while (!transparent) {
        const uint8_t* row = mask.Next();
        if (!row) break;
        for (uint32_t x = 0; x < mask.width && !transparent; x++) transparent = row[x] != opaque;
    }
    mask.Rewind();
    return transparent;
}

// Where a PNG goes: Encoded::bytes, or the caller's Sink, opened at the first byte.
class PngDestination final : public Png::Output {
public:
    PngDestination(Encoded& out, Sink* sink) : out_(out), sink_(sink) { out_.bytes.clear(); }

    bool Append(const uint8_t* data, size_t size) override {
        if (!sink_) {
            out_.bytes.append(reinterpret_cast<const char*>(data), size);
            return true;
        }
        if (!opened_) {
            opened_ = true;
            if (!sink_->Open("png")) return Fail();
        }
        return sink_->Append(data, size) || Fail();
    }

    bool Patch(uint64_t offset, const uint8_t* data, size_t size) override {
        if (sink_) return sink_->Patch(offset, data, size) || Fail();
        if (offset > out_.bytes.size() || size > out_.bytes.size() - offset) return false;
        std::memcpy(&out_.bytes[static_cast<size_t>(offset)], data, size);
        return true;
    }

    // The PNG begun again: what was written is dropped (the Sink opened anew at the next byte).
    void Restart() {
        out_.bytes.clear();
        opened_ = false;
    }

    // Whether the Sink failed.
    bool Failed() const { return failed_; }

    // The writer's result as a Status: a failed Sink is WriteFailed, anything else TooLarge.
    Status Result(bool ok) {
        if (ok) {
            out_.streamed = sink_ != nullptr;
            return Status::Ok;
        }
        out_.bytes.clear();
        return failed_ ? Status::WriteFailed : Status::TooLarge;
    }

private:
    bool Fail() {
        failed_ = true;
        return false;
    }

    Encoded& out_;
    Sink* sink_;
    bool opened_ = false;
    bool failed_ = false;
};

// Whether a JPEG written as stored shows what the PDF shows: 1 or 3 components in a gray or RGB
// space of as many (or none), the default /Decode. A CMYK JPEG never does: stand-alone viewers
// read an APP14 marker as Adobe's inverted CMYK, PDF reads the samples as ink (jpeg.h). A JPEG
// this decoder cannot read is stored, its only form.
bool JpegAsStored(const Source& src, std::string_view jpeg) {
    Jpeg::Info info;
    if (Jpeg::ReadInfo(reinterpret_cast<const uint8_t*>(jpeg.data()), jpeg.size(), info) != Jpeg::Result::Ok) return true;
    if (info.components == 4) return false;
    const PdfValue& csValue = src.Entry("ColorSpace", "CS");
    if (!csValue.IsNull()) {
        Space space;
        const SpaceContext ctx{src.doc, src.resources, src.inlineImage};
        if (ParseSpace(ctx, csValue, 0, space) != Status::Ok) return true;
        if (space.family != Family::Gray && space.family != Family::Rgb) return false;
    }
    double decode[2 * kMaxComponents];
    const size_t pairs = NumberPairs(src.doc, src.Entry("Decode", "D"), decode) / 2;
    for (size_t i = 0; i < pairs; i++) {
        if (decode[2 * i] != 0 || decode[2 * i + 1] != 1) return false;
    }
    return true;
}

// An image of `src` decoded a row at a time whatever its filter (of those decoded here), with its
// mask: what Encode writes as a PNG, and what Gray composes on white.
struct DecodedImage {
    ImageRows image;
    MaskKind kind = MaskKind::None;
    ImageRows mask;
    bool maskDecoded = false;
    uint32_t fullWidth = 0;    // the size before a reduced-resolution JPX decode (0: as decoded)
    uint32_t fullHeight = 0;
};

// `longerSide`: a JPX image may be decoded at a reduced resolution down to about this (0: full).
// A JPX opacity channel (/SMaskInData) is the soft mask when there is no /SMask stream, which
// takes precedence (ISO 32000-1 Table 89). `allowPacked`: a stencil, or gray of 1, 2 or 4 bits
// without a mask, may stay packed (ImageRows::packedBits) for a PNG written from it.
Status DecodeImage(const Source& src, uint64_t maxPixels, DecodedImage& d, uint32_t longerSide = 0,
                   bool allowPacked = false) {
    if (src.Entry("ImageMask", "IM").Bool(false)) return DecodeStencil(src, maxPixels, d.image, allowPacked);
    SampleRequest req;
    req.maxPixels = maxPixels;
    req.colourKey = true;
    req.longerSide = longerSide;
    if (allowPacked) {
        const PdfValue& mask = src.doc.Get(src.dict, "Mask");
        req.allowPacked = src.inlineImage ||
                          !(src.doc.Get(src.dict, "SMask").IsStream() || mask.IsStream() || mask.IsArray());
    }
    if (!src.inlineImage && !src.doc.Get(src.dict, "SMask").IsStream()) req.inDataAlpha = &d.mask;
    const Status status = DecodeSampled(src, req, d.image, &d.fullWidth, &d.fullHeight);
    if (status != Status::Ok) return status;
    if (d.mask.width > 0) {  // the JPX opacity channel
        d.kind = MaskKind::Soft;
        d.maskDecoded = true;
        return Status::Ok;
    }
    d.kind = ReadMask(src, maxPixels, d.mask, d.maskDecoded);
    return Status::Ok;
}

// Whether the alpha of the image's first `rows` rows is 255 everywhere: from `alpha` (a soft or
// stencil mask), else from the image's colour key. What it read is read again from the start after.
bool Opaque(ImageRows& image, AlphaRows* alpha, uint32_t rows) {
    std::vector<uint8_t> row(alpha ? image.width : 0);
    bool opaque = true;
    for (uint32_t y = 0; y < rows && opaque; y++) {
        const uint8_t* a = row.data();
        if (alpha) {
            alpha->Row(y, row.data());
        } else {
            if (!image.Next()) break;
            a = image.KeyAlpha();
        }
        for (uint32_t x = 0; x < image.width; x++) {
            if (a[x] != 255) {
                opaque = false;
                break;
            }
        }
    }
    if (alpha) alpha->Rewind();
    else image.Rewind();
    return opaque;
}

// The fewest bits that hold the gray of the image's first `rows` rows exactly (Png::GrayDepths),
// read from the image's first row; read again from the start after.
int GrayDepth(ImageRows& image, uint32_t rows) {
    uint8_t allowed = 7;
    for (uint32_t y = 0; y < rows && allowed != 0; y++) {
        const uint8_t* row = image.Next();
        if (!row) break;
        allowed = Png::GrayDepths(row, image.width, allowed);
    }
    image.Rewind();
    return Png::GrayDepthOf(allowed);
}

// The rows of an image for the PNG writer: its pixels or packed gray as they are, or its pixels
// with alpha interleaved. A row the data lacks ends the image early (`ended`).
struct PngRows {
    ImageRows* image;
    AlphaRows* alpha;  // the alpha of a soft or stencil mask, else of the colour key
    std::vector<uint8_t> rows[2];
    std::vector<uint8_t> alphaRow;
    bool ended = false;
};

const uint8_t* PlainRow(void* context, uint32_t) {
    PngRows& r = *static_cast<PngRows*>(context);
    const uint8_t* row = r.image->Next();
    if (!row) r.ended = true;
    return row;
}

const uint8_t* InterleavedRow(void* context, uint32_t y) {
    PngRows& r = *static_cast<PngRows*>(context);
    const uint8_t* pixels = r.image->Next();
    if (!pixels) {
        r.ended = true;
        return nullptr;
    }
    const uint8_t* alpha = r.alphaRow.data();
    if (r.alpha) r.alpha->Row(y, r.alphaRow.data());
    else alpha = r.image->KeyAlpha();
    const size_t width = r.image->width;
    const size_t ch = static_cast<size_t>(r.image->channels);
    uint8_t* target = r.rows[y & 1].data();
    for (size_t x = 0; x < width; x++) {
        std::memcpy(target + x * (ch + 1), pixels + x * ch, ch);
        target[x * (ch + 1) + ch] = alpha[x];
    }
    return target;
}

// The PNG of `d` from its first row: packed gray as stored, else its pixels with the alpha of its
// soft or stencil mask or of its colour key (left out when opaque everywhere), gray at the fewest
// exact bits. How many rows the data holds is known only once it has been read to its end: the
// PNG is begun with the image's every row unless a pass over the data has found fewer, and made
// again (once) with the rows there are when the data ends before them.
Status WriteImage(DecodedImage& d, Encoded& out, Sink* sink) {
    ImageRows& image = d.image;
    const bool masked = (d.kind == MaskKind::Soft || d.kind == MaskKind::Stencil) && d.maskDecoded;
    std::unique_ptr<AlphaRows> alpha;
    if (masked) alpha = std::make_unique<AlphaRows>(image, d.mask, d.kind == MaskKind::Stencil);
    PngDestination destination(out, sink);
    for (int attempt = 0; attempt < 3; attempt++) {
        const uint32_t rows = image.RowsKnown() ? image.Rows() : image.declaredHeight;
        bool opaque = true;
        int depth = 8;
        if (image.packedBits == 0) {
            if (alpha || image.keyed) opaque = Opaque(image, alpha.get(), rows);
            if (opaque && image.channels == 1) depth = GrayDepth(image, rows);
            // A pass to the end of the data found fewer rows: decide again on those.
            if (image.RowsKnown() && image.Rows() != rows) continue;
        }
        out.extension = "png";
        out.width = image.width;
        out.height = rows;
        PngRows source{&image, alpha.get(), {}, {}};
        bool ok;
        if (image.packedBits != 0) {
            ok = Png::EncodePackedGray(image.width, rows, image.packedBits, image.packedInvert, PlainRow, &source,
                                       destination);
        } else if (!opaque) {
            const size_t rowBytes = size_t(image.width) * static_cast<size_t>(image.channels + 1);
            source.rows[0].resize(rowBytes);
            source.rows[1].resize(rowBytes);
            source.alphaRow.resize(image.width);
            ok = Png::EncodeRows(image.width, rows, image.channels + 1, InterleavedRow, &source, destination);
        } else if (image.channels == 1) {
            ok = Png::EncodeGray(image.width, rows, depth, PlainRow, &source, destination);
        } else {
            ok = Png::EncodeRows(image.width, rows, image.channels, PlainRow, &source, destination);
        }
        if (!ok && source.ended && !destination.Failed()) {
            // The data ended before `rows` rows, which are known now: again with them.
            destination.Restart();
            image.Rewind();
            if (alpha) alpha->Rewind();
            continue;
        }
        const Status status = destination.Result(ok);
        if (status == Status::Ok) out.maskDropped = d.kind != MaskKind::None && !d.maskDecoded;
        return status;
    }
    return destination.Result(false);
}

Status Encode(const Source& src, Encoded& out, uint64_t maxPixels, Sink* sink) {
    out = Encoded();
    const Filters::Kind stop = StopFilter(src);
    const bool stencil = src.Entry("ImageMask", "IM").Bool(false);
    if (stop == Filters::Kind::Dct || stop == Filters::Kind::Jpx) {
        const int64_t w = src.Entry("Width", "W").Int(0);
        const int64_t h = src.Entry("Height", "H").Int(0);
        const uint32_t width = w > 0 && w <= kMaxDimension ? static_cast<uint32_t>(w) : 0;
        const uint32_t height = h > 0 && h <= kMaxDimension ? static_cast<uint32_t>(h) : 0;
        std::string data;
        const StreamStatus read = ReadData(src, data, kPassThroughLimit);
        if (!Usable(read, data)) return read == StreamStatus::TooLarge ? Status::TooLarge : Status::Corrupt;
        // The data must be whole: a JPEG or JPEG 2000 file cut at the limit is no file.
        if (read == StreamStatus::TooLarge) return Status::TooLarge;
        if (stop == Filters::Kind::Jpx && !stencil) {
            // JPEG 2000 is decoded and written as a PNG (with its masks); stored as .jp2 only
            // when this decoder cannot read it.
            Source jpxSrc = src;
            jpxSrc.encoded = data;
            DecodedImage d;
            if (DecodeImage(jpxSrc, maxPixels, d) == Status::Ok) return WriteImage(d, out, sink);
        }
        // The mask, decoded at its own size, and later the JPEG's pixels when they are written.
        DecodedImage m;
        m.kind = stencil ? MaskKind::None : ReadMask(src, maxPixels, m.mask, m.maskDecoded);
        // A mask that hides something, decoded; only pixels can carry it.
        const bool maskShows = m.kind == MaskKind::ColourKey ||
                               (m.kind != MaskKind::None && m.maskDecoded && Transparent(m.mask, m.kind));
        if (stop == Filters::Kind::Dct && !stencil) {
            Source jpegSrc = src;
            jpegSrc.encoded = JpegStart(data);
            if (!jpegSrc.encoded.empty() && (maskShows || !JpegAsStored(src, jpegSrc.encoded))) {
                SampleRequest req;
                req.maxPixels = maxPixels;
                req.colourKey = true;
                const Status status = DecodeSampled(jpegSrc, req, m.image);
                if (status == Status::Ok) return WriteImage(m, out, sink);
                // A JPEG this decoder cannot read (arithmetic, 12-bit, damaged, too large to
                // decode): as stored, which the pixel limit below still bounds.
            }
        }
        const Status status = stop == Filters::Kind::Dct ? PassJpeg(data, width, height, out)
                                                         : PassJpx(data, width, height, out);
        if (status == Status::Ok && uint64_t(out.width) * out.height > maxPixels) {
            out = Encoded();
            return Status::TooLarge;
        }
        if (status != Status::Ok) {
            out = Encoded();
            return status;
        }
        out.maskDropped = maskShows || (m.kind != MaskKind::None && !m.maskDecoded);
        return status;
    }
    if (stop != Filters::Kind::Flate && !Bitonal(stop)) return Status::Unsupported;

    DecodedImage d;
    const Status status = DecodeImage(src, maxPixels, d, 0, true);
    if (status != Status::Ok) return status;
    return WriteImage(d, out, sink);
}

// `d` as gray composited on white (image.h), reduced by area averaging to a longer side of at
// most `maxSide` (0: none), a source row at a time. Rows the data lacks (up to the image's
// declared height) are white, so that the pixels always cover the whole of image space.
void ComposeGray(DecodedImage& d, uint32_t maxSide, GrayImage& out) {
    ImageRows& image = d.image;
    const uint32_t width = image.width, height = image.declaredHeight;
    std::unique_ptr<AlphaRows> alpha;
    if ((d.kind == MaskKind::Soft || d.kind == MaskKind::Stencil) && d.maskDecoded) {
        alpha = std::make_unique<AlphaRows>(image, d.mask, d.kind == MaskKind::Stencil);
    }
    const bool keyed = !alpha && image.keyed;
    const uint32_t longer = std::max(width, height);
    uint32_t outWidth = width, outHeight = height;
    if (maxSide > 0 && longer > maxSide) {
        outWidth = std::max<uint32_t>(1, static_cast<uint32_t>(uint64_t(width) * maxSide / longer));
        outHeight = std::max<uint32_t>(1, static_cast<uint32_t>(uint64_t(height) * maxSide / longer));
    }
    out.width = outWidth;
    out.height = outHeight;
    // The image's own size: before a reduced-resolution decode, its declared height.
    out.sourceWidth = d.fullWidth ? d.fullWidth : width;
    out.sourceHeight = d.fullHeight ? d.fullHeight : height;
    out.pixels.resize(size_t(outWidth) * outHeight);
    uint8_t* target = reinterpret_cast<uint8_t*>(&out.pixels[0]);
    const bool reduce = outWidth != width || outHeight != height;
    // Reduction: each source pixel adds to the output pixel (x * outWidth / width, y * outHeight /
    // height); an output row is written when the source rows that map to it are done.
    std::vector<uint32_t> columnOf, columnCount, sums;
    if (reduce) {
        columnOf.resize(width);
        columnCount.assign(outWidth, 0);
        sums.assign(outWidth, 0);
        for (uint32_t x = 0; x < width; x++) {
            columnOf[x] = static_cast<uint32_t>(uint64_t(x) * outWidth / width);
            columnCount[columnOf[x]]++;
        }
    }
    uint32_t pendingRow = 0, pendingCount = 0;
    const auto flush = [&]() {
        uint8_t* row = target + size_t(pendingRow) * outWidth;
        for (uint32_t x = 0; x < outWidth; x++) {
            const uint32_t n = columnCount[x] * pendingCount;
            row[x] = static_cast<uint8_t>(n ? (sums[x] + n / 2) / n : 255);
            sums[x] = 0;
        }
        pendingCount = 0;
    };
    std::vector<uint8_t> gray(width), alphaRow(alpha ? width : 0);
    const size_t ch = static_cast<size_t>(image.channels);
    bool present = true;  // the data holds row y
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* pixels = present ? image.Next() : nullptr;
        present = pixels != nullptr;
        if (!pixels) {
            std::fill(gray.begin(), gray.end(), uint8_t(255));
        } else if (ch == 1) {
            std::memcpy(gray.data(), pixels, width);
        } else {
            for (uint32_t x = 0; x < width; x++) {
                const uint8_t* p = pixels + 3 * size_t(x);
                gray[x] = static_cast<uint8_t>((p[0] * 77u + p[1] * 150u + p[2] * 29u) >> 8);
            }
        }
        if (pixels && (alpha || keyed)) {
            const uint8_t* a = alphaRow.data();
            if (alpha) alpha->Row(y, alphaRow.data());
            else a = image.KeyAlpha();
            for (uint32_t x = 0; x < width; x++) {
                const uint32_t v = a[x];
                gray[x] = static_cast<uint8_t>((gray[x] * v + 255 * (255 - v) + 127) / 255);
            }
        }
        if (!reduce) {
            std::memcpy(target + size_t(y) * width, gray.data(), width);
            continue;
        }
        const uint32_t outRow = static_cast<uint32_t>(uint64_t(y) * outHeight / height);
        if (pendingCount > 0 && outRow != pendingRow) flush();
        pendingRow = outRow;
        for (uint32_t x = 0; x < width; x++) sums[columnOf[x]] += gray[x];
        pendingCount++;
    }
    if (reduce && pendingCount > 0) flush();
}

Status Gray(const Source& src, uint32_t maxSide, uint64_t maxPixels, GrayImage& out) {
    out = GrayImage();
    DecodedImage d;
    // A JPX image need not be decoded at more than the size asked for.
    const Status status = DecodeImage(src, maxPixels, d, maxSide);
    if (status != Status::Ok) return status;
    ComposeGray(d, maxSide, out);
    return Status::Ok;
}

} // namespace

Status EncodeXObject(const Document& document, const PdfValue& image, const PdfValue& resources, Encoded& out,
                     uint64_t maxPixels, Sink* sink, Parallel parallel) {
    out = Encoded();
    const PdfValue& stream = document.Resolve(image);
    if (!stream.IsStream()) return Status::Corrupt;
    const Source src{document, stream, document.Resolve(resources), false, {}, {}, parallel};
    return Encode(src, out, maxPixels, sink);
}

Status EncodeInline(const Document& document, const PdfValue& dictionary, std::string_view data,
                    const PdfValue& resources, Encoded& out, uint64_t maxPixels, Sink* sink, Parallel parallel) {
    out = Encoded();
    if (!dictionary.IsDict()) return Status::Corrupt;
    const Source src{document, dictionary, document.Resolve(resources), true, data, {}, parallel};
    return Encode(src, out, maxPixels, sink);
}

Status GrayXObject(const Document& document, const PdfValue& image, const PdfValue& resources, GrayImage& out,
                   uint32_t maxSide, uint64_t maxPixels) {
    out = GrayImage();
    const PdfValue& stream = document.Resolve(image);
    if (!stream.IsStream()) return Status::Corrupt;
    const Source src{document, stream, document.Resolve(resources), false, {}, {}, {}};
    return Gray(src, maxSide, maxPixels, out);
}

Status GrayInline(const Document& document, const PdfValue& dictionary, std::string_view data,
                  const PdfValue& resources, GrayImage& out, uint32_t maxSide, uint64_t maxPixels) {
    out = GrayImage();
    if (!dictionary.IsDict()) return Status::Corrupt;
    const Source src{document, dictionary, document.Resolve(resources), true, data, {}, {}};
    return Gray(src, maxSide, maxPixels, out);
}

} // namespace RayoPdf::Image
