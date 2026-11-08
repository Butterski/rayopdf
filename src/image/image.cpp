// SPDX-License-Identifier: Apache-2.0
// Image XObjects and inline images to file bytes (image.h). Choices the header leaves open:
// - /ImageMask stencils come out as opaque gray: black where the stencil paints, white elsewhere
//   (the fill colour is the content stream's, unknown here; black on white reads in any viewer).
// - A data stream shorter than the image keeps the rows present: the height shrinks to them (a
//   last partial row is completed with zero bytes), so memory follows the data that exists; no
//   complete or partial row at all is Corrupt. CCITT and JBIG2 data are the exception: their
//   decoders' output is read as the filter's byte stream in rows of the image's own (as poppler
//   does, whatever /Columns or the JBIG2 page width says), what a damaged stream decoded before
//   the damage is kept, and missing rows are white, as poppler shows them.
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

// An inline image's data run through its general-purpose filters (up to the first image filter),
// at most `limit` bytes.
StreamStatus InlineData(const Source& src, std::string& out, size_t limit) {
    const PdfValue& filter = src.Entry("Filter", "F");
    const PdfValue& parms = src.Entry("DecodeParms", "DP");
    const PdfValue* items = &filter;
    size_t n = filter.IsName() ? 1 : 0;
    if (filter.IsArray()) {
        items = filter.Array().begin();
        n = filter.Array().size();
    }
    if (n > kMaxFilters) return StreamStatus::Unsupported;
    std::string scratch;
    std::string_view data = src.inlineData;
    bool decoded = false;
    StreamStatus status = StreamStatus::Ok;
    for (size_t i = 0; i < n; i++) {
        const Filters::Kind kind = Filters::KindOf(src.doc.Resolve(items[i]).Name());
        if (kind == Filters::Kind::Crypt) continue;
        if (kind != Filters::Kind::Flate && kind != Filters::Kind::Lzw && kind != Filters::Kind::Ascii85 &&
            kind != Filters::Kind::AsciiHex && kind != Filters::Kind::RunLength) {
            status = StreamStatus::Unsupported;
            break;
        }
        const PdfValue& p = parms.IsArray() ? (i < parms.Array().size() ? src.doc.Resolve(parms.Array()[i]) : parms)
                                            : parms;
        Filters::Params params;
        if (p.IsDict()) {
            const auto number = [&](std::string_view key, int fallback) {
                const int64_t x = src.doc.Get(p, key).Int(fallback);
                return x < -1000000000 || x > 1000000000 ? fallback : static_cast<int>(x);
            };
            params.predictor = number("Predictor", 1);
            params.colors = number("Colors", 1);
            params.bitsPerComponent = number("BitsPerComponent", 8);
            params.columns = number("Columns", 1);
            params.earlyChange = number("EarlyChange", 1);
        }
        const StreamStatus step = Filters::Decode(kind, params, data, scratch, limit);
        if (step == StreamStatus::Corrupt) {
            out.clear();
            return StreamStatus::Corrupt;
        }
        out.swap(scratch);
        data = out;
        decoded = true;
        if (step != StreamStatus::Ok) status = step;
        if (step == StreamStatus::TooLarge) return status;
    }
    if (!decoded) out.assign(data.data(), std::min(data.size(), limit));
    return status;
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

// Decoded pixels, rows top to bottom. A string, so that samples already in pixel form are taken
// over without a copy.
struct Pixels {
    std::string data;
    uint32_t width = 0;
    uint32_t height = 0;  // rows present (at most the declared height)
    uint32_t declaredHeight = 0;
    int channels = 1;
    // Gray kept as stored, `packedBits` (1, 2 or 4) bits a sample in rows `packedRowBytes` apart:
    // only when the caller allows it (SampleRequest::allowPacked) and the samples are a PNG's
    // gray of that depth, or (`packedInvert`) its complement. 0: 8-bit pixels.
    int packedBits = 0;
    size_t packedRowBytes = 0;
    bool packedInvert = false;

    uint8_t* Bytes() { return reinterpret_cast<uint8_t*>(&data[0]); }
    const uint8_t* Bytes() const { return reinterpret_cast<const uint8_t*>(data.data()); }
};

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

// The samples of `layout` as whole rows: those the data holds, the last partial one completed
// with zeros; Corrupt for none. `rows` is how many. CCITT and JBIG2 data go through their
// decoders (DecodeBitonal).
Status ReadSamples(const Source& src, const Layout& layout, std::string& data, uint32_t& rows) {
    const Filters::Kind stop = StopFilter(src);
    if (Bitonal(stop)) return DecodeBitonal(src, stop, layout, data, rows);
    const size_t expected = layout.rowBytes * layout.height;
    // Slack past the samples, so that data a little longer than needed is no limit hit.
    const StreamStatus read = ReadData(src, data, expected + 4096);
    if (read == StreamStatus::Unsupported) return Status::Unsupported;
    if (!Usable(read, data)) return read == StreamStatus::TooLarge ? Status::TooLarge : Status::Corrupt;
    const uint64_t present = (uint64_t(data.size()) + layout.rowBytes - 1) / layout.rowBytes;
    rows = static_cast<uint32_t>(std::min<uint64_t>(present, layout.height));
    data.resize(size_t(rows) * layout.rowBytes, '\0');
    return Status::Ok;
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

// `data` (whole rows of `layout`, `rows` of them) to pixels of `space`; `data` may be taken over.
void ConvertPixels(const Layout& layout, uint32_t rows, std::string& data, const Space& space, const DecodeMap& map,
                   Pixels& out) {
    const size_t width = layout.width;
    const size_t n = static_cast<size_t>(space.components);
    const size_t ch = static_cast<size_t>(space.channels);
    out.width = layout.width;
    out.height = rows;
    out.declaredHeight = layout.height;
    out.channels = space.channels;
    if (map.identity && layout.bpc == 8) {
        out.data.swap(data);
        return;
    }
    const uint8_t* in = reinterpret_cast<const uint8_t*>(data.data());
    out.data.resize(width * ch * rows);
    std::vector<uint8_t> codes(width * n);
    const size_t codeCount = size_t(1) << map.codeBits;
    if (n == 1) {
        // Every code's colour once.
        std::vector<uint8_t> lut(codeCount * ch);
        for (size_t code = 0; code < codeCount; code++) {
            const double v = map.Value(0, static_cast<unsigned>(code));
            Convert(space, &v, lut.data() + code * ch);
        }
        for (uint32_t y = 0; y < rows; y++) {
            UnpackRow(in + size_t(y) * layout.rowBytes, width, layout.bpc, codes.data());
            uint8_t* target = out.Bytes() + size_t(y) * width * ch;
            if (ch == 1) {
                for (size_t x = 0; x < width; x++) target[x] = lut[codes[x]];
            } else {
                for (size_t x = 0; x < width; x++) std::memcpy(target + 3 * x, lut.data() + 3 * codes[x], 3);
            }
        }
        return;
    }
    if (space.family == Family::Rgb || space.family == Family::Cmyk) {
        // Per component, each code's 8-bit value.
        std::vector<uint8_t> lut(n * codeCount);
        for (size_t c = 0; c < n; c++) {
            for (size_t code = 0; code < codeCount; code++) {
                lut[c * codeCount + code] = To8(map.Value(static_cast<int>(c), static_cast<unsigned>(code)));
            }
        }
        for (uint32_t y = 0; y < rows; y++) {
            UnpackRow(in + size_t(y) * layout.rowBytes, width * n, layout.bpc, codes.data());
            uint8_t* target = out.Bytes() + size_t(y) * width * 3;
            const uint8_t* code = codes.data();
            if (n == 3) {
                for (size_t x = 0; x < width; x++, code += 3, target += 3) {
                    target[0] = lut[code[0]];
                    target[1] = lut[codeCount + code[1]];
                    target[2] = lut[2 * codeCount + code[2]];
                }
            } else {
                for (size_t x = 0; x < width; x++, code += 4, target += 3) {
                    const unsigned white = 255u - lut[3 * codeCount + code[3]];
                    target[0] = static_cast<uint8_t>(((255u - lut[code[0]]) * white + 127) / 255);
                    target[1] = static_cast<uint8_t>(((255u - lut[codeCount + code[1]]) * white + 127) / 255);
                    target[2] = static_cast<uint8_t>(((255u - lut[2 * codeCount + code[2]]) * white + 127) / 255);
                }
            }
        }
        return;
    }
    // Lab, DeviceN: per pixel, a run of equal samples converted once.
    std::vector<double> values(n * codeCount);
    for (size_t c = 0; c < n; c++) {
        for (size_t code = 0; code < codeCount; code++) {
            values[c * codeCount + code] = map.Value(static_cast<int>(c), static_cast<unsigned>(code));
        }
    }
    std::vector<uint8_t> last(n, 0);
    uint8_t lastPixel[3] = {0, 0, 0};
    bool haveLast = false;
    for (uint32_t y = 0; y < rows; y++) {
        UnpackRow(in + size_t(y) * layout.rowBytes, width * n, layout.bpc, codes.data());
        uint8_t* target = out.Bytes() + size_t(y) * width * ch;
        for (size_t x = 0; x < width; x++) {
            const uint8_t* code = codes.data() + x * n;
            if (!haveLast || std::memcmp(code, last.data(), n) != 0) {
                double v[kMaxComponents];
                for (size_t c = 0; c < n; c++) v[c] = values[c * codeCount + code[c]];
                Convert(space, v, lastPixel);
                std::memcpy(last.data(), code, n);
                haveLast = true;
            }
            std::memcpy(target + x * ch, lastPixel, ch);
        }
    }
}

// A stencil (ImageMask) as gray: 0 where it paints, 255 elsewhere. With `packed`, kept as its
// bits (gray bit * 255, complemented where 1 paints).
Status DecodeStencil(const Source& src, uint64_t maxPixels, Pixels& out, bool packed = false) {
    const Filters::Kind stop = StopFilter(src);
    if (stop != Filters::Kind::Flate && !Bitonal(stop)) return Status::Unsupported;
    Layout layout;
    Status status = ReadLayout(src, true, 1, maxPixels, layout);
    if (status != Status::Ok) return status;
    std::string data;
    uint32_t rows = 0;
    if ((status = ReadSamples(src, layout, data, rows)) != Status::Ok) return status;
    // /Decode [0 1] (the default): 0 paints. [1 0]: 1 paints.
    double decode[2];
    const bool inverted = Numbers(src.doc, src.Entry("Decode", "D"), decode, 2) && decode[0] > decode[1];
    const uint8_t painted = inverted ? 1 : 0;
    out.width = layout.width;
    out.height = rows;
    out.declaredHeight = layout.height;
    out.channels = 1;
    if (packed) {
        out.data.swap(data);
        out.packedBits = 1;
        out.packedRowBytes = layout.rowBytes;
        out.packedInvert = inverted;
        return Status::Ok;
    }
    out.data.resize(size_t(layout.width) * rows);
    const uint8_t* in = reinterpret_cast<const uint8_t*>(data.data());
    for (uint32_t y = 0; y < rows; y++) {
        const uint8_t* row = in + size_t(y) * layout.rowBytes;
        uint8_t* target = out.Bytes() + size_t(y) * layout.width;
        for (uint32_t x = 0; x < layout.width; x++) {
            const uint8_t bit = (row[x >> 3] >> (7 - (x & 7))) & 1;
            target[x] = bit == painted ? 0 : 255;
        }
    }
    return Status::Ok;
}

// Alpha from a /Mask colour key (ISO 32000-1 8.9.6.4): 0 where every sample as stored (before
// /Decode) lies in its component's range, else 255. Left empty without such a /Mask array.
void ColourKeyAlpha(const Source& src, const Layout& layout, uint32_t rows, const std::string& data,
                    std::vector<uint8_t>& alpha) {
    const PdfValue& mask = src.doc.Get(src.dict, "Mask");
    const size_t n = static_cast<size_t>(layout.components);
    double key[2 * kMaxComponents];
    if (!mask.IsArray() || NumberPairs(src.doc, mask, key) != 2 * n) return;
    const size_t width = layout.width;
    alpha.assign(width * rows, 255);
    const uint8_t* in = reinterpret_cast<const uint8_t*>(data.data());
    const unsigned sampleMask = layout.bpc == 16 ? 0xFFFFu : (1u << layout.bpc) - 1;
    for (uint32_t y = 0; y < rows; y++) {
        const uint8_t* row = in + size_t(y) * layout.rowBytes;
        for (size_t x = 0; x < width; x++) {
            bool inside = true;
            for (size_t c = 0; c < n && inside; c++) {
                const size_t i = x * n + c;
                unsigned sample;
                if (layout.bpc == 16) {
                    sample = Be16(row + 2 * i);
                } else if (layout.bpc == 8) {
                    sample = row[i];
                } else {
                    const size_t bit = i * static_cast<size_t>(layout.bpc);
                    sample = (row[bit >> 3] >> (8 - static_cast<size_t>(layout.bpc) - (bit & 7))) & sampleMask;
                }
                inside = sample >= key[2 * c] && sample <= key[2 * c + 1];
            }
            if (inside) alpha[size_t(y) * width + x] = 0;
        }
    }
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
    bool softMask = false;                     // a /SMask: DeviceGray when it has no /ColorSpace
    std::vector<uint8_t>* keyAlpha = nullptr;  // filled from a /Mask colour key, when given
    Pixels* inDataAlpha = nullptr;             // filled from a JPX opacity channel (/SMaskInData)
    uint32_t longerSide = 0;  // JPX: decode at a reduced resolution down to about this (0: full)
    bool allowPacked = false; // gray of 1, 2 or 4 bits that is a PNG's may stay packed (Pixels)
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
    Pixels& alpha = *req.inDataAlpha;
    const size_t pixels = size_t(image.width) * image.height;
    const size_t in = (static_cast<size_t>(n) + 1) * depth, colour = static_cast<size_t>(n) * depth;
    if (image.samples.size() < pixels * in) return Status::Corrupt;
    alpha.width = image.width;
    alpha.height = image.height;
    alpha.declaredHeight = image.height;
    alpha.channels = 1;
    alpha.data.resize(pixels);
    samples.resize(pixels * colour);
    const uint8_t* source = reinterpret_cast<const uint8_t*>(image.samples.data());
    uint8_t* target = reinterpret_cast<uint8_t*>(&samples[0]);
    uint8_t* opacity = alpha.Bytes();
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
    return Status::Ok;
}

// The pixels of a sampled image (no stencil; Flate-family, CCITT, JBIG2, DCT or JPX data):
// colour space, samples, /Decode (not for JPX). `fullWidth` x `fullHeight` (when given): the
// image's size before a reduced-resolution JPX decode.
Status DecodeSampled(const Source& src, const SampleRequest& req, Pixels& out, uint32_t* fullWidth = nullptr,
                     uint32_t* fullHeight = nullptr) {
    const Filters::Kind stop = StopFilter(src);
    const bool jpx = stop == Filters::Kind::Jpx;
    if (stop != Filters::Kind::Flate && stop != Filters::Kind::Dct && !jpx && !Bitonal(stop)) return Status::Unsupported;
    Space space;
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
    Layout layout;
    std::string data;
    uint32_t rows = 0;
    if (stop == Filters::Kind::Dct) {
        if ((status = DecodeJpegSamples(src, req.maxPixels, space, layout, data)) != Status::Ok) return status;
        rows = layout.height;
    } else if (jpx) {
        uint32_t width = 0, height = 0;
        status = DecodeJpxSamples(src, req, !csValue.IsNull() || req.softMask, space, layout, data, width, height);
        if (status != Status::Ok) return status;
        if (fullWidth) *fullWidth = width;
        if (fullHeight) *fullHeight = height;
        rows = layout.height;
    } else {
        // CCITT and JBIG2 decode to one bit a pixel of one component, whatever /BitsPerComponent says.
        if (Bitonal(stop) && space.components != 1) return Status::Corrupt;
        if ((status = ReadLayout(src, Bitonal(stop), space.components, req.maxPixels, layout)) != Status::Ok) return status;
        if ((status = ReadSamples(src, layout, data, rows)) != Status::Ok) return status;
    }
    if (req.keyAlpha) ColourKeyAlpha(src, layout, rows, data, *req.keyAlpha);
    DecodeMap map;
    MakeDecodeMap(src, space, layout.bpc, map, !jpx);
    if (req.allowPacked && stop != Filters::Kind::Dct && !jpx && space.family == Family::Gray &&
        space.components == 1 && space.channels == 1 && (layout.bpc == 1 || layout.bpc == 2 || layout.bpc == 4) &&
        (!req.keyAlpha || req.keyAlpha->empty())) {
        // The samples are a PNG's gray when every code's colour, as ConvertPixels makes it, is
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
            out.width = layout.width;
            out.height = rows;
            out.declaredHeight = layout.height;
            out.channels = 1;
            out.data.swap(data);
            out.packedBits = layout.bpc;
            out.packedRowBytes = layout.rowBytes;
            out.packedInvert = !same;
            return Status::Ok;
        }
    }
    ConvertPixels(layout, rows, data, space, map, out);
    return Status::Ok;
}

// The alpha of an image, a row at a time: a soft or stencil mask (gray) resampled to the image's
// width x rows by nearest neighbour (`invert` for a stencil, whose 0, painted, is opaque; rows
// the mask's data lacks are opaque), or a colour key's alpha (one byte a pixel). Made a row at a
// time, so that no whole alpha plane, nor an image with it interleaved, is held.
class AlphaRows {
public:
    AlphaRows(const Pixels& image, const Pixels* mask, bool invert, const std::vector<uint8_t>* key)
        : image_(image), mask_(mask), invert_(invert), key_(key) {
        if (mask_) {
            columns_.resize(image.width);
            for (uint32_t x = 0; x < image.width; x++) {
                columns_[x] = static_cast<uint32_t>(uint64_t(x) * mask_->width / image.width);
            }
        }
    }

    // Row y's alpha, image.width bytes, into `out`.
    void Row(uint32_t y, uint8_t* out) const {
        const uint32_t width = image_.width;
        if (key_) {
            std::memcpy(out, key_->data() + size_t(y) * width, width);
            return;
        }
        const uint32_t my = static_cast<uint32_t>(uint64_t(y) * mask_->declaredHeight / image_.declaredHeight);
        if (my >= mask_->height) {
            std::fill(out, out + width, uint8_t(255));
            return;
        }
        const size_t ch = static_cast<size_t>(mask_->channels);
        const uint8_t* row = mask_->Bytes() + size_t(my) * mask_->width * ch;
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t v = row[size_t(columns_[x]) * ch];
            out[x] = invert_ ? static_cast<uint8_t>(255 - v) : v;
        }
    }

    // Whether every pixel's alpha is 255.
    bool Opaque() const {
        std::vector<uint8_t> row(image_.width);
        for (uint32_t y = 0; y < image_.height; y++) {
            Row(y, row.data());
            for (uint8_t a : row) {
                if (a != 255) return false;
            }
        }
        return true;
    }

private:
    const Pixels& image_;
    const Pixels* mask_;
    bool invert_;
    const std::vector<uint8_t>* key_;
    std::vector<uint32_t> columns_;
};

