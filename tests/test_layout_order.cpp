// SPDX-License-Identifier: Apache-2.0
// Core tests of the reading-order stage (src/layout/reading_order.cpp): columns, cuts and page
// furniture, on pages built glyph by glyph and converted by the whole pipeline. Exit codes 750-799
// and 1850-1899.
#include "check.h"

#include "common/utf8.h"
#include "layout/convert.h"
#include "layout/page.h"
#include "layout/pipeline.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace RayoPdf;
using namespace RayoPdf::Layout;

namespace {

// Appends `text` to `page` as upright glyphs of `size` points from (x, y), each half an em wide; a
// space is a space glyph.
void AddText(PageData& page, std::string_view text, float x, float y, float size) {
    size_t at = 0;
    uint32_t cp = 0;
    size_t length = 0;
    while (Utf8::Decode(text, at, cp, length)) {
        Glyph g;
        const float w = 0.5f * size;
        g.u0 = x, g.u1 = x + w, g.vb = y - 0.2f * size, g.vt = y + 0.8f * size, g.v = y;
        g.size = size;
        g.text = static_cast<uint32_t>(page.text.size());
        page.text.append(text.substr(at, length));
        g.textLength = static_cast<uint16_t>(length);
        if (cp == ' ') g.flags |= kGlyphSpace;
        page.glyphs.push_back(g);
        x += w;
        at += length;
    }
}

PageData NewPage() {
    PageData page;
    page.number = 1;
    page.width = 612;
    page.height = 792;
    return page;
}

std::string Run(std::vector<PageData>& pages) {
    std::string md;
    ConvertReport report;
    std::vector<PageTextQuality> quality(pages.size());
    ConvertPages(pages, quality, {}, false, md, report);
    if (std::getenv("RAYOPDF_TEST_VERBOSE")) std::fprintf(stderr, "---\n%s", md.c_str());
    return md;
}

size_t Find(const std::string& md, const char* needle) { return md.find(needle); }

// A running head inside the running band but below the outer margin, ending in a folio, set apart
// from the text: removed on a single page; the text stays.
void TestFolioHeader() {
    std::vector<PageData> pages{NewPage()};
    AddText(pages[0], "Journal of Testing 12(3) 39", 72, 735, 10);
    for (int i = 0; i < 20; ++i)
        AddText(pages[0], "Body line " + std::to_string(i) + " of the running text goes here.", 72,
                700 - 12.0f * i, 10);
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "Journal of Testing") == std::string::npos, 750);
    RAYOPDF_CHECK(Find(md, "Body line 0 of") != std::string::npos, 751);
    RAYOPDF_CHECK(Find(md, "Body line 19 of") != std::string::npos, 752);
}

// Reference labels set apart from their entries by a tab-sized gap are read with their entries,
// not as a column of their own.
void TestLabelColumn() {
    std::vector<PageData> pages{NewPage()};
    for (int i = 0; i < 6; ++i) {
        const float y = 700 - 30.0f * i;
        AddText(pages[0], "[" + std::to_string(i + 1) + "]", 57, y, 9);
        AddText(pages[0], "Reference number " + std::to_string(i + 1) + " by Author and Others,", 78, y, 9);
        AddText(pages[0], "Journal of Examples, volume " + std::to_string(i + 10) + ".", 78, y - 11, 9);
    }
    for (int i = 0; i < 14; ++i)
        AddText(pages[0], "Right column text line " + std::to_string(i) + " runs along here.", 330,
                700 - 11.0f * i, 9);
    const std::string md = Run(pages);
    const size_t entry1 = Find(md, "Reference number 1"), label2 = Find(md, "[2]"), entry2 = Find(md, "Reference number 2");
    RAYOPDF_CHECK(entry1 != std::string::npos && label2 != std::string::npos && entry2 != std::string::npos, 753);
    RAYOPDF_CHECK(entry1 < label2 && label2 < entry2, 754);
}

