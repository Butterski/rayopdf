// SPDX-License-Identifier: Apache-2.0
// Builds small PDF files in memory for the core tests and fuzz seeds: numbered objects, streams,
// a classic cross-reference table and trailer, or a cross-reference stream (optionally with the
// objects in an object stream). Flate data is written as stored (uncompressed) deflate blocks,
// which every inflater must accept. PDFs are never committed (AGENTS.md rule 1): tests make the
// files they read.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace RayoPdf::Test {

// A zlib stream (RFC 1950) holding `data` in stored deflate blocks (RFC 1951 section 3.2.4).
inline std::string ZlibStored(std::string_view data) {
    std::string out = "\x78\x01";
    size_t at = 0;
    do {
        const size_t n = data.size() - at < 65535 ? data.size() - at : 65535;
        const bool last = at + n == data.size();
        out.push_back(last ? 1 : 0);
        out.push_back(static_cast<char>(n & 0xFF));
        out.push_back(static_cast<char>(n >> 8));
        out.push_back(static_cast<char>(~n & 0xFF));
        out.push_back(static_cast<char>((~n >> 8) & 0xFF));
        out.append(data.substr(at, n));
        at += n;
    } while (at < data.size());
    uint32_t a = 1, b = 0;
    for (unsigned char c : data) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t adler = (b << 16) | a;
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<char>((adler >> shift) & 0xFF));
    return out;
}

// What Build writes besides the classic layout.
struct BuildOptions {
    // A cross-reference stream (ISO 32000-1 section 7.5.8; /W [1 4 2], Flate) instead of the
    // table and trailer.
    bool xrefStream = false;
    // The cross-reference stream's rows through PNG predictor 12 (Up), as most writers do.
    bool pngPredictor = false;
    // Every object that is no stream in one object stream (section 7.5.7); implies xrefStream.
    bool objectStream = false;
};

class PdfBuilder {
public:
    // A new object number whose body Set fills later.
    int Reserve() {
        bodies_.emplace_back();
        streams_.push_back(false);
        return static_cast<int>(bodies_.size());
    }
    // Adds an object with `body` (a dictionary, array, number, ... as PDF text); its number.
    int Add(std::string body) {
        const int num = Reserve();
        Set(num, std::move(body));
        return num;
    }
    void Set(int num, std::string body) { bodies_[static_cast<size_t>(num - 1)] = std::move(body); }
    // Adds a stream: `dict` is the dictionary's entries without "<<" ">>" and without /Length.
    int AddStream(const std::string& dict, std::string_view data) {
        const int num = Add(StreamBody(dict, data));
        streams_[static_cast<size_t>(num - 1)] = true;
        return num;
    }
    // A Flate stream of `data` (stored blocks).
    int AddFlateStream(const std::string& dict, std::string_view data) {
        return AddStream(dict + " /Filter /FlateDecode", ZlibStored(data));
    }

    // The file: header, objects, classic xref, trailer with /Root `root` and `trailerExtra`.
    std::string Build(int root, const std::string& trailerExtra = "") const {
        std::string out = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
        std::vector<size_t> offsets;
        for (size_t i = 0; i < bodies_.size(); ++i) {
            offsets.push_back(out.size());
            out += std::to_string(i + 1) + " 0 obj\n" + bodies_[i] + "\nendobj\n";
        }
        const size_t xref = out.size();
        out += "xref\n0 " + std::to_string(bodies_.size() + 1) + "\n0000000000 65535 f \n";
        char line[32];
        for (size_t offset : offsets) {
            std::snprintf(line, sizeof line, "%010zu 00000 n \n", offset);
            out += line;
        }
        out += "trailer\n<< /Size " + std::to_string(bodies_.size() + 1) + " /Root " + std::to_string(root) +
               " 0 R " + trailerExtra + " >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
        return out;
    }

