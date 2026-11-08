// SPDX-License-Identifier: Apache-2.0
// Core tests of src/ocr, the OCR add-on's own code without an inference runtime (codes 1250-1299
// and 1500-1549): the dictionary and CTC decoding, resampling, DB box detection and line order, the
// PP-OCR engine (with and without a first pass) and the protocol server on a model-free stand-in
// runtime (ocr_fake_runtime.h), the Tesseract TSV reader, and the model folder's text files.
#include "check.h"
#include "common/ocr_protocol.h"
#include "ocr/detect.h"
#include "ocr/engine.h"
#include "ocr/models.h"
#include "ocr/raster.h"
#include "ocr/recognize.h"
#include "ocr/server.h"
#include "ocr/tesseract.h"
#include "ocr_fake_runtime.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace RayoPdf;
using namespace RayoPdf::Ocr;

// "a", "b", "c", "d": classes blank, a, b, c, d, space.
Dictionary TestDictionary() {
    Dictionary dictionary;
    RAYOPDF_CHECK(dictionary.Load("a\nb\r\nc\nd"), 1250);
    return dictionary;
}

void TestDictionary_() {
    const Dictionary dictionary = TestDictionary();
    RAYOPDF_CHECK(dictionary.Classes() == 6 && dictionary.Entry(1) == "a" && dictionary.Entry(2) == "b" &&
                      dictionary.Entry(4) == "d" && dictionary.Entry(5) == " " && dictionary.Entry(0).empty() &&
                      dictionary.Entry(6).empty(),
                  1251);
    Dictionary other;
    RAYOPDF_CHECK(!other.Load("") && !other.Load("a\n\nb\n") && !other.Load("\xff\n") &&
                      !other.Load(std::string(kMaxEntryBytes + 1, 'x')),
                  1252);
    RAYOPDF_CHECK(other.Load("\xc3\xa9\n\xe4\xb8\xad\n") && other.Classes() == 4 && other.Entry(2) == "\xe4\xb8\xad", 1253);
}

void TestCtc() {
    const Dictionary dictionary = TestDictionary();
    // Steps: a a blank a b space space c blank -> "a ab c" with repeats merged, blank splitting a|a.
    const uint32_t classes = 6, steps = 10;
    const uint32_t best[steps] = {1, 1, 0, 1, 2, 5, 5, 3, 0, 5};
    std::vector<float> probs(size_t(steps) * classes, 0.02f);
    for (uint32_t t = 0; t < steps; ++t) probs[size_t(t) * classes + best[t]] = 0.9f;
    probs[size_t(7) * classes + 3] = 0.5f;  // c is less sure
    std::vector<DecodedChar> decoded;
    DecodeCtc(probs.data(), steps, classes, decoded);
    RAYOPDF_CHECK(decoded.size() == 6 && decoded[0].cls == 1 && decoded[0].firstStep == 0 && decoded[0].lastStep == 1 &&
                      decoded[1].cls == 1 && decoded[1].firstStep == 3 && decoded[3].cls == 5 &&
                      decoded[3].lastStep == 6 && decoded[5].cls == 5,
                  1255);
    CropText text;
    AssembleText(decoded, dictionary, steps, 100.0f, text);
    // The trailing space goes; "aab c" keeps its inner space.
    RAYOPDF_CHECK(text.text == "aab c" && text.x0.size() == 5 && text.x1.size() == 5, 1256);
    bool ordered = text.x0[0] == 0.0f && text.x1[4] == 80.0f;
    for (size_t i = 0; i < 5; ++i) ordered = ordered && text.x0[i] < text.x1[i] && (i == 0 || text.x0[i] == text.x1[i - 1]);
    RAYOPDF_CHECK(ordered, 1257);
    // Mean of all kept characters' best probabilities: (0.9 * 5 + 0.5) / 6.
    RAYOPDF_CHECK(text.conf == ToConf((0.9 * 5 + 0.5) / 6), 1258);
    RAYOPDF_CHECK(ToConf(-1) == 0 && ToConf(2) == OcrProtocol::kConfScale && ToConf(0.12346) == 1235, 1259);
    std::vector<DecodedChar> spaces = {{5, 0, 0, 0.9f, false}, {5, 1, 1, 0.9f, false}};
    AssembleText(spaces, dictionary, 2, 10.0f, text);
    const bool onlySpaces = text.text.empty() && text.x0.empty() && text.conf == 0;
    // Space recovery: "a", two blank steps with space probability 0.13 and 0.26, "b" -> "a b" at 0.25
    // (1 - 0.87 * 0.74 = 0.356), "ab" at 0.5; the inserted space does not count in the confidence.
    std::vector<float> gap(size_t(4) * classes, 0.0f);
    const float rows[4][2] = {{1, 0.95f}, {0, 0.87f}, {0, 0.74f}, {2, 0.95f}};  // class, its probability
    for (uint32_t t = 0; t < 4; ++t) {
        gap[size_t(t) * classes + uint32_t(rows[t][0])] = rows[t][1];
        gap[size_t(t) * classes + 5] = 1.0f - rows[t][1];
    }
    gap[size_t(0) * classes + 5] = 0.0f;
    gap[size_t(3) * classes + 5] = 0.0f;
    DecodeCtc(gap.data(), 4, classes, decoded);
    std::vector<DecodedChar> strict = decoded;
    InsertSpaces(gap.data(), classes, 5, 0.5f, strict);
    InsertSpaces(gap.data(), classes, 5, 0.25f, decoded);
    AssembleText(decoded, dictionary, 4, 40.0f, text);
    RAYOPDF_CHECK(onlySpaces && strict.size() == 2 && decoded.size() == 3 && decoded[1].inserted &&
                      decoded[1].firstStep == 2 && text.text == "a b" && text.conf == ToConf(0.95),
                  1254);
}

