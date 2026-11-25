// SPDX-License-Identifier: Apache-2.0
// The engine of the OCR add-on rayopdf-ocr: an Engine turns a gray page into lines of text (detect
// + recognise), or line images into their text. PpOcrEngine runs PP-OCRv6's detection and
// recognition networks through a Runtime (ncnn, src/ocr/addon/) and does all pre- and
// post-processing itself.
//
// PpOcrEngine, one page:
//  1. Detection input: the page resampled so that its longer side is at most `detMaxSide` (never
//     enlarged), each side rounded to a multiple of 32 (at least 32), normalised with ImageNet's
//     mean and deviation; the network returns a text probability map of the same size.
//  2. DetectBoxes (detect.h) on that map, boxes scaled back to page pixels, SortIntoLines.
//  3. Orientation, with the text-line classifier (PP-LCNet textline_ori, 0 or 180 degrees) when
//     one is loaded. Turned a quarter: when at least kTurnShare of the page's line-like boxes
//     (one side at least twice the other, at least kTurnMinLines of them) stand upright, the
//     classifier, on up to kOrientSample of the longest turned a quarter counter-clockwise, tells
//     whether the text runs down (top to bottom) or up, unless the readings of kScoreSample of them
//     both ways (ReadingScore: the first pass, or the script pack where it reads nothing) differ.
//     Upside down: otherwise the classifier reads up to kOrientSample of the longest lying boxes;
//     when their mean probability of 180 degrees is at least kTurnProb, the page counts as upside
//     down only if ReadingScore reads more turned. The classifier knows Latin and CJK lines; it
//     misjudges Cyrillic and Hangul, so a page left standing with at least kSecondLookShare of its
//     lines unread after step 6, and either a classifier mean of kSecondLookFlip or as many lines
//     read nearly empty (below kProbeDensity), is read again upside down and kept so only when that
//     leaves under half as many lines unread. A turned page is turned upright and detected again;
//     its lines go back into the page's pixels with their baseline direction
//     (OcrProtocol::Line::dir) and spans along it.
//  4. Every box is cut out of the full-resolution page (SampleRect, at its own angle); a crop at
//     least 1.5 times as tall as wide is turned a quarter counter-clockwise (text that runs down),
//     or clockwise when the classifier finds it upside down that way (text that runs up). Such a
//     line keeps direction 0, as before turned pages were read, when its box is less than twice
//     as tall as wide or it reads fewer than kMinTurnedCharacters characters (a digit in a narrow
//     cell, an integral sign, a line number).
//  5. Recognition input: the crop resampled to height 48 (width in proportion, at most
//     kMaxRecWidth), padded to at least 320 columns as PaddleOCR does, normalised to [-1, 1];
//     DecodeCtc + InsertSpaces + AssembleText give text, confidence and character spans, mapped
//     back onto the page along the line's baseline.
// With a first pass (a cheaper recogniser: tiny's in front of small's), the tier's network still
// detects, so line boxes, and with them bullets and indents, stay the tier's. The first-pass
// recogniser reads every line in reading order; a line it reads below `rereadConf`, or with a CJK
// character (tiny's dictionary lacks the kana and most Han), is read again by the tier's
// recogniser, whose reading replaces it. Once at least `switchAfter` lines are read and more than
// `switchShare` of them were read again (a degraded scan), every further line of the page goes to
// the tier's recogniser alone.
//  6. Scripts (ScriptPack, when the add-on has one): a line whose reading holds too few characters
//     for its length (Unread: fewer than kUnreadDensity per line height of width) or is below
//     kUnreadConf is unread, as a line in a script the tier's dictionary lacks reads (Cyrillic as
//     punctuation and spaces, Hangul and Arabic as nothing). Pages without unread lines pay
//     nothing. Otherwise up to kProbeLines of the longest unread lines that the tier reads nearly
//     empty (below kProbeDensity characters per line height: a degraded Latin line reads at a low
//     confidence but with its characters, so it is not probed) are probed, each first by the
//     winners so far and only when none reads it by the pack's other recognisers (each loaded on
//     first use); a recogniser that reads one of them at least kScriptConf and
//     kScriptMargin above the tier's reading (or the tier's reading is unread), not unread itself
//     and with at least kScriptOwnShare of its letters in its script, wins the line, and every unread line of the page is
//     read by the winners, the most confident reading kept by the same rule. A recogniser that
//     reads right-to-left text left to right (config "order visual") has its reading put into
//     logical order (ToLogicalOrder).
// `lines` requests (Lines) read every image with the tier's recogniser, then route their unread
// lines the same way (the images of one request come from one font: one script).
// Lines with no text are left out of pages. At most kMaxPageLines lines per page. Inference is
// single-threaded (the runtime is set up so), and every step is deterministic: the same request
// gives the same lines on every run on a machine, whatever came before (networks load on first
// use but do not change results). Buffers are kept between calls.
#pragma once

