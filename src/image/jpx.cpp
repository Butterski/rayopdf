// SPDX-License-Identifier: Apache-2.0
// The JPX decoder's front end (src/image/jpx.h): the JP2 file format boxes of T.800 annex I, the
// channels a PDF reads (palette, channel definitions, sYCC to sRGB as OpenJPEG's opj_decompress
// converts it, upsampling of subsampled components, scaling to 8 or 16 bits), and the entry
// points. The codestream itself is decoded by jpx_codestream.cpp.
#include "image/jpx.h"

#include "image/jpx_internal.h"

#include <algorithm>
#include <cstring>

namespace RayoPdf::Image::Jpx {
namespace {

using Internal::CodestreamInfo;
using Internal::ComponentInfo;
using Internal::TileComponent;
using Internal::TileSink;

inline uint32_t Be16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }
inline uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

constexpr uint8_t kSignature[12] = {0, 0, 0, 12, 'j', 'P', ' ', ' ', 0x0D, 0x0A, 0x87, 0x0A};
constexpr int kMaxPaletteEntries = 1024;
constexpr int kMaxPaletteColumns = 256;

struct MapEntry {
    uint16_t component;
    uint8_t type;  // 0 direct, 1 palette
    uint8_t column;
};

struct ChannelDefinition {
    uint16_t channel;
    uint16_t type;         // 0 colour, 1 opacity, 2 premultiplied opacity
    uint16_t association;  // 0 the whole image, 1.. a colour, 65535 none
};

struct Jp2 {
    size_t codestream = 0;
    size_t codestreamSize = 0;
    bool hasColour = false;
    uint8_t method = 0;
    uint32_t enumerated = 0;
    size_t iccOffset = 0;
    size_t iccSize = 0;
    int paletteEntries = 0;
    int paletteColumns = 0;
    int paletteBits[kMaxPaletteColumns] = {};
    bool paletteSigned[kMaxPaletteColumns] = {};
    std::vector<int32_t> palette;  // entries x columns
    std::vector<MapEntry> map;
    std::vector<ChannelDefinition> definitions;
};

// A box at `pos` (I.4): its type, body and the position after it; boxes end at `end`.
bool NextBox(const uint8_t* data, size_t pos, size_t end, uint32_t& type, size_t& body, size_t& bodyEnd) {
    if (pos + 8 > end) return false;
    uint64_t length = Be32(data + pos);
    type = Be32(data + pos + 4);
    size_t header = 8;
    if (length == 1) {
        if (pos + 16 > end) return false;
        length = (uint64_t(Be32(data + pos + 8)) << 32) | Be32(data + pos + 12);
        header = 16;
    } else if (length == 0) {
        length = end - pos;
    }
    if (length < header) return false;
    body = pos + header;
    bodyEnd = length > end - pos ? end : pos + static_cast<size_t>(length);  // cut at the data's end
    return true;
}

constexpr uint32_t BoxType(const char (&t)[5]) {
    return (uint32_t(uint8_t(t[0])) << 24) | (uint32_t(uint8_t(t[1])) << 16) | (uint32_t(uint8_t(t[2])) << 8) | uint8_t(t[3]);
}

void ReadHeaderBox(const uint8_t* data, size_t pos, size_t end, Jp2& jp2) {
    for (int guard = 0; guard < 4096 && pos < end; guard++) {
        uint32_t type = 0;
        size_t body = 0, bodyEnd = 0;
        if (!NextBox(data, pos, end, type, body, bodyEnd)) break;
        const uint8_t* p = data + body;
        const size_t n = bodyEnd - body;
        if (type == BoxType("colr") && !jp2.hasColour && n >= 3) {
            // Only the first colour specification counts (I.5.3.3).
            jp2.method = p[0];
            if (jp2.method == 1 && n >= 7) {
                jp2.hasColour = true;
                jp2.enumerated = Be32(p + 3);
            } else if ((jp2.method == 2 || jp2.method == 3) && n > 3) {
                jp2.hasColour = true;
                jp2.iccOffset = body + 3;
                jp2.iccSize = n - 3;
            }
        } else if (type == BoxType("pclr") && n >= 3 && jp2.paletteColumns == 0) {
            const int entries = static_cast<int>(Be16(p));
            const int columns = p[2];
            if (entries < 1 || entries > kMaxPaletteEntries || columns < 1 || n < 3 + size_t(columns)) {
                pos = bodyEnd;
                continue;
            }
            size_t rowBytes = 0;
            bool ok = true;
            for (int j = 0; j < columns; j++) {
                jp2.paletteBits[j] = (p[3 + j] & 0x7F) + 1;
                jp2.paletteSigned[j] = (p[3 + j] & 0x80) != 0;
                if (jp2.paletteBits[j] > Internal::kMaxPrecision) ok = false;
                rowBytes += size_t(jp2.paletteBits[j] + 7) / 8;
            }
            if (!ok || n < 3 + size_t(columns) + rowBytes * entries) {
                pos = bodyEnd;
                continue;
            }
            jp2.palette.resize(size_t(entries) * columns);
            const uint8_t* q = p + 3 + columns;
            for (int i = 0; i < entries; i++) {
                for (int j = 0; j < columns; j++) {
                    const int bytes = (jp2.paletteBits[j] + 7) / 8;
                    uint32_t v = 0;
                    for (int k = 0; k < bytes; k++) v = (v << 8) | *q++;
                    int32_t value = static_cast<int32_t>(v & ((uint32_t(1) << jp2.paletteBits[j]) - 1));
                    if (jp2.paletteSigned[j] && (value >> (jp2.paletteBits[j] - 1))) value -= int32_t(1) << jp2.paletteBits[j];
                    jp2.palette[size_t(i) * columns + j] = value;
                }
            }
            jp2.paletteEntries = entries;
            jp2.paletteColumns = columns;
        } else if (type == BoxType("cmap") && jp2.map.empty()) {
            for (size_t i = 0; i + 4 <= n && jp2.map.size() < 1024; i += 4) {
                jp2.map.push_back(MapEntry{static_cast<uint16_t>(Be16(p + i)), p[i + 2], p[i + 3]});
            }
        } else if (type == BoxType("cdef") && jp2.definitions.empty() && n >= 2) {
            const size_t count = Be16(p);
            for (size_t i = 0; i < count && 2 + 6 * i + 6 <= n; i++) {
                const uint8_t* e = p + 2 + 6 * i;
                jp2.definitions.push_back(
                    ChannelDefinition{static_cast<uint16_t>(Be16(e)), static_cast<uint16_t>(Be16(e + 2)), static_cast<uint16_t>(Be16(e + 4))});
            }
        }
        pos = bodyEnd;
    }
}

// Finds the codestream: a JP2/JPX file's first contiguous codestream box, or the data itself.
Result ReadContainer(const uint8_t* data, size_t size, Jp2& jp2, bool& isJp2) {
    isJp2 = false;
    if (size >= 12 && std::memcmp(data, kSignature, 12) == 0) {
        isJp2 = true;
        size_t pos = 12;
        bool header = false;
        for (int guard = 0; guard < 65536 && pos < size; guard++) {
            uint32_t type = 0;
            size_t body = 0, bodyEnd = 0;
            if (!NextBox(data, pos, size, type, body, bodyEnd)) break;
            if (type == BoxType("jp2h") && !header) {
                header = true;
                ReadHeaderBox(data, body, bodyEnd, jp2);
            } else if (type == BoxType("jp2c")) {
                jp2.codestream = body;
                jp2.codestreamSize = bodyEnd - body;
                break;
            } else if (header && (type == BoxType("colr") || type == BoxType("pclr") || type == BoxType("cmap") ||
                                  type == BoxType("cdef"))) {
                // A header box misplaced after the header box: read anyway, as OpenJPEG does.
                ReadHeaderBox(data, pos, bodyEnd, jp2);
            }
            pos = bodyEnd;
        }
        if (jp2.codestreamSize == 0) return Result::Corrupt;
        if (jp2.paletteColumns == 0 || jp2.map.empty()) {
            jp2.paletteColumns = 0;
            jp2.map.clear();
        }
        return Result::Ok;
    }
    if (size >= 4 && Be16(data) == 0xFF4F && Be16(data + 2) == 0xFF51) {
        jp2.codestream = 0;
        jp2.codestreamSize = size;
        return Result::Ok;
    }
    return Result::Corrupt;
}

Colour ColourOf(const Jp2& jp2) {
    if (!jp2.hasColour) return Colour::Unspecified;
    if (jp2.method != 1) return Colour::Icc;
    switch (jp2.enumerated) {
    case 16:
    case 18: return Colour::Srgb;
    case 17: return Colour::Gray;
    case 12: return Colour::Cmyk;
    default: return Colour::Other;
    }
}

// The colour channels the colour specification has (0: unknown).
int ColourChannelsOf(const uint8_t* data, const Jp2& jp2) {
    if (!jp2.hasColour) return 0;
    if (jp2.method != 1) {
        if (jp2.iccSize < 20) return 0;
        const uint32_t space = Be32(data + jp2.iccOffset + 16);
        if (space == BoxType("GRAY")) return 1;
        if (space == BoxType("CMYK")) return 4;
        if (space == BoxType("RGB ") || space == BoxType("Lab ") || space == BoxType("XYZ ") || space == BoxType("YCbr")) return 3;
        return 0;
    }
    switch (jp2.enumerated) {
    case 0:
    case 15:
    case 17: return 1;
    case 12:
    case 13: return 4;
    case 1:
    case 3:
    case 4:
    case 9:
    case 11:
    case 14:
    case 16:
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24: return 3;
    default: return 0;
    }
}

void FillInfo(const uint8_t* data, const Jp2& jp2, bool isJp2, const CodestreamInfo& cs, Info& info) {
    info = Info();
    info.width = cs.x1 - cs.x0;
    info.height = cs.y1 - cs.y0;
    info.components = static_cast<int>(cs.components.size());
    for (const ComponentInfo& c : cs.components) info.precision = std::max(info.precision, c.precision);
    info.levels = cs.levels;
    info.jp2 = isJp2;
    info.reversible = cs.reversible;
    info.palette = jp2.paletteColumns > 0;
    info.channelDefinitions = !jp2.definitions.empty();
    info.colour = ColourOf(jp2);
    info.enumerated = jp2.hasColour && jp2.method == 1 ? jp2.enumerated : 0;
    if (info.colour == Colour::Icc) {
        info.iccOffset = jp2.iccOffset;
        info.iccSize = jp2.iccSize;
    }
    (void)data;
}

// ---------------------------------------------------------------------------- channel mapping

// One output channel: a component, through a palette column or directly.
struct Channel {
    int component = 0;
    int column = -1;  // palette column, -1 direct
    int precision = 8;
    bool isSigned = false;
};

struct Mapping {
    std::vector<Channel> channels;  // colour channels, then the opacity channel when kept
    int colourChannels = 0;
    bool opacity = false;
    bool premultiplied = false;
    bool sycc = false;  // the three colour channels are sYCC, converted to sRGB
    int bits = 8;
    bool scale = true;  // scale each channel to the full 8- or 16-bit range (not palette indices)
    int precision = 0;  // the largest channel precision
};

Result BuildMapping(const uint8_t* data, const Jp2& jp2, const CodestreamInfo& cs, const Options& options, Mapping& m) {
    // The channels after the palette (I.5.3.4, I.5.3.5).
    std::vector<Channel> all;
    const int components = static_cast<int>(cs.components.size());
    const bool palette = jp2.paletteColumns > 0 && !options.ignorePalette;
    if (palette) {
        // One mapping per palette column, checked as OpenJPEG checks them; a single component
        // whose mapping leaves a column unused gets every column mapped in order (OpenJPEG's
        // repair of a common broken component mapping box).
        const int columns = jp2.paletteColumns;
        if (static_cast<int>(jp2.map.size()) < columns) return Result::Corrupt;
        std::vector<MapEntry> map(jp2.map.begin(), jp2.map.begin() + columns);
        std::vector<uint8_t> used(size_t(columns), 0);
        for (int i = 0; i < columns; i++) {
            const MapEntry& e = map[size_t(i)];
            if (e.component >= components || e.type > 1 || e.column >= columns) return Result::Corrupt;
            if (e.type == 1 && (used[e.column] || e.column != i)) return Result::Corrupt;
            if (e.type == 0 && e.column != 0) return Result::Corrupt;
            used[e.column] = 1;
        }
        for (int i = 0; i < columns; i++) {
            if (!used[size_t(i)] && map[size_t(i)].type != 0) return Result::Corrupt;
        }
        if (components == 1 && std::find(used.begin(), used.end(), 0) != used.end()) {
            for (int i = 0; i < columns; i++) map[size_t(i)] = MapEntry{0, 1, static_cast<uint8_t>(i)};
        }
        for (const MapEntry& e : map) {
            Channel ch;
            ch.component = e.component;
            if (e.type == 1) {
                ch.column = e.column;
                ch.precision = jp2.paletteBits[e.column];
                ch.isSigned = jp2.paletteSigned[e.column];
            } else {
                ch.precision = cs.components[e.component].precision;
                ch.isSigned = cs.components[e.component].isSigned;
            }
            all.push_back(ch);
        }
    } else {
        for (int c = 0; c < components; c++) {
            Channel ch;
            ch.component = c;
            ch.precision = cs.components[c].precision;
            ch.isSigned = cs.components[c].isSigned;
            all.push_back(ch);
        }
    }
    const int count = static_cast<int>(all.size());
    int wanted = ColourChannelsOf(data, jp2);
    if (palette || options.ignorePalette) {
        // A palette's colour space describes its outputs; indices stay one channel.
        if (options.ignorePalette && jp2.paletteColumns > 0) wanted = 0;
    }
    if (wanted == 0 || wanted > count) wanted = count;
    // Channel definitions (I.5.3.6): colour channels by association, one opacity channel.
    std::vector<int> colour;
    int opacity = -1;
    bool premultiplied = false;
    if (!jp2.definitions.empty()) {
        std::vector<int> slot(size_t(count), -1);
        for (const ChannelDefinition& d : jp2.definitions) {
            if (d.channel >= count) continue;
            if (d.type == 0 && d.association >= 1 && d.association <= count) slot[d.association - 1] = d.channel;
            else if ((d.type == 1 || d.type == 2) && opacity < 0) {
                opacity = d.channel;
                premultiplied = d.type == 2;
            }
        }
        for (int k = 0; k < count && slot[size_t(k)] >= 0; k++) colour.push_back(slot[size_t(k)]);
        if (colour.empty()) {
            for (int k = 0; k < count; k++) {
                if (k != opacity) colour.push_back(k);
            }
        }
        if (options.pdfComponents > 0 && static_cast<int>(colour.size()) > options.pdfComponents) colour.resize(size_t(options.pdfComponents));
    } else {
        if (options.pdfComponents > 0) wanted = std::min(options.pdfComponents, count);
        else if (!jp2.hasColour && options.smaskInData != 0 && (count == 2 || count == 4)) wanted = count - 1;
        for (int k = 0; k < wanted; k++) colour.push_back(k);
        if (options.smaskInData != 0 && wanted < count) opacity = wanted;
    }
    m = Mapping();
    for (int k : colour) m.channels.push_back(all[size_t(k)]);
    m.colourChannels = static_cast<int>(m.channels.size());
    if (options.smaskInData != 0 && opacity >= 0) {
        m.channels.push_back(all[size_t(opacity)]);
        m.opacity = true;
        m.premultiplied = premultiplied;
    }
    // sYCC: enumerated 18, or (as opj_decompress guesses) three components with subsampled chroma.
    if (!palette && m.colourChannels == 3 && m.channels[0].component == 0 && m.channels[1].component == 1 &&
        m.channels[2].component == 2) {
        const bool guessed = components == 3 && cs.components[0].dx == cs.components[0].dy && cs.components[1].dx != 1;
        m.sycc = (jp2.hasColour && jp2.method == 1 && jp2.enumerated == 18) || guessed;
    }
    m.bits = 8;
    m.scale = !options.ignorePalette;
    for (const Channel& ch : m.channels) {
        if (ch.precision > 8) m.bits = 16;
        m.precision = std::max(m.precision, ch.precision);
    }
    return Result::Ok;
}

class PixelWriter {
public:
    PixelWriter(const Jp2& jp2, const Mapping& m, Image& image) : jp2_(jp2), m_(m), image_(image) {
        const size_t n = m.channels.size();
        fast_ = !m.sycc && m.bits == 8;
        for (size_t k = 0; k < n; k++) {
            const Channel& ch = m.channels[k];
            fast_ = fast_ && ch.column < 0 && ch.precision == 8 && !ch.isSigned;
        }
    }