void TestRaster() {
    // Identity, 2:1 box shrink, and bilinear growth.
    const uint8_t pixels[16] = {0, 100, 200, 250, 10, 110, 210, 240, 20, 30, 40, 50, 60, 70, 80, 90};
    const GrayView image{pixels, 4, 4};
    ResizeScratch scratch;
    std::vector<uint8_t> out;
    Resize(image, 4, 4, scratch, out);
    RAYOPDF_CHECK(out.size() == 16 && std::memcmp(out.data(), pixels, 16) == 0, 1260);
    Resize(image, 2, 2, scratch, out);
    RAYOPDF_CHECK(out.size() == 4 && out[0] == 55 && out[1] == 225 && out[2] == 45 && out[3] == 65, 1261);
    const uint8_t two[2] = {0, 200};
    Resize(GrayView{two, 2, 1}, 4, 1, scratch, out);
    RAYOPDF_CHECK(out[0] == 0 && out[1] == 50 && out[2] == 150 && out[3] == 200, 1262);
    // An axis-aligned rectangle centred on pixel centres samples the pixels themselves.
    RotatedRect rect;
    rect.cx = 1.5f;
    rect.cy = 0.5f;
    rect.width = 2;
    rect.height = 2;
    SampleRect(image, rect, 2, 2, out);
    RAYOPDF_CHECK(out[0] == 100 && out[1] == 200 && out[2] == 110 && out[3] == 210, 1263);
    std::vector<uint8_t> grid = {1, 2, 3, 4, 5, 6}, turned;  // 3 wide, 2 tall
    RotateCounterClockwise(grid, 3, 2, turned);
    RAYOPDF_CHECK(turned == std::vector<uint8_t>({3, 6, 2, 5, 1, 4}), 1264);
}

// A probability map with filled rectangles [x0, x1) x [y0, y1) of probability `p`.
struct Map {
    uint32_t width, height;
    std::vector<float> prob;
    Map(uint32_t w, uint32_t h) : width(w), height(h), prob(size_t(w) * h, 0.0f) {}
    void Fill(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, float p) {
        for (uint32_t y = y0; y < y1; ++y) {
            for (uint32_t x = x0; x < x1; ++x) prob[size_t(y) * width + x] = p;
        }
    }
};

