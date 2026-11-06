// SPDX-License-Identifier: Apache-2.0
#include "layout/interpreter.h"

#include "common/utf8.h"
#include "pdf/content_lexer.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_set>

namespace RayoPdf::Layout {
namespace {

// An affine matrix in PDF's row-vector convention: [x y 1] × M (ISO 32000-1 §8.3.4).
struct Matrix {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};

// `m` then `n`.
Matrix Multiply(const Matrix& m, const Matrix& n) {
    Matrix r;
    r.a = m.a * n.a + m.b * n.c;
    r.b = m.a * n.b + m.b * n.d;
    r.c = m.c * n.a + m.d * n.c;
    r.d = m.c * n.b + m.d * n.d;
    r.e = m.e * n.a + m.f * n.c + n.e;
    r.f = m.e * n.b + m.f * n.d + n.f;
    return r;
}

bool Finite(const Matrix& m) {
    return std::isfinite(m.a) && std::isfinite(m.b) && std::isfinite(m.c) && std::isfinite(m.d) &&
           std::isfinite(m.e) && std::isfinite(m.f);
}

struct Box {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool Empty() const { return !(x1 > x0) || !(y1 > y0); }
};

Box Intersect(const Box& a, const Box& b) {
    Box r;
    r.x0 = std::fmax(a.x0, b.x0);
    r.y0 = std::fmax(a.y0, b.y0);
    r.x1 = std::fmin(a.x1, b.x1);
    r.y1 = std::fmin(a.y1, b.y1);
    return r;
}

// The device box of the unit square under `m` (images, §8.9.4).
Box UnitSquare(const Matrix& m) {
    const double xs[4] = {m.e, m.a + m.e, m.c + m.e, m.a + m.c + m.e};
    const double ys[4] = {m.f, m.b + m.f, m.d + m.f, m.b + m.d + m.f};
    Box r{xs[0], ys[0], xs[0], ys[0]};
    for (int i = 1; i < 4; ++i) {
        r.x0 = std::fmin(r.x0, xs[i]);
        r.x1 = std::fmax(r.x1, xs[i]);
        r.y0 = std::fmin(r.y0, ys[i]);
        r.y1 = std::fmax(r.y1, ys[i]);
    }
    return r;
}

// (x, y) on the upright page into the frame of baseline direction `dir`.
inline void ToFrame(int dir, double x, double y, double& u, double& v) {
    switch (dir) {
    case 1: u = y, v = -x; break;
    case 2: u = -x, v = -y; break;
    case 3: u = -y, v = x; break;
    default: u = x, v = y; break;
    }
}

struct GState {
    Matrix ctm;
    Box clip;
    double charSpace = 0;
    double wordSpace = 0;
    double hScale = 1;
    double leading = 0;
    double fontSize = 0;
    double rise = 0;
    double lineWidth = 1;
    int render = 0;
    const Text::Font* font = nullptr;
    uint16_t fontId = 0;
    bool transparent = false;  // fill alpha 0 (/ca 0)
};

enum class Op : uint8_t {
    Unknown, q, Q, cm, BT, ET, Tc, Tw, Tz, TL, Tf, Tr, Ts, Td, TD, Tm, TStar, Tj, TJ, Quote, DoubleQuote,
    Do, BI, re, m, l, c, v, y, h, S, s, f, F, fStar, B, BStar, b, bStar, n, W, WStar, gs, w, BDC, BMC, EMC,
};

Op OpOf(std::string_view op) {
    switch (op.size()) {
    case 1:
        switch (op[0]) {
        case 'q': return Op::q;
        case 'Q': return Op::Q;
        case '\'': return Op::Quote;
        case '"': return Op::DoubleQuote;
        case 'm': return Op::m;
        case 'l': return Op::l;
        case 'c': return Op::c;
        case 'v': return Op::v;
        case 'y': return Op::y;
        case 'h': return Op::h;
        case 'S': return Op::S;
        case 's': return Op::s;
        case 'f': return Op::f;
        case 'F': return Op::F;
        case 'B': return Op::B;
        case 'b': return Op::b;
        case 'n': return Op::n;
        case 'W': return Op::W;
        case 'w': return Op::w;
        default: return Op::Unknown;
        }
    case 2:
        if (op == "cm") return Op::cm;
        if (op == "BT") return Op::BT;
        if (op == "ET") return Op::ET;
        if (op[0] == 'T') {
            switch (op[1]) {
            case 'c': return Op::Tc;
            case 'w': return Op::Tw;
            case 'z': return Op::Tz;
            case 'L': return Op::TL;
            case 'f': return Op::Tf;
            case 'r': return Op::Tr;
            case 's': return Op::Ts;
            case 'd': return Op::Td;
            case 'D': return Op::TD;
            case 'm': return Op::Tm;
            case '*': return Op::TStar;
            case 'j': return Op::Tj;
            case 'J': return Op::TJ;
            default: return Op::Unknown;
            }
        }
        if (op == "Do") return Op::Do;
        if (op == "BI") return Op::BI;
        if (op == "re") return Op::re;
        if (op == "f*") return Op::fStar;
        if (op == "B*") return Op::BStar;
        if (op == "b*") return Op::bStar;
        if (op == "W*") return Op::WStar;
        if (op == "gs") return Op::gs;
        return Op::Unknown;
    case 3:
        if (op == "BDC") return Op::BDC;
        if (op == "BMC") return Op::BMC;
        if (op == "EMC") return Op::EMC;
        return Op::Unknown;
    default:
        return Op::Unknown;
    }
}

// PDFDocEncoding bytes 0x18-0x1F and 0x80-0xA0 (ISO 32000-1 Annex D.2); the rest are Latin-1.
const uint16_t kPdfDocLow[8] = {0x02D8, 0x02C7, 0x02C6, 0x02D9, 0x02DD, 0x02DB, 0x02DA, 0x02DC};
const uint16_t kPdfDocHigh[33] = {
    0x2022, 0x2020, 0x2021, 0x2026, 0x2014, 0x2013, 0x0192, 0x2044, 0x2039, 0x203A, 0x2212,
    0x2030, 0x201E, 0x201C, 0x201D, 0x2018, 0x2019, 0x201A, 0x2122, 0xFB01, 0xFB02, 0x0141,
    0x0152, 0x0160, 0x0178, 0x017D, 0x0131, 0x0142, 0x0153, 0x0161, 0x017E, 0xFFFD, 0x20AC,
};

// One marked-content sequence of the stack.
struct MarkedContent {
    uint32_t firstGlyph = 0;
    uint32_t actualText = 0;     // offset in the page text
    uint32_t actualLength = 0;
    bool hasActualText = false;
    bool artifact = false;
    int32_t previousMcid = -1;
};

class Interpreter {
public:
    Interpreter(const Document& document, Text::FontCache& fonts, PageData& page, PageStats& stats)
        : doc_(document), fonts_(fonts), page_(page), stats_(stats) {}

