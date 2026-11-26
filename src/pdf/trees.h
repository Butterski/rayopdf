// SPDX-License-Identifier: Apache-2.0
// Name trees (ISO 32000-2 §7.9.6), number trees (§7.9.7) and destinations (§12.3.2): the catalog's
// named destinations, page labels and a structure tree's /ParentTree are read through them. Every
// walk is bounded: at most kMaxTreeDepth levels of /Kids and a budget of nodes and pairs, so a
// hostile tree (cyclic /Kids, millions of pairs) costs a bounded amount of work.
#pragma once

#include "pdf/document.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace RayoPdf {

constexpr uint32_t kMaxTreeDepth = 32;       // /Kids levels below a tree's root
constexpr size_t kMaxTreePairs = 1u << 20;   // nodes queued plus pairs read from one tree (Read)

enum class TreeKind : uint8_t { Name, Number };  // pairs in /Names (string keys) or /Nums (integer keys)

// Walks the tree at `root` (resolved): each node's pairs in their order, `visit(key, value)` with
// the key resolved and the value as the array holds it (a reference stays a reference), then its
// /Kids depth-first, the last kid first, at most `maxDepth` levels below the root. `budget` goes
// down by one for each kid queued, and by whatever `visit` takes from it; the walk stops when it
// is 0. No visited set: the depth and the budget bound a cyclic tree.
template <typename Visit>
void WalkTree(const Document& document, const PdfValue& root, TreeKind kind, uint32_t maxDepth, size_t& budget,
              Visit&& visit) {
    struct Level {
        const PdfValue* node;
        uint32_t depth;
    };
    const std::string_view pairsKey = kind == TreeKind::Name ? "Names" : "Nums";
    std::vector<Level> todo{{&root, 0}};
    while (!todo.empty() && budget > 0) {
        const Level level = todo.back();
        todo.pop_back();
        const PdfValue& pairs = document.Get(*level.node, pairsKey);
        if (pairs.IsArray()) {
            const PdfSpan<PdfValue> entries = pairs.Array();
            for (size_t i = 0; i + 1 < entries.size() && budget > 0; i += 2)
                visit(document.Resolve(entries[i]), entries[i + 1]);
        }
        const PdfValue& kids = document.Get(*level.node, "Kids");
        if (level.depth < maxDepth) {
            for (const PdfValue& kid : kids.Array()) {
                const PdfValue& child = document.Resolve(kid);
                if (child.IsDict() && budget > 0) {
                    budget--;
                    todo.push_back(Level{&child, level.depth + 1});
                }
            }
        }
    }
}

// A name tree's pairs, sorted by key (bytes): read once, then looked up by key. Of pairs with
// equal keys the first the walk meets is kept. The keys view the document's bytes.
class NameTree {
public:
    // Reads the tree at `root` (anything else: none), at most kMaxTreePairs nodes and pairs.
    void Read(const Document& document, const PdfValue& root);
    // The value of `key` as the tree holds it (unresolved), or null.
    const PdfValue* Find(std::string_view key) const;
    size_t Size() const { return pairs_.size(); }

private:
    std::vector<std::pair<std::string_view, const PdfValue*>> pairs_;
};

// A number tree's pairs, sorted by key, the first of equal keys kept (as NameTree), values
// unresolved.
void ReadNumberTree(const Document& document, const PdfValue& root,
                    std::vector<std::pair<int64_t, const PdfValue*>>& out);

// Destinations to pages: an explicit destination (an array whose first item is a page's
// dictionary), a name (looked up in the catalog's /Dests) or a string (in the catalog's
// /Names /Dests name tree, read on first use), a named one possibly a dictionary whose /D is
// the destination. One object serves a document; it is not thread-safe.
class Destinations {
public:
    explicit Destinations(const Document& document) : document_(document) {}
    // The 0-based index of the page `destination` (resolved or not) leads to, or -1.
    int PageOf(const PdfValue& destination);
    // The destination of an outline item or a link annotation `item` (resolved): its /Dest, else
    // the /D of its action /A when that is a GoTo action; null when it has neither.
    const PdfValue* TargetOf(const PdfValue& item) const;

private:
    const Document& document_;
    std::vector<std::pair<uintptr_t, int>> pages_;  // page dictionaries' addresses and indices, made on first use
    NameTree names_;
    bool namesRead_ = false;
};

} // namespace RayoPdf
