// SPDX-License-Identifier: Apache-2.0
// The built-in encoding of a Type 1 font program (Adobe Type 1 Font Format, section 2.3;
// ISO 32000-1 section 9.6.6.2; algorithms §3.3): read from the clear-text part before eexec, as
// `/Encoding StandardEncoding def` or an array filled by `dup <code> /<name> put`.

#include "text/program.h"

#include "pdf/syntax.h"

namespace RayoPdf::Text::Program {

bool ReadType1Encoding(std::string_view data, Type1Encoding& out) {
    out = Type1Encoding();
    // A PFB segment header: 0x80, type 1 (ASCII), a 4-byte little-endian length.
    if (data.size() >= 6 && static_cast<unsigned char>(data[0]) == 0x80 && data[1] == 1) {
        const uint32_t length = static_cast<uint32_t>(static_cast<unsigned char>(data[2]) |
                                                      static_cast<unsigned char>(data[3]) << 8 |
                                                      static_cast<unsigned char>(data[4]) << 16 |
                                                      static_cast<uint32_t>(static_cast<unsigned char>(data[5])) << 24);
        data = data.substr(6, length);
    }
    const size_t eexec = data.find("eexec");
    if (eexec != std::string_view::npos) data = data.substr(0, eexec);
    const size_t encoding = data.find("/Encoding");
    if (encoding == std::string_view::npos) return false;

    Syntax::Cursor c;
    c.data = data;
    c.pos = encoding + 9;
    Syntax::SkipSpace(c);
    if (Syntax::ReadToken(c) == "StandardEncoding") {
        out.standard = true;
        return true;
    }
    std::string_view names[256];
    bool any = false;
    // Tokens up to the array's closing `def` (or `readonly def`): the state counts the
    // `dup <code> /<name> put` sequence matched so far.
    int state = 0;
    int code = -1;
    std::string_view name;
    while (!c.AtEnd()) {
        Syntax::SkipSpace(c);
        if (c.AtEnd()) break;
        const size_t start = c.pos;
        const unsigned char ch = c.Peek();
        std::string_view token;
        bool isName = false;
        if (ch == '/') {
            c.pos++;
            token = Syntax::ReadToken(c);
            isName = true;
        } else if (Syntax::IsRegular(ch)) {
            token = Syntax::ReadToken(c);
        } else {
            c.pos++;
        }
        if (c.pos <= start) c.pos = start + 1;
        if (!isName && token == "def") break;
        if (state == 0) {
            state = !isName && token == "dup" ? 1 : 0;
        } else if (state == 1) {
            code = -1;
            if (!isName && !token.empty() && token.size() <= 3) {
                code = 0;
                for (char d : token) code = d >= '0' && d <= '9' && code >= 0 ? code * 10 + (d - '0') : -1;
            }
            state = code >= 0 && code < 256 ? 2 : 0;
        } else if (state == 2) {
            name = token;
            state = isName ? 3 : 0;
        } else {
            if (!isName && token == "put") {
                names[code] = name;
                any = true;
            }
            state = !isName && token == "dup" ? 1 : 0;
        }
    }
    if (!any) return false;
    for (const std::string_view& n : names) out.codes.Push(n);
    return true;
}

} // namespace RayoPdf::Text::Program
