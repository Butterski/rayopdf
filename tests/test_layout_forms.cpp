// SPDX-License-Identifier: Apache-2.0
// Core tests of form fields and annotations drawn as page text (src/layout/interpreter.cpp
// ReadAnnotations, --annotations): appearances placed on their widgets, synthesized values, check
// boxes as task items, comb fields, hidden annotations, FreeText (never a heading), tagged forms with and without
// OBJR items, OCR routing and duplicate text, the caps, and --annotations=links reading as 1.1 did.
// PDFs are built in memory (tests/pdf_builder.h). Exit codes 2600-2649.
#include "check.h"
#include "pdf_builder.h"

#include "layout/convert.h"
#include "layout/interpreter.h"
#include "layout/ocr.h"
#include "pdf/document.h"
#include "text/font.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace RayoPdf;
using namespace RayoPdf::Layout;
using RayoPdf::Test::MarkedText;
using RayoPdf::Test::PdfBuilder;
using RayoPdf::Test::TaggedBuilder;

namespace {

const char kHelv[] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>";

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

// The document reads `pdf` in place: the caller keeps it alive (a temporary would dangle).
std::unique_ptr<Document> Open(const std::string& pdf) {
    std::unique_ptr<Document> document;
    RAYOPDF_CHECK(Document::Open(pdf, "", document) == OpenStatus::Ok, 2600);
    return document;
}
std::unique_ptr<Document> Open(std::string&&) = delete;

std::string Convert(const std::string& pdf, AnnotationMode mode = AnnotationMode::All, ConvertReport* out = nullptr,
                    bool structure = true) {
    std::unique_ptr<Document> document = Open(pdf);
    ConvertOptions options;
    options.annotations = mode;
    options.structure = structure;
    options.ocr.mode = OcrMode::Off;
    std::string md;
    ConvertReport report;
    Layout::Convert(*document, options, md, report);
    if (out) *out = report;
    if (std::getenv("RAYOPDF_TEST_VERBOSE")) std::fprintf(stderr, "---\n%s", md.c_str());
    return md;
}

// A one-page document (US Letter) showing `content` (Helvetica as /F1), its annotations made by
// the caller as objects of `b` (Annot returns their numbers), with `acroForm` as the catalog's
// /AcroForm (none when empty).
struct FormDoc {
    PdfBuilder b;
    std::vector<int> annots;

    int Stream(const std::string& dict, const std::string& data) { return b.AddStream(dict, data); }
    // An appearance stream (a Form XObject) of box `bbox` showing `content`, with `resources`.
    int Appearance(const std::string& bbox, const std::string& content,
                   const std::string& resources = std::string("<< /Font << /Helv ") + kHelv + " >> >>") {
        return b.AddStream("/Type /XObject /Subtype /Form /BBox [" + bbox + "] /Resources " + resources, content);
    }
    int Annot(const std::string& body, bool listed = true) {
        const int n = b.Add(body);
        if (listed) annots.push_back(n);
        return n;
    }
    std::string Build(const std::string& content, const std::string& acroForm = "") {
        const int catalog = b.Reserve();
        const int pages = b.Reserve();
        const int stream = b.AddFlateStream("", content);
        std::string list;
        for (int n : annots) list += std::to_string(n) + " 0 R ";
        const int page = b.Add("<< /Type /Page /Parent " + std::to_string(pages) +
                               " 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 << /Type /Font /Subtype "
                               "/Type1 /BaseFont /Helvetica >> >> >> /Contents " +
                               std::to_string(stream) + " 0 R /Annots [" + list + "] >>");
        b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R" +
                           (acroForm.empty() ? std::string() : " /AcroForm " + acroForm) + " >>");
        b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] /Count 1 >>");
        return b.Build(catalog);
    }
};

// A text field with value `value` whose appearance shows it at (2, 4) in its 180 x 17 box.
int TextWidget(FormDoc& f, const std::string& rect, const std::string& name, const std::string& value,
               const std::string& extra = "") {
    const int ap = f.Appearance("0 0 180 17", "0 0 180 17 re S /Tx BMC BT /Helv 10 Tf 2 4 Td (" + value + ") Tj ET EMC");
    return f.Annot("<< /Type /Annot /Subtype /Widget /FT /Tx /T (" + name + ") /V (" + value + ") /Rect [" + rect +
                   "] /AP << /N " + std::to_string(ap) + " 0 R >> " + extra + " >>");
}

const char kLabel[] = "BT /F1 12 Tf 72 700 Td (Name:) Tj ET\n";

