// SPDX-License-Identifier: Apache-2.0
// Core tests of src/text/bidi (exit codes 600-639 and 800-849): the generated UAX #9 tables, the
// resolver against a sample of the Unicode BidiCharacterTest data (tests/bidi_character_test.h),
// and visual -> logical reordering of lines as a PDF shows them.
//
// Given paths to BidiCharacterTest.txt and/or BidiTest.txt (UCD of the tables' version), the binary
// checks every case of those files instead and prints the counts.

#include "check.h"

#include "bidi_character_test.h"
#include "common/utf8.h"
#include "text/bidi.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace RayoPdf;
using namespace RayoPdf::Text;

namespace {

// The code points of UTF-8 `text`.
std::vector<uint32_t> CodePoints(std::string_view text) {
    std::vector<uint32_t> out;
    size_t at = 0, length = 0;
    uint32_t cp = 0;
    while (Utf8::Decode(text, at, cp, length)) {
        out.push_back(cp);
        at += length;
    }
    return out;
}

// `visual` (UTF-8, one code point per item, left to right on the page) in logical order, with
// odd-level items mirrored the way a caller writes them.
std::string Logical(std::string_view visual, bool* rtl = nullptr, std::vector<uint8_t>* levels = nullptr) {
    const std::vector<uint32_t> cps = CodePoints(visual);
    std::vector<Bidi::Class> classes;
    for (uint32_t cp : cps) classes.push_back(Bidi::ClassOf(cp));
    std::vector<uint32_t> order;
    std::vector<uint8_t> itemLevels;
    const bool paragraphRtl = Bidi::VisualToLogical(classes.data(), classes.size(), order, &itemLevels);
    if (rtl) *rtl = paragraphRtl;
    if (levels) *levels = itemLevels;
    std::string out;
    for (uint32_t v : order) Utf8::Append(out, itemLevels[v] & 1 ? Bidi::Mirror(cps[v]) : cps[v]);
    return out;
}

std::vector<std::string_view> Split(std::string_view text, char separator) {
    std::vector<std::string_view> out;
    size_t at = 0;
    for (;;) {
        const size_t next = text.find(separator, at);
        out.push_back(text.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at));
        if (next == std::string_view::npos) break;
        at = next + 1;
    }
    return out;
}

std::vector<std::string_view> Words(std::string_view text) {
    std::vector<std::string_view> out;
    size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) ++at;
        size_t end = at;
        while (end < text.size() && text[end] != ' ' && text[end] != '\t') ++end;
        if (end > at) out.push_back(text.substr(at, end - at));
        at = end;
    }
    return out;
}

uint32_t Number(std::string_view text, int base) {
    return static_cast<uint32_t>(std::strtoul(std::string(text).c_str(), nullptr, base));
}

// The resolved levels as the test files write them ("x" for removed items).
std::string LevelText(const std::vector<uint8_t>& levels) {
    std::string out;
    for (const uint8_t l : levels) {
        if (!out.empty()) out += ' ';
        out += l == Bidi::kRemovedLevel ? std::string("x") : std::to_string(l);
    }
    return out;
}

std::string OrderText(const std::vector<uint32_t>& order) {
    std::string out;
    for (const uint32_t i : order) {
        if (!out.empty()) out += ' ';
        out += std::to_string(i);
    }
    return out;
}

std::string Joined(const std::vector<std::string_view>& words) {
    std::string out;
    for (const std::string_view w : words) {
        if (!out.empty()) out += ' ';
        out.append(w.data(), w.size());
    }
    return out;
}

// One BidiCharacterTest.txt line: whether the paragraph level, the levels and the order match.
bool CharacterCase(std::string_view line, bool& levelsOk, bool& orderOk) {
    const std::vector<std::string_view> fields = Split(line, ';');
    if (fields.size() < 5) return false;
    std::vector<uint32_t> cps;
    std::vector<Bidi::Class> classes;
    for (const std::string_view w : Words(fields[0])) {
        cps.push_back(Number(w, 16));
        classes.push_back(Bidi::ClassOf(cps.back()));
    }
    const uint32_t mode = Number(fields[1], 10);
    std::vector<uint8_t> levels;
    const int direction = mode == 2 ? -1 : static_cast<int>(mode);
    const uint8_t paragraph = Bidi::ResolveLevels(classes.data(), cps.data(), cps.size(), direction, levels);
    std::vector<uint32_t> order;
    Bidi::LogicalToVisual(levels.data(), levels.size(), order);
    levelsOk = paragraph == Number(fields[2], 10) && LevelText(levels) == Joined(Words(fields[3]));
    orderOk = OrderText(order) == Joined(Words(fields[4]));
    return true;
}