    // The file with a cross-reference stream as `options` says; the stream's dictionary holds
    // /Root `root` and `trailerExtra`. The object stream, when there is one, and the
    // cross-reference stream take the next two object numbers.
    std::string Build(int root, const std::string& trailerExtra, const BuildOptions& options) const {
        if (!options.xrefStream && !options.objectStream) return Build(root, trailerExtra);
        std::string out = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
        const size_t count = bodies_.size();
        // Per object: type 1 (offset) or 2 (object stream, index).
        std::vector<int> types(count + 3, 0);
        std::vector<size_t> field2(count + 3, 0);
        std::vector<size_t> field3(count + 3, 0);
        std::string header;
        std::string packed;
        int packedCount = 0;
        const size_t container = count + 1;
        for (size_t i = 0; i < count; ++i) {
            if (options.objectStream && !streams_[i]) {
                types[i + 1] = 2;
                field2[i + 1] = container;
                field3[i + 1] = static_cast<size_t>(packedCount++);
                header += std::to_string(i + 1) + " " + std::to_string(packed.size()) + " ";
                packed += bodies_[i] + "\n";
                continue;
            }
            types[i + 1] = 1;
            field2[i + 1] = out.size();
            out += std::to_string(i + 1) + " 0 obj\n" + bodies_[i] + "\nendobj\n";
        }
        size_t next = count + 1;
        if (options.objectStream) {
            types[container] = 1;
            field2[container] = out.size();
            const std::string data = header + packed;
            out += std::to_string(container) + " 0 obj\n" +
                   StreamBody("/Type /ObjStm /N " + std::to_string(packedCount) + " /First " +
                                  std::to_string(header.size()) + " /Filter /FlateDecode",
                              ZlibStored(data)) +
                   "\nendobj\n";
            next++;
        }
        const size_t self = next;
        const size_t size = self + 1;
        types[self] = 1;
        field2[self] = out.size();
        std::string rows;
        std::string previous(7, '\0');
        for (size_t num = 0; num < size; ++num) {
            unsigned char row[7];
            if (num == 0 || types[num] == 0) {
                row[0] = 0;
                for (int k = 1; k < 5; ++k) row[k] = 0;
                row[5] = 0xFF;
                row[6] = 0xFF;
            } else {
                row[0] = static_cast<unsigned char>(types[num]);
                for (int k = 0; k < 4; ++k) row[1 + k] = static_cast<unsigned char>(field2[num] >> (24 - 8 * k));
                row[5] = static_cast<unsigned char>(field3[num] >> 8);
                row[6] = static_cast<unsigned char>(field3[num]);
            }
            if (options.pngPredictor) {
                rows.push_back('\x02');  // Up
                for (int k = 0; k < 7; ++k) {
                    const unsigned char up = static_cast<unsigned char>(previous[static_cast<size_t>(k)]);
                    rows.push_back(static_cast<char>(row[k] - up));
                }
                previous.assign(reinterpret_cast<const char*>(row), 7);
            } else {
                rows.append(reinterpret_cast<const char*>(row), 7);
            }
        }
        std::string dict = "/Type /XRef /Size " + std::to_string(size) + " /W [1 4 2] /Root " + std::to_string(root) +
                           " 0 R " + trailerExtra + " /Filter /FlateDecode";
        if (options.pngPredictor) dict += " /DecodeParms << /Predictor 12 /Columns 7 >>";
        out += std::to_string(self) + " 0 obj\n" + StreamBody(dict, ZlibStored(rows)) + "\nendobj\n";
        out += "startxref\n" + std::to_string(field2[self]) + "\n%%EOF\n";
        return out;
    }

private:
    static std::string StreamBody(const std::string& dict, std::string_view data) {
        std::string body = "<< " + dict + " /Length " + std::to_string(data.size()) + " >>\nstream\n";
        body.append(data);
        body += "\nendstream";
        return body;
    }

    std::vector<std::string> bodies_;
    std::vector<bool> streams_;
};

// A one-page document (US Letter) showing `content` with the font resources `fonts` (the entries
// of /Font, for example "/F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>").
inline std::string OnePage(std::string_view content,
                           const std::string& fonts = "/F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>") {
    PdfBuilder b;
    const int catalog = b.Reserve();
    const int pages = b.Reserve();
    const int stream = b.AddFlateStream("", content);
    const int page = b.Add("<< /Type /Page /Parent " + std::to_string(pages) +
                           " 0 R /MediaBox [0 0 612 792] /Resources << /Font << " + fonts +
                           " >> >> /Contents " + std::to_string(stream) + " 0 R >>");
    b.Set(catalog, "<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
    b.Set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] /Count 1 >>");
    return b.Build(catalog);
}