void FilledTextField() {
    FormDoc f;
    TextWidget(f, "120 695 300 712", "name", "Jane Doe");
    const std::string pdf = f.Build(kLabel);
    ConvertReport report;
    const std::string md = Convert(pdf, AnnotationMode::All, &report);
    RAYOPDF_CHECK(Has(md, "Name:") && Has(md, "Jane Doe") && md.find("Name:") < md.find("Jane Doe"), 2601);
    RAYOPDF_CHECK(!Has(Convert(pdf, AnnotationMode::Links), "Jane Doe") && !Has(Convert(pdf, AnnotationMode::None), "Jane"),
                  2602);
    // The appearance's border is no ruling or shape of the page; its glyphs are the annotation's.
    std::unique_ptr<Document> document = Open(pdf);
    Text::FontCache fonts(*document);
    PageData page;
    PageStats stats;
    InterpretPage(*document, fonts, 0, AnnotationMode::All, page, stats);
    RAYOPDF_CHECK(page.shapes.empty() && page.rulings.empty() && page.annotationTexts.size() == 1 &&
                      page.annotationFirst == 5 && page.annotationEnd == page.glyphs.size() && page.glyphs.size() == 13,
                  2603);
    // Counted apart from the content (the page's text quality), and in the report.
    RAYOPDF_CHECK(stats.glyphs == 5 && stats.annotationGlyphs == 8 && report.glyphs == 13, 2604);
    // Placed on /Rect: the value starts at the box's x + 2.
    const Glyph& j = page.glyphs[page.annotationFirst];
    RAYOPDF_CHECK(j.u0 > 121.5f && j.u0 < 122.5f && j.v > 698.5f && j.v < 699.5f, 2605);
}

void Synthesized() {
    // NeedAppearances: values drawn from /V in the font of /DA, from /DR (a font missing there:
    // Helvetica); a stale appearance gives way to the value.
    FormDoc f;
    f.Annot(std::string("<< /Type /Annot /Subtype /Widget /FT /Tx /T (a) /V (Hello world) /DA (/Helv 0 Tf 0 g) ") +
            "/Rect [120 695 300 712] >>");
    f.Annot("<< /Type /Annot /Subtype /Widget /FT /Tx /T (b) /V (Plain value) /DA (/Nope 10 Tf 0 g) "
            "/Rect [120 655 300 672] >>");
    const int stale = f.Appearance("0 0 180 17", "BT /Helv 10 Tf 2 4 Td (Stale text) Tj ET");
    f.Annot("<< /Type /Annot /Subtype /Widget /FT /Tx /T (c) /V (Fresh text) /DA (/Helv 9 Tf 0 g) "
            "/Rect [120 615 300 632] /AP << /N " + std::to_string(stale) + " 0 R >> >>");
    // A value no WinAnsi font can show is left to the JSON fields (Cyrillic, UTF-16BE).
    f.Annot("<< /Type /Annot /Subtype /Widget /FT /Tx /T (d) /V <FEFF0416043E> /DA (/Helv 9 Tf 0 g) "
            "/Rect [120 575 300 592] >>");
    // A choice field: the display text of its selected option.
    f.Annot("<< /Type /Annot /Subtype /Widget /FT /Ch /Ff 131072 /T (e) /V (fr) /Opt [[(de) (Germany)] [(fr) "
            "(France)]] /DA (/Helv 9 Tf 0 g) /Rect [120 535 300 552] >>");
    // A password is never written.
    f.Annot("<< /Type /Annot /Subtype /Widget /FT /Tx /Ff 8192 /T (p) /V (hunter2) /DA (/Helv 9 Tf 0 g) "
            "/Rect [120 495 300 512] >>");
    const std::string md = Convert(f.Build(kLabel, std::string("<< /Fields [] /NeedAppearances true /DR << /Font << /Helv ") +
                                                      kHelv + " >> >> >>"));
    RAYOPDF_CHECK(Has(md, "Hello world") && Has(md, "Plain value"), 2606);
    RAYOPDF_CHECK(Has(md, "Fresh text") && !Has(md, "Stale"), 2607);
    RAYOPDF_CHECK(!Has(md, "\xD0\x96") && Has(md, "France") && !Has(md, "hunter2"), 2608);

    // Without NeedAppearances a field with an appearance shows it; one without shows its value.
    FormDoc g;
    const int shown = g.Appearance("0 0 180 17", "BT /Helv 10 Tf 2 4 Td (Shown text) Tj ET");
    g.Annot("<< /Type /Annot /Subtype /Widget /FT /Tx /T (c) /V (Other) /Rect [120 615 300 632] /AP << /N " +
            std::to_string(shown) + " 0 R >> >>");
    g.Annot("<< /Type /Annot /Subtype /Widget /FT /Tx /T (n) /V (No appearance) /Rect [120 575 300 592] >>");
    const std::string plain = Convert(g.Build(kLabel, "<< /Fields [] /DA (/Helv 0 Tf 0 g) >>"));
    RAYOPDF_CHECK(Has(plain, "Shown text") && !Has(plain, "Other") && Has(plain, "No appearance"), 2609);
}