void TestDetect() {
    Map map(200, 100);
    map.Fill(10, 10, 110, 30, 0.9f);   // a line
    map.Fill(120, 12, 180, 28, 0.9f);  // the same row, further right
    map.Fill(10, 60, 60, 80, 0.9f);    // a second row
    map.Fill(100, 60, 102, 90, 0.9f);  // too thin (2 pixels)
    map.Fill(150, 60, 190, 80, 0.3f);  // above thresh, below boxThresh
    DetectParams params;
    DetectScratch scratch;
    std::vector<TextBox> boxes;
    DetectBoxes(map.prob.data(), map.width, map.height, 2.0f, 2.0f, params, scratch, boxes);
    RAYOPDF_CHECK(boxes.size() == 3, 1265);
    SortIntoLines(boxes);
    // Raster order of first pixels is already row order here; sizes: 99 x 19 pixel centres, grown by
    // d = 99 * 19 * 1.4 / (2 * 118) on each side, then doubled to page pixels.
    const float d = 99.0f * 19.0f * 1.4f / (2.0f * 118.0f);
    const RotatedRect& first = boxes[0].rect;
    RAYOPDF_CHECK(std::fabs(first.cx - 119.0f) < 0.01f && std::fabs(first.cy - 39.0f) < 0.01f &&
                      std::fabs(first.width - 2 * (99 + 2 * d)) < 0.05f && std::fabs(first.height - 2 * (19 + 2 * d)) < 0.05f &&
                      first.ux == 1.0f && first.uy == 0.0f && boxes[0].score > 0.89f,
                  1266);
    RAYOPDF_CHECK(boxes[1].rect.cx > first.cx && boxes[2].rect.cy > first.cy, 1267);
    const PixelBox bounds = Bounds(first, 400, 200);
    RAYOPDF_CHECK(bounds.x0 == 0 && bounds.x1 == int32_t(std::floor(119.0f + first.width / 2)) + 1 && bounds.y0 == 0 &&
                      bounds.y1 > 59,
                  1268);
    // Out-of-order input: the right box first, then the lower row, then the left box.
    std::vector<TextBox> shuffled = {boxes[2], boxes[1], boxes[0]};
    SortIntoLines(shuffled);
    RAYOPDF_CHECK(shuffled[0].rect.cx == boxes[0].rect.cx && shuffled[1].rect.cx == boxes[1].rect.cx &&
                      shuffled[2].rect.cy == boxes[2].rect.cy,
                  1269);

    // A bar rotated by about 10 degrees keeps its angle (the width axis stays near horizontal).
    Map tilted(300, 200);
    for (uint32_t y = 0; y < 200; ++y) {
        for (uint32_t x = 0; x < 300; ++x) {
            const float dx = x - 150.0f, dy = y - 100.0f;
            const float a = dx * 0.9848f + dy * 0.1736f, b = -dx * 0.1736f + dy * 0.9848f;
            if (std::fabs(a) <= 100 && std::fabs(b) <= 10) tilted.prob[size_t(y) * 300 + x] = 0.95f;
        }
    }
    DetectBoxes(tilted.prob.data(), 300, 200, 1.0f, 1.0f, params, scratch, boxes);
    RAYOPDF_CHECK(boxes.size() == 1 && std::fabs(boxes[0].rect.uy - 0.1736f) < 0.03f && boxes[0].rect.ux > 0.98f &&
                      std::fabs(boxes[0].rect.cx - 150) < 1.5f && std::fabs(boxes[0].rect.cy - 100) < 1.5f,
                  1270);
    // The candidate limit: only the first component in raster order is examined.
    params.maxCandidates = 1;
    DetectBoxes(map.prob.data(), map.width, map.height, 1.0f, 1.0f, params, scratch, boxes);
    RAYOPDF_CHECK(boxes.size() == 1 && boxes[0].rect.cx < 70, 1271);
    // An empty map and an all-text map.
    Map empty(64, 32), full(64, 32);
    full.Fill(0, 0, 64, 32, 1.0f);
    params.maxCandidates = 3000;
    DetectBoxes(empty.prob.data(), 64, 32, 1.0f, 1.0f, params, scratch, boxes);
    const bool none = boxes.empty();
    DetectBoxes(full.prob.data(), 64, 32, 1.0f, 1.0f, params, scratch, boxes);
    RAYOPDF_CHECK(none && boxes.size() == 1, 1272);
}

// A white page with black bars [x0, x1) x [y0, y1).
std::vector<uint8_t> Page(uint32_t width, uint32_t height, const std::vector<std::vector<uint32_t>>& bars) {
    std::vector<uint8_t> page(size_t(width) * height, 255);
    for (const auto& bar : bars) {
        for (uint32_t y = bar[1]; y < bar[3]; ++y) {
            for (uint32_t x = bar[0]; x < bar[2]; ++x) page[size_t(y) * width + x] = 0;
        }
    }
    return page;
}

