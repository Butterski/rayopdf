// SPDX-License-Identifier: Apache-2.0
// The Standard security handler (ISO 32000-1 section 7.6.3, ISO 32000-2 section 7.6.4):
// revisions 2 to 6 with RC4 (40 to 128-bit), AES-128 (AESV2) and AES-256 (AESV3), from the
// parsed /Encrypt dictionary to per-object decryption. Permissions are read but not enforced: a
// converter only reads text. Own MD5, SHA-256/384/512, RC4 and AES (roadmap D2); no dependency.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace RayoPdf::Crypt {

enum class Status : uint8_t {
    Ok,
    PasswordRequired,  // neither the given password nor "" opens it as user or owner
    Unsupported,       // another /Filter (public-key handlers), or a V/R/method not listed above
    Corrupt,           // the dictionary is incomplete or inconsistent (missing /O or /U, bad lengths)
};

enum class Method : uint8_t {
    Identity,  // no encryption (crypt filter /Identity, or /CFM /None)
    Rc4,       // /V2 or V 1-2
    Aes128,    // /AESV2
    Aes256,    // /AESV3
};

// A crypt filter as the document names it: /StmF, /StrF and /EFF entries look up /CF.
struct CryptFilter {
    Method method = Method::Identity;
    int lengthBytes = 0;  // key length in bytes, 5 to 32; 0 when the dictionary gives none
};

// The /Encrypt dictionary's entries, resolved by the document reader. Byte strings hold the
// string's bytes exactly as stored (they are never encrypted themselves).
struct EncryptParams {
    std::string_view filter;      // /Filter, "Standard" for this handler
    int v = 0;                    // /V
    int r = 0;                    // /R
    int lengthBits = 40;          // /Length (V 2-3), default 40
    std::string_view o, u;        // /O, /U
    std::string_view oe, ue;      // /OE, /UE (R 5-6)
    std::string_view perms;       // /Perms (R 5-6)
    int32_t p = 0;                // /P as a signed 32-bit integer
    bool encryptMetadata = true;  // /EncryptMetadata
    // V 4-5: the crypt filters /StmF and /StrF name in /CF ("Identity" when the name is
    // /Identity or absent). For V 1-3 the reader leaves them as they are; Open derives RC4.
    CryptFilter stream;
    CryptFilter string;
    std::string_view id0;         // the first string of the trailer's /ID, "" when none
};

class SecurityHandler {
public:
    // Derives the file key: `password` (UTF-8 for R 5-6, Latin-1 bytes for R 2-4) is tried as the
    // user password, then as the owner password; an empty password is tried the same way.
    Status Open(const EncryptParams& params, std::string_view password);

    // Decrypts the bytes of a string (`stream` false) or a stream (`stream` true) of object
    // `num` generation `gen` into `out` (replaced). AES data shorter than one block, of a length
    // that is no multiple of 16, or with bad padding still yields what decrypts (padding left as
    // is) and returns false; the caller counts it.
    bool Decrypt(uint32_t num, uint16_t gen, bool stream, std::string_view in, std::string& out) const;

    // Decrypts with the method of an explicit crypt filter (a stream's /Crypt filter naming an
    // entry of /CF other than /StmF's).
    bool DecryptWith(const CryptFilter& filter, uint32_t num, uint16_t gen, std::string_view in,
                     std::string& out) const;

    bool EncryptMetadata() const { return encryptMetadata_; }
    Method StreamMethod() const { return stream_.method; }
    Method StringMethod() const { return string_.method; }
    int Revision() const { return revision_; }
    bool OpenedAsOwner() const { return owner_; }
    // R 5-6: /Perms decrypted with the file key matches /P, /EncryptMetadata and "adb". False
    // for R 2-4, a missing /Perms or a mismatch (which ISO 32000-2 does not make fatal).
    bool PermsValid() const { return permsValid_; }

private:
    uint8_t key_[32] = {};
    int keyBytes_ = 0;
    int revision_ = 0;
    bool owner_ = false;
    bool permsValid_ = false;
    bool encryptMetadata_ = true;
    CryptFilter stream_;
    CryptFilter string_;
};

// Primitives, exposed for the tests (FIPS 180-4, RFC 1321 and FIPS 197 vectors).
void Md5(std::string_view data, uint8_t digest[16]);
void Sha256(std::string_view data, uint8_t digest[32]);
void Sha384(std::string_view data, uint8_t digest[48]);
void Sha512(std::string_view data, uint8_t digest[64]);
// RC4 of `data` with `key` (1 to 256 bytes), in place.
void Rc4(const uint8_t* key, size_t keyBytes, uint8_t* data, size_t size);
// AES-CBC decryption with a 16 or 32-byte key: `data` holds the IV (first 16 bytes) then whole
// blocks; the plaintext (padding included) goes to `out`. False for a bad length or key size.
bool AesCbcDecrypt(const uint8_t* key, size_t keyBytes, std::string_view data, std::string& out);
// AES-CBC encryption without padding (whole blocks), for the R 6 key derivation (Algorithm 2.B)
// and the tests. False for a bad length or key size.
bool AesCbcEncrypt(const uint8_t* key, size_t keyBytes, const uint8_t iv[16], std::string_view data,
                   std::string& out);
// AES-ECB decryption of one block (R 5-6 /Perms check).
bool AesEcbDecryptBlock(const uint8_t* key, size_t keyBytes, const uint8_t in[16], uint8_t out[16]);

} // namespace RayoPdf::Crypt