void StateDictionary() {
    FormDoc f;
    const int first = f.Appearance("0 0 200 20", "BT /Helv 10 Tf 2 5 Td (First state) Tj ET");
    const int second = f.Appearance("0 0 200 20", "BT /Helv 10 Tf 2 5 Td (Second state) Tj ET");
    f.Annot("<< /Type /Annot /Subtype /FreeText /Rect [72 600 272 620] /AS /B /AP << /N << /A " + std::to_string(first) +
            " 0 R /B " + std::to_string(second) + " 0 R >> >> >>");
    // A state dictionary without /AS shows nothing.
    f.Annot("<< /Type /Annot /Subtype /FreeText /Rect [72 560 272 580] /AP << /N << /A " + std::to_string(first) +
            " 0 R >> >> >>");
    const std::string md = Convert(f.Build(kLabel));
    RAYOPDF_CHECK(Has(md, "Second state") && !Has(md, "First state"), 2610);
}

void CheckBoxes() {
    FormDoc f;
    const std::string zapf = "<< /Font << /ZaDb << /Type /Font /Subtype /Type1 /BaseFont /ZapfDingbats >> >> >>";
    const int on = f.Appearance("0 0 10 10", "BT /ZaDb 8 Tf 1 2 Td (4) Tj ET", zapf);
    const int off = f.Appearance("0 0 10 10", "", zapf);
    const std::string states = " /AP << /N << /Yes " + std::to_string(on) + " 0 R /Off " + std::to_string(off) + " 0 R >> >>";
    f.Annot("<< /Type /Annot /Subtype /Widget /FT /Btn /T (agree) /V /Yes /AS /Yes /Rect [72 698 82 708]" + states + " >>");
    f.Annot("<< /Type /Annot /Subtype /Widget /FT /Btn /T (spam) /V /Off /AS /Off /Rect [72 678 82 688]" + states + " >>");
    // A radio group: the parent field holds the type, flags and value; its widgets the states.
    const int group = f.Annot("", false);
    const std::string parent = " /Parent " + std::to_string(group) + " 0 R";
    const int a = f.Annot("<< /Type /Annot /Subtype /Widget /AS /A /Rect [72 638 82 648]" + parent + states + " >>");
    const int b = f.Annot("<< /Type /Annot /Subtype /Widget /AS /Off /Rect [72 618 82 628]" + parent + states + " >>");
    f.b.Set(group, "<< /FT /Btn /Ff 49152 /T (choice) /V /A /Kids [" + std::to_string(a) + " 0 R " + std::to_string(b) +
                       " 0 R] >>");
    // A pushbutton shows no field text.
    const int submit = f.Appearance("0 0 60 20", "BT /Helv 10 Tf 5 6 Td (Submit) Tj ET");
    f.Annot("<< /Type /Annot /Subtype /Widget /FT /Btn /Ff 65536 /T (go) /Rect [72 560 132 580] /AP << /N " +
            std::to_string(submit) + " 0 R >> >>");
    const std::string labels = "BT /F1 10 Tf 86 700 Td (I agree) Tj ET BT /F1 10 Tf 86 680 Td (Send offers) Tj ET "
                               "BT /F1 10 Tf 86 640 Td (Option A) Tj ET BT /F1 10 Tf 86 620 Td (Option B) Tj ET";
    const std::string md = Convert(f.Build(labels));
    RAYOPDF_CHECK(Has(md, "- [x] I agree") && Has(md, "- [ ] Send offers"), 2611);
    RAYOPDF_CHECK(Has(md, "- [x] Option A") && Has(md, "- [ ] Option B"), 2612);
    RAYOPDF_CHECK(!Has(md, "Submit") && !Has(md, "\xE2\x9C\x94"), 2613);
}