void TestEngine() {
    const Dictionary dictionary = TestDictionary();
    Testing::FakeRuntime runtime(dictionary.Classes());
    PpOcrSettings settings;
    PpOcrEngine engine(runtime, dictionary, settings);
    const std::vector<uint8_t> page = Page(400, 200, {{40, 40, 360, 70}, {40, 120, 200, 150}});
    std::vector<OcrProtocol::Line> lines;
    RAYOPDF_CHECK(engine.Page(GrayView{page.data(), 400, 200}, 300, lines) == EngineStatus::Ok, 1273);
    RAYOPDF_CHECK(lines.size() == 2 && lines[0].y0 < 40 && lines[0].y1 > 70 && lines[0].x0 < 40 && lines[0].x1 > 360 &&
                      lines[1].y0 > 70 && lines[1].x1 < 260,
                  1274);
    bool spans = true;
    for (const OcrProtocol::Line& line : lines) {
        spans = spans && !line.text.empty() && line.chars.size() == OcrProtocol::CodePoints(line.text) && line.conf > 0;
        for (const OcrProtocol::CharSpan& span : line.chars) {
            spans = spans && span.x0 >= line.x0 && span.x1 <= line.x1 && span.x0 <= span.x1;
        }
    }
    RAYOPDF_CHECK(spans && lines[0].text.find('c') != std::string::npos, 1275);
    // Deterministic: the same page again gives the same lines.
    std::vector<OcrProtocol::Line> again;
    engine.Page(GrayView{page.data(), 400, 200}, 300, again);
    bool same = again.size() == lines.size();
    for (size_t i = 0; same && i < lines.size(); ++i) {
        same = again[i].text == lines[i].text && again[i].x0 == lines[i].x0 && again[i].conf == lines[i].conf &&
               again[i].chars.size() == lines[i].chars.size();
    }
    RAYOPDF_CHECK(same, 1276);

    // A line image of height 48 is read as it is: 16 black columns, 16 white, 16 gray 64, then white
    // up to 320 (no padding) -> "c", blank, "b".
    std::vector<uint8_t> line(size_t(320) * 48, 255);
    for (uint32_t y = 0; y < 48; ++y) {
        for (uint32_t x = 0; x < 16; ++x) line[size_t(y) * 320 + x] = 0;
        for (uint32_t x = 32; x < 48; ++x) line[size_t(y) * 320 + x] = 64;
    }
    OcrProtocol::Line result;
    RAYOPDF_CHECK(engine.Line(GrayView{line.data(), 320, 48}, result) == EngineStatus::Ok, 1277);
    RAYOPDF_CHECK(result.text == "cb" && result.x0 == 0 && result.y0 == 0 && result.x1 == 320 && result.y1 == 48 &&
                      result.chars.size() == 2 && result.chars[0].x0 == 0 && result.chars[1].x1 == 48,
                  1278);
    // Empty images and runtime failures.
    RAYOPDF_CHECK(engine.Page(GrayView{nullptr, 0, 0}, 0, lines) == EngineStatus::Ok && lines.empty() &&
                      engine.Line(GrayView{nullptr, 0, 5}, result) == EngineStatus::Ok && result.text.empty(),
                  1279);
    runtime.failDetect = true;
    RAYOPDF_CHECK(engine.Page(GrayView{page.data(), 400, 200}, 300, lines) == EngineStatus::Failed, 1280);
    runtime.failDetect = false;
    runtime.failRecognize = true;
    RAYOPDF_CHECK(engine.Line(GrayView{line.data(), 320, 48}, result) == EngineStatus::Failed, 1281);
    // A recogniser whose class count differs from the dictionary's fails instead of misreading.
    Testing::FakeRuntime wrong(dictionary.Classes() + 1);
    PpOcrEngine mismatched(wrong, dictionary, settings);
    RAYOPDF_CHECK(mismatched.Line(GrayView{line.data(), 320, 48}, result) == EngineStatus::Failed, 1282);
}

