// SPDX-License-Identifier: Apache-2.0
// The Unicode Bidirectional Algorithm (UAX #9, algorithms §4.4). One resolver implements the
// paragraph's rules for text in logical order: P2-P3, X1-X10 (explicit embeddings, overrides and
// isolates over a directional status stack of depth 125, isolating run sequences), W1-W7, N0
// (paired brackets, BD16 with a 63-entry stack), N1-N2, I1-I2 and L1; ResolveLevels exposes it and
// is checked against the Unicode BidiCharacterTest data.
//
// A PDF places right-to-left text glyph by glyph from left to right on the page, so a line arrives
// in visual order and its logical order has to be recovered. The forward algorithm is not
// invertible in general; VisualToLogical runs the resolver on an approximation of the logical
// sequence and then undoes L2:
//   - Paragraph direction (P2/P3 adapted), unless the caller forces one: right-to-left when the
//     line's R and AL items outnumber its L items; on a tie, the first strong item read from the
//     right decides (in a right-to-left paragraph the rightmost item is the logical first).
//   - Processing order: the visual order for a left-to-right paragraph, the reversed visual order
//     for a right-to-left one (exact for right-to-left runs; left-to-right runs inside it come out
//     reversed, which only the "previous strong" searches of W2 and W7 notice).
//   - W2 and W7 therefore take the governing strong type of a number at the paragraph's own level
//     from the geometry: in a left-to-right paragraph the nearest strong item to its left; in a
//     right-to-left paragraph an L item to its left when the nearest strong item there is L (the
//     number continues a left-to-right run: "Windows 10"), else the nearest strong item to its
//     right (the logical previous one), else the paragraph direction.
//   - Explicit formatting characters keep their meaning, but a closing PDF or PDI that comes before
//     any opener in processing order (the reversed text of a right-to-left line) trades places with
//     the next opener, so that each pair encloses what it enclosed in the logical text.
//   - Brackets: in a right-to-left paragraph the page shows the mirrored glyph of a bracket at an
//     odd level, and the reversed order turns the pairs of the even levels around; both read right
//     when every bracket is mirrored before N0 pairs them.
//   - Undoing L2: reversing, from the highest level down to the lowest odd level, every run of
//     items at that level or higher maps visual order back to logical order (each pass reverses
//     nested intervals, so the sequence of passes is its own inverse).

#include "text/bidi.h"

#include "text/bidi_tables.h"

#include <algorithm>

namespace RayoPdf::Text::Bidi {

Class ClassOf(uint32_t codePoint) {
    if (codePoint < 128) return static_cast<Class>(kAsciiClasses[codePoint]);
    if (codePoint > 0x10FFFF) return Class::L;
    // The last range starting at or before the code point.
    const uint32_t key = codePoint << 5 | 31u;
    const uint32_t* end = kClassRanges + kClassRangeCount;
    const uint32_t* it = std::upper_bound(kClassRanges, end, key);
    return static_cast<Class>(it[-1] & 31u);
}

uint32_t Mirror(uint32_t codePoint) {
    size_t lo = 0, hi = kMirrorCount;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (kMirrorPairs[mid][0] < codePoint) lo = mid + 1;
        else hi = mid;
    }
    return lo < kMirrorCount && kMirrorPairs[lo][0] == codePoint ? kMirrorPairs[lo][1] : codePoint;
}

