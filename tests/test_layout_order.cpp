// SPDX-License-Identifier: Apache-2.0
// Core tests of the reading-order stage (src/layout/reading_order.cpp): columns, cuts and page
// furniture, on pages built glyph by glyph and converted by the whole pipeline. Exit codes 750-799,
// 1850-1899 and, for page numbers counted up from page to page, 2155-2156.
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

// An address run's first paragraph, too long for an address line, names an author whose name the
// author line sets in capitals with an accent ("JÓZSEF" for "József"): the run moves whole.
void TestAddressesAccentedCapitals() {
    std::vector<PageData> pages{NewPage(), NewPage()};
    pages[1].number = 2;
    AddText(pages[0], "A Title of the Paper", 72, 740, 16);
    AddText(pages[0], "JÓZSEF FARKAS AND BOB WRITER", 72, 712, 10);
    AddText(pages[0], "Abstract. We study a question of the test and answer it here.", 72, 684, 9);
    for (int i = 0; i < 30; ++i)
        AddText(pages[0], "Body line " + std::to_string(i) + " of the running text goes on here.", 72, 650 - 12.0f * i, 10);
    for (int i = 0; i < 10; ++i)
        AddText(pages[1], "More body text on the second page, line " + std::to_string(i) + ".", 72, 700 - 12.0f * i, 10);
    AddText(pages[1], "References", 72, 560, 12);
    AddText(pages[1], "[1] A. Author, A paper of the field, Journal 1 (2000), 1-2.", 72, 540, 9);
    AddText(pages[1],
            "József Farkas, Division of Computing Science and Mathematics, University of Stirling, "
            "Stirling FK9 4LA, Scotland, United Kingdom",
            72, 500, 8);
    AddText(pages[1], "E-mail address: jf@stir.ac.uk", 72, 488, 8);
    AddText(pages[1], "Bob Writer, Institute of Beta, Betatown", 72, 464, 8);
    AddText(pages[1], "E-mail address: bob@beta.org", 72, 452, 8);
    const std::string md = Run(pages);
    const size_t first = Find(md, "József Farkas, Division"), abstract = Find(md, "Abstract.");
    RAYOPDF_CHECK(first != std::string::npos && first < Find(md, "Bob Writer, Institute") && Find(md, "bob@beta.org") < abstract,
                  1866);
}

