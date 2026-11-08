// SPDX-License-Identifier: Apache-2.0
// Glyph outlines (text/outline.h). The TrueType reader is forked from rayomd-gui
// src/text/glyf.cpp and the Type 2 interpreter from rayomd-gui src/text/cff_outlines.cpp, both at
// 9fbbfaf (Apache-2.0, same author); subroutine calls, seac and the Type 1 format are added here.

#include "text/outline.h"

#include "pdf/syntax.h"
#include "text/tables.h"

#include <algorithm>
#include <cstring>

namespace RayoPdf::Text::Program {
namespace {

uint32_t U16(std::string_view d, size_t at) {
    if (at > d.size() || d.size() - at < 2) return 0;
    return static_cast<uint32_t>(static_cast<unsigned char>(d[at]) << 8 | static_cast<unsigned char>(d[at + 1]));
}

uint32_t U32(std::string_view d, size_t at) {
    if (at > d.size() || d.size() - at < 4) return 0;
    return U16(d, at) << 16 | U16(d, at + 2);
}

int16_t I16(const uint8_t* p) {
    return static_cast<int16_t>(static_cast<uint16_t>(p[0] << 8 | p[1]));
}

void MapPoint(const Raster::Matrix& m, double x, double y, float& ox, float& oy) {
    double dx, dy;
    m.Apply(x, y, dx, dy);
    ox = static_cast<float>(dx);
    oy = static_cast<float>(dy);
}

// ---- TrueType ---------------------------------------------------------------------------------

constexpr uint32_t kMaxGlyfPoints = 1u << 20;
constexpr uint32_t kMaxGlyfComponents = 4096;
enum : uint8_t { kOnCurve = 0x01, kXShort = 0x02, kYShort = 0x04, kRepeat = 0x08, kXSame = 0x10, kYSame = 0x20 };
enum : uint16_t {
    kArgsAreWords = 0x0001,
    kArgsAreXy = 0x0002,
    kHaveScale = 0x0008,
    kMoreComponents = 0x0020,
    kXyScale = 0x0040,
    kTwoByTwo = 0x0080,
};

struct GlyfBudget {
    uint32_t points = 0;
    uint32_t components = 0;
};

class GlyfReader {
public:
    GlyfReader(const GlyfOutlines& font, GlyfBudget& budget) : font_(font), budget_(budget) {}

    bool Emit(uint32_t glyph, const Raster::Matrix& m, int depth, Raster::Path& out) {
        if (glyph >= font_.glyphCount) return false;
        uint32_t start = 0, end = 0;
        if (!Locate(glyph, start, end)) return false;
        if (start == end) return true;  // no outline (a space)
        const uint8_t* const p = reinterpret_cast<const uint8_t*>(font_.glyf.data()) + start;
        const size_t size = end - start;
        if (size < 10) return false;
        const int16_t contours = I16(p);
        if (contours >= 0) return Simple(p, size, static_cast<uint32_t>(contours), m, out);
        return Composite(p, size, m, depth, out);
    }

private:
    bool Locate(uint32_t glyph, uint32_t& start, uint32_t& end) const {
        if (font_.longLoca) {
            if ((uint64_t(glyph) + 2) * 4 > font_.loca.size()) return false;
            start = U32(font_.loca, 4 * size_t(glyph));
            end = U32(font_.loca, 4 * size_t(glyph) + 4);
        } else {
            if ((uint64_t(glyph) + 2) * 2 > font_.loca.size()) return false;
            start = 2u * U16(font_.loca, 2 * size_t(glyph));
            end = 2u * U16(font_.loca, 2 * size_t(glyph) + 2);
        }
        return start <= end && end <= font_.glyf.size();
    }

    bool Simple(const uint8_t* p, size_t size, uint32_t contours, const Raster::Matrix& m, Raster::Path& out) {
        if (contours == 0) return true;
        size_t at = 10;
        if (contours * 2 > size - at) return false;
        endPoints_.resize(contours);
        int32_t previous = -1;
        for (uint32_t c = 0; c < contours; c++) {
            const int32_t e = static_cast<uint16_t>(I16(p + at + 2 * c));
            if (e <= previous) return false;
            endPoints_[c] = static_cast<uint16_t>(e);
            previous = e;
        }
        at += contours * 2;
        const uint32_t points = static_cast<uint32_t>(previous) + 1;
        budget_.points += points;
        if (budget_.points > kMaxGlyfPoints) return false;
        if (size - at < 2) return false;
        const size_t instructions = static_cast<uint16_t>(I16(p + at));
        at += 2;
        if (instructions > size - at) return false;
        at += instructions;

        flags_.resize(points);
        for (uint32_t i = 0; i < points;) {
            if (at >= size) return false;
            const uint8_t flag = p[at++];
            flags_[i++] = flag;
            if ((flag & kRepeat) != 0) {
                if (at >= size) return false;
                uint32_t repeat = p[at++];
                while (repeat-- > 0 && i < points) flags_[i++] = flag;
            }
        }
        xs_.resize(points);
        ys_.resize(points);
        for (int axis = 0; axis < 2; ++axis) {
            const uint8_t shortBit = axis == 0 ? kXShort : kYShort, sameBit = axis == 0 ? kXSame : kYSame;
            std::vector<int32_t>& values = axis == 0 ? xs_ : ys_;
            int32_t value = 0;
            for (uint32_t i = 0; i < points; i++) {
                const uint8_t flag = flags_[i];
                if ((flag & shortBit) != 0) {
                    if (at >= size) return false;
                    value += (flag & sameBit) != 0 ? p[at] : -static_cast<int32_t>(p[at]);
                    at += 1;
                } else if ((flag & sameBit) == 0) {
                    if (size - at < 2) return false;
                    value += I16(p + at);
                    at += 2;
                }
                values[i] = value;
            }
        }
        uint32_t first = 0;
        for (uint32_t c = 0; c < contours; c++) {
            const uint32_t last = endPoints_[c];
            Contour(first, last, m, out);
            first = last + 1;
        }
        return true;
    }

