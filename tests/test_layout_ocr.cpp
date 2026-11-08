// SPDX-License-Identifier: Apache-2.0
// Core tests of OCR routing and the OCR client (exit codes 1200-1249): the add-on protocol's frame
// codec (common/ocr_protocol.h), routing (layout/ocr_route.h), page rasters and the OCR text layer
// (layout/ocr_layer.h), the client against the stand-in add-on tests/ocr_mock_addon.cpp in each of
// its failure modes (layout/ocr_client.h), and whole conversions with and without an add-on
// (layout/ocr.h), including documents converted on several threads at once.
#include "check.h"
#include "pdf_builder.h"

#include "common/ocr_protocol.h"
#include "layout/convert.h"
#include "layout/ocr_client.h"
#include "layout/ocr_layer.h"
#include "layout/ocr_route.h"
#include "layout/pipeline.h"
#include "pdf/document.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef RAYOPDF_OCR_MOCK
#error "RAYOPDF_OCR_MOCK names the stand-in add-on (CMakeLists.txt)"
#endif

using namespace RayoPdf;
using namespace RayoPdf::Layout;
namespace P = RayoPdf::OcrProtocol;

namespace {

const char* const kMock = RAYOPDF_OCR_MOCK;
const char* const kNoAddon = "/nonexistent/rayopdf-ocr-missing";

void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

void Mode(const char* mode) {
    SetEnv("RAYOPDF_OCR_MOCK_MODE", mode);
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void Show(const char* name, const std::string& md) {
    if (std::getenv("RAYOPDF_TEST_VERBOSE")) std::fprintf(stderr, "--- %s\n%s", name, md.c_str());
}

// ---------------------------------------------------------------- frames in memory

struct Memory {
    std::string_view data;
    size_t chunk = 7;  // bytes a read returns at most: frames arrive in pieces
};

size_t ReadMemory(void* context, char* buffer, size_t size) {
    Memory* m = static_cast<Memory*>(context);
    const size_t n = std::min({size, m->data.size(), m->chunk});
    std::memcpy(buffer, m->data.data(), n);
    m->data.remove_prefix(n);
    return n;
}

P::FrameStatus ReadOne(std::string_view bytes, P::Frame& frame) {
    Memory m{bytes};
    return P::ReadFrame(ReadMemory, &m, frame);
}

// A frame of `header` (its '\n' added) and `payload`.
std::string RawFrame(const std::string& header, std::string_view payload = {}) {
    const uint32_t n = static_cast<uint32_t>(header.size() + 1);
    std::string out;
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((n >> (8 * i)) & 0xFF));
    out += header;
    out += '\n';
    out.append(payload.data(), payload.size());
    return out;
}

bool SameLines(const P::Response& a, const P::Response& b) {
    if (a.lines.size() != b.lines.size()) return false;
    for (size_t i = 0; i < a.lines.size(); ++i) {
        const P::Line &x = a.lines[i], &y = b.lines[i];
        if (x.x0 != y.x0 || x.y0 != y.y0 || x.x1 != y.x1 || x.y1 != y.y1 || x.text != y.text || x.conf != y.conf ||
            x.chars.size() != y.chars.size())
            return false;
        for (size_t k = 0; k < x.chars.size(); ++k)
            if (x.chars[k].x0 != y.chars[k].x0 || x.chars[k].x1 != y.chars[k].x1) return false;
    }
    return true;
}

void TestProtocolRoundTrips() {
    P::Frame frame;
    std::string bytes, error;
    // Requests: page, lines, quit.
    P::Request page;
    page.id = 41;
    page.width = 3;
    page.height = 2;
    page.dpi = 300;
    P::WriteRequest(page, "abcdef", bytes);
    RAYOPDF_CHECK(bytes.compare(4, std::string::npos,
                                "{\"id\":41,\"op\":\"page\",\"width\":3,\"height\":2,\"channels\":1,\"dpi\":300,"
                                "\"payload\":6}\nabcdef") == 0,
                  1200);
    P::Request back;
    RAYOPDF_CHECK(ReadOne(bytes, frame) == P::FrameStatus::Ok && frame.payload == "abcdef" &&
                      P::ParseRequest(frame.header, frame.payload.size(), back, error) && back.id == 41 &&
                      back.op == P::Op::Page && back.width == 3 && back.height == 2 && back.dpi == 300,
                  1200);
    P::Request lines;
    lines.id = 2;
    lines.op = P::Op::Lines;
    lines.lines = {{2, 1}, {1, 3}};
    P::WriteRequest(lines, "12345", bytes);
    RAYOPDF_CHECK(ReadOne(bytes, frame) == P::FrameStatus::Ok &&
                      P::ParseRequest(frame.header, frame.payload.size(), back, error) && back.op == P::Op::Lines &&
                      back.lines.size() == 2 && back.lines[1].height == 3,
                  1200);
    P::Request quit;
    quit.op = P::Op::Quit;
    quit.id = 9;
    P::WriteRequest(quit, {}, bytes);
    RAYOPDF_CHECK(ReadOne(bytes, frame) == P::FrameStatus::Ok &&
                      P::ParseRequest(frame.header, 0, back, error) && back.op == P::Op::Quit && back.id == 9,
                  1200);

    // Responses: escapes, non-ASCII text, confidences, negative coordinates.
    P::Response response;
    response.id = 7;
    const char* texts[] = {"plain", "quote \" back \\ tab\t", "\xC5\xBC\xC3\xB3\xC5\x82w \xE4\xB8\xAD\xF0\x9F\x98\x80", ""};
    const uint16_t confs[] = {0, 10000, 5000, 1234};
    for (int i = 0; i < 4; ++i) {
        P::Line line;
        line.x0 = -5 + i, line.y0 = i, line.x1 = 100, line.y1 = 20 + i;
        line.text = texts[i];
        line.conf = confs[i];
        for (size_t k = 0; k < P::CodePoints(line.text); ++k)
            line.chars.push_back({static_cast<int32_t>(k), static_cast<int32_t>(k + 1)});
        response.lines.push_back(line);
    }
    P::WriteResponse(response, bytes);
    P::Response parsed;
    RAYOPDF_CHECK(ReadOne(bytes, frame) == P::FrameStatus::Ok && frame.payload.empty() &&
                      P::ParseResponse(frame.header, parsed, error) && parsed.id == 7 && parsed.ok &&
                      SameLines(parsed, response),
                  1201);
    std::string again;
    P::WriteResponse(parsed, again);
    RAYOPDF_CHECK(again == bytes, 1201);
    RAYOPDF_CHECK(Contains(bytes, "\"conf\":0,") && Contains(bytes, "\"conf\":1,") && Contains(bytes, "\"conf\":0.5,") &&
                      Contains(bytes, "\"conf\":0.1234,"),
                  1201);
    P::Response failure;
    failure.id = 3;
    failure.ok = false;
    failure.error = P::kErrorBadPixels;
    P::WriteResponse(failure, bytes);
    RAYOPDF_CHECK(ReadOne(bytes, frame) == P::FrameStatus::Ok && P::ParseResponse(frame.header, parsed, error) &&
                      !parsed.ok && parsed.error == "bad-pixels" && parsed.id == 3,
                  1201);

    // Handshake.
    P::Handshake hello;
    hello.version = "0.0.9";
    hello.protocol = 1;
    hello.engine = "pp-ocrv6";
    hello.tier = "small";
    hello.runtime = "ncnn";
    hello.models = {{"det", "aa"}, {"rec", "bb"}};
    hello.scripts = {"latin", "cjk"};
    P::WriteHandshake(hello, bytes);
    P::Handshake read;
    RAYOPDF_CHECK(ReadOne(bytes, frame) == P::FrameStatus::Ok && P::ParseHandshake(frame.header, read, error) &&
                      read.version == "0.0.9" && read.protocol == 1 && read.engine == "pp-ocrv6" && read.tier == "small" &&
                      read.models.size() == 2 && read.models[1].first == "rec" && read.models[1].second == "bb" &&
                      read.scripts.size() == 2 && read.scripts[1] == "cjk",
                  1202);
    // Members in another order, unknown ones skipped.
    RAYOPDF_CHECK(P::ParseHandshake("{\"x\":[1,{\"y\":null}],\"protocol\":1, \"rayopdf-ocr\":\"v\"}", read, error) &&
                      read.version == "v",
                  1202);
}

void TestFrameErrors() {
    P::Frame frame;
    RAYOPDF_CHECK(ReadOne("", frame) == P::FrameStatus::End, 1203);
    RAYOPDF_CHECK(ReadOne(std::string_view("\x05\x00", 2), frame) == P::FrameStatus::Truncated, 1203);
    RAYOPDF_CHECK(ReadOne(std::string_view("\x05\x00\x00\x00{}", 6), frame) == P::FrameStatus::Truncated, 1203);
    RAYOPDF_CHECK(ReadOne(std::string_view("\x01\x00\x10\x00", 4), frame) == P::FrameStatus::TooLarge, 1203);
    RAYOPDF_CHECK(ReadOne(std::string_view("\x01\x00\x00\x00\n", 5), frame) == P::FrameStatus::Malformed, 1203);
    RAYOPDF_CHECK(ReadOne(RawFrame("{}x"), frame) == P::FrameStatus::Malformed, 1203);
    RAYOPDF_CHECK(ReadOne(RawFrame("{\n}"), frame) == P::FrameStatus::Malformed, 1203);
    std::string noBreak = RawFrame("{}");
    noBreak[6] = ' ';
    RAYOPDF_CHECK(ReadOne(noBreak, frame) == P::FrameStatus::Malformed, 1203);
    RAYOPDF_CHECK(ReadOne(RawFrame("{\"payload\":-1}"), frame) == P::FrameStatus::Malformed, 1203);
    RAYOPDF_CHECK(ReadOne(RawFrame("{\"payload\":1,\"payload\":1}", "ab"), frame) == P::FrameStatus::Malformed, 1203);
    RAYOPDF_CHECK(ReadOne(RawFrame("{\"payload\":268435456}"), frame) == P::FrameStatus::TooLarge, 1203);
    RAYOPDF_CHECK(ReadOne(RawFrame("{\"payload\":5}", "abc"), frame) == P::FrameStatus::Truncated, 1203);
    std::string deep;
    for (int i = 0; i < 40; ++i) deep += "[";
    for (int i = 0; i < 40; ++i) deep += "]";
    RAYOPDF_CHECK(ReadOne(RawFrame("{\"a\":" + deep + "}"), frame) == P::FrameStatus::Malformed, 1203);
    // Two frames back to back.
    const std::string two = RawFrame("{\"payload\":2}", "xy") + RawFrame("{}");
    Memory m{two};
    RAYOPDF_CHECK(P::ReadFrame(ReadMemory, &m, frame) == P::FrameStatus::Ok && frame.payload == "xy" &&
                      P::ReadFrame(ReadMemory, &m, frame) == P::FrameStatus::Ok && frame.header == "{}\n" &&
                      P::ReadFrame(ReadMemory, &m, frame) == P::FrameStatus::End,
                  1203);
}

void TestRequestErrors() {
    P::Request r;
    std::string error;
    const auto bad = [&](const char* header, uint64_t payload, const char* code) {
        return !P::ParseRequest(header, payload, r, error) && error == code;
    };
    RAYOPDF_CHECK(bad("{\"id\":5,\"op\":\"page\",\"width\":2,\"height\":2,\"payload\":3}", 3, "bad-pixels") && r.id == 5, 1204);
    RAYOPDF_CHECK(bad("{\"id\":5,\"op\":\"page\",\"width\":65536,\"height\":65536,\"payload\":0}", 0, "too-large"), 1204);
    RAYOPDF_CHECK(bad("{\"op\":\"page\",\"width\":1,\"height\":1,\"payload\":1}", 1, "bad-request"), 1204);
    RAYOPDF_CHECK(bad("{\"id\":1,\"op\":\"scan\",\"payload\":0}", 0, "bad-request"), 1204);
    RAYOPDF_CHECK(bad("{\"id\":1,\"op\":\"page\",\"width\":1,\"height\":1,\"channels\":3,\"payload\":1}", 1, "bad-request"), 1204);
    RAYOPDF_CHECK(bad("{\"id\":1,\"id\":2,\"op\":\"quit\"}", 0, "bad-request"), 1204);
    RAYOPDF_CHECK(bad("{\"id\":1,\"op\":\"lines\",\"lines\":[],\"payload\":0}", 0, "bad-request"), 1204);
    RAYOPDF_CHECK(bad("{\"id\":1.5,\"op\":\"quit\"}", 0, "bad-request"), 1204);
    RAYOPDF_CHECK(bad("{\"id\":01,\"op\":\"quit\"}", 0, "bad-request"), 1204);
    RAYOPDF_CHECK(P::ParseRequest("{\"id\":18446744073709551615,\"op\":\"quit\"}", 0, r, error) &&
                      r.id == UINT64_MAX,
                  1204);
    RAYOPDF_CHECK(bad("{\"id\":18446744073709551616,\"op\":\"quit\"}", 0, "bad-request"), 1204);
}

void TestResponseErrors() {
    P::Response r;
    std::string error;
    const auto ok = [&](const std::string& header) { return P::ParseResponse(header, r, error); };
    const std::string line = "{\"box\":[0,0,4,2],\"text\":\"ab\",\"conf\":0.5,\"chars\":[[0,2],[2,4]]}";
    RAYOPDF_CHECK(ok("{\"id\":1,\"status\":\"ok\",\"lines\":[" + line + "]}") && r.lines.size() == 1, 1205);
    RAYOPDF_CHECK(!ok("{\"id\":1,\"status\":\"ok\",\"lines\":[{\"box\":[0,0,4,2],\"text\":\"abc\",\"conf\":0.5,"
                      "\"chars\":[[0,2],[2,4]]}]}"),
                  1205);
    RAYOPDF_CHECK(!ok("{\"id\":1,\"status\":\"ok\",\"lines\":[{\"box\":[4,0,0,2],\"text\":\"\",\"conf\":0,\"chars\":[]}]}"), 1205);
    RAYOPDF_CHECK(!ok("{\"id\":1,\"status\":\"ok\",\"lines\":[{\"box\":[0,0,4],\"text\":\"\",\"conf\":0,\"chars\":[]}]}"), 1205);
    RAYOPDF_CHECK(!ok("{\"id\":1,\"status\":\"ok\",\"lines\":[{\"box\":[0,0,4,2],\"text\":\"\",\"conf\":1.5,\"chars\":[]}]}"), 1205);
    RAYOPDF_CHECK(!ok("{\"id\":1,\"status\":\"ok\",\"lines\":[{\"box\":[0,0,4,2],\"text\":\"\",\"chars\":[]}]}"), 1205);
    RAYOPDF_CHECK(!ok("{\"id\":1,\"status\":\"error\"}"), 1205);
    RAYOPDF_CHECK(!ok("{\"id\":1,\"status\":\"error\",\"error\":\"x\",\"lines\":[]}"), 1205);
    RAYOPDF_CHECK(!ok("{\"id\":1,\"status\":\"ok\",\"error\":\"x\"}"), 1205);
    RAYOPDF_CHECK(!ok("{\"id\":1,\"status\":\"maybe\"}"), 1205);
    RAYOPDF_CHECK(!ok("{\"status\":\"ok\"}"), 1205);
    RAYOPDF_CHECK(!ok("{\"id\":1,\"status\":\"ok\",\"lines\":[{\"box\":[0,0,1,1],\"text\":\"\xC3\",\"conf\":0,\"chars\":[[0,1]]}]}"),
                  1205);
    RAYOPDF_CHECK(!ok("{\"id\":1,\"status\":\"ok\",\"lines\":[{\"box\":[0,0,1,1],\"text\":\"\\ud800\",\"conf\":0,\"chars\":[[0,1]]}]}"),
                  1205);
    RAYOPDF_CHECK(ok("{\"id\":1,\"status\":\"ok\",\"lines\":[{\"box\":[0,0,1,1],\"text\":\"\\ud83d\\ude00\",\"conf\":0,"
                     "\"chars\":[[0,1]]}]}") &&
                      r.lines[0].text == "\xF0\x9F\x98\x80",
                  1205);
    RAYOPDF_CHECK(!ok("{\"id\":1,\"status\":\"ok\",\"lines\":[{\"box\":[0,0,1,1],\"text\":\"a\nb\",\"conf\":0,"
                      "\"chars\":[[0,1],[0,1],[0,1]]}]}"),
                  1205);
    // Confidence: rounded half up to four decimals; no exponents or signs.
    const auto conf = [&](const char* text, int expected) {
        const std::string header = std::string("{\"id\":1,\"status\":\"ok\",\"lines\":[{\"box\":[0,0,1,1],\"text\":\"\",\"conf\":") +
                                   text + ",\"chars\":[]}]}";
        const bool parsed = ok(header);
        return expected < 0 ? !parsed : parsed && r.lines[0].conf == expected;
    };
    RAYOPDF_CHECK(conf("0.99995", 10000) && conf("0.12344", 1234) && conf("0.12345", 1235) && conf("1.0000", 10000) &&
                      conf("0.5", 5000) && conf("1.00001", -1) && conf("1e0", -1) && conf("-0.1", -1) && conf("0.", -1) &&
                      conf("00.1", -1),
                  1206);
}

// ---------------------------------------------------------------- routing

PageData NewPage() {
    PageData page;
    page.number = 1;
    page.width = 612;
    page.height = 792;
    return page;
}

void AddText(PageData& page, std::string_view text, float x, float y, float size, uint16_t flags = 0) {
    for (char c : text) {
        Glyph g;
        g.x0 = x, g.x1 = x + 0.5f * size, g.y0 = y - 0.2f * size, g.y1 = y + 0.8f * size;
        g.u0 = g.x0, g.u1 = g.x1, g.vb = g.y0, g.vt = g.y1, g.v = y;
        g.size = size;
        g.text = static_cast<uint32_t>(page.text.size());
        page.text.push_back(c);
        g.textLength = 1;
        g.flags = flags | (c == ' ' ? kGlyphSpace : 0);
        page.glyphs.push_back(g);
        x += 0.5f * size;
    }
}

ImageBox Placed(float x0, float y0, float x1, float y1, uint32_t w, uint32_t h, uint64_t identity = 1) {
    ImageBox image;
    image.x0 = x0, image.y0 = y0, image.x1 = x1, image.y1 = y1;
    image.width = w, image.height = h;
    image.identity = identity;
    image.matrix[0] = x1 - x0, image.matrix[3] = y1 - y0, image.matrix[4] = x0, image.matrix[5] = y0;
    return image;
}

const char* const kSentence = "The quick brown fox jumps over the lazy dog and keeps running far away. ";

void TestRouting() {
    PageSignals s;
    OcrPlan plan;
    // An image-only page (a scan): the whole page.
    PageData scan = NewPage();
    scan.images.push_back(Placed(0, 0, 612, 792, 2550, 3300));
    AddText(scan, "12", 300, 20, 10);  // a page number stamped over it
    MeasurePage(scan, 2, 0, s);
    RAYOPDF_CHECK(s.visibleChars == 2 && s.invisibleChars == 0 && s.coverage > 0.99f && !s.broken, 1210);
    PlanPage(scan, s, OcrMode::Auto, plan);
    RAYOPDF_CHECK(plan.route == OcrRoute::Page && plan.images.size() == 1 && !plan.replaceInvisible, 1211);
    PlanPage(scan, s, OcrMode::Off, plan);
    RAYOPDF_CHECK(plan.route == OcrRoute::None, 1211);
    // Strips: a scan in three images covers the page together.
    PageData strips = NewPage();
    for (int i = 0; i < 3; ++i) strips.images.push_back(Placed(0, 264.0f * i, 612, 264.0f * (i + 1), 2550, 1100, 10 + i));
    MeasurePage(strips, 0, 0, s);
    PlanPage(strips, s, OcrMode::Auto, plan);
    RAYOPDF_CHECK(plan.route == OcrRoute::Page && plan.images.size() == 3, 1211);
    // An existing invisible OCR layer is the page's text.
    PageData layered = scan;
    for (int i = 0; i < 3; ++i) AddText(layered, kSentence, 72, 700 - 20.0f * i, 10, kGlyphInvisible);
    MeasurePage(layered, layered.glyphs.size(), 0, s);
    RAYOPDF_CHECK(s.invisibleChars >= kPageMinChars && s.visibleChars == 2, 1210);
    PlanPage(layered, s, OcrMode::Auto, plan);
    RAYOPDF_CHECK(plan.route == OcrRoute::None, 1211);
    PlanPage(layered, s, OcrMode::Force, plan);
    RAYOPDF_CHECK(plan.route == OcrRoute::Page && plan.replaceInvisible, 1212);
    // A digital page: a large image with nothing over it is a region to check; a small one, one
    // under text, one too few pixels and a repeated placement are not.
    PageData digital = NewPage();
    for (int i = 0; i < 4; ++i) AddText(digital, kSentence, 72, 720 - 14.0f * i, 10);
    digital.images.push_back(Placed(72, 300, 540, 600, 1400, 900, 1));    // candidate
    digital.images.push_back(Placed(72, 100, 100, 128, 100, 100, 2));     // under 5 % of the page
    digital.images.push_back(Placed(60, 680, 560, 740, 2000, 240, 3));    // the text is drawn over it
    digital.images.push_back(Placed(72, 150, 540, 290, 40, 1000, 4));     // 40 pixels wide
    digital.images.push_back(Placed(72, 300, 540, 600, 1400, 900, 1));    // the candidate again
    MeasurePage(digital, digital.glyphs.size(), 0, s);
    RAYOPDF_CHECK(s.visibleChars > 200 && !s.broken && !s.vectorText, 1210);
    PlanPage(digital, s, OcrMode::Auto, plan);
    RAYOPDF_CHECK(plan.route == OcrRoute::Regions && plan.checkText && plan.images.size() == 1 && plan.images[0] == 0,
                  1211);
    PlanPage(digital, s, OcrMode::Force, plan);
    RAYOPDF_CHECK(plan.route == OcrRoute::Regions && !plan.checkText && plan.images.size() == 1, 1212);
    // No images: nothing, whatever the mode.
    PageData plain = NewPage();
    MeasurePage(plain, 0, 0, s);
    PlanPage(plain, s, OcrMode::Force, plan);
    RAYOPDF_CHECK(plan.route == OcrRoute::None, 1212);
}

void TestBrokenAndVector() {
    PageSignals s;
    PageData good = NewPage();
    for (int i = 0; i < 6; ++i) AddText(good, kSentence, 72, 700 - 14.0f * i, 10);
    MeasurePage(good, good.glyphs.size(), 0, s);
    RAYOPDF_CHECK(!s.broken, 1213);
    // Unmapped or guessed codes for most glyphs.
    MeasurePage(good, good.glyphs.size(), good.glyphs.size(), s);
    RAYOPDF_CHECK(s.broken, 1213);
    // Junk (private use) characters.
    PageData junk = good;
    for (int i = 0; i < 20; ++i) {
        Glyph g = junk.glyphs[0];
        g.text = static_cast<uint32_t>(junk.text.size());
        junk.text += "\xEE\x80\x80";  // U+E000
        g.textLength = 3;
        g.flags = 0;
        junk.glyphs.push_back(g);
    }
    MeasurePage(junk, junk.glyphs.size(), 0, s);
    RAYOPDF_CHECK(s.broken, 1213);
    // Consonant soup: a font whose codes map to the wrong letters.
    PageData soup = NewPage();
    for (int i = 0; i < 10; ++i) AddText(soup, "Thx qwck brwn fxx jmps vvr thx lzz dgg nd kps rnnng fr wz ", 72, 700 - 14.0f * i, 10);
    MeasurePage(soup, soup.glyphs.size(), 0, s);
    RAYOPDF_CHECK(s.broken, 1213);
    // Vector outlines: hundreds of glyph-sized filled curves on a page without text.
    PageData outlines = NewPage();
    for (int i = 0; i < 400; ++i) {
        Shape shape;
        shape.x0 = 72 + 6.0f * (i % 70), shape.x1 = shape.x0 + 5;
        shape.y0 = 700 - 12.0f * (i / 70), shape.y1 = shape.y0 + 7;
        shape.filled = shape.curved = true;
        outlines.shapes.push_back(shape);
    }
    MeasurePage(outlines, 0, 0, s);
    RAYOPDF_CHECK(s.vectorText, 1213);
}

// ---------------------------------------------------------------- rasters

// Gray `w` x `h`: `lines` rows of character-like strokes on white, one row every `pitch` pixels.
std::string TextRaster(uint32_t w, uint32_t h, int lines, uint32_t pitch, uint32_t top = 0) {
    std::string px(size_t(w) * h, static_cast<char>(0xFF));
    const uint32_t band = pitch * 6 / 10, cell = std::max<uint32_t>(4, band * 6 / 10);
    const uint32_t stroke = std::max<uint32_t>(1, cell / 6);
    for (int l = 0; l < lines; ++l) {
        const uint32_t y0 = top + pitch / 4 + uint32_t(l) * pitch;
        for (uint32_t c = 0, x = w / 20; x + cell < w - w / 20; ++c, x += cell + cell / 4) {
            if (c % 6 == 5) continue;  // a word gap
            for (uint32_t y = y0; y < y0 + band && y < h; ++y) {
                for (uint32_t s = 0; s < stroke; ++s) {
                    px[size_t(y) * w + x + cell / 5 + s] = 20;
                    px[size_t(y) * w + x + 3 * cell / 5 + s] = 20;
                }
                if (y == y0 + band / 2)
                    for (uint32_t s = cell / 5; s < 3 * cell / 5; ++s) px[size_t(y) * w + x + s] = 20;
            }
        }
    }
    return px;
}

void TestLooksLikeText() {
    TextEvidence e;
    const std::string text = TextRaster(800, 600, 12, 48);
    RAYOPDF_CHECK(LooksLikeText(reinterpret_cast<const uint8_t*>(text.data()), 800, 600, &e), 1214);
    RAYOPDF_CHECK(e.textBands == 12 && e.midtones < 0.01f, 1214);
    // One line of text (a caption, a chart title) is no text region.
    const std::string caption = TextRaster(800, 600, 1, 48);
    RAYOPDF_CHECK(!LooksLikeText(reinterpret_cast<const uint8_t*>(caption.data()), 800, 600), 1214);
    // A photo: a gradient with noise (eval/producers/make_images.py's photo.png).
    std::string photo(1024 * 683, '\0');
    uint32_t lcg = 1;
    for (uint32_t y = 0; y < 683; ++y)
        for (uint32_t x = 0; x < 1024; ++x) {
            lcg = (1103515245u * lcg + 12345u) & 0x7FFFFFFFu;
            const int v = int(x * 200 / 1024 + y * 55 / 683) + int(lcg % 41) - 20;
            photo[size_t(y) * 1024 + x] = static_cast<char>(std::min(255, std::max(0, v)));
        }
    RAYOPDF_CHECK(!LooksLikeText(reinterpret_cast<const uint8_t*>(photo.data()), 1024, 683), 1214);
    // A bar chart: five tall bars and an axis.
    std::string chart(640 * 320, static_cast<char>(0xFF));
    for (uint32_t b = 0; b < 5; ++b)
        for (uint32_t y = 320 - 60 - 40 * b; y < 300; ++y)
            for (uint32_t x = 60 + 110 * b; x < 140 + 110 * b; ++x) chart[size_t(y) * 640 + x] = 90;
    for (uint32_t x = 40; x < 620; ++x) chart[size_t(300) * 640 + x] = 0;
    RAYOPDF_CHECK(!LooksLikeText(reinterpret_cast<const uint8_t*>(chart.data()), 640, 320), 1214);
    // An icon: a filled circle.
    std::string icon(64 * 64, static_cast<char>(0xFF));
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            if ((x - 32) * (x - 32) + (y - 32) * (y - 32) < 28 * 28) icon[size_t(y) * 64 + x] = 40;
    RAYOPDF_CHECK(!LooksLikeText(reinterpret_cast<const uint8_t*>(icon.data()), 64, 64), 1214);
    // Blank, black and tiny.
    const std::string blank(500 * 500, static_cast<char>(0xFF)), black(500 * 500, '\0');
    RAYOPDF_CHECK(!LooksLikeText(reinterpret_cast<const uint8_t*>(blank.data()), 500, 500) &&
                      !LooksLikeText(reinterpret_cast<const uint8_t*>(black.data()), 500, 500) &&
                      !LooksLikeText(reinterpret_cast<const uint8_t*>(text.data()), 20, 10),
                  1214);
}

void TestCanvas() {
    OcrCanvas canvas;
    // Scale clamped to 72-300 dpi; a large region lowered to the pixel cap.
    RAYOPDF_CHECK(StartCanvas(0, 0, 612, 792, 100.0f, canvas) && canvas.scale == kOcrMaxScale &&
                      canvas.width == 2550 && canvas.height == 3300,
                  1220);
    RAYOPDF_CHECK(StartCanvas(0, 0, 612, 792, 0.1f, canvas) && canvas.scale == kOcrMinScale && canvas.width == 612, 1220);
    RAYOPDF_CHECK(StartCanvas(0, 0, 14400, 14400, 4.0f, canvas) &&
                      uint64_t(canvas.width) * canvas.height <= kOcrMaxCanvasPixels,
                  1220);
    RAYOPDF_CHECK(!StartCanvas(0, 0, 0, 792, 1.0f, canvas) && !StartCanvas(10, 0, 5, 792, 1.0f, canvas), 1220);
    ImageBox image = Placed(0, 0, 612, 792, 1275, 1650);
    RAYOPDF_CHECK(std::fabs(ImageScale(image) - 1275.0f / 612.0f) < 1e-3f, 1220);

    // Drawn upright: the image's first row is the top of the canvas.
    RAYOPDF_CHECK(StartCanvas(0, 0, 100, 50, 1.0f, canvas), 1221);
    std::string img(4 * 2, static_cast<char>(0xFF));
    img[0] = 0;  // top left pixel
    const float upright[6] = {100, 0, 0, 50, 0, 0};
    DrawGray(canvas, reinterpret_cast<const uint8_t*>(img.data()), 4, 2, upright);
    RAYOPDF_CHECK(uint8_t(canvas.pixels[0]) == 0 && uint8_t(canvas.pixels[24]) == 0 && uint8_t(canvas.pixels[25]) == 255 &&
                      uint8_t(canvas.pixels[size_t(24) * 100]) == 0 && uint8_t(canvas.pixels[size_t(25) * 100]) == 255,
                  1221);
    // Rotated a quarter turn (an image drawn sideways): its top-left pixel lands bottom left.
    RAYOPDF_CHECK(StartCanvas(0, 0, 50, 100, 1.0f, canvas), 1221);
    const float turned[6] = {0, 100, -50, 0, 50, 0};
    DrawGray(canvas, reinterpret_cast<const uint8_t*>(img.data()), 4, 2, turned);
    RAYOPDF_CHECK(uint8_t(canvas.pixels[size_t(99) * 50 + 0]) == 0 && uint8_t(canvas.pixels[size_t(99) * 50 + 30]) == 255 &&
                      uint8_t(canvas.pixels[0]) == 255,
                  1221);
    // Darker wins where images overlap.
    std::string mid(4, static_cast<char>(128));
    const float whole[6] = {50, 0, 0, 100, 0, 0};
    DrawGray(canvas, reinterpret_cast<const uint8_t*>(mid.data()), 2, 2, whole);
    RAYOPDF_CHECK(uint8_t(canvas.pixels[size_t(99) * 50]) == 0 && uint8_t(canvas.pixels[0]) == 128, 1221);
}

P::Line OcrLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, const std::string& text, uint16_t conf = 9000) {
    P::Line line;
    line.x0 = x0, line.y0 = y0, line.x1 = x1, line.y1 = y1;
    line.text = text;
    line.conf = conf;
    const size_t n = P::CodePoints(text);
    for (size_t k = 0; k < n; ++k)
        line.chars.push_back({static_cast<int32_t>(x0 + (x1 - x0) * int64_t(k) / int64_t(n)),
                              static_cast<int32_t>(x0 + (x1 - x0) * int64_t(k + 1) / int64_t(n))});
    return line;
}