// amsart's labels of several authors' addresses: an author's surname in parentheses starts an
// address that ends with a full stop, and a web address set in typewriter type (a code block)
// stands between; the run moves whole.
void TestAddressesSurnameLabels() {
    std::vector<PageData> pages{NewPage(), NewPage()};
    pages[1].number = 2;
    AddText(pages[0], "A Title of the Paper", 72, 740, 16);
    AddText(pages[0], "ANN AUTHOR AND BOB WRITER", 72, 712, 10);
    AddText(pages[0], "Abstract. We study a question of the test and answer it here.", 72, 684, 9);
    for (int i = 0; i < 30; ++i)
        AddText(pages[0], "Body line " + std::to_string(i) + " of the running text goes on here.", 72, 650 - 12.0f * i, 10);
    for (int i = 0; i < 10; ++i)
        AddText(pages[1], "More body text on the second page, line " + std::to_string(i) + ".", 72, 700 - 12.0f * i, 10);
    AddText(pages[1], "References", 72, 560, 12);
    AddText(pages[1], "[1] A. Author, A paper of the field, Journal 1 (2000), 1-2.", 72, 540, 9);
    AddText(pages[1], "(Author) Department of Mathematics, University of Alpha, Alphaville.", 72, 500, 8);
    AddText(pages[1], "E-mail address: ann@alpha.edu", 72, 488, 8);
    const size_t mono = pages[1].glyphs.size();
    AddText(pages[1], "http://alpha.edu/~ann/", 72, 464, 8);
    for (size_t i = mono; i < pages[1].glyphs.size(); ++i) pages[1].glyphs[i].flags |= kGlyphMonospace;
    AddText(pages[1], "(Writer) Institute of Beta, Betatown.", 72, 440, 8);
    AddText(pages[1], "E-mail address: bob@beta.org", 72, 428, 8);
    const std::string md = Run(pages);
    const size_t first = Find(md, "(Author) Department"), url = Find(md, "```\nhttp://alpha.edu"),
                 abstract = Find(md, "Abstract.");
    RAYOPDF_CHECK(first < url && url < Find(md, "(Writer) Institute") && Find(md, "bob@beta.org") < abstract, 1867);
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

// A lone page number at the foot, centred on the text: set between two dashes ("- 7 -", whose first
// dash is no list marker), or set apart from double-spaced text by less than its line spacing plus a
// quarter em but more than half an em. Both go; a list item "- 7" under the text, flush with it,
// stays, and so does a flush-left number at the same gap.
void TestFootPageNumbers() {
    // 25 body lines of 10 points, `step` points apart, from 700 down; the text runs from 72 to 307.
    const auto build = [](const char* number, float x, float y, float step) {
        std::vector<PageData> pages{NewPage()};
        for (int i = 0; i < 25; ++i)
            AddText(pages[0], "Body line " + std::to_string(i) + " of the running text goes on here.", 72,
                    700 - step * i, 10);
        AddText(pages[0], number, x, y, 10);
        return Run(pages);
    };
    // Double spacing (a 14-point gap between lines): the last line at 124, its box bottom at 122; a
    // number at 98 has its top at 106, 16 points below: under the spacing plus a quarter em.
    std::string md = build("7", 187, 98, 24);
    RAYOPDF_CHECK(Find(md, "\n7\n") == std::string::npos && Find(md, "Body line 24 of") != std::string::npos, 1880);
    RAYOPDF_CHECK(Find(build("7", 72, 98, 24), "\n7\n") != std::string::npos, 1881);
    // Single spacing: the last line at 412; "- 7 -" at 30, in the outer margin, is far below it.
    md = build("- 7 -", 180, 30, 12);
    RAYOPDF_CHECK(Find(md, "7 -") == std::string::npos && Find(md, "Body line 24 of") != std::string::npos, 1882);
    RAYOPDF_CHECK(Find(build("- 7", 72, 98, 24), "- 7\n") != std::string::npos, 1883);
}

// Two columns of running text, each ending in smaller notes (a numbered note, a marked one): the
// body runs on from the first column to the second, the notes come after it. A caption at the foot
// of a column is no note and stays where it is read.
void TestNotesAfterColumns() {
    const auto build = [](const char* leftFoot) {
        std::vector<PageData> pages{NewPage()};
        for (int i = 0; i < 20; ++i) {
            AddText(pages[0], "Left body line " + std::to_string(i) + " runs on here.", 72, 380.0f - 12 * i, 10);
            AddText(pages[0], "Right body line " + std::to_string(i) + " runs on too.", 320, 380.0f - 12 * i, 10);
        }
        AddText(pages[0], leftFoot, 72, 120, 8);
        AddText(pages[0], "goes on in a second line of notes.", 72, 110, 8);
        AddText(pages[0], "\xE2\x80\xA0The right note says what it says,", 320, 120, 8);
        AddText(pages[0], "and its second line ends it here.", 320, 110, 8);
        return Run(pages);
    };
    std::string md = build("1 The left note tells its source and");
    RAYOPDF_CHECK(Find(md, "Right body line 0") != std::string::npos && Find(md, "1 The left note") != std::string::npos &&
                      Find(md, "The right note") != std::string::npos && Find(md, "Right body line 19") < Find(md, "1 The left note") &&
                      Find(md, "1 The left note") < Find(md, "The right note"),
                  1860);
    md = build("Figure 3. A caption under the column and");
    RAYOPDF_CHECK(Find(md, "Figure 3. A caption") != std::string::npos && Find(md, "Figure 3. A caption") < Find(md, "Right body line 0"),
                  1861);
}

// Page numbers 17 to 26 on ten pages, each on a line set in the text's own pitch between the
// text's last line (which says something else on every page) and a bottom line that differs on
// every page, so not the outermost line: the numbers go, counted up from page to page (or by the
// counts and the edge rows); every other line stays.
void TestCountedPageNumbers() {
    std::vector<PageData> pages;
    const char* const notes[] = {"Printed in Lisbon", "Draft for review", "Copy two of six", "Internal use only",
                                 "Revised in March", "Archive edition", "Second printing", "Field copy",
                                 "Proof for authors", "Library copy"};
    const char* const last[] = {"and the harbour froze early", "while grain prices doubled", "as families left in spring",
                                "when the new road arrived", "once teachers came by train", "after the mill burned",
                                "before the records were lost", "where two bridges stood", "during a long drought",
                                "until the town recovered"};
    for (int p = 0; p < 10; ++p) {
        pages.push_back(NewPage());
        pages.back().number = p + 1;
        for (int i = 0; i < 49; ++i)
            AddText(pages.back(), "Text line " + std::to_string(i) + " of page " + std::to_string(p + 1) + " goes on.",
                    72, 690 - 12.0f * i, 10);
        AddText(pages.back(), last[p], 72, 102, 10);
        AddText(pages.back(), std::to_string(17 + p), 300, 90, 10);
        AddText(pages.back(), notes[p], 72, 78, 10);
    }
    const std::string md = Run(pages);
    bool numbers = false;
    for (int v = 17; v <= 26; ++v) numbers = numbers || Find(md, ("\n" + std::to_string(v) + "\n").c_str()) != std::string::npos;
    RAYOPDF_CHECK(!numbers && Find(md, "Proof for authors") != std::string::npos && Find(md, "during a long drought") != std::string::npos,
                  2155);
}

// Ten pages whose first lines, high in the top band, say different things: nothing is removed.
void TestDifferentFirstLines() {
    std::vector<PageData> pages;
    const char* const openings[] = {"The harbour froze early that winter",  "Grain prices doubled within a month",
                                    "Most families left by the spring",     "A new road reached the valley in May",
                                    "Teachers arrived with the first train", "The mill burned down twice",
                                    "Records of the council are lost",      "Two bridges were built by hand",
                                    "Letters describe a long drought",      "By autumn the town had recovered"};
    for (int p = 0; p < 10; ++p) {
        pages.push_back(NewPage());
        pages.back().number = p + 1;
        AddText(pages.back(), openings[p], 72, 740, 10);
        for (int i = 0; i < 20; ++i)
            AddText(pages.back(), "Plain body line number " + std::to_string(i) + " on its page", 72, 690 - 12.0f * i, 10);
    }
    const std::string md = Run(pages);
    bool all = true;
    for (const char* opening : openings) all = all && Find(md, opening) != std::string::npos;
    RAYOPDF_CHECK(all, 2156);
}

// A scanned page (one picture over the whole page) whose text is only lines at its edges: the
// stamps a repository or a publisher set over the scan are furniture, though the page has fewer
// lines than the sparse guard asks for; a caption over the scan stays. Without the picture the
// page keeps its title line (TestSparseMetadata).
void TestScanStamps() {
    std::vector<PageData> pages{NewPage()};
    ImageBox scan;
    scan.x0 = 0, scan.y0 = 0, scan.x1 = 612, scan.y1 = 792;
    pages[0].images.push_back(scan);
    AddText(pages[0], "New Directions, Vol. 11, Art. 4", 242, 763, 10);
    AddText(pages[0], "The Japan Society of Examples", 22, 22, 10);
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "New Directions") == std::string::npos && Find(md, "Japan Society") == std::string::npos, 1862);
    std::vector<PageData> captioned{NewPage()};
    captioned[0].images.push_back(scan);
    AddText(captioned[0], "Repository stamp of the library", 200, 770, 10);
    AddText(captioned[0], "Figure 3. The scanned plate of the survey", 150, 40, 10);
    const std::string withCaption = Run(captioned);
    RAYOPDF_CHECK(Find(withCaption, "Repository stamp") == std::string::npos &&
                      Find(withCaption, "Figure 3. The scanned plate") != std::string::npos,
                  1863);
}

