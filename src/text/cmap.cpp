// SPDX-License-Identifier: Apache-2.0
// CMap parsing (algorithms §3.2, §3.4). The CMap language is PostScript: the parser tokenizes with
// the PDF lexical rules (pdf/syntax.h) and acts on the begin*/end* sections and the /WMode and
// usecmap entries; every other PostScript construct is skipped token by token.

#include "text/cmap.h"

#include "common/utf8.h"
#include "pdf/syntax.h"
#include "pdf/value.h"
#include "text/font.h"
#include "text/tables.h"

#include <algorithm>

namespace RayoPdf::Text {
namespace {

enum class TokenKind : uint8_t { String, Name, Number, Keyword, ArrayOpen, ArrayClose, Other };

struct Token {
    TokenKind kind = TokenKind::Other;
    std::string_view text;  // String and Name bytes (in the lexer's arena or the data), Keyword bytes (in the data)
    int64_t number = 0;
};

// Tokens of a CMap. Names and strings that need no decoding view the data; the other string and
// name bytes live in the arena until Release.
class Lexer {
public:
    explicit Lexer(std::string_view data) : builder_(arena_, true) { cursor_.data = data; }

    bool Next(Token& token) {
        Syntax::SkipSpace(cursor_);
        if (cursor_.AtEnd()) return false;
        const size_t start = cursor_.pos;
        const unsigned char ch = cursor_.Peek();
        token = Token();
        bool ok = false;
        if (ch == '<' && cursor_.Peek(1) == '<') {
            cursor_.pos += 2;
        } else if (ch == '>') {
            cursor_.pos += cursor_.Peek(1) == '>' ? 2 : 1;
        } else if (ch == '<') {
            ok = Syntax::ReadHexString(cursor_, builder_, token.text);
            token.kind = TokenKind::String;
        } else if (ch == '(') {
            ok = Syntax::ReadLiteralString(cursor_, builder_, token.text);
            token.kind = TokenKind::String;
        } else if (ch == '/') {
            ok = Syntax::ReadName(cursor_, builder_, token.text);
            token.kind = TokenKind::Name;
        } else if (ch == '[') {
            cursor_.pos++;
            token.kind = TokenKind::ArrayOpen;
            ok = true;
        } else if (ch == ']') {
            cursor_.pos++;
            token.kind = TokenKind::ArrayClose;
            ok = true;
        } else if (Syntax::IsRegular(ch)) {
            token.text = Syntax::ReadToken(cursor_);
            PdfValue value;
            if (Syntax::StartsNumber(ch) && Syntax::ParseNumber(token.text, value)) {
                token.kind = TokenKind::Number;
                token.number = value.Int(-1);
            } else {
                token.kind = TokenKind::Keyword;
            }
            ok = true;
        } else {
            cursor_.pos++;  // ')', '{', '}'
        }
        if (!ok) token = Token();
        if (cursor_.pos <= start) cursor_.pos = start + 1;  // progress on any input
        return true;
    }

    // Forgets the bytes of the tokens read so far.
    void Release() { arena_.Reset(); }

