// SPDX-License-Identifier: Apache-2.0
// AES (FIPS 197) with 32-bit T-tables computed at compile time, and RC4. Blocks are loaded and
// stored byte by byte as big-endian words, so the code is independent of the host's byte order.
// Not constant-time: it decrypts documents, not secrets held at rest.
#include "crypt/aes.h"
#include "crypt/crypt.h"

#include <cstring>

namespace RayoPdf::Crypt {
namespace Detail {
namespace {

struct AesTables {
    uint8_t sbox[256];
    uint8_t inverse[256];
    uint32_t te[4][256];  // encryption round: SubBytes + MixColumns of one byte, rotated per row
    uint32_t td[4][256];  // decryption round: InvSubBytes + InvMixColumns of one byte
};

constexpr uint8_t XTime(uint8_t x) { return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

constexpr uint8_t GfMul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r = static_cast<uint8_t>(r ^ a);
        a = XTime(a);
        b = static_cast<uint8_t>(b >> 1);
    }
    return r;
}

constexpr uint8_t Rotl8(uint8_t x, int s) { return static_cast<uint8_t>((x << s) | (x >> (8 - s))); }
constexpr uint32_t Ror32(uint32_t x, int s) { return s ? (x >> s) | (x << (32 - s)) : x; }

constexpr AesTables MakeAesTables() {
    AesTables t{};
    // The S-box from the multiplicative inverse: p walks the powers of the generator 3 and q
    // its inverse, then the affine map (FIPS 197 section 5.1.1).
    uint8_t p = 1, q = 1;
    do {
        p = static_cast<uint8_t>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));
        q = static_cast<uint8_t>(q ^ (q << 1));
        q = static_cast<uint8_t>(q ^ (q << 2));
        q = static_cast<uint8_t>(q ^ (q << 4));
        if (q & 0x80) q = static_cast<uint8_t>(q ^ 0x09);
        const uint8_t x = static_cast<uint8_t>(q ^ Rotl8(q, 1) ^ Rotl8(q, 2) ^ Rotl8(q, 3) ^ Rotl8(q, 4));
        t.sbox[p] = static_cast<uint8_t>(x ^ 0x63);
    } while (p != 1);
    t.sbox[0] = 0x63;
    for (int i = 0; i < 256; ++i) t.inverse[t.sbox[i]] = static_cast<uint8_t>(i);
    for (int i = 0; i < 256; ++i) {
        const uint8_t s = t.sbox[i];
        const uint32_t e = static_cast<uint32_t>(GfMul(s, 2)) << 24 | static_cast<uint32_t>(s) << 16 |
                           static_cast<uint32_t>(s) << 8 | GfMul(s, 3);
        const uint8_t v = t.inverse[i];
        const uint32_t d = static_cast<uint32_t>(GfMul(v, 14)) << 24 | static_cast<uint32_t>(GfMul(v, 9)) << 16 |
                           static_cast<uint32_t>(GfMul(v, 13)) << 8 | GfMul(v, 11);
        for (int r = 0; r < 4; ++r) {
            t.te[r][i] = Ror32(e, 8 * r);
            t.td[r][i] = Ror32(d, 8 * r);
        }
    }
    return t;
}

constexpr AesTables kAes = MakeAesTables();

inline uint32_t LoadBe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
           static_cast<uint32_t>(p[3]);
}
inline void StoreBe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

inline uint32_t SubWord(uint32_t w) {
    return static_cast<uint32_t>(kAes.sbox[w >> 24]) << 24 | static_cast<uint32_t>(kAes.sbox[(w >> 16) & 0xff]) << 16 |
           static_cast<uint32_t>(kAes.sbox[(w >> 8) & 0xff]) << 8 | kAes.sbox[w & 0xff];
}

// InvMixColumns of one round-key word: Td maps InvSubBytes first, so feed it SubBytes.
inline uint32_t InvMixColumn(uint32_t w) {
    return kAes.td[0][kAes.sbox[w >> 24]] ^ kAes.td[1][kAes.sbox[(w >> 16) & 0xff]] ^
           kAes.td[2][kAes.sbox[(w >> 8) & 0xff]] ^ kAes.td[3][kAes.sbox[w & 0xff]];
}

inline uint32_t Byte(uint32_t w, int shift) { return (w >> shift) & 0xff; }

} // namespace