// A page whose text lies all at its edges (a blank page of a manuscript collection): its head, a
// folio row spread across the text, and its foot, a lone page number, are furniture though the
// page has fewer lines than the sparse guard asks for, and though each is all the text left when
// it is examined. A page holding only a title at its head keeps it.
void TestEdgeOnlyPage() {
    std::vector<PageData> pages{NewPage()};
    AddText(pages[0], "John Bibb Tate Memoir, 1921-1983", 72, 757, 10);
    AddText(pages[0], "Manuscript Collection No. 117", 444, 757, 10);
    AddText(pages[0], "3", 303, 62, 12);
    const std::string md = Run(pages);
    std::vector<PageData> titled{NewPage()};
    AddText(titled[0], "Notes on the Survey", 72, 740, 10);
    const std::string title = Run(titled);
    RAYOPDF_CHECK(Find(md, "Memoir") == std::string::npos && Find(md, "Manuscript") == std::string::npos &&
                      Find(md, "3") == std::string::npos && Find(title, "Notes on the Survey") != std::string::npos,
                  1864);
}

// A figure page whose only other text is the figure's label: the page number at its foot is
// furniture, the label stays.
void TestSparsePageNumber() {
    std::vector<PageData> pages{NewPage()};
    AddText(pages[0], "Fig. 6", 280, 400, 12);
    AddText(pages[0], "Page 11 of 15", 463, 52, 10);
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "Page 11") == std::string::npos && Find(md, "Fig. 6") != std::string::npos, 1865);
}

