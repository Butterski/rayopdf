// SPDX-License-Identifier: Apache-2.0
// UTF-8 text of single code points with process lifetime, for lookups that return string views
// without owning a buffer (font codes mapped through a CMap or a character collection).
#pragma once

#include <cstdint>
#include <string_view>

namespace RayoPdf::Text {

// The UTF-8 of BMP code point `cp` (a table of all 65,536, built once on first use); empty for
// surrogates and code points past U+FFFF.
std::string_view BmpText(uint32_t cp);

} // namespace RayoPdf::Text