    // A plain hex string at the cursor after white space: '<', hex digits only, '>'. Its bytes
    // decoded (as ReadHexString does: an odd last digit is its byte's high half) into `out`,
    // `size` at most `capacity`; the cursor moves past it. False, the cursor unmoved, for anything
    // else (a comment, white space inside, too long, the end), which the general lexer reads.
    bool PlainHex(char* out, size_t capacity, size_t& size) {
        const char* const data = cursor_.data.data();
        const size_t end = cursor_.data.size();
        size_t at = cursor_.pos;
        while (at < end && Syntax::IsWhite(static_cast<unsigned char>(data[at]))) ++at;
        if (at >= end || data[at] != '<') return false;
        size = 0;
        int high = -1;
        for (++at; at < end; ++at) {
            const unsigned char ch = static_cast<unsigned char>(data[at]);
            const int d = Syntax::kHexValues.of[ch];
            if (d < 0) break;
            if (high < 0) {
                if (size == capacity) return false;
                high = d;
            } else {
                out[size++] = static_cast<char>(high << 4 | d);
                high = -1;
            }
        }
        if (at >= end || data[at] != '>') return false;
        if (high >= 0) out[size++] = static_cast<char>(high << 4);
        cursor_.pos = at + 1;
        return true;
    }
    size_t Position() const { return cursor_.pos; }
    void Rewind(size_t pos) { cursor_.pos = pos; }

private:
    Syntax::Cursor cursor_;
    PdfArena arena_;
    Syntax::Builder builder_;
};

bool CodeOf(std::string_view bytes, uint32_t& code) {
    if (bytes.empty() || bytes.size() > 4) return false;
    code = 0;
    for (unsigned char c : bytes) code = code << 8 | c;
    return true;
}

// UTF-16BE `units` as UTF-8 onto `out`: surrogate pairs joined, a lone surrogate as U+FFFD.
void AppendUtf16(const uint16_t* units, size_t count, std::string& out) {
    for (size_t i = 0; i < count; ++i) {
        const uint32_t u = units[i];
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < count && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
            Utf8::Append(out, 0x10000 + ((u - 0xD800) << 10) + (units[i + 1] - 0xDC00));
            ++i;
        } else {
            Utf8::Append(out, u);
        }
    }
}

// One UTF-16 unit as UTF-8 onto `out`: AppendUtf16 of a single unit (a surrogate alone is U+FFFD).
inline void AppendUnit(uint32_t u, std::string& out) {
    if (u < 0x80) {
        out.push_back(static_cast<char>(u));
        return;
    }
    if (u >= 0xD800 && u <= 0xDFFF) u = Utf8::kReplacement;
    if (u < 0x800) {
        const char bytes[2] = {static_cast<char>(0xC0 | (u >> 6)), static_cast<char>(0x80 | (u & 0x3F))};
        out.append(bytes, 2);
    } else {
        const char bytes[3] = {static_cast<char>(0xE0 | (u >> 12)), static_cast<char>(0x80 | ((u >> 6) & 0x3F)),
                               static_cast<char>(0x80 | (u & 0x3F))};
        out.append(bytes, 3);
    }
}

// The units from `u` up to `end` (exclusive) that AppendUnit writes with `width` bytes each:
// U+0000-007F, U+0080-07FF, U+0800-D7FF, the surrogates (each U+FFFD) and U+E000-FFFF.
struct UnitPiece {
    uint32_t end;
    uint32_t width;
    bool surrogate;
};
inline UnitPiece PieceOf(uint32_t u) {
    if (u < 0x80) return {0x80, 1, false};
    if (u < 0x800) return {0x800, 2, false};
    if (u < 0xD800) return {0xD800, 3, false};
    if (u < 0xE000) return {0xE000, 3, true};
    return {0x10000, 3, false};
}

// The UTF-16 units of a destination string; a single byte is read as one unit (producers write
// <41> for "A"), an odd last byte is dropped. At most 256 units: longer ones are cut.
size_t UnitsOf(std::string_view bytes, uint16_t* units, size_t max) {
    if (bytes.size() == 1) {
        units[0] = static_cast<unsigned char>(bytes[0]);
        return 1;
    }
    size_t count = 0;
    for (size_t i = 0; i + 1 < bytes.size() && count < max; i += 2) {
        units[count++] = static_cast<uint16_t>(static_cast<unsigned char>(bytes[i]) << 8 | static_cast<unsigned char>(bytes[i + 1]));
    }
    return count;
}

constexpr size_t kMaxUnits = 256;

enum class Section : uint8_t { None, Codespace, BfChar, BfRange, CidChar, CidRange, NotdefRange };

Section SectionOf(std::string_view keyword) {
    if (keyword == "begincodespacerange") return Section::Codespace;
    if (keyword == "beginbfchar") return Section::BfChar;
    if (keyword == "beginbfrange") return Section::BfRange;
    if (keyword == "begincidchar") return Section::CidChar;
    if (keyword == "begincidrange") return Section::CidRange;
    if (keyword == "beginnotdefrange") return Section::NotdefRange;
    return Section::None;
}

// What a section maps, for the two kinds of CMap.
class Sink {
public:
    virtual ~Sink() = default;
    Codespace* codespace = nullptr;
    int wmode = 0;
    std::string useCMap;
    // Room for more codes; false once a limit is reached.
    virtual bool Full() const = 0;
    virtual void Char(uint32_t code, const Token& destination) = 0;
    // A range whose destination is a string or a number.
    virtual void Range(uint32_t low, uint32_t high, const Token& destination, Section section) = 0;
    // Element `index` of an array destination of a bfrange starting at `low`.
    virtual void ArrayItem(uint32_t code, const Token& destination) = 0;
    virtual void Cut() = 0;
};

// A bfchar or bfrange entry of plain hex strings (nearly every entry), read without tokens: the
// sink gets the calls Parse's token loop makes for it. False, nothing read, for any other entry
// (a name, number or array destination, a comment, a code of no 1-4 bytes), which the loop reads.
bool FastEntry(Lexer& lexer, Section section, Sink& sink) {
    const size_t start = lexer.Position();
    char low[4], high[4], destination[kMaxUnits * 2];
    size_t lowSize = 0, highSize = 0, destinationSize = 0;
    const bool read = lexer.PlainHex(low, sizeof low, lowSize) && lowSize > 0 &&
                      (section == Section::BfChar || lexer.PlainHex(high, sizeof high, highSize)) &&
                      lexer.PlainHex(destination, sizeof destination, destinationSize);
    if (!read) {
        lexer.Rewind(start);
        return false;
    }
    uint32_t lowCode = 0, highCode = 0;
    CodeOf(std::string_view(low, lowSize), lowCode);
    Token token;
    token.kind = TokenKind::String;
    token.text = std::string_view(destination, destinationSize);
    if (section == Section::BfChar) {
        sink.Char(lowCode, token);
    } else if (CodeOf(std::string_view(high, highSize), highCode) && highCode >= lowCode) {
        if (highCode - lowCode >= kMaxRangeCodes) {
            highCode = lowCode + kMaxRangeCodes - 1;
            sink.Cut();
        }
        sink.Range(lowCode, highCode, token, section);
    }
    return true;
}

void Parse(std::string_view data, Sink& sink) {
    Lexer lexer(data);
    Token token;
    Token group[3];
    size_t operands = 0;
    Section section = Section::None;
    bool lastWasWMode = false;
    std::string lastName;
    while (!sink.Full()) {
        if (operands == 0 && (section == Section::BfChar || section == Section::BfRange) &&
            FastEntry(lexer, section, sink)) {
            lexer.Release();
            continue;
        }
        if (!lexer.Next(token)) break;
        if (token.kind == TokenKind::Keyword) {
            const Section begins = SectionOf(token.text);
            if (begins != Section::None) {
                section = begins;
            } else if (token.text.substr(0, 3) == "end") {
                section = Section::None;
            } else if (token.text == "usecmap" && !lastName.empty()) {
                sink.useCMap = lastName;
            }
            operands = 0;
            lastWasWMode = false;
            lexer.Release();
            continue;
        }
        if (token.kind == TokenKind::Name) {
            lastName.assign(token.text.data(), token.text.size());
            lastWasWMode = token.text == "WMode";
            if (section == Section::None) continue;
        } else if (token.kind == TokenKind::Number && lastWasWMode) {
            sink.wmode = token.number == 1 ? 1 : 0;
            lastWasWMode = false;
            continue;
        }
        if (section == Section::None) continue;
        if (token.kind == TokenKind::ArrayOpen) {
            // An array destination: only after the two codes of a bfrange.
            uint32_t low = 0, high = 0;
            const bool usable = section == Section::BfRange && operands == 2 && group[0].kind == TokenKind::String &&
                                group[1].kind == TokenKind::String && CodeOf(group[0].text, low) &&
                                CodeOf(group[1].text, high) && high >= low;
            operands = 0;
            lexer.Release();
            uint32_t index = 0;
            while (lexer.Next(token) && token.kind != TokenKind::ArrayClose && token.kind != TokenKind::Keyword) {
                if (usable && low + index <= high && index < kMaxRangeCodes && !sink.Full()) {
                    sink.ArrayItem(low + index, token);
                }
                ++index;
                lexer.Release();
            }
            if (usable && high - low >= kMaxRangeCodes) sink.Cut();
            if (token.kind == TokenKind::Keyword) section = SectionOf(token.text);
            lexer.Release();
            continue;
        }
        if (token.kind != TokenKind::String && token.kind != TokenKind::Number && token.kind != TokenKind::Name) {
            operands = 0;
            lexer.Release();
            continue;
        }
        group[operands++] = token;
        const size_t need = section == Section::Codespace || section == Section::BfChar || section == Section::CidChar ? 2 : 3;
        if (operands < need) continue;
        operands = 0;
        uint32_t low = 0, high = 0;
        if (group[0].kind != TokenKind::String || !CodeOf(group[0].text, low)) {
            lexer.Release();
            continue;
        }
        switch (section) {
        case Section::Codespace:
            if (group[1].kind == TokenKind::String && sink.codespace) sink.codespace->Add(group[0].text, group[1].text);
            break;
        case Section::BfChar:
        case Section::CidChar:
            sink.Char(low, group[1]);
            break;
        case Section::BfRange:
        case Section::CidRange:
        case Section::NotdefRange:
            if (group[1].kind == TokenKind::String && CodeOf(group[1].text, high) && high >= low) {
                if (high - low >= kMaxRangeCodes) {
                    high = low + kMaxRangeCodes - 1;
                    sink.Cut();
                }
                sink.Range(low, high, group[2], section);
            }
            break;
        case Section::None:
            break;
        }
        lexer.Release();
    }
}

// One-unit bfranges of this many codes or more are written in place (UnicodeSink::Range).
constexpr uint64_t kInPlaceCodes = 8;

class UnicodeSink final : public Sink {
public:
    explicit UnicodeSink(ToUnicodeMap& map) : map_(map) { codespace = &map.codespace; }