namespace {

constexpr uint8_t kNone = 0xFF;
constexpr uint8_t kMaxDepth = 125;
// BD16's stack of opening brackets.
constexpr size_t kMaxBrackets = 63;
constexpr uint32_t kNoMatch = UINT32_MAX;

uint8_t U(Class c) {
    return static_cast<uint8_t>(c);
}

bool IsStrong(Class c) {
    return c == Class::L || c == Class::R || c == Class::AL;
}

bool IsRemoved(Class c) {
    return c == Class::LRE || c == Class::RLE || c == Class::LRO || c == Class::RLO || c == Class::PDF ||
           c == Class::BN;
}

bool IsIsolateInitiator(Class c) {
    return c == Class::LRI || c == Class::RLI || c == Class::FSI;
}

// What N1 and N2 resolve: neutrals and isolate formatting characters.
bool IsNeutral(uint8_t t) {
    return t == U(Class::B) || t == U(Class::S) || t == U(Class::WS) || t == U(Class::ON) || t == U(Class::LRI) ||
           t == U(Class::RLI) || t == U(Class::FSI) || t == U(Class::PDI);
}

// L1 resets these before a separator and at the end of the line.
bool IsTrailingKind(Class c) {
    return c == Class::WS || IsIsolateInitiator(c) || c == Class::PDI || IsRemoved(c);
}

// Strong direction for N0-N2: L, or R for R, AN and EN; kNone for anything else.
uint8_t StrongDirection(uint8_t t) {
    if (t == U(Class::L)) return U(Class::L);
    if (t == U(Class::R) || t == U(Class::AN) || t == U(Class::EN)) return U(Class::R);
    return kNone;
}

uint8_t DirectionOf(uint8_t level) {
    return (level & 1) ? U(Class::R) : U(Class::L);
}

// Bidi_Paired_Bracket of `cp` (0 when it is none) and whether it opens. U+2329/U+232A count as
// their canonical equivalents U+3008/U+3009 (BD16).
uint32_t Bracket(uint32_t cp, bool& opening) {
    size_t lo = 0, hi = kBracketCount;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (kBrackets[mid][0] < cp) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= kBracketCount || kBrackets[lo][0] != cp) return 0;
    opening = (kBrackets[lo][1] >> 24) != 0;
    return kBrackets[lo][1] & 0xFFFFFF;
}

uint32_t CanonicalBracket(uint32_t cp) {
    if (cp == 0x2329) return 0x3008;
    if (cp == 0x232A) return 0x3009;
    return cp;
}

// The least level above `level` that is odd (right-to-left) or even (left-to-right): X2-X5c.
uint8_t NextLevel(uint8_t level, bool rtl) {
    return rtl ? static_cast<uint8_t>((level + 1) | 1) : static_cast<uint8_t>((level + 2) & ~1);
}

// Scratch reused per thread; every array has one entry per item.
struct Work {
    std::vector<uint8_t> types;        // current bidi type
    std::vector<uint8_t> levels;       // explicit, then resolved level
    std::vector<uint32_t> matching;    // matching PDI of an isolate initiator, opener of a PDI
    std::vector<uint32_t> runOf;       // level run of each kept item
    std::vector<uint32_t> runStart;    // per level run: first index into `kept`
    std::vector<uint32_t> kept;        // the items X9 keeps, in order
    std::vector<uint32_t> sequence;    // one isolating run sequence (item indices)
    std::vector<uint8_t> runUsed;
    std::vector<uint32_t> openers;     // BD16 stack: positions in `sequence`
    std::vector<uint32_t> openerBrackets;
    std::vector<uint64_t> pairs;       // opener position << 32 | closer position
};

// The paragraph level P2-P3 give items [from, to) (isolates skipped through their matching PDI);
// `fallback` when no strong item is found.
uint8_t FirstStrongLevel(const Class* classes, const std::vector<uint32_t>& matching, size_t from, size_t to,
                         uint8_t fallback) {
    for (size_t i = from; i < to; ++i) {
        const Class c = classes[i];
        if (c == Class::L) return 0;
        if (c == Class::R || c == Class::AL) return 1;
        if (IsIsolateInitiator(c)) {
            if (matching[i] == kNoMatch) return fallback;  // to the end of the paragraph
            i = matching[i];
        }
    }
    return fallback;
}

// Per item: whether `governing` holds the W2/W7 strong type for it (visual input), else kNone.
struct Options {
    const uint8_t* governing = nullptr;
};