    // Every channel is an 8-bit unsigned component taken as it is.
    bool Bytes() const { return fast_; }

    // With Bytes(): writes `width` pixels of image row `y` from x = `x` from row j of the tile's
    // components, converted straight to bytes. `bytes` is the calling thread's buffer.
    void TileRow(const TileComponent* components, uint32_t j, uint32_t x, uint32_t y, uint32_t width,
                 std::vector<uint8_t>& bytes) const {
        const size_t n = m_.channels.size();
        uint8_t* out = reinterpret_cast<uint8_t*>(&image_.samples[0]) + (size_t(y) * image_.width + x) * n;
        if (n == 1) {
            components[m_.channels[0].component].RowBytes(j, width, out);
            return;
        }
        bytes.resize(n * width);
        for (size_t k = 0; k < n; k++) components[m_.channels[k].component].RowBytes(j, width, bytes.data() + k * width);
        const uint8_t* b = bytes.data();
        if (n == 3) {
            for (uint32_t i = 0; i < width; i++, out += 3) {
                out[0] = b[i];
                out[1] = b[width + i];
                out[2] = b[2 * size_t(width) + i];
            }
            return;
        }
        for (uint32_t i = 0; i < width; i++) {
            for (size_t k = 0; k < n; k++) *out++ = b[k * width + i];
        }
    }

