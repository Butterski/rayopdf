// SPDX-License-Identifier: Apache-2.0
#include "common/cpu.h"

namespace RayoPdf {
namespace {

bool gBaseline = false;  // ForceCpuBaseline

uint32_t Detect() {
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
    __builtin_cpu_init();
    uint32_t features = 0;
    if (__builtin_cpu_supports("sse4.2")) features |= kCpuSse42;
    if (__builtin_cpu_supports("avx2")) features |= kCpuAvx2;
    if (__builtin_cpu_supports("bmi2")) features |= kCpuBmi2;
    if (__builtin_cpu_supports("pclmul")) features |= kCpuPclmul;
    return features;
#else
    return 0;
#endif
}

} // namespace

uint32_t CpuFeatures() {
    static const uint32_t kFeatures = Detect();
    return gBaseline ? 0 : kFeatures;
}

void ForceCpuBaseline(bool baseline) { gBaseline = baseline; }

} // namespace RayoPdf
