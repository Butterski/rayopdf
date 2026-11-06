// SPDX-License-Identifier: Apache-2.0
// Core tests of src/crypt: digest and cipher vectors from the standards, and the Standard
// security handler against files encrypted by qpdf. Exit codes 100-199.
// `test_crypt --bench` also prints AES-256-CBC and RC4 throughput (never fails on time).
#include "check.h"
#include "crypt/crypt.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace RayoPdf::Crypt;

namespace {

std::string Hex(std::string_view hex) {
    std::string out;
    const auto nibble = [](char c) {
        return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<char>(nibble(hex[i]) << 4 | nibble(hex[i + 1])));
    return out;
}

std::string ToHex(const uint8_t* p, size_t n) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        out.push_back(kDigits[p[i] >> 4]);
        out.push_back(kDigits[p[i] & 15]);
    }
    return out;
}

const uint8_t* U8(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }

std::string Md5Hex(std::string_view s) {
    uint8_t d[16];
    Md5(s, d);
    return ToHex(d, 16);
}
std::string Sha256Hex(std::string_view s) {
    uint8_t d[32];
    Sha256(s, d);
    return ToHex(d, 32);
}
std::string Sha384Hex(std::string_view s) {
    uint8_t d[48];
    Sha384(s, d);
    return ToHex(d, 48);
}
std::string Sha512Hex(std::string_view s) {
    uint8_t d[64];
    Sha512(s, d);
    return ToHex(d, 64);
}

