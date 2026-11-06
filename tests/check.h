// SPDX-License-Identifier: Apache-2.0
// The core tests' one assertion: a failed check prints itself and ends the test executable with
// its own exit code, so the code alone names the check (crypt 100-199, pdf 200-299, text 300-399,
// layout 400-499).
#pragma once

#include <cstdio>
#include <cstdlib>

#define RAYOPDF_CHECK(condition, code)                                                              \
    do {                                                                                            \
        if (!(condition)) {                                                                         \
            std::fprintf(stderr, "check %d failed: %s (%s:%d)\n", (code), #condition, __FILE__,     \
                         __LINE__);                                                                 \
            std::exit(code);                                                                        \
        }                                                                                           \
    } while (0)