void ResolveSequence(const Class* classes, const uint32_t* codePoints, Work& w, uint8_t sos, uint8_t eos,
                     uint8_t paragraphLevel, const Options& options) {
    const std::vector<uint32_t>& seq = w.sequence;
    const size_t n = seq.size();
    uint8_t* t = w.types.data();
    const uint8_t level = w.levels[seq[0]];
    const uint8_t embedding = DirectionOf(level);
    const bool outermost = options.governing && level == paragraphLevel;

    // W1.
    for (size_t k = 0; k < n; ++k) {
        if (t[seq[k]] != U(Class::NSM)) continue;
        if (k == 0) {
            t[seq[k]] = sos;
        } else {
            const Class previous = classes[seq[k - 1]];
            t[seq[k]] = IsIsolateInitiator(previous) || previous == Class::PDI ? U(Class::ON) : t[seq[k - 1]];
        }
    }
    // W2, W3.
    uint8_t lastStrong = sos;
    for (size_t k = 0; k < n; ++k) {
        uint8_t& type = t[seq[k]];
        if (type == U(Class::L) || type == U(Class::R) || type == U(Class::AL)) lastStrong = type;
        if (type == U(Class::EN)) {
            const uint8_t given = outermost ? options.governing[seq[k]] : kNone;
            const uint8_t strong = given != kNone ? given : lastStrong;
            if (strong == U(Class::AL)) type = U(Class::AN);
        }
    }
    for (size_t k = 0; k < n; ++k) {
        if (t[seq[k]] == U(Class::AL)) t[seq[k]] = U(Class::R);
    }
    // W4.
    for (size_t k = 1; k + 1 < n; ++k) {
        uint8_t& type = t[seq[k]];
        const uint8_t before = t[seq[k - 1]], after = t[seq[k + 1]];
        if (type == U(Class::ES) && before == U(Class::EN) && after == U(Class::EN)) type = U(Class::EN);
        else if (type == U(Class::CS) && before == after && (before == U(Class::EN) || before == U(Class::AN)))
            type = before;
    }
    // W5.
    for (size_t k = 0; k < n;) {
        if (t[seq[k]] != U(Class::ET)) {
            ++k;
            continue;
        }
        size_t end = k;
        while (end < n && t[seq[end]] == U(Class::ET)) ++end;
        const bool number = (k > 0 && t[seq[k - 1]] == U(Class::EN)) || (end < n && t[seq[end]] == U(Class::EN));
        if (number) {
            for (size_t j = k; j < end; ++j) t[seq[j]] = U(Class::EN);
        }
        k = end;
    }
    // W6, W7.
    lastStrong = sos;
    for (size_t k = 0; k < n; ++k) {
        uint8_t& type = t[seq[k]];
        if (type == U(Class::ES) || type == U(Class::ET) || type == U(Class::CS)) type = U(Class::ON);
        if (type == U(Class::L) || type == U(Class::R)) lastStrong = type;
        if (type == U(Class::EN)) {
            uint8_t strong = lastStrong;
            if (outermost && options.governing[seq[k]] != kNone) {
                strong = options.governing[seq[k]] == U(Class::L) ? U(Class::L) : U(Class::R);
            }
            if (strong == U(Class::L)) type = U(Class::L);
        }
    }

    // N0: paired brackets (BD16), resolved in order of their opening brackets.
    if (codePoints) {
        w.openers.clear();
        w.openerBrackets.clear();
        w.pairs.clear();
        for (size_t k = 0; k < n; ++k) {
            const uint32_t i = seq[k];
            if (t[i] != U(Class::ON)) continue;
            bool opening = false;
            const uint32_t pair = Bracket(codePoints[i], opening);
            if (pair == 0) continue;
            if (opening) {
                if (w.openers.size() == kMaxBrackets) break;
                w.openers.push_back(static_cast<uint32_t>(k));
                w.openerBrackets.push_back(CanonicalBracket(pair));
                continue;
            }
            const uint32_t self = CanonicalBracket(codePoints[i]);
            for (size_t d = w.openers.size(); d-- > 0;) {
                if (w.openerBrackets[d] != self) continue;
                w.pairs.push_back(static_cast<uint64_t>(w.openers[d]) << 32 | k);
                w.openers.resize(d);
                w.openerBrackets.resize(d);
                break;
            }
        }
        std::sort(w.pairs.begin(), w.pairs.end());
        for (const uint64_t pair : w.pairs) {
            const size_t open = static_cast<size_t>(pair >> 32), close = static_cast<size_t>(pair & 0xFFFFFFFF);
            bool same = false, opposite = false;
            for (size_t k = open + 1; k < close; ++k) {
                const uint8_t d = StrongDirection(t[seq[k]]);
                if (d == embedding) same = true;
                else if (d != kNone) opposite = true;
            }
            uint8_t resolved = kNone;
            if (same) {
                resolved = embedding;
            } else if (opposite) {
                uint8_t context = sos;
                for (size_t k = open; k-- > 0;) {
                    const uint8_t d = StrongDirection(t[seq[k]]);
                    if (d != kNone) {
                        context = d;
                        break;
                    }
                }
                resolved = context != embedding ? context : embedding;
            }
            if (resolved == kNone) continue;
            for (const size_t at : {open, close}) {
                t[seq[at]] = resolved;
                // Nonspacing marks after a bracket take its new type (they were ON through W1).
                for (size_t k = at + 1; k < n && classes[seq[k]] == Class::NSM; ++k) t[seq[k]] = resolved;
            }
        }
    }

    // N1, N2.
    for (size_t k = 0; k < n;) {
        if (!IsNeutral(t[seq[k]])) {
            ++k;
            continue;
        }
        size_t end = k;
        while (end < n && IsNeutral(t[seq[end]])) ++end;
        const uint8_t before = k > 0 ? StrongDirection(t[seq[k - 1]]) : sos;
        const uint8_t after = end < n ? StrongDirection(t[seq[end]]) : eos;
        const uint8_t resolved = before == after && before != kNone ? before : embedding;
        for (size_t j = k; j < end; ++j) t[seq[j]] = resolved;
        k = end;
    }
}

