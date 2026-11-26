// SPDX-License-Identifier: Apache-2.0
#include "pdf/trees.h"

#include <algorithm>

namespace RayoPdf {

void NameTree::Read(const Document& document, const PdfValue& root) {
    pairs_.clear();
    if (!root.IsDict()) return;
    size_t budget = kMaxTreePairs;
    WalkTree(document, root, TreeKind::Name, kMaxTreeDepth, budget, [&](const PdfValue& key, const PdfValue& value) {
        budget--;
        if (key.IsString()) pairs_.emplace_back(key.String(), &value);
    });
    std::stable_sort(pairs_.begin(), pairs_.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
}

const PdfValue* NameTree::Find(std::string_view key) const {
    const auto at = std::lower_bound(pairs_.begin(), pairs_.end(), key,
                                     [](const auto& pair, std::string_view k) { return pair.first < k; });
    return at != pairs_.end() && at->first == key ? at->second : nullptr;
}

void ReadNumberTree(const Document& document, const PdfValue& root,
                    std::vector<std::pair<int64_t, const PdfValue*>>& out) {
    out.clear();
    if (!root.IsDict()) return;
    size_t budget = kMaxTreePairs;
    WalkTree(document, root, TreeKind::Number, kMaxTreeDepth, budget, [&](const PdfValue& key, const PdfValue& value) {
        budget--;
        if (key.type == PdfValue::Type::Int) out.emplace_back(key.integer, &value);
    });
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    out.erase(std::unique(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first == b.first; }),
              out.end());
}

int Destinations::PageOf(const PdfValue& destination) {
    const PdfValue* d = &document_.Resolve(destination);
    if (d->IsName()) {
        d = &document_.Get(document_.Get(document_.Catalog(), "Dests"), d->Name());
    } else if (d->IsString()) {
        if (!namesRead_) {
            namesRead_ = true;
            names_.Read(document_, document_.Get(document_.Get(document_.Catalog(), "Names"), "Dests"));
        }
        const PdfValue* named = names_.Find(d->String());
        if (!named) return -1;
        d = &document_.Resolve(*named);
    }
    // A named destination's value may be a dictionary holding it as /D.
    if (d->IsDict()) d = &document_.Get(*d, "D");
    const PdfSpan<PdfValue> items = d->Array();
    if (items.size() == 0 || !items[0].IsRef()) return -1;
    if (pages_.empty()) {
        const int count = document_.PageCount();
        pages_.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) pages_.emplace_back(reinterpret_cast<uintptr_t>(&document_.PageDict(i)), i);
        // By address, then index: a page dictionary listed twice leads to its first place.
        std::sort(pages_.begin(), pages_.end());
    }
    const PdfValue* page = &document_.Resolve(items[0]);
    const uintptr_t key = reinterpret_cast<uintptr_t>(page);
    const auto at = std::lower_bound(pages_.begin(), pages_.end(), std::make_pair(key, -1));
    return at != pages_.end() && at->first == key && page->IsDict() ? at->second : -1;
}

const PdfValue* Destinations::TargetOf(const PdfValue& item) const {
    if (const PdfValue* dest = item.Find("Dest")) return dest;
    const PdfValue& action = document_.Get(item, "A");
    if (!document_.Get(action, "S").IsName("GoTo")) return nullptr;
    return action.Find("D");
}

} // namespace RayoPdf
