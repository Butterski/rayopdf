// SPDX-License-Identifier: Apache-2.0
// The core tests' one assertion: a failed check prints its code and ends the test executable with
// a nonzero status, so the printed code alone names the check (crypt 100-199, pdf 200-299, text
// 300-399, bidi 600-639 and 800-819, text repair 820-849, CJK 640-699, layout 400-499, 700-749
// and 1900-1949, layout order 750-799 and 1850-1899, image 500-549 and 1000-1049, CCITT
// 1050-1099, JBIG2 1100-1149, JPEG 2000 1150-1199, tables 550-599, 850-899 and 1700-1749, tagged
// 900-949, common 950-999 and 1680-1699, CPU dispatch 1650-1679, OCR routing 1200-1249, OCR add-on
// 1250-1299 and 1500-1549, exact recovery 1300-1349, glyph-shape recovery 1350-1399, OCR text
// structure 1400-1449, text fonts 1450-1479, LaTeX math 1550-1599, pages on several threads
// 1600-1649, OCR image tables 1750-1799, headings 1800-1849, C++ API and C ABI 1950-1999, output
// formats 2000-2049, robust statistics 2100-2149, statistical layout 2150-2199, LaTeX math
// structure 2200-2249, small models 2250-2299, tables 2300-2349).
// An exit status has 8 bits, so codes past 255 leave the process as code % 256, and as 1 when that
// would be 0.
#pragma once

#include <cstdio>
#include <cstdlib>

#define RAYOPDF_CHECK(condition, code)                                                              \
    do {                                                                                            \
        if (!(condition)) {                                                                         \
            std::fprintf(stderr, "check %d failed: %s (%s:%d)\n", (code), #condition, __FILE__,     \
                         __LINE__);                                                                 \
            std::exit((code) % 256 == 0 ? 1 : (code) % 256);                                        \
        }                                                                                           \
    } while (0)