    void Run(int index, bool withLinks);

private:
    void RunStream(std::string_view content, const PdfValue& resources, int depth);
    bool Execute(Op op, const std::vector<PdfValue>& operands, const PdfValue& resources, int depth,
                 ContentLexer& lexer);
    bool ShowString(std::string_view bytes);
    bool ShowArray(const PdfValue& array);
    void EmitGlyph(const Text::Code& code, const Matrix& trm, double advance);
    bool SetFont(const PdfValue& resources, std::string_view name, double size);
    void SetFontEntry(const PdfValue& entry, double size);
    void ApplyExtGState(const PdfValue& resources, std::string_view name);
    void DoXObject(const PdfValue& resources, std::string_view name, int depth);
    void BeginMarked(std::string_view tag, const PdfValue* properties, const PdfValue& resources);
    void EndMarked();
    void Paint(bool fill, bool stroke);
    void PathPoint(double x, double y, bool curve);
    void ReadLinks(int index);
    void NextLine(double tx, double ty) {
        Matrix t;
        t.e = tx;
        t.f = ty;
        lineMatrix_ = Multiply(t, lineMatrix_);
        textMatrix_ = lineMatrix_;
    }

    const Document& doc_;
    Text::FontCache& fonts_;
    PageData& page_;
    PageStats& stats_;

    GState gs_;
    std::vector<GState> stack_;
    Matrix textMatrix_;
    Matrix lineMatrix_;
    Box pageBox_;
    uint32_t operators_ = 0;
    bool budgetHit_ = false;
    Matrix pageMatrix_;  // default user space to the upright page

    std::vector<const Text::Font*> fontIds_;
    std::vector<MarkedContent> marked_;
    uint32_t markedOverflow_ = 0;
    int artifactDepth_ = 0;
    int32_t mcid_ = -1;

    // Current path, in device space.
    Box pathBox_;
    bool pathEmpty_ = true;
    bool pathCurved_ = false;
    uint16_t pathSegments_ = 0;
    uint16_t pathSubpaths_ = 0;
    bool pathIsRect_ = false;
    double startX_ = 0, startY_ = 0;  // current subpath start (user space)
    double curX_ = 0, curY_ = 0;      // current point (user space)
    int pendingClip_ = 0;             // W or W* seen: the path clips at the next painting operator

