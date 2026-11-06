// SPDX-License-Identifier: Apache-2.0
// MD5 (RFC 1321) and SHA-256/384/512 (FIPS 180-4), one-shot. Words are assembled byte by byte,
// so the results do not depend on the host's byte order. Inputs are untrusted but the digests
// read every byte exactly once: no bounds depend on content.
#include "crypt/crypt.h"

#include <cstring>

namespace RayoPdf::Crypt {
namespace {

inline uint32_t Rotl32(uint32_t x, unsigned c) { return (x << c) | (x >> (32U - c)); }  // 0 < c < 32
inline uint32_t Rotr32(uint32_t x, unsigned c) { return (x >> c) | (x << (32U - c)); }  // 0 < c < 32
inline uint64_t Rotr64(uint64_t x, unsigned c) { return (x >> c) | (x << (64U - c)); }  // 0 < c < 64

inline uint32_t LoadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}
inline uint32_t LoadBe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
           static_cast<uint32_t>(p[3]);
}
inline uint64_t LoadBe64(const uint8_t* p) {
    return static_cast<uint64_t>(LoadBe32(p)) << 32 | LoadBe32(p + 4);
}
inline void StoreLe32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
inline void StoreBe32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}
inline void StoreBe64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (56 - 8 * i));
}

// Runs `compress` over every whole block of `data`, then over the padded tail: 0x80, zeros, and
// the message length in bits (`lengthBytes` wide, big-endian unless `littleEndianLength`).
template <size_t kBlock, size_t kLengthBytes, bool kLittleEndianLength, typename Compress>
void HashBlocks(std::string_view data, Compress compress) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
    const size_t size = data.size();
    const size_t whole = size / kBlock;
    for (size_t i = 0; i < whole; ++i) compress(bytes + i * kBlock);
    uint8_t tail[2 * kBlock] = {};
    const size_t rest = size - whole * kBlock;
    if (rest) std::memcpy(tail, bytes + whole * kBlock, rest);
    tail[rest] = 0x80;
    const size_t tailSize = rest + 1 + kLengthBytes <= kBlock ? kBlock : 2 * kBlock;
    // Bit length modulo 2^64; the high 64 bits of SHA-384/512's 128-bit length field stay zero
    // (no input here reaches 2^61 bytes).
    const uint64_t bits = static_cast<uint64_t>(size) << 3;
    for (size_t i = 0; i < 8; ++i) {
        const auto byte = static_cast<uint8_t>(bits >> (8 * i));
        if (kLittleEndianLength) {
            tail[tailSize - kLengthBytes + i] = byte;
        } else {
            tail[tailSize - 1 - i] = byte;
        }
    }
    compress(tail);
    if (tailSize == 2 * kBlock) compress(tail + kBlock);
}

constexpr uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
constexpr uint8_t kMd5Shift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21,
    6, 10, 15, 21, 6, 10, 15, 21,
};

constexpr uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr uint64_t kSha512K[80] = {
    0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc, 0x3956c25bf348b538,
    0x59f111f1b605d019, 0x923f82a4af194f9b, 0xab1c5ed5da6d8118, 0xd807aa98a3030242, 0x12835b0145706fbe,
    0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2, 0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235,
    0xc19bf174cf692694, 0xe49b69c19ef14ad2, 0xefbe4786384f25e3, 0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65,
    0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5, 0x983e5152ee66dfab,
    0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4, 0xc6e00bf33da88fc2, 0xd5a79147930aa725,
    0x06ca6351e003826f, 0x142929670a0e6e70, 0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed,
    0x53380d139d95b3df, 0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b,
    0xa2bfe8a14cf10364, 0xa81a664bbc423001, 0xc24b8b70d0f89791, 0xc76c51a30654be30, 0xd192e819d6ef5218,
    0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8, 0x19a4c116b8d2d0c8, 0x1e376c085141ab53,
    0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8, 0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb, 0x5b9cca4f7763e373,
    0x682e6ff3d6b2b8a3, 0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
    0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b, 0xca273eceea26619c,
    0xd186b8c721c0c207, 0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178, 0x06f067aa72176fba, 0x0a637dc5a2c898a6,
    0x113f9804bef90dae, 0x1b710b35131c471b, 0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc,
    0x431d67c49c100d4c, 0x4cc5d4becb3e42b6, 0x597f299cfc657e2a, 0x5fcb6fab3ad6faec, 0x6c44198c4a475817,
};

