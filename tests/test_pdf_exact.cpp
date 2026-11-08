// SPDX-License-Identifier: Apache-2.0
// Core tests of the rayomd-source/1 reader, pdf/rayomd_source.h (exit codes 1300-1329): a valid
// payload, and corrupt, truncated, wrong-version, oversized and malformed ones made with
// tests/exact_builder.h. tests/test_layout_exact.cpp tests the conversion's use of it.
#include "check.h"
#include "exact_builder.h"
#include "pdf_builder.h"

#include "pdf/rayomd_source.h"

#include <cstring>
#include <string>
#include <string_view>

using namespace RayoPdf;
using RayoPdf::Test::BuildProfilePdf;
using RayoPdf::Test::ProfilePdf;

namespace {

RayomdSource::Status StatusOf(std::string_view pdf) {
    RayomdSource::Result result;
    RayomdSource::Read(pdf, result);
    return result.status;
}

ProfilePdf Valid() {
    ProfilePdf spec;
    spec.source = "# Title\n\nSome *text* with a [link](https://example.com).\n";
    spec.lines = {"Title", "Some text with a link."};
    return spec;
}

void TestValid() {
    const std::string pdf = BuildProfilePdf(Valid());
    RayomdSource::Result result;
    RayomdSource::Read(pdf, result);
    RAYOPDF_CHECK(result.status == RayomdSource::Status::Ok, 1300);
    RAYOPDF_CHECK(result.source == Valid().source, 1301);
    // A view into the file, not a copy.
    RAYOPDF_CHECK(result.source.data() >= pdf.data() && result.source.data() + result.source.size() <= pdf.data() + pdf.size(),
                  1302);
    RAYOPDF_CHECK(result.info.profile == "rayomd-source/1" && result.info.producerVersion == "5.3.0" &&
                      result.info.sourceBytes == Valid().source.size(),
                  1303);

    ProfilePdf empty = Valid();
    empty.source.clear();
    RayomdSource::Read(BuildProfilePdf(empty), result);
    RAYOPDF_CHECK(result.status == RayomdSource::Status::Ok && result.source.empty(), 1304);

    // PDF 1.7 only as PDF/A-3 or PDF/UA-1.
    ProfilePdf old = Valid();
    old.header = "%PDF-1.7";
    RAYOPDF_CHECK(StatusOf(BuildProfilePdf(old)) == RayomdSource::Status::CorruptPdf, 1305);
    old.xmpExtra = " pdfaid:part=\"3\"";
    RAYOPDF_CHECK(StatusOf(BuildProfilePdf(old)) == RayomdSource::Status::Ok, 1306);
}

void TestDamaged() {
    const std::string pdf = BuildProfilePdf(Valid());
    // One byte of the payload changed: the digest no longer matches.
    std::string flipped = pdf;
    const size_t at = flipped.find("Some *text*");
    flipped[at] = 's';
    RAYOPDF_CHECK(StatusOf(flipped) == RayomdSource::Status::IntegrityMismatch, 1307);
    // A packet giving another digest.
    ProfilePdf digest = Valid();
    digest.digest = std::string(64, '0');
    RAYOPDF_CHECK(StatusOf(BuildProfilePdf(digest)) == RayomdSource::Status::IntegrityMismatch, 1308);
    // Truncated anywhere: no startxref and %%EOF at the end, or a cut object.
    for (size_t keep : {pdf.size() - 1 - 6, pdf.size() / 2, size_t(20), size_t(0)}) {
        RAYOPDF_CHECK(StatusOf(std::string_view(pdf).substr(0, keep)) == RayomdSource::Status::CorruptPdf, 1309);
    }
    // A length that is not the stream's.
    ProfilePdf length = Valid();
    length.declaredLength = static_cast<long long>(length.source.size()) + 1;
    RAYOPDF_CHECK(StatusOf(BuildProfilePdf(length)) == RayomdSource::Status::CorruptPdf, 1310);
    // A source with a filter (version 1 stores it unfiltered).
    ProfilePdf filtered = Valid();
    filtered.sourceExtra = " /Filter /FlateDecode";
    RAYOPDF_CHECK(StatusOf(BuildProfilePdf(filtered)) == RayomdSource::Status::CorruptPdf, 1311);
    // Incremental updates and encryption are outside the profile.
    ProfilePdf updated = Valid();
    updated.trailerExtra = " /Prev 9";
    RAYOPDF_CHECK(StatusOf(BuildProfilePdf(updated)) == RayomdSource::Status::CorruptPdf, 1312);
    updated.trailerExtra = " /Encrypt 3 0 R";
    RAYOPDF_CHECK(StatusOf(BuildProfilePdf(updated)) == RayomdSource::Status::CorruptPdf, 1313);
    // Valid digest over bytes that are not UTF-8.
    ProfilePdf latin = Valid();
    latin.source = "caf\xE9\n";
    RAYOPDF_CHECK(StatusOf(BuildProfilePdf(latin)) == RayomdSource::Status::InvalidUtf8, 1314);
    // A cross-reference table claiming far more entries than the file holds is refused before any
    // allocation for it.
    std::string huge = pdf;
    const size_t xref = huge.rfind("xref\n0 9\n");
    huge.replace(xref, 9, "xref\n0 999999\n");
    RAYOPDF_CHECK(StatusOf(huge) == RayomdSource::Status::CorruptPdf, 1315);
    // A second profile attribute: the packet is ambiguous.
    ProfilePdf twice = Valid();
    twice.xmpExtra = " rayomd:profile=\"rayomd-source/1\"";
    RAYOPDF_CHECK(StatusOf(BuildProfilePdf(twice)) == RayomdSource::Status::NotReversible, 1316);
}

void TestVersionsAndLimits() {
    ProfilePdf version = Valid();
    version.profile = "rayomd-source/2";
    RAYOPDF_CHECK(StatusOf(BuildProfilePdf(version)) == RayomdSource::Status::UnsupportedProfile, 1317);
    ProfilePdf encoding = Valid();
    encoding.encoding = "UTF-16";
    RAYOPDF_CHECK(StatusOf(BuildProfilePdf(encoding)) == RayomdSource::Status::UnsupportedProfile, 1318);
    // A declared source past 10 MiB is refused before its stream is read.
    ProfilePdf large = Valid();
    large.declaredLength = static_cast<long long>(RayomdSource::kMaxSourceBytes) + 1;
    RAYOPDF_CHECK(StatusOf(BuildProfilePdf(large)) == RayomdSource::Status::LimitExceeded, 1319);
    // A source of exactly 10 MiB is read.
    ProfilePdf limit = Valid();
    limit.source.assign(RayomdSource::kMaxSourceBytes, 'a');
    RAYOPDF_CHECK(StatusOf(BuildProfilePdf(limit)) == RayomdSource::Status::Ok, 1320);
    // A PDF past 256 MiB is refused unread.
    std::string oversized = BuildProfilePdf(Valid());
    oversized.resize(RayomdSource::kMaxPdfBytes + 1, ' ');
    RAYOPDF_CHECK(StatusOf(oversized) == RayomdSource::Status::LimitExceeded, 1321);
    // An ordinary PDF has no profile.
    const std::string plain = Test::OnePage("BT /F1 12 Tf 72 700 Td (Hello) Tj ET");
    RAYOPDF_CHECK(StatusOf(plain) != RayomdSource::Status::Ok, 1322);
}

void TestPrimitives() {
    char hex[64];
    RayomdSource::Sha256Hex("abc", hex);  // FIPS 180-4 example
    RAYOPDF_CHECK(std::memcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 64) == 0, 1323);
    RAYOPDF_CHECK(RayomdSource::IsValidUtf8("plain \xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80"), 1324);
    for (std::string_view bad : {std::string_view("\xC0\xAF"), std::string_view("\xED\xA0\x80"),
                                 std::string_view("\xF4\x90\x80\x80"), std::string_view("\xE2\x82"),
                                 std::string_view("\x80")}) {
        RAYOPDF_CHECK(!RayomdSource::IsValidUtf8(bad), 1325);
    }
}

} // namespace

int main() {
    TestValid();
    TestDamaged();
    TestVersionsAndLimits();
    TestPrimitives();
    std::puts("pdf exact tests passed");
    return 0;
}
