// SPDX-License-Identifier: Apache-2.0
// Exact recovery (roadmap D5, step 1): a PDF RayoMD wrote with --embed-source carries its Markdown
// (pdf/rayomd_source.h); a conversion writes those bytes unchanged instead of converting the pages,
// once two checks pass.
//
// 1. The profile's own: the strict object path, the declared length, the SHA-256 and UTF-8. It
//    catches damage, not forgery: whoever writes a PDF can compute the digest.
// 2. The pages' (SourceMatchesPages): the payload must say what the pages show. Letters and digits
//    (bidi classes L, R, AL, EN, AN; private use excluded; ASCII case folded) of the payload and of
//    the normal conversion's Markdown (its `<!-- rayopdf: -->` comments left out) are compared in
//    windows of kExactWindow consecutive code points, a window read either way round (RayoMD draws
//    right-to-left text in visual order). A code point is matched when a window holding it occurs
//    on the other side. At most kExactSlack + 1/kExactShareDivisor of the payload's letters and
//    digits may be unmatched (text the pages do not show: link and image targets, TeX commands of
//    rendered formulas, comments), and at most as many of the pages'. More than 65,535 distinct
//    letters and digits between both sides, or windows that pile up past 64 probes in the hash
//    table (hostile text), fail the check.
//
// What passing guarantees: the Markdown written shows, in its letters and digits, the text of the
// pages in the same local order, and adds or drops no more than that slack of letters and digits.
// What it does not: punctuation, Markdown syntax, white space and link targets are not compared
// (the slack bounds the letters they hold), and the payload's author is not authenticated. A payload
// that fails either check is not used; the normal conversion runs and the report says why.
#pragma once

#include "layout/convert.h"
#include "pdf/document.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace RayoPdf::Layout {

constexpr size_t kExactWindow = 8;
constexpr size_t kExactSlack = 32;
constexpr size_t kExactShareDivisor = 5;

// The document's payload in `source` (a view into the document's bytes) with ExactUse::Used;
// ExactUse::None when the file claims none (no /AF in the catalog, or no rayomd-source namespace in
// its bytes); else the reason the claimed payload is rejected (Unsupported, Corrupt, Integrity,
// Limit, Utf8).
ExactUse FindExactSource(const Document& document, std::string_view& source);

// What SourceMatchesPages compared: letters and digits of each side and those left unmatched.
struct ExactMatch {
    size_t sourceContent = 0;
    size_t sourceUnmatched = 0;
    size_t pageContent = 0;
    size_t pageUnmatched = 0;
};

// Check 2 above: whether `source` says what `converted` (the normal conversion of the same pages)
// shows.
bool SourceMatchesPages(std::string_view source, std::string_view converted, ExactMatch& match);

} // namespace RayoPdf::Layout
