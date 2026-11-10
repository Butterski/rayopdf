// SPDX-License-Identifier: Apache-2.0
// Core tests of the pages of one document read on several threads at once (roadmap D4): the
// Document's shared mode (objects parsed once whatever the thread, a section whose result could
// depend on the order undone), one FontCache shared by several threads (each font loaded and
// completed once), Convert's Markdown and counters the same for 1, 2, 4 and 8 page workers, and
// the page threads' pool. Exit codes 1600-1649.
#include "check.h"
#include "pdf_builder.h"

#include "layout/convert.h"
#include "layout/page_pool.h"
#include "pdf/document.h"
#include "text/font.h"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace RayoPdf;
using namespace RayoPdf::Test;

namespace {

constexpr int kThreads = 4;

std::unique_ptr<Document> OpenOk(const std::string& pdf, int code) {
    std::unique_ptr<Document> doc;
    RAYOPDF_CHECK(Document::Open(pdf, "", doc) == OpenStatus::Ok, code);
    return doc;
}

bool SameReport(const DocumentReport& a, const DocumentReport& b) {
    return a.repaired == b.repaired && a.brokenObjects == b.brokenObjects && a.salvagedStreams == b.salvagedStreams &&
           a.failedStreams == b.failedStreams && a.limitHits == b.limitHits;
}

bool SameReport(const Layout::ConvertReport& a, const Layout::ConvertReport& b) {
    return a.pages == b.pages && a.pagesWithoutText == b.pagesWithoutText && a.glyphs == b.glyphs &&
           a.unmappedGlyphs == b.unmappedGlyphs && a.guessedGlyphs == b.guessedGlyphs &&
           a.hiddenGlyphs == b.hiddenGlyphs && a.headings == b.headings && a.paragraphs == b.paragraphs &&
           a.listItems == b.listItems && a.codeBlocks == b.codeBlocks && a.tables == b.tables &&
           a.links == b.links && a.footnotes == b.footnotes && a.headerFooterLines == b.headerFooterLines &&
           a.contentErrors == b.contentErrors && a.structure == b.structure && a.exact == b.exact &&
           a.ocrBrokenPages == b.ocrBrokenPages && a.ocrVectorTextPages == b.ocrVectorTextPages;
}

// Page `index`'s content, copied (PageContent's view lasts until the thread's next call).
std::string Content(const Document& doc, int index) {
    std::string_view view;
    doc.PageContent(index, view);
    return std::string(view);
}

// A ToUnicode CMap mapping the codes `low` to `high` to the same code points.
std::string IdentityToUnicode(char low, char high) {
    char range[64];
    std::snprintf(range, sizeof range, "<%02X> <%02X> <%04X>", static_cast<unsigned char>(low),
                  static_cast<unsigned char>(high), static_cast<unsigned char>(low));
    return std::string("/CIDInit /ProcSet findresource begin 12 dict begin begincmap /CMapName /T def\n"
                       "1 begincodespacerange <00> <FF> endcodespacerange\n1 beginbfrange ") +
           range + " endbfrange\nendcmap CMapName currentdict /CMap defineresource pop end end\n";
}

// A document of `count` pages that share fonts: F1 Helvetica; F2 a TrueType font whose ToUnicode
// gives only the capitals and which has /Widths, so it waits for a lower-case code (shown from page
// 3 on) to complete itself; F3 a direct dictionary in each page's resources. Each page also links
// to a URI. `cycle` makes the contents of pages 1 and 2 two streams whose /Length names the other
// (an object needed while it is parsed: the result depends on which page reads first).
std::string ManyPages(int count, bool cycle = false, const BuildOptions& options = BuildOptions()) {
    PdfBuilder b;
    const int catalog = b.Reserve();
    const int pagesNum = b.Reserve();
    const int helvetica = b.Add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
    const int toUnicode = b.AddFlateStream("", IdentityToUnicode('A', 'Z'));
    std::string widths = "[";
    for (int c = 32; c <= 126; ++c) widths += c % 3 == 0 ? " 600" : " 500";
    widths += "]";
    const int pending = b.Add("<< /Type /Font /Subtype /TrueType /BaseFont /Pending /Encoding /WinAnsiEncoding "
                              "/FirstChar 32 /LastChar 126 /Widths " + widths + " /ToUnicode " +
                              std::to_string(toUnicode) + " 0 R >>");
    const std::string fonts = "/F1 " + std::to_string(helvetica) + " 0 R /F2 " + std::to_string(pending) +
                              " 0 R /F3 << /Type /Font /Subtype /Type1 /BaseFont /Courier >>";
    int cycleA = 0, cycleB = 0;
    if (cycle) {
        cycleA = b.Reserve();
        cycleB = b.Reserve();
        b.Set(cycleA, "<< /Length " + std::to_string(cycleB) + " 0 R >>\nstream\nBT /F1 12 Tf 72 720 Td (Cycle one) Tj ET"
                      "\nendstream");
        b.Set(cycleB, "<< /Length " + std::to_string(cycleA) + " 0 R >>\nstream\nBT /F1 12 Tf 72 720 Td (Cycle two) Tj ET"
                      "\nendstream");
    }
    std::string kids;
    static const char* const kTopics[] = {"Rivers", "Mountains", "Deserts", "Forests", "Oceans", "Islands",
                                          "Glaciers", "Valleys", "Plains", "Lakes"};
    for (int i = 1; i <= count; ++i) {
        std::string content = "BT /F1 18 Tf 72 740 Td (On " + std::string(kTopics[(i - 1) % 10]) + ") Tj ET\n";
        for (int line = 0; line < 12; ++line) {
            content += "BT /F2 11 Tf 72 " + std::to_string(700 - 16 * line) + " Td (PAGE " + std::to_string(i) +
                       " LINE " + std::to_string(line) + (i >= 3 ? " with lower case" : "") + ") Tj ET\n";
        }
        content += "BT /F3 10 Tf 72 400 Td (code_" + std::to_string(i) + "\\(\\)) Tj ET\n";
        content += "BT /F1 9 Tf 300 40 Td (" + std::to_string(i) + ") Tj ET\n";
        int stream = 0;
        if (cycle && i == 1) stream = cycleA;
        else if (cycle && i == 2) stream = cycleB;
        else stream = b.AddFlateStream("", content);
        const int annot = b.Add("<< /Type /Annot /Subtype /Link /Rect [72 735 300 760] /A << /S /URI /URI "
                                "(https://example.com/" + std::to_string(i) + ") >> >>");
        const int page = b.Add("<< /Type /Page /Parent " + std::to_string(pagesNum) +
                               " 0 R /MediaBox [0 0 612 792] /Resources << /Font << " + fonts +
                               " >> >> /Annots [" + std::to_string(annot) + " 0 R] /Contents " +
                               std::to_string(stream) + " 0 R >>");
        kids += std::to_string(page) + " 0 R ";
    }
    b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pagesNum) + " 0 R >>");
    b.Set(pagesNum, "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(count) + " >>");
    return b.Build(catalog, "", options);
}