// Tagged documents (ISO 32000-1 §14.7-14.8): US Letter pages whose content marks its text with
// MCIDs, and a structure tree built element by element. Elements are named by the handles
// Element returns (1-based); 0 is the tree root.
class TaggedBuilder {
public:
    explicit TaggedBuilder(int pageCount = 1) {
        catalog_ = pdf_.Reserve();
        pages_ = pdf_.Reserve();
        root_ = pdf_.Reserve();
        for (int i = 0; i < pageCount; ++i) pageObjects_.push_back(pdf_.Reserve());
        parentTree_.resize(static_cast<size_t>(pageCount));
        pageExtra_.resize(static_cast<size_t>(pageCount));
    }
    // The builder underneath, for further objects (Form XObjects).
    PdfBuilder& Pdf() { return pdf_; }
    int PageObject(int page) const { return pageObjects_[static_cast<size_t>(page)]; }
    int ObjectOf(int element) const { return elements_[static_cast<size_t>(element - 1)].object; }
    // A structure element of type `type` (without the slash) as the next kid of `parent`; `extra`
    // adds dictionary entries ("/Alt (a chart)", "/A << /O /Table /ColSpan 2 >>").
    int Element(int parent, const std::string& type, const std::string& extra = "") {
        Item item;
        item.object = pdf_.Reserve();
        item.parent = parent;
        item.type = type;
        item.extra = extra;
        elements_.push_back(item);
        const int handle = static_cast<int>(elements_.size());
        Kid(parent, std::to_string(item.object) + " 0 R");
        return handle;
    }
    // Marked-content id `mcid` of page `page` (0-based) as the next kid of `element`. Without
    // `withPage` the element gets no /Pg: readers find the page through the ParentTree.
    void Mcid(int element, int page, int mcid, bool withPage = true) {
        Item& item = elements_[static_cast<size_t>(element - 1)];
        if (withPage && item.page < 0) item.page = page;
        Kid(element, std::to_string(mcid));
        std::vector<int>& slots = parentTree_[static_cast<size_t>(page)];
        if (slots.size() <= static_cast<size_t>(mcid)) slots.resize(static_cast<size_t>(mcid) + 1, 0);
        slots[static_cast<size_t>(mcid)] = item.object;
    }
    // Any kid as PDF text (an MCR or OBJR dictionary, a reference).
    void Kid(int element, const std::string& kid) {
        std::string& kids = element == 0 ? rootKids_ : elements_[static_cast<size_t>(element - 1)].kids;
        kids += kid + " ";
    }
    // More entries for page `page`'s dictionary ("/Annots [12 0 R]") and for the catalog
    // ("/AcroForm << ... >>").
    void PageExtra(int page, const std::string& entries) { pageExtra_[static_cast<size_t>(page)] += " " + entries; }
    void CatalogExtra(const std::string& entries) { catalogExtra_ += " " + entries; }
    // The file: page `i` shows contents[i] with Helvetica as /F1 and `resources` (more resource
    // entries); the StructTreeRoot carries `rootExtra` (a /RoleMap).
    std::string Build(const std::vector<std::string>& contents, const std::string& rootExtra = "",
                      const std::string& resources = "") {
        std::string kids;
        for (size_t i = 0; i < pageObjects_.size(); ++i) {
            const int stream = pdf_.AddFlateStream("", i < contents.size() ? contents[i] : std::string());
            pdf_.Set(pageObjects_[i], "<< /Type /Page /Parent " + std::to_string(pages_) +
                                          " 0 R /MediaBox [0 0 612 792] /StructParents " + std::to_string(i) +
                                          " /Resources << /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont "
                                          "/Helvetica >> >> " +
                                          resources + " >> /Contents " + std::to_string(stream) + " 0 R" +
                                          pageExtra_[i] + " >>");
            kids += std::to_string(pageObjects_[i]) + " 0 R ";
        }
        for (const Item& item : elements_) {
            const std::string parent = std::to_string(item.parent == 0 ? root_ : ObjectOf(item.parent)) + " 0 R";
            std::string body = "<< /Type /StructElem /S /" + item.type + " /P " + parent;
            if (item.page >= 0) body += " /Pg " + std::to_string(pageObjects_[static_cast<size_t>(item.page)]) + " 0 R";
            body += " /K [" + item.kids + "] " + item.extra + " >>";
            pdf_.Set(item.object, body);
        }
        std::string nums;
        for (size_t i = 0; i < parentTree_.size(); ++i) {
            nums += std::to_string(i) + " [";
            for (int object : parentTree_[i]) nums += object ? std::to_string(object) + " 0 R " : std::string("null ");
            nums += "] ";
        }
        pdf_.Set(root_, "<< /Type /StructTreeRoot /K [" + rootKids_ + "] /ParentTree << /Nums [" + nums +
                            "] >> /ParentTreeNextKey " + std::to_string(parentTree_.size()) + " " + rootExtra + " >>");
        pdf_.Set(pages_, "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pageObjects_.size()) + " >>");
        pdf_.Set(catalog_, "<< /Type /Catalog /Pages " + std::to_string(pages_) + " 0 R /StructTreeRoot " +
                               std::to_string(root_) + " 0 R /MarkInfo << /Marked true >>" + catalogExtra_ + " >>");
        return pdf_.Build(catalog_);
    }

private:
    struct Item {
        int object = 0;
        int parent = 0;
        int page = -1;
        std::string type, extra, kids;
    };
    PdfBuilder pdf_;
    int catalog_ = 0, pages_ = 0, root_ = 0;
    std::vector<int> pageObjects_;
    std::vector<Item> elements_;
    std::string rootKids_;
    std::vector<std::vector<int>> parentTree_;
    std::vector<std::string> pageExtra_;
    std::string catalogExtra_;
};

// Content-stream text `text` (no parentheses or backslashes) at (x, y) in Helvetica `size`, in a
// marked-content sequence of tag `tag` with MCID `mcid` (no MCID when negative).
inline std::string MarkedText(const std::string& tag, int mcid, double x, double y, double size, const std::string& text) {
    char head[160];
    if (mcid >= 0)
        std::snprintf(head, sizeof head, "/%s << /MCID %d >> BDC BT /F1 %g Tf %g %g Td (", tag.c_str(), mcid, size, x, y);
    else
        std::snprintf(head, sizeof head, "/%s BMC BT /F1 %g Tf %g %g Td (", tag.c_str(), size, x, y);
    return std::string(head) + text + ") Tj ET EMC\n";
}

} // namespace RayoPdf::Test