void CombAndHidden() {
    FormDoc f;
    std::string cells = "BT /Helv 10 Tf";
    for (int i = 0; i < 5; ++i) cells += " " + std::to_string(i == 0 ? 6 : 20) + " " + (i == 0 ? "4" : "0") + " Td (" +
                                         std::to_string(i + 1) + ") Tj";
    const int comb = f.Appearance("0 0 100 17", cells + " ET");
    f.Annot("<< /Type /Annot /Subtype /Widget /FT /Tx /Ff 16777216 /MaxLen 5 /T (zip) /V (12345) /Rect [120 695 220 712] "
            "/AP << /N " + std::to_string(comb) + " 0 R >> >>");
    // Hidden (flag 2) and NoView (flag 32) annotations are not shown.
    const int secret = f.Appearance("0 0 180 17", "BT /Helv 10 Tf 2 4 Td (Secret one) Tj ET");
    f.Annot("<< /Type /Annot /Subtype /Widget /FT /Tx /T (h) /F 2 /Rect [120 655 300 672] /AP << /N " +
            std::to_string(secret) + " 0 R >> >>");
    f.Annot("<< /Type /Annot /Subtype /FreeText /F 32 /Rect [120 615 300 632] /AP << /N " + std::to_string(secret) +
            " 0 R >> >>");
    // A FreeText annotation's appearance is page text.
    const int note = f.Appearance("0 0 200 20", "BT /Helv 10 Tf 2 5 Td (A reviewer note) Tj ET");
    f.Annot("<< /Type /Annot /Subtype /FreeText /Rect [72 560 272 580] /AP << /N " + std::to_string(note) + " 0 R >> >>");
    const std::string md = Convert(f.Build("BT /F1 12 Tf 72 700 Td (Postcode:) Tj ET"));
    RAYOPDF_CHECK(Has(md, "12345"), 2614);
    RAYOPDF_CHECK(!Has(md, "Secret"), 2615);
    RAYOPDF_CHECK(Has(md, "A reviewer note"), 2616);
}

void NoteNoHeading() {
    // A FreeText note set large over body text reads as a paragraph, never as a heading.
    FormDoc f;
    const int note = f.Appearance("0 0 300 30", "BT /Helv 20 Tf 2 8 Td (Check the totals again) Tj ET");
    f.Annot("<< /Type /Annot /Subtype /FreeText /Rect [72 720 372 750] /AP << /N " + std::to_string(note) + " 0 R >> >>");
    std::string body = "BT /F1 10 Tf 12 TL 72 680 Td";
    for (int i = 0; i < 8; ++i)
        body += " (This is line " + std::to_string(i + 1) + " of the body text of the page, in its own size.) '";
    const std::string pdf = f.Build(body + " ET");
    const std::string md = Convert(pdf);
    RAYOPDF_CHECK(Has(md, "Check the totals again") && !Has(md, "# Check the totals"), 2641);
}

void NoteOverScan() {
    // A FreeText note on a page whose text is an invisible layer (OCR over a scan) leaves that
    // layer the page's text.
    FormDoc f;
    const int note = f.Appearance("0 0 300 20", "BT /Helv 12 Tf 2 5 Td (https://doi.org/10.1/x) Tj ET");
    f.Annot("<< /Type /Annot /Subtype /FreeText /Rect [72 600 372 620] /AP << /N " + std::to_string(note) + " 0 R >> >>");
    const std::string pdf = f.Build("BT 3 Tr /F1 12 Tf 72 700 Td (The scanned page text) Tj ET");
    const std::string md = Convert(pdf);
    RAYOPDF_CHECK(Has(md, "The scanned page text") && Has(md, "https://doi.org/10.1/x"), 2642);
}

void FontFallback() {
    // An appearance naming a font its resources lack reads it from /AcroForm /DR; one without
    // resources uses /DR.
    FormDoc f;
    const int other = f.Appearance("0 0 180 17", "BT /Helv 10 Tf 2 4 Td (From the form fonts) Tj ET",
                                   std::string("<< /Font << /Other ") + kHelv + " >> >>");
    f.Annot("<< /Type /Annot /Subtype /Widget /FT /Tx /T (a) /Rect [120 695 300 712] /AP << /N " + std::to_string(other) +
            " 0 R >> >>");
    const int bare = f.b.AddStream("/Type /XObject /Subtype /Form /BBox [0 0 180 17]",
                                   "BT /Helv 10 Tf 2 4 Td (No resources) Tj ET");
    f.Annot("<< /Type /Annot /Subtype /Widget /FT /Tx /T (b) /Rect [120 655 300 672] /AP << /N " + std::to_string(bare) +
            " 0 R >> >>");
    const std::string pdf = f.Build(kLabel, std::string("<< /Fields [] /DR << /Font << /Helv ") + kHelv + " >> >> >>");
    const std::string md = Convert(pdf);
    RAYOPDF_CHECK(Has(md, "From the form fonts") && Has(md, "No resources"), 2617);
}