    std::vector<uint32_t> formStack_;          // object numbers of the forms being run
    std::vector<std::string> formBuffers_;     // decoded form content, one per depth
};

void Interpreter::Run(int index, bool withLinks) {
    const PdfRect crop = doc_.CropBox(index);
    const int rotate = doc_.Rotate(index);
    const double w = crop.Width(), h = crop.Height();
    // Device space is the upright page: the CropBox origin moved to (0, 0), then turned clockwise
    // by /Rotate as a viewer shows it (§7.7.3.3).
    Matrix base;
    base.e = -crop.x0;
    base.f = -crop.y0;
    Matrix turn;
    switch (rotate) {
    case 90: turn = Matrix{0, -1, 1, 0, 0, w}; break;
    case 180: turn = Matrix{-1, 0, 0, -1, w, h}; break;
    case 270: turn = Matrix{0, 1, -1, 0, h, 0}; break;
    default: break;
    }
    base = Multiply(base, turn);
    pageMatrix_ = base;
    const bool sideways = rotate == 90 || rotate == 270;
    page_.width = static_cast<float>(sideways ? h : w);
    page_.height = static_cast<float>(sideways ? w : h);
    pageBox_ = Box{0, 0, page_.width, page_.height};
    gs_ = GState();
    gs_.ctm = base;
    gs_.clip = pageBox_;

    std::string content;
    const StreamStatus status = doc_.PageContent(index, content);
    if (status == StreamStatus::Corrupt || status == StreamStatus::Salvaged || status == StreamStatus::TooLarge)
        page_.contentErrors++;
    if (!content.empty()) RunStream(content, doc_.Resources(index), 0);
    // Marked content left open at the end still applies its ActualText.
    while (!marked_.empty()) EndMarked();
    if (withLinks) ReadLinks(index);
    page_.limitHit = budgetHit_;
}

void Interpreter::RunStream(std::string_view content, const PdfValue& resources, int depth) {
    ContentLexer lexer(content);
    std::string_view op;
    std::vector<PdfValue> operands;
    const size_t stackBase = stack_.size();
    bool error = false;
    while (lexer.Next(op, operands)) {
        if (++operators_ > kMaxOperatorsPerPage) {
            budgetHit_ = true;
            break;
        }
        const Op code = OpOf(op);
        if (code == Op::Unknown) continue;
        if (!Execute(code, operands, resources, depth, lexer)) error = true;
        if (budgetHit_) break;
    }
    // A stream restores the state stack it started with (forms run inside q/Q, §8.10.1).
    while (stack_.size() > stackBase) {
        gs_ = stack_.back();
        stack_.pop_back();
    }
    if (error) page_.contentErrors++;
}

// Numbers of the operands; false when there are fewer than `n` or one is not a number.
bool Numbers(const std::vector<PdfValue>& operands, size_t n, double* out) {
    if (operands.size() < n) return false;
    const size_t base = operands.size() - n;
    for (size_t i = 0; i < n; ++i) {
        const PdfValue& v = operands[base + i];
        if (!v.IsNumber()) return false;
        out[i] = v.Number();
        if (!std::isfinite(out[i])) return false;
    }
    return true;
}

bool Interpreter::Execute(Op op, const std::vector<PdfValue>& operands, const PdfValue& resources, int depth,
                          ContentLexer& lexer) {
    double n[6];
    switch (op) {
    case Op::q:
        if (stack_.size() >= static_cast<size_t>(kMaxStateDepth)) return false;
        stack_.push_back(gs_);
        return true;
    case Op::Q:
        if (stack_.empty()) return true;
        gs_ = stack_.back();
        stack_.pop_back();
        return true;
    case Op::cm: {
        if (!Numbers(operands, 6, n)) return false;
        const Matrix m = Multiply(Matrix{n[0], n[1], n[2], n[3], n[4], n[5]}, gs_.ctm);
        if (!Finite(m)) return false;
        gs_.ctm = m;
        return true;
    }
    case Op::BT:
        textMatrix_ = lineMatrix_ = Matrix();
        return true;
    case Op::ET:
        return true;
    case Op::Tc:
        if (!Numbers(operands, 1, n)) return false;
        gs_.charSpace = n[0];
        return true;
    case Op::Tw:
        if (!Numbers(operands, 1, n)) return false;
        gs_.wordSpace = n[0];
        return true;
    case Op::Tz:
        if (!Numbers(operands, 1, n)) return false;
        gs_.hScale = n[0] / 100;
        return true;
    case Op::TL:
        if (!Numbers(operands, 1, n)) return false;
        gs_.leading = n[0];
        return true;
    case Op::Tf:
        if (operands.size() < 2 || !operands[operands.size() - 2].IsName() || !operands.back().IsNumber())
            return false;
        return SetFont(resources, operands[operands.size() - 2].Name(), operands.back().Number());
    case Op::Tr:
        if (!Numbers(operands, 1, n)) return false;
        // Modes are 0-7; anything else (out of int range included) is malformed.
        if (!(n[0] >= 0 && n[0] <= 7)) return false;
        gs_.render = static_cast<int>(n[0]);
        return true;
    case Op::Ts:
        if (!Numbers(operands, 1, n)) return false;
        gs_.rise = n[0];
        return true;
    case Op::Td:
        if (!Numbers(operands, 2, n)) return false;
        NextLine(n[0], n[1]);
        return true;
    case Op::TD:
        if (!Numbers(operands, 2, n)) return false;
        gs_.leading = -n[1];
        NextLine(n[0], n[1]);
        return true;
    case Op::Tm: {
        if (!Numbers(operands, 6, n)) return false;
        const Matrix m{n[0], n[1], n[2], n[3], n[4], n[5]};
        textMatrix_ = lineMatrix_ = m;
        return true;
    }
    case Op::TStar:
        NextLine(0, -gs_.leading);
        return true;
    case Op::Tj:
        if (operands.empty() || !operands.back().IsString()) return false;
        return ShowString(operands.back().String());
    case Op::TJ:
        if (operands.empty() || !operands.back().IsArray()) return false;
        return ShowArray(operands.back());
    case Op::Quote:
        NextLine(0, -gs_.leading);
        if (operands.empty() || !operands.back().IsString()) return false;
        return ShowString(operands.back().String());
    case Op::DoubleQuote: {
        const size_t count = operands.size();
        if (count < 3 || !operands[count - 1].IsString() || !operands[count - 3].IsNumber() ||
            !operands[count - 2].IsNumber())
            return false;
        gs_.wordSpace = operands[count - 3].Number();
        gs_.charSpace = operands[count - 2].Number();
        NextLine(0, -gs_.leading);
        return ShowString(operands.back().String());
    }
    case Op::Do:
        if (operands.empty() || !operands.back().IsName()) return false;
        DoXObject(resources, operands.back().Name(), depth);
        return true;
    case Op::BI: {
        const Box box = Intersect(UnitSquare(gs_.ctm), pageBox_);
        if (!box.Empty() && page_.images.size() < kMaxShapesPerPage)
            page_.images.push_back(ImageBox{float(box.x0), float(box.y0), float(box.x1), float(box.y1)});
        (void)lexer;
        return true;
    }
    case Op::re: {
        if (!Numbers(operands, 4, n)) return false;
        const bool wasEmpty = pathEmpty_;
        PathPoint(n[0], n[1], false);
        PathPoint(n[0] + n[2], n[1] + n[3], false);
        PathPoint(n[0] + n[2], n[1], false);
        PathPoint(n[0], n[1] + n[3], false);
        // Four segments of one subpath; an axis-aligned rectangle when the CTM keeps axes.
        pathSegments_ = static_cast<uint16_t>(pathSegments_ + 4 < 60000 ? pathSegments_ + 4 : 60000);
        pathSubpaths_++;
        const bool axisAligned = (std::fabs(gs_.ctm.b) < 1e-9 && std::fabs(gs_.ctm.c) < 1e-9) ||
                                 (std::fabs(gs_.ctm.a) < 1e-9 && std::fabs(gs_.ctm.d) < 1e-9);
        pathIsRect_ = wasEmpty && axisAligned;
        curX_ = startX_ = n[0];
        curY_ = startY_ = n[1];
        return true;
    }
    case Op::m:
        if (!Numbers(operands, 2, n)) return false;
        startX_ = curX_ = n[0];
        startY_ = curY_ = n[1];
        pathSubpaths_++;
        if (!pathEmpty_) pathIsRect_ = false;
        PathPoint(n[0], n[1], false);
        return true;
    case Op::l:
        if (!Numbers(operands, 2, n)) return false;
        if (pathEmpty_) return false;
        curX_ = n[0];
        curY_ = n[1];
        PathPoint(n[0], n[1], false);
        if (pathSegments_ < 60000) pathSegments_++;
        return true;
    case Op::c:
        if (!Numbers(operands, 6, n)) return false;
        if (pathEmpty_) return false;
        PathPoint(n[0], n[1], true);
        PathPoint(n[2], n[3], true);
        PathPoint(n[4], n[5], true);
        curX_ = n[4];
        curY_ = n[5];
        if (pathSegments_ < 60000) pathSegments_++;
        return true;
    case Op::v:
    case Op::y:
        if (!Numbers(operands, 4, n)) return false;
        if (pathEmpty_) return false;
        PathPoint(n[0], n[1], true);
        PathPoint(n[2], n[3], true);
        curX_ = n[2];
        curY_ = n[3];
        if (pathSegments_ < 60000) pathSegments_++;
        return true;
    case Op::h:
        curX_ = startX_;
        curY_ = startY_;
        return true;
    case Op::S: Paint(false, true); return true;
    case Op::s: Paint(false, true); return true;
    case Op::f:
    case Op::F:
    case Op::fStar: Paint(true, false); return true;
    case Op::B:
    case Op::BStar:
    case Op::b:
    case Op::bStar: Paint(true, true); return true;
    case Op::n: Paint(false, false); return true;
    case Op::W:
    case Op::WStar:
        pendingClip_ = 1;
        return true;
    case Op::w:
        if (!Numbers(operands, 1, n)) return false;
        gs_.lineWidth = n[0];
        return true;
    case Op::gs:
        if (operands.empty() || !operands.back().IsName()) return false;
        ApplyExtGState(resources, operands.back().Name());
        return true;
    case Op::BDC:
        if (operands.size() < 2 || !operands[operands.size() - 2].IsName()) {
            BeginMarked({}, nullptr, resources);
            return false;
        }
        BeginMarked(operands[operands.size() - 2].Name(), &operands.back(), resources);
        return true;
    case Op::BMC:
        BeginMarked(operands.empty() ? std::string_view() : operands.back().Name(), nullptr, resources);
        return true;
    case Op::EMC:
        EndMarked();
        return true;
    case Op::Unknown:
        return true;
    }
    return true;
}

void Interpreter::PathPoint(double x, double y, bool curve) {
    const Matrix& m = gs_.ctm;
    const double dx = x * m.a + y * m.c + m.e;
    const double dy = x * m.b + y * m.d + m.f;
    if (!std::isfinite(dx) || !std::isfinite(dy)) return;
    if (pathEmpty_) {
        pathBox_ = Box{dx, dy, dx, dy};
        pathEmpty_ = false;
    } else {
        pathBox_.x0 = std::fmin(pathBox_.x0, dx);
        pathBox_.y0 = std::fmin(pathBox_.y0, dy);
        pathBox_.x1 = std::fmax(pathBox_.x1, dx);
        pathBox_.y1 = std::fmax(pathBox_.y1, dy);
    }
    if (curve) pathCurved_ = true;
}

void Interpreter::Paint(bool fill, bool stroke) {
    if (!pathEmpty_) {
        if (pendingClip_) gs_.clip = Intersect(gs_.clip, pathBox_);
        if ((fill || stroke) && page_.shapes.size() < kMaxShapesPerPage) {
            Box box = pathBox_;
            if (stroke) {
                // Half the line width around the path; the line width scales with the CTM.
                const double scale = std::sqrt(std::fabs(gs_.ctm.a * gs_.ctm.d - gs_.ctm.b * gs_.ctm.c));
                const double half = std::fmax(gs_.lineWidth * scale, 0.1) / 2;
                box.x0 -= half, box.y0 -= half, box.x1 += half, box.y1 += half;
            }
            const Box visible = Intersect(box, gs_.clip);
            if (visible.x1 >= visible.x0 && visible.y1 >= visible.y0) {
                Shape s;
                s.x0 = float(box.x0), s.y0 = float(box.y0), s.x1 = float(box.x1), s.y1 = float(box.y1);
                s.filled = fill;
                s.stroked = stroke;
                s.curved = pathCurved_;
                s.segments = pathSegments_;
                if (pathIsRect_ && pathSubpaths_ == 1 && !pathCurved_)
                    s.kind = ShapeKind::Rect;
                else if (pathSegments_ == 1 && pathSubpaths_ == 1 && !pathCurved_)
                    s.kind = ShapeKind::Line;
                page_.shapes.push_back(s);
            }
        }
    } else if (pendingClip_) {
        // Clipping to an empty path hides everything after it.
        gs_.clip = Box{0, 0, 0, 0};
    }
    pathEmpty_ = true;
    pathCurved_ = false;
    pathSegments_ = 0;
    pathSubpaths_ = 0;
    pathIsRect_ = false;
    pendingClip_ = 0;
}

bool Interpreter::SetFont(const PdfValue& resources, std::string_view name, double size) {
    gs_.fontSize = std::isfinite(size) ? size : 0;
    const PdfValue& fontsDict = doc_.Get(resources, "Font");
    const PdfValue* entry = fontsDict.Find(name);
    if (!entry) {
        gs_.font = nullptr;
        return false;
    }
    SetFontEntry(*entry, size);
    return true;
}

void Interpreter::SetFontEntry(const PdfValue& entry, double size) {
    gs_.fontSize = std::isfinite(size) ? size : 0;
    const Text::Font& font = fonts_.Get(entry);
    gs_.font = &font;
    size_t id = 0;
    while (id < fontIds_.size() && fontIds_[id] != &font) ++id;
    if (id == fontIds_.size() && fontIds_.size() < 65535) fontIds_.push_back(&font);
    gs_.fontId = static_cast<uint16_t>(id < 65535 ? id : 65534);
}

void Interpreter::ApplyExtGState(const PdfValue& resources, std::string_view name) {
    const PdfValue& states = doc_.Get(resources, "ExtGState");
    const PdfValue* entry = states.Find(name);
    if (!entry) return;
    const PdfValue& state = doc_.Resolve(*entry);
    if (!state.IsDict()) return;
    const PdfValue& font = doc_.Get(state, "Font");
    const PdfValue fontArray = font;
    if (fontArray.IsArray() && fontArray.Array().size() >= 2) {
        double size = 0;
        if (doc_.Number(fontArray.Array()[1], size)) SetFontEntry(fontArray.Array()[0], size);
    }
    double alpha = 1;
    if (doc_.Number(doc_.Get(state, "ca"), alpha)) gs_.transparent = alpha <= 0.0;
    double width = 0;
    if (doc_.Number(doc_.Get(state, "LW"), width)) gs_.lineWidth = width;
}

void Interpreter::DoXObject(const PdfValue& resources, std::string_view name, int depth) {
    const PdfValue& xobjects = doc_.Get(resources, "XObject");
    const PdfValue* entry = xobjects.Find(name);
    if (!entry) return;
    const PdfValue& xobject = doc_.Resolve(*entry);
    if (!xobject.IsStream()) return;
    const PdfValue& subtype = doc_.Get(xobject, "Subtype");
    if (subtype.IsName("Image")) {
        const Box box = Intersect(UnitSquare(gs_.ctm), gs_.clip);
        if (!box.Empty() && page_.images.size() < kMaxShapesPerPage)
            page_.images.push_back(ImageBox{float(box.x0), float(box.y0), float(box.x1), float(box.y1)});
        return;
    }
    if (!subtype.IsName("Form")) return;
    if (depth + 1 > kMaxFormDepth) {
        budgetHit_ = true;
        return;
    }
    const uint32_t num = entry->IsRef() ? entry->refNum : 0;
    if (num != 0) {
        for (uint32_t active : formStack_)
            if (active == num) {
                page_.contentErrors++;
                return;
            }
    }
    if (formBuffers_.size() <= static_cast<size_t>(depth)) formBuffers_.resize(static_cast<size_t>(depth) + 1);
    std::string& data = formBuffers_[static_cast<size_t>(depth)];
    const StreamStatus status = doc_.StreamData(xobject, data);
    if (status == StreamStatus::Corrupt || status == StreamStatus::NotStream || status == StreamStatus::Salvaged ||
        status == StreamStatus::TooLarge)
        page_.contentErrors++;
    if (data.empty()) return;

    const GState saved = gs_;
    const size_t savedStack = stack_.size();
    const PdfValue& matrix = doc_.Get(xobject, "Matrix");
    if (matrix.IsArray() && matrix.Array().size() == 6) {
        double m[6];
        bool ok = true;
        for (int i = 0; i < 6; ++i) ok = ok && doc_.Number(matrix.Array()[static_cast<size_t>(i)], m[i]);
        if (ok) {
            const Matrix product = Multiply(Matrix{m[0], m[1], m[2], m[3], m[4], m[5]}, gs_.ctm);
            if (Finite(product)) gs_.ctm = product;
        }
    }
    PdfRect bbox;
    if (doc_.Rect(doc_.Get(xobject, "BBox"), bbox)) {
        const Matrix& c = gs_.ctm;
        const Matrix unit{bbox.x1 - bbox.x0, 0, 0, bbox.y1 - bbox.y0, bbox.x0, bbox.y0};
        gs_.clip = Intersect(gs_.clip, UnitSquare(Multiply(unit, c)));
    }
    const PdfValue& own = doc_.Get(xobject, "Resources");
    formStack_.push_back(num);
    // `data` must stay unchanged while the lexer reads it: deeper forms use deeper buffers.
    const std::string_view content(data);
    RunStream(content, own.IsDict() ? own : resources, depth + 1);
    formStack_.pop_back();
    while (stack_.size() > savedStack) stack_.pop_back();
    gs_ = saved;
}

void Interpreter::BeginMarked(std::string_view tag, const PdfValue* properties, const PdfValue& resources) {
    if (marked_.size() >= static_cast<size_t>(kMaxStateDepth)) {
        markedOverflow_++;
        return;
    }
    MarkedContent mc;
    mc.firstGlyph = static_cast<uint32_t>(page_.glyphs.size());
    mc.previousMcid = mcid_;
    mc.artifact = tag == "Artifact";
    if (mc.artifact) artifactDepth_++;
    if (properties) {
        const PdfValue* dict = properties;
        if (properties->IsName()) dict = &doc_.Get(doc_.Get(resources, "Properties"), properties->Name());
        if (dict->IsDict()) {
            const PdfValue& actual = doc_.Get(*dict, "ActualText");
            if (actual.IsString() && page_.text.size() < (size_t(1) << 30)) {
                mc.actualText = static_cast<uint32_t>(page_.text.size());
                AppendTextString(actual.String().substr(0, 4096), page_.text);
                mc.actualLength = static_cast<uint32_t>(page_.text.size() - mc.actualText);
                mc.hasActualText = true;
            }
            const PdfValue& mcid = doc_.Get(*dict, "MCID");
            if (mcid.type == PdfValue::Type::Int && mcid.integer >= 0 && mcid.integer < INT32_MAX)
                mcid_ = static_cast<int32_t>(mcid.integer);
        }
    }
    marked_.push_back(mc);
}

void Interpreter::EndMarked() {
    if (markedOverflow_ > 0) {
        markedOverflow_--;
        return;
    }
    if (marked_.empty()) return;
    const MarkedContent mc = marked_.back();
    marked_.pop_back();
    mcid_ = mc.previousMcid;
    if (mc.artifact && artifactDepth_ > 0) artifactDepth_--;
    if (!mc.hasActualText) return;
    // ActualText replaces the enclosed glyphs (§14.9.4): the first visible one carries the text and
    // the union of their boxes; the others leave the text.
    std::vector<Glyph>& glyphs = page_.glyphs;
    uint32_t carrier = UINT32_MAX;
    for (uint32_t i = mc.firstGlyph; i < glyphs.size(); ++i) {
        Glyph& g = glyphs[i];
        if (g.flags & kGlyphDropped) continue;
        if (carrier == UINT32_MAX) {
            carrier = i;
            continue;
        }
        Glyph& c = glyphs[carrier];
        if (g.dir == c.dir) {
            c.x0 = std::fmin(c.x0, g.x0), c.y0 = std::fmin(c.y0, g.y0);
            c.x1 = std::fmax(c.x1, g.x1), c.y1 = std::fmax(c.y1, g.y1);
            c.u0 = std::fmin(c.u0, g.u0), c.u1 = std::fmax(c.u1, g.u1);
            c.vb = std::fmin(c.vb, g.vb), c.vt = std::fmax(c.vt, g.vt);
        }
        g.flags |= kGlyphDropped;
    }
    if (carrier == UINT32_MAX) return;
    Glyph& c = glyphs[carrier];
    std::string_view text = std::string_view(page_.text).substr(mc.actualText, mc.actualLength);
    // A space-only replacement is a word break: keep a space glyph.
    bool blank = true;
    for (char ch : text) blank = blank && (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r');
    c.text = mc.actualText;
    c.textLength = static_cast<uint16_t>(mc.actualLength < 65535 ? mc.actualLength : 65535);
    c.flags = static_cast<uint16_t>(c.flags & ~(kGlyphUnmapped | kGlyphGuessed | kGlyphSpace));
    if (blank) c.flags |= kGlyphSpace;
    if (mc.actualLength == 0) c.flags |= kGlyphDropped;
}

bool Interpreter::ShowArray(const PdfValue& array) {
    for (const PdfValue& item : array.Array()) {
        if (budgetHit_) return true;
        if (item.IsString()) {
            if (!ShowString(item.String())) return false;
        } else if (item.IsNumber()) {
            const double adjust = item.Number();
            if (!std::isfinite(adjust)) continue;
            const double shift = -adjust / 1000 * gs_.fontSize;
            Matrix t;
            if (gs_.font && gs_.font->Vertical())
                t.f = shift;
            else
                t.e = shift * gs_.hScale;
            textMatrix_ = Multiply(t, textMatrix_);
        }
    }
    return true;
}

// Appends the UTF-8 text of one code to the page text, normalized: presentation-form ligatures
// expanded (algorithms §4.4). Returns the flags the text implies.
uint16_t AppendGlyphText(std::string_view unicode, std::string& out) {
    uint16_t flags = 0;
    size_t at = 0;
    uint32_t cp = 0;
    size_t length = 0;
    bool any = false, allSpace = true;
    while (Utf8::Decode(unicode, at, cp, length)) {
        at += length;
        any = true;
        switch (cp) {
        case 0xFB00: out += "ff"; allSpace = false; continue;
        case 0xFB01: out += "fi"; allSpace = false; continue;
        case 0xFB02: out += "fl"; allSpace = false; continue;
        case 0xFB03: out += "ffi"; allSpace = false; continue;
        case 0xFB04: out += "ffl"; allSpace = false; continue;
        case 0xFB05:
        case 0xFB06: out += "st"; allSpace = false; continue;
        default: break;
        }
        const bool space = cp == 0x20 || cp == 0xA0 || cp == 0x09 || cp == 0x0A || cp == 0x0D ||
                           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000;
        if (space) {
            out.push_back(' ');
            continue;
        }
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0) || cp == 0x200B || cp == 0xFEFF) continue;
        allSpace = false;
        Utf8::Append(out, cp);
    }
    if (any && allSpace) flags |= kGlyphSpace;
    return flags;
}

bool Interpreter::ShowString(std::string_view bytes) {
    const Text::Font* font = gs_.font;
    if (!font) return false;
    const bool vertical = font->Vertical();
    const double tfs = gs_.fontSize;
    const double th = gs_.hScale;
    Matrix tm = Multiply(textMatrix_, gs_.ctm);
    size_t at = 0;
    Text::Code code;
    while (at < bytes.size() && font->Next(bytes, at, code)) {
        // Trm = [Tfs·Th 0 0 Tfs 0 Trise] × Tm × CTM (§9.4.2).
        Matrix trm;
        trm.a = tfs * th * tm.a;
        trm.b = tfs * th * tm.b;
        trm.c = tfs * tm.c;
        trm.d = tfs * tm.d;
        trm.e = gs_.rise * tm.c + tm.e;
        trm.f = gs_.rise * tm.d + tm.f;
        double advance;
        if (vertical) {
            advance = code.verticalAdvance * tfs + gs_.charSpace + (code.wordSpace ? gs_.wordSpace : 0);
        } else {
            advance = (code.width * tfs + gs_.charSpace + (code.wordSpace ? gs_.wordSpace : 0)) * th;
        }
        EmitGlyph(code, trm, code.width);
        if (budgetHit_) return true;
        if (!std::isfinite(advance)) advance = 0;
        if (vertical) {
            tm.e += advance * tm.c;
            tm.f += advance * tm.d;
            textMatrix_.e += advance * textMatrix_.c;
            textMatrix_.f += advance * textMatrix_.d;
        } else {
            tm.e += advance * tm.a;
            tm.f += advance * tm.b;
            textMatrix_.e += advance * textMatrix_.a;
            textMatrix_.f += advance * textMatrix_.b;
        }
    }
    return true;
}

void Interpreter::EmitGlyph(const Text::Code& code, const Matrix& trm, double width) {
    stats_.glyphs++;
    if (code.source == Text::UnicodeSource::None) stats_.unmapped++;
    if (code.source == Text::UnicodeSource::Guess) stats_.guessed++;
    if (page_.glyphs.size() >= kMaxGlyphsPerPage || page_.text.size() >= (size_t(1) << 30)) {
        budgetHit_ = true;
        return;
    }
    const Text::Font& font = *gs_.font;
    const double size = std::sqrt(trm.c * trm.c + trm.d * trm.d);
    const double runLength = std::sqrt(trm.a * trm.a + trm.b * trm.b);
    if (!std::isfinite(size) || !std::isfinite(runLength) || !std::isfinite(trm.e) || !std::isfinite(trm.f)) return;

    Glyph g;
    // Baseline direction from the text matrix's x axis, or the y axis turned when x is degenerate.
    double bx = trm.a, by = trm.b;
    if (runLength < 1e-9) bx = trm.d, by = -trm.c;
    if (std::fabs(bx) >= std::fabs(by))
        g.dir = bx >= 0 ? 0 : 2;
    else
        g.dir = by > 0 ? 1 : 3;

    double ascent = font.Ascent(), descent = font.Descent();
    if (!(ascent > descent)) ascent = 0.8, descent = -0.2;
    double x0 = 0, x1 = width, y0 = descent, y1 = ascent;
    if (font.Vertical()) {
        x0 = -0.5, x1 = 0.5, y0 = code.verticalAdvance < 0 ? code.verticalAdvance : -1, y1 = 0;
    }
    double dx[4], dy[4];
    const double cx[4] = {x0, x1, x0, x1}, cy[4] = {y0, y0, y1, y1};
    for (int i = 0; i < 4; ++i) {
        dx[i] = cx[i] * trm.a + cy[i] * trm.c + trm.e;
        dy[i] = cx[i] * trm.b + cy[i] * trm.d + trm.f;
    }
    double bx0 = dx[0], bx1 = dx[0], by0 = dy[0], by1 = dy[0];
    double fu0 = 0, fu1 = 0, fv0 = 0, fv1 = 0;
    for (int i = 0; i < 4; ++i) {
        bx0 = std::fmin(bx0, dx[i]), bx1 = std::fmax(bx1, dx[i]);
        by0 = std::fmin(by0, dy[i]), by1 = std::fmax(by1, dy[i]);
        double u, v;
        ToFrame(g.dir, dx[i], dy[i], u, v);
        if (i == 0) fu0 = fu1 = u, fv0 = fv1 = v;
        fu0 = std::fmin(fu0, u), fu1 = std::fmax(fu1, u);
        fv0 = std::fmin(fv0, v), fv1 = std::fmax(fv1, v);
    }
    double ou, ov;
    ToFrame(g.dir, trm.e, trm.f, ou, ov);
    g.x0 = float(bx0), g.y0 = float(by0), g.x1 = float(bx1), g.y1 = float(by1);
    g.u0 = float(fu0), g.u1 = float(fu1), g.vb = float(fv0), g.vt = float(fv1), g.v = float(ov);
    g.size = float(size);
    g.font = gs_.fontId;
    g.mcid = mcid_;

    const Text::FontStyle& style = font.Style();
    uint16_t flags = 0;
    if (style.bold || gs_.render == 2) flags |= kGlyphBold;
    if (style.italic) flags |= kGlyphItalic;
    if (style.monospace) flags |= kGlyphMonospace;
    if (style.symbolic) flags |= kGlyphSymbolic;
    // A sheared text matrix is a synthetic oblique (algorithms §6.6).
    if (runLength > 1e-9 && size > 1e-9) {
        const double cosine = (trm.a * trm.c + trm.b * trm.d) / (runLength * size);
        if (std::fabs(cosine) > 0.1) flags |= kGlyphItalic;
    }
    const int mode = gs_.render & 7;
    if (mode == 3 || mode == 7 || gs_.transparent) flags |= kGlyphInvisible;
    if (mode == 1 || mode == 5) flags |= kGlyphStrokeOnly;
    if (artifactDepth_ > 0) flags |= kGlyphArtifact;
    // Clipped away: the glyph's centre is outside the clip (which lies within the page).
    const double mx = (bx0 + bx1) / 2, my = (by0 + by1) / 2;
    const Box& clip = gs_.clip;
    if (mx < clip.x0 - 1 || mx > clip.x1 + 1 || my < clip.y0 - 1 || my > clip.y1 + 1 || clip.Empty())
        flags |= kGlyphClipped;
    if (code.source == Text::UnicodeSource::None) flags |= kGlyphUnmapped;
    if (code.source == Text::UnicodeSource::Guess) flags |= kGlyphGuessed;

    g.text = static_cast<uint32_t>(page_.text.size());
    flags |= AppendGlyphText(code.unicode, page_.text);
    const size_t length = page_.text.size() - g.text;
    g.textLength = static_cast<uint16_t>(length < 65535 ? length : 65535);
    if (length == 0 && !(flags & kGlyphUnmapped)) flags |= kGlyphDropped;  // control characters
    g.flags = flags;
    page_.glyphs.push_back(g);
}

void Interpreter::ReadLinks(int index) {
    const PdfValue& annots = doc_.Get(doc_.PageDict(index), "Annots");
    if (!annots.IsArray()) return;
    for (const PdfValue& item : annots.Array()) {
        if (page_.links.size() >= kMaxLinksPerPage) break;
        const PdfValue& annot = doc_.Resolve(item);
        if (!doc_.Get(annot, "Subtype").IsName("Link")) continue;
        const PdfValue& action = doc_.Get(annot, "A");
        if (!doc_.Get(action, "S").IsName("URI")) continue;
        const PdfValue& uri = doc_.Get(action, "URI");
        if (!uri.IsString() || uri.String().empty()) continue;
        PdfRect rect;
        if (!doc_.Rect(doc_.Get(annot, "Rect"), rect)) continue;
        Box box = UnitSquare(Multiply(Matrix{rect.Width(), 0, 0, rect.Height(), rect.x0, rect.y0}, pageMatrix_));
        LinkArea link;
        link.x0 = float(box.x0), link.y0 = float(box.y0), link.x1 = float(box.x1), link.y1 = float(box.y1);
        link.uri = static_cast<uint32_t>(page_.text.size());
        std::string_view bytes = uri.String().substr(0, 8192);
        // URIs are 7-bit ASCII (§12.6.4.7); spaces and controls are percent-encoded.
        for (unsigned char ch : bytes) {
            if (ch <= 0x20 || ch >= 0x7F) {
                static const char kHex[] = "0123456789ABCDEF";
                page_.text.push_back('%');
                page_.text.push_back(kHex[ch >> 4]);
                page_.text.push_back(kHex[ch & 15]);
            } else {
                page_.text.push_back(static_cast<char>(ch));
            }
        }
        link.uriLength = static_cast<uint32_t>(page_.text.size() - link.uri);
        page_.links.push_back(link);
    }
}

} // namespace

