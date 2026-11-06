// SPDX-License-Identifier: Apache-2.0
// Core tests of src/text/bidi (exit codes 600-639): the generated UAX #9 tables and visual ->
// logical reordering of lines as a PDF shows them.

#include "check.h"

#include "common/utf8.h"
#include "text/bidi.h"

#include <cstdint>
#include <cstdio>
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

    // Explicit formatting characters do not break the line apart (treated as BN / ON).
    RAYOPDF_CHECK(Logical("\xE2\x80\xAB" "בא") == "אב" "\xE2\x80\xAB", 635);
}

} // namespace

int main() {
    TestTables();
    TestReordering();
    std::puts("test_text_bidi: ok");
    return 0;
}