    bool Full() const override { return map_.entries.size() >= kMaxMappedCodes; }
    void Cut() override { map_.truncated = true; }

    void Char(uint32_t code, const Token& destination) override {
        const size_t start = map_.text.size();
        if (destination.kind == TokenKind::String && destination.text.size() <= 2) {
            // One unit (UnitsOf: a single byte is a unit itself), the common destination.
            const std::string_view bytes = destination.text;
            if (bytes.size() == 1) {
                AppendUnit(static_cast<unsigned char>(bytes[0]), map_.text);
            } else if (bytes.size() == 2) {
                AppendUnit(static_cast<uint32_t>(static_cast<unsigned char>(bytes[0])) << 8 | static_cast<unsigned char>(bytes[1]),
                           map_.text);
            }
        } else if (destination.kind == TokenKind::String) {
            uint16_t units[kMaxUnits];
            AppendUtf16(units, UnitsOf(destination.text, units, kMaxUnits), map_.text);
        } else if (destination.kind == TokenKind::Name) {
            GlyphNameToUnicode(destination.text, map_.text);
        } else if (destination.kind == TokenKind::Number && destination.number >= 0 && destination.number <= 0x10FFFF) {
            Utf8::Append(map_.text, static_cast<uint32_t>(destination.number));
        }
        Add(code, start);
    }