    // Writes `width` pixels of image row `y` from x = `x`, sample i of component c at
    // region.samples[c][i] (the row's start already applied).
    void Row(const int32_t* const* rows, uint32_t x, uint32_t y, uint32_t width) const {
        const size_t n = m_.channels.size();
        const size_t bytes = m_.bits / 8;
        uint8_t* out = reinterpret_cast<uint8_t*>(&image_.samples[0]) + (size_t(y) * image_.width + x) * n * bytes;
        if (fast_) {
            // 8-bit unsigned channels arrive clipped to [0, 255].
            for (size_t k = 0; k < n; k++) Internal::NarrowBytes(rows[m_.channels[k].component], out + k, width, n);
            return;
        }
        int32_t v[Internal::kMaxComponents + 1];
        for (uint32_t i = 0; i < width; i++) {
            for (size_t k = 0; k < n; k++) {
                const Channel& ch = m_.channels[k];
                int32_t s = rows[ch.component][i];
                if (ch.column >= 0) {
                    const int32_t index = s < 0 ? 0 : (s >= jp2_.paletteEntries ? jp2_.paletteEntries - 1 : s);
                    s = jp2_.palette[size_t(index) * jp2_.paletteColumns + ch.column];
                }
                v[k] = s;
            }
            if (m_.sycc) Sycc(v);
            for (size_t k = 0; k < n; k++) {
                const Channel& ch = m_.channels[k];
                const uint32_t max = (uint32_t(1) << ch.precision) - 1;
                int64_t u = int64_t(v[k]) + (ch.isSigned ? (int64_t(1) << (ch.precision - 1)) : 0);
                u = u < 0 ? 0 : (u > int64_t(max) ? int64_t(max) : u);
                if (m_.bits == 8) {
                    *out++ = static_cast<uint8_t>(ch.precision == 8 || !m_.scale ? u : (uint64_t(u) * 255 + max / 2) / max);
                } else {
                    const uint32_t w =
                        static_cast<uint32_t>(ch.precision == 16 || !m_.scale ? u : (uint64_t(u) * 65535 + max / 2) / max);
                    *out++ = static_cast<uint8_t>(w >> 8);
                    *out++ = static_cast<uint8_t>(w);
                }
            }
        }
    }

private:
    // opj_decompress's sycc_to_rgb (color.c): offsets removed, double-precision products
    // truncated toward zero, clipped to the precision.
    void Sycc(int32_t* v) const {
        const int precision = m_.channels[0].precision;
        const int offset = 1 << (precision - 1);
        const int upb = (1 << precision) - 1;
        const int y = v[0];
        const int cb = v[1] - offset;
        const int cr = v[2] - offset;
        int r = y + static_cast<int>(1.402 * static_cast<float>(cr));
        int g = y - static_cast<int>(0.344 * static_cast<float>(cb) + 0.714 * static_cast<float>(cr));
        int b = y + static_cast<int>(1.772 * static_cast<float>(cb));
        v[0] = r < 0 ? 0 : (r > upb ? upb : r);
        v[1] = g < 0 ? 0 : (g > upb ? upb : g);
        v[2] = b < 0 ? 0 : (b > upb ? upb : b);
    }