void TestOcrGlyphs() {
    OcrCanvas canvas;
    RAYOPDF_CHECK(StartCanvas(0, 0, 612, 792, 2.0f, canvas), 1222);
    P::Response response;
    // Given bottom first; one below the confidence floor; one outside the canvas.
    response.lines.push_back(OcrLine(144, 300, 900, 330, "Second line of the scan."));
    response.lines.push_back(OcrLine(144, 200, 900, 230, "First line \xC5\xBC\xC3\xB3\xC5\x82w."));
    response.lines.push_back(OcrLine(144, 400, 900, 430, "noise", 3000));
    response.lines.push_back(OcrLine(5000, 400, 6000, 430, "outside"));
    std::vector<PageData> pages(1, NewPage());
    const uint32_t added = AddOcrGlyphs(canvas, response, pages[0]);
    RAYOPDF_CHECK(added == 14 + 20, 1222);
    const Glyph& first = pages[0].glyphs[0];
    RAYOPDF_CHECK(pages[0].GlyphText(first) == "F" && (first.flags & kGlyphOcr) && (first.flags & kGlyphInvisible) &&
                      std::fabs(first.x0 - 72) < 0.01f && std::fabs(first.y1 - (792 - 100)) < 0.01f &&
                      std::fabs(first.y0 - (792 - 115)) < 0.01f,
                  1222);
    RAYOPDF_CHECK((pages[0].glyphs[5].flags & kGlyphSpace) && pages[0].GlyphText(pages[0].glyphs[11]) == "\xC5\xBC", 1222);
    std::string md;
    ConvertReport report;
    std::vector<PageTextQuality> quality(1);
    ConvertPages(pages, quality, {}, false, md, report);
    Show("ocr glyphs", md);
    RAYOPDF_CHECK(Contains(md, "First line \xC5\xBC\xC3\xB3\xC5\x82w.") && Contains(md, "Second line of the scan.") &&
                      md.find("First") < md.find("Second") && !Contains(md, "noise") && !Contains(md, "outside") &&
                      !Contains(md, "no extractable text"),
                  1222);

    // Beside visible text, the OCR layer stays (an image region on a digital page), while an
    // ordinary invisible layer leaves.
    std::vector<PageData> mixed(1, NewPage());
    AddText(mixed[0], "Visible heading text of the page.", 72, 720, 12);
    AddText(mixed[0], "Hidden duplicate", 72, 600, 12, kGlyphInvisible);
    P::Response region;
    region.lines.push_back(OcrLine(144, 400, 700, 430, "Text inside a figure."));
    AddOcrGlyphs(canvas, region, mixed[0]);
    std::vector<PageTextQuality> q1(1);
    ConvertPages(mixed, q1, {}, false, md, report);
    Show("ocr beside text", md);
    RAYOPDF_CHECK(Contains(md, "Visible heading text") && Contains(md, "Text inside a figure.") &&
                      !Contains(md, "Hidden duplicate"),
                  1223);
    // An OCR failure on a page without text says so.
    std::vector<PageData> empty(1, NewPage());
    std::vector<PageTextQuality> q2(1);
    q2[0].ocrFailed = true;
    ConvertPages(empty, q2, {}, false, md, report);
    RAYOPDF_CHECK(Contains(md, "<!-- rayopdf: page 1 has no extractable text; OCR failed -->"), 1223);
}