    void ArrayItem(uint32_t code, const Token& destination) override { Char(code, destination); }

    void Range(uint32_t low, uint32_t high, const Token& destination, Section section) override {
        if (section == Section::NotdefRange) return;
        if (destination.kind == TokenKind::Number) {
            // cidrange in a ToUnicode stream: consecutive code points.
            if (destination.number < 0 || destination.number > 0x10FFFF) return;
            for (uint32_t code = low; code <= high && !Full(); ++code) {
                const size_t start = map_.text.size();
                Utf8::Append(map_.text, static_cast<uint32_t>(destination.number) + (code - low));
                Add(code, start);
                if (code == UINT32_MAX) break;
            }
            return;
        }
        if (destination.kind != TokenKind::String) return;
        uint16_t units[kMaxUnits];
        const size_t count = UnitsOf(destination.text, units, kMaxUnits);
        if (count == 0) return;
        const uint16_t last = units[count - 1];
        if (count == 1 && static_cast<uint64_t>(high) - low + 1 < kInPlaceCodes) {
            // A short one-unit range: code by code (sizing the map for it costs more).
            for (uint32_t code = low; code <= high && !Full(); ++code) {
                const size_t start = map_.text.size();
                AppendUnit(static_cast<uint16_t>(last + (code - low)), map_.text);
                Add(code, start);
                if (code == UINT32_MAX) break;
            }
            return;
        }
        if (count == 1) {
            // One unit counting up, the common range: every code gets an entry of 1-3 bytes
            // (AppendUnit), written in place, up to the map's limit. The units run in pieces of one
            // UTF-8 length (UnitPiece): the text's size first, then each piece written by index.
            const size_t first = map_.entries.size();
            const uint64_t room = first < kMaxMappedCodes ? kMaxMappedCodes - first : 0;
            const uint64_t span = static_cast<uint64_t>(high) - low + 1;
            const size_t n = static_cast<size_t>(span < room ? span : room);
            const size_t start = map_.text.size();
            size_t bytes = 0;
            for (size_t i = 0; i < n;) {
                const UnitPiece piece = PieceOf(static_cast<uint16_t>(last + i));
                const size_t m = std::min<size_t>(n - i, piece.end - static_cast<uint16_t>(last + i));
                bytes += m * piece.width;
                i += m;
            }
            map_.entries.resize(first + n);
            map_.text.resize(start + bytes);
            UnicodeEntry* const entry = map_.entries.data() + first;
            char* const base = &map_.text[0];
            size_t at = start;
            for (size_t i = 0; i < n;) {
                const uint32_t u = static_cast<uint16_t>(last + i);
                const UnitPiece piece = PieceOf(u);
                const size_t m = std::min<size_t>(n - i, piece.end - u);
                char* const out = base + at;
                UnicodeEntry* const to = entry + i;
                const uint32_t code = low + static_cast<uint32_t>(i);
                const uint32_t offset = static_cast<uint32_t>(at);
                if (piece.width == 1) {
                    for (size_t j = 0; j < m; ++j) out[j] = static_cast<char>(u + j);
                } else if (piece.width == 2) {
                    for (size_t j = 0; j < m; ++j) {
                        const uint32_t v = u + static_cast<uint32_t>(j);
                        out[2 * j] = static_cast<char>(0xC0 | (v >> 6));
                        out[2 * j + 1] = static_cast<char>(0x80 | (v & 0x3F));
                    }
                } else {
                    for (size_t j = 0; j < m; ++j) {
                        const uint32_t v = piece.surrogate ? Utf8::kReplacement : u + static_cast<uint32_t>(j);
                        out[3 * j] = static_cast<char>(0xE0 | (v >> 12));
                        out[3 * j + 1] = static_cast<char>(0x80 | ((v >> 6) & 0x3F));
                        out[3 * j + 2] = static_cast<char>(0x80 | (v & 0x3F));
                    }
                }
                for (size_t j = 0; j < m; ++j) {
                    to[j] = UnicodeEntry{code + static_cast<uint32_t>(j), offset + static_cast<uint32_t>(j * piece.width), piece.width};
                }
                at += m * piece.width;
                i += m;
            }
            if (n >= kMinRunCodes) map_.runs.push_back(UnicodeRun{static_cast<uint32_t>(first), static_cast<uint32_t>(n), last});
            return;
        }
        for (uint32_t code = low; code <= high && !Full(); ++code) {
            units[count - 1] = static_cast<uint16_t>(last + (code - low));
            const size_t start = map_.text.size();
            AppendUtf16(units, count, map_.text);
            Add(code, start);
            if (code == UINT32_MAX) break;
        }
    }

private:
    void Add(uint32_t code, size_t start) {
        const size_t length = map_.text.size() - start;
        if (length == 0) return;
        map_.entries.push_back(UnicodeEntry{code, static_cast<uint32_t>(start), static_cast<uint32_t>(length)});
    }