Bidi::Class ClassByName(std::string_view name, bool& ok) {
    static const char* const kNames[] = {"L", "R", "AL", "EN", "ES", "ET", "AN", "CS", "NSM", "BN", "B", "S",
                                         "WS", "ON", "LRE", "LRO", "RLE", "RLO", "PDF", "LRI", "RLI", "FSI", "PDI"};
    for (size_t i = 0; i < sizeof kNames / sizeof kNames[0]; ++i) {
        if (name == kNames[i]) return static_cast<Bidi::Class>(i);
    }
    ok = false;
    return Bidi::Class::L;
}

// Checks every case of a BidiCharacterTest.txt or BidiTest.txt file; prints and returns the
// number of failures (-1 when the file cannot be read).
long RunFile(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return -1;
    std::string data;
    char buffer[65536];
    size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) data.append(buffer, n);
    std::fclose(f);
    long passed = 0, failed = 0;
    std::string levelsLine, reorderLine;
    for (const std::string_view raw : Split(data, '\n')) {
        std::string_view line = raw;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line[0] == '#') continue;
        if (line.substr(0, 8) == "@Levels:") {
            levelsLine = Joined(Words(line.substr(8)));
            continue;
        }
        if (line.substr(0, 9) == "@Reorder:") {
            reorderLine = Joined(Words(line.substr(9)));
            continue;
        }
        const std::vector<std::string_view> fields = Split(line, ';');
        if (fields.size() >= 5) {
            bool levelsOk = false, orderOk = false;
            CharacterCase(line, levelsOk, orderOk);
            if (levelsOk && orderOk) ++passed;
            else if (++failed <= 10) std::printf("FAIL %.*s\n", static_cast<int>(line.size()), line.data());
            continue;
        }
        if (fields.size() != 2) continue;
        // BidiTest.txt: classes; bitset of paragraph modes (1 auto, 2 LTR, 4 RTL).
        std::vector<Bidi::Class> classes;
        bool ok = true;
        for (const std::string_view w : Words(fields[0])) classes.push_back(ClassByName(w, ok));
        const uint32_t modes = Number(Words(fields[1]).empty() ? "0" : Words(fields[1])[0], 10);
        for (int bit = 0; bit < 3 && ok; ++bit) {
            if (!(modes & (1u << bit))) continue;
            std::vector<uint8_t> levels;
            Bidi::ResolveLevels(classes.data(), nullptr, classes.size(), bit == 0 ? -1 : bit - 1, levels);
            std::vector<uint32_t> order;
            Bidi::LogicalToVisual(levels.data(), levels.size(), order);
            if (LevelText(levels) == levelsLine && OrderText(order) == reorderLine) ++passed;
            else if (++failed <= 10)
                std::printf("FAIL %.*s (mode %d)\n", static_cast<int>(line.size()), line.data(), bit);
        }
    }
    std::printf("%s: %ld passed, %ld failed\n", path, passed, failed);
    return failed;
}

// `logical` (UTF-8) shown the way UAX #9 displays it (L2 order, odd levels mirrored), then read
// back with VisualToLogical with the code points: whether it comes back unchanged.
bool RoundTrip(std::string_view logical, int direction) {
    const std::vector<uint32_t> cps = CodePoints(logical);
    std::vector<Bidi::Class> classes;
    for (const uint32_t cp : cps) classes.push_back(Bidi::ClassOf(cp));
    std::vector<uint8_t> levels;
    Bidi::ResolveLevels(classes.data(), cps.data(), cps.size(), direction, levels);
    std::vector<uint32_t> order;
    Bidi::LogicalToVisual(levels.data(), levels.size(), order);
    std::vector<uint32_t> shown;
    std::vector<Bidi::Class> shownClasses;
    std::string expected;
    for (size_t i = 0; i < cps.size(); ++i) {
        if (levels[i] != Bidi::kRemovedLevel) Utf8::Append(expected, cps[i]);
    }
    for (const uint32_t i : order) {
        shown.push_back(levels[i] & 1 ? Bidi::Mirror(cps[i]) : cps[i]);
        shownClasses.push_back(Bidi::ClassOf(shown.back()));
    }
    std::vector<uint32_t> back;
    std::vector<uint8_t> backLevels;
    Bidi::VisualToLogical(shownClasses.data(), shown.data(), shown.size(), back, &backLevels, direction);
    std::string out;
    for (const uint32_t v : back) Utf8::Append(out, backLevels[v] & 1 ? Bidi::Mirror(shown[v]) : shown[v]);
    if (out != expected)
        std::printf("round trip: %.*s -> %s\n", static_cast<int>(logical.size()), logical.data(), out.c_str());
    return out == expected;
}