    const Jp2& jp2_;
    const Mapping& m_;
    Image& image_;
    bool fast_ = false;
};

// Writes each tile straight into the image: every component at full size. With threads the rows
// of a tile are written in parts: a row reads its own samples and writes its own pixels.
class DirectSink : public TileSink {
public:
    DirectSink(const PixelWriter& writer, Image& image, Internal::Workers workers)
        : writer_(writer), image_(image), workers_(workers), scratch_(workers.threads ? workers.threads->Workers() : 1) {}

    Result Begin(const CodestreamInfo& info) override {
        ox_ = info.components[0].x0;
        oy_ = info.components[0].y0;
        return Result::Ok;
    }

    void Tile(const TileComponent* components, int count) override {
        const TileComponent& first = components[0];
        if (first.width == 0 || first.height == 0 || first.x0 < ox_ || first.y0 < oy_) return;
        const uint32_t x = first.x0 - ox_;
        const uint32_t y = first.y0 - oy_;
        if (x >= image_.width || y >= image_.height) return;
        const uint32_t width = std::min(first.width, image_.width - x);
        const uint32_t height = std::min(first.height, image_.height - y);
        for (int c = 1; c < count; c++) {
            if (components[c].width < width || components[c].height < height) return;
        }
        const auto rows = [&](unsigned worker, uint32_t from, uint32_t to) {
            Scratch& scratch = scratch_[workers_.threads ? worker : 0];
            if (writer_.Bytes()) {
                for (uint32_t j = from; j < to; j++) writer_.TileRow(components, j, x, y + j, width, scratch.bytes);
                return;
            }
            // Each image row: the components' samples converted into one row buffer each.
            scratch.samples.resize(size_t(count) * width);
            const int32_t* pointers[Internal::kMaxComponents];
            for (int c = 0; c < count; c++) pointers[c] = scratch.samples.data() + size_t(c) * width;
            for (uint32_t j = from; j < to; j++) {
                for (int c = 0; c < count; c++) components[c].Row(j, width, scratch.samples.data() + size_t(c) * width);
                writer_.Row(pointers, x, y + j, width);
            }
        };
        if (workers_.threads && workers_.threads->Started() > 0 && uint64_t(width) * height >= workers_.splitSamples) {
            const uint32_t lines = workers_.partLines;
            workers_.threads->Run(workers_.worker, (size_t(height) + lines - 1) / lines, [&](unsigned worker, size_t part) {
                const uint32_t from = static_cast<uint32_t>(part) * lines;
                rows(worker, from, static_cast<uint32_t>(std::min<uint64_t>(height, uint64_t(from) + lines)));
            });
        } else {
            rows(workers_.worker, 0, height);
        }
    }

private:
    struct Scratch {
        std::vector<uint8_t> bytes;     // TileRow's channels, a row each
        std::vector<int32_t> samples;   // the components' samples of one row
    };

