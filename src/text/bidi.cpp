// SPDX-License-Identifier: Apache-2.0
// UAX #9 for one line of visual input (algorithms §4.4). A PDF places right-to-left text glyph by
// glyph from left to right on the page, so a line arrives in visual order and its logical order
// has to be recovered. The forward algorithm (resolve levels on logical text, then reorder with
// L2) is not invertible in general; this file runs the resolution rules on an approximation of
// the logical sequence and then undoes L2:
//   - Paragraph direction (P2/P3 adapted), unless the caller forces one: right-to-left when the
//     line's R and AL items outnumber its L items; on a tie, the first strong item read from the
//     right decides (in a right-to-left paragraph the rightmost item is the logical first).
//   - The rules W1-W7, N1-N2, I1-I2 and L1 run over the items in processing order: the visual
//     order for a left-to-right paragraph, the reversed visual order for a right-to-left one
//     (exact for right-to-left runs; left-to-right runs inside it come out reversed, which only
//     the "previous strong" searches of W2 and W7 notice).
//   - W2 and W7 therefore take the governing strong type of a number from the geometry: in a
//     left-to-right paragraph the nearest strong item to its left; in a right-to-left paragraph an
//     L item to its left when the nearest strong item there is L (the number continues a
//     left-to-right run: "Windows 10"), else the nearest strong item to its right (the logical
//     previous one), else the paragraph direction.
//   - No explicit embeddings: LRE, RLE, LRO, RLO and PDF count as BN, the isolates LRI, RLI, FSI
//     and PDI as ON (X1-X10 do not run).
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

// Working types: the resolved classes use Class values; kNone marks "no strong item".
constexpr uint8_t kNone = 0xFF;

uint8_t U(Class c) {
    return static_cast<uint8_t>(c);
}

bool IsStrong(Class c) {
    return c == Class::L || c == Class::R || c == Class::AL;
}

// Neutral or isolate after the class mapping: what N1 and N2 resolve.
bool IsNeutral(uint8_t t) {
    return t == U(Class::BN) || t == U(Class::B) || t == U(Class::S) || t == U(Class::WS) || t == U(Class::ON);
}

// L1 resets these at the logical end of the line.
bool IsTrailingKind(Class c) {
    switch (c) {
    case Class::WS: case Class::BN: case Class::S: case Class::B:
    case Class::LRE: case Class::LRO: case Class::RLE: case Class::RLO: case Class::PDF:
    case Class::LRI: case Class::RLI: case Class::FSI: case Class::PDI:
        return true;
    default:
        return false;
    }
}

// Strong direction for N1: L, or R for R, AN and EN; kNone for anything else.
uint8_t NeutralContext(uint8_t t) {
    if (t == U(Class::L)) return U(Class::L);
    if (t == U(Class::R) || t == U(Class::AN) || t == U(Class::EN)) return U(Class::R);
    return kNone;
}

} // namespace

