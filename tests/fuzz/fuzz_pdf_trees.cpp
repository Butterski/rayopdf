// SPDX-License-Identifier: Apache-2.0
// libFuzzer target of the name and number trees and destinations (pdf/trees.h): the input as a PDF
// file through Document::Open, then the catalog's /Names /Dests name tree read and each of its
// keys looked up again (a key found must lead to a pair with that key), every destination of it
// and of the catalog's /Dests resolved to a page, and the number trees /PageLabels and the
// structure tree's /ParentTree read (sorted by key, no key twice).
// Seeds: `python3 tools/fuzz.py seeds` (real PDFs).
#include "pdf/document.h"
#include "pdf/trees.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    using namespace RayoPdf;
    std::unique_ptr<Document> document;
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);
    if (Document::Open(bytes, "", document) != OpenStatus::Ok) return 0;
    const PdfValue& catalog = document->Catalog();
    const int pages = document->PageCount();
    Destinations destinations(*document);
    const auto check = [&](const PdfValue& destination) {
        const int page = destinations.PageOf(destination);
        if (page < -1 || page >= pages) __builtin_trap();
    };
    NameTree names;
    names.Read(*document, document->Get(document->Get(catalog, "Names"), "Dests"));
    size_t budget = kMaxTreePairs;
    WalkTree(*document, document->Resolve(document->Get(document->Get(catalog, "Names"), "Dests")), TreeKind::Name,
             kMaxTreeDepth, budget, [&](const PdfValue& key, const PdfValue& value) {
                 budget--;
                 if (!key.IsString()) return;
                 if (!names.Find(key.String())) __builtin_trap();
                 check(value);
                 check(key);
             });
    const PdfValue& dests = document->Get(catalog, "Dests");
    size_t entries = 0;
    for (const PdfDictEntry& entry : dests.Entries()) {
        if (++entries > 4096) break;
        check(entry.value);
    }
    std::vector<std::pair<int64_t, const PdfValue*>> numbers;
    for (const PdfValue* root : {&document->Get(catalog, "PageLabels"),
                                 &document->Get(document->Get(catalog, "StructTreeRoot"), "ParentTree")}) {
        ReadNumberTree(*document, *root, numbers);
        for (size_t i = 1; i < numbers.size(); ++i)
            if (numbers[i - 1].first >= numbers[i].first) __builtin_trap();
    }
    return 0;
}