    const PixelWriter& writer_;
    Image& image_;
    Internal::Workers workers_;
    uint32_t ox_ = 0, oy_ = 0;
    std::vector<Scratch> scratch_;  // by worker
};

// Collects the components as planes.
class PlaneSink : public TileSink {
public:
    explicit PlaneSink(std::vector<Component>& components) : components_(components) {}

    Result Begin(const CodestreamInfo& info) override {
        components_.resize(info.components.size());
        for (size_t c = 0; c < info.components.size(); c++) {
            const ComponentInfo& ci = info.components[c];
            Component& comp = components_[c];
            comp.x0 = ci.x0;
            comp.y0 = ci.y0;
            comp.width = ci.width;
            comp.height = ci.height;
            comp.dx = ci.dx;
            comp.dy = ci.dy;
            comp.precision = ci.precision;
            comp.isSigned = ci.isSigned;
            comp.samples.assign(size_t(ci.width) * ci.height, 0);
        }
        return Result::Ok;
    }

    void Tile(const TileComponent* components, int count) override {
        for (int c = 0; c < count && c < static_cast<int>(components_.size()); c++) {
            const TileComponent& t = components[c];
            Component& comp = components_[size_t(c)];
            if (t.width == 0 || t.height == 0 || t.x0 < comp.x0 || t.y0 < comp.y0) continue;
            const uint32_t x = t.x0 - comp.x0;
            const uint32_t y = t.y0 - comp.y0;
            if (x >= comp.width || y >= comp.height) continue;
            const uint32_t width = std::min(t.width, comp.width - x);
            const uint32_t height = std::min(t.height, comp.height - y);
            for (uint32_t j = 0; j < height; j++) t.Row(j, width, comp.samples.data() + size_t(y + j) * comp.width + x);
        }
    }

private:
    std::vector<Component>& components_;
};

uint32_t CeilShift(uint32_t v, int r) { return static_cast<uint32_t>((uint64_t(v) + (uint64_t(1) << r) - 1) >> r); }

// The threads and the grain `options` asks for (parts of whole multiples of 16 lines).
Internal::Workers WorkersOf(const Options& options) {
    Internal::Workers workers;
    workers.threads = options.threads;
    workers.worker = options.worker;
    workers.splitSamples = options.splitSamples;
    workers.partLines = std::max<uint32_t>(16, options.partLines & ~uint32_t(15));
    return workers;
}

} // namespace

