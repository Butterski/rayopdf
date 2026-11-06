// SPDX-License-Identifier: Apache-2.0
// The core tests' one assertion: a failed check prints its code and ends the test executable with
// a nonzero status, so the printed code alone names the check (crypt 100-199, pdf 200-299, text
// 300-399, bidi 600-639, CJK 640-699, layout 400-499 and 700-749, image 500-549, tables 550-599).
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