// Shared mode with nothing order-sensitive: every thread reads every object and every page, in
// orders of its own; the section stands, and objects, contents and counts are those of one thread.
void TestSharedObjects() {
    BuildOptions packed;
    packed.objectStream = true;
    for (const std::string& pdf : {ManyPages(8), ManyPages(8, false, packed)}) {
        const auto serial = OpenOk(pdf, 1600);
        const auto shared = OpenOk(pdf, 1600);
        const uint32_t objects = serial->ObjectCount();
        std::vector<std::string> contents(static_cast<size_t>(serial->PageCount()));
        for (uint32_t num = 1; num < objects; ++num) serial->Object(num);
        for (int i = 0; i < serial->PageCount(); ++i) contents[static_cast<size_t>(i)] = Content(*serial, i);
        RAYOPDF_CHECK(shared->BeginShared(), 1601);
        std::vector<int> wrong(kThreads, 0);
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                for (uint32_t k = 1; k < objects; ++k) {
                    const uint32_t num = t % 2 ? objects - k : (k * 7 + static_cast<uint32_t>(t)) % (objects - 1) + 1;
                    if (shared->Object(num).type != serial->Object(num).type) wrong[static_cast<size_t>(t)]++;
                }
                std::string_view content;
                for (int i = 0; i < shared->PageCount(); ++i) {
                    const int page = (i + t) % shared->PageCount();
                    shared->PageContent(page, content);
                    if (content != contents[static_cast<size_t>(page)]) wrong[static_cast<size_t>(t)]++;
                    if (shared->CropBox(page).Width() != 612) wrong[static_cast<size_t>(t)]++;
                }
            });
        }
        for (std::thread& thread : threads) thread.join();
        for (int t = 0; t < kThreads; ++t) RAYOPDF_CHECK(wrong[static_cast<size_t>(t)] == 0, 1602);
        RAYOPDF_CHECK(shared->EndShared(), 1603);
        // The contents again on one thread: each stream counted once more on both sides.
        std::string_view content;
        for (int i = 0; i < serial->PageCount(); ++i) {
            serial->PageContent(i, content);
            shared->PageContent(i, content);
        }
        RAYOPDF_CHECK(SameReport(shared->Report(), serial->Report()), 1604);
    }
}