#include "common/ocr_protocol.h"
#include "ocr/detect.h"
#include "ocr/raster.h"
#include "ocr/recognize.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace RayoPdf::Ocr {

constexpr uint32_t kRecHeight = 48;
constexpr uint32_t kMaxRecWidth = 3200;
constexpr uint32_t kMaxPageLines = 4096;
constexpr uint32_t kDefaultDetMaxSide = 1280;
constexpr uint32_t kMaxDetSide = 4096;
// The orientation classifier's input (PP-LCNet_x0_25_textline_ori: 160 x 80, ImageNet-normalised).
constexpr uint32_t kClsWidth = 160;
constexpr uint32_t kClsHeight = 80;

enum class RuntimeStatus : uint8_t { Ok, Failed };

// One inference runtime with a recognition network, and a detection network unless it was loaded
// for recognition only (then Detect fails), or the orientation classifier alone.
class Runtime {
public:
    virtual ~Runtime() = default;
    // Input 1x3xHxW planes; output H x W probabilities (replaced).
    virtual RuntimeStatus Detect(const float* planes, uint32_t width, uint32_t height, std::vector<float>& prob) = 0;
    // Input 1x3x48xW planes; output steps x classes probabilities (replaced).
    virtual RuntimeStatus Recognize(const float* planes, uint32_t width, std::vector<float>& probs, uint32_t& steps,
                                    uint32_t& classes) = 0;
    // Input 1x3xkClsHeightxkClsWidth planes; output the class probabilities (replaced). Only a
    // runtime loaded with the classifier has one.
    virtual RuntimeStatus Classify(const float*, std::vector<float>&) { return RuntimeStatus::Failed; }
};

enum class EngineStatus : uint8_t { Ok, Failed };

class Engine {
public:
    virtual ~Engine() = default;
    // The lines of a page (replaced), boxes and spans in its pixels, in reading order.
    virtual EngineStatus Page(const GrayView& page, uint32_t dpi, std::vector<OcrProtocol::Line>& lines) = 0;
    // The text of each line image (`lines` replaced, one per image); each box is its whole image.
    virtual EngineStatus Lines(const std::vector<GrayView>& images, std::vector<OcrProtocol::Line>& lines) = 0;
};

// A recognition network and the dictionary of its classes.
struct Recognizer {
    Runtime* runtime = nullptr;
    const Dictionary* dictionary = nullptr;
    bool visualOrder = false;  // reads right-to-left text left to right
    float spaceGap = 0;        // InsertInkSpaces' gap in line heights (0: off)
    // A recogniser of the script pack: the code points of its script's letters. Its reading wins
    // a line only when at least half of its letters are of its script (OwnShare).
    uint32_t lettersFirst = 0, lettersLast = 0;
};

// The recognisers of the script pack (rayopdf-ocr-models/scripts/), each loaded on first use.
class ScriptPack {
public:
    virtual ~ScriptPack() = default;
    virtual size_t Count() const = 0;
    // Recogniser `index` (below Count()), loading it on first use; null when it cannot be loaded
    // (then it is not tried again).
    virtual const Recognizer* Get(size_t index) = 0;
};

// First-pass settings, measured on the scanned development pages (docs/development/ocr-engine.md).
constexpr uint16_t kDefaultRereadConf = 9950;  // per myriad
constexpr uint32_t kDefaultSwitchAfter = 8;
constexpr float kDefaultSwitchShare = 0.5f;