// The rows of an image with its alpha interleaved, for Png::EncodeRows: two row buffers in turn.
struct InterleavedRows {
    const Pixels* image;
    const AlphaRows* alpha;
    std::vector<uint8_t> rows[2];
    std::vector<uint8_t> alphaRow;
};

const uint8_t* InterleavedRow(void* context, uint32_t y) {
    InterleavedRows& r = *static_cast<InterleavedRows*>(context);
    const size_t width = r.image->width;
    const size_t ch = static_cast<size_t>(r.image->channels);
    uint8_t* target = r.rows[y & 1].data();
    r.alpha->Row(y, r.alphaRow.data());
    const uint8_t* pixels = r.image->Bytes() + size_t(y) * width * ch;
    for (size_t x = 0; x < width; x++) {
        std::memcpy(target + x * (ch + 1), pixels + x * ch, ch);
        target[x * (ch + 1) + ch] = r.alphaRow[x];
    }
    return target;
}

enum class MaskKind : uint8_t {
    None,
    Soft,       // /SMask: gray, the alpha itself
    Stencil,    // /Mask stream: a stencil as DecodeStencil gives it, 0 where the image shows
    ColourKey,  // /Mask array: applied while the image's samples are decoded
};

// The mask of `src` (an /SMask stream before a /Mask, as ISO 32000-1 11.6.5.3 orders them),
// decoded at its own size into `mask`; `decoded` false when its data cannot be decoded.
MaskKind ReadMask(const Source& src, uint64_t maxPixels, Pixels& mask, bool& decoded) {
    decoded = false;
    if (src.inlineImage) return MaskKind::None;  // inline images have neither (ISO 32000-1 Table 93)
    const PdfValue& smask = src.doc.Get(src.dict, "SMask");
    if (smask.IsStream()) {
        const Source maskSrc{src.doc, smask, src.resources, false, {}, {}};
        SampleRequest req;
        req.maxPixels = maxPixels;
        req.softMask = true;
        decoded = DecodeSampled(maskSrc, req, mask) == Status::Ok;
        if (decoded && mask.channels == 3) {  // not the DeviceGray it must be: its luminance
            uint8_t* bytes = mask.Bytes();
            for (size_t i = 0, n = size_t(mask.width) * mask.height; i < n; i++) {
                const uint8_t* p = bytes + 3 * i;
                bytes[i] = static_cast<uint8_t>((p[0] * 77u + p[1] * 150u + p[2] * 29u) >> 8);
            }
            mask.channels = 1;
        }
        return MaskKind::Soft;
    }
    const PdfValue& stencil = src.doc.Get(src.dict, "Mask");
    if (stencil.IsStream()) {
        const Source maskSrc{src.doc, stencil, src.resources, false, {}, {}};
        decoded = DecodeStencil(maskSrc, maxPixels, mask) == Status::Ok;
        return MaskKind::Stencil;
    }
    if (stencil.IsArray()) {
        decoded = true;
        return MaskKind::ColourKey;
    }
    return MaskKind::None;
}

