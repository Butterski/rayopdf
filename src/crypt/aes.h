// SPDX-License-Identifier: Apache-2.0
// Internal AES (FIPS 197) block interface of the crypt module: an expanded key holds both the
// encryption and the equivalent-inverse-cipher decryption schedules (FIPS 197 section 5.3.5).
#pragma once

#include <cstddef>
#include <cstdint>

namespace RayoPdf::Crypt::Detail {

struct AesKey {
    uint32_t enc[60];
    uint32_t dec[60];
    int rounds = 0;  // 10, 12 or 14
};

// False unless `keyBytes` is 16, 24 or 32.
bool AesExpandKey(const uint8_t* key, size_t keyBytes, AesKey& out);
void AesEncryptBlock(const AesKey& key, const uint8_t in[16], uint8_t out[16]);
void AesDecryptBlock(const AesKey& key, const uint8_t in[16], uint8_t out[16]);
// CBC over `blocks` whole blocks. `in` and `out` may be the same buffer.
void AesCbcDecryptBlocks(const AesKey& key, const uint8_t iv[16], const uint8_t* in, size_t blocks, uint8_t* out);
void AesCbcEncryptBlocks(const AesKey& key, const uint8_t iv[16], const uint8_t* in, size_t blocks, uint8_t* out);

} // namespace RayoPdf::Crypt::Detail
