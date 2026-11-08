// SPDX-License-Identifier: Apache-2.0
// Forked from rayomd src/core/rayomd_pdf_source.h at ef106ca (Apache-2.0, same author).
// The reader of RayoMD's `rayomd-source/1` profile (RayoMD docs/development/reversible_pdf_profile.md):
// a PDF that `rayomd --export ... --embed-source` wrote carries its exact UTF-8 Markdown as an
// unfiltered /EmbeddedFile `source.md` (/AFRelationship /Source, referenced from the catalog's
// /AF and /Names /EmbeddedFiles) and an XMP /Metadata packet giving the profile, the encoding, the
// exact length and the SHA-256 of those bytes.
//
// The parser is RayoMD's own and as narrow as the profile: one classic cross-reference section,
// generation 0, objects in increasing offset order, no /Prev, /XRefStm, /Encrypt, xref or object
// streams, no stream filter on the source or the metadata. It does not repair: a file another
// program rewrote is rejected. Every loop is bounded (tokens 4,096 a dictionary, nesting 16,
// names 1 KiB, strings 64 KiB) and every limit fails closed: the PDF at most kMaxPdfBytes, the
// source at most kMaxSourceBytes, the XMP packet 16 KiB, the table 1,000,001 entries.
//
// What passing guarantees: the bytes returned are the ones the packet describes (length, SHA-256)
// and valid UTF-8. The digest is no signature: whoever writes a PDF can compute it, so it detects
// damage, not forgery. Whether the source matches what the pages show is layout/exact.h's check.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace RayoPdf::RayomdSource {

constexpr size_t kMaxPdfBytes = size_t(256) << 20;
constexpr size_t kMaxSourceBytes = size_t(10) << 20;

enum class Status : uint8_t {
    Ok,
    NotReversible,       // no profile packet: the catalog has no /Metadata stream naming the profile
    UnsupportedProfile,  // another profile version, or an encoding other than UTF-8
    CorruptPdf,          // the file or the profile's objects are not as the profile writes them
    IntegrityMismatch,   // the source's SHA-256 is not the packet's
    LimitExceeded,       // the PDF or the declared source past its limit
    InvalidUtf8,         // the source passes the digest but is not UTF-8
};

// The packet's attributes, views into the PDF's bytes.
struct Info {
    std::string_view profile;
    std::string_view producerVersion;
    size_t sourceBytes = 0;
};

struct Result {
    Status status = Status::CorruptPdf;
    Info info;
    std::string_view source;  // Ok: the Markdown, a view into the PDF's bytes
};

// Reads the profile from `pdf`, a whole file. `result.source` is set only for Status::Ok.
void Read(std::string_view pdf, Result& result);

// Whether `text` is well-formed UTF-8 (no overlong forms, surrogates or code points past U+10FFFF).
bool IsValidUtf8(std::string_view text);

// The SHA-256 of `data` as 64 lowercase hex digits, into `out` (64 bytes, no terminator).
void Sha256Hex(std::string_view data, char out[64]);

} // namespace RayoPdf::RayomdSource