    ToUnicodeMap& map_;
};

class CidSink final : public Sink {
public:
    explicit CidSink(EncodingCMap& map) : map_(map) { codespace = &map.codespace; }

    bool Full() const override { return mapped_ >= kMaxMappedCodes; }
    void Cut() override { map_.truncated = true; }

    void Char(uint32_t code, const Token& destination) override {
        uint32_t cid = 0;
        if (CidOf(destination, cid)) Push(map_.cids, code, code, cid);
    }
    void ArrayItem(uint32_t code, const Token& destination) override { Char(code, destination); }
    void Range(uint32_t low, uint32_t high, const Token& destination, Section section) override {
        uint32_t cid = 0;
        if (!CidOf(destination, cid)) return;
        Push(section == Section::NotdefRange ? map_.notdefs : map_.cids, low, high, cid);
    }

private:
    static bool CidOf(const Token& destination, uint32_t& cid) {
        if (destination.kind == TokenKind::Number) {
            if (destination.number < 0 || destination.number > 0xFFFFFFFFll) return false;
            cid = static_cast<uint32_t>(destination.number);
            return true;
        }
        return destination.kind == TokenKind::String && CodeOf(destination.text, cid);
    }
    void Push(std::vector<CidRange>& ranges, uint32_t low, uint32_t high, uint32_t cid) {
        mapped_ += high - low + 1;
        ranges.push_back(CidRange{low, high, cid});
    }

    EncodingCMap& map_;
    uint64_t mapped_ = 0;
};

bool InRange(const CodespaceRange& range, std::string_view bytes, size_t at) {
    for (uint8_t i = 0; i < range.bytes; ++i) {
        const unsigned char b = static_cast<unsigned char>(bytes[at + i]);
        if (b < range.low[i] || b > range.high[i]) return false;
    }
    return true;
}

void SortRanges(std::vector<CidRange>& ranges) {
    std::stable_sort(ranges.begin(), ranges.end(), [](const CidRange& a, const CidRange& b) { return a.low < b.low; });
}

const CidRange* FindRange(const std::vector<CidRange>& ranges, uint32_t code) {
    auto it = std::upper_bound(ranges.begin(), ranges.end(), code,
                               [](uint32_t c, const CidRange& r) { return c < r.low; });
    if (it == ranges.begin()) return nullptr;
    --it;
    return code <= it->high ? &*it : nullptr;
}

} // namespace