void TestResolver() {
    // The sample of BidiCharacterTest.txt.
    int levelFailures = 0, orderFailures = 0;
    for (const Test::BidiCharacterCase& c : Test::kBidiCharacterCases) {
        bool levelsOk = false, orderOk = false;
        RAYOPDF_CHECK(CharacterCase(c.text, levelsOk, orderOk), 800);
        if (!levelsOk && ++levelFailures <= 5) std::printf("levels differ: line %d\n", c.line);
        if (!orderOk && ++orderFailures <= 5) std::printf("order differs: line %d\n", c.line);
    }
    RAYOPDF_CHECK(levelFailures == 0, 801);
    RAYOPDF_CHECK(orderFailures == 0, 802);

    // An override: "a RLO b c PDF d" shows b and c reversed; RLO and PDF are removed (X9).
    const Bidi::Class override[] = {Bidi::Class::L, Bidi::Class::RLO, Bidi::Class::L, Bidi::Class::L,
                                    Bidi::Class::PDF, Bidi::Class::L};
    std::vector<uint8_t> levels;
    RAYOPDF_CHECK(Bidi::ResolveLevels(override, nullptr, 6, -1, levels) == 0 && LevelText(levels) == "0 x 1 1 x 0",
                  803);
    std::vector<uint32_t> order;
    Bidi::LogicalToVisual(levels.data(), levels.size(), order);
    RAYOPDF_CHECK(OrderText(order) == "0 3 2 5", 804);
    // An isolate is neutral to its surroundings: "R LRI L PDI R" in an auto paragraph is right to left.
    const Bidi::Class isolate[] = {Bidi::Class::R, Bidi::Class::LRI, Bidi::Class::L, Bidi::Class::PDI, Bidi::Class::R};
    RAYOPDF_CHECK(Bidi::ResolveLevels(isolate, nullptr, 5, -1, levels) == 1 && LevelText(levels) == "1 1 2 1 1", 805);
    // P2 skips isolates: "LRI L PDI R" is right to left.
    RAYOPDF_CHECK(Bidi::ResolveLevels(isolate + 1, nullptr, 4, -1, levels) == 1, 806);
    // Embeddings deeper than 125 overflow without crashing; the text stays at level 125.
    std::vector<Bidi::Class> deep(130, Bidi::Class::RLE);
    deep.push_back(Bidi::Class::R);
    RAYOPDF_CHECK(Bidi::ResolveLevels(deep.data(), nullptr, deep.size(), 0, levels) == 0 && levels.back() == 125, 807);
    Bidi::ResolveLevels(nullptr, nullptr, 0, -1, levels);
    RAYOPDF_CHECK(levels.empty(), 808);
}

void TestVisualRoundTrips() {
    // Lines shown by the forward algorithm read back in logical order: brackets around embedded
    // text, numbers, isolates.
    RAYOPDF_CHECK(RoundTrip("שלום (abc) עולם", -1), 809);
    RAYOPDF_CHECK(RoundTrip("Hello [שלום] world", -1), 810);
    RAYOPDF_CHECK(RoundTrip("מחיר: 1,234.50 (כולל מע״מ)", -1), 811);
    RAYOPDF_CHECK(RoundTrip("השם \xE2\x81\xA6Windows 10\xE2\x81\xA9 חדש", -1), 812);
    RAYOPDF_CHECK(RoundTrip("see \xE2\x81\xA7שלום [עולם]\xE2\x81\xA9 now", 0), 813);
    RAYOPDF_CHECK(RoundTrip("عام ١٢٣ (و 45)", -1), 814);
}