void Tagged(bool objr) {
    TaggedBuilder t;
    const int doc = t.Element(0, "Document");
    // A heading: a tree of paragraphs alone says no more than layout (check 2).
    const int h = t.Element(doc, "H1");
    t.Mcid(h, 0, 2);
    const int p = t.Element(doc, "P");
    t.Mcid(p, 0, 0);
    const int form = t.Element(doc, "Form");
    const int q = t.Element(doc, "P");
    t.Mcid(q, 0, 1);
    const int ap = t.Pdf().AddStream(std::string("/Type /XObject /Subtype /Form /BBox [0 0 180 17] /Resources << /Font "
                                                 "<< /Helv ") + kHelv + " >> >>",
                                     "/Tx BMC BT /Helv 10 Tf 2 4 Td (Jane Doe) Tj ET EMC");
    const int widget = t.Pdf().Add("<< /Type /Annot /Subtype /Widget /FT /Tx /T (name) /V (Jane Doe) /Rect [72 655 252 672] "
                                   "/StructParent 1 /AP << /N " + std::to_string(ap) + " 0 R >> >>");
    if (objr) t.Kid(form, "<< /Type /OBJR /Obj " + std::to_string(widget) + " 0 R /Pg " + std::to_string(t.PageObject(0)) + " 0 R >>");
    t.PageExtra(0, "/Annots [" + std::to_string(widget) + " 0 R]");
    t.CatalogExtra("/AcroForm << /Fields [" + std::to_string(widget) + " 0 R] >>");
    const std::string pdf = t.Build({MarkedText("H1", 2, 72, 740, 18, "Application") +
                                     MarkedText("P", 0, 72, 700, 12, "Applicant name below the line.") +
                                     MarkedText("P", 1, 72, 600, 12, "Signed at the end of the form.")});
    ConvertReport report;
    const std::string md = Convert(pdf, AnnotationMode::All, &report);
    // The tree is used (the value is no untagged content against coverage), and the value is kept
    // in its element's place: between the two paragraphs.
    RAYOPDF_CHECK(report.structure == StructureUse::Used, objr ? 2618 : 2620);
    RAYOPDF_CHECK(Has(md, "Jane Doe") && md.find("Applicant") < md.find("Jane Doe") && md.find("Jane Doe") < md.find("Signed"),
                  objr ? 2619 : 2621);
}

void OcrRouting() {
    // A scanned form: a page image and filled fields. The fields' text is no text layer: the page
    // still goes to OCR whole.
    FormDoc f;
    std::string pixels(100 * 100, '\xF0');
    const int image = f.b.AddStream("/Type /XObject /Subtype /Image /Width 100 /Height 100 /ColorSpace /DeviceGray "
                                    "/BitsPerComponent 8",
                                    pixels);
    TextWidget(f, "120 695 300 712", "a", "Filled value one");
    TextWidget(f, "120 655 300 672", "b", "Filled value two");
    TextWidget(f, "120 615 300 632", "c", "Filled value three");
    TextWidget(f, "120 575 300 592", "d", "Filled value four");
    const std::string content = "q 612 0 0 792 0 0 cm /Im1 Do Q";
    // The page's resources carry the image: rebuild the page with them.
    const int catalog = f.b.Reserve();
    const int pages = f.b.Reserve();
    const int stream = f.b.AddFlateStream("", content);
    std::string list;
    for (int n : f.annots) list += std::to_string(n) + " 0 R ";
    const int page = f.b.Add("<< /Type /Page /Parent " + std::to_string(pages) +
                             " 0 R /MediaBox [0 0 612 792] /Resources << /XObject << /Im1 " + std::to_string(image) +
                             " 0 R >> >> /Contents " + std::to_string(stream) + " 0 R /Annots [" + list + "] >>");
    f.b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
    f.b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] /Count 1 >>");
    const std::string pdf = f.b.Build(catalog);
    std::unique_ptr<Document> document = Open(pdf);
    Text::FontCache fonts(*document);
    PageData data;
    PageStats stats;
    InterpretPage(*document, fonts, 0, AnnotationMode::All, data, stats);
    RAYOPDF_CHECK(data.annotationEnd - data.annotationFirst >= 50 && data.images.size() == 1, 2622);
    PageTextQuality quality;
    quality.glyphs = stats.glyphs;
    quality.unreliable = stats.unmapped + stats.guessed;
    PageRoute route;
    RoutePage(data, quality, OcrMode::Auto, route);
    RAYOPDF_CHECK(route.plan.route == OcrRoute::Page && !route.plan.textLayer, 2623);
}

void Duplicates() {
    // A filled form printed flat with its widgets kept: the value twice at one place reads once,
    // and is not made bold.
    FormDoc f;
    TextWidget(f, "120 695 300 712", "name", "Jane Doe");
    const std::string md = Convert(f.Build(std::string(kLabel) + "BT /F1 10 Tf 122 699 Td (Jane Doe) Tj ET"));
    RAYOPDF_CHECK(Has(md, "Jane Doe") && !Has(md, "**Jane Doe**") && md.find("Jane Doe") == md.rfind("Jane Doe"), 2624);
}