void AppendTextString(std::string_view bytes, std::string& out) {
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE && static_cast<unsigned char>(bytes[1]) == 0xFF) {
        size_t i = 2;
        while (i + 1 < bytes.size()) {
            uint32_t unit = (uint32_t(static_cast<unsigned char>(bytes[i])) << 8) | static_cast<unsigned char>(bytes[i + 1]);
            i += 2;
            if (unit >= 0xD800 && unit < 0xDC00 && i + 1 < bytes.size()) {
                const uint32_t low =
                    (uint32_t(static_cast<unsigned char>(bytes[i])) << 8) | static_cast<unsigned char>(bytes[i + 1]);
                if (low >= 0xDC00 && low < 0xE000) {
                    unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                    i += 2;
                }
            }
            // A language escape (U+001B ... U+001B) is skipped (§7.9.2.2.1).
            if (unit == 0x1B) {
                while (i + 1 < bytes.size()) {
                    const uint32_t next = (uint32_t(static_cast<unsigned char>(bytes[i])) << 8) |
                                          static_cast<unsigned char>(bytes[i + 1]);
                    i += 2;
                    if (next == 0x1B) break;
                }
                continue;
            }
            Utf8::Append(out, unit);
        }
        return;
    }
    if (bytes.size() >= 3 && bytes.substr(0, 3) == "\xEF\xBB\xBF") {
        const std::string_view rest = bytes.substr(3);
        size_t at = 0;
        uint32_t cp = 0;
        size_t length = 0;
        while (Utf8::Decode(rest, at, cp, length)) {
            Utf8::Append(out, cp);
            at += length;
        }
        return;
    }
    for (unsigned char ch : bytes) {
        uint32_t cp = ch;
        if (ch >= 0x18 && ch <= 0x1F)
            cp = kPdfDocLow[ch - 0x18];
        else if (ch >= 0x80 && ch <= 0xA0)
            cp = kPdfDocHigh[ch - 0x80];
        Utf8::Append(out, cp);
    }
}