// An object needed while it is parsed: the shared section is undone (objects, counts), and the
// document then reads as if the section had never been.
void TestSharedCycle() {
    const std::string pdf = ManyPages(4, true);
    const auto doc = OpenOk(pdf, 1610);
    const auto fresh = OpenOk(pdf, 1610);
    // Pages 1 and 2: their contents are the first two objects after the fonts' (Reserve order).
    const PdfValue& page = doc->PageDict(0);
    const PdfValue* contents = page.Find("Contents");
    RAYOPDF_CHECK(contents && contents->IsRef(), 1611);
    const DocumentReport before = doc->Report();
    RAYOPDF_CHECK(doc->BeginShared(), 1612);
    Content(*doc, 1);  // enters the cycle at page 2's stream
    RAYOPDF_CHECK(!doc->SharedIntact(), 1613);
    RAYOPDF_CHECK(!doc->EndShared(), 1614);
    RAYOPDF_CHECK(SameReport(doc->Report(), before), 1615);
    // Undone: entered at page 1's stream now, as a document never shared does.
    std::string a = Content(*doc, 0);
    std::string b = Content(*fresh, 0);
    RAYOPDF_CHECK(a == b && a.find("Cycle one") != std::string::npos, 1616);
    a = Content(*doc, 1);
    b = Content(*fresh, 1);
    RAYOPDF_CHECK(a == b && SameReport(doc->Report(), fresh->Report()), 1617);
    // A section without the cycle (both objects parsed now) stands.
    RAYOPDF_CHECK(doc->BeginShared(), 1618);
    Content(*doc, 1);
    RAYOPDF_CHECK(doc->EndShared(), 1619);
}

// Two pages whose contents are streams of one chain of `length`, each stream's /Length the next
// stream: page 1 has the chain's head, page 2 the stream `tail` further on.
std::string Chain(int length, int tail) {
    PdfBuilder b;
    const int catalog = b.Reserve();
    const int pagesNum = b.Reserve();
    std::vector<int> chain;
    for (int i = 0; i < length; ++i) chain.push_back(b.Reserve());
    for (int i = 0; i < length; ++i) {
        const std::string next = i + 1 < length ? std::to_string(chain[static_cast<size_t>(i) + 1]) + " 0 R" : "33";
        b.Set(chain[static_cast<size_t>(i)], "<< /Length " + next + " >>\nstream\nBT /F1 12 Tf 72 720 Td (Link) Tj ET"
                                             "\nendstream");
    }
    std::string kids;
    for (const int stream : {chain[0], chain[static_cast<size_t>(tail)]}) {
        const int page = b.Add("<< /Type /Page /Parent " + std::to_string(pagesNum) + " 0 R /MediaBox [0 0 612 792] "
                               "/Contents " + std::to_string(stream) + " 0 R >>");
        kids += std::to_string(page) + " 0 R ";
    }
    b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pagesNum) + " 0 R >>");
    b.Set(pagesNum, "<< /Type /Pages /Kids [" + kids + "] /Count 2 >>");
    return b.Build(catalog);
}