bool Codespace::Add(std::string_view low, std::string_view high) {
    if (low.size() != high.size() || low.empty() || low.size() > 4 || ranges.size() >= kMaxCodespaceRanges) return false;
    CodespaceRange range;
    range.bytes = static_cast<uint8_t>(low.size());
    for (size_t i = 0; i < low.size(); ++i) {
        range.low[i] = static_cast<uint8_t>(low[i]);
        range.high[i] = static_cast<uint8_t>(high[i]);
    }
    ranges.push_back(range);
    return true;
}

uint8_t Codespace::Read(std::string_view bytes, size_t at, uint32_t& code) const {
    const size_t remaining = bytes.size() - at;
    const auto value = [&](size_t n) {
        uint32_t v = 0;
        for (size_t i = 0; i < n; ++i) v = v << 8 | static_cast<unsigned char>(bytes[at + i]);
        return v;
    };
    if (ranges.empty()) {
        const size_t n = remaining < 2 ? remaining : 2;
        code = value(n);
        return static_cast<uint8_t>(n);
    }
    for (uint8_t n = 1; n <= 4 && n <= remaining; ++n) {
        for (const CodespaceRange& range : ranges) {
            if (range.bytes == n && InRange(range, bytes, at)) {
                code = value(n);
                return n;
            }
        }
    }
    const unsigned char first = static_cast<unsigned char>(bytes[at]);
    uint8_t byFirst = 5, shortest = 5;
    for (const CodespaceRange& range : ranges) {
        if (range.bytes < shortest) shortest = range.bytes;
        if (first >= range.low[0] && first <= range.high[0] && range.bytes < byFirst) byFirst = range.bytes;
    }
    size_t n = byFirst <= 4 ? byFirst : shortest;
    if (n > remaining) n = remaining;
    code = value(n);
    return static_cast<uint8_t>(n);
}

const UnicodeEntry* ToUnicodeMap::Find(uint32_t code) const {
    auto it = std::lower_bound(entries.begin(), entries.end(), code,
                               [](const UnicodeEntry& e, uint32_t c) { return e.code < c; });
    return it != entries.end() && it->code == code ? &*it : nullptr;
}

bool ParseToUnicode(std::string_view data, ToUnicodeMap& out) {
    out = ToUnicodeMap();
    UnicodeSink sink(out);
    Parse(data, sink);
    // Most maps list their codes in increasing order, each once: then they are kept as read.
    const auto before = [](const UnicodeEntry& a, const UnicodeEntry& b) { return a.code < b.code; };
    const auto unordered = [](const UnicodeEntry& a, const UnicodeEntry& b) { return a.code >= b.code; };
    if (std::adjacent_find(out.entries.begin(), out.entries.end(), unordered) != out.entries.end()) {
        std::stable_sort(out.entries.begin(), out.entries.end(), before);
        // One entry per code: the last definition.
        size_t kept = 0;
        for (size_t i = 0; i < out.entries.size(); ++i) {
            if (i + 1 < out.entries.size() && out.entries[i + 1].code == out.entries[i].code) continue;
            out.entries[kept++] = out.entries[i];
        }
        out.entries.resize(kept);
        out.runs.clear();
    }
    if (sink.Full()) out.truncated = true;
    return !out.entries.empty();
}

bool EncodingCMap::Cid(uint32_t code, uint32_t& cid) const {
    if (const CidRange* range = FindRange(cids, code)) {
        cid = range->cid + (code - range->low);
        return true;
    }
    if (const CidRange* range = FindRange(notdefs, code)) {
        cid = range->cid;
        return true;
    }
    cid = 0;
    return false;
}

bool ParseEncodingCMap(std::string_view data, EncodingCMap& out) {
    out = EncodingCMap();
    CidSink sink(out);
    Parse(data, sink);
    out.wmode = sink.wmode;
    out.useCMap = sink.useCMap;
    if (sink.Full()) out.truncated = true;
    SortRanges(out.cids);
    SortRanges(out.notdefs);
    return !out.codespace.Empty() || !out.cids.empty() || !out.useCMap.empty();
}

} // namespace RayoPdf::Text