// A column holding a display formula (many short pieces with few characters) is still a column of
// text: the left column is read to its end before the right one starts.
void TestFormulaColumn() {
    std::vector<PageData> pages{NewPage()};
    for (int i = 0; i < 12; ++i)
        AddText(pages[0], "Left column line " + std::to_string(i) + " alpha beta gamma.", 72, 700 - 12.0f * i, 10);
    for (int i = 0; i < 3; ++i)
        AddText(pages[0], "Right top line " + std::to_string(i) + " alpha beta gamma.", 330, 700 - 12.0f * i, 10);
    const char* pieces[] = {"X", "E =", "w", "(1)", "i", "X", "F =", "v", "(2)"};
    for (int i = 0; i < 9; ++i) AddText(pages[0], pieces[i], 340 + 40.0f * (i % 4), 664 - 12.0f * (i / 2 + 1), 10);
    for (int i = 0; i < 3; ++i)
        AddText(pages[0], "Right end line " + std::to_string(i) + " alpha beta gamma.", 330, 580 - 12.0f * i, 10);
    const std::string md = Run(pages);
    const size_t leftEnd = Find(md, "Left column line 11"), rightTop = Find(md, "Right top line 0");
    RAYOPDF_CHECK(leftEnd != std::string::npos && rightTop != std::string::npos, 755);
    RAYOPDF_CHECK(leftEnd < rightTop, 756);
}

// Double-spaced text set high on the page keeps its first line: a header must be set apart by
// more than the text's own line spacing.
void TestDoubleSpacedTop() {
    std::vector<PageData> pages{NewPage()};
    for (int i = 0; i < 20; ++i)
        AddText(pages[0], "Spaced line " + std::to_string(i) + " of a double-spaced letter.", 72,
                760 - 30.0f * i, 12);
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "Spaced line 0 of") != std::string::npos, 757);
    RAYOPDF_CHECK(Find(md, "Spaced line 19 of") != std::string::npos, 758);
}


// Two rows of small print at the top (a journal line and its DOI) set close together and apart from
// the text as a block: both are furniture, even above a heading.
void TestMetadataBlock() {
    std::vector<PageData> pages{NewPage()};
    AddText(pages[0], "Journal of Examples 4(2), 2021", 72, 760, 8);
    AddText(pages[0], "DOI: https://doi.org/10.1000/example", 72, 750, 8);
    AddText(pages[0], "A Section Title", 72, 715, 14);
    for (int i = 0; i < 20; ++i)
        AddText(pages[0], "Text line " + std::to_string(i) + " of the article body runs here.", 72, 690 - 12.0f * i, 10);
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "doi.org") == std::string::npos, 759);
    RAYOPDF_CHECK(Find(md, "Journal of Examples") == std::string::npos, 760);
    RAYOPDF_CHECK(Find(md, "A Section Title") != std::string::npos && Find(md, "Text line 19 of") != std::string::npos, 761);
}

// A column of short list items beside a column of prose is a column too: the list is read to
// its end before the prose.
void TestListColumn() {
    std::vector<PageData> pages{NewPage()};
    for (int i = 0; i < 12; ++i) AddText(pages[0], "- item " + std::to_string(i + 1), 72, 700 - 12.0f * i, 10);
    AddText(pages[0], "More items", 72, 540, 12);
    for (int i = 0; i < 8; ++i) AddText(pages[0], "- extra " + std::to_string(i + 1), 72, 520 - 12.0f * i, 10);
    for (int i = 0; i < 10; ++i)
        AddText(pages[0], "Right prose line " + std::to_string(i) + " alpha beta gamma.", 330, 700 - 11.0f * i, 10);
    const std::string md = Run(pages);
    const size_t lastItem = Find(md, "item 12"), prose = Find(md, "Right prose line 0");
    RAYOPDF_CHECK(lastItem != std::string::npos && prose != std::string::npos, 762);
    RAYOPDF_CHECK(lastItem < prose, 763);
}

} // namespace

// A nearly empty page (a scanned page's text layer: a title line and a repository footer) loses
// its URL footer; its other lines stay.
void TestSparseMetadata() {
    std::vector<PageData> pages{NewPage()};
    AddText(pages[0], "New Directions, Vol. 11, Art. 4", 242, 763, 10);
    AddText(pages[0], "http://dh.example.edu/newdirections/vol11/iss4/4", 22, 22, 10);
    AddText(pages[0], "NEW DIRECTIONS JULY 1984 2", 489, 19, 6);
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "dh.example.edu") == std::string::npos, 764);
    RAYOPDF_CHECK(Find(md, "New Directions, Vol. 11") != std::string::npos, 765);
}

