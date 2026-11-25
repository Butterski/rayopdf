// SPDX-License-Identifier: Apache-2.0
#include "layout/features.h"

#include <cstddef>

namespace RayoPdf::Layout {

void TextShares(std::string_view text, float& digitShare, float& capitalShare, bool& endsInDigit) {
    size_t bytes = 0, digits = 0, letters = 0, capitals = 0;
    char last = 0;
    for (const char c : text) {
        if (c == ' ') continue;
        bytes++;
        last = c;
        if (c >= '0' && c <= '9') digits++;
        if (c >= 'A' && c <= 'Z') letters++, capitals++;
        if (c >= 'a' && c <= 'z') letters++;
    }
    digitShare = bytes ? float(digits) / float(bytes) : 0;
    capitalShare = letters ? float(capitals) / float(letters) : 0;
    endsInDigit = last >= '0' && last <= '9';
}

void TextWords(std::string_view text, float& words, float& lowerShare, bool& endsSentence) {
    size_t count = 0, lettered = 0, lower = 0;
    // The last word's letters and digits (a UTF-8 lead byte counts as a letter) and its last byte.
    size_t letters = 0, digits = 0;
    char last = 0;
    bool inWord = false;
    for (const char c : text) {
        if (c == ' ') {
            inWord = false;
            continue;
        }
        if (!inWord) {
            inWord = true;
            count++;
            letters = digits = 0;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) lettered++, lower += c >= 'a';
        }
        const unsigned char u = static_cast<unsigned char>(c);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || u >= 0xC0) letters++;
        if (c >= '0' && c <= '9') digits++;
        last = c;
    }
    words = float(count);
    lowerShare = lettered ? float(lower) / float(lettered) : 0;
    endsSentence = (last == '.' || last == '?' || last == '!') && letters >= 1 && letters + digits >= 2;
}

} // namespace RayoPdf::Layout