// The resolver: `classes` in logical order, the paragraph level decided. Fills w.levels; removed
// items get the level of the embedding they sit in.
void Resolve(const Class* classes, const uint32_t* codePoints, size_t count, uint8_t paragraphLevel, Work& w,
             const Options& options) {
    w.types.resize(count);
    w.levels.resize(count);
    w.matching.assign(count, kNoMatch);
    for (size_t i = 0; i < count; ++i) w.types[i] = U(classes[i]);

    // BD9: matching isolate initiators and PDIs.
    w.openers.clear();
    for (size_t i = 0; i < count; ++i) {
        if (IsIsolateInitiator(classes[i])) {
            w.openers.push_back(static_cast<uint32_t>(i));
        } else if (classes[i] == Class::PDI && !w.openers.empty()) {
            w.matching[w.openers.back()] = static_cast<uint32_t>(i);
            w.matching[i] = w.openers.back();
            w.openers.pop_back();
        } else if (classes[i] == Class::B) {
            w.openers.clear();
        }
    }

    // X1-X8: the directional status stack.
    struct Status {
        uint8_t level;
        uint8_t override;  // kNone, L or R
        bool isolate;
    };
    Status stack[kMaxDepth + 2];
    size_t depth = 0;
    stack[depth++] = Status{paragraphLevel, kNone, false};
    size_t overflowIsolates = 0, overflowEmbeddings = 0, validIsolates = 0;
    for (size_t i = 0; i < count; ++i) {
        const Class c = classes[i];
        const Status& top = stack[depth - 1];
        switch (c) {
        case Class::RLE: case Class::LRE: case Class::RLO: case Class::LRO: {
            w.levels[i] = top.level;
            const bool rtl = c == Class::RLE || c == Class::RLO;
            const uint8_t next = NextLevel(top.level, rtl);
            if (next <= kMaxDepth && overflowIsolates == 0 && overflowEmbeddings == 0) {
                const uint8_t override = c == Class::RLO ? U(Class::R) : c == Class::LRO ? U(Class::L) : kNone;
                stack[depth++] = Status{next, override, false};
            } else if (overflowIsolates == 0) {
                ++overflowEmbeddings;
            }
            break;
        }
        case Class::RLI: case Class::LRI: case Class::FSI: {
            w.levels[i] = top.level;
            if (top.override != kNone) w.types[i] = top.override;
            bool rtl = c == Class::RLI;
            if (c == Class::FSI) {
                const size_t end = w.matching[i] == kNoMatch ? count : w.matching[i];
                rtl = FirstStrongLevel(classes, w.matching, i + 1, end, 0) == 1;
            }
            const uint8_t next = NextLevel(top.level, rtl);
            if (next <= kMaxDepth && overflowIsolates == 0 && overflowEmbeddings == 0) {
                ++validIsolates;
                stack[depth++] = Status{next, kNone, true};
            } else {
                ++overflowIsolates;
            }
            break;
        }
        case Class::PDI:
            if (overflowIsolates > 0) {
                --overflowIsolates;
            } else if (validIsolates > 0) {
                overflowEmbeddings = 0;
                while (!stack[depth - 1].isolate && depth > 1) --depth;
                if (depth > 1) --depth;
                --validIsolates;
            }
            w.levels[i] = stack[depth - 1].level;
            if (stack[depth - 1].override != kNone) w.types[i] = stack[depth - 1].override;
            break;
        case Class::PDF:
            w.levels[i] = top.level;
            if (overflowIsolates > 0) {
            } else if (overflowEmbeddings > 0) {
                --overflowEmbeddings;
            } else if (!top.isolate && depth >= 2) {
                --depth;
            }
            break;
        case Class::B:
            w.levels[i] = paragraphLevel;
            break;
        case Class::BN:
            w.levels[i] = top.level;
            break;
        default:
            w.levels[i] = top.level;
            if (top.override != kNone) w.types[i] = top.override;
            break;
        }
    }

    // X9, X10: level runs of the kept items, linked into isolating run sequences.
    w.kept.clear();
    for (size_t i = 0; i < count; ++i) {
        if (!IsRemoved(classes[i])) w.kept.push_back(static_cast<uint32_t>(i));
    }
    w.runOf.assign(count, kNoMatch);
    w.runStart.clear();
    for (size_t k = 0; k < w.kept.size(); ++k) {
        if (k == 0 || w.levels[w.kept[k]] != w.levels[w.kept[k - 1]]) w.runStart.push_back(static_cast<uint32_t>(k));
        w.runOf[w.kept[k]] = static_cast<uint32_t>(w.runStart.size() - 1);
    }
    const size_t runs = w.runStart.size();
    w.runUsed.assign(runs, 0);
    const auto runEnd = [&](size_t r) {
        return r + 1 < runs ? w.runStart[r + 1] : static_cast<uint32_t>(w.kept.size());
    };
    for (size_t r = 0; r < runs; ++r) {
        if (w.runUsed[r]) continue;
        const uint32_t first = w.kept[w.runStart[r]];
        // A run starting with a matched PDI continues its initiator's sequence.
        if (classes[first] == Class::PDI && w.matching[first] != kNoMatch) continue;
        w.sequence.clear();
        size_t run = r;
        for (;;) {
            w.runUsed[run] = 1;
            for (uint32_t k = w.runStart[run]; k < runEnd(run); ++k) w.sequence.push_back(w.kept[k]);
            const uint32_t last = w.sequence.back();
            if (!IsIsolateInitiator(classes[last]) || w.matching[last] == kNoMatch) break;
            const uint32_t next = w.runOf[w.matching[last]];
            if (next == kNoMatch || w.runUsed[next]) break;
            run = next;
        }
        const uint32_t first0 = w.sequence.front(), last = w.sequence.back();
        const uint8_t level = w.levels[first0];
        // sos/eos: the higher of the sequence's level and its neighbour's (removed items skipped).
        // (A sequence can only end with an isolate initiator that has no matching PDI: eos then
        // compares with the paragraph level.)
        uint8_t before = paragraphLevel, after = paragraphLevel;
        for (size_t i = first0; i-- > 0;) {
            if (!IsRemoved(classes[i])) {
                before = w.levels[i];
                break;
            }
        }
        if (!IsIsolateInitiator(classes[last])) {
            for (size_t i = last + 1; i < count; ++i) {
                if (!IsRemoved(classes[i])) {
                    after = w.levels[i];
                    break;
                }
            }
        }
        const uint8_t sos = DirectionOf(std::max(level, before));
        const uint8_t eos = DirectionOf(std::max(w.levels[last], after));
        ResolveSequence(classes, codePoints, w, sos, eos, paragraphLevel, options);
    }

    // I1, I2, after every sequence (sos and eos read the explicit levels of neighbours).
    for (const uint32_t i : w.kept) {
        uint8_t& l = w.levels[i];
        const uint8_t t = w.types[i];
        if (!(l & 1)) {
            if (t == U(Class::R)) l = static_cast<uint8_t>(l + 1);
            else if (t == U(Class::AN) || t == U(Class::EN)) l = static_cast<uint8_t>(l + 2);
        } else if (t == U(Class::L) || t == U(Class::AN) || t == U(Class::EN)) {
            l = static_cast<uint8_t>(l + 1);
        }
    }

    // L1: separators, and the whitespace and isolate formatting before them or at the line's end.
    bool trailing = true;
    for (size_t i = count; i-- > 0;) {
        const Class c = classes[i];
        if (c == Class::S || c == Class::B) {
            w.levels[i] = paragraphLevel;
            trailing = true;
        } else if (trailing && IsTrailingKind(c)) {
            w.levels[i] = paragraphLevel;
        } else {
            trailing = false;
        }
    }
}

