// SPDX-License-Identifier: Apache-2.0
// Core tests of the tagged path (src/layout/structure.cpp): the structure tree read, checked and
// used for blocks and reading order, on tagged PDFs built in memory (tests/pdf_builder.h) and
// converted by the whole pipeline. Exit codes 900-949.
#include "check.h"
#include "pdf_builder.h"

#include "layout/convert.h"
#include "layout/structure.h"
#include "pdf/document.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace RayoPdf;
using namespace RayoPdf::Layout;
using RayoPdf::Test::MarkedText;
using RayoPdf::Test::TaggedBuilder;

namespace {

std::string Run(const std::string& pdf, ConvertReport& report, bool structure = true, int lastPage = 0) {
    std::unique_ptr<Document> document;
    RAYOPDF_CHECK(Document::Open(pdf, "", document) == OpenStatus::Ok, 900);
    ConvertOptions options;
    options.structure = structure;
    options.lastPage = lastPage;
    std::string md;
    Convert(*document, options, md, report);
    if (std::getenv("RAYOPDF_TEST_VERBOSE")) std::fprintf(stderr, "--- %s\n%s", StructureUseName(report.structure), md.c_str());
    return md;
}

bool Has(const std::string& md, const char* needle) { return md.find(needle) != std::string::npos; }

// A heading in body size and two paragraphs: only the tree says it is a heading.
std::string HeadingDocument(const std::string& headingType) {
    TaggedBuilder t;
    const int doc = t.Element(0, "Document");
    const int h = t.Element(doc, headingType);
    t.Mcid(h, 0, 0);
    const int p1 = t.Element(doc, "P");
    t.Mcid(p1, 0, 1);
    const int p2 = t.Element(doc, "P");
    t.Mcid(p2, 0, 2);
    return t.Build({MarkedText("H1", 0, 72, 700, 12, "Results and methods") +
                    MarkedText("P", 1, 72, 670, 12, "The first paragraph says something plain.") +
                    MarkedText("P", 2, 72, 640, 12, "The second paragraph says something else.")});
}

void Headings() {
    ConvertReport report;
    std::string md = Run(HeadingDocument("H1"), report);
    RAYOPDF_CHECK(report.structure == StructureUse::Used, 901);
    RAYOPDF_CHECK(Has(md, "# Results and methods\n"), 902);
    RAYOPDF_CHECK(Has(md, "\n\nThe first paragraph says something plain.\n\nThe second"), 903);
    md = Run(HeadingDocument("H2"), report);
    RAYOPDF_CHECK(Has(md, "## Results and methods\n"), 904);
    // Without the tree the same file has no heading.
    md = Run(HeadingDocument("H1"), report, false);
    RAYOPDF_CHECK(report.structure == StructureUse::None && !Has(md, "# Results"), 905);
}

void Lists() {
    TaggedBuilder t;
    const int doc = t.Element(0, "Document");
    const int h = t.Element(doc, "H1");
    t.Mcid(h, 0, 0);
    const int l = t.Element(doc, "L");
    std::string c = MarkedText("H1", 0, 72, 720, 18, "Lists");
    int mcid = 1;
    double y = 690;
    const auto item = [&](int list, const char* label, const char* text, double x) {
        const int li = t.Element(list, "LI");
        const int lbl = t.Element(li, "Lbl");
        t.Mcid(lbl, 0, mcid);
        c += MarkedText("Lbl", mcid++, x, y, 12, label);
        const int body = t.Element(li, "LBody");
        t.Mcid(body, 0, mcid);
        c += MarkedText("LBody", mcid++, x + 18, y, 12, text);
        y -= 20;
        return li;
    };
    item(l, "1.", "Prepare the parts", 72);
    const int second = item(l, "2.", "Assemble the frame", 72);
    const int nested = t.Element(second, "L");
    item(nested, "-", "Tighten every screw", 96);
    item(l, "3.", "Test the result", 72);
    ConvertReport report;
    const std::string md = Run(t.Build({c}), report);
    RAYOPDF_CHECK(report.structure == StructureUse::Used, 906);
    RAYOPDF_CHECK(Has(md, "1. Prepare the parts\n2. Assemble the frame\n   - Tighten every screw\n3. Test the result"), 907);
}

// A table with no rules at all, a header row of TH and a cell spanning two columns.
std::string TableDocument(bool span) {
    TaggedBuilder t;
    const int doc = t.Element(0, "Document");
    const int h = t.Element(doc, "H1");
    t.Mcid(h, 0, 0);
    std::string c = MarkedText("H1", 0, 72, 720, 18, "Figures");
    const int table = t.Element(doc, "Table");
    int mcid = 1;
    const char* const cells[3][2] = {{"Name", "Value"}, {"Alpha", "12"}, {"Beta", "34"}};
    for (int r = 0; r < 3; ++r) {
        const int tr = t.Element(table, "TR");
        const double y = 690 - 20 * r;
        if (span && r == 2) {
            const int td = t.Element(tr, "TD", "/A << /O /Table /ColSpan 2 >>");
            t.Mcid(td, 0, mcid);
            c += MarkedText("TD", mcid++, 72, y, 12, "Both columns");
            continue;
        }
        for (int k = 0; k < 2; ++k) {
            const int cell = t.Element(tr, r == 0 ? "TH" : "TD");
            t.Mcid(cell, 0, mcid);
            c += MarkedText(r == 0 ? "TH" : "TD", mcid++, 72 + 150 * k, y, 12, cells[r][k]);
        }
    }
    return t.Build({c});
}

void Tables() {
    ConvertReport report;
    std::string md = Run(TableDocument(false), report);
    RAYOPDF_CHECK(report.structure == StructureUse::Used && report.tables == 1, 908);
    RAYOPDF_CHECK(Has(md, "| Name | Value |\n| --- | --- |\n| Alpha | 12 |\n| Beta | 34 |"), 909);
    md = Run(TableDocument(true), report);
    RAYOPDF_CHECK(Has(md, "| Name | Value |\n| --- | --- |\n| Alpha | 12 |\n| Both columns | Both columns |"), 910);
}

// Two columns side by side: the tree reads the right one first (rule 1 holds: they do not overlap
// horizontally), the layout would read the left one first.
void Order() {
    TaggedBuilder t;
    const int doc = t.Element(0, "Document");
    const int h = t.Element(doc, "H1");
    t.Mcid(h, 0, 0);
    const int right = t.Element(doc, "P");
    t.Mcid(right, 0, 2);
    const int left = t.Element(doc, "P");
    t.Mcid(left, 0, 1);
    const std::string c = MarkedText("H1", 0, 72, 720, 18, "Columns") +
                          MarkedText("P", 1, 72, 690, 12, "Left column text") +
                          MarkedText("P", 2, 340, 690, 12, "Right column text");
    ConvertReport report;
    const std::string md = Run(t.Build({c}), report);
    RAYOPDF_CHECK(report.structure == StructureUse::Used, 911);
    const size_t r = md.find("Right column"), l = md.find("Left column");
    RAYOPDF_CHECK(r != std::string::npos && l != std::string::npos && r < l, 912);
}

// Artifacts (running heads) and untagged text are left out.
void Artifacts() {
    TaggedBuilder t;
    const int doc = t.Element(0, "Document");
    const int h = t.Element(doc, "H1");
    t.Mcid(h, 0, 0);
    std::string c = MarkedText("Artifact", -1, 72, 760, 9, "Running head of the report");
    c += MarkedText("H1", 0, 72, 720, 18, "Introduction");
    for (int i = 1; i <= 20; ++i) {
        const int p = t.Element(doc, "P");
        t.Mcid(p, 0, i);
        c += MarkedText("P", i, 72, 700 - 20 * i, 12, "Body text line number " + std::to_string(i) + " of the page.");
    }
    c += "BT /F1 9 Tf 300 40 Td (7) Tj ET\n";  // an untagged folio
    ConvertReport report;
    const std::string md = Run(t.Build({c}), report);
    RAYOPDF_CHECK(report.structure == StructureUse::Used, 913);
    RAYOPDF_CHECK(!Has(md, "Running head") && !Has(md, "\n7\n") && Has(md, "line number 20 of the page."), 914);
}

// The checks: too little text in the tree, only paragraphs, an order that contradicts the page.
void Checks() {
    ConvertReport report;
    {
        TaggedBuilder t;
        const int doc = t.Element(0, "Document");
        const int h = t.Element(doc, "H1");
        t.Mcid(h, 0, 0);
        std::string c = MarkedText("H1", 0, 72, 720, 18, "Tagged");
        c += "BT /F1 12 Tf 72 690 Td (A long untagged paragraph that the tree does not know about at all) Tj ET\n";
        const std::string pdf = t.Build({c});
        const std::string on = Run(pdf, report);
        RAYOPDF_CHECK(report.structure == StructureUse::Coverage, 915);
        const std::string off = Run(pdf, report, false);
        RAYOPDF_CHECK(on == off, 916);
    }
    {
        TaggedBuilder t;
        const int doc = t.Element(0, "Document");
        const int p1 = t.Element(doc, "P");
        t.Mcid(p1, 0, 0);
        const int p2 = t.Element(doc, "Span");
        t.Mcid(p2, 0, 1);
        Run(t.Build({MarkedText("P", 0, 72, 700, 12, "Only paragraphs here.") +
                     MarkedText("Span", 1, 72, 680, 12, "And spans.")}),
            report);
        RAYOPDF_CHECK(report.structure == StructureUse::Generic, 917);
    }
    {
        // Ten lines read bottom to top.
        TaggedBuilder t;
        const int doc = t.Element(0, "Document");
        const int h = t.Element(doc, "H1");
        t.Mcid(h, 0, 10);
        std::string c;
        for (int i = 0; i < 10; ++i) {
            const int p = t.Element(doc, "P");
            t.Mcid(p, 0, i);
            c += MarkedText("P", i, 72, 300 + 30 * i, 12, "Line " + std::to_string(i));
        }
        c += MarkedText("H1", 10, 72, 700, 18, "Bottom up");
        Run(t.Build({c}), report);
        RAYOPDF_CHECK(report.structure == StructureUse::Order, 918);
    }
}

// The RoleMap: custom types map to standard ones; a cycle rejects the tree.
void RoleMap() {
    ConvertReport report;
    {
        TaggedBuilder t;
        const int doc = t.Element(0, "Document");
        const int h = t.Element(doc, "Chapter#20Title");
        t.Mcid(h, 0, 0);
        const int p = t.Element(doc, "Body");
        t.Mcid(p, 0, 1);
        const std::string md = Run(t.Build({MarkedText("Span", 0, 72, 700, 12, "Mapped heading") +
                                            MarkedText("Span", 1, 72, 670, 12, "Mapped body text.")},
                                           "/RoleMap << /Chapter#20Title /Heading1 /Heading1 /H1 /Body /P >>"),
                                   report);
        RAYOPDF_CHECK(report.structure == StructureUse::Used && Has(md, "# Mapped heading\n"), 919);
    }
    {
        TaggedBuilder t;
        const int doc = t.Element(0, "Document");
        const int h = t.Element(doc, "H1");
        t.Mcid(h, 0, 0);
        Run(t.Build({MarkedText("H1", 0, 72, 700, 12, "Cycle")}, "/RoleMap << /A /B /B /A >>"), report);
        RAYOPDF_CHECK(report.structure == StructureUse::RoleCycle, 920);
    }
}

// Hostile trees: an element that is its own descendant, nesting past the depth limit.
void Hostile() {
    ConvertReport report;
    {
        TaggedBuilder t;
        const int doc = t.Element(0, "Document");
        const int h = t.Element(doc, "H1");
        t.Mcid(h, 0, 0);
        const int p = t.Element(doc, "P");
        t.Mcid(p, 0, 1);
        t.Kid(p, std::to_string(t.ObjectOf(doc)) + " 0 R");  // back to the Document
        t.Kid(h, std::to_string(t.ObjectOf(h)) + " 0 R");    // itself
        const std::string md = Run(t.Build({MarkedText("H1", 0, 72, 700, 18, "Loops") +
                                            MarkedText("P", 1, 72, 670, 12, "Read once.")}),
                                   report);
        RAYOPDF_CHECK(report.structure == StructureUse::Used && Has(md, "# Loops\n\nRead once."), 921);
    }
    {
        TaggedBuilder t;
        int parent = t.Element(0, "Document");
        for (uint32_t i = 0; i < kMaxStructureDepth + 8; ++i) parent = t.Element(parent, "Div");
        const int h = t.Element(parent, "H1");
        t.Mcid(h, 0, 0);
        const std::string pdf = t.Build({MarkedText("H1", 0, 72, 700, 12, "Deep")});
        std::unique_ptr<Document> document;
        RAYOPDF_CHECK(Document::Open(pdf, "", document) == OpenStatus::Ok, 922);
        Structure s;
        ReadStructure(*document, 0, 0, s);
        RAYOPDF_CHECK(s.use == StructureUse::Broken, 923);
    }
}

// Pages from the ParentTree when elements have no /Pg; MCIDs inside a Form XObject (MCR /Stm).
void ParentTreeAndForms() {
    ConvertReport report;
    {
        TaggedBuilder t;
        const int doc = t.Element(0, "Document");
        const int h = t.Element(doc, "H2");
        t.Mcid(h, 0, 0, false);
        const int p = t.Element(doc, "P");
        t.Mcid(p, 0, 1, false);
        const std::string md = Run(t.Build({MarkedText("H2", 0, 72, 700, 12, "No page entry") +
                                            MarkedText("P", 1, 72, 670, 12, "Found through the ParentTree.")}),
                                   report);
        RAYOPDF_CHECK(report.structure == StructureUse::Used && Has(md, "## No page entry\n"), 924);
    }
    {
        TaggedBuilder t;
        const int doc = t.Element(0, "Document");
        const int h = t.Element(doc, "H1");
        const int p = t.Element(doc, "P");
        t.Mcid(p, 0, 0);
        const int form = t.Pdf().AddStream("/Type /XObject /Subtype /Form /BBox [0 0 612 792] /Resources << /Font << "
                                           "/F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >> >>",
                                           MarkedText("H1", 0, 72, 700, 12, "Heading inside a form"));
        t.Kid(h, "<< /Type /MCR /MCID 0 /Pg " + std::to_string(t.PageObject(0)) + " 0 R /Stm " + std::to_string(form) +
                     " 0 R >>");
        const std::string c = "/Fm1 Do\n" + MarkedText("P", 0, 72, 670, 12, "Page text after the form.");
        const std::string md =
            Run(t.Build({c}, "", "/XObject << /Fm1 " + std::to_string(form) + " 0 R >>"), report);
        RAYOPDF_CHECK(report.structure == StructureUse::Used && Has(md, "# Heading inside a form\n"), 925);
    }
}

// Code, quotes, emphasis and strike-through from inline elements.
void Inline() {
    TaggedBuilder t;
    const int doc = t.Element(0, "Document");
    const int h = t.Element(doc, "H1");
    t.Mcid(h, 0, 0);
    const int p = t.Element(doc, "P");
    t.Mcid(p, 0, 1);
    const int strong = t.Element(p, "Strong");
    t.Mcid(strong, 0, 2);
    t.Mcid(p, 0, 3);
    const int struck = t.Element(p, "Span", "/A << /O /Layout /TextDecorationType /LineThrough >>");
    t.Mcid(struck, 0, 4);
    const int quote = t.Element(doc, "BlockQuote");
    const int qp = t.Element(quote, "P");
    t.Mcid(qp, 0, 5);
    const int code = t.Element(doc, "Code", "/A << /O /Layout /Placement /Block >>");
    t.Mcid(code, 0, 6);
    std::string c = MarkedText("H1", 0, 72, 720, 18, "Inline");
    c += MarkedText("Span", 1, 72, 690, 12, "Plain") + MarkedText("Strong", 2, 110, 690, 12, "strong") +
         MarkedText("Span", 3, 160, 690, 12, "and") + MarkedText("Span", 4, 190, 690, 12, "gone");
    c += MarkedText("P", 5, 90, 660, 12, "A quoted sentence.");
    c += MarkedText("Code", 6, 72, 630, 12, "x = 1");
    ConvertReport report;
    const std::string md = Run(t.Build({c}), report);
    RAYOPDF_CHECK(report.structure == StructureUse::Used, 926);
    RAYOPDF_CHECK(Has(md, "**strong**"), 927);
    RAYOPDF_CHECK(Has(md, "~~gone~~"), 928);
    RAYOPDF_CHECK(Has(md, "> A quoted sentence."), 929);
    RAYOPDF_CHECK(Has(md, "```\nx = 1\n```"), 930);
}

// Untagged files, and pages outside the converted range.
void UntaggedAndRange() {
    ConvertReport report;
    Run(Test::OnePage("BT /F1 12 Tf 72 700 Td (Untagged) Tj ET"), report);
    RAYOPDF_CHECK(report.structure == StructureUse::None && report.structureElements == 0, 931);
    TaggedBuilder t(2);
    const int doc = t.Element(0, "Document");
    const int h = t.Element(doc, "H1");
    t.Mcid(h, 0, 0);
    const int p = t.Element(doc, "P");
    t.Mcid(p, 1, 0);
    const std::string md = Run(t.Build({MarkedText("H1", 0, 72, 700, 12, "First page"),
                                        MarkedText("P", 0, 72, 700, 12, "Second page.")}),
                               report, true, 1);
    RAYOPDF_CHECK(report.structure == StructureUse::Used && Has(md, "# First page") && !Has(md, "Second page"), 932);
    // /ActualText with control characters: they are no text; a tab is a space.
    const std::string controls = Run(Test::OnePage("/Span << /ActualText <FEFF0008> >> BDC BT /F1 12 Tf 72 700 Td (x) Tj "
                                                   "ET EMC /Span << /ActualText <FEFF00410009004200070043> >> BDC BT /F1 "
                                                   "12 Tf 72 680 Td (y) Tj ET EMC"),
                                     report);
    RAYOPDF_CHECK(controls.find('\x08') == std::string::npos && controls.find('\x07') == std::string::npos &&
                      Has(controls, "A BC"),
                  933);
}

// Images written to files: a Figure set inline in a sentence (PlaceImages adds a glyph for it to
// its line after the tree's glyph tables were built: SafeDocs 0000680 read past them) and a block
// Figure, both with /Alt.
void Figures() {
    TaggedBuilder t;
    const int doc = t.Element(0, "Document");
    const int h = t.Element(doc, "H1");
    t.Mcid(h, 0, 0);
    const int p = t.Element(doc, "P");
    t.Mcid(p, 0, 1);
    const int icon = t.Element(p, "Figure", "/Alt (a dot)");
    t.Mcid(icon, 0, 2);
    t.Mcid(p, 0, 3);
    const int chart = t.Element(doc, "Figure", "/Alt (a chart)");
    t.Mcid(chart, 0, 4);
    const auto image = [&](int mcid, double x, double y, double side) {
        char head[160];
        std::snprintf(head, sizeof head, "/Figure << /MCID %d >> BDC q %g 0 0 %g %g %g cm BI /W 32 /H 32 /CS /G /BPC 8 ID\n",
                      mcid, side, side, x, y);
        return std::string(head) + std::string(32 * 32, static_cast<char>(0x80 + mcid)) + "\nEI Q EMC\n";
    };
    // The icon starts its line: the first glyph read of the line is the one added later.
    std::string c = MarkedText("H1", 0, 72, 720, 18, "Pictures");
    c += image(2, 72, 690, 10);
    c += MarkedText("P", 1, 86, 690, 12, "starts the line.");
    c += MarkedText("P", 3, 72, 672, 12, "A second line of the paragraph.");
    c += image(4, 72, 400, 250);
    const std::string pdf = t.Build({c});
    std::unique_ptr<Document> document;
    RAYOPDF_CHECK(Document::Open(pdf, "", document) == OpenStatus::Ok, 934);
    ConvertOptions options;
    options.imageDirectory = "/tmp/rayopdf-test-tagged-images";
    options.imageLinkPrefix = "img/";
    std::string md;
    ConvertReport report;
    Convert(*document, options, md, report);
    if (std::getenv("RAYOPDF_TEST_VERBOSE")) std::fprintf(stderr, "--- figures\n%s", md.c_str());
    RAYOPDF_CHECK(report.structure == StructureUse::Used && report.images == 2, 935);
    RAYOPDF_CHECK(Has(md, "![a dot](img/p1-1.png) starts the line.") && Has(md, "A second line of the paragraph."), 936);
    RAYOPDF_CHECK(Has(md, "![a chart](img/"), 937);
}

} // namespace

int main() {
    Headings();
    Lists();
    Tables();
    Order();
    Artifacts();
    Checks();
    RoleMap();
    Hostile();
    ParentTreeAndForms();
    Inline();
    UntaggedAndRange();
    Figures();
    std::puts("test_layout_tagged: ok");
    return 0;
}