bool VisualToLogical(const Class* classes, size_t count, std::vector<uint32_t>& logical, std::vector<uint8_t>* levels,
                     int direction) {
    logical.resize(count);
    for (size_t i = 0; i < count; ++i) logical[i] = static_cast<uint32_t>(i);
    if (levels) levels->assign(count, 0);

    size_t left = 0, right = 0;
    bool arabicNumber = false;
    for (size_t i = 0; i < count; ++i) {
        if (classes[i] == Class::L) ++left;
        else if (classes[i] == Class::R || classes[i] == Class::AL) ++right;
        else if (classes[i] == Class::AN) arabicNumber = true;
    }
    if (right == 0 && !arabicNumber) return false;
    bool rtl = direction == 1 || (direction != 0 && right > left);
    if (direction < 0 && right == left) {
        for (size_t i = count; i-- > 0;) {
            if (IsStrong(classes[i])) {
                rtl = classes[i] != Class::L;
                break;
            }
        }
    }
    const uint8_t embedding = rtl ? U(Class::R) : U(Class::L);
    const uint8_t baseLevel = rtl ? 1 : 0;

    // Scratch, reused per thread: resolved types in processing order, nearest strong classes to
    // the left and right of each visual item, and levels in visual order.
    thread_local std::vector<uint8_t> scratch;
    scratch.resize(count * 4);
    uint8_t* type = scratch.data();
    uint8_t* strongLeft = type + count;
    uint8_t* strongRight = strongLeft + count;
    uint8_t* level = strongRight + count;

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
    // The visual index of processing position i.
    const auto visual = [&](size_t i) { return rtl ? count - 1 - i : i; };
    // W2/W7: the strong type governing a number at visual index v.
    const auto governing = [&](size_t v) -> uint8_t {
        if (!rtl) return strongLeft[v] != kNone ? strongLeft[v] : U(Class::L);
        if (strongLeft[v] == U(Class::L)) return U(Class::L);
        return strongRight[v] != kNone ? strongRight[v] : U(Class::R);
    };

    // Class mapping (no explicit embeddings, see above).
    for (size_t i = 0; i < count; ++i) {
        const Class c = classes[visual(i)];
        switch (c) {
        case Class::LRE: case Class::LRO: case Class::RLE: case Class::RLO: case Class::PDF:
            type[i] = U(Class::BN);
            break;
        case Class::LRI: case Class::RLI: case Class::FSI: case Class::PDI:
            type[i] = U(Class::ON);
            break;
        default:
            type[i] = U(c);
            break;
        }
    }
    // W1: a nonspacing mark takes the type of the item before it (glyph order of marks and bases
    // is arbitrary in a PDF, so the item after it when nothing precedes), else of the paragraph.
    for (size_t i = 0; i < count; ++i) {
        if (type[i] != U(Class::NSM)) continue;
        uint8_t from = kNone;
        for (size_t j = i; j-- > 0;) {
            if (type[j] != U(Class::NSM) && type[j] != U(Class::BN)) {
                from = type[j];
                break;
            }
        }
        if (from == kNone) {
            for (size_t j = i + 1; j < count; ++j) {
                if (type[j] != U(Class::NSM) && type[j] != U(Class::BN)) {
                    from = type[j];
                    break;
                }
            }
        }
        type[i] = from == kNone ? embedding : from;
    }
    // W2, W3.
    for (size_t i = 0; i < count; ++i) {
        if (type[i] == U(Class::EN) && governing(visual(i)) == U(Class::AL)) type[i] = U(Class::AN);
    }
    for (size_t i = 0; i < count; ++i) {
        if (type[i] == U(Class::AL)) type[i] = U(Class::R);
    }
    // W4: a single separator between two numbers of the same type.
    for (size_t i = 1; i + 1 < count; ++i) {
        const uint8_t before = type[i - 1], after = type[i + 1];
        if (type[i] == U(Class::ES) && before == U(Class::EN) && after == U(Class::EN)) type[i] = U(Class::EN);
        else if (type[i] == U(Class::CS) && before == after && (before == U(Class::EN) || before == U(Class::AN)))
            type[i] = before;
    }
    // W5: terminators next to a European number.
    for (size_t i = 0; i < count;) {
        if (type[i] != U(Class::ET)) {
            ++i;
            continue;
        }
        size_t end = i;
        while (end < count && type[end] == U(Class::ET)) ++end;
        const bool number = (i > 0 && type[i - 1] == U(Class::EN)) || (end < count && type[end] == U(Class::EN));
        if (number) {
            for (size_t j = i; j < end; ++j) type[j] = U(Class::EN);
        }
        i = end;
    }
    // W6, W7.
    for (size_t i = 0; i < count; ++i) {
        if (type[i] == U(Class::ES) || type[i] == U(Class::ET) || type[i] == U(Class::CS)) type[i] = U(Class::ON);
        else if (type[i] == U(Class::EN) && governing(visual(i)) == U(Class::L)) type[i] = U(Class::L);
    }
    // N1, N2: a run of neutrals between two items of one direction takes it, else the paragraph's.
    for (size_t i = 0; i < count;) {
        if (!IsNeutral(type[i])) {
            ++i;
            continue;
        }
        size_t end = i;
        while (end < count && IsNeutral(type[end])) ++end;
        const uint8_t before = i > 0 ? NeutralContext(type[i - 1]) : embedding;
        const uint8_t after = end < count ? NeutralContext(type[end]) : embedding;
        const uint8_t resolved = before == after && before != kNone ? before : embedding;
        for (size_t j = i; j < end; ++j) type[j] = resolved;
        i = end;
    }
    // I1, I2 into visual order.
    uint8_t highest = baseLevel;
    for (size_t i = 0; i < count; ++i) {
        uint8_t l = baseLevel;
        const uint8_t t = type[i];
        if (baseLevel == 0) {
            if (t == U(Class::R)) l = 1;
            else if (t == U(Class::AN) || t == U(Class::EN)) l = 2;
        } else if (t == U(Class::L) || t == U(Class::AN) || t == U(Class::EN)) {
            l = 2;
        }
        level[visual(i)] = l;
        highest = l > highest ? l : highest;
    }
    // L1: separators, and whitespace before them or at the logical end, take the paragraph level.
    bool trailing = true;
    for (size_t i = count; i-- > 0;) {
        const Class c = classes[visual(i)];
        if (c == Class::S || c == Class::B) {
            level[visual(i)] = baseLevel;
            trailing = true;
        } else if (trailing && IsTrailingKind(c)) {
            level[visual(i)] = baseLevel;
        } else {
            trailing = false;
        }
    }
    if (levels) levels->assign(level, level + count);

    // Undo L2 on a copy of the levels that moves with the items.
    uint8_t* moving = type;  // the types are no longer needed
    std::copy(level, level + count, moving);
    const uint8_t lowestOdd = 1;
    for (uint8_t k = highest; k >= lowestOdd; --k) {
        for (size_t i = 0; i < count;) {
            if (moving[i] < k) {
                ++i;
                continue;
            }
            size_t end = i;
            while (end < count && moving[end] >= k) ++end;
            const auto first = logical.begin();
            std::reverse(first + static_cast<std::ptrdiff_t>(i), first + static_cast<std::ptrdiff_t>(end));
            std::reverse(moving + i, moving + end);
            i = end;
        }
    }
    return rtl;
}

} // namespace RayoPdf::Text::Bidi