thread_local Work tWork;

// Reverses, from the highest level down to the lowest odd one, every run at that level or higher
// (L2) in `order`, the levels moving with their items.
void ReverseRuns(std::vector<uint32_t>& order, std::vector<uint8_t>& moving) {
    const size_t count = order.size();
    uint8_t highest = 0, lowest = 0xFF;
    for (const uint8_t l : moving) {
        highest = std::max(highest, l);
        lowest = std::min(lowest, l);
    }
    const int lowestOdd = lowest | 1;
    for (int k = highest; k >= lowestOdd; --k) {
        for (size_t i = 0; i < count;) {
            if (moving[i] < k) {
                ++i;
                continue;
            }
            size_t end = i;
            while (end < count && moving[end] >= k) ++end;
            const auto first = order.begin();
            std::reverse(first + static_cast<std::ptrdiff_t>(i), first + static_cast<std::ptrdiff_t>(end));
            std::reverse(moving.begin() + static_cast<std::ptrdiff_t>(i),
                         moving.begin() + static_cast<std::ptrdiff_t>(end));
            i = end;
        }
    }
}

} // namespace

uint8_t ResolveLevels(const Class* classes, const uint32_t* codePoints, size_t count, int direction,
                      std::vector<uint8_t>& levels) {
    Work& w = tWork;
    w.matching.assign(count, kNoMatch);
    // BD9 is needed by P2-P3 before Resolve recomputes it.
    w.openers.clear();
    for (size_t i = 0; i < count; ++i) {
        if (IsIsolateInitiator(classes[i])) {
            w.openers.push_back(static_cast<uint32_t>(i));
        } else if (classes[i] == Class::PDI && !w.openers.empty()) {
            w.matching[w.openers.back()] = static_cast<uint32_t>(i);
            w.openers.pop_back();
        }
    }
    const uint8_t paragraphLevel =
        direction == 0 ? 0 : direction == 1 ? 1 : FirstStrongLevel(classes, w.matching, 0, count, 0);
    Resolve(classes, codePoints, count, paragraphLevel, w, Options{});
    levels.assign(w.levels.begin(), w.levels.begin() + static_cast<std::ptrdiff_t>(count));
    for (size_t i = 0; i < count; ++i) {
        if (IsRemoved(classes[i])) levels[i] = kRemovedLevel;
    }
    return paragraphLevel;
}