// Counts the recognitions a runtime runs.
class CountingRuntime final : public Runtime {
public:
    explicit CountingRuntime(Runtime& inner) : inner_(inner) {}
    uint32_t recognized = 0;
    RuntimeStatus Detect(const float* planes, uint32_t width, uint32_t height, std::vector<float>& prob) override {
        return inner_.Detect(planes, width, height, prob);
    }
    RuntimeStatus Recognize(const float* planes, uint32_t width, std::vector<float>& probs, uint32_t& steps,
                            uint32_t& classes) override {
        ++recognized;
        return inner_.Recognize(planes, width, probs, steps, classes);
    }

private:
    Runtime& inner_;
};

void TestFirstPass() {
    RAYOPDF_CHECK(!HasCjk("abc \xc3\xa9 \xe2\x82\xac") && !HasCjk("") && !HasCjk("\xe2\xb9\xbf") &&
                      HasCjk("\xe6\x97\xa5\xe6\x9c\xac") && HasCjk("a\xe3\x82\xa2") && HasCjk("\xef\xbc\xa1") &&
                      !HasCjk("\xf0\x9f\x98\x80") && !HasCjk("\xe6\x97"),
                  1500);
    // The first pass's dictionary and the recogniser's differ, so the text tells who read a line.
    Dictionary firstDictionary, recognizerDictionary;
    RAYOPDF_CHECK(firstDictionary.Load("\xe4\xb8\xad\nb\nc\nd") && recognizerDictionary.Load("w\nx\ny\nz"), 1501);
    Testing::FakeRuntime firstFake(firstDictionary.Classes()), recognizerFake(recognizerDictionary.Classes());
    CountingRuntime first(firstFake), recognizer(recognizerFake);
    // Three black bars: read "c" by the first pass, "y" by the recogniser, at confidence 0.9.
    const std::vector<uint8_t> page = Page(400, 300, {{40, 40, 360, 70}, {40, 120, 360, 150}, {40, 200, 360, 230}});
    const GrayView view{page.data(), 400, 300};
    std::vector<OcrProtocol::Line> lines;
    PpOcrSettings settings;
    settings.rereadConf = 0;  // nothing is doubtful
    PpOcrEngine sure(recognizer, recognizerDictionary, first, firstDictionary, settings);
    RAYOPDF_CHECK(sure.Page(view, 300, lines) == EngineStatus::Ok && lines.size() == 3 &&
                      lines[0].text.find('c') != std::string::npos && lines[2].text.find('y') == std::string::npos &&
                      first.recognized == 3 && recognizer.recognized == 0,
                  1502);
    // Every line below the confidence threshold is read again, and the recogniser's reading is kept.
    settings.rereadConf = kDefaultRereadConf;
    first.recognized = recognizer.recognized = 0;
    PpOcrEngine doubtful(recognizer, recognizerDictionary, first, firstDictionary, settings);
    RAYOPDF_CHECK(doubtful.Page(view, 300, lines) == EngineStatus::Ok && lines.size() == 3 &&
                      lines[0].text.find('y') != std::string::npos && lines[2].text.find('c') == std::string::npos &&
                      first.recognized == 3 && recognizer.recognized == 3,
                  1503);
    // Once more than half of at least one line was read again, the rest go to the recogniser alone.
    settings.switchAfter = 1;
    first.recognized = recognizer.recognized = 0;
    PpOcrEngine degraded(recognizer, recognizerDictionary, first, firstDictionary, settings);
    std::vector<OcrProtocol::Line> switched;
    RAYOPDF_CHECK(degraded.Page(view, 300, switched) == EngineStatus::Ok && first.recognized == 1 &&
                      recognizer.recognized == 3,
                  1504);
    bool same = switched.size() == lines.size();
    for (size_t i = 0; same && i < lines.size(); ++i) {
        same = switched[i].text == lines[i].text && switched[i].conf == lines[i].conf &&
               switched[i].x0 == lines[i].x0 && switched[i].chars.size() == lines[i].chars.size();
    }
    RAYOPDF_CHECK(same, 1505);
    // A confident line holding a CJK character is read again: a mid-gray bar is class 1, "中".
    settings = PpOcrSettings();
    settings.rereadConf = 0;
    first.recognized = recognizer.recognized = 0;
    std::vector<uint8_t> mixed = Page(400, 200, {{40, 40, 360, 70}});
    for (uint32_t y = 120; y < 150; ++y) {
        for (uint32_t x = 40; x < 360; ++x) mixed[size_t(y) * 400 + x] = 100;
    }
    PpOcrEngine cjk(recognizer, recognizerDictionary, first, firstDictionary, settings);
    RAYOPDF_CHECK(cjk.Page(GrayView{mixed.data(), 400, 200}, 300, lines) == EngineStatus::Ok && lines.size() == 2 &&
                      lines[0].text.find('c') != std::string::npos && !HasCjk(lines[1].text) &&
                      lines[1].text.find('w') != std::string::npos && first.recognized == 2 && recognizer.recognized == 1,
                  1506);
    // `lines` requests use the recogniser alone; a failing recogniser fails the page.
    std::vector<uint8_t> line(size_t(320) * 48, 255);
    for (uint32_t y = 0; y < 48; ++y) {
        for (uint32_t x = 0; x < 16; ++x) line[size_t(y) * 320 + x] = 0;
    }
    OcrProtocol::Line result;
    first.recognized = recognizer.recognized = 0;
    RAYOPDF_CHECK(doubtful.Line(GrayView{line.data(), 320, 48}, result) == EngineStatus::Ok && result.text == "y" &&
                      first.recognized == 0 && recognizer.recognized == 1,
                  1507);
    recognizerFake.failRecognize = true;
    RAYOPDF_CHECK(doubtful.Page(view, 300, lines) == EngineStatus::Failed, 1508);
}

