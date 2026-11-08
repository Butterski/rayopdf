// SPDX-License-Identifier: Apache-2.0
#include "layout/exact.h"

#include "common/utf8.h"
#include "pdf/rayomd_source.h"
#include "text/bidi.h"

#include <algorithm>
#include <vector>

namespace RayoPdf::Layout {

const char* ExactUseName(ExactUse use) {
    switch (use) {
    case ExactUse::Off: return "off";
    case ExactUse::None: return "none";
    case ExactUse::Used: return "used";
    case ExactUse::Pages: return "pages";
    case ExactUse::Unsupported: return "unsupported";
    case ExactUse::Corrupt: return "corrupt";
    case ExactUse::Integrity: return "integrity";
    case ExactUse::Limit: return "limit";
    case ExactUse::Utf8: return "utf8";
    case ExactUse::Mismatch: return "mismatch";
    }
    return "none";
}

namespace {

// The XMP namespace of the profile: a payload is claimed only by a file whose bytes name it.
constexpr std::string_view kProfileNamespace = "https://rayomd.dev/ns/source/";
constexpr std::string_view kOwnComment = "<!-- rayopdf:";
// Distinct letters and digits both sides may hold: ranks fit 16 bits, 0xFFFF left unused.
constexpr size_t kMaxRanks = 0xFFFF;

bool IsContent(uint32_t cp) {
    if (cp < 0x80) return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
    if ((cp >= 0xE000 && cp <= 0xF8FF) || cp >= 0xF0000) return false;  // private use
    switch (Text::Bidi::ClassOf(cp)) {
    case Text::Bidi::Class::L:
    case Text::Bidi::Class::R:
    case Text::Bidi::Class::AL:
    case Text::Bidi::Class::EN:
    case Text::Bidi::Class::AN: return true;
    default: return false;
    }
}

// The letters and digits of `text`, ASCII letters in lower case; with `skipOwnComments` the
// converter's `<!-- rayopdf: ... -->` comments left out.
void ContentOf(std::string_view text, bool skipOwnComments, std::vector<uint32_t>& out) {
    out.clear();
    size_t at = 0;
    while (at < text.size()) {
        if (skipOwnComments && text[at] == '<' && text.compare(at, kOwnComment.size(), kOwnComment) == 0) {
            const size_t end = text.find("-->", at + kOwnComment.size());
            at = end == std::string_view::npos ? text.size() : end + 3;
            continue;
        }
        uint32_t cp = 0;
        size_t length = 1;
        Utf8::Decode(text, at, cp, length);
        at += length;
        if (!IsContent(cp)) continue;
        if (cp >= 'A' && cp <= 'Z') cp += 'a' - 'A';
        out.push_back(cp);
    }
}

// Replaces each code point of `a` and `b` by its rank among the distinct code points of both, so
// that a window of kExactWindow ranks packs into a Key. False when there are more than
// kMaxRanks.
bool Rank(std::vector<uint32_t>& a, std::vector<uint32_t>& b) {
    std::vector<uint16_t> bmp(0x10000, 0);  // 1 for a code point seen, then its rank
    std::vector<uint32_t> astral;
    for (const std::vector<uint32_t>* side : {&a, &b}) {
        for (uint32_t cp : *side) {
            if (cp < 0x10000) {
                bmp[cp] = 1;
            } else {
                astral.push_back(cp);
            }
        }
    }
    std::sort(astral.begin(), astral.end());
    astral.erase(std::unique(astral.begin(), astral.end()), astral.end());
    size_t rank = 0;
    for (uint16_t& slot : bmp) {
        if (slot == 0) continue;
        if (rank >= kMaxRanks) return false;
        slot = static_cast<uint16_t>(rank++);
    }
    if (astral.size() > kMaxRanks - rank) return false;
    for (std::vector<uint32_t>* side : {&a, &b}) {
        for (uint32_t& cp : *side) {
            if (cp < 0x10000) {
                cp = bmp[cp];
            } else {
                cp = static_cast<uint32_t>(rank + static_cast<size_t>(std::lower_bound(astral.begin(), astral.end(), cp) -
                                                                      astral.begin()));
            }
        }
    }
    return true;
}

// A window of kExactWindow ranks, 16 bits each, in reading order: comparing keys compares the
// windows. Of the two readings (forward and backward) the lower, so that a window and its mirror
// image are the same key.
struct Key {
    uint64_t high = 0;
    uint64_t low = 0;