void LinksAsBefore() {
    // --annotations=links reads a form as 1.1 did: the same output as the file without its
    // widgets and FreeText annotations, links kept.
    const auto build = [](bool forms) {
        FormDoc f;
        if (forms) TextWidget(f, "120 695 300 712", "name", "Jane Doe");
        f.Annot("<< /Type /Annot /Subtype /Link /Rect [70 695 110 712] /A << /S /URI /URI (https://example.org/n) >> >>");
        if (forms) {
            const int note = f.Appearance("0 0 200 20", "BT /Helv 10 Tf 2 5 Td (A note) Tj ET");
            f.Annot("<< /Type /Annot /Subtype /FreeText /Rect [72 560 272 580] /AP << /N " + std::to_string(note) +
                    " 0 R >> >>");
        }
        return f.Build(std::string(kLabel) + "BT /F1 12 Tf 72 640 Td (Some more text on the page.) Tj ET");
    };
    ConvertReport a, b;
    const std::string with = Convert(build(true), AnnotationMode::Links, &a);
    const std::string without = Convert(build(false), AnnotationMode::All, &b);
    RAYOPDF_CHECK(with == without && Has(with, "](https://example.org/n)") && a.glyphs == b.glyphs && a.links == b.links,
                  2625);
}

std::string Json(const std::string& pdf, AnnotationMode mode) {
    std::unique_ptr<Document> document = Open(pdf);
    ConvertOptions options;
    options.annotations = mode;
    options.format = OutputFormat::Json;
    options.ocr.mode = OcrMode::Off;
    std::string json;
    ConvertReport report;
    Layout::Convert(*document, options, json, report);
    return json;
}

void JsonFields() {
    // The JSON output's "fields": terminal fields with a widget on a converted page, their full
    // names, inherited types and values.
    FormDoc f;
    const int name = TextWidget(f, "120 695 300 712", "name", "Jane Doe", "/Ff 2");
    const int agree = f.Annot("<< /Type /Annot /Subtype /Widget /FT /Btn /T (agree) /V /Yes /AS /Yes /Rect [72 678 82 688] >>");
    const int group = f.Annot("", false);
    const int a = f.Annot("<< /Type /Annot /Subtype /Widget /AS /Off /Rect [72 638 82 648] /Parent " +
                          std::to_string(group) + " 0 R >>");
    const int b = f.Annot("<< /Type /Annot /Subtype /Widget /AS /1 /Rect [72 618 82 628] /Parent " +
                          std::to_string(group) + " 0 R >>");
    f.b.Set(group, "<< /FT /Btn /Ff 32768 /T (size) /V /1 /Opt [(Small) (Large)] /Kids [" + std::to_string(a) + " 0 R " +
                       std::to_string(b) + " 0 R] >>");
    const int multi = f.Annot("<< /Type /Annot /Subtype /Widget /FT /Ch /Ff 2097152 /T (langs) /V [(de) (fr)] "
                              "/Rect [120 535 300 552] >>");
    const int secret = f.Annot("<< /Type /Annot /Subtype /Widget /FT /Tx /Ff 8192 /T (pin) /V (1234) /Rect [120 495 300 512] >>");
    // A parent holding a kid field, and a field whose widget is on no page.
    const int parent = f.b.Reserve();
    const int child = f.Annot("<< /Type /Annot /Subtype /Widget /T (city) /V (Lyon) /Rect [120 455 300 472] /Parent " +
                              std::to_string(parent) + " 0 R >>");
    f.b.Set(parent, "<< /T (address) /FT /Tx /Kids [" + std::to_string(child) + " 0 R] >>");
    const int nowhere = f.b.Add("<< /Type /Annot /Subtype /Widget /FT /Tx /T (lost) /V (gone) /Rect [0 0 10 10] >>");
    std::string list;
    for (int n : {name, agree, group, multi, secret, parent, nowhere}) list += std::to_string(n) + " 0 R ";
    const std::string pdf = f.Build(kLabel, "<< /Fields [" + list + "] >>");
    const std::string json = Json(pdf, AnnotationMode::All);
    RAYOPDF_CHECK(Has(json, "],\"fields\":[{\"name\":\"name\",\"type\":\"text\",\"value\":\"Jane Doe\",\"page\":1,\"box\":[") &&
                      Has(json, "\"read_only\":false,\"required\":true}") && json.find("\"fields\"") < json.find("\"blocks\""),
                  2629);
    RAYOPDF_CHECK(Has(json, "{\"name\":\"agree\",\"type\":\"checkbox\",\"value\":true,") &&
                      Has(json, "{\"name\":\"size\",\"type\":\"radio\",\"value\":\"Large\",") &&
                      Has(json, "{\"name\":\"langs\",\"type\":\"choice\",\"value\":[\"de\",\"fr\"],"),
                  2630);
    RAYOPDF_CHECK(Has(json, "{\"name\":\"pin\",\"type\":\"text\",\"value\":null,") && !Has(json, "1234") &&
                      Has(json, "{\"name\":\"address.city\",\"type\":\"text\",\"value\":\"Lyon\",") && !Has(json, "lost"),
                  2631);
    // Not with --annotations=links (1.1's JSON), nor in a file without fields.
    RAYOPDF_CHECK(!Has(Json(pdf, AnnotationMode::Links), "\"fields\"") &&
                      !Has(Json(Test::OnePage("BT /F1 12 Tf 72 700 Td (No form.) Tj ET"), AnnotationMode::All), "\"fields\""),
                  2632);
}