bool AesExpandKey(const uint8_t* key, size_t keyBytes, AesKey& out) {
    if (keyBytes != 16 && keyBytes != 24 && keyBytes != 32) return false;
    const int nk = static_cast<int>(keyBytes / 4);
    out.rounds = nk + 6;
    const int total = 4 * (out.rounds + 1);
    uint32_t* w = out.enc;
    for (int i = 0; i < nk; ++i) w[i] = LoadBe32(key + 4 * i);
    uint8_t rcon = 1;
    for (int i = nk; i < total; ++i) {
        uint32_t t = w[i - 1];
        if (i % nk == 0) {
            t = SubWord((t << 8) | (t >> 24)) ^ (static_cast<uint32_t>(rcon) << 24);
            rcon = XTime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            t = SubWord(t);
        }
        w[i] = w[i - nk] ^ t;
    }
    // Equivalent inverse cipher: round keys in reverse order, inner ones through InvMixColumns.
    for (int r = 0; r <= out.rounds; ++r) {
        for (int j = 0; j < 4; ++j) {
            const uint32_t k = w[4 * (out.rounds - r) + j];
            out.dec[4 * r + j] = (r == 0 || r == out.rounds) ? k : InvMixColumn(k);
        }
    }
    return true;
}

void AesEncryptBlock(const AesKey& key, const uint8_t in[16], uint8_t out[16]) {
    const uint32_t* rk = key.enc;
    uint32_t s0 = LoadBe32(in) ^ rk[0], s1 = LoadBe32(in + 4) ^ rk[1];
    uint32_t s2 = LoadBe32(in + 8) ^ rk[2], s3 = LoadBe32(in + 12) ^ rk[3];
    const auto& te = kAes.te;
    for (int r = 1; r < key.rounds; ++r) {
        rk += 4;
        const uint32_t t0 = te[0][s0 >> 24] ^ te[1][Byte(s1, 16)] ^ te[2][Byte(s2, 8)] ^ te[3][s3 & 0xff] ^ rk[0];
        const uint32_t t1 = te[0][s1 >> 24] ^ te[1][Byte(s2, 16)] ^ te[2][Byte(s3, 8)] ^ te[3][s0 & 0xff] ^ rk[1];
        const uint32_t t2 = te[0][s2 >> 24] ^ te[1][Byte(s3, 16)] ^ te[2][Byte(s0, 8)] ^ te[3][s1 & 0xff] ^ rk[2];
        const uint32_t t3 = te[0][s3 >> 24] ^ te[1][Byte(s0, 16)] ^ te[2][Byte(s1, 8)] ^ te[3][s2 & 0xff] ^ rk[3];
        s0 = t0, s1 = t1, s2 = t2, s3 = t3;
    }
    rk += 4;
    const uint8_t* sb = kAes.sbox;
    const auto last = [sb](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        return static_cast<uint32_t>(sb[a >> 24]) << 24 | static_cast<uint32_t>(sb[Byte(b, 16)]) << 16 |
               static_cast<uint32_t>(sb[Byte(c, 8)]) << 8 | sb[d & 0xff];
    };
    StoreBe32(out, last(s0, s1, s2, s3) ^ rk[0]);
    StoreBe32(out + 4, last(s1, s2, s3, s0) ^ rk[1]);
    StoreBe32(out + 8, last(s2, s3, s0, s1) ^ rk[2]);
    StoreBe32(out + 12, last(s3, s0, s1, s2) ^ rk[3]);
}

void AesDecryptBlock(const AesKey& key, const uint8_t in[16], uint8_t out[16]) {
    const uint32_t* rk = key.dec;
    uint32_t s0 = LoadBe32(in) ^ rk[0], s1 = LoadBe32(in + 4) ^ rk[1];
    uint32_t s2 = LoadBe32(in + 8) ^ rk[2], s3 = LoadBe32(in + 12) ^ rk[3];
    const auto& td = kAes.td;
    for (int r = 1; r < key.rounds; ++r) {
        rk += 4;
        const uint32_t t0 = td[0][s0 >> 24] ^ td[1][Byte(s3, 16)] ^ td[2][Byte(s2, 8)] ^ td[3][s1 & 0xff] ^ rk[0];
        const uint32_t t1 = td[0][s1 >> 24] ^ td[1][Byte(s0, 16)] ^ td[2][Byte(s3, 8)] ^ td[3][s2 & 0xff] ^ rk[1];
        const uint32_t t2 = td[0][s2 >> 24] ^ td[1][Byte(s1, 16)] ^ td[2][Byte(s0, 8)] ^ td[3][s3 & 0xff] ^ rk[2];
        const uint32_t t3 = td[0][s3 >> 24] ^ td[1][Byte(s2, 16)] ^ td[2][Byte(s1, 8)] ^ td[3][s0 & 0xff] ^ rk[3];
        s0 = t0, s1 = t1, s2 = t2, s3 = t3;
    }
    rk += 4;
    const uint8_t* inv = kAes.inverse;
    const auto last = [inv](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        return static_cast<uint32_t>(inv[a >> 24]) << 24 | static_cast<uint32_t>(inv[Byte(b, 16)]) << 16 |
               static_cast<uint32_t>(inv[Byte(c, 8)]) << 8 | inv[d & 0xff];
    };
    StoreBe32(out, last(s0, s3, s2, s1) ^ rk[0]);
    StoreBe32(out + 4, last(s1, s0, s3, s2) ^ rk[1]);
    StoreBe32(out + 8, last(s2, s1, s0, s3) ^ rk[2]);
    StoreBe32(out + 12, last(s3, s2, s1, s0) ^ rk[3]);
}