// A lone page number a little further in than the running band, set apart from the text, is
// still a page number.
void TestInnerFolio() {
    std::vector<PageData> pages{NewPage()};
    for (int i = 0; i < 30; ++i)
        AddText(pages[0], "Body line " + std::to_string(i) + " of a page with deep margins here.", 90,
                650 - 12.0f * i, 10);
    AddText(pages[0], "259", 300, 105, 10);
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "259") == std::string::npos, 766);
    RAYOPDF_CHECK(Find(md, "Body line 29 of") != std::string::npos, 767);
}

// A running title set off the text's left edge (right-aligned) inside the running band is removed;
// a footer whose folio sits on a slightly lower baseline beside it goes with it.
void TestOffsetRows() {
    std::vector<PageData> pages{NewPage()};
    AddText(pages[0], "Effects of Red Fruit Oil on Colitis Cancer", 330, 720, 10);
    for (int i = 0; i < 30; ++i)
        AddText(pages[0], "Body line " + std::to_string(i) + " of the article text runs on.", 72, 690 - 12.0f * i, 10);
    AddText(pages[0], "J Example Med Assoc, Volume 63, Number 3, 2013", 330, 46, 8);
    AddText(pages[0], "110", 72, 43, 8);
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "Red Fruit Oil") == std::string::npos, 768);
    RAYOPDF_CHECK(Find(md, "Example Med Assoc") == std::string::npos, 769);
    RAYOPDF_CHECK(Find(md, "Body line 0 of") != std::string::npos && Find(md, "Body line 29 of") != std::string::npos, 770);
}

// An equation's number is a paragraph of its own, never a list item: alone at the right of its
// formula's line, at the end of that line a quad after the formula, and on the left of a formula;
// after two equations a quad apart, a little more than a quad after them. "(3)" as the last word
// of a sentence stays in it, and "(1)" before its item's text is a marker.
void TestEquationNumbers() {
    std::vector<PageData> pages{NewPage()};
    for (int i = 0; i < 4; ++i)
        AddText(pages[0], "Body line " + std::to_string(i) + " of the article runs on to the margin here", 72,
                740 - 12.0f * i, 10);
    AddText(pages[0], "E = mc + pq", 200, 680, 10);
    AddText(pages[0], "(13)", 520, 680, 10);
    AddText(pages[0], "As the formula shows, the energy is given by (3)", 72, 660, 10);
    AddText(pages[0], "and the text goes on in the next line of the paragraph.", 72, 648, 10);
    AddText(pages[0], "F = ma + kx,", 200, 620, 10);
    AddText(pages[0], "(14)", 200 + 12 * 5 + 10, 620, 10);
    AddText(pages[0], "(15)", 72, 590, 10);
    AddText(pages[0], "G = ab + cd", 200, 590, 10);
    AddText(pages[0], "(1)", 72, 560, 10);
    AddText(pages[0], "The first item of a list set with a tab.", 100, 560, 10);
    AddText(pages[0], "a = b,", 150, 530, 10);
    AddText(pages[0], "c = d,", 192, 530, 10);
    AddText(pages[0], "(16)", 235, 530, 10);
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "\n\n(13)\n") != std::string::npos && Find(md, "- (13)") == std::string::npos, 771);
    RAYOPDF_CHECK(Find(md, "given by (3) and the text") != std::string::npos, 772);
    RAYOPDF_CHECK(Find(md, "F = ma + kx,\n\n(14)\n") != std::string::npos, 773);
    RAYOPDF_CHECK(Find(md, "\n\n(15)\n\nG = ab + cd") != std::string::npos, 774);
    RAYOPDF_CHECK(Find(md, "(1) The first item") != std::string::npos, 775);
    RAYOPDF_CHECK(Find(md, "c = d,\n\n(16)") != std::string::npos, 786);
}