    Raster::Point Point(uint32_t i, const Raster::Matrix& m) const {
        Raster::Point p;
        MapPoint(m, xs_[i], ys_[i], p.x, p.y);
        return p;
    }

    static Raster::Point Mid(Raster::Point a, Raster::Point b) { return {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f}; }

    // One contour of quadratics: consecutive off-curve points have an on-curve point between them.
    void Contour(uint32_t first, uint32_t last, const Raster::Matrix& m, Raster::Path& out) const {
        const uint32_t n = last - first + 1;
        if (n < 2) return;
        // Start on an on-curve point: the first, else the last, else the middle of the two.
        Raster::Point start;
        uint32_t begin = 0, count = n;
        const bool firstOn = (flags_[first] & kOnCurve) != 0;
        const bool lastOn = (flags_[last] & kOnCurve) != 0;
        if (firstOn) {
            start = Point(first, m);
            begin = 1, count = n - 1;
        } else if (lastOn) {
            start = Point(last, m);
            begin = 0, count = n - 1;
        } else {
            start = Mid(Point(first, m), Point(last, m));
        }
        out.MoveTo(start.x, start.y);
        bool pending = false;
        Raster::Point control;
        for (uint32_t k = 0; k < count; k++) {
            const uint32_t i = first + (begin + k) % n;
            const Raster::Point pt = Point(i, m);
            if ((flags_[i] & kOnCurve) != 0) {
                if (pending)
                    out.QuadTo(control.x, control.y, pt.x, pt.y);
                else
                    out.LineTo(pt.x, pt.y);
                pending = false;
            } else {
                if (pending) {
                    const Raster::Point mid = Mid(control, pt);
                    out.QuadTo(control.x, control.y, mid.x, mid.y);
                }
                control = pt;
                pending = true;
            }
        }
        if (pending) out.QuadTo(control.x, control.y, start.x, start.y);
        out.Close();
    }

    bool Composite(const uint8_t* p, size_t size, const Raster::Matrix& m, int depth, Raster::Path& out) {
        if (depth >= kMaxGlyfDepth) return false;
        size_t at = 10;
        for (;;) {
            if (++budget_.components > kMaxGlyfComponents) return false;
            if (size - at < 4) return false;
            const uint16_t flags = static_cast<uint16_t>(I16(p + at));
            const uint16_t component = static_cast<uint16_t>(I16(p + at + 2));
            at += 4;
            double dx = 0, dy = 0;
            if ((flags & kArgsAreWords) != 0) {
                if (size - at < 4) return false;
                dx = I16(p + at);
                dy = I16(p + at + 2);
                at += 4;
            } else {
                if (size - at < 2) return false;
                dx = static_cast<int8_t>(p[at]);
                dy = static_cast<int8_t>(p[at + 1]);
                at += 2;
            }
            double a = 1, b = 0, c = 0, d = 1;
            if ((flags & kHaveScale) != 0) {
                if (size - at < 2) return false;
                a = d = I16(p + at) / 16384.0;
                at += 2;
            } else if ((flags & kXyScale) != 0) {
                if (size - at < 4) return false;
                a = I16(p + at) / 16384.0;
                d = I16(p + at + 2) / 16384.0;
                at += 4;
            } else if ((flags & kTwoByTwo) != 0) {
                if (size - at < 8) return false;
                a = I16(p + at) / 16384.0;
                b = I16(p + at + 2) / 16384.0;
                c = I16(p + at + 4) / 16384.0;
                d = I16(p + at + 6) / 16384.0;
                at += 8;
            }
            // Point-matched components (anchor points instead of offsets) are skipped.
            if ((flags & kArgsAreXy) != 0) {
                const Raster::Matrix local{a, b, c, d, dx, dy};
                // The component's own point arrays are separate: decode it with a fresh reader.
                GlyfReader child(font_, budget_);
                if (!child.Emit(component, local.Then(m), depth + 1, out)) return false;
            }
            if ((flags & kMoreComponents) == 0) break;
        }
        return true;
    }

    const GlyfOutlines& font_;
    GlyfBudget& budget_;
    std::vector<uint16_t> endPoints_;
    std::vector<uint8_t> flags_;
    std::vector<int32_t> xs_, ys_;
};

// ---- Charstrings (Type 2 and Type 1) ----------------------------------------------------------

constexpr int kMaxStack = 48;

// The path a charstring draws: relative moves, lines and curves from the current point.
class Pen {
public:
    Pen(Raster::Path& path, const Raster::Matrix& m) : path_(path), m_(m) {}

    void MoveTo(double x, double y) {
        Close();
        x_ = x, y_ = y;
        float px, py;
        MapPoint(m_, x_ + dx_, y_ + dy_, px, py);
        path_.MoveTo(px, py);
        open_ = true;
    }
    void RMoveTo(double dx, double dy) { MoveTo(x_ + dx, y_ + dy); }
    void RLineTo(double dx, double dy) {
        EnsureOpen();
        x_ += dx, y_ += dy;
        float px, py;
        MapPoint(m_, x_ + dx_, y_ + dy_, px, py);
        path_.LineTo(px, py);
    }
    void RCurveTo(double dx1, double dy1, double dx2, double dy2, double dx3, double dy3) {
        EnsureOpen();
        const double x1 = x_ + dx1, y1 = y_ + dy1, x2 = x1 + dx2, y2 = y1 + dy2;
        x_ = x2 + dx3, y_ = y2 + dy3;
        float a, b, c, d, e, f;
        MapPoint(m_, x1 + dx_, y1 + dy_, a, b);
        MapPoint(m_, x2 + dx_, y2 + dy_, c, d);
        MapPoint(m_, x_ + dx_, y_ + dy_, e, f);
        path_.CubicTo(a, b, c, d, e, f);
    }
    void Close() {
        if (open_) path_.Close();
        open_ = false;
    }
    // Where later glyph parts are drawn (seac's accent): an offset of the origin, the current
    // point reset.
    void Offset(double dx, double dy) {
        Close();
        dx_ = dx, dy_ = dy;
        x_ = y_ = 0;
    }
    double X() const { return x_; }
    double Y() const { return y_; }
    void SetPoint(double x, double y) { x_ = x, y_ = y; }

private:
    void EnsureOpen() {
        if (!open_) MoveTo(x_, y_);
    }