void JsonAnnotations() {
    // The JSON output's "annotations": markup annotations in /Annots order with their boxes,
    // contents, author, date and the text a text markup marks; the Markdown stays as it is.
    const auto build = [](bool annotated) {
        FormDoc f;
        if (annotated) {
            f.Annot("<< /Type /Annot /Subtype /Highlight /Rect [0 630 612 660] /QuadPoints [0 654 612 654 0 636 612 636] "
                    "/Contents (Check this) /T (Ann) /M (D:20240506070809Z) >>");
            f.Annot("<< /Type /Annot /Subtype /Highlight /F 2 /Rect [0 590 612 615] /QuadPoints [0 612 612 612 0 596 612 596] "
                    "/Contents (hidden) >>");
            f.Annot("<< /Type /Annot /Subtype /Text /Rect [500 700 520 720] /Contents <FEFF004E006F00740065> >>");
            f.Annot("<< /Type /Annot /Subtype /StrikeOut /Rect [60 590 400 615] >>");
            f.Annot("<< /Type /Annot /Subtype /Square /Rect [60 500 400 515] /Contents (not markup) >>");
        }
        return f.Build("BT /F1 12 Tf 72 640 Td (Marked words here.) Tj ET\nBT /F1 12 Tf 72 600 Td (Other text.) Tj ET\n"
                       "BT /F1 12 Tf 72 400 Td (More body text below the marks.) Tj ET");
    };
    const std::string pdf = build(true), plain = build(false);
    const std::string json = Json(pdf, AnnotationMode::All);
    RAYOPDF_CHECK(Has(json, "],\"annotations\":[{\"page\":1,\"type\":\"highlight\",\"boxes\":[[0.00,636.00,612.00,654.00]],"
                            "\"contents\":\"Check this\",\"author\":\"Ann\",\"date\":\"2024-05-06T07:08:09Z\","
                            "\"text\":\"Marked words here.\"}") &&
                      json.find("\"annotations\"") < json.find("\"blocks\""),
                  2633);
    RAYOPDF_CHECK(Has(json, ",{\"page\":1,\"type\":\"note\",\"boxes\":[[500.00,700.00,520.00,720.00]],\"contents\":\"Note\","
                            "\"author\":null,\"date\":null,\"text\":null}") &&
                      !Has(json, "hidden") && !Has(json, "not markup"),
                  2634);
    // Without quads a text markup's box is its /Rect.
    RAYOPDF_CHECK(Has(json, "{\"page\":1,\"type\":\"strikeout\",\"boxes\":[[60.00,590.00,400.00,615.00]],\"contents\":null,"
                            "\"author\":null,\"date\":null,\"text\":\"Other text.\"}]"),
                  2635);
    // The Markdown and the blocks do not change; --annotations=links keeps none.
    const std::string bare = Json(plain, AnnotationMode::All);
    RAYOPDF_CHECK(Convert(pdf) == Convert(plain) && !Has(Json(pdf, AnnotationMode::Links), "\"annotations\"") &&
                      !Has(bare, "\"annotations\"") && json.substr(json.find(",\"blocks\"")) == bare.substr(bare.find(",\"blocks\"")),
                  2636);
}