// A double-spaced manuscript: the lines of a paragraph are two ems apart and still one paragraph;
// its title in a larger size, spaced in proportion, is one heading; an indented line starts the
// next paragraph. In a single-spaced document a gap of two ems still parts its paragraphs.
void TestWideLeading() {
    std::vector<PageData> pages{NewPage()};
    AddText(pages[0], "A Long Title of the Manuscript", 72, 740, 16);
    AddText(pages[0], "That Takes Two Lines", 72, 740 - 33, 16);
    float y = 660;
    for (int p = 0; p < 3; ++p) {
        for (int i = 0; i < 5; ++i, y -= 20) {
            const bool last = i == 4;
            AddText(pages[0],
                    std::string(i == 0 ? "Paragraph " + std::to_string(p) + " starts here and" : "goes on with more words and") +
                        (last ? " ends." : " then it"),
                    i == 0 ? 87.0f : 72.0f, y, 10);
        }
    }
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "A Long Title of the Manuscript That Takes Two Lines\n") != std::string::npos, 776);
    RAYOPDF_CHECK(Find(md, "Paragraph 0 starts here and then it goes on with more words and then it goes on") != std::string::npos, 777);
    RAYOPDF_CHECK(Find(md, "ends.\n\nParagraph 1 starts here") != std::string::npos, 778);
    std::vector<PageData> single{NewPage()};
    y = 700;
    for (int p = 0; p < 3; ++p, y -= 12) {
        for (int i = 0; i < 5; ++i, y -= 12)
            AddText(single[0], std::string(i == 0 ? "Block " + std::to_string(p) + " starts here and" : "goes on with more words and") +
                                   (i == 4 ? " ends" : " then it"), 72, y, 10);
    }
    const std::string spaced = Run(single);
    RAYOPDF_CHECK(Find(spaced, "ends\n\nBlock 1 starts") != std::string::npos && Find(spaced, "ends\n\nBlock 2 starts") != std::string::npos, 779);
}

// A paragraph broken by a figure set into its column runs on below the figure's caption in
// lowercase: one paragraph again, the caption before it. A body paragraph that names a table
// ("Table 1 shows …") is no float: the text around it stays apart.
void TestParagraphAroundFloat() {
    const auto build = [](const char* between) {
        std::vector<PageData> pages{NewPage()};
        PageData& page = pages[0];
        const char* before[] = {"The opening paragraph of the section starts with words that fill a whole line of",
                                "the column and goes on with more words in the same manner over the full width of",
                                "the column until the text breaks off in the middle of a sentence near the figure"};
        for (int i = 0; i < 3; ++i) AddText(page, before[i], 72, 700.0f - 12 * i, 10);
        ImageBox figure;
        figure.x0 = 150, figure.x1 = 450, figure.y0 = 470, figure.y1 = 640;
        page.images.push_back(figure);
        AddText(page, between, 72, 440, 9);
        const char* after[] = {"and the sentence goes on below the figure with words that fill the line of text again",
                               "until the paragraph ends."};
        for (int i = 0; i < 2; ++i) AddText(page, after[i], 72, 400.0f - 12 * i, 10);
        return Run(pages);
    };
    const std::string md = build("Figure 1: A drawing of the setup.");
    const size_t caption = Find(md, "Figure 1: A drawing"), start = Find(md, "The opening paragraph");
    RAYOPDF_CHECK(Find(md, "near the figure and the sentence goes on below") != std::string::npos, 780);
    RAYOPDF_CHECK(caption != std::string::npos && caption < start, 781);
    const std::string body = build("Table 1 shows the setup.");
    RAYOPDF_CHECK(Find(body, "near the figure\n\nTable 1 shows") != std::string::npos, 782);
}

// Authors' addresses printed after the bibliography under a title block whose author line is set
// in capitals (amsart) move to before the abstract, in their order; under an author line in
// mixed case they stay at the end, where the source put them.
void TestAddressesAtEnd() {
    const auto build = [](const char* authors) {
        std::vector<PageData> pages{NewPage(), NewPage()};
        pages[1].number = 2;
        AddText(pages[0], "A Title of the Paper", 72, 740, 16);
        AddText(pages[0], authors, 72, 712, 10);
        AddText(pages[0], "Abstract. We study a question of the test and answer it here.", 72, 684, 9);
        for (int i = 0; i < 30; ++i)
            AddText(pages[0], "Body line " + std::to_string(i) + " of the running text goes on here.", 72,
                    650 - 12.0f * i, 10);
        for (int i = 0; i < 10; ++i)
            AddText(pages[1], "More body text on the second page, line " + std::to_string(i) + ".", 72,
                    700 - 12.0f * i, 10);
        AddText(pages[1], "References", 72, 560, 12);
        AddText(pages[1], "[1] A. Author, A paper of the field, Journal 1 (2000), 1-2.", 72, 540, 9);
        AddText(pages[1], "Department of Mathematics, University of Alpha, Alphaville", 72, 500, 9);
        AddText(pages[1], "E-mail address: ann@alpha.edu", 72, 488, 9);
        AddText(pages[1], "Bob Writer, Institute of Beta, Betatown", 72, 464, 9);
        AddText(pages[1], "E-mail address: bob@beta.org", 72, 452, 9);
        return Run(pages);
    };
    const std::string md = build("ANN AUTHOR AND BOB WRITER");
    const size_t authors = Find(md, "ANN AUTHOR"), abstract = Find(md, "Abstract."), first = Find(md, "Department of"),
                 second = Find(md, "Bob Writer, Institute");
    RAYOPDF_CHECK(authors != std::string::npos && authors < first && first < second && second < abstract, 783);
    RAYOPDF_CHECK(Find(md, "bob@beta.org") < abstract && Find(md, "A paper of the field") != std::string::npos, 784);
    const std::string mixed = build("Ann Author and Bob Writer");
    RAYOPDF_CHECK(Find(mixed, "A paper of the field") < Find(mixed, "Department of"), 785);
}

