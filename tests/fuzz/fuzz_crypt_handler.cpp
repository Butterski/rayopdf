// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of the Standard security handler: an arbitrary /Encrypt dictionary and
// password through SecurityHandler::Open, then the rest of the input through Decrypt as a
// string and as a stream. Layout: 4 header bytes (V, R, flags, /Length in bytes), then /O, /U,
// /OE, /UE, /Perms, /ID[0] and the password, each prefixed with a 1-byte length, then the data.
#include "crypt/crypt.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

using namespace RayoPdf::Crypt;

namespace {

bool Take(std::string_view& input, std::string_view& field) {
    if (input.empty()) return false;
    const size_t n = static_cast<uint8_t>(input[0]);
    input.remove_prefix(1);
    if (n > input.size()) return false;
    field = input.substr(0, n);
    input.remove_prefix(n);
    return true;
}

Method MethodOf(unsigned bits) { return static_cast<Method>(bits & 3); }

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view input(reinterpret_cast<const char*>(data), size);
    if (input.size() < 4) return 0;
    EncryptParams params;
    params.filter = "Standard";
    params.v = static_cast<uint8_t>(input[0]) % 7;
    params.r = static_cast<uint8_t>(input[1]) % 8;
    const auto flags = static_cast<uint8_t>(input[2]);
    params.encryptMetadata = flags & 1;
    params.stream = CryptFilter{MethodOf(flags >> 1), (flags >> 5) * 4};
    params.string = CryptFilter{MethodOf(flags >> 3), (flags >> 5) * 4};
    params.lengthBits = 8 * static_cast<uint8_t>(input[3]);
    params.p = -4;
    input.remove_prefix(4);
    std::string_view password;
    if (!Take(input, params.o) || !Take(input, params.u) || !Take(input, params.oe) || !Take(input, params.ue) ||
        !Take(input, params.perms) || !Take(input, params.id0) || !Take(input, password)) {
        return 0;
    }
    SecurityHandler handler;
    if (handler.Open(params, password) != Status::Ok) return 0;
    std::string out;
    handler.Decrypt(1, 0, false, input, out);
    handler.Decrypt(0xFFFFFF, 0xFFFF, true, input, out);
    handler.DecryptWith(CryptFilter{Method::Aes128, 16}, 7, 0, input, out);
    handler.DecryptWith(CryptFilter{Method::Aes256, 32}, 7, 0, input, out);
    return 0;
}