void Xfa() {
    // An XFA form whose fields live only in its XFA data: the Markdown starts with a comment that
    // says so; a form with widgets on its pages, --annotations=links and the JSON have none.
    const std::string comment = "<!-- rayopdf: this PDF is an XFA form; its fields are not read -->\n";
    const auto build = [](bool widget, const std::string& catalogTail) {
        FormDoc f;
        const int xfa = f.Stream("", "<xdp:xdp xmlns:xdp=\"http://ns.adobe.com/xdp/\"></xdp:xdp>");
        if (widget) TextWidget(f, "120 695 300 712", "name", "Jane Doe");
        return f.Build(std::string(kLabel), "<< /Fields [] /XFA " + std::to_string(xfa) + " 0 R >>" + catalogTail);
    };
    const std::string bare = build(false, ""), withWidget = build(true, ""), dynamic = build(true, " /NeedsRendering true");
    const std::string md = Convert(bare);
    RAYOPDF_CHECK(md.compare(0, comment.size(), comment) == 0 && Has(md, "Name:") && md.find(comment, 1) == std::string::npos,
                  2638);
    RAYOPDF_CHECK(!Has(Convert(bare, AnnotationMode::Links), "XFA") && !Has(Json(bare, AnnotationMode::All), "XFA"), 2639);
    RAYOPDF_CHECK(!Has(Convert(withWidget), "XFA") && Convert(dynamic).compare(0, comment.size(), comment) == 0, 2640);
}

void Limits() {
    // Past kMaxAnnotationsPerPage drawn annotations the page counts a limit hit; a form that draws
    // itself ends as a content error.
    FormDoc f;
    const int self = f.b.Reserve();
    f.b.Set(self, "<< /Type /XObject /Subtype /Form /BBox [0 0 20 10] /Resources << /XObject << /X " +
                      std::to_string(self) + " 0 R >> >> /Length 6 >>\nstream\n/X Do \nendstream");
    f.Annot("<< /Type /Annot /Subtype /FreeText /Rect [500 700 520 710] /AP << /N " + std::to_string(self) + " 0 R >> >>");
    const std::string pdf = f.Build("BT /F1 12 Tf 72 740 Td (A form drawing itself.) Tj ET");
    std::unique_ptr<Document> cyclic = Open(pdf);
    Text::FontCache fonts(*cyclic);
    PageData page;
    PageStats stats;
    InterpretPage(*cyclic, fonts, 0, AnnotationMode::All, page, stats);
    RAYOPDF_CHECK(page.contentErrors > 0 && page.annotationTexts.empty() && !page.limitHit, 2627);

    FormDoc g;
    const int n2 = g.Appearance("0 0 20 10", "BT /Helv 8 Tf 1 2 Td (n) Tj ET");
    std::string many;
    for (uint32_t i = 0; i < kMaxAnnotationsPerPage + 4; ++i) {
        const int x = 20 + static_cast<int>(i % 25) * 22, y = 40 + static_cast<int>(i / 25) * 4;
        many += "<< /Type /Annot /Subtype /FreeText /Rect [" + std::to_string(x) + " " + std::to_string(y) + " " +
                std::to_string(x + 20) + " " + std::to_string(y + 10) + "] /AP << /N " + std::to_string(n2) + " 0 R >> >> ";
    }
    const int catalog = g.b.Reserve();
    const int pages = g.b.Reserve();
    const int stream = g.b.AddFlateStream("", "BT /F1 12 Tf 72 740 Td (Many notes.) Tj ET");
    const int pageObject = g.b.Add("<< /Type /Page /Parent " + std::to_string(pages) +
                                   " 0 R /MediaBox [0 0 612 792] /Contents " + std::to_string(stream) + " 0 R /Annots [" +
                                   many + "] >>");
    g.b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
    g.b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(pageObject) + " 0 R] /Count 1 >>");
    const std::string crowdedPdf = g.b.Build(catalog);
    std::unique_ptr<Document> crowded = Open(crowdedPdf);
    Text::FontCache crowdedFonts(*crowded);
    InterpretPage(*crowded, crowdedFonts, 0, AnnotationMode::All, page, stats);
    RAYOPDF_CHECK(page.limitHit && page.annotationTexts.size() == kMaxAnnotationsPerPage, 2628);
    // Every one is also a markup annotation, kept up to kMaxMarkupsPerPage.
    RAYOPDF_CHECK(page.markups.size() == kMaxMarkupsPerPage && page.markupBoxes.size() == kMaxMarkupsPerPage, 2637);
}

} // namespace

int main() {
    FilledTextField();
    Synthesized();
    StateDictionary();
    CheckBoxes();
    CombAndHidden();
    NoteNoHeading();
    NoteOverScan();
    FontFallback();
    Tagged(true);
    Tagged(false);
    OcrRouting();
    Duplicates();
    LinksAsBefore();
    JsonFields();
    JsonAnnotations();
    Xfa();
    Limits();
    std::puts("test_layout_forms: ok");
    return 0;
}