struct Stream {
    std::string data;
    size_t at = 0;
};

size_t ReadStream(void* context, char* buffer, size_t size) {
    Stream& s = *static_cast<Stream*>(context);
    if (s.at >= s.data.size()) return 0;
    size_t take = s.data.size() - s.at;
    if (take > size) take = size;
    if (take > 7) take = 7;  // small pieces, as a pipe may hand them over
    std::memcpy(buffer, s.data.data() + s.at, take);
    s.at += take;
    return take;
}

bool WriteStream(void* context, std::string_view bytes) {
    static_cast<std::string*>(context)->append(bytes);
    return true;
}

std::string RawFrame(const std::string& header, const std::string& payload = std::string()) {
    std::string frame;
    const uint32_t n = uint32_t(header.size());
    for (int i = 0; i < 4; ++i) frame.push_back(char((n >> (8 * i)) & 0xff));
    return frame + header + payload;
}

ServeStatus RunServer(const std::string& input, Engine& engine, std::string& output) {
    Stream stream{input, 0};
    output.clear();
    ServerIo io;
    io.read = ReadStream;
    io.readContext = &stream;
    io.write = WriteStream;
    io.writeContext = &output;
    OcrProtocol::Handshake handshake;
    handshake.version = "0.0.9";
    handshake.protocol = 1;
    handshake.engine = "pp-ocrv6";
    handshake.tier = "test";
    handshake.runtime = "fake";
    handshake.models.emplace_back("dict.txt", std::string(64, 'a'));
    handshake.scripts.push_back("latin");
    return Serve(io, handshake, engine);
}

// The frames of `output`: handshake, then responses.
bool ReadAnswers(const std::string& output, OcrProtocol::Handshake& handshake, std::vector<OcrProtocol::Response>& responses) {
    Stream stream{output, 0};
    OcrProtocol::Frame frame;
    std::string error;
    if (OcrProtocol::ReadFrame(ReadStream, &stream, frame) != OcrProtocol::FrameStatus::Ok ||
        !OcrProtocol::ParseHandshake(frame.header, handshake, error)) {
        return false;
    }
    responses.clear();
    for (;;) {
        const OcrProtocol::FrameStatus status = OcrProtocol::ReadFrame(ReadStream, &stream, frame);
        if (status == OcrProtocol::FrameStatus::End) return true;
        OcrProtocol::Response response;
        if (status != OcrProtocol::FrameStatus::Ok || !OcrProtocol::ParseResponse(frame.header, response, error)) {
            return false;
        }
        responses.push_back(response);
    }
}

