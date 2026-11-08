// SPDX-License-Identifier: Apache-2.0
// Builds PDFs in RayoMD's rayomd-source/1 layout (pdf/rayomd_source.h) for the exact-recovery
// tests: one US Letter page showing `lines` in Helvetica, the source as an unfiltered
// /EmbeddedFile, its /Filespec, the XMP packet and the catalog, laid out object by object as
// `rayomd --export ... --embed-source` writes them. Each field can be bent to make a broken file.
// PDFs are never committed (AGENTS.md rule 1): tests make the files they read.
#pragma once

#include "pdf/rayomd_source.h"

#include <cstdio>
#include <string>
#include <vector>

namespace RayoPdf::Test {

struct ProfilePdf {
    std::string source;
    std::vector<std::string> lines;  // the page's text, one line each (printable ASCII)
    std::string header = "%PDF-2.0";
    std::string profile = "rayomd-source/1";
    std::string encoding = "UTF-8";
    std::string xmpExtra;             // more attributes of the rdf:Description (" pdfaid:part=\"3\"")
    long long declaredLength = -1;    // the packet's rayomd:length; -1: the source's size
    std::string digest;               // the packet's rayomd:sha256; empty: the source's
    std::string sourceExtra;          // more entries of the /EmbeddedFile dictionary
    std::string trailerExtra;         // more entries of the trailer
};

inline std::string PdfLiteral(const std::string& text) {
    std::string out = "(";
    for (char c : text) {
        if (c == '(' || c == ')' || c == '\\') out += '\\';
        out += c;
    }
    return out + ")";
}

inline std::string BuildProfilePdf(const ProfilePdf& spec) {
    std::string content;
    double y = 740;
    for (const std::string& line : spec.lines) {
        char position[64];
        std::snprintf(position, sizeof position, "BT /F1 12 Tf 72 %.0f Td ", y);
        content += position + PdfLiteral(line) + " Tj ET\n";
        y -= 18;
    }
    char digest[64];
    RayomdSource::Sha256Hex(spec.source, digest);
    const long long length = spec.declaredLength >= 0 ? spec.declaredLength : static_cast<long long>(spec.source.size());
    const std::string xmp =
        "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?><x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
        "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description "
        "xmlns:rayomd=\"https://rayomd.dev/ns/source/1.0/\" rayomd:profile=\"" + spec.profile +
        "\" rayomd:producer=\"5.3.0\" rayomd:encoding=\"" + spec.encoding + "\" rayomd:length=\"" +
        std::to_string(length) + "\" rayomd:sha256=\"" +
        (spec.digest.empty() ? std::string(digest, sizeof digest) : spec.digest) +
        "\" rayomd:attachment=\"source.md\"" + spec.xmpExtra +
        "/></rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>";
    const auto stream = [](const std::string& dict, const std::string& data) {
        return "<< " + dict + " /Length " + std::to_string(data.size()) + " >>\nstream\n" + data + "\nendstream";
    };
    const std::vector<std::string> objects = {
        "<< /Type /Pages /Kids [4 0 R] /Count 1 >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
        stream("", content),
        "<< /Type /Page /Parent 1 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 2 0 R >> >> "
        "/Contents 3 0 R >>",
        stream("/Type /EmbeddedFile /Subtype /text#2Fmarkdown /Params << /Size " + std::to_string(length) + " >>" +
                   spec.sourceExtra,
               spec.source),
        "<< /Type /Filespec /F (source.md) /UF (source.md) /EF << /F 5 0 R /UF 5 0 R >> /AFRelationship /Source >>",
        stream("/Type /Metadata /Subtype /XML", xmp),
        "<< /Type /Catalog /Pages 1 0 R /Metadata 7 0 R /Names << /EmbeddedFiles << /Names [(source.md) 6 0 R] >> "
        ">> /AF [6 0 R] >>",
    };
    std::string pdf = spec.header + "\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> offsets;
    for (size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t offset : offsets) {
        char entry[24];
        std::snprintf(entry, sizeof entry, "%010zu 00000 n \n", offset);
        pdf += entry;
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 8 0 R" + spec.trailerExtra +
           " >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return pdf;
}

} // namespace RayoPdf::Test
