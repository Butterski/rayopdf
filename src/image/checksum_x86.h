// SPDX-License-Identifier: Apache-2.0
// Internal to the image module: the PNG writer's checksums on x86-64 extensions, ported from
// zlib-ng (checksum_x86.cpp). Png::Crc32 and Deflate::Adler32 choose them through CpuFeatures()
// (common/cpu.h); their own code is the reference the tests compare these with.
#pragma once

#include <cstddef>
#include <cstdint>

#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)
#define RAYOPDF_CHECKSUM_X86 1
#endif

#if RAYOPDF_CHECKSUM_X86
namespace RayoPdf::Image::Checksum {

// The CRC-32 of PNG chunks (Png::Crc32's convention: start with 0, pass the result on) over
// `size` >= 16 bytes from a 16-byte aligned `data`, folding 64 bytes a step with carry-less
// multiplies. Needs kCpuPclmul and kCpuSse42.
uint32_t Crc32Fold(const uint8_t* data, size_t size, uint32_t crc);

// Adler-32 (Deflate::Adler32's convention) over `size` bytes, a multiple of 32, through 256-bit
// lanes. Needs kCpuAvx2.
uint32_t Adler32Avx2(const uint8_t* data, size_t size, uint32_t adler);

} // namespace RayoPdf::Image::Checksum
#endif