void TestTables() {
    RAYOPDF_CHECK(Bidi::ClassOf('A') == Bidi::Class::L && Bidi::ClassOf('z') == Bidi::Class::L, 600);
    RAYOPDF_CHECK(Bidi::ClassOf('1') == Bidi::Class::EN && Bidi::ClassOf('+') == Bidi::Class::ES, 601);
    RAYOPDF_CHECK(Bidi::ClassOf('$') == Bidi::Class::ET && Bidi::ClassOf(',') == Bidi::Class::CS, 602);
    RAYOPDF_CHECK(Bidi::ClassOf(' ') == Bidi::Class::WS && Bidi::ClassOf('\t') == Bidi::Class::S, 603);
    RAYOPDF_CHECK(Bidi::ClassOf('(') == Bidi::Class::ON && Bidi::ClassOf('\n') == Bidi::Class::B, 604);
    RAYOPDF_CHECK(Bidi::ClassOf(0x05D0) == Bidi::Class::R && Bidi::ClassOf(0x0627) == Bidi::Class::AL, 605);
    RAYOPDF_CHECK(Bidi::ClassOf(0x0661) == Bidi::Class::AN && Bidi::ClassOf(0x0300) == Bidi::Class::NSM, 606);
    RAYOPDF_CHECK(Bidi::ClassOf(0x202B) == Bidi::Class::RLE && Bidi::ClassOf(0x2067) == Bidi::Class::RLI, 607);
    RAYOPDF_CHECK(Bidi::ClassOf(0x2069) == Bidi::Class::PDI && Bidi::ClassOf(0x200D) == Bidi::Class::BN, 608);
    // Unassigned code points take their block's default (@missing lines).
    RAYOPDF_CHECK(Bidi::ClassOf(0x05FF) == Bidi::Class::R && Bidi::ClassOf(0x07BF) == Bidi::Class::AL, 609);
    RAYOPDF_CHECK(Bidi::ClassOf(0x20CF) == Bidi::Class::ET && Bidi::ClassOf(0x1F600) == Bidi::Class::ON, 610);
    RAYOPDF_CHECK(Bidi::ClassOf(0x4E2D) == Bidi::Class::L && Bidi::ClassOf(0x10FFFF) == Bidi::Class::BN, 611);
    RAYOPDF_CHECK(Bidi::ClassOf(0x110000) == Bidi::Class::L && Bidi::ClassOf(0xFB1D) == Bidi::Class::R, 612);
    RAYOPDF_CHECK(Bidi::Mirror('(') == ')' && Bidi::Mirror(')') == '(' && Bidi::Mirror('<') == '>', 613);
    RAYOPDF_CHECK(Bidi::Mirror(0x00AB) == 0x00BB && Bidi::Mirror('a') == 'a' && Bidi::Mirror(0x2208) == 0x220B, 614);
    RAYOPDF_CHECK(Bidi::Mirror(0x110000) == 0x110000 && Bidi::Mirror(0xFF63) == 0xFF62, 615);
}