// Whether a decoded soft or stencil mask hides any part of its image.
bool Transparent(const Pixels& mask, MaskKind kind) {
    const uint8_t opaque = kind == MaskKind::Stencil ? 0 : 255;
    const uint8_t* bytes = mask.Bytes();
    for (size_t i = 0, n = size_t(mask.width) * mask.height; i < n; i++) {
        if (bytes[i] != opaque) return true;
    }
    return false;
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

Status WritePng(const Pixels& image, const AlphaRows* alpha, Encoded& out, Sink* sink) {
    const bool opaque = !alpha || alpha->Opaque();
    out.extension = "png";
    out.width = image.width;
    out.height = image.height;
    PngDestination destination(out, sink);
    bool ok;
    if (opaque) {
        ok = Png::EncodePixels(image.Bytes(), image.width, image.height, image.channels, destination);
    } else {
        const size_t rowBytes = size_t(image.width) * static_cast<size_t>(image.channels + 1);
        InterleavedRows rows{&image, alpha, {std::vector<uint8_t>(rowBytes), std::vector<uint8_t>(rowBytes)},
                             std::vector<uint8_t>(image.width)};
        ok = Png::EncodeRows(image.width, image.height, image.channels + 1, InterleavedRow, &rows, destination);
    }
    return destination.Result(ok);
}

// `image` with its soft or stencil mask, as a PNG; `key` the colour-key alpha DecodeSampled made.
Status WriteMasked(const Pixels& image, MaskKind kind, bool decoded, const Pixels& mask,
                   const std::vector<uint8_t>& key, Encoded& out, Sink* sink) {
    Status status;
    if ((kind == MaskKind::Soft || kind == MaskKind::Stencil) && decoded) {
        const AlphaRows alpha(image, &mask, kind == MaskKind::Stencil, nullptr);
        status = WritePng(image, &alpha, out, sink);
    } else if (!key.empty()) {
        const AlphaRows alpha(image, nullptr, false, &key);
        status = WritePng(image, &alpha, out, sink);
    } else {
        status = WritePng(image, nullptr, out, sink);
    }
    if (status == Status::Ok) out.maskDropped = kind != MaskKind::None && !decoded;
    return status;
}

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

// An image of `src` decoded to pixels whatever its filter (of those decoded here), with its
// mask: what Encode writes as a PNG, and what Gray composes on white.
struct DecodedImage {
    Pixels image;
    MaskKind kind = MaskKind::None;
    Pixels mask;
    bool maskDecoded = false;
    std::vector<uint8_t> key;  // colour-key alpha, one byte a pixel, when /Mask is an array
    uint32_t fullWidth = 0;    // the size before a reduced-resolution JPX decode (0: as decoded)
    uint32_t fullHeight = 0;
};

// `longerSide`: a JPX image may be decoded at a reduced resolution down to about this (0: full).
// A JPX opacity channel (/SMaskInData) is the soft mask when there is no /SMask stream, which
// takes precedence (ISO 32000-1 Table 89). `allowPacked`: a stencil, or gray of 1, 2 or 4 bits
// without a mask, may stay packed (Pixels::packedBits) for a PNG written from it.
Status DecodeImage(const Source& src, uint64_t maxPixels, DecodedImage& d, uint32_t longerSide = 0,
                   bool allowPacked = false) {
    if (src.Entry("ImageMask", "IM").Bool(false)) return DecodeStencil(src, maxPixels, d.image, allowPacked);
    SampleRequest req;
    req.maxPixels = maxPixels;
    req.keyAlpha = &d.key;
    req.longerSide = longerSide;
    if (allowPacked) {
        const PdfValue& mask = src.doc.Get(src.dict, "Mask");
        req.allowPacked = src.inlineImage ||
                          !(src.doc.Get(src.dict, "SMask").IsStream() || mask.IsStream() || mask.IsArray());
    }
    Pixels inData;
    if (!src.inlineImage && !src.doc.Get(src.dict, "SMask").IsStream()) req.inDataAlpha = &inData;
    const Status status = DecodeSampled(src, req, d.image, &d.fullWidth, &d.fullHeight);
    if (status != Status::Ok) return status;
    if (!inData.data.empty()) {
        d.kind = MaskKind::Soft;
        d.mask = std::move(inData);
        d.maskDecoded = true;
        return Status::Ok;
    }
    d.kind = ReadMask(src, maxPixels, d.mask, d.maskDecoded);
    return Status::Ok;
}

// A PNG of packed gray (Pixels::packedBits): the stored rows, complemented where needed, without
// being widened to 8 bits a pixel and narrowed again (a 300 dpi scan page is 8 MB of 8-bit pixels
// against 1 MB packed). The same pixels as WritePng of the 8-bit gray.
Status WritePacked(const Pixels& image, Encoded& out, Sink* sink) {
    out.extension = "png";
    out.width = image.width;
    out.height = image.height;
    PngDestination destination(out, sink);
    return destination.Result(Png::EncodeGray(image.Bytes(), image.packedRowBytes, image.width, image.height,
                                              image.packedBits, image.packedInvert, destination));
}

Status Encode(const Source& src, Encoded& out, uint64_t maxPixels, Sink* sink) {
    out = Encoded();
    const Filters::Kind stop = StopFilter(src);
    const bool stencil = src.Entry("ImageMask", "IM").Bool(false);
    Pixels mask;
    bool maskDecoded = false;
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
            if (DecodeImage(jpxSrc, maxPixels, d) == Status::Ok) {
                return WriteMasked(d.image, d.kind, d.maskDecoded, d.mask, d.key, out, sink);
            }
        }
        const MaskKind kind = stencil ? MaskKind::None : ReadMask(src, maxPixels, mask, maskDecoded);
        // A mask that hides something, decoded; only pixels can carry it.
        const bool maskShows = kind == MaskKind::ColourKey || (kind != MaskKind::None && maskDecoded && Transparent(mask, kind));
        if (stop == Filters::Kind::Dct && !stencil) {
            Source jpegSrc = src;
            jpegSrc.encoded = JpegStart(data);
            if (!jpegSrc.encoded.empty() && (maskShows || !JpegAsStored(src, jpegSrc.encoded))) {
                Pixels image;
                std::vector<uint8_t> key;
                SampleRequest req;
                req.maxPixels = maxPixels;
                req.keyAlpha = &key;
                const Status status = DecodeSampled(jpegSrc, req, image);
                if (status == Status::Ok) return WriteMasked(image, kind, maskDecoded, mask, key, out, sink);
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
        out.maskDropped = maskShows || (kind != MaskKind::None && !maskDecoded);
        return status;
    }
    if (stop != Filters::Kind::Flate && !Bitonal(stop)) return Status::Unsupported;

    DecodedImage d;
    const Status status = DecodeImage(src, maxPixels, d, 0, true);
    if (status != Status::Ok) return status;
    if (d.image.packedBits != 0) return WritePacked(d.image, out, sink);
    if (stencil) return WritePng(d.image, nullptr, out, sink);
    return WriteMasked(d.image, d.kind, d.maskDecoded, d.mask, d.key, out, sink);
}

// `d` as gray composited on white (image.h), reduced by area averaging to a longer side of at
// most `maxSide` (0: none), a source row at a time. Rows the data lacks (up to the image's
// declared height) are white, so that the pixels always cover the whole of image space.
void ComposeGray(const DecodedImage& d, uint32_t maxSide, GrayImage& out) {
    const Pixels& image = d.image;
    const uint32_t width = image.width, height = std::max(image.height, image.declaredHeight);
    std::unique_ptr<AlphaRows> alpha;
    if ((d.kind == MaskKind::Soft || d.kind == MaskKind::Stencil) && d.maskDecoded) {
        alpha = std::make_unique<AlphaRows>(image, &d.mask, d.kind == MaskKind::Stencil, nullptr);
    } else if (!d.key.empty()) {
        alpha = std::make_unique<AlphaRows>(image, nullptr, false, &d.key);
    }
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
    for (uint32_t y = 0; y < height; y++) {
        if (y >= image.height) {
            std::fill(gray.begin(), gray.end(), uint8_t(255));
        } else if (ch == 1) {
            std::memcpy(gray.data(), image.Bytes() + size_t(y) * width, width);
        } else {
            const uint8_t* pixels = image.Bytes() + size_t(y) * width * ch;
            for (uint32_t x = 0; x < width; x++) {
                const uint8_t* p = pixels + 3 * size_t(x);
                gray[x] = static_cast<uint8_t>((p[0] * 77u + p[1] * 150u + p[2] * 29u) >> 8);
            }
        }
        if (alpha && y < image.height) {
            alpha->Row(y, alphaRow.data());
            for (uint32_t x = 0; x < width; x++) {
                const uint32_t a = alphaRow[x];
                gray[x] = static_cast<uint8_t>((gray[x] * a + 255 * (255 - a) + 127) / 255);
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
                     uint64_t maxPixels, Sink* sink) {
    out = Encoded();
    const PdfValue& stream = document.Resolve(image);
    if (!stream.IsStream()) return Status::Corrupt;
    const Source src{document, stream, document.Resolve(resources), false, {}, {}};
    return Encode(src, out, maxPixels, sink);
}

Status EncodeInline(const Document& document, const PdfValue& dictionary, std::string_view data,
                    const PdfValue& resources, Encoded& out, uint64_t maxPixels, Sink* sink) {
    out = Encoded();
    if (!dictionary.IsDict()) return Status::Corrupt;
    const Source src{document, dictionary, document.Resolve(resources), true, data, {}};
    return Encode(src, out, maxPixels, sink);
}

Status GrayXObject(const Document& document, const PdfValue& image, const PdfValue& resources, GrayImage& out,
                   uint32_t maxSide, uint64_t maxPixels) {
    out = GrayImage();
    const PdfValue& stream = document.Resolve(image);
    if (!stream.IsStream()) return Status::Corrupt;
    const Source src{document, stream, document.Resolve(resources), false, {}, {}};
    return Gray(src, maxSide, maxPixels, out);
}

Status GrayInline(const Document& document, const PdfValue& dictionary, std::string_view data,
                  const PdfValue& resources, GrayImage& out, uint32_t maxSide, uint64_t maxPixels) {
    out = GrayImage();
    if (!dictionary.IsDict()) return Status::Corrupt;
    const Source src{document, dictionary, document.Resolve(resources), true, data, {}};
    return Gray(src, maxSide, maxPixels, out);
}

} // namespace RayoPdf::Image