void Sha512Family(std::string_view data, const uint64_t initial[8], uint8_t* digest, size_t digestBytes) {
    uint64_t h[8];
    std::memcpy(h, initial, sizeof(h));
    HashBlocks<128, 16, false>(data, [&h](const uint8_t* block) {
        uint64_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = LoadBe64(block + 8 * i);
        for (int i = 16; i < 80; ++i) {
            const uint64_t s0 = Rotr64(w[i - 15], 1) ^ Rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
            const uint64_t s1 = Rotr64(w[i - 2], 19) ^ Rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
        for (int i = 0; i < 80; ++i) {
            const uint64_t t1 = k + (Rotr64(e, 14) ^ Rotr64(e, 18) ^ Rotr64(e, 41)) + ((e & f) ^ (~e & g)) +
                                kSha512K[i] + w[i];
            const uint64_t t2 = (Rotr64(a, 28) ^ Rotr64(a, 34) ^ Rotr64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
            k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
    });
    uint8_t full[64];
    for (int i = 0; i < 8; ++i) StoreBe64(full + 8 * i, h[i]);
    std::memcpy(digest, full, digestBytes);
}

} // namespace

void Md5(std::string_view data, uint8_t digest[16]) {
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    HashBlocks<64, 8, true>(data, [&h](const uint8_t* block) {
        uint32_t m[16];
        for (int i = 0; i < 16; ++i) m[i] = LoadLe32(block + 4 * i);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (unsigned i = 0; i < 64; ++i) {
            uint32_t f;
            unsigned g;
            if (i < 16) {
                f = (b & c) | (~b & d), g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c), g = (5 * i + 1) & 15;
            } else if (i < 48) {
                f = b ^ c ^ d, g = (3 * i + 5) & 15;
            } else {
                f = c ^ (b | ~d), g = (7 * i) & 15;
            }
            const uint32_t next = d;
            d = c, c = b;
            b = b + Rotl32(a + f + kMd5K[i] + m[g], kMd5Shift[i]);
            a = next;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d;
    });
    for (int i = 0; i < 4; ++i) StoreLe32(digest + 4 * i, h[i]);
}

void Sha256(std::string_view data, uint8_t digest[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    HashBlocks<64, 8, false>(data, [&h](const uint8_t* block) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = LoadBe32(block + 4 * i);
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = Rotr32(w[i - 15], 7) ^ Rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Rotr32(w[i - 2], 17) ^ Rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = k + (Rotr32(e, 6) ^ Rotr32(e, 11) ^ Rotr32(e, 25)) + ((e & f) ^ (~e & g)) +
                                kSha256K[i] + w[i];
            const uint32_t t2 = (Rotr32(a, 2) ^ Rotr32(a, 13) ^ Rotr32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
    });
    for (int i = 0; i < 8; ++i) StoreBe32(digest + 4 * i, h[i]);
}

void Sha384(std::string_view data, uint8_t digest[48]) {
    static constexpr uint64_t kInitial[8] = {
        0xcbbb9d5dc1059ed8, 0x629a292a367cd507, 0x9159015a3070dd17, 0x152fecd8f70e5939,
        0x67332667ffc00b31, 0x8eb44a8768581511, 0xdb0c2e0d64f98fa7, 0x47b5481dbefa4fa4,
    };
    Sha512Family(data, kInitial, digest, 48);
}

void Sha512(std::string_view data, uint8_t digest[64]) {
    static constexpr uint64_t kInitial[8] = {
        0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
        0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179,
    };
    Sha512Family(data, kInitial, digest, 64);
}

} // namespace RayoPdf::Crypt