// ---------------------------------------------------------------- the client

OcrOptions MockOptions(uint32_t requestMs = 20000, uint32_t handshakeMs = 20000) {
    OcrOptions options;
    options.addon = kMock;
    options.requestTimeoutMs = requestMs;
    options.handshakeTimeoutMs = handshakeMs;
    return options;
}

using Clock = std::chrono::steady_clock;

double SecondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// A page request of `raster` (`w` x `h`).
OcrStatus Ask(OcrSession& session, const std::string& raster, uint32_t w, uint32_t h, P::Response& response) {
    P::Request request;
    request.width = w, request.height = h, request.dpi = 150;
    return session.Recognize(request, raster, response);
}

void TestDiscovery() {
    SetEnv("RAYOPDF_OCR", nullptr);
    RAYOPDF_CHECK(FindOcrAddon(kNoAddon).empty() && FindOcrAddon(kMock) == kMock, 1230);
    SetEnv("RAYOPDF_OCR", kMock);
    RAYOPDF_CHECK(FindOcrAddon(std::string()) == kMock && FindOcrAddon(kNoAddon).empty(), 1230);
    SetEnv("RAYOPDF_OCR", kNoAddon);  // a variable naming nothing is not replaced by a later step
    RAYOPDF_CHECK(FindOcrAddon(std::string()).empty(), 1230);
    SetEnv("RAYOPDF_OCR", nullptr);
    OcrOptions missing;
    missing.addon = kNoAddon;
    OcrSession session(missing);
    P::Response response;
    RAYOPDF_CHECK(session.Ensure() == OcrStatus::Missing && Ask(session, "x", 1, 1, response) == OcrStatus::Missing, 1238);
}