// A figure's caption in small print at the foot of a page, set apart from the text and over the
// page number, is the figure's, not furniture: the page number goes, the caption stays.
void TestCaptionAtFoot() {
    std::vector<PageData> pages{NewPage()};
    for (int i = 0; i < 25; ++i)
        AddText(pages[0], "Body line " + std::to_string(i) + " of the running text goes on here.", 72, 700 - 12.0f * i, 10);
    AddText(pages[0], "Figure 3. Results of the second test.", 72, 46, 8);
    AddText(pages[0], "12", 182, 24, 10);
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "Figure 3. Results") != std::string::npos && Find(md, "\n12\n") == std::string::npos, 1884);
}

// A page whose text ends high, its page number set right under its last line (filings converted
// from HTML): the number, alone, centred on the text and set apart from it, goes. Set flush left,
// or close under the text, it stays.
void TestPageNumberUnderShortText() {
    // 20 body lines from 700 down, 12 points apart: the last at 472; the text runs from 72 to 302.
    const auto build = [](float x, float y) {
        std::vector<PageData> pages{NewPage()};
        for (int i = 0; i < 20; ++i)
            AddText(pages[0], "Body line " + std::to_string(i) + " of the running text goes on here.", 72,
                    700 - 12.0f * i, 10);
        AddText(pages[0], "83", x, y, 10);
        return Run(pages);
    };
    const std::string md = build(182, 440);
    RAYOPDF_CHECK(Find(md, "83") == std::string::npos && Find(md, "Body line 19 of") != std::string::npos, 1885);
    RAYOPDF_CHECK(Find(build(72, 440), "\n83\n") != std::string::npos, 1886);
    RAYOPDF_CHECK(Find(build(182, 458), "83") != std::string::npos, 1887);
}

// A patent's running head, set close over the description's text: removed. The bare publication
// number under a front page's barcode, without separators in its digits, stays.
void TestPatentHead() {
    const auto build = [](const char* head) {
        std::vector<PageData> pages{NewPage()};
        AddText(pages[0], head, 72, 740, 10);
        for (int i = 0; i < 25; ++i)
            AddText(pages[0], "[00" + std::to_string(10 + i) + "] The device of the invention holds a part.", 72,
                    726 - 12.0f * i, 10);
        return Run(pages);
    };
    const std::string md = build("US 2020/0159120 A1 May 21, 2020");
    RAYOPDF_CHECK(Find(md, "0159120") == std::string::npos && Find(md, "[0010] The device") != std::string::npos, 1888);
    RAYOPDF_CHECK(Find(build("US 20200081341A1"), "US 20200081341A1") != std::string::npos, 1889);
}