    Raster::Path& path_;
    const Raster::Matrix& m_;
    double x_ = 0, y_ = 0;
    double dx_ = 0, dy_ = 0;
    bool open_ = false;
};

// A number operand of a charstring at `at` (b0 >= 32, or 28 in Type 2, or 255 as a 32-bit
// integer in Type 1 and 16.16 fixed in Type 2).
bool ReadNumber(const uint8_t* bytes, size_t size, size_t& at, bool type2, double& value) {
    const uint8_t b0 = bytes[at];
    if (b0 == 28) {
        if (size - at < 3) return false;
        value = static_cast<int16_t>(static_cast<uint16_t>(bytes[at + 1] << 8 | bytes[at + 2]));
        at += 3;
    } else if (b0 <= 246) {
        value = b0 - 139;
        at += 1;
    } else if (b0 <= 254) {
        if (size - at < 2) return false;
        value = b0 <= 250 ? (b0 - 247) * 256 + bytes[at + 1] + 108 : -(b0 - 251) * 256 - bytes[at + 1] - 108;
        at += 2;
    } else {
        if (size - at < 5) return false;
        const int32_t whole = static_cast<int32_t>(static_cast<uint32_t>(bytes[at + 1]) << 24 |
                                                   static_cast<uint32_t>(bytes[at + 2]) << 16 |
                                                   static_cast<uint32_t>(bytes[at + 3]) << 8 | bytes[at + 4]);
        value = type2 ? whole / 65536.0 : whole;
        at += 5;
    }
    return true;
}

// The bias of a Type 2 subroutine INDEX.
int32_t Bias(size_t count) {
    return count < 1240 ? 107 : count < 33900 ? 1131 : 32768;
}

class Type2 {
public:
    Type2(const CffOutlines& font, uint32_t glyph, Pen& pen) : font_(font), pen_(pen) {
        const uint8_t fd = glyph < font.fdOfGlyph.size() ? font.fdOfGlyph[glyph] : 0;
        local_ = fd < font.fonts.size() ? &font.fonts[fd].localSubrs : nullptr;
    }