void TestClient() {
    const std::string raster = TextRaster(400, 300, 5, 48);
    P::Response response;
    Mode("ok");
    {
        OcrSession session(MockOptions());
        RAYOPDF_CHECK(session.Ensure() == OcrStatus::Ok && session.Handshake().version == "0.0.0-mock" &&
                          session.Handshake().protocol == 1 && session.Handshake().models.size() == 1,
                      1231);
        RAYOPDF_CHECK(Ask(session, raster, 400, 300, response) == OcrStatus::Ok && response.id == 1 &&
                          response.lines.size() == 5 && response.lines[0].text == "Mock line 5 of the scanned page",
                      1231);
        P::Response second;
        RAYOPDF_CHECK(Ask(session, raster, 400, 300, second) == OcrStatus::Ok && second.id == 2 &&
                          SameLines(second, response),
                      1231);
        P::Request lines;
        lines.op = P::Op::Lines;
        lines.lines = {{4, 2}, {3, 3}};
        RAYOPDF_CHECK(session.Recognize(lines, std::string(17, 'x'), response) == OcrStatus::Ok &&
                          response.lines.size() == 2 && response.lines[1].x1 == 3 && response.lines[1].text == "word",
                      1231);
        session.Quit();
        RAYOPDF_CHECK(!session.Running(), 1231);
    }
    // Crash: reported, then a new process on the next request.
    Mode("crash");
    {
        OcrSession session(MockOptions());
        RAYOPDF_CHECK(Ask(session, raster, 400, 300, response) == OcrStatus::Crashed && !session.Running(), 1232);
        Mode("ok");
        RAYOPDF_CHECK(Ask(session, raster, 400, 300, response) == OcrStatus::Ok && response.id == 1, 1232);
    }
    // Hang: killed after the request timeout.
    Mode("hang");
    {
        OcrSession session(MockOptions(400));
        const Clock::time_point start = Clock::now();
        RAYOPDF_CHECK(Ask(session, raster, 400, 300, response) == OcrStatus::Timeout && !session.Running(), 1233);
        const double took = SecondsSince(start);
        RAYOPDF_CHECK(took >= 0.35 && took < 15, 1233);
    }
    // Garbage and a header that is no JSON.
    for (const char* mode : {"garbage", "bad-json"}) {
        Mode(mode);
        OcrSession session(MockOptions());
        RAYOPDF_CHECK(Ask(session, raster, 400, 300, response) == OcrStatus::Garbage && !session.Running(), 1234);
    }
    Mode("wrong-id");
    {
        OcrSession session(MockOptions());
        RAYOPDF_CHECK(Ask(session, raster, 400, 300, response) == OcrStatus::WrongId && !session.Running(), 1235);
    }
    Mode("error");
    {
        OcrSession session(MockOptions());
        RAYOPDF_CHECK(Ask(session, raster, 400, 300, response) == OcrStatus::Refused && session.Running() &&
                          response.error == "engine-failed",
                      1236);
        RAYOPDF_CHECK(Ask(session, raster, 400, 300, response) == OcrStatus::Refused && response.id == 2, 1236);
    }
    // Start-up failures stay failures for the session, without a second attempt.
    for (const char* mode : {"crash-start", "hang-start", "bad-handshake"}) {
        Mode(mode);
        OcrSession session(MockOptions(20000, 500));
        RAYOPDF_CHECK(session.Ensure() == OcrStatus::StartFailed && !session.Running(), 1237);
        const Clock::time_point start = Clock::now();
        RAYOPDF_CHECK(Ask(session, raster, 400, 300, response) == OcrStatus::StartFailed && SecondsSince(start) < 0.2,
                      1237);
    }
    Mode("ok");
}