void TestReordering() {
    bool rtl = true;
    std::vector<uint8_t> levels;
    // Empty input.
    std::vector<uint32_t> order{7};
    RAYOPDF_CHECK(!Bidi::VisualToLogical(nullptr, 0, order, &levels) && order.empty() && levels.empty(), 616);

    // All left-to-right: unchanged, every level 0.
    RAYOPDF_CHECK(Logical("Hello, world (1.5)", &rtl, &levels) == "Hello, world (1.5)" && !rtl, 617);
    bool zero = true;
    for (uint8_t l : levels) zero = zero && l == 0;
    RAYOPDF_CHECK(zero, 618);

    // Hebrew: "שלום עולם" placed right to left reads back in logical order.
    RAYOPDF_CHECK(Logical("םלוע םולש", &rtl, &levels) == "שלום עולם" && rtl, 619);
    RAYOPDF_CHECK(levels.size() == 9 && levels[0] == 1 && levels[4] == 1, 620);

    // Digits inside right-to-left text stay left to right: "בשנת 2024 היה" shown as "היה 2024 תנשב".
    RAYOPDF_CHECK(Logical("היה 2024 תנשב", &rtl, &levels) == "בשנת 2024 היה" && rtl, 621);
    RAYOPDF_CHECK(levels[4] == 2 && levels[7] == 2 && levels[3] == 1, 622);
    // A decimal number with separators and a terminator: "מחיר 1,234.50% כאן".
    RAYOPDF_CHECK(Logical("ןאכ 1,234.50% ריחמ") == "מחיר 1,234.50% כאן", 623);
    // Two numbers separated by a space stay in right-to-left order: "שנים 1990 2000".
    RAYOPDF_CHECK(Logical("2000 1990 םינש") == "שנים 1990 2000", 624);

    // An embedded Latin phrase keeps its internal order: "קרא את the manual עכשיו".
    RAYOPDF_CHECK(Logical("וישכע the manual תא ארק") == "קרא את the manual עכשיו", 625);
    // Latin followed by digits inside Hebrew reads as one left-to-right run: "גרסה Windows 10 חדשה".
    RAYOPDF_CHECK(Logical("השדח Windows 10 הסרג") == "גרסה Windows 10 חדשה", 626);

    // Mirrored brackets: the glyphs look like "(...)" on the page; logically the first character is the
    // opening bracket, which the page showed mirrored. "(שלום) עולם" shown as "םלוע (םולש)".
    RAYOPDF_CHECK(Logical("םלוע (םולש)", &rtl, &levels) == "(שלום) עולם" && rtl, 627);
    RAYOPDF_CHECK(levels[4] == 1 && levels[9] == 1, 628);

    // Arabic with Arabic-Indic digits (AN) and European digits after Arabic letters (EN -> AN):
    // "عام ١٢٣ و 45" shown as "45 و ١٢٣ ماع".
    RAYOPDF_CHECK(Logical("45 و ١٢٣ ماع", &rtl) == "عام ١٢٣ و 45" && rtl, 629);

    // A left-to-right line with a Hebrew word: "see שלום here" shown as "see םולש here".
    RAYOPDF_CHECK(Logical("see םולש here", &rtl, &levels) == "see שלום here" && !rtl, 630);
    RAYOPDF_CHECK(levels[4] == 1 && levels[0] == 0 && levels[3] == 0 && levels[8] == 0, 631);
    // ... and with an Arabic-Indic number only: levels 2, order unchanged.
    RAYOPDF_CHECK(Logical("page ١٢ of", &rtl, &levels) == "page ١٢ of" && !rtl && levels[5] == 2, 632);

    // Trailing whitespace (logically at the end: the visual left of a right-to-left line) takes the
    // paragraph level (L1).
    RAYOPDF_CHECK(Logical("  םולש", &rtl, &levels) == "שלום  " && levels[0] == 1 && levels[1] == 1, 633);

    // A nonspacing mark keeps to its base: alef with dagesh (U+05BC), the mark placed left of it.
    RAYOPDF_CHECK(Logical("\xD7\x91\xD6\xBC\xD7\x90") == "\xD7\x90\xD6\xBC\xD7\x91", 634);

    // A Hebrew sentence inside a left-to-right document: its final period sits at the right edge.
    // Forced left-to-right, the period stays last; left to the majority rule it would come first.
    const std::vector<uint32_t> cps = CodePoints("תירבע.");
    std::vector<Bidi::Class> classes;
    for (uint32_t cp : cps) classes.push_back(Bidi::ClassOf(cp));
    rtl = Bidi::VisualToLogical(classes.data(), classes.size(), order, &levels, 0);
    RAYOPDF_CHECK(!rtl && order.size() == 6 && order[0] == 4 && order[4] == 0 && order[5] == 5 && levels[5] == 0, 636);
    RAYOPDF_CHECK(Bidi::VisualToLogical(classes.data(), classes.size(), order, &levels, 1) && order[0] == 5, 637);
    RAYOPDF_CHECK(Bidi::VisualToLogical(classes.data(), classes.size(), order, &levels) && order[0] == 5, 638);
    // Forcing a direction on a line without right-to-left items changes nothing.
    classes.assign(3, Bidi::Class::L);
    RAYOPDF_CHECK(!Bidi::VisualToLogical(classes.data(), 3, order, &levels, 1) && order[0] == 0 && levels[2] == 0, 639);

    // An embedding without its PDF runs to the line's end; the removed RLE keeps its place.
    RAYOPDF_CHECK(Logical("\xE2\x80\xAB" "בא") == "אב" "\xE2\x80\xAB", 635);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        long failures = 0;
        for (int i = 1; i < argc; ++i) {
            const long f = RunFile(argv[i]);
            RAYOPDF_CHECK(f >= 0, 815);
            failures += f;
        }
        RAYOPDF_CHECK(failures == 0, 816);
        return 0;
    }
    TestTables();
    TestReordering();
    TestResolver();
    TestVisualRoundTrips();
    std::puts("test_text_bidi: ok");
    return 0;
}