// Script routing (step 6), measured on the multilingual and scanned development pages.
constexpr float kUnreadDensity = 0.5f;     // characters per line height of width
constexpr uint16_t kUnreadConf = 8000;     // per myriad
constexpr uint32_t kProbeLines = 3;
constexpr uint16_t kScriptConf = 8000;     // per myriad
constexpr uint16_t kScriptMargin = 500;    // per myriad
constexpr float kScriptOwnShare = 0.5f;    // of its letters in the recogniser's script
// Orientation (step 3).
constexpr float kTurnShare = 0.6f;
constexpr uint32_t kTurnMinLines = 3;
constexpr uint32_t kOrientSample = 5;
constexpr float kTurnProb = 0.5f;
constexpr float kSecondLookShare = 0.8f;  // of a page's line-like lines unread: read it upside down too
constexpr float kSecondLookFlip = 0.1f;   // ... when the classifier gives 180 degrees at least this,
                                          // or that share of the lines read nearly empty
constexpr float kProbeDensity = 0.25f;    // a line is probed when the tier reads fewer characters
constexpr uint32_t kScoreSample = 3;      // lines ReadingScore reads each way
constexpr uint32_t kShortcutLines = 6;    // lines read before the script shortcut is decided
constexpr uint32_t kMinTurnedCharacters = 4;  // a standing box reading fewer stays a line of direction 0
constexpr uint16_t kScriptSure = 9000;    // ... no other recogniser tried after one this confident

struct PpOcrSettings {
    DetectParams detect;  // the tier's
    uint32_t detMaxSide = kDefaultDetMaxSide;
    float spaceThreshold = kDefaultSpaceThreshold;  // InsertSpaces (recognize.h)
    uint16_t rereadConf = kDefaultRereadConf;
    uint32_t switchAfter = kDefaultSwitchAfter;
    float switchShare = kDefaultSwitchShare;
};

// What a PpOcrEngine did (rayopdf-ocr --image --stats; the measurements of ocr-engine.md).
struct EngineStats {
    uint64_t recognitions = 0;        // every recogniser run
    uint64_t scriptRecognitions = 0;  // ... of the script pack
    uint64_t classifications = 0;     // orientation classifier runs
    uint32_t pages = 0, turnedPages = 0, secondLooks = 0;
    float lastFlip = -1;              // the last page's mean probability of 180 degrees (-1: none)
};

class PpOcrEngine final : public Engine {
public:
    // One tier: `runtime` detects and recognises.
    PpOcrEngine(Runtime& runtime, const Dictionary& dictionary, const PpOcrSettings& settings)
        : recognizer_{&runtime, &dictionary}, first_{&runtime, &dictionary}, firstPass_(false), settings_(settings) {}
    // A first pass: `runtime` detects and reads again what `first` reads doubtfully.
    PpOcrEngine(Runtime& runtime, const Dictionary& dictionary, Runtime& first, const Dictionary& firstDictionary,
                const PpOcrSettings& settings)
        : recognizer_{&runtime, &dictionary}, first_{&first, &firstDictionary}, firstPass_(true),
          settings_(settings) {}
    // The script pack (null: none) and the orientation classifier (null: pages are read as they
    // stand, and vertical lines are taken to run down).
    void SetScripts(ScriptPack* scripts) { scripts_ = scripts; }
    void SetOrientation(Runtime* classifier) { orient_ = classifier; }
    EngineStats stats;

    EngineStatus Page(const GrayView& page, uint32_t dpi, std::vector<OcrProtocol::Line>& lines) override;
    EngineStatus Lines(const std::vector<GrayView>& images, std::vector<OcrProtocol::Line>& lines) override;

private:
    // A line being read: where it is and how its crop is cut, and its reading so far.
    struct Read {
        RotatedRect rect;
        PixelBox bounds;
        uint32_t cropWidth = 0, cropHeight = 0;  // sampled along the rectangle's axes
        uint32_t quarters = 0;                   // clockwise quarter turns from the crop to the read image
        CropText text;                           // spans along the read image's width (crop pixels)
        bool unread = false;
        float density = 0;                       // the tier's reading's Density
        bool lying = false;                      // turned to be read, but a line of direction 0
    };