Result ReadInfo(const uint8_t* data, size_t size, Info& info) {
    info = Info();
    Jp2 jp2;
    bool isJp2 = false;
    Result r = ReadContainer(data, size, jp2, isJp2);
    if (r != Result::Ok) return r;
    CodestreamInfo cs;
    r = Internal::ReadMainHeader(data + jp2.codestream, jp2.codestreamSize, cs);
    if (r != Result::Ok) return r;
    FillInfo(data, jp2, isJp2, cs, info);
    return Result::Ok;
}

int ReduceFor(const Info& info, uint32_t minSide) {
    int reduce = 0;
    while (reduce < info.levels) {
        const uint32_t w = CeilShift(info.width, reduce + 1);
        const uint32_t h = CeilShift(info.height, reduce + 1);
        if (w < minSide || h < minSide) break;
        reduce++;
    }
    return reduce;
}

Result DecodeComponents(const uint8_t* data, size_t size, const Options& options, Info& info,
                        std::vector<Component>& components) {
    components.clear();
    info = Info();
    Jp2 jp2;
    bool isJp2 = false;
    Result r = ReadContainer(data, size, jp2, isJp2);
    if (r != Result::Ok) return r;
    const uint8_t* cs = data + jp2.codestream;
    CodestreamInfo csInfo;
    r = Internal::ReadMainHeader(cs, jp2.codestreamSize, csInfo);
    if (r != Result::Ok) return r;
    FillInfo(data, jp2, isJp2, csInfo, info);
    PlaneSink sink(components);
    r = Internal::DecodeCodestream(cs, jp2.codestreamSize, options.reduce, options.maxPixels, sink, WorkersOf(options));
    if (r != Result::Ok) components.clear();
    return r;
}