// A page whose text lies all at its edges keeps text that is no head or foot: the last entries of
// a reference list run on from the page before (a URL in them is no publication stamp), and a row
// of numbers printed by a program (a page number holds one or two).
void TestEdgeOnlyContent() {
    std::vector<PageData> refs{NewPage()};
    AddText(refs[0], "[29]", 72, 757, 10);
    AddText(refs[0], "\"Dynamic texture database,\" http://projects.example.org/dyntex/database.html.", 98, 757, 10);
    AddText(refs[0], "[30]", 72, 745, 10);
    AddText(refs[0], "\"Image sequence server,\" http://images.example.org/sequences/.", 98, 745, 10);
    const std::string md = Run(refs);
    std::vector<PageData> data{NewPage()};
    AddText(data[0], "(3, 6, 192, 192)", 72, 757, 10);
    const std::string shape = Run(data);
    RAYOPDF_CHECK(Find(md, "[29]") != std::string::npos && Find(md, "Image sequence server") != std::string::npos &&
                      Find(shape, "192, 192") != std::string::npos,
                  1870);
}

// A running head over a rule typed as underscores: the head is set apart from the text past its
// rule and goes; the text stays.
void TestTypedRuleUnderHead() {
    std::vector<PageData> pages{NewPage()};
    AddText(pages[0], "114 - Jet-Lube Extreme", 53, 746, 10);
    AddText(pages[0], "Revision date 06-03-2019", 425, 746, 10);
    AddText(pages[0], std::string(98, '_'), 53, 734, 10);
    for (int i = 0; i < 30; ++i)
        AddText(pages[0], "Body line " + std::to_string(i) + " of the safety data sheet, as it runs on here.", 53,
                712 - 12.0f * i, 10);
    const std::string md = Run(pages);
    RAYOPDF_CHECK(Find(md, "Revision date") == std::string::npos && Find(md, "Jet-Lube") == std::string::npos &&
                      Find(md, "Body line 0 of") != std::string::npos,
                  1868);
}

// A download stamp wrapped onto a last line in the outer margin of a two-column page, under a
// folio row: the stamp's last line, set off the text's left edge on a page that is not set flush
// left, waits with its first, and all three rows go. A paragraph in the body's size whose last
// line ends in the margin under a line with a URL stays.
void TestWrappedStamp() {
    std::vector<PageData> pages{NewPage()};
    for (int i = 0; i < 30; ++i)
        for (const float x : {41.0f, 304.0f})
            AddText(pages[0], "Line " + std::to_string(i) + " of the clinical study.", x, 700 - 9.0f * i, 7);
    AddText(pages[0], "Clin Cancer Res 2006;12(15) August 1, 2006", 41, 25, 9);
    AddText(pages[0], "4618", 285, 25, 9);
    AddText(pages[0], "www.aacrjournals.org", 451, 25, 9);
    AddText(pages[0], "Downloaded from clincancerres.aacrjournals.org on July 19, 2017. 2006 American Assoc.", 76, 11, 9);
    AddText(pages[0], "Research.", 269, 1, 9);
    const std::string md = Run(pages);
    std::vector<PageData> paragraph{NewPage()};
    for (int i = 0; i < 30; ++i)
        AddText(paragraph[0], "Body line " + std::to_string(i) + " of the manual runs on here.", 41, 700 - 9.0f * i, 7);
    AddText(paragraph[0], "http://www.cpan.org/ is the archive of Perl software, see the", 41, 24, 7);
    AddText(paragraph[0], "question earlier in this document.", 41, 15, 7);
    const std::string kept = Run(paragraph);
    RAYOPDF_CHECK(Find(md, "Research.") == std::string::npos && Find(md, "Downloaded from") == std::string::npos &&
                      Find(md, "Clin Cancer Res") == std::string::npos && Find(md, "Line 29 of the clinical") != std::string::npos &&
                      Find(kept, "earlier in this document") != std::string::npos,
                  1869);
}