    bool operator<(const Key& other) const { return high != other.high ? high < other.high : low < other.low; }
    bool operator==(const Key& other) const { return high == other.high && low == other.low; }
};

static_assert(kExactWindow == 8, "a Key holds 8 ranks of 16 bits");

Key WindowKey(const uint32_t* ranks) {
    Key forward, backward;
    for (size_t j = 0; j < 4; ++j) {
        forward.high = forward.high << 16 | ranks[j];
        forward.low = forward.low << 16 | ranks[j + 4];
        backward.high = backward.high << 16 | ranks[7 - j];
        backward.low = backward.low << 16 | ranks[3 - j];
    }
    return backward < forward ? backward : forward;
}

// The windows of one side, in an open-addressing table with linear probing. No window is all
// ranks 0xFFFF (Rank keeps ranks below kMaxRanks), so that key marks an empty slot. A probe
// sequence past kMaxProbes fails closed: hostile text that piles keys into one run only makes the
// check fail.
class KeySet {
public:
    explicit KeySet(size_t count) {
        size_t capacity = 16;
        while (capacity < 2 * count) capacity *= 2;
        slots_.assign(capacity, kEmpty);
        mask_ = capacity - 1;
    }

    bool Insert(const Key& key) {
        for (size_t probe = 0, at = Hash(key) & mask_; probe < kMaxProbes; ++probe, at = (at + 1) & mask_) {
            if (slots_[at] == key) return true;
            if (slots_[at] == kEmpty) {
                slots_[at] = key;
                return true;
            }
        }
        return false;
    }

    bool Contains(const Key& key) const {
        for (size_t probe = 0, at = Hash(key) & mask_; probe < kMaxProbes; ++probe, at = (at + 1) & mask_) {
            if (slots_[at] == key) return true;
            if (slots_[at] == kEmpty) return false;
        }
        return false;
    }

private:
    static constexpr size_t kMaxProbes = 64;
    static constexpr Key kEmpty = {~uint64_t(0), ~uint64_t(0)};

    static size_t Hash(const Key& key) {
        uint64_t h = key.high * 0x9E3779B97F4A7C15ull ^ (key.low + 0x632BE59BD9B4E019ull) * 0xC2B2AE3D27D4EB4Full;
        h ^= h >> 29;
        return static_cast<size_t>(h);
    }

    std::vector<Key> slots_;
    size_t mask_ = 0;
};

// The ranks of `a` that no window of `b` covers: a window of `a` found among those of `b` covers
// its kExactWindow ranks. All of `a` when either side is shorter than a window, or when `b`'s
// windows overflow their table.
size_t Unmatched(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    if (a.size() < kExactWindow || b.size() < kExactWindow) return a.size();
    const size_t windows = b.size() - kExactWindow + 1;
    KeySet set(windows);
    for (size_t i = 0; i < windows; ++i) {
        if (!set.Insert(WindowKey(b.data() + i))) return a.size();
    }
    size_t covered = 0;
    size_t coveredEnd = 0;  // positions below are counted already
    for (size_t i = 0; i + kExactWindow <= a.size(); ++i) {
        if (!set.Contains(WindowKey(a.data() + i))) continue;
        const size_t from = std::max(i, coveredEnd);
        covered += i + kExactWindow - from;
        coveredEnd = i + kExactWindow;
    }
    return a.size() - covered;
}

bool WithinSlack(size_t unmatched, size_t content) {
    return unmatched <= kExactSlack + content / kExactShareDivisor;
}

} // namespace

ExactUse FindExactSource(const Document& document, std::string_view& source) {
    source = {};
    const std::string_view bytes = document.Bytes();
    if (!document.Catalog().Find("AF") || bytes.find(kProfileNamespace) == std::string_view::npos) {
        return ExactUse::None;
    }
    RayomdSource::Result result;
    RayomdSource::Read(bytes, result);
    switch (result.status) {
    case RayomdSource::Status::Ok: source = result.source; return ExactUse::Used;
    case RayomdSource::Status::UnsupportedProfile: return ExactUse::Unsupported;
    case RayomdSource::Status::IntegrityMismatch: return ExactUse::Integrity;
    case RayomdSource::Status::LimitExceeded: return ExactUse::Limit;
    case RayomdSource::Status::InvalidUtf8: return ExactUse::Utf8;
    case RayomdSource::Status::NotReversible:  // the namespace is there, the packet is not as written
    case RayomdSource::Status::CorruptPdf: break;
    }
    return ExactUse::Corrupt;
}

bool SourceMatchesPages(std::string_view source, std::string_view converted, ExactMatch& match) {
    match = ExactMatch();
    std::vector<uint32_t> payload, pages;
    ContentOf(source, false, payload);
    ContentOf(converted, true, pages);
    match.sourceContent = payload.size();
    match.pageContent = pages.size();
    if (!Rank(payload, pages)) {
        match.sourceUnmatched = payload.size();
        match.pageUnmatched = pages.size();
        return false;
    }
    match.sourceUnmatched = Unmatched(payload, pages);
    if (!WithinSlack(match.sourceUnmatched, match.sourceContent)) return false;
    match.pageUnmatched = Unmatched(pages, payload);
    return WithinSlack(match.pageUnmatched, match.pageContent);
}

} // namespace RayoPdf::Layout