    EngineStatus DetectPage(const GrayView& page);
    // `read`'s image (the crop turned) into crop_ (width and height of the read image returned).
    void CutRead(const GrayView& page, const Read& read, uint32_t& width, uint32_t& height);
    // The probability that the image in crop_ (width x height) is upside down; false on failure.
    bool UpsideDown(uint32_t width, uint32_t height, float& probability);
    // Clockwise quarter turns that bring the page upright (0 when it stands or nothing tells).
    EngineStatus PageTurn(const GrayView& page, uint32_t& quarters);
    // The sample boxes of PageTurn (order_) read turned `quarters`: per line the confidence of the
    // first pass, or of the script pack when the first pass leaves it unread; 0 for an unread line.
    // `second`: the other way of a pair, which tries only the pack's recogniser that read the first.
    EngineStatus ReadingScore(const GrayView& page, uint32_t quarters, bool second, uint64_t& score);
    // Reads every box of boxes_ on `page` into reads_ (step 4-6).
    EngineStatus ReadBoxes(const GrayView& page);
    // Step 6 on reads_: `image(i)` puts read i's image into crop_ and returns its size. Sets
    // script_ to the first winner when none is set.
    template <typename Cut> EngineStatus RouteScripts(Cut image);
    // Once kShortcutLines lines are read (`tried` after), when two thirds of them are left nearly
    // empty, step 6 runs on them: a winner reads the further lines first (ReadByScript).
    template <typename Cut> EngineStatus Shortcut(Cut image, bool& tried);
    // The image in crop_ read by script_'s recogniser into `text` when that reads it surely
    // (kScriptConf, not unread, kScriptOwnShare of its letters in its script).
    bool ReadByScript(uint32_t width, uint32_t height, CropText& text);
    // Recognises `crop` (cropWidth x cropHeight gray) with `recognizer` into `text`, spans in crop
    // pixels.
    EngineStatus RecognizeCrop(const Recognizer& recognizer, const std::vector<uint8_t>& crop, uint32_t cropWidth,
                               uint32_t cropHeight, CropText& text);

    Recognizer recognizer_, first_;  // recognizer_ also detects
    bool firstPass_;
    PpOcrSettings settings_;
    ScriptPack* scripts_ = nullptr;
    Runtime* orient_ = nullptr;
    float flip_ = -1;           // PageTurn: the lying samples' mean probability of 180 degrees, -1 when none
    int32_t script_ = -1;       // the pack's recogniser that reads this page or request, -1 none yet
    int32_t scoreScript_ = -1;  // ReadingScore: the recogniser that read the first way
    ResizeScratch resize_;
    DetectScratch detect_;
    std::vector<uint8_t> small_, crop_, turned_, line_, upright_;
    std::vector<float> planes_, prob_, probs_;
    std::vector<TextBox> boxes_;
    std::vector<DecodedChar> decoded_;
    std::vector<Read> reads_, firstReads_;
    std::vector<uint32_t> order_;
    CropText text_, other_;
};

// True when `text` holds a CJK character (U+2E80-U+9FFF, U+F900-U+FAFF, U+FF00-U+FFEF): the
// first pass re-reads such lines (tiny's dictionary has no kana and fewer Han).
bool HasCjk(std::string_view text);

// True when a reading of an image `width` x `height` (height across the text) at `conf` holds too
// little text for its length to be a reading of its script (step 6).
bool Unread(const CropText& text, uint32_t width, uint32_t height);
// Non-space code points of a reading, and their number per line height of its image's width.
uint32_t Characters(const CropText& text);
float Density(const CropText& text, uint32_t width, uint32_t height);

// A reading in visual order (left to right) put into logical order for right-to-left text:
// UAX #9 for a right-to-left paragraph with these classes: right-to-left letters (Hebrew, Arabic
// and their presentation forms), left-to-right letters, numbers (ASCII and Arabic-Indic digits
// with the separators between them), neutrals. Runs of left-to-right letters and numbers keep
// their order, everything else is reversed, and paired brackets in right-to-left text are mirrored.
// Each code point keeps its span.
void ToLogicalOrder(CropText& text);

} // namespace RayoPdf::Ocr