void TestServer() {
    const Dictionary dictionary = TestDictionary();
    Testing::FakeRuntime runtime(dictionary.Classes());
    PpOcrSettings settings;
    PpOcrEngine engine(runtime, dictionary, settings);

    const std::vector<uint8_t> page = Page(400, 200, {{40, 40, 360, 70}});
    std::string input, frame;
    OcrProtocol::Request request;
    request.id = 1;
    request.op = OcrProtocol::Op::Page;
    request.width = 400;
    request.height = 200;
    request.dpi = 300;
    OcrProtocol::WriteRequest(request, std::string_view(reinterpret_cast<const char*>(page.data()), page.size()), frame);
    input += frame;
    // Two line images: 320 x 48 with a black start, and 10 x 10 white.
    std::string lines(size_t(320) * 48 + 100, char(255));
    for (uint32_t y = 0; y < 48; ++y) std::memset(&lines[size_t(y) * 320], 0, 16);
    request = OcrProtocol::Request();
    request.id = 2;
    request.op = OcrProtocol::Op::Lines;
    request.dpi = 300;
    request.lines = {{320, 48}, {10, 10}};
    OcrProtocol::WriteRequest(request, lines, frame);
    input += frame;
    input += RawFrame("{\"id\":3,\"op\":\"dance\"}\n");  // a bad request with a readable id
    request = OcrProtocol::Request();
    request.id = 4;
    request.op = OcrProtocol::Op::Quit;
    OcrProtocol::WriteRequest(request, std::string_view(), frame);
    input += frame;
    input += RawFrame("{\"id\":5,\"op\":\"quit\"}\n");  // never read: the server has stopped

    std::string output;
    RAYOPDF_CHECK(RunServer(input, engine, output) == ServeStatus::Quit, 1283);
    OcrProtocol::Handshake handshake;
    std::vector<OcrProtocol::Response> responses;
    RAYOPDF_CHECK(ReadAnswers(output, handshake, responses) && handshake.engine == "pp-ocrv6" &&
                      handshake.models.size() == 1 && responses.size() == 4,
                  1284);
    RAYOPDF_CHECK(responses[0].id == 1 && responses[0].ok && responses[0].lines.size() == 1 &&
                      responses[0].lines[0].text.find('c') != std::string::npos,
                  1285);
    RAYOPDF_CHECK(responses[1].id == 2 && responses[1].ok && responses[1].lines.size() == 2 &&
                      responses[1].lines[0].text == "c" && responses[1].lines[1].text.empty() &&
                      responses[1].lines[1].x1 == 10 && responses[1].lines[1].y1 == 10,
                  1286);
    RAYOPDF_CHECK(responses[2].id == 3 && !responses[2].ok && responses[2].error == OcrProtocol::kErrorBadRequest &&
                      responses[3].id == 4 && responses[3].ok && responses[3].lines.empty(),
                  1287);
    // Same input, same bytes.
    std::string second;
    RunServer(input, engine, second);
    RAYOPDF_CHECK(second == output, 1288);

    // The input ends between frames: the handshake alone, then End.
    RAYOPDF_CHECK(RunServer(std::string(), engine, output) == ServeStatus::End && ReadAnswers(output, handshake, responses) &&
                      responses.empty(),
                  1289);
    // A frame cut short: an error with id 0, then the server stops.
    RAYOPDF_CHECK(RunServer(RawFrame("{\"id\":7,\"op\":\"page\",\"width\":10,\"height\":10,\"channels\":1,\"dpi\":0,\"payload\":100}\n",
                                     std::string(20, 'x')),
                            engine, output) == ServeStatus::BrokenFrame &&
                      ReadAnswers(output, handshake, responses) && responses.size() == 1 && responses[0].id == 0 &&
                      !responses[0].ok,
                  1290);
    // A length over the header limit: too-large.
    std::string huge = "\xff\xff\xff\x7f{";
    RAYOPDF_CHECK(RunServer(huge, engine, output) == ServeStatus::BrokenFrame && ReadAnswers(output, handshake, responses) &&
                      responses.size() == 1 && responses[0].error == OcrProtocol::kErrorTooLarge,
                  1291);
    // An engine failure answers engine-failed and the server goes on.
    runtime.failDetect = true;
    request = OcrProtocol::Request();
    request.id = 8;
    request.op = OcrProtocol::Op::Page;
    request.width = 400;
    request.height = 200;
    OcrProtocol::WriteRequest(request, std::string_view(reinterpret_cast<const char*>(page.data()), page.size()), frame);
    RAYOPDF_CHECK(RunServer(frame + frame, engine, output) == ServeStatus::End && ReadAnswers(output, handshake, responses) &&
                      responses.size() == 2 && responses[1].id == 8 &&
                      responses[1].error == OcrProtocol::kErrorEngineFailed,
                  1292);
}