// Body lines of `size` points from `top` down, `count` of them, 1.2 em apart, from x = 72.
void AddBody(PageData& page, int count, float top, float size) {
    for (int i = 0; i < count; ++i)
        AddText(page, "Body line " + std::to_string(i) + " of the running text goes on here.", 72, top - 1.2f * size * i,
                size);
}

// Marks the glyphs from `from` on as drawn inside /Artifact marked content.
void MarkArtifact(PageData& page, size_t from) {
    for (size_t i = from; i < page.glyphs.size(); ++i) page.glyphs[i].flags |= kGlyphArtifact;
}

// A head the producer marked as an artifact goes even set close above the text in the body face;
// the same line unmarked stays, and so does every line of a page whose text is all artifacts, and a
// marked row longer than a head (a disclaimer sentence).
void TestArtifactRows() {
    const auto build = [](const char* head, bool marked, bool all) {
        std::vector<PageData> pages{NewPage()};
        AddText(pages[0], head, 72, 718, 10);
        if (marked) MarkArtifact(pages[0], 0);
        const size_t body = pages[0].glyphs.size();
        AddBody(pages[0], 30, 700, 10);
        if (all) MarkArtifact(pages[0], body);
        return Run(pages);
    };
    const char* head = "Food Act 2001 - 14.12.2017";
    const std::string md = build(head, true, false);
    RAYOPDF_CHECK(Find(md, "Food Act") == std::string::npos && Find(md, "Body line 0 of") != std::string::npos, 787);
    RAYOPDF_CHECK(Find(build(head, false, false), "Food Act") != std::string::npos, 788);
    RAYOPDF_CHECK(Find(build(head, true, true), "Food Act") != std::string::npos, 789);
    RAYOPDF_CHECK(Find(build("This project has been funded with the support of the European Commission and its partners.", true,
                             false),
                       "funded with the support") != std::string::npos,
                  1855);
}

// A right-aligned name over a URL at the head of a page: the name row carries evidence of its own
// (set off the text's edge) and goes with the metadata row under it. Two rows of small print at
// the foot, set apart but with no metadata, stay: a caption or a note.
void TestNameOverUrl() {
    std::vector<PageData> pages{NewPage()};
    AddText(pages[0], "Jonathan Example", 480, 740, 10);
    AddText(pages[0], "http://cap.example.edu/profiles/Jonathan_Example/", 330, 728, 10);
    AddBody(pages[0], 40, 705, 8);
    AddText(pages[0], "A remark set in small print below the text,", 72, 70, 7);
    AddText(pages[0], "continued on a second line.", 72, 62, 7);
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "Jonathan Example") == std::string::npos && Find(md, "cap.example.edu") == std::string::npos &&
                      Find(md, "Body line 0 of") != std::string::npos,
                  790);
    RAYOPDF_CHECK(Find(md, "A remark set in small print") != std::string::npos && Find(md, "second line") != std::string::npos,
                  791);
}

// A running head of a folio and a title set 14.5 % down a page with wide margins is removed; a
// single line there ("Chapter 2", which opens a chapter) stays.
void TestDeepFolioHead() {
    const auto build = [](bool folio) {
        std::vector<PageData> pages{NewPage()};
        pages[0].height = 842;
        if (folio) {
            AddText(pages[0], "164", 106, 720, 9);
            AddText(pages[0], "Adam A. Zych", 270, 720, 9);
        } else {
            AddText(pages[0], "Chapter 2", 106, 720, 9);
        }
        AddBody(pages[0], 12, 691, 9);
        return Run(pages);
    };
    const std::string md = build(true);
    RAYOPDF_CHECK(Find(md, "Zych") == std::string::npos && Find(md, "Body line 0 of") != std::string::npos, 792);
    RAYOPDF_CHECK(Find(build(false), "Chapter 2") != std::string::npos, 793);
}