// A running head of two pieces spread across loosely leaded text (1.6 ems between its lines, so
// that 1.5 times that spacing exceeds the head's gap of 2.1 ems) goes; 1.7 ems over the text, closer
// than its spacing plus a quarter em, it stays.
void TestLooseLeadingHead() {
    // 25 body lines from 700 down, 26 points apart: a 16-point gap between them; the first line's
    // top at 708. The text runs from 72 to 297.
    const auto build = [](float gap) {
        std::vector<PageData> pages{NewPage()};
        const float y = 708 + gap + 2;  // the head's box bottom `gap` points over the first line
        AddText(pages[0], "CHAPTER 5. TESTING", 72, y, 10);
        AddText(pages[0], "5.1. DESCRIPTION", 217, y, 10);
        for (int i = 0; i < 25; ++i)
            AddText(pages[0], "Body line " + std::to_string(i) + " of the running text goes on here.", 72,
                    700 - 26.0f * i, 10);
        return Run(pages);
    };
    const std::string md = build(21);
    RAYOPDF_CHECK(Find(md, "CHAPTER 5") == std::string::npos && Find(md, "DESCRIPTION") == std::string::npos &&
                      Find(md, "Body line 0 of") != std::string::npos,
                  1871);
    RAYOPDF_CHECK(Find(build(17), "CHAPTER 5") != std::string::npos, 1872);
}

// A page whose text ends high, its running foot set right under its last line (filings converted
// from HTML): the foot, the page's lowest row, pieces spread across the text in smaller print and set
// apart from it, goes. In the body's size, or reading as a caption, it stays.
void TestFootUnderShortText() {
    // 20 body lines from 700 down, 12 points apart: the last at 472, its box bottom at 470; the text
    // runs from 72 to 297. The foot at 440 has its top at 446.4.
    const auto build = [](const char* left, const char* right, float size) {
        std::vector<PageData> pages{NewPage()};
        for (int i = 0; i < 20; ++i)
            AddText(pages[0], "Body line " + std::to_string(i) + " of the running text goes on here.", 72,
                    700 - 12.0f * i, 10);
        AddText(pages[0], left, 72, 440, size);
        AddText(pages[0], right, 297 - 0.5f * size * float(std::string_view(right).size()), 440, size);
        return Run(pages);
    };
    const std::string md = build("105", "Example Corp./2019 Form 10-K", 8);
    RAYOPDF_CHECK(Find(md, "Form 10-K") == std::string::npos && Find(md, "105") == std::string::npos &&
                      Find(md, "Body line 19 of") != std::string::npos,
                  1873);
    RAYOPDF_CHECK(Find(build("105", "Example Corp./2019 Form 10-K", 10), "Form 10-K") != std::string::npos, 1874);
    RAYOPDF_CHECK(Find(build("Figure 3.", "Results of the second test.", 8), "Results of the second") != std::string::npos,
                  1875);
}

// A running foot set at twice the body size, a folio at its end, in the foot band and set apart from
// the text, goes; at 2.5 times the body size it stays.
void TestLargeFoot() {
    const auto build = [](float size) {
        std::vector<PageData> pages{NewPage()};
        for (int i = 0; i < 25; ++i)
            AddText(pages[0], "Body line " + std::to_string(i) + " of the running text goes on here.", 72,
                    700 - 12.0f * i, 10);
        AddText(pages[0], "EXAMPLE CORPORATION 2014 ANNUAL REPORT 15", 72, 40, size);
        return Run(pages);
    };
    const std::string md = build(20);
    RAYOPDF_CHECK(Find(md, "ANNUAL REPORT") == std::string::npos && Find(md, "Body line 24 of") != std::string::npos,
                  1876);
    RAYOPDF_CHECK(Find(build(25), "ANNUAL REPORT") != std::string::npos, 1877);
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
    TestAddressesAccentedCapitals();
    TestAddressesSurnameLabels();
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
    TestFootPageNumbers();
    TestNotesAfterColumns();
    TestCountedPageNumbers();
    TestDifferentFirstLines();
    TestScanStamps();
    TestEdgeOnlyPage();
    TestSparsePageNumber();
    TestCaptionAtFoot();
    TestPageNumberUnderShortText();
    TestPatentHead();
    TestEdgeOnlyContent();
    TestTypedRuleUnderHead();
    TestWrappedStamp();
    TestLooseLeadingHead();
    TestFootUnderShortText();
    TestLargeFoot();
    std::printf("test_layout_order: ok\n");
    return 0;
}