void TestTesseractTsv() {
    const std::string tsv =
        "level\tpage_num\tblock_num\tpar_num\tline_num\tword_num\tleft\ttop\twidth\theight\tconf\ttext\n"
        "1\t1\t0\t0\t0\t0\t0\t0\t600\t400\t-1\t\n"
        "4\t1\t1\t1\t1\t0\t10\t20\t200\t30\t-1\t\n"
        "5\t1\t1\t1\t1\t1\t10\t20\t80\t30\t96.5\tHello\n"
        "5\t1\t1\t1\t1\t2\t100\t22\t110\t28\t90.5\tw\xc3\xb6rld\n"
        "5\t1\t1\t1\t1\t3\t220\t22\t10\t28\t95\t \n"
        "5\t1\t1\t1\t2\t1\t10\t60\t40\t30\t50\tNext\n"
        "5\t1\t1\t1\t2\t2\tbad\t60\t40\t30\t50\tskipped\n";
    std::vector<OcrProtocol::Line> lines;
    RAYOPDF_CHECK(ParseTesseractTsv(tsv, lines) && lines.size() == 2, 1293);
    const OcrProtocol::Line& first = lines[0];
    RAYOPDF_CHECK(first.text == "Hello w\xc3\xb6rld" && first.x0 == 10 && first.y0 == 20 && first.x1 == 210 &&
                      first.y1 == 50 && first.conf == ToConf(0.935) && first.chars.size() == 11 &&
                      first.chars[0].x0 == 10 && first.chars[4].x1 == 90 && first.chars[5].x0 == 90 &&
                      first.chars[5].x1 == 100 && first.chars[10].x1 == 210,
                  1294);
    RAYOPDF_CHECK(lines[1].text == "Next" && lines[1].conf == 5000, 1295);
    RAYOPDF_CHECK(!ParseTesseractTsv("not a tsv\n", lines) && ParseTesseractTsv("level\tx\n", lines) && lines.empty(),
                  1296);
}

void TestModelFiles() {
    std::vector<ModelFile> files;
    const std::string hash(64, 'b');
    RAYOPDF_CHECK(ParseManifest(hash + " 12 det.ort\n" + hash + " 0 dict.txt\r\n", files) && files.size() == 2 &&
                      files[0].bytes == 12 && files[1].name == "dict.txt",
                  1297);
    RAYOPDF_CHECK(!ParseManifest("", files) && !ParseManifest(hash + " 12 ../x\n", files) &&
                      !ParseManifest(hash + " 12 a\n" + hash + " 1 a\n", files) &&
                      !ParseManifest(std::string(64, 'B') + " 1 a\n", files) && !ParseManifest(hash + " x a\n", files),
                  1298);
    ModelSet set;
    float value = 0;
    RAYOPDF_CHECK(ParseConfig("model PP-OCRv6_small\ndet_thresh 0.2\ndet_box_thresh 0.45\ndet_unclip_ratio 1.4\n"
                              "rec_height 48\nfuture_key 7\n",
                              set) &&
                      set.model == "PP-OCRv6_small" && std::fabs(set.detect.boxThresh - 0.45f) < 1e-6f &&
                      std::fabs(set.detect.unclipRatio - 1.4f) < 1e-6f && ParseDecimal("2", value) && value == 2.0f &&
                      !ParseDecimal("-1", value) && !ParseDecimal("1e3", value) && !ParseDecimal("1.", value) &&
                      !ParseConfig("model m\ndet_thresh 0.2\ndet_box_thresh 0.45\n", set) &&
                      !ParseConfig("model m\ndet_thresh 1.5\ndet_box_thresh 0.45\ndet_unclip_ratio 1.4\n", set),
                  1299);
}

} // namespace

int main() {
    TestDictionary_();
    TestCtc();
    TestRaster();
    TestDetect();
    TestEngine();
    TestFirstPass();
    TestServer();
    TestTesseractTsv();
    TestModelFiles();
    return 0;
}