// A foot row spread across the text, under which download stamps reach far into the left margin:
// the row's spread is measured against the text between the running bands, so it goes too.
void TestStampedFoot() {
    std::vector<PageData> pages{NewPage()};
    AddBody(pages[0], 50, 720, 8);
    AddText(pages[0], "Optical Engineering", 72, 40, 8);
    AddText(pages[0], "090901-22", 290, 40, 8);
    AddText(pages[0], "September 2013/Vol. 52(9)", 440, 40, 8);
    AddText(pages[0], "Downloaded From: https://www.example.org/journals/x on 13 Jun 2021", 10, 20, 7);
    AddText(pages[0], "Terms of Use: https://www.example.org/terms-of-use", 10, 12, 7);
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "090901-22") == std::string::npos && Find(md, "Downloaded From") == std::string::npos &&
                      Find(md, "Body line 49 of") != std::string::npos,
                  794);
}

// A line opening with a literal '#' (a comment of a listing) is no folio: at the head of a page,
// set apart, it stays.
void TestHashIsNoFolio() {
    std::vector<PageData> pages{NewPage()};
    AddText(pages[0], "# create the train and valid datasets", 72, 735, 10);
    AddBody(pages[0], 30, 700, 10);
    RAYOPDF_CHECK(Find(Run(pages), "create the train") != std::string::npos, 795);
}

// A footnote whose marker is followed by a no-break space is still a footnote: under a removed
// page number it stays, though it holds a URL.
void TestNoBreakFootnote() {
    std::vector<PageData> pages{NewPage()};
    AddBody(pages[0], 40, 720, 10);
    AddText(pages[0], "15\xC2\xA0https://www.example.gov/content/5524", 72, 60, 8);
    AddText(pages[0], "17", 300, 48, 10);
    RAYOPDF_CHECK(Find(Run(pages), "example.gov/content/5524") != std::string::npos, 796);
}

// "Página 6 / 12" set in bold (no running title) is a page number.
void TestPageWords() {
    std::vector<PageData> pages{NewPage()};
    AddBody(pages[0], 40, 720, 10);
    const size_t from = pages[0].glyphs.size();
    AddText(pages[0], "P\xC3\xA1gina 6 / 12", 480, 50, 10);
    for (size_t i = from; i < pages[0].glyphs.size(); ++i) pages[0].glyphs[i].flags |= kGlyphBold;
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "gina 6") == std::string::npos && Find(md, "Body line 39 of") != std::string::npos, 797);
}

// A supplement's page number ("S84", bold in the corner) and an electronic page's ("e123") are page
// numbers.
void TestLetteredPageNumbers() {
    for (const char* folio : {"S84", "e123"}) {
        std::vector<PageData> pages{NewPage()};
        AddBody(pages[0], 40, 720, 10);
        const size_t from = pages[0].glyphs.size();
        AddText(pages[0], folio, 40, 50, 9);
        for (size_t i = from; i < pages[0].glyphs.size(); ++i) pages[0].glyphs[i].flags |= kGlyphBold;
        const std::string md = Run(pages);
        RAYOPDF_CHECK(Find(md, folio) == std::string::npos && Find(md, "Body line 39 of") != std::string::npos, 1856);
    }
}

// A journal masthead of five rows of small print over the text goes as one block.
void TestFiveRowMasthead() {
    std::vector<PageData> pages{NewPage()};
    const char* rows[] = {"Supplemental material to this article can be found at:", "http://journal.example.org/content/suppl/1",
                          "0026-895X/11/7906-1044-1052$25.00", "MOLECULAR EXAMPLES Vol. 79, No. 6",
                          "Mol Examples 79:1044-1052, 2011 Printed in U.S.A."};
    for (int i = 0; i < 5; ++i) AddText(pages[0], rows[i], 72, 770 - 8.0f * i, 6);
    AddBody(pages[0], 40, 700, 10);
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "Supplemental material") == std::string::npos && Find(md, "MOLECULAR EXAMPLES") == std::string::npos &&
                      Find(md, "Printed in") == std::string::npos && Find(md, "Body line 0 of") != std::string::npos,
                  798);
}