    // Runs a charstring (`depth` 0) or a subroutine of it.
    bool Run(std::string_view code, int depth) {
        if (depth > kMaxSubrDepth) return false;
        const uint8_t* const bytes = reinterpret_cast<const uint8_t*>(code.data());
        const size_t size = code.size();
        size_t at = 0;
        while (at < size) {
            const uint8_t b0 = bytes[at];
            if (b0 == 28 || b0 >= 32) {
                double value = 0;
                if (!ReadNumber(bytes, size, at, true, value)) return false;
                if (count_ >= kMaxStack) return false;
                stack_[count_++] = value;
                continue;
            }
            if (++ops_ > kMaxCharstringOps) return false;
            at++;
            switch (b0) {
            case 1:   // hstem
            case 3:   // vstem
            case 18:  // hstemhm
            case 23:  // vstemhm
                TakeWidth((count_ & 1) != 0);
                stems_ += static_cast<uint32_t>(count_) / 2;
                count_ = 0;
                break;
            case 19:    // hintmask
            case 20: {  // cntrmask
                TakeWidth((count_ & 1) != 0);
                stems_ += static_cast<uint32_t>(count_) / 2;
                count_ = 0;
                const size_t maskBytes = (stems_ + 7) / 8;
                if (maskBytes > size - at) return false;
                at += maskBytes;
                break;
            }
            case 21:  // rmoveto
                TakeWidth(count_ > 2);
                if (count_ < 2) return false;
                pen_.RMoveTo(stack_[0], stack_[1]);
                count_ = 0;
                break;
            case 22:  // hmoveto
                TakeWidth(count_ > 1);
                if (count_ < 1) return false;
                pen_.RMoveTo(stack_[0], 0);
                count_ = 0;
                break;
            case 4:  // vmoveto
                TakeWidth(count_ > 1);
                if (count_ < 1) return false;
                pen_.RMoveTo(0, stack_[0]);
                count_ = 0;
                break;
            case 5:  // rlineto
                if (count_ < 2 || (count_ & 1) != 0) return false;
                for (int i = 0; i < count_; i += 2) pen_.RLineTo(stack_[i], stack_[i + 1]);
                count_ = 0;
                break;
            case 6:  // hlineto
            case 7:  // vlineto
                if (count_ < 1) return false;
                for (int i = 0; i < count_; i++) {
                    if (((i & 1) == 0) == (b0 == 6))
                        pen_.RLineTo(stack_[i], 0);
                    else
                        pen_.RLineTo(0, stack_[i]);
                }
                count_ = 0;
                break;
            case 8:  // rrcurveto
                if (count_ < 6 || count_ % 6 != 0) return false;
                for (int i = 0; i < count_; i += 6) Curve(i);
                count_ = 0;
                break;
            case 24:  // rcurveline
                if (count_ < 8 || (count_ - 2) % 6 != 0) return false;
                for (int i = 0; i < count_ - 2; i += 6) Curve(i);
                pen_.RLineTo(stack_[count_ - 2], stack_[count_ - 1]);
                count_ = 0;
                break;
            case 25:  // rlinecurve
                if (count_ < 8 || (count_ - 6) % 2 != 0) return false;
                for (int i = 0; i < count_ - 6; i += 2) pen_.RLineTo(stack_[i], stack_[i + 1]);
                Curve(count_ - 6);
                count_ = 0;
                break;
            case 26: {  // vvcurveto: dx1? {dya dxb dyb dyc}+
                if (count_ < 4 || count_ % 4 > 1) return false;
                int i = 0;
                double dx1 = 0;
                if (count_ % 4 == 1) dx1 = stack_[i++];
                for (; i < count_; i += 4) {
                    pen_.RCurveTo(dx1, stack_[i], stack_[i + 1], stack_[i + 2], 0, stack_[i + 3]);
                    dx1 = 0;
                }
                count_ = 0;
                break;
            }
            case 27: {  // hhcurveto: dy1? {dxa dxb dyb dxc}+
                if (count_ < 4 || count_ % 4 > 1) return false;
                int i = 0;
                double dy1 = 0;
                if (count_ % 4 == 1) dy1 = stack_[i++];
                for (; i < count_; i += 4) {
                    pen_.RCurveTo(stack_[i], dy1, stack_[i + 1], stack_[i + 2], stack_[i + 3], 0);
                    dy1 = 0;
                }
                count_ = 0;
                break;
            }
            case 30:    // vhcurveto
            case 31: {  // hvcurveto: alternating groups of 4, the last with an optional 5th
                if (count_ < 4 || count_ % 4 > 1) return false;
                bool horizontal = b0 == 31;
                for (int i = 0; i + 4 <= count_; i += 4) {
                    const double last = (i + 5 == count_) ? stack_[i + 4] : 0;
                    if (horizontal)
                        pen_.RCurveTo(stack_[i], 0, stack_[i + 1], stack_[i + 2], last, stack_[i + 3]);
                    else
                        pen_.RCurveTo(0, stack_[i], stack_[i + 1], stack_[i + 2], stack_[i + 3], last);
                    horizontal = !horizontal;
                }
                count_ = 0;
                break;
            }
            case 10:    // callsubr
            case 29: {  // callgsubr
                if (count_ < 1) return false;
                const std::vector<std::string_view>* subrs = b0 == 10 ? local_ : &font_.globalSubrs;
                if (!subrs) return false;
                const double index = stack_[--count_] + Bias(subrs->size());
                if (!(index >= 0 && index < double(subrs->size()))) return false;
                if (!Run((*subrs)[static_cast<size_t>(index)], depth + 1)) return false;
                if (ended_) return true;
                break;
            }
            case 11:  // return
                return true;
            case 14:  // endchar; with 4 operands (after the width) the seac form
                TakeWidth(count_ == 1 || count_ == 5);
                if (count_ == 4) {
                    count_ = 0;
                    if (!Seac(stack_[0], stack_[1], stack_[2], stack_[3])) return false;
                } else if (count_ != 0) {
                    return false;
                }
                pen_.Close();
                ended_ = true;
                return true;
            case 12: {
                if (at >= size) return false;
                if (!Escape(bytes[at++])) return false;
                break;
            }
            default:  // reserved
                return false;
            }
        }
        return depth > 0;  // a subroutine may end without return; a glyph must end with endchar
    }

    bool Ended() const { return ended_; }

private:
    // At the first stack-clearing operator, an extra leading operand is the width.
    void TakeWidth(bool present) {
        if (widthChecked_) return;
        widthChecked_ = true;
        if (present && count_ > 0) {
            for (int i = 1; i < count_; i++) stack_[i - 1] = stack_[i];
            count_--;
        }
    }

    void Curve(int i) {
        pen_.RCurveTo(stack_[i], stack_[i + 1], stack_[i + 2], stack_[i + 3], stack_[i + 4], stack_[i + 5]);
    }

    // The glyph of a StandardEncoding code, by its name in the charset.
    int32_t StandardGlyph(double code) const {
        if (!(code >= 0 && code < 256)) return -1;
        const uint16_t name = Tables::kEncodings[static_cast<int>(Tables::Encoding::Standard)][static_cast<int>(code)];
        if (name == Tables::kNoName) return -1;
        return CffGlyphByName(font_, Tables::GlyphName(name));
    }

    bool Seac(double adx, double ady, double bchar, double achar) {
        if (seac_) return false;  // no seac within seac
        const int32_t base = StandardGlyph(bchar), accent = StandardGlyph(achar);
        if (base < 0 || accent < 0) return false;
        for (const int32_t part : {base, accent}) {
            Type2 sub(font_, static_cast<uint32_t>(part), pen_);
            sub.seac_ = true;
            sub.ops_ = ops_;
            if (part == accent) pen_.Offset(adx, ady);
            if (!sub.Run(font_.charStrings[static_cast<size_t>(part)], 0)) return false;
            ops_ = sub.ops_;
        }
        return true;
    }