// ---------------------------------------------------------------- whole conversions

// A document of US Letter pages: "t" a page of text, "s" a scanned page (a gray image of five
// lines filling the page), "r" text with a text-bearing figure, "c" text with a chart, "o" a scan
// with an invisible OCR layer.
std::string MakeDocument(const std::string& kinds) {
    Test::PdfBuilder b;
    const int catalog = b.Reserve();
    const int pagesObject = b.Reserve();
    const std::string gray = "/Type /XObject /Subtype /Image /ColorSpace /DeviceGray /BitsPerComponent 8";
    const int scan = b.AddFlateStream(gray + " /Width 612 /Height 792", TextRaster(612, 792, 5, 60, 100));
    const int figure = b.AddFlateStream(gray + " /Width 600 /Height 400", TextRaster(600, 400, 6, 60));
    std::string chartPixels(600 * 300, static_cast<char>(0xFF));
    for (uint32_t k = 0; k < 5; ++k)
        for (uint32_t y = 280 - 40 * k; y < 290; ++y)
            for (uint32_t x = 50 + 110 * k; x < 120 + 110 * k; ++x) chartPixels[size_t(y) * 600 + x] = 60;
    const int chart = b.AddFlateStream(gray + " /Width 600 /Height 300", chartPixels);
    const std::string resources = "/Resources << /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >> "
                                  "/XObject << /Sc " + std::to_string(scan) + " 0 R /Fi " + std::to_string(figure) +
                                  " 0 R /Ch " + std::to_string(chart) + " 0 R >> >>";
    std::string kids;
    int number = 0;
    for (char kind : kinds) {
        ++number;
        std::string content;
        const std::string tag = "Page " + std::to_string(number) + " ";
        if (kind == 't' || kind == 'r' || kind == 'c') {
            for (int i = 0; i < 3; ++i)
                content += "BT /F1 11 Tf 72 " + std::to_string(720 - 16 * i) + " Td (" + tag +
                           "body text that a reader extracts from the text layer.) Tj ET\n";
        }
        if (kind == 's' || kind == 'o') content += "q 612 0 0 792 0 0 cm /Sc Do Q\n";
        if (kind == 'o')
            for (int i = 0; i < 2; ++i)
                content += "BT 3 Tr /F1 11 Tf 72 " + std::to_string(400 - 16 * i) + " Td (" + tag +
                           "an invisible OCR layer over the scan, long enough.) Tj ET\n";
        if (kind == 'r') content += "q 450 0 0 300 72 250 cm /Fi Do Q\n";
        if (kind == 'c') content += "q 450 0 0 225 72 250 cm /Ch Do Q\n";
        const int stream = b.AddFlateStream("", content);
        const int page = b.Add("<< /Type /Page /Parent " + std::to_string(pagesObject) + " 0 R /MediaBox [0 0 612 792] " +
                               resources + " /Contents " + std::to_string(stream) + " 0 R >>");
        kids += std::to_string(page) + " 0 R ";
    }
    b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pagesObject) + " 0 R >>");
    b.Set(pagesObject, "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(kinds.size()) + " >>");
    return b.Build(catalog);
}