// A page number or a folio row closer to the text than an em but set apart by more than the line
// spacing plus a quarter em: a lone number centred on the text, or a folio row of separate pieces
// reaching the text's edge, goes. A number flush left at that gap, and a folio row at the text's own
// line spacing, stay.
void TestCloseFolioRows() {
    // 55 body lines of 10 points from 720 down to 72 (box bottom 70), 2 points apart; the text runs
    // from 72 to 302. A row at baseline 55 has its top at 63: 7 points apart.
    const auto build = [](float x, float y, bool folio) {
        std::vector<PageData> pages{NewPage()};
        AddBody(pages[0], 55, 720, 10);
        if (folio) {
            AddText(pages[0], "Journal of Testing", 72, y, 10);
            AddText(pages[0], "39", x, y, 10);
        } else {
            AddText(pages[0], "17", x, y, 10);
        }
        return Run(pages);
    };
    std::string md = build(182, 55, false);
    RAYOPDF_CHECK(Find(md, "\n17\n") == std::string::npos && Find(md, "Body line 54 of") != std::string::npos, 1850);
    RAYOPDF_CHECK(Find(build(72, 55, false), "\n17\n") != std::string::npos, 1851);
    md = build(250, 55, true);
    RAYOPDF_CHECK(Find(md, "Journal of Testing") == std::string::npos && Find(md, "Body line 54 of") != std::string::npos,
                  1852);
    RAYOPDF_CHECK(Find(build(250, 60, true), "Journal of Testing") != std::string::npos, 1853);
}

// A body-size row at the head of a page overlapping a row of small print under it, set apart from
// the text, goes with that row.
void TestOverlappingHeadRow() {
    std::vector<PageData> pages{NewPage()};
    AddText(pages[0], "Research Article", 72, 745, 10);
    AddText(pages[0], "Journal of Testing 12(3), 2024", 100, 741, 7);
    AddBody(pages[0], 40, 705, 10);
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "Research Article") == std::string::npos && Find(md, "Journal of Testing") == std::string::npos &&
                      Find(md, "Body line 0 of") != std::string::npos,
                  1854);
}

// A row in the outer tenth of the page, a little larger than the body, set apart by three ems: a
// journal line at the head or a print stamp at the foot goes (neither in the outer margin nor
// smaller than the body). A bold title of that size stays.
void TestWideGapRows() {
    const auto build = [](const char* row, float y, bool bold) {
        std::vector<PageData> pages{NewPage()};
        const size_t from = pages[0].glyphs.size();
        AddText(pages[0], row, 72, y, 12);
        if (bold)
            for (size_t i = from; i < pages[0].glyphs.size(); ++i) pages[0].glyphs[i].flags |= kGlyphBold;
        // 50 body lines from 681 down to 93: the head row at 730 (7 % from the top, its bottom 727.6)
        // and the foot row at 51 (7 % from the bottom, its top 60.6) each lie 30 points (3 em) away.
        AddBody(pages[0], 50, 681, 10);
        return Run(pages);
    };
    std::string md = build("Journal of Testing 12 (2020)", 730, false);
    RAYOPDF_CHECK(Find(md, "Journal of Testing") == std::string::npos && Find(md, "Body line 0 of") != std::string::npos,
                  1857);
    RAYOPDF_CHECK(Find(build("A Study of Testing", 730, true), "A Study of Testing") != std::string::npos, 1858);
    md = build("Printed: Fri, 19 Aug 2022", 51, false);
    RAYOPDF_CHECK(Find(md, "Printed:") == std::string::npos && Find(md, "Body line 49 of") != std::string::npos, 1859);
}

int main() {
    TestFolioHeader();
    TestLabelColumn();
    TestFormulaColumn();
    TestDoubleSpacedTop();
    TestMetadataBlock();
    TestListColumn();
    TestSparseMetadata();
    TestInnerFolio();
    TestOffsetRows();
    TestEquationNumbers();
    TestWideLeading();
    TestParagraphAroundFloat();
    TestAddressesAtEnd();
    TestArtifactRows();
    TestNameOverUrl();
    TestDeepFolioHead();
    TestStampedFoot();
    TestHashIsNoFolio();
    TestNoBreakFootnote();
    TestPageWords();
    TestLetteredPageNumbers();
    TestFiveRowMasthead();
    TestCloseFolioRows();
    TestOverlappingHeadRow();
    TestWideGapRows();
    std::printf("test_layout_order: ok\n");
    return 0;
}