    bool Escape(uint8_t op) {
        const double* s = stack_;
        switch (op) {
        case 0:  // dotsection (deprecated)
            count_ = 0;
            return true;
        case 35:  // flex: two curves and a flex depth
            if (count_ != 13) return false;
            Curve(0);
            Curve(6);
            break;
        case 34:  // hflex: dx1 dx2 dy2 dx3 dx4 dx5 dx6
            if (count_ != 7) return false;
            pen_.RCurveTo(s[0], 0, s[1], s[2], s[3], 0);
            pen_.RCurveTo(s[4], 0, s[5], -s[2], s[6], 0);
            break;
        case 36:  // hflex1: dx1 dy1 dx2 dy2 dx3 dx4 dx5 dy5 dx6
            if (count_ != 9) return false;
            pen_.RCurveTo(s[0], s[1], s[2], s[3], s[4], 0);
            pen_.RCurveTo(s[5], 0, s[6], s[7], s[8], -(s[1] + s[3] + s[7]));
            break;
        case 37: {  // flex1: dx1 dy1 dx2 dy2 dx3 dy3 dx4 dy4 dx5 dy5 d6
            if (count_ != 11) return false;
            const double dx = s[0] + s[2] + s[4] + s[6] + s[8];
            const double dy = s[1] + s[3] + s[5] + s[7] + s[9];
            pen_.RCurveTo(s[0], s[1], s[2], s[3], s[4], s[5]);
            if ((dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy))
                pen_.RCurveTo(s[6], s[7], s[8], s[9], s[10], -dy);
            else
                pen_.RCurveTo(s[6], s[7], s[8], s[9], -dx, s[10]);
            break;
        }
        // The arithmetic operators fonts use in hint and flex helpers.
        case 9:  // abs
            if (count_ < 1) return false;
            stack_[count_ - 1] = stack_[count_ - 1] < 0 ? -stack_[count_ - 1] : stack_[count_ - 1];
            return true;
        case 10:  // add
        case 11:  // sub
        case 12:  // div
        case 24:  // mul
            if (count_ < 2) return false;
            --count_;
            if (op == 10) stack_[count_ - 1] += stack_[count_];
            else if (op == 11) stack_[count_ - 1] -= stack_[count_];
            else if (op == 24) stack_[count_ - 1] *= stack_[count_];
            else stack_[count_ - 1] = stack_[count_] != 0 ? stack_[count_ - 1] / stack_[count_] : 0;
            return true;
        case 14:  // neg
            if (count_ < 1) return false;
            stack_[count_ - 1] = -stack_[count_ - 1];
            return true;
        case 18:  // drop
            if (count_ < 1) return false;
            --count_;
            return true;
        case 27:  // dup
            if (count_ < 1 || count_ >= kMaxStack) return false;
            stack_[count_] = stack_[count_ - 1];
            ++count_;
            return true;
        case 28:  // exch
            if (count_ < 2) return false;
            std::swap(stack_[count_ - 1], stack_[count_ - 2]);
            return true;
        default:  // storage, conditionals, random: fail the glyph
            return false;
        }
        count_ = 0;
        return true;
    }