void InterpretPage(const Document& document, Text::FontCache& fonts, int index, bool withLinks, PageData& page,
                   PageStats& stats) {
    page.Clear();
    page.number = index + 1;
    Interpreter interpreter(document, fonts, page, stats);
    interpreter.Run(index, withLinks);
}

void ReadOutline(const Document& document, std::vector<OutlineEntry>& out) {
    out.clear();
    const PdfValue& outlines = document.Get(document.Catalog(), "Outlines");
    if (!outlines.IsDict()) return;
    struct Pending {
        const PdfValue* node;
        int level;
    };
    // Depth-first preorder: a node, its children, then its next sibling. Object numbers are
    // visited once, so a cyclic /Next or /First chain ends.
    std::vector<Pending> work;
    std::unordered_set<uint32_t> seen;
    if (const PdfValue* first = outlines.Find("First")) work.push_back({first, 1});
    int visits = 0;
    while (!work.empty() && static_cast<int>(out.size()) < kMaxOutlineEntries && visits++ < 4 * kMaxOutlineEntries) {
        const Pending p = work.back();
        work.pop_back();
        if (p.node->IsRef() && !seen.insert(p.node->refNum).second) continue;
        const PdfValue& node = document.Resolve(*p.node);
        if (!node.IsDict()) continue;
        OutlineEntry entry;
        entry.level = p.level;
        AppendTextString(document.Get(node, "Title").String(), entry.title);
        out.push_back(std::move(entry));
        if (const PdfValue* next = node.Find("Next")) work.push_back({next, p.level});
        if (p.level < 64)
            if (const PdfValue* child = node.Find("First")) work.push_back({child, p.level + 1});
    }
}

} // namespace RayoPdf::Layout