std::string ConvertWith(const std::string& pdf, const ConvertOptions& options, ConvertReport& report) {
    std::unique_ptr<RayoPdf::Document> doc;
    RAYOPDF_CHECK(RayoPdf::Document::Open(pdf, "", doc) == OpenStatus::Ok, 1240);
    std::string md;
    Convert(*doc, options, md, report);
    return md;
}

void TestConvertWithoutAddon() {
    const std::string pdf = MakeDocument("tstcro");
    ConvertOptions off, missing, force;
    off.ocr.mode = OcrMode::Off;
    missing.ocr.addon = kNoAddon;
    force.ocr.mode = OcrMode::Force;
    force.ocr.addon = kNoAddon;
    ConvertReport a, b, c;
    const std::string mdOff = ConvertWith(pdf, off, a);
    const std::string mdMissing = ConvertWith(pdf, missing, b);
    const std::string mdForce = ConvertWith(pdf, force, c);
    Show("no add-on", mdMissing);
    RAYOPDF_CHECK(mdOff == mdMissing && mdOff == mdForce && Contains(mdOff, "<!-- rayopdf: page 2 has no extractable text -->"),
                  1240);
    RAYOPDF_CHECK(a.ocrPagesRouted == 0 && !a.ocrAddonMissing && b.ocrPagesRouted == 1 && b.ocrAddonMissing &&
                      b.ocrPages == 0 && b.ocrFailures == 0,
                  1240);
    // Force: the scan, the scan with its OCR layer, and the two figures are routed.
    RAYOPDF_CHECK(c.ocrPagesRouted == 4 && c.ocrAddonMissing, 1240);
}