void TestDigests() {
    // RFC 1321 appendix A.5 test suite.
    RAYOPDF_CHECK(Md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e", 100);
    RAYOPDF_CHECK(Md5Hex("a") == "0cc175b9c0f1b6a831c399e269772661", 101);
    RAYOPDF_CHECK(Md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72", 102);
    RAYOPDF_CHECK(Md5Hex("message digest") == "f96b697d7cb7938d525a2f31aaf161d0", 103);
    RAYOPDF_CHECK(Md5Hex("abcdefghijklmnopqrstuvwxyz") == "c3fcd3d76192e4007dfb496cca67e13b", 104);
    RAYOPDF_CHECK(Md5Hex("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") ==
                      "d174ab98d277d9f5a5611c2c9f419d9f",
                  105);
    RAYOPDF_CHECK(Md5Hex("1234567890123456789012345678901234567890123456789012345678901234567890123456789"
                         "0") == "57edf4a22be3c955ac49da2e2107b67a",
                  106);

    // FIPS 180-4 examples (NIST CSRC "Examples with intermediate values"): "abc" and the
    // two-block messages.
    const std::string_view two256 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    const std::string_view two512 = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopq"
                                    "klmnopqrlmnopqrsmnopqrstnopqrstu";
    RAYOPDF_CHECK(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 107);
    RAYOPDF_CHECK(Sha256Hex(two256) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", 108);
    RAYOPDF_CHECK(Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 109);
    RAYOPDF_CHECK(Sha384Hex("abc") == "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
                                      "8086072ba1e7cc2358baeca134c825a7",
                  110);
    RAYOPDF_CHECK(Sha384Hex(two512) == "09330c33f71147e83d192fc782cd1b4753111b173b3b05d22fa08086e3b0f712"
                                       "fcc7c71a557e2db966c3e9fa91746039",
                  111);
    RAYOPDF_CHECK(Sha512Hex("abc") == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                                      "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
                  112);
    RAYOPDF_CHECK(Sha512Hex(two512) == "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
                                       "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909",
                  113);
    // One million 'a' (FIPS 180-4 / RFC 6234 test 3): many whole blocks.
    const std::string million(1000000, 'a');
    RAYOPDF_CHECK(Sha256Hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", 114);
    // Padding boundaries: 56 bytes need a second block for MD5's length, 64 bytes are one whole
    // block plus a padding block (digests from Python's hashlib).
    RAYOPDF_CHECK(Md5Hex(std::string(56, 'a')) == "3b0c8ac703f828b04c6c197006d17218", 115);
    RAYOPDF_CHECK(Sha256Hex(std::string(64, 'a')) ==
                      "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb",
                  116);
}

void TestCiphers() {
    // FIPS 197 appendix C.1 (AES-128) and C.3 (AES-256).
    const std::string plain = Hex("00112233445566778899aabbccddeeff");
    const std::string key128 = Hex("000102030405060708090a0b0c0d0e0f");
    const std::string key256 = Hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    const uint8_t zeroIv[16] = {};
    std::string cipher;
    RAYOPDF_CHECK(AesCbcEncrypt(U8(key128), 16, zeroIv, plain, cipher), 120);
    RAYOPDF_CHECK(ToHex(U8(cipher), cipher.size()) == "69c4e0d86a7b0430d8cdb78070b4c55a", 121);
    uint8_t block[16];
    RAYOPDF_CHECK(AesEcbDecryptBlock(U8(key128), 16, U8(cipher), block), 122);
    RAYOPDF_CHECK(std::memcmp(block, plain.data(), 16) == 0, 123);
    RAYOPDF_CHECK(AesCbcEncrypt(U8(key256), 32, zeroIv, plain, cipher), 124);
    RAYOPDF_CHECK(ToHex(U8(cipher), cipher.size()) == "8ea2b7ca516745bfeafc49904b496089", 125);
    RAYOPDF_CHECK(AesEcbDecryptBlock(U8(key256), 32, U8(cipher), block), 126);
    RAYOPDF_CHECK(std::memcmp(block, plain.data(), 16) == 0, 127);

    // NIST SP 800-38A F.2.1 and F.2.5: CBC-AES128 and CBC-AES256, four blocks.
    const std::string iv = Hex("000102030405060708090a0b0c0d0e0f");
    const std::string message = Hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                                    "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
    struct {
        std::string key;
        std::string_view expected;
    } const cases[2] = {
        {Hex("2b7e151628aed2a6abf7158809cf4f3c"),
         "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
         "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7"},
        {Hex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4"),
         "f58c4c04d6e5f1ba779eabfb5f7bfbd69cfc4e967edb808d679f777bc6702c7d"
         "39f23369a9d9bacfa530e26304231461b2eb05e2c39be9fcda6c19078c6a9d1b"},
    };
    for (const auto& c : cases) {
        RAYOPDF_CHECK(AesCbcEncrypt(U8(c.key), c.key.size(), U8(iv), message, cipher), 128);
        RAYOPDF_CHECK(ToHex(U8(cipher), cipher.size()) == c.expected, 129);
        std::string decrypted;
        RAYOPDF_CHECK(AesCbcDecrypt(U8(c.key), c.key.size(), iv + cipher, decrypted), 130);
        RAYOPDF_CHECK(decrypted == message, 131);
    }
    std::string out;
    RAYOPDF_CHECK(!AesCbcDecrypt(U8(key128), 16, std::string(20, 'x'), out), 132);
    RAYOPDF_CHECK(!AesCbcDecrypt(U8(key128), 24, std::string(32, 'x'), out), 133);
    RAYOPDF_CHECK(!AesCbcEncrypt(U8(key128), 16, zeroIv, std::string(15, 'x'), out), 134);

    // RC4: the well-known Wikipedia vectors and RFC 6229 section 2 (40-bit key 0102030405,
    // keystream offsets 0 and 16).
    struct {
        std::string_view key, plain, expected;
    } const rc4[3] = {
        {"Key", "Plaintext", "bbf316e8d940af0ad3"},
        {"Wiki", "pedia", "1021bf0420"},
        {"Secret", "Attack at dawn", "45a01f645fc35b383552544b9bf5"},
    };
    for (const auto& c : rc4) {
        std::string data(c.plain);
        Rc4(reinterpret_cast<const uint8_t*>(c.key.data()), c.key.size(), reinterpret_cast<uint8_t*>(&data[0]),
            data.size());
        RAYOPDF_CHECK(ToHex(U8(data), data.size()) == c.expected, 135);
    }
    std::string stream(32, '\0');
    const std::string rfcKey = Hex("0102030405");
    Rc4(U8(rfcKey), 5, reinterpret_cast<uint8_t*>(&stream[0]), stream.size());
    RAYOPDF_CHECK(ToHex(U8(stream), 32) == "b2396305f03dc027ccc3524a0a1118a86982944f18fc82d589c403a47a0d0919", 136);
}

// qpdf 12.4.2 (Apache-2.0) output, in the rayopdf-eval-producers:1 image: a hand-written
// 5-object PDF (an /Info /Title string and an uncompressed content stream) encrypted with
// `qpdf --allow-weak-crypto --object-streams=disable --compress-streams=n --encrypt user7 owner9
// <40 | 128 --use-aes=n | 128 --use-aes=y [--cleartext-metadata] | 256 [--force-R5]> -- in.pdf
// out.pdf`. qpdf renumbers: the /Title string is in object 2 0, the stream is object 5 0. The
// /Encrypt entries, /ID[0] and the encrypted bytes are copied verbatim below; the plaintexts are
// what `qpdf --decrypt` gives, and the file keys what `qpdf --show-encryption-key` prints.
struct Sample {
    const char* name;
    int v, r, lengthBits;
    Method method;
    bool encryptMetadata;
    const char *o, *u, *oe, *ue, *perms, *id0, *title, *stream;
};

constexpr const char* kTitle = "RayoPDF crypt sample title";
constexpr const char* kContent = "BT /F1 12 Tf 10 50 Td (Hello, encrypted world!) Tj ET";

const Sample kSamples[] = {
    {"R2 RC4-40", 1, 2, 40, Method::Rc4, true,
     "df810aa7dd5253f833edf81174f4a16e30be76c9c438ae3348f13350683d5735",
     "dee36d56bb553a741ce3969c1e502cb911759602e99d961db62db4f200847d0c", "", "", "",
     "70c251692c1d1e363e5267edb4e7fc79", "438570cc5ffb7f6fd02a03c592d39e130f3d8e5b5cd530575858",
     "fbc3c12153b3f03c5dfda3f24bce0b94da9941e2cd60be2e47de5a02672a18608a0bddefd9e0ddd4e3a76f2b2566dc0b8fbd9e25e1"},
    {"R3 RC4-128", 2, 3, 128, Method::Rc4, true,
     "6b5b31559e3fa2c9e8563f7d81e556f3cba645231892aa151aadc38f46ac8e56",
     "d1f5b303b11d3b5cbe3c612887bc47970021446990b9e4114071a4d9104984c1", "", "", "",
     "892062397ddf6242716dd30652e1c370", "f7087e6495ef6eb87f14bdef0fe35795b200dce77094e91024a4",
     "e55ab3f96ff69863035114b0c151211f66e5cf0232165fe52ea11e47672d900349a62c6374adb60b4930e587b8ce78eb98b5592068"},
    {"R4 AESV2", 4, 4, 128, Method::Aes128, true,
     "6b5b31559e3fa2c9e8563f7d81e556f3cba645231892aa151aadc38f46ac8e56",
     "bb23c1f871c7dca85cf3df7d609b0b650021446990b9e4114071a4d9104984c1", "", "", "",
     "38ec3d37c4dc1a318de6812283c99529",
     "0986f9469128719dabf07cec422ba09f88d3c656b51fd24abd9e0959fbd895bb207239b09b8fe37ae5dae7cf8b4fe2a0",
     "af3aa356fa708321807f24bd6cd6cbe80835f4f3ea2bfc2f46c6aeb76dc8addfb5376c6a05025656a1df07cf1434cb2e"
     "6a060f9201028a73e4580536f00b21f222782f07251bcba7852e0e08136e84a6"},
    {"R4 AESV2 cleartext metadata", 4, 4, 128, Method::Aes128, false,
     "6b5b31559e3fa2c9e8563f7d81e556f3cba645231892aa151aadc38f46ac8e56",
     "c79d24fe2a33211eaef45e9b057afe120021446990b9e4114071a4d9104984c1", "", "", "",
     "96df31495d909bf6b85ada0e35021aa1",
     "8f0210b14b5db72a8aa40b27673102ed17f616fbfcecee8aed54a5f9e82ae5452f589e13b251d967439d072b40cf861b",
     "b905dd0557f77bd4ec4a43a91b004b564f9fe15fc2e7203c28ae249c8c34ebe5e8120663058c6c5b704c786598f78d11"
     "2eadc46cd503d2afbed1c6e63d5628d30cf246b41b3f1eb30ed555ecc0cc0edd"},
    {"R5 AESV3", 5, 5, 256, Method::Aes256, true,
     "dd8b946b88c48e22d118a9ec968daf73905ab2dfa86c1777cd34f60d774c6f1c7b86955ffdf1be1a6b77eb73bdfdf4c3",
     "47e4e7f9f3d0b511f406d48a81c959d25e45c3c3175066375cf82b3df2dd5903a08c2cdc905debebabc17d33134af0e4",
     "6df6545f28cc346ce63ae5deb5be302d7596837547290558b68eda298b684197",
     "d4469b3b39b7568c474f0ca33c93062d22daef1bb8e4ca136ea49a2a9024ba55", "25930f74f17c9db33d874a7b59bf5e93",
     "a7e425e4baf3a1318a444fa0ce3d6676",
     "4fecf170b450e5cf1feddd03c79a9be188d10c0b785724b4f284abbf13132e6fbebfd40459bde1f11aaaef637831444b",
     "c6249338e647f2f10986b81943b23d4a87a71573b4257cba9fc791a5f53742feece757c61a7b1b90cd8e3dc063359efd"
     "7b62581645542480ed8eee6b056102d58fd242fec2793e6f5bb74f40cf3fb5f5"},
    {"R6 AESV3", 5, 6, 256, Method::Aes256, true,
     "51d79d199e2c0a6f6a2600ae0a0f3d07b41fa6e2ba58e55c828f181c308dfc58ea57358c966cf7ba32a98748771e576b",
     "6110b84925d62b0336e0d49573e2bd764c6ae683cbc39188da5c9e91b5b16c5e3a738cea4dd815bcbe9b89d6e7b43068",
     "a5ad440b8b97aa5b1a65143dc4643a5267783127230d95e5becb82e41bbe347b",
     "9219e8d5090faaab469d2f5f7a9dca75016cdfdc3d5da73fe5656ab45eb06cc2", "cd7bcf0bafe421d4ab6ea3d1c40b4fe6",
     "7a534a1d4dc9a2381e705fc3fddef928",
     "4f5c225ab705b44ccb6c6e6250107e2904e5489de69276f708550d2edf0f972dd38c17ad9c7c6939ead71d6d31e11162",
     "1c48b69bb60187fe8dd050201f9970d7cc510acd98910eb6b696c538d64e6347014de9fda753b886a9a33ca0bb41da80"
     "ceadf7c4b7b736fc8f9d48bb0b2827ca93b0cfb07e6dd91f3a896a476be108e9"},
};

// Owns the decoded bytes an EncryptParams views.
struct ParamsHolder {
    std::string o, u, oe, ue, perms, id0;
    EncryptParams params;

    explicit ParamsHolder(const Sample& s)
        : o(Hex(s.o)), u(Hex(s.u)), oe(Hex(s.oe)), ue(Hex(s.ue)), perms(Hex(s.perms)), id0(Hex(s.id0)) {
        params.filter = "Standard";
        params.v = s.v;
        params.r = s.r;
        params.lengthBits = s.lengthBits;
        params.o = o, params.u = u, params.oe = oe, params.ue = ue, params.perms = perms;
        params.p = -4;
        params.encryptMetadata = s.encryptMetadata;
        params.id0 = id0;
        if (s.v >= 4) {
            params.stream = CryptFilter{s.method, s.method == Method::Aes256 ? 32 : 16};
            params.string = params.stream;
        }
    }
};

void CheckSampleDecrypts(const SecurityHandler& handler, const Sample& s) {
    std::string out;
    RAYOPDF_CHECK(handler.Decrypt(2, 0, false, Hex(s.title), out), 140);
    RAYOPDF_CHECK(out == kTitle, 141);
    RAYOPDF_CHECK(handler.Decrypt(5, 0, true, Hex(s.stream), out), 142);
    RAYOPDF_CHECK(out == kContent, 143);
    // The wrong object number gives another key: garbage, never the plaintext.
    handler.Decrypt(3, 0, false, Hex(s.title), out);
    RAYOPDF_CHECK(out != kTitle || s.method == Method::Aes256, 144);
}

void TestHandlerSamples() {
    for (const Sample& s : kSamples) {
        std::fprintf(stderr, "sample %s\n", s.name);
        ParamsHolder h(s);
        SecurityHandler handler;
        RAYOPDF_CHECK(handler.Open(h.params, "user7") == Status::Ok, 145);
        RAYOPDF_CHECK(!handler.OpenedAsOwner(), 146);
        RAYOPDF_CHECK(handler.Revision() == s.r, 147);
        RAYOPDF_CHECK(handler.StreamMethod() == s.method && handler.StringMethod() == s.method, 148);
        RAYOPDF_CHECK(handler.EncryptMetadata() == s.encryptMetadata, 149);
        RAYOPDF_CHECK(handler.PermsValid() == (s.r >= 5), 150);
        CheckSampleDecrypts(handler, s);

        RAYOPDF_CHECK(handler.Open(h.params, "owner9") == Status::Ok, 151);
        RAYOPDF_CHECK(handler.OpenedAsOwner(), 152);
        CheckSampleDecrypts(handler, s);

        RAYOPDF_CHECK(handler.Open(h.params, "") == Status::PasswordRequired, 153);
        RAYOPDF_CHECK(handler.Open(h.params, "user8") == Status::PasswordRequired, 154);
        // A failed Open leaves the handler as constructed: no revision, nothing decrypted.
        RAYOPDF_CHECK(handler.Revision() == 0 && !handler.OpenedAsOwner(), 155);
    }
}

// Malformed dictionaries and unsupported handlers.
void TestHandlerStatus() {
    const Sample& r4 = kSamples[2];
    const Sample& r6 = kSamples[5];
    SecurityHandler handler;
    {
        ParamsHolder h(r4);
        h.params.filter = "Adobe.PubSec";
        RAYOPDF_CHECK(handler.Open(h.params, "user7") == Status::Unsupported, 160);
    }
    {
        ParamsHolder h(r4);
        h.params.v = 3;
        RAYOPDF_CHECK(handler.Open(h.params, "user7") == Status::Unsupported, 161);
        h.params.v = 4, h.params.r = 7;
        RAYOPDF_CHECK(handler.Open(h.params, "user7") == Status::Unsupported, 162);
    }
    {
        ParamsHolder h(r4);
        h.params.stream.method = Method::Aes256;  // AESV3 under V 4
        RAYOPDF_CHECK(handler.Open(h.params, "user7") == Status::Unsupported, 163);
    }
    {
        ParamsHolder h(r4);
        h.params.o = h.params.o.substr(0, 31);
        RAYOPDF_CHECK(handler.Open(h.params, "user7") == Status::Corrupt, 164);
        h.params.o = std::string_view();
        RAYOPDF_CHECK(handler.Open(h.params, "user7") == Status::Corrupt, 165);
    }
    {
        ParamsHolder h(kSamples[1]);
        h.params.lengthBits = 44;
        RAYOPDF_CHECK(handler.Open(h.params, "user7") == Status::Corrupt, 166);
    }
    {
        ParamsHolder h(r6);
        h.params.ue = h.params.ue.substr(0, 16);
        RAYOPDF_CHECK(handler.Open(h.params, "user7") == Status::Corrupt, 167);
    }
    {
        ParamsHolder h(r6);
        h.params.u = h.params.u.substr(0, 40);
        RAYOPDF_CHECK(handler.Open(h.params, "user7") == Status::Corrupt, 168);
    }
    {
        // Longer /O and /U (some writers pad them) are truncated to 48 bytes.
        ParamsHolder h(r6);
        h.o += std::string(79, '\0');
        h.u += std::string(79, '\0');
        h.params.o = h.o, h.params.u = h.u;
        RAYOPDF_CHECK(handler.Open(h.params, "user7") == Status::Ok, 169);
        CheckSampleDecrypts(handler, r6);
    }
    {
        // A /Perms mismatch is not fatal.
        ParamsHolder h(r6);
        h.params.p = -3904;
        RAYOPDF_CHECK(handler.Open(h.params, "owner9") == Status::Ok, 170);
        RAYOPDF_CHECK(!handler.PermsValid(), 171);
        CheckSampleDecrypts(handler, r6);
    }
    {
        // EncryptMetadata enters the R 4 key: the flag flipped no longer opens.
        ParamsHolder h(kSamples[3]);
        h.params.encryptMetadata = true;
        RAYOPDF_CHECK(handler.Open(h.params, "user7") == Status::PasswordRequired, 172);
    }
}

// An empty user password opens the document whatever password is given: a wrong one falls back
// to "". The R 2 dictionary is built here from Algorithms 2 and 4 (/U = RC4 of the padding).
void TestEmptyUserPassword() {
    EncryptParams params;
    params.filter = "Standard";
    params.v = 1, params.r = 2;
    const std::string o(32, '\x11');
    params.o = o;
    params.p = -1;
    const std::string id0 = Hex("00112233445566778899aabbccddeeff");
    params.id0 = id0;
    // File key per Algorithm 2 for the empty password.
    std::string input(Hex("28bf4e5e4e758a4164004e56fffa01082e2e00b6d0683e802f0ca9fe6453697a"));
    input += o;
    input += std::string(4, '\xff');
    input += id0;
    uint8_t digest[16];
    Md5(input, digest);
    std::string u(Hex("28bf4e5e4e758a4164004e56fffa01082e2e00b6d0683e802f0ca9fe6453697a"));
    Rc4(digest, 5, reinterpret_cast<uint8_t*>(&u[0]), 32);
    params.u = u;
    SecurityHandler handler;
    RAYOPDF_CHECK(handler.Open(params, "") == Status::Ok, 175);
    RAYOPDF_CHECK(handler.Open(params, "anything") == Status::Ok, 176);
    RAYOPDF_CHECK(!handler.OpenedAsOwner(), 177);
    // Per-object key and RC4: decrypting twice with the same object restores the input.
    std::string once, twice;
    RAYOPDF_CHECK(handler.Decrypt(7, 1, true, "hello", once), 178);
    RAYOPDF_CHECK(handler.Decrypt(7, 1, true, once, twice) && twice == "hello", 179);
    // Fuzz regression: an AESV2 filter over a 40-bit file key still gets a 16-byte AES key.
    std::string aes;
    handler.DecryptWith(CryptFilter{Method::Aes128, 16}, 7, 1, std::string(48, 'x'), aes);
    RAYOPDF_CHECK(aes.size() <= 32, 190);
}

// AES framing: truncated data, bad padding, the IV alone.
void TestAesFraming() {
    const Sample& r4 = kSamples[2];
    ParamsHolder h(r4);
    SecurityHandler handler;
    RAYOPDF_CHECK(handler.Open(h.params, "user7") == Status::Ok, 180);
    const std::string title = Hex(r4.title);  // 16 IV + 32 ciphertext
    std::string out;
    RAYOPDF_CHECK(!handler.Decrypt(2, 0, false, title.substr(0, 10), out) && out.empty(), 181);
    RAYOPDF_CHECK(handler.Decrypt(2, 0, false, title.substr(0, 16), out) && out.empty(), 182);
    // One whole block: decrypts, but its last byte is no valid padding.
    RAYOPDF_CHECK(!handler.Decrypt(2, 0, false, title.substr(0, 32), out), 183);
    RAYOPDF_CHECK(out.size() == 16 && out.compare(0, 16, kTitle, 16) == 0, 184);
    // A trailing partial block is dropped; what decrypts is kept.
    RAYOPDF_CHECK(!handler.Decrypt(2, 0, false, title + "abc", out), 185);
    RAYOPDF_CHECK(out == kTitle, 186);
    // Identity copies.
    RAYOPDF_CHECK(handler.DecryptWith(CryptFilter{Method::Identity, 0}, 2, 0, "plain", out) && out == "plain", 187);
}

void Bench() {
    const size_t size = 64u << 20;
    std::vector<uint8_t> data(size + 16);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 131 + 7);
    const std::string_view view(reinterpret_cast<const char*>(data.data()), data.size());
    uint8_t key[32];
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i);
    std::string out;
    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    AesCbcDecrypt(key, 32, view, out);
    double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    std::printf("AES-256-CBC decrypt: %.1f MB/s (%zu MB)\n", static_cast<double>(size) / 1e6 / seconds, size >> 20);
    start = Clock::now();
    Rc4(key, 16, data.data(), size);
    seconds = std::chrono::duration<double>(Clock::now() - start).count();
    std::printf("RC4: %.1f MB/s (%zu MB)\n", static_cast<double>(size) / 1e6 / seconds, size >> 20);
    const uint8_t iv[16] = {};
    start = Clock::now();
    AesCbcEncrypt(key, 32, iv, view.substr(16), out);
    seconds = std::chrono::duration<double>(Clock::now() - start).count();
    std::printf("AES-256-CBC encrypt: %.1f MB/s (%zu MB)\n", static_cast<double>(size) / 1e6 / seconds, size >> 20);
    std::printf("checksum %u\n", static_cast<unsigned>(static_cast<uint8_t>(out[100]) ^ data[100]));
}

} // namespace

int main(int argc, char** argv) {
    TestDigests();
    TestCiphers();
    TestHandlerSamples();
    TestHandlerStatus();
    TestEmptyUserPassword();
    TestAesFraming();
    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) Bench();
    std::printf("test_crypt: ok\n");
    return 0;
}