// The nesting limit that page order hits and another order does not (the chain's tail parsed
// first, so that the head's parse stops at it): the section is undone. A chain within the limit
// read the same way stands.
void TestSharedNesting() {
    const std::string deep = Chain(40, 20);
    const auto serial = OpenOk(deep, 1605);
    Content(*serial, 0);
    Content(*serial, 1);
    const auto doc = OpenOk(deep, 1605);
    const DocumentReport before = doc->Report();
    RAYOPDF_CHECK(serial->Report().limitHits > before.limitHits, 1606);
    RAYOPDF_CHECK(doc->BeginShared(), 1607);
    std::thread([&] { Content(*doc, 1); }).join();
    RAYOPDF_CHECK(doc->SharedIntact(), 1608);
    Content(*doc, 0);
    RAYOPDF_CHECK(!doc->SharedIntact(), 1609);
    RAYOPDF_CHECK(!doc->EndShared(), 1624);
    RAYOPDF_CHECK(SameReport(doc->Report(), before), 1625);
    Content(*doc, 0);
    Content(*doc, 1);
    RAYOPDF_CHECK(SameReport(doc->Report(), serial->Report()), 1626);

    const std::string shallow = Chain(30, 20);
    const auto flat = OpenOk(shallow, 1627);
    const auto other = OpenOk(shallow, 1627);
    Content(*flat, 0);
    Content(*flat, 1);
    RAYOPDF_CHECK(other->BeginShared(), 1628);
    std::thread([&] { Content(*other, 1); }).join();
    Content(*other, 0);
    RAYOPDF_CHECK(other->EndShared(), 1629);
    RAYOPDF_CHECK(SameReport(other->Report(), flat->Report()), 1629);
}

// A StreamReader open: no shared mode (its count could have to come before another thread's).
void TestSharedWithReader() {
    const std::string pdf = ManyPages(2);  // the document views it
    const auto doc = OpenOk(pdf, 1620);
    const PdfValue* contents = doc->PageDict(0).Find("Contents");
    RAYOPDF_CHECK(contents != nullptr, 1620);
    {
        Document::StreamReader reader;
        RAYOPDF_CHECK(reader.Open(*doc, *contents) == StreamStatus::Ok, 1621);
        RAYOPDF_CHECK(!doc->BeginShared(), 1622);
    }
    RAYOPDF_CHECK(doc->BeginShared() && doc->EndShared(), 1623);
}

// One FontCache and three that share it, on four threads: every thread gets the same Font for an
// entry, each font loaded once, and a pending font completes once while others read it.
void TestSharedFonts() {
    const std::string pdf = ManyPages(3);  // the document views it
    const auto doc = OpenOk(pdf, 1630);
    const PdfValue& fonts = doc->Get(doc->Resources(0), "Font");
    RAYOPDF_CHECK(fonts.IsDict(), 1630);
    std::vector<const PdfValue*> entries;
    for (const PdfDictEntry& entry : fonts.Entries()) entries.push_back(&entry.value);
    RAYOPDF_CHECK(entries.size() == 3, 1631);
    Text::FontCache owner(*doc);
    std::vector<std::vector<const Text::Font*>> got(kThreads);
    std::vector<std::string> texts(kThreads);
    RAYOPDF_CHECK(doc->BeginShared(), 1632);
    {
        std::vector<std::unique_ptr<Text::FontCache>> caches;
        for (int t = 1; t < kThreads; ++t) caches.emplace_back(new Text::FontCache(owner));
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                Text::FontCache& cache = t == 0 ? owner : *caches[static_cast<size_t>(t - 1)];
                for (int round = 0; round < 50; ++round) {
                    for (size_t e = 0; e < entries.size(); ++e) {
                        const Text::Font& font = cache.Get(*entries[(e + static_cast<size_t>(t)) % entries.size()]);
                        if (round == 0) got[static_cast<size_t>(t)].push_back(&font);
                    }
                }
                const Text::Font& pending = cache.Get(*entries[1]);
                const std::string_view shown = "ABC lower XYZ";
                size_t at = 0;
                Text::Code code;
                while (pending.Next(shown, at, code)) texts[static_cast<size_t>(t)].append(code.unicode);
                if (pending.SpaceWidth() <= 0) texts[static_cast<size_t>(t)] += "!";
            });
        }
        for (std::thread& thread : threads) thread.join();
    }
    RAYOPDF_CHECK(doc->EndShared(), 1633);
    for (int t = 0; t < kThreads; ++t) {
        RAYOPDF_CHECK(got[static_cast<size_t>(t)].size() == entries.size(), 1634);
        for (const Text::Font* font : got[static_cast<size_t>(t)]) {
            bool known = false;
            for (const Text::Font* other : got[0]) known = known || other == font;
            RAYOPDF_CHECK(known, 1635);
        }
        RAYOPDF_CHECK(texts[static_cast<size_t>(t)] == "ABC lower XYZ", 1636);
    }
    RAYOPDF_CHECK(owner.Count() == entries.size(), 1637);
}