void TestConvertWithMock() {
    Mode("ok");
    const std::string pdf = MakeDocument("tstc");
    ConvertOptions options;
    options.ocr = MockOptions();
    ConvertReport report;
    const std::string md = ConvertWith(pdf, options, report);
    Show("mock add-on", md);
    const size_t p1 = md.find("Page 1 body"), l1 = md.find("Mock line 1"), l5 = md.find("Mock line 5"),
                 p3 = md.find("Page 3 body");
    RAYOPDF_CHECK(p1 != std::string::npos && l1 != std::string::npos && l5 != std::string::npos && p3 != std::string::npos &&
                      p1 < l1 && l1 < l5 && l5 < p3 && !Contains(md, "Mock line 6") && !Contains(md, "no extractable text"),
                  1241);
    // The chart is checked and left alone: only the scan is routed.
    RAYOPDF_CHECK(report.ocrPagesRouted == 1 && report.ocrPages == 1 && report.ocrFailures == 0 && !report.ocrAddonMissing,
                  1241);

    // A text-bearing figure on a digital page is read beside the page's text.
    ConvertReport regions;
    const std::string withFigure = ConvertWith(MakeDocument("r"), options, regions);
    Show("figure", withFigure);
    RAYOPDF_CHECK(Contains(withFigure, "Page 1 body") && Contains(withFigure, "Mock line 1") &&
                      Contains(withFigure, "Mock line 6") && regions.ocrPagesRouted == 1 && regions.ocrPages == 1,
                  1245);

    // Force: an invisible OCR layer gives way to the add-on's text.
    ConvertOptions force = options;
    force.ocr.mode = OcrMode::Force;
    ConvertReport forced;
    const std::string replaced = ConvertWith(MakeDocument("o"), force, forced);
    RAYOPDF_CHECK(Contains(replaced, "Mock line 3") && !Contains(replaced, "invisible OCR layer") && forced.ocrPages == 1,
                  1244);
    ConvertReport kept;
    const std::string auto_ = ConvertWith(MakeDocument("o"), options, kept);
    RAYOPDF_CHECK(Contains(auto_, "invisible OCR layer") && !Contains(auto_, "Mock line") && kept.ocrPagesRouted == 0,
                  1244);

    // Low confidence: no text, the page keeps its comment.
    Mode("low-conf");
    ConvertReport low;
    const std::string unsure = ConvertWith(MakeDocument("s"), options, low);
    RAYOPDF_CHECK(Contains(unsure, "has no extractable text -->") && low.ocrPages == 0 && low.ocrFailures == 0, 1241);
    Mode("ok");
}

