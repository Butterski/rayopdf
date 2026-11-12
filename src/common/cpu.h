// SPDX-License-Identifier: Apache-2.0
// The one place a kernel is chosen at run time (roadmap D1): the instruction-set extensions of the
// CPU the program runs on, beyond the x86-64 baseline (SSE2) every build targets. A kernel compiled
// for an extension (`__attribute__((target(...)))`) runs where CpuFeatures() names it; its baseline
// path is the reference the tests check it against.
#pragma once

#include <cstdint>

namespace RayoPdf {

enum : uint32_t {
    kCpuSse42 = 1,
    kCpuAvx2 = 2,
    kCpuBmi2 = 4,
    kCpuPclmul = 8,
};

// The extensions of this CPU (read once, from __builtin_cpu_supports; none on other compilers or
// architectures), or none while ForceCpuBaseline is on.
uint32_t CpuFeatures();

// For tests: true makes CpuFeatures() report none, so every dispatched kernel takes its baseline
// path. Not thread-safe; false by default.
void ForceCpuBaseline(bool baseline);

} // namespace RayoPdf