void AesCbcDecryptBlocks(const AesKey& key, const uint8_t iv[16], const uint8_t* in, size_t blocks, uint8_t* out) {
    uint8_t chain[16], cipher[16], plain[16];
    std::memcpy(chain, iv, 16);
    for (size_t b = 0; b < blocks; ++b) {
        std::memcpy(cipher, in + 16 * b, 16);  // kept before `out` may overwrite it in place
        AesDecryptBlock(key, cipher, plain);
        for (int i = 0; i < 16; ++i) out[16 * b + i] = static_cast<uint8_t>(plain[i] ^ chain[i]);
        std::memcpy(chain, cipher, 16);
    }
}

void AesCbcEncryptBlocks(const AesKey& key, const uint8_t iv[16], const uint8_t* in, size_t blocks, uint8_t* out) {
    uint8_t chain[16];
    std::memcpy(chain, iv, 16);
    for (size_t b = 0; b < blocks; ++b) {
        uint8_t block[16];
        for (int i = 0; i < 16; ++i) block[i] = static_cast<uint8_t>(in[16 * b + i] ^ chain[i]);
        AesEncryptBlock(key, block, chain);
        std::memcpy(out + 16 * b, chain, 16);
    }
}

} // namespace Detail

void Rc4(const uint8_t* key, size_t keyBytes, uint8_t* data, size_t size) {
    if (keyBytes == 0 || keyBytes > 256) return;
    uint8_t s[256];
    for (int i = 0; i < 256; ++i) s[i] = static_cast<uint8_t>(i);
    uint8_t j = 0;
    for (size_t i = 0; i < 256; ++i) {
        j = static_cast<uint8_t>(j + s[i] + key[i % keyBytes]);
        const uint8_t t = s[i];
        s[i] = s[j];
        s[j] = t;
    }
    uint8_t x = 0, y = 0;
    for (size_t n = 0; n < size; ++n) {
        x = static_cast<uint8_t>(x + 1);
        const uint8_t sx = s[x];
        y = static_cast<uint8_t>(y + sx);
        const uint8_t sy = s[y];
        s[x] = sy;
        s[y] = sx;
        data[n] ^= s[static_cast<uint8_t>(sx + sy)];
    }
}

bool AesCbcDecrypt(const uint8_t* key, size_t keyBytes, std::string_view data, std::string& out) {
    out.clear();
    if ((keyBytes != 16 && keyBytes != 32) || data.size() < 16 || data.size() % 16 != 0) return false;
    Detail::AesKey expanded;
    Detail::AesExpandKey(key, keyBytes, expanded);
    const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
    const size_t blocks = data.size() / 16 - 1;
    out.resize(blocks * 16);
    if (blocks) Detail::AesCbcDecryptBlocks(expanded, bytes, bytes + 16, blocks, reinterpret_cast<uint8_t*>(&out[0]));
    return true;
}

bool AesCbcEncrypt(const uint8_t* key, size_t keyBytes, const uint8_t iv[16], std::string_view data,
                   std::string& out) {
    out.clear();
    if ((keyBytes != 16 && keyBytes != 32) || data.size() % 16 != 0) return false;
    Detail::AesKey expanded;
    Detail::AesExpandKey(key, keyBytes, expanded);
    out.resize(data.size());
    if (!data.empty()) {
        Detail::AesCbcEncryptBlocks(expanded, iv, reinterpret_cast<const uint8_t*>(data.data()), data.size() / 16,
                                    reinterpret_cast<uint8_t*>(&out[0]));
    }
    return true;
}

bool AesEcbDecryptBlock(const uint8_t* key, size_t keyBytes, const uint8_t in[16], uint8_t out[16]) {
    if (keyBytes != 16 && keyBytes != 32) return false;
    Detail::AesKey expanded;
    Detail::AesExpandKey(key, keyBytes, expanded);
    Detail::AesDecryptBlock(expanded, in, out);
    return true;
}

} // namespace RayoPdf::Crypt