void TestConvertFailures() {
    // A crash on the first scan: no further page is sent, every scan says OCR failed, and the text
    // pages are as without an add-on.
    const std::string pdf = MakeDocument("tsts");
    ConvertOptions off;
    off.ocr.mode = OcrMode::Off;
    ConvertReport offReport;
    const std::string mdOff = ConvertWith(pdf, off, offReport);
    for (const char* mode : {"crash", "hang", "garbage", "wrong-id", "error", "bad-handshake"}) {
        Mode(mode);
        ConvertOptions options;
        options.ocr = MockOptions(400, 500);
        ConvertReport report;
        const std::string md = ConvertWith(pdf, options, report);
        std::string expected = mdOff;
        for (const char* page : {"2", "4"}) {
            const std::string from = std::string("page ") + page + " has no extractable text -->";
            const size_t at = expected.find(from);
            RAYOPDF_CHECK(at != std::string::npos, 1242);
            expected.replace(at, from.size(), std::string("page ") + page + " has no extractable text; OCR failed -->");
        }
        Show(mode, md);
        RAYOPDF_CHECK(md == expected, 1242);
        RAYOPDF_CHECK(report.ocrPagesRouted == 2 && report.ocrFailures == 2 && report.ocrPages == 0 &&
                          !report.ocrAddonMissing,
                      1242);
    }
    Mode("ok");
}

void TestWorkers() {
    // Documents converted on three threads, each with its add-on kept from document to document,
    // give what one session gives converting them in order.
    Mode("ok");
    const std::vector<std::string> kinds = {"ts", "st", "tsts", "s", "tc", "ss"};
    std::vector<std::string> pdfs;
    for (const std::string& k : kinds) pdfs.push_back(MakeDocument(k));
    std::vector<std::string> expected(pdfs.size());
    {
        OcrSession session(MockOptions());
        for (size_t i = 0; i < pdfs.size(); ++i) {
            ConvertOptions options;
            options.ocr = MockOptions();
            options.ocr.session = &session;
            ConvertReport report;
            expected[i] = ConvertWith(pdfs[i], options, report);
        }
    }
    std::vector<std::string> got(pdfs.size());
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 3; ++t) {
        threads.emplace_back([&, t] {
            OcrSession session(MockOptions());
            for (size_t i = t; i < pdfs.size(); i += 3) {
                ConvertOptions options;
                options.ocr = MockOptions();
                options.ocr.session = &session;
                ConvertReport report;
                got[i] = ConvertWith(pdfs[i], options, report);
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    for (size_t i = 0; i < pdfs.size(); ++i)
        RAYOPDF_CHECK(got[i] == expected[i] && Contains(got[i], "Mock line 1") == (kinds[i].find('s') != std::string::npos),
                      1243);
}

} // namespace

int main() {
    TestProtocolRoundTrips();
    TestFrameErrors();
    TestRequestErrors();
    TestResponseErrors();
    TestRouting();
    TestBrokenAndVector();
    TestLooksLikeText();
    TestCanvas();
    TestOcrGlyphs();
    TestDiscovery();
    TestClient();
    TestConvertWithoutAddon();
    TestConvertWithMock();
    TestConvertFailures();
    TestWorkers();
    std::puts("test_layout_ocr: ok");
    return 0;
}