    const CffOutlines& font_;
    Pen& pen_;
    const std::vector<std::string_view>* local_ = nullptr;
    double stack_[kMaxStack] = {};
    int count_ = 0;
    uint32_t stems_ = 0;
    uint32_t ops_ = 0;
    bool widthChecked_ = false;
    bool ended_ = false;
    bool seac_ = false;
};

// ---- Type 1 -----------------------------------------------------------------------------------

constexpr uint16_t kEexecKey = 55665, kCharstringKey = 4330;
constexpr size_t kMaxType1Glyphs = 65536;

void Decrypt(std::string_view in, uint16_t key, size_t skip, std::string& out) {
    out.clear();
    uint16_t r = key;
    for (size_t i = 0; i < in.size(); ++i) {
        const uint8_t c = static_cast<uint8_t>(in[i]);
        const uint8_t p = static_cast<uint8_t>(c ^ (r >> 8));
        r = static_cast<uint16_t>((c + r) * 52845u + 22719u);
        if (i >= skip) out.push_back(static_cast<char>(p));
    }
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// The clear text and the encrypted part of a Type 1 program (PFB segments joined).
void SplitType1(std::string_view data, std::string& clear, std::string& encrypted) {
    clear.clear();
    encrypted.clear();
    if (data.size() >= 6 && static_cast<unsigned char>(data[0]) == 0x80) {
        // PFB: segments of type 1 (ASCII) and 2 (binary), each 0x80 type length(4, LE).
        size_t at = 0;
        bool binary = false;
        for (int segments = 0; segments < 64 && at + 6 <= data.size() && static_cast<unsigned char>(data[at]) == 0x80;
             ++segments) {
            const uint8_t type = static_cast<uint8_t>(data[at + 1]);
            const uint32_t length = static_cast<uint32_t>(static_cast<unsigned char>(data[at + 2]) |
                                                          static_cast<unsigned char>(data[at + 3]) << 8 |
                                                          static_cast<unsigned char>(data[at + 4]) << 16 |
                                                          static_cast<uint32_t>(static_cast<unsigned char>(data[at + 5])) << 24);
            at += 6;
            const std::string_view part = data.substr(at, std::min<size_t>(length, data.size() - at));
            at += part.size();
            if (type == 3) break;
            if (type == 2) binary = true;
            (binary ? encrypted : clear).append(part.data(), part.size());
        }
        if (!encrypted.empty()) return;
        // ASCII segments only: the eexec part is hex within them.
        const std::string joined = clear;
        clear.clear();
        SplitType1(joined, clear, encrypted);
        return;
    }
    const size_t eexec = data.find("eexec");
    if (eexec == std::string_view::npos) {
        clear.assign(data.data(), data.size());
        return;
    }
    clear.assign(data.data(), eexec);
    size_t at = eexec + 5;
    while (at < data.size() && (data[at] == ' ' || data[at] == '\r' || data[at] == '\n' || data[at] == '\t')) ++at;
    const std::string_view rest = data.substr(at);
    // Hex when the first four bytes are hex digits (Type 1 Font Format, section 7.2).
    bool hex = rest.size() >= 4;
    for (size_t i = 0; i < 4 && hex; ++i) hex = HexValue(rest[i]) >= 0;
    if (!hex) {
        encrypted.assign(rest.data(), rest.size());
        return;
    }
    int high = -1;
    for (const char c : rest) {
        const int v = HexValue(c);
        if (v < 0) continue;
        if (high < 0) {
            high = v;
        } else {
            encrypted.push_back(static_cast<char>(high << 4 | v));
            high = -1;
        }
    }
}

// A decimal integer token at `at` (after spaces), or -1.
int64_t ReadInt(std::string_view text, size_t& at) {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\r' || text[at] == '\n' || text[at] == '\t')) ++at;
    int64_t v = 0;
    size_t digits = 0;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9' && digits < 10) v = v * 10 + (text[at++] - '0'), ++digits;
    return digits ? v : -1;
}

// Skips the token after a length ("RD", "-|") and one space; the binary data follows.
bool SkipRd(std::string_view text, size_t& at) {
    while (at < text.size() && text[at] == ' ') ++at;
    const size_t start = at;
    while (at < text.size() && text[at] != ' ' && text[at] != '\r' && text[at] != '\n' && at - start < 8) ++at;
    if (at == start || at >= text.size()) return false;
    ++at;  // the single space before the binary data
    return true;
}

class Type1 {
public:
    Type1(const Type1Outlines& font, Pen& pen) : font_(font), pen_(pen) {}

    bool Glyph(std::string_view name, int seacDepth) {
        const auto it = std::lower_bound(font_.charStrings.begin(), font_.charStrings.end(), name,
                                         [](const std::pair<std::string, std::string>& e, std::string_view n) {
                                             return std::string_view(e.first) < n;
                                         });
        if (it == font_.charStrings.end() || it->first != name) return false;
        seacDepth_ = seacDepth;
        return Run(it->second, 0);
    }

private:
    bool Run(std::string_view code, int depth) {
        if (depth > kMaxSubrDepth) return false;
        const uint8_t* const bytes = reinterpret_cast<const uint8_t*>(code.data());
        const size_t size = code.size();
        size_t at = 0;
        while (at < size) {
            const uint8_t b0 = bytes[at];
            if (b0 >= 32) {
                double value = 0;
                if (!ReadNumber(bytes, size, at, false, value)) return false;
                if (count_ >= kMaxStack) return false;
                stack_[count_++] = value;
                continue;
            }
            if (++ops_ > kMaxCharstringOps) return false;
            at++;
            switch (b0) {
            case 1:  // hstem
            case 3:  // vstem
                count_ = 0;
                break;
            case 4:  // vmoveto
                if (count_ < 1) return false;
                Move(0, stack_[count_ - 1]);
                count_ = 0;
                break;
            case 5:  // rlineto
                if (count_ < 2) return false;
                pen_.RLineTo(stack_[count_ - 2], stack_[count_ - 1]);
                count_ = 0;
                break;
            case 6:  // hlineto
                if (count_ < 1) return false;
                pen_.RLineTo(stack_[count_ - 1], 0);
                count_ = 0;
                break;
            case 7:  // vlineto
                if (count_ < 1) return false;
                pen_.RLineTo(0, stack_[count_ - 1]);
                count_ = 0;
                break;
            case 8:  // rrcurveto
                if (count_ < 6) return false;
                pen_.RCurveTo(stack_[count_ - 6], stack_[count_ - 5], stack_[count_ - 4], stack_[count_ - 3],
                              stack_[count_ - 2], stack_[count_ - 1]);
                count_ = 0;
                break;
            case 9:  // closepath
                pen_.Close();
                count_ = 0;
                break;
            case 10: {  // callsubr
                if (count_ < 1) return false;
                const double index = stack_[--count_];
                if (!(index >= 0 && index < double(font_.subrs.size()))) return false;
                if (!Run(font_.subrs[static_cast<size_t>(index)], depth + 1)) return false;
                if (ended_) return true;
                break;
            }
            case 11:  // return
                return true;
            case 13:  // hsbw: sbx wx
                if (count_ < 2) return false;
                pen_.SetPoint(stack_[count_ - 2], 0);
                if (seacDepth_ == 0) sbx_ = stack_[count_ - 2];
                count_ = 0;
                break;
            case 14:  // endchar
                pen_.Close();
                ended_ = true;
                return true;
            case 21:  // rmoveto
                if (count_ < 2) return false;
                Move(stack_[count_ - 2], stack_[count_ - 1]);
                count_ = 0;
                break;
            case 22:  // hmoveto
                if (count_ < 1) return false;
                Move(stack_[count_ - 1], 0);
                count_ = 0;
                break;
            case 30:  // vhcurveto
                if (count_ < 4) return false;
                pen_.RCurveTo(0, stack_[count_ - 4], stack_[count_ - 3], stack_[count_ - 2], stack_[count_ - 1], 0);
                count_ = 0;
                break;
            case 31:  // hvcurveto
                if (count_ < 4) return false;
                pen_.RCurveTo(stack_[count_ - 4], 0, stack_[count_ - 3], stack_[count_ - 2], 0, stack_[count_ - 1]);
                count_ = 0;
                break;
            case 12:
                if (at >= size) return false;
                if (!Escape(bytes[at++])) return false;
                if (ended_) return true;
                break;
            default:
                return false;
            }
        }
        return depth > 0;
    }

    // A move: during flex the point is collected instead (OtherSubrs 1 and 2).
    void Move(double dx, double dy) {
        if (flex_) {
            pen_.SetPoint(pen_.X() + dx, pen_.Y() + dy);
            if (flexCount_ < 7) flexX_[flexCount_] = pen_.X(), flexY_[flexCount_] = pen_.Y();
            ++flexCount_;
            return;
        }
        pen_.RMoveTo(dx, dy);
    }

    bool Escape(uint8_t op) {
        switch (op) {
        case 0:  // dotsection
        case 1:  // vstem3
        case 2:  // hstem3
            count_ = 0;
            return true;
        case 6: {  // seac: asb adx ady bchar achar
            if (count_ < 5 || seacDepth_ > 0) return false;
            const double asb = stack_[count_ - 5], adx = stack_[count_ - 4], ady = stack_[count_ - 3];
            const double bchar = stack_[count_ - 2], achar = stack_[count_ - 1];
            if (!(bchar >= 0 && bchar < 256 && achar >= 0 && achar < 256)) return false;
            const auto& standard = Tables::kEncodings[static_cast<int>(Tables::Encoding::Standard)];
            const uint16_t baseName = standard[static_cast<int>(bchar)], accentName = standard[static_cast<int>(achar)];
            if (baseName == Tables::kNoName || accentName == Tables::kNoName) return false;
            const double sbx = sbx_;  // the left sidebearing of the glyph holding the seac
            count_ = 0;
            Type1 base(font_, pen_);
            base.ops_ = ops_;
            if (!base.Glyph(Tables::GlyphName(baseName), 1)) return false;
            // The accent's origin: adx - asb from the base's (its own hsbw adds its sidebearing).
            pen_.Offset(adx - asb + sbx, ady);
            Type1 accent(font_, pen_);
            accent.ops_ = base.ops_;
            if (!accent.Glyph(Tables::GlyphName(accentName), 1)) return false;
            ops_ = accent.ops_;
            pen_.Close();
            ended_ = true;
            return true;
        }
        case 7:  // sbw: sbx sby wx wy
            if (count_ < 4) return false;
            pen_.SetPoint(stack_[count_ - 4], stack_[count_ - 3]);
            if (seacDepth_ == 0) sbx_ = stack_[count_ - 4];
            count_ = 0;
            return true;
        case 12:  // div
            if (count_ < 2) return false;
            --count_;
            stack_[count_ - 1] = stack_[count_] != 0 ? stack_[count_ - 1] / stack_[count_] : 0;
            return true;
        case 16: {  // callothersubr: args n othersubr#
            if (count_ < 2) return false;
            const int other = static_cast<int>(stack_[count_ - 1]);
            const int n = static_cast<int>(stack_[count_ - 2]);
            count_ -= 2;
            if (n < 0 || n > count_) return false;
            // Arguments to the PostScript stack, the last on top.
            psCount_ = 0;
            for (int i = 0; i < n && psCount_ < kMaxStack; ++i) ps_[psCount_++] = stack_[count_ - n + i];
            count_ -= n;
            if (other == 1) {  // flex start
                flex_ = true;
                flexCount_ = 0;
            } else if (other == 0 && flex_) {  // flex end: fh x y; the end point comes back by pop
                flex_ = false;
                if (flexCount_ >= 7) {
                    // Point 0 is the reference point; 1-3 and 4-6 are the two curves.
                    pen_.SetPoint(flexX_[0], flexY_[0]);
                    for (int c = 0; c < 2; ++c) {
                        const int k = 1 + 3 * c;
                        const double x0 = pen_.X(), y0 = pen_.Y();
                        pen_.RCurveTo(flexX_[k] - x0, flexY_[k] - y0, flexX_[k + 1] - flexX_[k],
                                      flexY_[k + 1] - flexY_[k], flexX_[k + 2] - flexX_[k + 1],
                                      flexY_[k + 2] - flexY_[k + 1]);
                    }
                }
                // pop pop setcurrentpoint: x then y.
                psCount_ = 2;
                ps_[0] = pen_.Y();
                ps_[1] = pen_.X();
            }
            // OtherSubr 2 (flex point) needs nothing: Move collects. OtherSubr 3 (hint
            // replacement) and the others leave their arguments for pop.
            return true;
        }
        case 17:  // pop
            if (psCount_ > 0) {
                if (count_ >= kMaxStack) return false;
                stack_[count_++] = ps_[--psCount_];
            }
            return true;
        case 33:  // setcurrentpoint
            if (count_ < 2) return false;
            pen_.SetPoint(stack_[count_ - 2], stack_[count_ - 1]);
            count_ = 0;
            return true;
        default:
            return false;
        }
    }

    const Type1Outlines& font_;
    Pen& pen_;
    double stack_[kMaxStack] = {};
    int count_ = 0;
    double ps_[kMaxStack] = {};
    int psCount_ = 0;
    bool flex_ = false;
    int flexCount_ = 0;
    double flexX_[7] = {}, flexY_[7] = {};
    double sbx_ = 0;
    int seacDepth_ = 0;
    uint32_t ops_ = 0;
    bool ended_ = false;
};

} // namespace

// ---- Public -----------------------------------------------------------------------------------

bool ReadGlyfOutlines(const Sfnt& sfnt, GlyfOutlines& out) {
    out = GlyfOutlines();
    if (sfnt.glyf.empty() || sfnt.loca.empty() || sfnt.head.size() < 54) return false;
    out.glyf = sfnt.glyf;
    out.loca = sfnt.loca;
    out.longLoca = U16(sfnt.head, 50) == 1;
    const uint32_t upem = U16(sfnt.head, 18);
    out.unitsPerEm = static_cast<uint16_t>(upem >= 16 && upem <= 16384 ? upem : 1000);
    const size_t locaEntries = out.loca.size() / (out.longLoca ? 4 : 2);
    out.glyphCount = locaEntries > 0 ? static_cast<uint32_t>(std::min<size_t>(locaEntries - 1, 65535)) : 0;
    if (sfnt.glyphCount > 0 && sfnt.glyphCount < out.glyphCount) out.glyphCount = sfnt.glyphCount;
    return out.glyphCount > 0;
}

bool GlyfOutline(const GlyfOutlines& font, uint32_t glyph, const Raster::Matrix& m, Raster::Path& out) {
    GlyfBudget budget;
    GlyfReader reader(font, budget);
    return reader.Emit(glyph, m, 0, out) && !out.Overflowed();
}

bool CffGlyphOutline(const CffOutlines& font, uint32_t glyph, const Raster::Matrix& m, Raster::Path& out) {
    if (glyph >= font.charStrings.size()) return false;
    Pen pen(out, m);
    Type2 interpreter(font, glyph, pen);
    return interpreter.Run(font.charStrings[glyph], 0) && interpreter.Ended() && !out.Overflowed();
}

bool ReadType1Outlines(std::string_view data, Type1Outlines& out) {
    out = Type1Outlines();
    std::string clear, encrypted, priv;
    SplitType1(data, clear, encrypted);
    // /FontMatrix [a b c d e f] in the clear text.
    const size_t fm = clear.find("/FontMatrix");
    if (fm != std::string::npos) {
        Syntax::Cursor c;
        c.data = clear;
        c.pos = fm + 11;
        Syntax::SkipSpace(c);
        if (!c.AtEnd() && c.Peek() == '[') {
            c.pos++;
            double values[6];
            int n = 0;
            while (n < 6 && !c.AtEnd()) {
                Syntax::SkipSpace(c);
                const std::string_view token = Syntax::ReadToken(c);
                PdfValue number;
                if (token.empty() || !Syntax::ParseNumber(token, number)) break;
                values[n++] = number.Number();
            }
            bool finite = n == 6;
            for (int i = 0; i < n && finite; ++i) finite = values[i] > -1e6 && values[i] < 1e6;
            if (finite && values[0] * values[3] - values[1] * values[2] != 0)
                for (int i = 0; i < 6; ++i) out.matrix[i] = values[i];
        }
    }
    Decrypt(encrypted, kEexecKey, 4, priv);
    const std::string_view text(priv);
    // /lenIV (default 4; -1 means the charstrings are not encrypted).
    int64_t lenIV = 4;
    const size_t lenAt = text.find("/lenIV");
    if (lenAt != std::string_view::npos) {
        size_t at = lenAt + 6;
        while (at < text.size() && text[at] == ' ') ++at;
        if (at < text.size() && text[at] == '-') {
            lenIV = -1;
        } else {
            lenIV = ReadInt(text, at);
            if (lenIV < 0 || lenIV > 64) lenIV = 4;
        }
    }
    const auto decode = [&](std::string_view bytes, std::string& charstring) {
        if (lenIV < 0)
            charstring.assign(bytes.data(), bytes.size());
        else
            Decrypt(bytes, kCharstringKey, static_cast<size_t>(lenIV), charstring);
    };
    // /Subrs N array  dup i n RD <n bytes> NP ...
    const size_t subrsAt = text.find("/Subrs");
    const size_t charsAt = text.find("/CharStrings");
    if (subrsAt != std::string_view::npos && (charsAt == std::string_view::npos || subrsAt < charsAt)) {
        size_t at = subrsAt + 6;
        const int64_t count = ReadInt(text, at);
        if (count > 0 && count <= int64_t(kMaxType1Glyphs)) {
            out.subrs.resize(static_cast<size_t>(count));
            for (int64_t i = 0; i < count; ++i) {
                const size_t dup = text.find("dup", at);
                if (dup == std::string_view::npos || (charsAt != std::string_view::npos && dup > charsAt)) break;
                at = dup + 3;
                const int64_t index = ReadInt(text, at);
                const int64_t length = ReadInt(text, at);
                if (index < 0 || length < 0 || !SkipRd(text, at) || size_t(length) > text.size() - at) break;
                if (index < count) decode(text.substr(at, static_cast<size_t>(length)), out.subrs[static_cast<size_t>(index)]);
                at += static_cast<size_t>(length);
            }
        }
    }
    // /CharStrings N dict dup begin  /name n RD <n bytes> ND ... end
    if (charsAt == std::string_view::npos) return false;
    size_t at = charsAt + 12;
    const size_t begin = text.find("begin", at);
    if (begin == std::string_view::npos) return false;
    at = begin + 5;
    while (at < text.size() && out.charStrings.size() < kMaxType1Glyphs) {
        while (at < text.size() && text[at] != '/' && text.compare(at, 3, "end") != 0) ++at;
        if (at >= text.size() || text[at] != '/') break;
        const size_t nameStart = ++at;
        while (at < text.size() && text[at] != ' ' && text[at] != '\r' && text[at] != '\n' && text[at] != '\t' &&
               text[at] != '/')
            ++at;
        const std::string_view name = text.substr(nameStart, at - nameStart);
        const int64_t length = ReadInt(text, at);
        if (name.empty() || length < 0 || !SkipRd(text, at) || size_t(length) > text.size() - at) break;
        std::string charstring;
        decode(text.substr(at, static_cast<size_t>(length)), charstring);
        out.charStrings.emplace_back(std::string(name), std::move(charstring));
        at += static_cast<size_t>(length);
    }
    std::stable_sort(out.charStrings.begin(), out.charStrings.end(),
                     [](const std::pair<std::string, std::string>& a, const std::pair<std::string, std::string>& b) {
                         return a.first < b.first;
                     });
    return !out.charStrings.empty();
}

bool Type1GlyphOutline(const Type1Outlines& font, std::string_view name, const Raster::Matrix& m, Raster::Path& out) {
    Pen pen(out, m);
    Type1 interpreter(font, pen);
    return interpreter.Glyph(name, 0) && !out.Overflowed();
}

} // namespace RayoPdf::Text::Program