Result Decode(const uint8_t* data, size_t size, const Options& options, Image& image) {
    image = Image();
    Jp2 jp2;
    bool isJp2 = false;
    Result r = ReadContainer(data, size, jp2, isJp2);
    if (r != Result::Ok) return r;
    const uint8_t* cs = data + jp2.codestream;
    CodestreamInfo csInfo;
    r = Internal::ReadMainHeader(cs, jp2.codestreamSize, csInfo);
    if (r != Result::Ok) return r;
    FillInfo(data, jp2, isJp2, csInfo, image.info);
    Mapping m;
    r = BuildMapping(data, jp2, csInfo, options, m);
    if (r != Result::Ok) return r;
    if (m.channels.empty()) return Result::Corrupt;
    const int reduce = std::max(0, std::min(options.reduce, csInfo.levels));
    image.reduce = reduce;
    image.width = CeilShift(csInfo.x1, reduce) - CeilShift(csInfo.x0, reduce);
    image.height = CeilShift(csInfo.y1, reduce) - CeilShift(csInfo.y0, reduce);
    if (uint64_t(image.width) * image.height > options.maxPixels) return Result::TooLarge;
    image.channels = static_cast<int>(m.channels.size());
    image.colourChannels = m.colourChannels;
    image.opacity = m.opacity;
    image.premultiplied = m.premultiplied;
    image.bitsPerComponent = m.bits;
    image.precision = m.precision;
    image.colour = m.sycc ? Colour::Srgb : image.info.colour;
    if (jp2.paletteColumns > 0 && options.ignorePalette) image.colour = Colour::Unspecified;
    const size_t bytes = size_t(image.width) * image.height * size_t(image.channels) * size_t(m.bits / 8);
    image.samples.assign(bytes, '\0');
    const PixelWriter writer(jp2, m, image);
    const Internal::Workers workers = WorkersOf(options);
    bool full = true;
    for (const ComponentInfo& ci : csInfo.components) full = full && ci.dx == 1 && ci.dy == 1;
    if (full) {
        DirectSink sink(writer, image, workers);
        r = Internal::DecodeCodestream(cs, jp2.codestreamSize, reduce, options.maxPixels, sink, workers);
    } else {
        // Subsampled components: decode the planes, then repeat each sample over the image
        // pixels whose reference grid position it covers.
        std::vector<Component> planes;
        PlaneSink sink(planes);
        r = Internal::DecodeCodestream(cs, jp2.codestreamSize, reduce, options.maxPixels, sink, workers);
        if (r == Result::Ok) {
            const uint32_t ox = CeilShift(csInfo.x0, reduce);
            const uint32_t oy = CeilShift(csInfo.y0, reduce);
            const size_t n = planes.size();
            std::vector<std::vector<int32_t>> rows(n, std::vector<int32_t>(image.width));
            const int32_t* pointers[Internal::kMaxComponents];
            for (uint32_t y = 0; y < image.height; y++) {
                for (size_t c = 0; c < n; c++) {
                    const Component& p = planes[c];
                    if (p.width == 0 || p.height == 0) {
                        std::fill(rows[c].begin(), rows[c].end(), 0);
                    } else {
                        const uint32_t gy = (oy + y) / p.dy;
                        const uint32_t sy = gy < p.y0 ? 0 : std::min(gy - p.y0, p.height - 1);
                        const int32_t* src = p.samples.data() + size_t(sy) * p.width;
                        for (uint32_t x = 0; x < image.width; x++) {
                            const uint32_t gx = (ox + x) / p.dx;
                            const uint32_t sx = gx < p.x0 ? 0 : std::min(gx - p.x0, p.width - 1);
                            rows[c][x] = src[sx];
                        }
                    }
                    pointers[c] = rows[c].data();
                }
                writer.Row(pointers, 0, y, image.width);
            }
        }
    }
    if (r != Result::Ok) {
        image.samples.clear();
        image.samples.shrink_to_fit();
    }
    return r;
}

} // namespace RayoPdf::Image::Jpx