void LogicalToVisual(const uint8_t* levels, size_t count, std::vector<uint32_t>& visual) {
    visual.clear();
    std::vector<uint8_t> moving;
    for (size_t i = 0; i < count; ++i) {
        if (levels[i] == kRemovedLevel) continue;
        visual.push_back(static_cast<uint32_t>(i));
        moving.push_back(levels[i]);
    }
    ReverseRuns(visual, moving);
}

bool VisualToLogical(const Class* classes, size_t count, std::vector<uint32_t>& logical, std::vector<uint8_t>* levels,
                     int direction) {
    return VisualToLogical(classes, nullptr, count, logical, levels, direction);
}

bool VisualToLogical(const Class* classes, const uint32_t* codePoints, size_t count, std::vector<uint32_t>& logical,
                     std::vector<uint8_t>* levels, int direction) {
    logical.resize(count);
    for (size_t i = 0; i < count; ++i) logical[i] = static_cast<uint32_t>(i);
    if (levels) levels->assign(count, 0);

    size_t left = 0, right = 0;
    bool other = false;  // AN or an explicit formatting character
    for (size_t i = 0; i < count; ++i) {
        const Class c = classes[i];
        if (c == Class::L) ++left;
        else if (c == Class::R || c == Class::AL) ++right;
        else if (c == Class::AN || (c >= Class::LRE && c <= Class::PDI)) other = true;
    }
    if (right == 0 && !other) return false;
    bool rtl = direction == 1 || (direction != 0 && right > left);
    if (direction < 0 && right == left) {
        for (size_t i = count; i-- > 0;) {
            if (IsStrong(classes[i])) {
                rtl = classes[i] != Class::L;
                break;
            }
        }
    }

    // The items in processing order (see the top of the file): classes, code points (mirrored for
    // a right-to-left paragraph), and the geometric W2/W7 strong type of each.
    thread_local std::vector<Class> pClasses;
    thread_local std::vector<uint32_t> pCodePoints;
    thread_local std::vector<uint8_t> governing, strongLeft, strongRight, moving;
    pClasses.resize(count);
    pCodePoints.resize(codePoints ? count : 0);
    governing.resize(count);
    strongLeft.resize(count);
    strongRight.resize(count);
    const auto visual = [&](size_t i) { return rtl ? count - 1 - i : i; };
    for (size_t i = 0; i < count; ++i) {
        pClasses[i] = classes[visual(i)];
        if (codePoints) pCodePoints[i] = rtl ? Mirror(codePoints[visual(i)]) : codePoints[visual(i)];
    }
    // A closer before any opener of its kind trades places with the next opener: the innermost
    // pending closer with the first opener after it.
    thread_local std::vector<uint32_t> pending;
    for (const bool isolate : {false, true}) {
        size_t open = 0;
        pending.clear();
        for (size_t i = 0; i < count; ++i) {
            const Class c = pClasses[i];
            const bool opener = isolate ? IsIsolateInitiator(c)
                                        : (c == Class::LRE || c == Class::RLE || c == Class::LRO || c == Class::RLO);
            const bool closer = isolate ? c == Class::PDI : c == Class::PDF;
            if (opener && open == 0 && !pending.empty()) {
                std::swap(pClasses[pending.back()], pClasses[i]);
                if (codePoints) std::swap(pCodePoints[pending.back()], pCodePoints[i]);
                pending.pop_back();
            } else if (opener) {
                ++open;
            } else if (closer && open > 0) {
                --open;
            } else if (closer) {
                pending.push_back(static_cast<uint32_t>(i));
            }
        }
    }

    uint8_t last = kNone;
    for (size_t v = 0; v < count; ++v) {
        strongLeft[v] = last;
        if (IsStrong(classes[v])) last = U(classes[v]);
    }
    last = kNone;
    for (size_t v = count; v-- > 0;) {
        strongRight[v] = last;
        if (IsStrong(classes[v])) last = U(classes[v]);
    }
    for (size_t i = 0; i < count; ++i) {
        const size_t v = visual(i);
        uint8_t g;
        if (!rtl) g = strongLeft[v] != kNone ? strongLeft[v] : U(Class::L);
        else if (strongLeft[v] == U(Class::L)) g = U(Class::L);
        else g = strongRight[v] != kNone ? strongRight[v] : U(Class::R);
        governing[i] = g;
    }

    Work& w = tWork;
    Options options;
    options.governing = governing.data();
    const uint8_t paragraphLevel = rtl ? 1 : 0;
    Resolve(pClasses.data(), codePoints ? pCodePoints.data() : nullptr, count, paragraphLevel, w, options);

    // Levels in visual order; removed items take the level of the item before them in processing
    // order (they sit inside its run).
    moving.resize(count);
    uint8_t previous = paragraphLevel;
    for (size_t i = 0; i < count; ++i) {
        uint8_t l = w.levels[i];
        if (IsRemoved(pClasses[i])) l = previous;
        moving[visual(i)] = l;
        previous = l;
    }
    if (levels) levels->assign(moving.begin(), moving.end());
    ReverseRuns(logical, moving);
    return rtl;
}

} // namespace RayoPdf::Text::Bidi
