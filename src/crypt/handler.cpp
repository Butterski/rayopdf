// SPDX-License-Identifier: Apache-2.0
// The Standard security handler: file-key derivation and password checks of ISO 32000-1
// section 7.6.3 (Algorithms 2 to 7, revisions 2 to 4), Adobe extension level 3 (revision 5) and
// ISO 32000-2 section 7.6.4.3 (Algorithms 2.A and 2.B, revision 6), plus the per-object keys of
// Algorithm 1. See docs/research/algorithms.md section 1.4.
#include "common/utf8.h"
#include "crypt/aes.h"
#include "crypt/crypt.h"

#include <cstring>

namespace RayoPdf::Crypt {
namespace {

// ISO 32000-1 section 7.6.3.3, Algorithm 2 step a.
constexpr uint8_t kPadding[32] = {
    0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41, 0x64, 0x00, 0x4E, 0x56, 0xFF, 0xFA, 0x01, 0x08,
    0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80, 0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A,
};

constexpr size_t kMaxPasswordR6 = 127;  // ISO 32000-2 section 7.6.4.3.3: UTF-8, at most 127 bytes

const uint8_t* Bytes(std::string_view s) { return reinterpret_cast<const uint8_t*>(s.data()); }
std::string_view View(const uint8_t* p, size_t n) { return {reinterpret_cast<const char*>(p), n}; }

void PadPassword(std::string_view password, uint8_t out[32]) {
    const size_t n = password.size() < 32 ? password.size() : 32;
    if (n) std::memcpy(out, password.data(), n);
    std::memcpy(out + n, kPadding, 32 - n);
}

// Revisions 2 to 4 ------------------------------------------------------------------------------

struct LegacyParams {
    int r = 0;
    int n = 0;                // file key length in bytes, 5 to 16
    std::string_view o, u;    // first 32 bytes
    uint32_t p = 0;
    std::string_view id0;
    bool encryptMetadata = true;
};

// Algorithm 2: the file key from a padded user password.
void FileKeyR4(const LegacyParams& lp, const uint8_t padded[32], uint8_t key[16]) {
    std::string input;
    input.reserve(32 + 32 + 4 + lp.id0.size() + 4);
    input.append(View(padded, 32));
    input.append(lp.o);
    for (int i = 0; i < 4; ++i) input.push_back(static_cast<char>(lp.p >> (8 * i)));
    input.append(lp.id0);
    if (lp.r >= 4 && !lp.encryptMetadata) input.append(4, '\xff');
    uint8_t digest[16];
    Md5(input, digest);
    if (lp.r >= 3) {
        for (int i = 0; i < 50; ++i) Md5(View(digest, static_cast<size_t>(lp.n)), digest);
    }
    std::memcpy(key, digest, static_cast<size_t>(lp.n));
}

// Algorithms 4 and 5: the /U value the key would produce, compared with the stored one (all 32
// bytes for R 2, the first 16 for R 3-4, whose remaining bytes are arbitrary).
bool UserKeyMatches(const LegacyParams& lp, const uint8_t key[16]) {
    const auto n = static_cast<size_t>(lp.n);
    if (lp.r == 2) {
        uint8_t value[32];
        std::memcpy(value, kPadding, 32);
        Rc4(key, n, value, 32);
        return std::memcmp(value, lp.u.data(), 32) == 0;
    }
    std::string input(View(kPadding, 32));
    input.append(lp.id0);
    uint8_t value[16];
    Md5(input, value);
    uint8_t roundKey[16];
    for (int i = 0; i < 20; ++i) {
        for (size_t k = 0; k < n; ++k) roundKey[k] = static_cast<uint8_t>(key[k] ^ i);
        Rc4(roundKey, n, value, 16);
    }
    return std::memcmp(value, lp.u.data(), 16) == 0;
}

// Algorithm 6: true and the file key when `padded` is the user password.
bool AuthenticateUserR4(const LegacyParams& lp, const uint8_t padded[32], uint8_t key[16]) {
    FileKeyR4(lp, padded, key);
    return UserKeyMatches(lp, key);
}

// Algorithm 7: the owner password decrypts /O into the padded user password (Algorithm 3 steps
// a-d give the RC4 key), which Algorithm 6 then checks.
bool AuthenticateOwnerR4(const LegacyParams& lp, std::string_view password, uint8_t key[16]) {
    uint8_t padded[32];
    PadPassword(password, padded);
    uint8_t digest[16];
    Md5(View(padded, 32), digest);
    // Algorithm 3 step c hashes the whole previous digest, unlike Algorithm 2's first n bytes.
    if (lp.r >= 3) {
        for (int i = 0; i < 50; ++i) Md5(View(digest, 16), digest);
    }
    const auto n = static_cast<size_t>(lp.n);
    uint8_t user[32];
    std::memcpy(user, lp.o.data(), 32);
    if (lp.r == 2) {
        Rc4(digest, n, user, 32);
    } else {
        uint8_t roundKey[16];
        for (int i = 19; i >= 0; --i) {
            for (size_t k = 0; k < n; ++k) roundKey[k] = static_cast<uint8_t>(digest[k] ^ i);
            Rc4(roundKey, n, user, 32);
        }
    }
    return AuthenticateUserR4(lp, user, key);
}

// Revisions 5 and 6 -----------------------------------------------------------------------------

// R 5: SHA-256(password, salt, [U]). R 6: Algorithm 2.B (ISO 32000-2 section 7.6.4.3.4, with
// the errata's K1 = password + K + [U] built from the current K).
void HashR6(int r, std::string_view password, std::string_view salt, std::string_view u48, uint8_t out[32]) {
    std::string input;
    input.reserve(password.size() + 8 + 48);
    input.append(password).append(salt).append(u48);
    uint8_t k[64];
    Sha256(input, k);
    if (r == 5) {
        std::memcpy(out, k, 32);
        return;
    }
    size_t kBytes = 32;
    std::string k1, e;
    // 64 rounds, then until E's last byte <= round - 32; that byte is at most 255, so at most
    // 288 rounds run.
    for (int round = 1;; ++round) {
        const size_t unit = password.size() + kBytes + u48.size();
        k1.resize(64 * unit);
        for (size_t i = 0; i < 64; ++i) {
            char* at = &k1[i * unit];
            if (!password.empty()) std::memcpy(at, password.data(), password.size());
            std::memcpy(at + password.size(), k, kBytes);
            if (!u48.empty()) std::memcpy(at + password.size() + kBytes, u48.data(), u48.size());
        }
        AesCbcEncrypt(k, 16, k + 16, k1, e);  // 64 * unit is a multiple of 16
        unsigned sum = 0;
        for (int i = 0; i < 16; ++i) sum += static_cast<uint8_t>(e[static_cast<size_t>(i)]);
        // The first 16 bytes of E as a big-endian number mod 3 equal their sum mod 3 (256 = 1 mod 3).
        switch (sum % 3) {
        case 0: Sha256(e, k), kBytes = 32; break;
        case 1: Sha384(e, k), kBytes = 48; break;
        default: Sha512(e, k), kBytes = 64; break;
        }
        if (round >= 64 && static_cast<uint8_t>(e.back()) <= round - 32) break;
    }
    std::memcpy(out, k, 32);
}

struct ModernParams {
    int r = 0;
    std::string_view o, u;  // first 48 bytes: hash (32), validation salt (8), key salt (8)
    std::string_view oe, ue;  // first 32 bytes
};

// Algorithm 2.A: the file key when `password` (already truncated) is the user (`owner` false) or
// the owner password.
bool AuthenticateR6(const ModernParams& mp, std::string_view password, bool owner, uint8_t key[32]) {
    const std::string_view own = owner ? mp.o : mp.u;
    const std::string_view u48 = owner ? mp.u : std::string_view();
    uint8_t hash[32];
    HashR6(mp.r, password, own.substr(32, 8), u48, hash);
    if (std::memcmp(hash, own.data(), 32) != 0) return false;
    HashR6(mp.r, password, own.substr(40, 8), u48, hash);
    Detail::AesKey aes;
    Detail::AesExpandKey(hash, 32, aes);
    const uint8_t zeroIv[16] = {};
    Detail::AesCbcDecryptBlocks(aes, zeroIv, Bytes(owner ? mp.oe : mp.ue), 2, key);
    return true;
}

// The password as Latin-1 bytes when it is well-formed UTF-8 with non-ASCII code points that all
// fit in one byte; R 2-4 passwords are byte strings, a command line usually supplies UTF-8.
bool Utf8ToLatin1(std::string_view password, std::string& out) {
    out.clear();
    bool nonAscii = false;
    size_t at = 0;
    uint32_t cp = 0;
    size_t length = 0;
    while (Utf8::Decode(password, at, cp, length)) {
        if (cp > 0xFF) return false;  // includes U+FFFD, which malformed input decodes to
        if (cp >= 0x80) nonAscii = true;
        out.push_back(static_cast<char>(cp));
        at += length;
    }
    return nonAscii;
}

bool ValidMethod(Method m, bool v5) {
    if (m == Method::Identity) return true;
    return v5 ? m == Method::Aes256 : m != Method::Aes256;
}

} // namespace

Status SecurityHandler::Open(const EncryptParams& params, std::string_view password) {
    *this = SecurityHandler();
    if (params.filter != "Standard") return Status::Unsupported;
    const int v = params.v, r = params.r;
    const bool legacy = (v == 1 || v == 2 || v == 4) && r >= 2 && r <= 4;
    const bool modern = v == 5 && (r == 5 || r == 6);
    if (!legacy && !modern) return Status::Unsupported;

    CryptFilter stream, string;
    if (v == 4 || v == 5) {
        stream = params.stream;
        string = params.string;
        if (!ValidMethod(stream.method, v == 5) || !ValidMethod(string.method, v == 5)) return Status::Unsupported;
    }

    std::string latin1;
    const bool hasLatin1 = legacy && Utf8ToLatin1(password, latin1);
    // Candidates in order: the password as given, its Latin-1 form (R 2-4), then "".
    std::string_view candidates[3];
    int count = 0;
    candidates[count++] = password;
    if (hasLatin1) candidates[count++] = latin1;
    if (!password.empty()) candidates[count++] = std::string_view();

    if (legacy) {
        if (params.o.size() < 32 || params.u.size() < 32) return Status::Corrupt;
        LegacyParams lp;
        lp.r = r;
        lp.o = params.o.substr(0, 32);
        lp.u = params.u.substr(0, 32);
        lp.p = static_cast<uint32_t>(params.p);
        lp.id0 = params.id0;
        lp.encryptMetadata = params.encryptMetadata;
        if (r == 2 || v == 1) {
            lp.n = 5;
        } else if (v == 4 && (stream.method == Method::Aes128 || string.method == Method::Aes128)) {
            lp.n = 16;
        } else {
            // V 2 (and V 4 with /V2 filters): /Length in bits, a multiple of 8 from 40 to 128; a
            // V 4 crypt filter's own /Length takes precedence.
            int bits = params.lengthBits;
            if (v == 4) {
                const int filterBytes = stream.method == Method::Rc4 ? stream.lengthBytes : string.lengthBytes;
                if (filterBytes >= 5 && filterBytes <= 16) bits = 8 * filterBytes;
            }
            if (bits < 40 || bits > 128 || bits % 8 != 0) return Status::Corrupt;
            lp.n = bits / 8;
        }
        if (v != 4) {
            stream = CryptFilter{Method::Rc4, lp.n};
            string = stream;
        }
        uint8_t key[16];
        bool opened = false;
        for (int c = 0; c < count && !opened; ++c) {
            uint8_t padded[32];
            PadPassword(candidates[c], padded);
            if (AuthenticateUserR4(lp, padded, key)) {
                opened = true;
            } else if (AuthenticateOwnerR4(lp, candidates[c], key)) {
                opened = owner_ = true;
            }
        }
        if (!opened) return Status::PasswordRequired;
        std::memcpy(key_, key, static_cast<size_t>(lp.n));
        keyBytes_ = lp.n;
    } else {
        if (params.o.size() < 48 || params.u.size() < 48 || params.oe.size() < 32 || params.ue.size() < 32) {
            return Status::Corrupt;
        }
        ModernParams mp;
        mp.r = r;
        mp.o = params.o.substr(0, 48);
        mp.u = params.u.substr(0, 48);
        mp.oe = params.oe.substr(0, 32);
        mp.ue = params.ue.substr(0, 32);
        // SASLprep (RFC 4013) of the password is not applied: passwords outside its fixed points
        // (some non-ASCII forms) may fail to open.
        uint8_t key[32];
        bool opened = false;
        for (int c = 0; c < count && !opened; ++c) {
            const std::string_view truncated = candidates[c].substr(0, kMaxPasswordR6);
            if (AuthenticateR6(mp, truncated, false, key)) {
                opened = true;
            } else if (AuthenticateR6(mp, truncated, true, key)) {
                opened = owner_ = true;
            }
        }
        if (!opened) return Status::PasswordRequired;
        std::memcpy(key_, key, 32);
        keyBytes_ = 32;
        // /Perms (Algorithm 2.A step f): AES-256-ECB of P, 'T'/'F' for EncryptMetadata and "adb".
        // A mismatch is reported, not fatal: the key already proved itself against /U or /O.
        if (params.perms.size() >= 16) {
            uint8_t perms[16];
            AesEcbDecryptBlock(key_, 32, Bytes(params.perms), perms);
            const auto p = static_cast<uint32_t>(params.p);
            bool ok = perms[9] == 'a' && perms[10] == 'd' && perms[11] == 'b';
            for (int i = 0; i < 4; ++i) ok = ok && perms[i] == static_cast<uint8_t>(p >> (8 * i));
            ok = ok && perms[8] == (params.encryptMetadata ? 'T' : 'F');
            permsValid_ = ok;
        }
    }
    revision_ = r;
    encryptMetadata_ = params.encryptMetadata;
    stream_ = stream;
    string_ = string;
    return Status::Ok;
}

bool SecurityHandler::Decrypt(uint32_t num, uint16_t gen, bool stream, std::string_view in, std::string& out) const {
    return DecryptWith(stream ? stream_ : string_, num, gen, in, out);
}

bool SecurityHandler::DecryptWith(const CryptFilter& filter, uint32_t num, uint16_t gen, std::string_view in,
                                  std::string& out) const {
    if (filter.method == Method::Identity) {
        out.assign(in.data(), in.size());
        return true;
    }
    out.clear();
    if (keyBytes_ == 0) return false;  // not opened

    // Algorithm 1 (ISO 32000-1 section 7.6.2): MD5(file key, num low 3 bytes LE, gen 2 bytes LE,
    // ["sAlT"]) truncated to n + 5 bytes, at most 16. AES-256 uses the file key itself.
    uint8_t objectKey[16];
    const uint8_t* key = key_;
    size_t keyBytes = static_cast<size_t>(keyBytes_);
    if (filter.method != Method::Aes256) {
        uint8_t input[32 + 5 + 4];
        std::memcpy(input, key_, keyBytes);
        size_t n = keyBytes;
        input[n++] = static_cast<uint8_t>(num);
        input[n++] = static_cast<uint8_t>(num >> 8);
        input[n++] = static_cast<uint8_t>(num >> 16);
        input[n++] = static_cast<uint8_t>(gen);
        input[n++] = static_cast<uint8_t>(gen >> 8);
        if (filter.method == Method::Aes128) {
            std::memcpy(input + n, "sAlT", 4);
            n += 4;
        }
        Md5(View(input, n), objectKey);
        key = objectKey;
        // AES-128 needs all 16 bytes; only an AESV2 filter named in a document with a file key
        // under 11 bytes (no conforming writer makes one) would otherwise truncate it.
        keyBytes = filter.method == Method::Aes128 || keyBytes + 5 >= 16 ? 16 : keyBytes + 5;
    } else if (keyBytes_ != 32) {
        return false;  // an AESV3 filter in a document whose file key is not 256-bit
    }

    if (filter.method == Method::Rc4) {
        out.assign(in.data(), in.size());
        if (!out.empty()) Rc4(key, keyBytes, reinterpret_cast<uint8_t*>(&out[0]), out.size());
        return true;
    }

    // AES-CBC: the IV, then whole blocks ending in PKCS#7 padding (section 7.6.3.1, -2 7.6.4).
    if (in.size() < 16) return false;
    const size_t blocks = (in.size() - 16) / 16;
    bool ok = in.size() % 16 == 0;
    if (blocks == 0) return ok;  // the IV alone: an empty string
    if (!AesCbcDecrypt(key, keyBytes, in.substr(0, 16 + 16 * blocks), out)) return false;
    const auto pad = static_cast<uint8_t>(out.back());
    bool padded = pad >= 1 && pad <= 16;
    for (size_t i = 0; padded && i < pad; ++i) padded = static_cast<uint8_t>(out[out.size() - 1 - i]) == pad;
    if (padded) {
        out.resize(out.size() - pad);
    } else {
        ok = false;
    }
    return ok;
}

} // namespace RayoPdf::Crypt