// Convert on 1, 2, 4 and 8 page workers: the same Markdown, the same counters, the same document
// report, for plain, packed (object streams) and order-sensitive documents.
void TestWorkerCounts() {
    BuildOptions packed;
    packed.objectStream = true;
    const std::vector<std::string> pdfs = {ManyPages(2), ManyPages(9), ManyPages(9, false, packed), ManyPages(6, true)};
    for (const std::string& pdf : pdfs) {
        for (const Layout::MathMode math : {Layout::MathMode::Unicode, Layout::MathMode::Latex}) {
            std::string expected;
            Layout::ConvertReport expectedReport;
            DocumentReport expectedDocument;
            for (const unsigned workers : {1u, 2u, 4u, 8u, 0u}) {
                const auto doc = OpenOk(pdf, 1640);
                Layout::ConvertOptions options;
                options.pageWorkers = workers;
                options.math = math;
                std::string markdown;
                Layout::ConvertReport report;
                Layout::Convert(*doc, options, markdown, report);
                if (workers == 1) {
                    expected = markdown;
                    expectedReport = report;
                    expectedDocument = doc->Report();
                    RAYOPDF_CHECK(report.pages == doc->PageCount() && markdown.find("LINE 5") != std::string::npos, 1641);
                    continue;
                }
                RAYOPDF_CHECK(markdown == expected, 1642);
                RAYOPDF_CHECK(SameReport(report, expectedReport), 1643);
                RAYOPDF_CHECK(SameReport(doc->Report(), expectedDocument), 1644);
            }
        }
    }
}

// The pool: each index of a round run once, on worker numbers below Workers(), worker 0 being the
// calling thread; rounds one after another, many of them.
void TestPagePool() {
    Layout::PagePool pool(4);
    pool.Start(3);
    RAYOPDF_CHECK(pool.Workers() == 4, 1645);
    const std::thread::id self = std::this_thread::get_id();
    for (int round = 0; round < 200; ++round) {
        const size_t count = static_cast<size_t>(round % 11) * 3;
        std::vector<std::atomic<int>> runs(count);
        std::atomic<int> wrong{0};
        pool.Run(count, [&](unsigned worker, size_t index) {
            runs[index].fetch_add(1);
            if (worker >= 4 || (worker == 0) != (std::this_thread::get_id() == self)) wrong.fetch_add(1);
        });
        for (const std::atomic<int>& runsOf : runs) RAYOPDF_CHECK(runsOf.load() == 1, 1646);
        RAYOPDF_CHECK(wrong.load() == 0, 1647);
    }
}

// Threads started on demand: none at first; started from a job of the calling thread they join
// the round that runs, started between rounds they serve the next; never more than Workers() - 1,
// and each index still runs once.
void TestPagePoolStart() {
    bool once = true, counts = true;
    for (int round = 0; round < 200; ++round) {
        Layout::PagePool pool(4);
        counts = counts && pool.Started() == 0;
        const size_t count = 64;
        const size_t at = static_cast<size_t>(round % 5);
        const unsigned first = static_cast<unsigned>(round % 4);  // 0: none during the round
        std::vector<std::atomic<int>> runs(count);
        std::atomic<int> wrong{0};
        pool.Run(count, [&](unsigned worker, size_t index) {
            if (worker == 0 && index == at) pool.Start(first);
            runs[index].fetch_add(1);
            if (worker > first) wrong.fetch_add(1);
        });
        for (const std::atomic<int>& runsOf : runs) once = once && runsOf.load() == 1;
        counts = counts && pool.Started() == first && wrong.load() == 0;
        pool.Start(9);
        counts = counts && pool.Started() == 3;
        std::vector<std::atomic<int>> again(7);
        pool.Run(again.size(), [&](unsigned, size_t index) { again[index].fetch_add(1); });
        for (const std::atomic<int>& runsOf : again) once = once && runsOf.load() == 1;
    }
    RAYOPDF_CHECK(once, 1648);
    RAYOPDF_CHECK(counts, 1649);
}

} // namespace

int main() {
    TestSharedObjects();
    TestSharedCycle();
    TestSharedNesting();
    TestSharedWithReader();
    TestSharedFonts();
    TestWorkerCounts();
    TestPagePool();
    TestPagePoolStart();
    std::puts("layout pages tests passed");
    return 0;
}
