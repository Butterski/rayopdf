// SPDX-License-Identifier: Apache-2.0
// The object layer of document.h. Parsing style and the classic cross-reference reader follow
// rayomd-gui src/pdf/reader.cpp at 9fbbfaf (Apache-2.0, same author); the rest is roadmap M1:
// algorithms §1.1 (structure), §1.2 (repair), §1.4 (encryption) and §1.5 (limits).
//
// Objects live in a flat slot vector indexed by object number, filled from the cross-reference
// sections newest first (the first entry for a number wins) and parsed on first use into the
// document's arena. When the sections cannot be read, or an entry points at no `N G obj`, one scan
// over the file finds every object (the last definition of a number wins) and `trailer`
// dictionaries; the scan runs at most once per document.
#include "pdf/document.h"

#include "common/compiler.h"
#include "common/per_thread.h"
#include "crypt/crypt.h"
#include "pdf/inflate.h"
#include "pdf/syntax.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace RayoPdf {
namespace {

using Syntax::Cursor;

constexpr int kMaxReferenceChain = 32;
constexpr size_t kMaxTreeDepth = 64;
constexpr size_t kMaxXrefSections = 1024;
// Object() parsing an object while parsing another (an indirect /Length, an object stream's
// container) nests at most this deep.
constexpr int kMaxNestedLoads = 32;
constexpr size_t kMaxPages = size_t(1) << 20;
constexpr size_t kMaxFilters = 16;
// "%PDF-" is looked for this far into the file, "startxref" this far from its end first.
constexpr size_t kHeaderWindow = 1024;
constexpr size_t kStartxrefWindow = 4096;
// How far into an object the repair scan looks for a /Type it needs (/XRef, /ObjStm, /Catalog).
constexpr size_t kTypeWindow = 4096;

const PdfValue& Null() {
    static const PdfValue kNull{};
    return kNull;
}

// A non-negative decimal integer token: digits only, at most INT64_MAX.
bool ReadUnsigned(std::string_view token, uint64_t& out) {
    size_t i = 0;
    while (i < token.size() && token[i] == '0') i++;
    if (token.empty() || token.size() - i > 19) return false;
    uint64_t value = 0;
    for (; i < token.size(); i++) {
        const unsigned d = static_cast<unsigned>(token[i]) - '0';
        if (d > 9) return false;
        value = value * 10 + d;
    }
    if (value > static_cast<uint64_t>(INT64_MAX)) return false;
    out = value;
    return true;
}

// Syntax::SkipSpace, its white space here.
inline void SkipSpaceInline(Cursor& c) {
    const char* const data = c.data.data();
    const size_t size = c.data.size();
    size_t pos = c.pos;
    while (pos < size && Syntax::IsWhite(static_cast<unsigned char>(data[pos]))) pos++;
    c.pos = pos;
    if (pos < size && data[pos] == '%') Syntax::SkipSpace(c);
}

// SkipSpace, then ReadUnsigned of ReadToken: a token of up to 18 digits read here, any other by
// them.
inline bool NextUnsigned(Cursor& c, uint64_t& out) {
    SkipSpaceInline(c);
    const char* const data = c.data.data();
    const size_t size = c.data.size();
    const size_t start = c.pos;
    size_t pos = start;
    uint64_t value = 0;
    for (unsigned d; pos < size && (d = static_cast<unsigned>(static_cast<unsigned char>(data[pos])) - '0') < 10; pos++) {
        value = value * 10 + d;
    }
    if (pos > start && pos - start <= 18 && (pos >= size || !Syntax::IsRegular(static_cast<unsigned char>(data[pos])))) {
        c.pos = pos;
        out = value;
        return true;
    }
    return ReadUnsigned(Syntax::ReadToken(c), out);
}

// SkipSpace, then whether ReadToken reads `word`; the cursor is past the token when it does.
inline bool NextKeyword(Cursor& c, std::string_view word) {
    SkipSpaceInline(c);
    const size_t pos = c.pos;
    const size_t end = pos + word.size();
    if (end > c.data.size() || c.data.compare(pos, word.size(), word) != 0) return false;
    if (end < c.data.size() && Syntax::IsRegular(static_cast<unsigned char>(c.data[end]))) return false;
    c.pos = end;
    return true;
}

// A cross-reference entry in its fixed form at data[at]: 10 digits, a space, 5 digits, a space,
// 'n' or 'f' ending the token. False (nothing read) for any other form, which the token reader
// then takes.
bool FixedXrefEntry(std::string_view data, size_t at, uint64_t& offset, uint64_t& gen, char& type) {
    if (data.size() - at < 18 || data[at + 10] != ' ' || data[at + 16] != ' ') return false;
    if (at + 18 < data.size() && Syntax::IsRegular(static_cast<unsigned char>(data[at + 18]))) return false;
    type = data[at + 17];
    return (type == 'n' || type == 'f') && ReadUnsigned(data.substr(at, 10), offset) &&
           ReadUnsigned(data.substr(at + 11, 5), gen);
}

// The first `needle` in `data` at or after `from`, found by memchr on its byte at `pivot` (pick a
// rare one).
size_t FindFrom(std::string_view data, size_t from, std::string_view needle, size_t pivot) {
    const size_t n = data.size();
    const size_t m = needle.size();
    if (m > n || from > n - m) return std::string_view::npos;
    const char* const base = data.data();
    size_t at = from + pivot;
    const size_t lastPivot = n - m + pivot;
    while (at <= lastPivot) {
        const void* hit = std::memchr(base + at, needle[pivot], lastPivot - at + 1);
        if (!hit) return std::string_view::npos;
        const size_t start = static_cast<size_t>(static_cast<const char*>(hit) - base) - pivot;
        if (std::memcmp(base + start, needle.data(), m) == 0) return start;
        at = start + pivot + 1;
    }
    return std::string_view::npos;
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// The common part of two rectangles (empty when they do not meet: x0 > x1 or y0 > y1).
PdfRect Intersect(const PdfRect& a, const PdfRect& b) {
    PdfRect r{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
    return r;
}

StreamStatus Worse(StreamStatus a, StreamStatus b) {
    const auto rank = [](StreamStatus s) {
        switch (s) {
            case StreamStatus::Ok: return 0;
            case StreamStatus::Salvaged: return 1;
            case StreamStatus::Unsupported: return 2;
            case StreamStatus::TooLarge: return 3;
            case StreamStatus::Corrupt: return 4;
            case StreamStatus::NotStream: return 5;
        }
        return 5;
    };
    return rank(a) >= rank(b) ? a : b;
}

enum class SlotKind : uint8_t {
    None,        // no cross-reference entry
    Free,        // a free entry (or an xref stream entry of an unknown type): the null object
    Offset,      // at `pos` in the file, generation `aux`
    Compressed,  // in the object stream numbered `pos`, at index `aux`
};

enum class SlotState : uint8_t { Unread, Reading, Read };

struct Slot {
    uint64_t pos = 0;
    const PdfValue* value = nullptr;  // once Read: the parsed value, nullptr for null
    uint32_t aux = 0;
    SlotKind kind = SlotKind::None;
    SlotState state = SlotState::Unread;  // through StateOf and SetState once Open is done
};

// A slot's state, read without the parse lock in shared mode: Read is stored with release once the
// slot's value, kind and generation are final, and loaded with acquire (plain moves on x86). The
// slot stays a trivially copyable struct (std::atomic is not; C++17 has no atomic_ref).
SlotState StateOf(const Slot& slot) {
    SlotState state;
    __atomic_load(&slot.state, &state, __ATOMIC_ACQUIRE);
    return state;
}

void SetState(Slot& slot, SlotState state) {
    __atomic_store(&slot.state, &state, __ATOMIC_RELEASE);
}

// An `N G obj` the repair scan found, `offset` at N.
struct ScanHit {
    uint64_t offset;
    uint32_t num;
    uint32_t gen;
};

struct ScanSlot {
    uint64_t offset = 0;
    uint32_t gen = 0;
    bool present = false;
};

// A page's content, its pieces decoded onto the end of one buffer kept by each thread across pages
// and documents (PageContent's view points into it); a page that left it larger than
// kKeptContent frees it at the next page.
struct PageBuffer {
    Inflate::Output bytes;
    std::string piece;  // a piece decoded whole before it is copied onto the page's bytes
};
constexpr size_t kKeptContent = size_t(8) << 20;

using Stage = Filters::Stage;

// StreamData's working buffers: the decrypted data and a filter's output before it swaps into
// place. The document's own, and each thread's own while it is shared (Impl::Scratch).
struct StreamScratch {
    std::string decrypted;
    std::string filtered;
};

} // namespace

struct Document::Impl {
    explicit Impl(std::string_view data) : bytes(data), builder(arena, true), streamBuilder(arena) {}

    struct ObjectStream {
        bool ok = false;
        bool loading = false;  // being read (an object of it needed meanwhile fails)
        size_t first = 0;
        std::string data;
        std::vector<std::pair<uint32_t, size_t>> index;  // object number, offset from /First
    };

    // Unresolved inherited page attributes (ISO 32000-1 section 7.7.3.4).
    struct Inherited {
        const PdfValue* resources = nullptr;
        const PdfValue* mediaBox = nullptr;
        const PdfValue* cropBox = nullptr;
        const PdfValue* rotate = nullptr;
    };

    // A leaf of the page tree. Its attributes are resolved on first use (ResolvedPage): counting
    // the pages (--inspect) parses no resources dictionary.
    struct Page {
        const PdfValue* dict = nullptr;
        Inherited inherited;
        bool resolved = false;
        const PdfValue* resources = nullptr;
        PdfRect mediaBox;
        PdfRect cropBox;
        int rotate = 0;
    };

    std::string_view bytes;
    size_t headerPos = std::string_view::npos;
    std::string_view headerVersion;
    std::string_view version;
    // Added to every offset the cross-reference data gives: the header's position when the
    // offsets count from it (garbage before the header).
    uint64_t delta = 0;
    // Object numbers at and past this are ignored: far beyond what the file can hold.
    uint64_t numLimit = 0;

    std::vector<Slot> slots;
    PdfArena arena;
    // Values read from the file view its names and plain strings (`bytes` outlives the document);
    // values read from an object stream copy theirs, as FullRepair drops the streams' data.
    Syntax::Builder builder;
    Syntax::Builder streamBuilder;
    PdfValue trailer;
    const PdfValue* catalog = nullptr;
    bool chainIncomplete = false;  // an older section could not be read
    bool fullRepair = false;       // the slots come from the scan
    bool tooLarge = false;
    std::vector<uint64_t> visitedSections;
    std::vector<uint32_t> pendingFree;

    bool scanned = false;
    std::vector<ScanHit> hits;  // in file order
    std::vector<ScanSlot> scan;
    std::vector<size_t> trailerKeywords;

    bool endstreamsFound = false;
    std::vector<size_t> endstreams;

    std::unordered_map<uint32_t, std::unique_ptr<ObjectStream>> objectStreams;

    Crypt::SecurityHandler security;
    bool encrypted = false;
    uint32_t encryptNum = 0;
    const PdfValue* encryptDict = nullptr;
    std::unordered_map<size_t, uint64_t> streamIds;  // stream data offset -> num << 16 | gen

    std::vector<Page> pages;

    uint64_t decoded = 0;
    DocumentReport report;
    int loads = 0;          // nested parses (the parse lock's holder's, while shared)
    std::string decrypted;  // DecryptStrings (under the parse lock while shared)
    StreamScratch scratch;  // StreamData's buffers, while not shared (Scratch)
    // Streams being read that are not counted yet (StreamReader), in the order they were opened,
    // each with the thread that opened it.
    struct Hold {
        StreamReader* reader;
        std::thread::id thread;
    };
    std::vector<Hold> uncounted;

    // ---------------------------------------------------------------- shared mode (BeginShared)

    bool shared = false;
    std::recursive_mutex parseMutex;  // while shared: first parses (Object), ResolvedPage, streamIds
    std::mutex statsMutex;            // while shared: report, decoded, uncounted, maxHeld
    std::atomic<bool> orderSensitive{false};
    uint64_t maxHeld = 0;  // the largest budget hold (a decode's limit and its thread's holds) seen
    // What the shared section changed, to undo it: slots as they were before they left Unread,
    // object streams loaded, pages resolved; the report and the budget as it began.
    std::vector<std::pair<uint32_t, Slot>> undoSlots;
    std::vector<uint32_t> undoStreams;
    std::vector<size_t> undoPages;
    DocumentReport savedReport;
    uint64_t savedDecoded = 0;

    // One more of a report counter.
    void Hit(uint32_t DocumentReport::*counter) {
        if (RAYOPDF_UNLIKELY(shared)) {
            std::lock_guard<std::mutex> lock(statsMutex);
            ++(report.*counter);
            return;
        }
        ++(report.*counter);
    }

    void MarkRepaired() {
        if (RAYOPDF_UNLIKELY(shared)) {
            std::lock_guard<std::mutex> lock(statsMutex);
            report.repaired = true;
            return;
        }
        report.repaired = true;
    }

    // Something whose outcome can depend on the order objects are read in (BeginShared).
    void OrderSensitive() { orderSensitive.store(true, std::memory_order_relaxed); }

    // Slot `num` is about to leave Unread (under the parse lock).
    void Leaving(uint32_t num) {
        if (shared) undoSlots.emplace_back(num, slots[num]);
    }

    // StreamData's buffers: the document's, or the calling thread's while shared.
    StreamScratch& Scratch() { return shared ? PerThread<StreamScratch>() : scratch; }

    // ---------------------------------------------------------------- values

    bool GenerationMatches(uint32_t num, uint16_t gen) const {
        if (num >= slots.size()) return false;
        const Slot& slot = slots[num];
        if (slot.kind == SlotKind::Offset) return slot.aux == gen;
        return slot.kind == SlotKind::Compressed && gen == 0;
    }

    const PdfValue& Resolve(const PdfValue& value) {
        const PdfValue* current = &value;
        for (int hops = 0; current->IsRef(); hops++) {
            if (hops >= kMaxReferenceChain) {
                Hit(&DocumentReport::limitHits);
                return Null();
            }
            const uint32_t num = current->refNum;
            const uint16_t gen = current->refGen;
            const PdfValue& object = Object(num);
            if (!GenerationMatches(num, gen)) return Null();
            current = &object;
        }
        return *current;
    }

    const PdfValue& Get(const PdfValue& dict, std::string_view key) {
        const PdfValue* entry = Resolve(dict).Find(key);
        return entry ? Resolve(*entry) : Null();
    }

    // An entry without following references (while the cross-reference data is still read).
    static const PdfValue& Direct(const PdfValue& dict, std::string_view key) {
        const PdfValue* entry = dict.Find(key);
        return entry && !entry->IsRef() ? *entry : Null();
    }

    bool Rect(const PdfValue& value, PdfRect& out) {
        const PdfValue& array = Resolve(value);
        if (!array.IsArray() || array.Array().size() != 4) return false;
        double v[4];
        for (size_t i = 0; i < 4; i++) {
            const PdfValue& item = Resolve(array.Array()[i]);
            if (!item.IsNumber() || !std::isfinite(item.Number())) return false;
            v[i] = item.Number();
        }
        out = {std::min(v[0], v[2]), std::min(v[1], v[3]), std::max(v[0], v[2]), std::max(v[1], v[3])};
        return true;
    }

    // ---------------------------------------------------------------- objects

    void Assign(uint64_t num, SlotKind kind, uint64_t pos, uint32_t aux) {
        if (num == 0) return;
        if (num >= numLimit) {
            report.limitHits++;
            return;
        }
        if (num >= slots.size()) slots.resize(static_cast<size_t>(num) + 1);
        Slot& slot = slots[static_cast<size_t>(num)];
        if (slot.kind != SlotKind::None) return;
        slot.kind = kind;
        slot.pos = pos;
        slot.aux = aux;
    }

    void FindEndstreams() {
        if (endstreamsFound) return;
        endstreamsFound = true;
        for (size_t at = FindFrom(bytes, 0, "endstream", 8); at != std::string_view::npos;
             at = FindFrom(bytes, at + 9, "endstream", 8)) {
            endstreams.push_back(at);
        }
    }

    // The end of the data of a stream starting at `start`: `start + length` when "endstream"
    // follows there, else the next "endstream" without the end of line before it, else the
    // length as given or the end of the file.
    size_t StreamEnd(size_t start, int64_t length) {
        const size_t size = bytes.size();
        const bool fits = length >= 0 && static_cast<uint64_t>(length) <= size - start;
        if (fits) {
            Cursor after{bytes, start + static_cast<size_t>(length)};
            Syntax::SkipWhite(after);
            if (Syntax::ReadToken(after) == "endstream") return start + static_cast<size_t>(length);
        }
        FindEndstreams();
        const auto it = std::lower_bound(endstreams.begin(), endstreams.end(), start);
        if (it != endstreams.end()) {
            size_t end = *it;
            if (end > start && bytes[end - 1] == '\n') end--;
            if (end > start && bytes[end - 1] == '\r') end--;
            return end;
        }
        return fits ? start + static_cast<size_t>(length) : size;
    }

    // The object at `offset`: "num gen obj" (any number or generation where `expectNum` is
    // negative), then its value; a dictionary followed by "stream" is a stream. `resolveLength`
    // follows an indirect /Length. Strings are decrypted where `decrypt` and the document is
    // encrypted.
    bool ParseIndirect(uint64_t offset, int64_t expectNum, bool resolveLength, bool decrypt, PdfValue& out,
                       uint32_t& num, uint32_t& gen) {
        if (offset >= bytes.size()) return false;
        Cursor c{bytes, static_cast<size_t>(offset)};
        uint64_t n = 0;
        uint64_t g = 0;
        if (!NextUnsigned(c, n) || n > UINT32_MAX) return false;
        if (expectNum >= 0 && n != static_cast<uint64_t>(expectNum)) return false;
        if (!NextUnsigned(c, g) || g > 65535) return false;
        if (!NextKeyword(c, "obj")) return false;
        num = static_cast<uint32_t>(n);
        gen = static_cast<uint32_t>(g);
        SkipSpaceInline(c);
        if (c.Peek() == 'e') {
            Cursor probe = c;
            if (NextKeyword(probe, "endobj")) {
                out = PdfValue();
                return true;
            }
        }
        if (!Syntax::ParseValue(c, Syntax::Mode::File, 0, builder, out)) return false;
        if (out.type == PdfValue::Type::Dict) {
            Cursor s = c;
            if (NextKeyword(s, "stream")) {
                // The data begins after the end of line of "stream": CR LF or LF (a lone CR, and
                // spaces before the end of line, tolerated).
                Cursor eol = s;
                while (eol.Peek() == ' ' || eol.Peek() == '\t') eol.pos++;
                if (eol.Peek() == '\r' || eol.Peek() == '\n') s = eol;
                if (s.Peek() == '\r') s.pos++;
                if (s.Peek() == '\n') s.pos++;
                const size_t start = s.pos;
                int64_t length = -1;
                if (const PdfValue* entry = out.Find("Length")) {
                    if (entry->type == PdfValue::Type::Int) {
                        length = entry->integer;
                    } else if (entry->IsRef() && resolveLength) {
                        const PdfValue& value = Resolve(*entry);
                        if (value.type == PdfValue::Type::Int) length = value.integer;
                    }
                }
                out.type = PdfValue::Type::Stream;
                out.streamOffset = start;
                out.streamLength = StreamEnd(start, length) - start;
            }
        }
        if (decrypt && encrypted && num != encryptNum && !Direct(out, "Type").IsName("XRef")) {
            DecryptStrings(out, num, static_cast<uint16_t>(gen), 0);
            if (out.IsStream()) streamIds[out.streamOffset] = static_cast<uint64_t>(num) << 16 | gen;
        }
        return true;
    }

    void DecryptStrings(PdfValue& value, uint32_t num, uint16_t gen, int depth) {
        if (depth > Syntax::kMaxDepth + 1) return;
        switch (value.type) {
            case PdfValue::Type::String: {
                if (value.count == 0) return;
                security.Decrypt(num, gen, false, value.String(), decrypted);
                char* block = decrypted.empty() ? nullptr : static_cast<char*>(arena.Allocate(decrypted.size()));
                if (decrypted.empty() || !block) {
                    value.count = 0;
                    value.data = nullptr;
                    return;
                }
                std::memcpy(block, decrypted.data(), decrypted.size());
                value.data = block;
                value.count = static_cast<uint32_t>(decrypted.size());
                return;
            }
            case PdfValue::Type::Array: {
                PdfValue* items = const_cast<PdfValue*>(static_cast<const PdfValue*>(value.data));
                for (uint32_t i = 0; i < value.count; i++) DecryptStrings(items[i], num, gen, depth + 1);
                return;
            }
            case PdfValue::Type::Dict:
            case PdfValue::Type::Stream: {
                PdfDictEntry* entries = const_cast<PdfDictEntry*>(static_cast<const PdfDictEntry*>(value.data));
                for (uint32_t i = 0; i < value.count; i++) DecryptStrings(entries[i].value, num, gen, depth + 1);
                return;
            }
            default: return;
        }
    }

    ObjectStream* LoadObjectStream(uint32_t container) {
        const auto it = objectStreams.find(container);
        if (it != objectStreams.end()) {
            if (it->second->loading) OrderSensitive();  // which of its objects fails depends on the order
            return it->second.get();
        }
        // The entry goes in first (not ok): a stream reached again while it loads fails.
        ObjectStream* stream = (objectStreams[container] = std::make_unique<ObjectStream>()).get();
        if (shared) undoStreams.push_back(container);
        stream->loading = true;
        FillObjectStream(container, *stream);
        stream->loading = false;
        return stream;
    }

    void FillObjectStream(uint32_t container, ObjectStream& stream) {
        // Object streams are no compressed objects themselves (no nesting).
        if (container >= slots.size() || slots[container].kind != SlotKind::Offset) return;
        const PdfValue& object = Object(container);
        if (!object.IsStream()) return;
        const int64_t n = Get(object, "N").Int(-1);
        const int64_t first = Get(object, "First").Int(-1);
        if (n < 0 || first < 0) return;
        std::string data;
        const StreamStatus status = StreamData(object, data, kDefaultStreamLimit);
        if (status != StreamStatus::Ok && status != StreamStatus::Salvaged && status != StreamStatus::TooLarge) {
            return;
        }
        if (static_cast<uint64_t>(first) > data.size()) return;
        stream.first = static_cast<size_t>(first);
        Cursor c{std::string_view(data).substr(0, stream.first), 0};
        for (int64_t k = 0; k < n; k++) {
            uint64_t num = 0;
            uint64_t offset = 0;
            Syntax::SkipSpace(c);
            if (!ReadUnsigned(Syntax::ReadToken(c), num)) break;
            Syntax::SkipSpace(c);
            if (!ReadUnsigned(Syntax::ReadToken(c), offset)) break;
            if (num > UINT32_MAX) break;
            stream.index.emplace_back(static_cast<uint32_t>(num), static_cast<size_t>(offset));
        }
        stream.data = std::move(data);
        stream.ok = true;
    }

    bool ParseCompressed(uint32_t num, uint64_t container, uint32_t index, PdfValue& out) {
        if (container > UINT32_MAX) return false;
        ObjectStream* stream = LoadObjectStream(static_cast<uint32_t>(container));
        if (!stream || !stream->ok || index >= stream->index.size() || stream->index[index].first != num) return false;
        const size_t at = stream->first + stream->index[index].second;
        if (at < stream->first || at >= stream->data.size()) return false;
        Cursor c{stream->data, at};
        Syntax::SkipSpace(c);
        if (!Syntax::ParseValue(c, Syntax::Mode::File, 0, streamBuilder, out)) return false;
        return true;
    }

    // Object `num`, parsed on first use (ObjectShared while shared).
    RAYOPDF_NOINLINE const PdfValue& Object(uint32_t num) {
        if (num == 0 || num >= slots.size()) return Null();
        if (RAYOPDF_UNLIKELY(shared)) return ObjectShared(num);
        return Load(num);
    }

    // Object while shared: without a lock once Read; a first parse under the parse lock, so that
    // only its holder parses and Reading is always a slot on its own stack.
    RAYOPDF_NOINLINE const PdfValue& ObjectShared(uint32_t num) {
        const Slot& seen = slots[num];
        if (StateOf(seen) == SlotState::Read) return seen.value ? *seen.value : Null();
        std::lock_guard<std::recursive_mutex> lock(parseMutex);
        return Load(num);
    }

    RAYOPDF_HOT_INLINE const PdfValue& Load(uint32_t num) {
        {
            Slot& slot = slots[num];
            const SlotState state = slot.state;
            if (state == SlotState::Read) return slot.value ? *slot.value : Null();
            if (state == SlotState::Reading) {
                Hit(&DocumentReport::limitHits);  // a cycle: an object needed to parse itself
                OrderSensitive();                 // what each object of the cycle gets depends on where it was entered
                return Null();
            }
            if (slot.kind == SlotKind::None || slot.kind == SlotKind::Free) {
                Leaving(num);
                SetState(slot, SlotState::Read);
                return Null();
            }
            if (loads >= kMaxNestedLoads) {
                Hit(&DocumentReport::limitHits);
                OrderSensitive();  // how deep a parse nests depends on what was parsed before
                return Null();
            }
            Leaving(num);
            SetState(slot, SlotState::Reading);
        }
        loads++;
        // Slots are read by index from here: nothing below resizes them, but references are kept
        // short anyway.
        const SlotKind kind = slots[num].kind;
        const uint64_t pos = slots[num].pos;
        const uint32_t aux = slots[num].aux;
        PdfValue value;
        bool ok = false;
        uint32_t gotGen = 0;
        if (kind == SlotKind::Offset) {
            ok = ParseAt(num, pos, value, gotGen);
            if (ok) slots[num].aux = gotGen;  // the object's own generation, which references use
        } else {
            ok = ParseCompressed(num, pos, aux, value);
        }
        if (!ok) {
            EnsureScan();
            if (num < scan.size() && scan[num].present && (kind != SlotKind::Offset || scan[num].offset != pos)) {
                ok = ParseAt(num, scan[num].offset, value, gotGen);
                if (ok) {
                    Slot& slot = slots[num];
                    slot.kind = SlotKind::Offset;
                    slot.pos = scan[num].offset;
                    slot.aux = gotGen;
                    MarkRepaired();
                }
            }
            if (!ok) Hit(&DocumentReport::brokenObjects);
        }
        loads--;
        const PdfValue* stored = nullptr;
        if (ok && !value.IsNull()) {
            void* block = arena.Allocate(sizeof(PdfValue));
            if (block) stored = new (block) PdfValue(value);
        }
        Slot& slot = slots[num];
        slot.value = stored;
        SetState(slot, SlotState::Read);
        return stored ? *stored : Null();
    }

    bool ParseAt(uint32_t num, uint64_t offset, PdfValue& out, uint32_t& gen) {
        uint32_t gotNum = 0;
        return ParseIndirect(offset, num, true, true, out, gotNum, gen);
    }

    // ---------------------------------------------------------------- streams

    // The filters of `dict` with their parameters; false past kMaxFilters. `resolve` false reads
    // direct entries only (the cross-reference data being read).
    bool GatherStages(const PdfValue& dict, bool resolve, Stage* stages, size_t& count, std::string_view& cryptName) {
        count = 0;
        const PdfValue& filter = resolve ? Get(dict, "Filter") : Direct(dict, "Filter");
        const PdfValue* parmsEntry = dict.Find("DecodeParms");
        if (!parmsEntry) parmsEntry = dict.Find("DP");
        const PdfValue& parms = !parmsEntry ? Null()
                                : resolve   ? Resolve(*parmsEntry)
                                : parmsEntry->IsRef() ? Null()
                                                      : *parmsEntry;
        const PdfSpan<PdfValue> names = filter.Array();
        const PdfSpan<PdfValue> parmItems = parms.Array();
        const size_t n = filter.IsName() ? 1 : names.size();
        if (n > kMaxFilters) return false;
        for (size_t i = 0; i < n; i++) {
            const PdfValue& name = filter.IsName() ? filter : resolve ? Resolve(names[i]) : names[i];
            Stage& stage = stages[count++];
            stage.kind = Filters::KindOf(name.Name());
            // A dictionary of /DecodeParms applies to every filter of an array (only Flate and LZW
            // read it); an array gives one each, null for none.
            const PdfValue* p = &parms;
            if (parms.IsArray()) {
                p = i < parmItems.size() ? &parmItems[i] : &Null();
                if (resolve) p = &Resolve(*p);
                else if (p->IsRef()) p = &Null();
            }
            if (!p->IsDict()) continue;
            const auto number = [&](std::string_view key, int fallback) {
                const PdfValue& v = resolve ? Get(*p, key) : Direct(*p, key);
                const int64_t x = v.Int(fallback);
                return x < -1000000000 || x > 1000000000 ? fallback : static_cast<int>(x);
            };
            stage.params.predictor = number("Predictor", 1);
            stage.params.colors = number("Colors", 1);
            stage.params.bitsPerComponent = number("BitsPerComponent", 8);
            stage.params.columns = number("Columns", 1);
            stage.params.earlyChange = number("EarlyChange", 1);
            if (stage.kind == Filters::Kind::Crypt && i == 0) {
                cryptName = (resolve ? Get(*p, "Name") : Direct(*p, "Name")).Name();
            }
        }
        return true;
    }

    // Decodes `raw` (already decrypted) with `stages` into `out`.
    StreamStatus RunStages(std::string_view raw, const Stage* stages, size_t count, std::string& out, size_t limit) {
        std::string& scratch2 = Scratch().filtered;
        std::string_view data = raw;
        StreamStatus status = StreamStatus::Ok;
        bool decodedAny = false;
        for (size_t i = 0; i < count; i++) {
            const Filters::Kind kind = stages[i].kind;
            if (kind == Filters::Kind::Crypt) continue;  // handled before (a Crypt filter comes first)
            if (kind != Filters::Kind::Flate && kind != Filters::Kind::Lzw && kind != Filters::Kind::Ascii85 &&
                kind != Filters::Kind::AsciiHex && kind != Filters::Kind::RunLength) {
                status = Worse(status, StreamStatus::Unsupported);
                break;
            }
            const StreamStatus step = Filters::Decode(kind, stages[i].params, data, scratch2, limit);
            out.swap(scratch2);
            data = out;
            decodedAny = true;
            if (step == StreamStatus::Corrupt) {
                out.clear();
                return StreamStatus::Corrupt;
            }
            status = Worse(status, step);
            if (step == StreamStatus::TooLarge) return status;
        }
        if (!decodedAny || data.data() != out.data()) {
            if (data.size() > limit) {
                out.assign(data.data(), limit);
                return Worse(status, StreamStatus::TooLarge);
            }
            out.assign(data.data(), data.size());
        }
        return status;
    }

    bool CryptFilterFor(std::string_view name, Crypt::CryptFilter& out) {
        out = Crypt::CryptFilter();
        if (name.empty() || name == "Identity") return true;
        if (!encryptDict) return false;
        const PdfValue& filter = Get(Get(*encryptDict, "CF"), name);
        if (!filter.IsDict()) return false;
        const std::string_view method = Get(filter, "CFM").Name();
        if (method.empty() || method == "None") out.method = Crypt::Method::Identity;
        else if (method == "V2") out.method = Crypt::Method::Rc4;
        else if (method == "AESV2") out.method = Crypt::Method::Aes128;
        else if (method == "AESV3") out.method = Crypt::Method::Aes256;
        else return false;
        // /Length is bytes by ISO 32000-1 Table 25, bits as many writers give it.
        int64_t length = Get(filter, "Length").Int(0);
        if (length > 32) length /= 8;
        out.lengthBytes = length >= 5 && length <= 32 ? static_cast<int>(length) : 0;
        if (out.method == Crypt::Method::Aes128) out.lengthBytes = 16;
        if (out.method == Crypt::Method::Aes256) out.lengthBytes = 32;
        return true;
    }

    // StreamData up to its filters: the stream's stages (`count` of them) and the data they read,
    // `raw` (decrypted into `buffer` when the document is encrypted), with `start` the status of
    // that data, and `limit` cut to what the decode budget leaves. A status other than Ok ends the
    // read with nothing, counted.
    StreamStatus Prepare(const PdfValue& value, size_t& limit, Stage* stages, size_t& count, std::string& buffer,
                         std::string_view& raw, StreamStatus& start) {
        const PdfValue& stream = Resolve(value);
        if (!stream.IsStream()) return StreamStatus::NotStream;
        if (stream.streamOffset > bytes.size() || stream.streamLength > bytes.size() - stream.streamOffset) {
            Hit(&DocumentReport::failedStreams);
            return StreamStatus::Corrupt;
        }
        // Everything that resolves references first: resolving may load an object stream, which
        // decodes through here and uses the scratch buffers.
        std::string_view cryptName;
        if (!GatherStages(stream, true, stages, count, cryptName)) {
            Hit(&DocumentReport::limitHits);
            return StreamStatus::Unsupported;
        }
        bool decrypt = false;
        uint64_t id = 0;
        Crypt::CryptFilter explicitFilter;
        bool useExplicit = false;
        if (encrypted) {
            bool known = false;
            if (RAYOPDF_UNLIKELY(shared)) {
                std::lock_guard<std::recursive_mutex> lock(parseMutex);  // parses add to streamIds
                const auto it = streamIds.find(stream.streamOffset);
                known = it != streamIds.end();
                if (known) id = it->second;
            } else {
                const auto it = streamIds.find(stream.streamOffset);
                known = it != streamIds.end();
                if (known) id = it->second;
            }
            const PdfValue& type = Get(stream, "Type");
            decrypt = known && !type.IsName("XRef") && !(type.IsName("Metadata") && !security.EncryptMetadata());
            if (decrypt && count > 0 && stages[0].kind == Filters::Kind::Crypt) {
                if (cryptName.empty() || cryptName == "Identity") {
                    decrypt = false;
                } else if (CryptFilterFor(cryptName, explicitFilter)) {
                    useExplicit = true;
                } else {
                    Hit(&DocumentReport::failedStreams);
                    return StreamStatus::Unsupported;
                }
            }
        }
        if (!Budget(limit)) return StreamStatus::TooLarge;
        raw = bytes.substr(stream.streamOffset, stream.streamLength);
        start = StreamStatus::Ok;
        if (decrypt) {
            const uint32_t num = static_cast<uint32_t>(id >> 16);
            const uint16_t gen = static_cast<uint16_t>(id & 0xFFFF);
            const bool clean = useExplicit ? security.DecryptWith(explicitFilter, num, gen, raw, buffer)
                                           : security.Decrypt(num, gen, true, raw, buffer);
            if (!clean) start = StreamStatus::Salvaged;
            raw = buffer;
        }
        return StreamStatus::Ok;
    }

    // A decoded stream in the budget and the report.
    void Count(StreamStatus status, uint64_t size) {
        if (RAYOPDF_UNLIKELY(shared)) {
            std::lock_guard<std::mutex> lock(statsMutex);
            Counted(status, size);
            return;
        }
        Counted(status, size);
    }

    void Counted(StreamStatus status, uint64_t size) {
        decoded += size;
        if (status == StreamStatus::Salvaged) report.salvagedStreams++;
        else if (status == StreamStatus::Corrupt) report.failedStreams++;
        else if (status == StreamStatus::TooLarge) report.limitHits++;
    }

    // A StreamReader opened: held back until it is counted.
    void HoldBack(StreamReader* reader) {
        if (RAYOPDF_UNLIKELY(shared)) {
            std::lock_guard<std::mutex> lock(statsMutex);
            uncounted.push_back({reader, std::this_thread::get_id()});
            return;
        }
        uncounted.push_back({reader, std::thread::id()});
    }

    // A StreamReader counted: no longer held back.
    void Uncount(StreamReader* reader, StreamStatus status, uint64_t size) {
        if (RAYOPDF_UNLIKELY(shared)) {
            std::lock_guard<std::mutex> lock(statsMutex);
            Release(reader);
            Counted(status, size);
            return;
        }
        Release(reader);
        Counted(status, size);
    }

    void Release(StreamReader* reader) {
        uncounted.erase(std::remove_if(uncounted.begin(), uncounted.end(),
                                       [reader](const Hold& hold) { return hold.reader == reader; }),
                        uncounted.end());
    }

    // The decode budget before a decode of at most `limit` bytes: false (counted) once it is spent,
    // else true with `limit` cut to what it leaves, after CountUncounted.
    //
    // While shared, a thread never counts another thread's streams. A check that could cut, refuse
    // or count held-back streams (the budget left not above this decode's limit and the holds of
    // its thread) marks the section order-sensitive; and since a one-thread reading has decoded at
    // most the shared section's total before any of its checks, with the same limits and holds,
    // EndShared needs only the largest hold to prove that none of its checks could have.
    bool Budget(size_t& limit) {
        if (RAYOPDF_UNLIKELY(shared)) {
            std::lock_guard<std::mutex> lock(statsMutex);
            uint64_t held = std::min<uint64_t>(limit, kDocumentDecodeBudget);
            const std::thread::id self = std::this_thread::get_id();
            for (const Hold& hold : uncounted) {
                if (hold.thread == self) held += hold.reader->limit_;
            }
            if (held > maxHeld) maxHeld = held;
            if (decoded >= kDocumentDecodeBudget || kDocumentDecodeBudget - decoded <= held) OrderSensitive();
            return BudgetLeft(limit);
        }
        CountUncounted(limit);
        return BudgetLeft(limit);
    }

    bool BudgetLeft(size_t& limit) {
        if (decoded >= kDocumentDecodeBudget) {
            report.limitHits++;
            return false;
        }
        limit = static_cast<size_t>(std::min<uint64_t>(limit, kDocumentDecodeBudget - decoded));
        return true;
    }

    // Before a decode of at most `limit` bytes: when the streams being read but not counted yet
    // could change its budget check or its limit (the budget left is not more than their limits
    // and this one together), each is read to its end and counted now, in the order they were
    // opened, as StreamData would have counted them before this decode.
    void CountUncounted(size_t limit) {
        if (uncounted.empty()) return;
        uint64_t held = std::min<uint64_t>(limit, kDocumentDecodeBudget);
        for (const Hold& hold : uncounted) held += hold.reader->limit_;
        if (decoded < kDocumentDecodeBudget && kDocumentDecodeBudget - decoded > held) return;
        std::vector<Hold> readers;
        readers.swap(uncounted);
        for (const Hold& hold : readers) {
            StreamReader* reader = hold.reader;
            Filters::Chain chain;
            chain.Start(reader->stages_.data(), reader->stages_.size(), reader->raw_, reader->limit_);
            std::string_view piece;
            while (chain.Next(piece)) {}
            reader->Count(Worse(reader->start_, chain.Status()), chain.Size());
        }
    }

    StreamStatus StreamData(const PdfValue& value, std::string& out, size_t limit) {
        out.clear();
        Stage stages[kMaxFilters];
        size_t count = 0;
        std::string_view raw;
        StreamStatus start = StreamStatus::Ok;
        const StreamStatus prepared = Prepare(value, limit, stages, count, Scratch().decrypted, raw, start);
        if (prepared != StreamStatus::Ok) return prepared;
        const StreamStatus status = Worse(start, RunStages(raw, stages, count, out, limit));
        Count(status, out.size());
        return status;
    }

    // StreamData(value, ·, limit) onto `out` from its byte `at` on: `length` bytes, the same bytes,
    // status and counts. A stream of one Flate filter (Crypt aside) without a predictor inflates
    // straight into `out`; any other is decoded into `piece` as StreamData decodes it, then copied.
    StreamStatus StreamDataInto(const PdfValue& value, Inflate::Output& out, size_t at, size_t limit,
                                std::string& piece, size_t& length) {
        length = 0;
        Stage stages[kMaxFilters];
        size_t count = 0;
        std::string_view raw;
        StreamStatus start = StreamStatus::Ok;
        const StreamStatus prepared = Prepare(value, limit, stages, count, Scratch().decrypted, raw, start);
        if (prepared != StreamStatus::Ok) return prepared;
        // The filters RunStages runs (it skips Crypt).
        const Stage* last = nullptr;
        size_t decoders = 0;
        for (size_t i = 0; i < count; i++) {
            if (stages[i].kind == Filters::Kind::Crypt) continue;
            decoders++;
            last = &stages[i];
        }
        StreamStatus status;
        if (decoders == 1 && last->kind == Filters::Kind::Flate && last->params.predictor != 2 &&
            last->params.predictor < 10) {
            status = Worse(start, Filters::DecodeFlateInto(raw, out, at, limit, length));
        } else {
            status = Worse(start, RunStages(raw, stages, count, piece, limit));
            length = piece.size();
            uint8_t* data = out.Grow(at + length);
            if (length > 0) std::memcpy(data + at, piece.data(), length);
        }
        Count(status, length);
        return status;
    }

    // ---------------------------------------------------------------- cross-reference data

    void FindHeader() {
        const size_t at = FindFrom(bytes.substr(0, std::min(bytes.size(), kHeaderWindow + 5)), 0, "%PDF-", 1);
        if (at == std::string_view::npos || at >= kHeaderWindow) return;
        headerPos = at;
        size_t end = at + 5;
        while (end < bytes.size() && end < at + 13 && (IsDigit(bytes[end]) || bytes[end] == '.')) end++;
        headerVersion = bytes.substr(at + 5, end - at - 5);
    }

    // True when a cross-reference section starts at `offset`: "xref" or "N G obj".
    bool SectionAt(uint64_t offset) const {
        if (offset >= bytes.size()) return false;
        Cursor c{bytes, static_cast<size_t>(offset)};
        Syntax::SkipSpace(c);
        const std::string_view first = Syntax::ReadToken(c);
        if (first == "xref") return true;
        uint64_t value = 0;
        if (!ReadUnsigned(first, value)) return false;
        Syntax::SkipSpace(c);
        if (!ReadUnsigned(Syntax::ReadToken(c), value)) return false;
        Syntax::SkipSpace(c);
        return Syntax::ReadToken(c) == "obj";
    }

    bool ReadClassic(Cursor& c, PdfValue& sectionTrailer) {
        pendingFree.clear();
        bool ended = false;  // "trailer" met
        while (!ended) {
            Syntax::SkipSpace(c);
            Cursor probe = c;
            const std::string_view token = Syntax::ReadToken(probe);
            if (token == "trailer") {
                c = probe;
                break;
            }
            uint64_t first = 0;
            uint64_t count = 0;
            if (!ReadUnsigned(token, first)) return false;
            c = probe;
            Syntax::SkipSpace(c);
            if (!ReadUnsigned(Syntax::ReadToken(c), count)) return false;
            // The slots of the whole subsection at once rather than one reallocation per doubling;
            // no more than its fixed-form entries (18 bytes or more each) can fill.
            if (first < numLimit) {
                const uint64_t fits = (bytes.size() - c.pos) / 18 + 1;
                const uint64_t want = std::min<uint64_t>(first + std::min(count, fits), numLimit);
                if (want > slots.capacity()) slots.reserve(static_cast<size_t>(want));
            }
            uint64_t shift = 0;
            // Every entry takes at least three tokens of the file, so `count` cannot outrun it.
            for (uint64_t k = 0; k < count; k++) {
                Syntax::SkipSpace(c);
                uint64_t offset = 0;
                uint64_t gen = 0;
                char type = 0;
                if (FixedXrefEntry(bytes, c.pos, offset, gen, type)) {
                    c.pos += 18;
                } else {
                    Cursor entry = c;
                    const std::string_view entryOffset = Syntax::ReadToken(entry);
                    if (entryOffset == "trailer") {
                        // Fewer entries than the subsection said.
                        c = entry;
                        ended = true;
                        break;
                    }
                    Syntax::SkipSpace(entry);
                    const std::string_view entryGen = Syntax::ReadToken(entry);
                    Syntax::SkipSpace(entry);
                    const std::string_view entryType = Syntax::ReadToken(entry);
                    if (!ReadUnsigned(entryOffset, offset) || !ReadUnsigned(entryGen, gen) ||
                        (entryType != "n" && entryType != "f")) {
                        return false;
                    }
                    type = entryType[0];
                    c = entry;
                }
                if (gen > 65535) return false;
                // A common writer bug: a table from object 0 numbered from 1.
                if (k == 0 && first == 1 && type == 'f' && offset == 0 && gen == 65535) shift = 1;
                const uint64_t num = first + k - shift;
                // An in-use entry at offset 0, where the header is, stands for no object (Quartz
                // writes them for unused numbers).
                if (type == 'n' && offset != 0) {
                    Assign(num, SlotKind::Offset, offset + delta, static_cast<uint32_t>(gen));
                } else if (num < numLimit && num <= UINT32_MAX) {
                    pendingFree.push_back(static_cast<uint32_t>(num));
                }
            }
        }
        Syntax::SkipSpace(c);
        PdfValue dict;
        if (!Syntax::ParseValue(c, Syntax::Mode::File, 0, builder, dict) || dict.type != PdfValue::Type::Dict) {
            return false;
        }
        sectionTrailer = dict;
        // A hybrid file (ISO 32000-1 section 7.5.8.4): the stream's entries come after the table's
        // in-use entries and before its free ones, which often stand for the compressed objects.
        const PdfValue& stm = Direct(dict, "XRefStm");
        // `pendingFree` stays put while the stream is read: ReadXrefStream reads no table.
        if (stm.type == PdfValue::Type::Int && stm.integer >= 0) {
            const uint64_t offset = static_cast<uint64_t>(stm.integer) + delta;
            if (std::find(visitedSections.begin(), visitedSections.end(), offset) == visitedSections.end()) {
                visitedSections.push_back(offset);
                PdfValue ignored;
                if (ReadXrefStream(offset, ignored)) report.xrefSections++;
                else chainIncomplete = true;
            }
        }
        for (uint32_t num : pendingFree) Assign(num, SlotKind::Free, 0, 0);
        pendingFree.clear();
        return true;
    }

    bool ReadXrefStream(uint64_t offset, PdfValue& sectionTrailer) {
        PdfValue stream;
        uint32_t num = 0;
        uint32_t gen = 0;
        if (!ParseIndirect(offset, -1, false, false, stream, num, gen) || !stream.IsStream()) return false;
        const PdfValue& w = Direct(stream, "W");
        if (!w.IsArray() || w.Array().size() < 3) return false;
        uint64_t widths[3];
        const PdfSpan<PdfValue> items = w.Array();
        for (size_t i = 0; i < 3; i++) {
            const PdfValue& item = items[i];
            if (item.type != PdfValue::Type::Int || item.integer < 0 || item.integer > 8) return false;
            widths[i] = static_cast<uint64_t>(item.integer);
        }
        const size_t entrySize = static_cast<size_t>(widths[0] + widths[1] + widths[2]);
        if (entrySize == 0) return false;
        const int64_t size = Direct(stream, "Size").Int(-1);
        if (size > static_cast<int64_t>(kMaxObjects)) tooLarge = true;
        Stage stages[kMaxFilters];
        size_t count = 0;
        std::string_view cryptName;
        if (!GatherStages(stream, false, stages, count, cryptName)) return false;
        std::string data;
        const StreamStatus status = RunStages(bytes.substr(stream.streamOffset, stream.streamLength), stages, count,
                                              data, kDefaultStreamLimit);
        if (status != StreamStatus::Ok && status != StreamStatus::Salvaged) return false;
        const PdfValue& index = Direct(stream, "Index");
        const auto field = [&](const unsigned char* p, uint64_t width, uint64_t fallback) {
            if (width == 0) return fallback;
            uint64_t value = 0;
            for (uint64_t i = 0; i < width; i++) value = value << 8 | p[i];
            return value;
        };
        const unsigned char* p = reinterpret_cast<const unsigned char*>(data.data());
        size_t at = 0;
        const auto subsection = [&](int64_t first, int64_t n) {
            if (first < 0 || n < 0) return false;
            for (int64_t k = 0; k < n; k++) {
                if (data.size() - at < entrySize) return false;
                const unsigned char* e = p + at;
                at += entrySize;
                const uint64_t type = field(e, widths[0], 1);
                const uint64_t f2 = field(e + widths[0], widths[1], 0);
                const uint64_t f3 = field(e + widths[0] + widths[1], widths[2], 0);
                const uint64_t objectNum = static_cast<uint64_t>(first) + static_cast<uint64_t>(k);
                if (type == 1 && f2 != 0 && f3 <= 65535) {
                    Assign(objectNum, SlotKind::Offset, f2 + delta, static_cast<uint32_t>(f3));
                } else if (type == 2 && f2 <= UINT32_MAX && f3 <= UINT32_MAX) {
                    Assign(objectNum, SlotKind::Compressed, f2, static_cast<uint32_t>(f3));
                } else {
                    Assign(objectNum, SlotKind::Free, 0, 0);  // free, or a type with no meaning: null
                }
            }
            return true;
        };
        if (index.IsArray()) {
            const PdfSpan<PdfValue> items = index.Array();
            for (size_t i = 0; i + 1 < items.size(); i += 2) {
                if (!subsection(items[i].Int(-1), items[i + 1].Int(-1))) break;
            }
        } else {
            subsection(0, size < 0 ? 0 : size);
        }
        sectionTrailer = stream;
        sectionTrailer.type = PdfValue::Type::Dict;
        sectionTrailer.streamOffset = 0;
        sectionTrailer.streamLength = 0;
        return true;
    }

    bool ReadXrefChain() {
        const size_t size = bytes.size();
        const size_t tail = size > kStartxrefWindow ? size - kStartxrefWindow : 0;
        size_t marker = bytes.substr(tail).rfind("startxref");
        if (marker != std::string_view::npos) marker += tail;
        else marker = bytes.rfind("startxref");
        if (marker == std::string_view::npos) return false;
        Cursor c{bytes, marker + 9};
        Syntax::SkipSpace(c);
        uint64_t start = 0;
        if (!ReadUnsigned(Syntax::ReadToken(c), start)) return false;
        if (SectionAt(start)) {
            delta = 0;
        } else if (headerPos != std::string_view::npos && headerPos > 0 && SectionAt(start + headerPos)) {
            delta = headerPos;
        } else {
            return false;
        }
        uint64_t next = start + delta;
        bool newest = true;
        for (;;) {
            if (std::find(visitedSections.begin(), visitedSections.end(), next) != visitedSections.end()) {
                report.limitHits++;  // a /Prev cycle
                break;
            }
            if (visitedSections.size() >= kMaxXrefSections) {
                report.limitHits++;
                chainIncomplete = true;
                break;
            }
            visitedSections.push_back(next);
            PdfValue section;
            Cursor at{bytes, static_cast<size_t>(next)};
            Syntax::SkipSpace(at);
            bool ok = false;
            if (Syntax::ReadToken(at) == "xref") ok = ReadClassic(at, section);
            else ok = ReadXrefStream(next, section);
            if (!ok) {
                if (newest) return false;
                chainIncomplete = true;
                break;
            }
            report.xrefSections++;
            if (newest || !Direct(trailer, "Root").IsRef()) {
                if (newest || Direct(section, "Root").IsRef()) trailer = section;
            }
            newest = false;
            const PdfValue* prev = section.Find("Prev");
            if (!prev) break;
            // pdf.js accepts the illegal "/Prev N 0 R" as offset N.
            int64_t offset = prev->IsRef() ? static_cast<int64_t>(prev->refNum) : prev->Int(-1);
            if (offset < 0 || static_cast<uint64_t>(offset) + delta >= size) {
                chainIncomplete = true;
                break;
            }
            next = static_cast<uint64_t>(offset) + delta;
        }
        const int64_t declared = Direct(trailer, "Size").Int(0);
        if (declared > static_cast<int64_t>(kMaxObjects)) tooLarge = true;
        return true;
    }

    // ---------------------------------------------------------------- repair

    // One pass over the file: every "N G obj" outside stream data, and every "trailer".
    void EnsureScan() {
        if (scanned) return;
        scanned = true;
        const std::string_view data = bytes;
        const size_t size = data.size();
        constexpr size_t npos = std::string_view::npos;
        size_t nextObj = 0, nextStream = 0, nextTrailer = 0;
        bool haveObj = false, haveStream = false, haveTrailer = false;
        size_t pos = 0;
        uint32_t highest = 0;
        for (;;) {
            if (!haveObj || (nextObj != npos && nextObj < pos)) nextObj = FindFrom(data, pos, "obj", 2), haveObj = true;
            if (!haveStream || (nextStream != npos && nextStream < pos)) {
                nextStream = FindFrom(data, pos, "stream", 5);
                haveStream = true;
            }
            if (!haveTrailer || (nextTrailer != npos && nextTrailer < pos)) {
                nextTrailer = FindFrom(data, pos, "trailer", 4);
                haveTrailer = true;
            }
            const size_t p = std::min(nextObj, std::min(nextStream, nextTrailer));
            if (p == npos) break;
            if (p == nextObj) {
                pos = p + 3;
                ScanHit hit;
                if (ObjectHeaderBefore(p, hit)) {
                    hits.push_back(hit);
                    highest = std::max(highest, hit.num);
                }
            } else if (p == nextStream) {
                pos = p + 6;
                const bool keyword = (p == 0 || !Syntax::IsRegular(static_cast<unsigned char>(data[p - 1]))) &&
                                     p + 6 < size && (data[p + 6] == '\r' || data[p + 6] == '\n');
                if (keyword) {
                    const size_t end = FindFrom(data, p + 6, "endstream", 8);
                    if (end != npos) pos = end + 9;
                }
            } else {
                pos = p + 7;
                const bool keyword = (p == 0 || !Syntax::IsRegular(static_cast<unsigned char>(data[p - 1]))) &&
                                     (p + 7 >= size || !Syntax::IsRegular(static_cast<unsigned char>(data[p + 7])));
                if (keyword) trailerKeywords.push_back(p);
            }
        }
        if (!hits.empty()) {
            scan.resize(static_cast<size_t>(highest) + 1);
            for (const ScanHit& hit : hits) {
                ScanSlot& slot = scan[hit.num];
                slot.offset = hit.offset;
                slot.gen = hit.gen;
                slot.present = true;
            }
        }
    }

    // "N G" before the "obj" at `at` (white space between, a non-regular byte before N).
    bool ObjectHeaderBefore(size_t at, ScanHit& hit) const {
        const std::string_view data = bytes;
        if (at + 3 < data.size() && Syntax::IsRegular(static_cast<unsigned char>(data[at + 3]))) return false;
        size_t q = at;
        const auto white = [&](size_t i) { return Syntax::IsWhite(static_cast<unsigned char>(data[i])); };
        if (q == 0 || !white(q - 1)) return false;
        while (q > 0 && white(q - 1)) q--;
        const size_t genEnd = q;
        while (q > 0 && IsDigit(data[q - 1]) && genEnd - q < 6) q--;
        if (q == genEnd || genEnd - q > 5) return false;
        const size_t genStart = q;
        if (q == 0 || !white(q - 1)) return false;
        while (q > 0 && white(q - 1)) q--;
        const size_t numEnd = q;
        while (q > 0 && IsDigit(data[q - 1]) && numEnd - q < 11) q--;
        if (q == numEnd || numEnd - q > 10) return false;
        if (q > 0 && Syntax::IsRegular(static_cast<unsigned char>(data[q - 1]))) return false;
        uint64_t num = 0;
        uint64_t gen = 0;
        if (!ReadUnsigned(data.substr(q, numEnd - q), num) ||
            !ReadUnsigned(data.substr(genStart, genEnd - genStart), gen)) {
            return false;
        }
        if (num == 0 || num >= numLimit || gen > 65535) return false;
        hit.offset = q;
        hit.num = static_cast<uint32_t>(num);
        hit.gen = static_cast<uint32_t>(gen);
        return true;
    }

    // The bytes of the object the scan found at hits[i], up to kTypeWindow: where to look for a
    // /Type name before parsing it.
    std::string_view HitWindow(size_t i) const {
        const size_t start = static_cast<size_t>(hits[i].offset);
        size_t end = i + 1 < hits.size() ? static_cast<size_t>(hits[i + 1].offset) : bytes.size();
        end = std::min(end, start + kTypeWindow);
        return bytes.substr(start, end > start ? end - start : 0);
    }

    bool Winning(size_t i) const {
        const ScanHit& hit = hits[i];
        return hit.num < scan.size() && scan[hit.num].offset == hit.offset;
    }

    // The slots from the scan, the trailer from the last `trailer` dictionary with /Root, else the
    // last cross-reference stream's dictionary with /Root. False when no object was found.
    bool FullRepair() {
        EnsureScan();
        if (hits.empty()) return false;
        fullRepair = true;
        report.repaired = true;
        slots.assign(scan.size(), Slot());
        for (size_t num = 1; num < scan.size(); num++) {
            if (!scan[num].present) continue;
            slots[num].kind = SlotKind::Offset;
            slots[num].pos = scan[num].offset;
            slots[num].aux = scan[num].gen;
        }
        objectStreams.clear();
        catalog = nullptr;
        trailer = PdfValue();
        PdfValue fallback;
        for (size_t k = trailerKeywords.size(); k-- > 0;) {
            Cursor c{bytes, trailerKeywords[k] + 7};
            Syntax::SkipSpace(c);
            PdfValue dict;
            if (!Syntax::ParseValue(c, Syntax::Mode::File, 0, builder, dict) || dict.type != PdfValue::Type::Dict) {
                continue;
            }
            if (dict.Find("Root")) {
                trailer = dict;
                break;
            }
            if (fallback.IsNull()) fallback = dict;
        }
        if (trailer.IsNull()) {
            for (size_t i = hits.size(); i-- > 0;) {
                if (FindFrom(HitWindow(i), 0, "/XRef", 4) == std::string_view::npos) continue;
                PdfValue stream;
                uint32_t num = 0;
                uint32_t gen = 0;
                if (!ParseIndirect(hits[i].offset, hits[i].num, false, false, stream, num, gen)) continue;
                if (!stream.IsDict() || !Direct(stream, "Type").IsName("XRef") || !stream.Find("Root")) continue;
                trailer = stream;
                trailer.type = PdfValue::Type::Dict;
                trailer.streamOffset = 0;
                trailer.streamLength = 0;
                break;
            }
        }
        if (trailer.IsNull()) trailer = fallback;
        return true;
    }

    // After the security handler is set up: the objects of every object stream the scan found
    // (direct definitions win; a later stream wins over an earlier one).
    void RegisterObjectStreams() {
        for (size_t i = 0; i < hits.size(); i++) {
            if (!Winning(i) || FindFrom(HitWindow(i), 0, "/ObjStm", 6) == std::string_view::npos) continue;
            const uint32_t container = hits[i].num;
            ObjectStream* stream = LoadObjectStream(container);
            if (!stream || !stream->ok) continue;
            for (size_t k = 0; k < stream->index.size(); k++) {
                const uint32_t num = stream->index[k].first;
                if (num == 0 || num >= numLimit || num == container) continue;
                if (num >= slots.size()) slots.resize(static_cast<size_t>(num) + 1);
                Slot& slot = slots[num];
                if (slot.kind == SlotKind::Offset || slot.state != SlotState::Unread) continue;
                slot.kind = SlotKind::Compressed;
                slot.pos = container;
                slot.aux = static_cast<uint32_t>(k);
            }
        }
    }

    // The objects of numbers no readable section gave, from the scan (an older section was lost).
    void FillFromScan() {
        EnsureScan();
        for (size_t num = 1; num < scan.size(); num++) {
            if (!scan[num].present) continue;
            if (num >= slots.size()) slots.resize(num + 1);
            if (slots[num].kind != SlotKind::None) continue;
            slots[num].kind = SlotKind::Offset;
            slots[num].pos = scan[num].offset;
            slots[num].aux = scan[num].gen;
            report.repaired = true;
        }
    }

    bool UsableCatalog(const PdfValue& value) { return value.IsDict() && Get(value, "Pages").IsDict(); }

    // A catalog by its /Type: the last one in the file with a page tree, else any object.
    const PdfValue* FindCatalog() {
        for (size_t i = hits.size(); i-- > 0;) {
            if (!Winning(i) || FindFrom(HitWindow(i), 0, "/Catalog", 3) == std::string_view::npos) continue;
            const PdfValue& value = Object(hits[i].num);
            if (Get(value, "Type").IsName("Catalog") && UsableCatalog(value)) return &value;
        }
        for (size_t num = slots.size(); num-- > 1;) {
            if (slots[num].kind != SlotKind::Compressed) continue;
            const PdfValue& value = Object(static_cast<uint32_t>(num));
            if (value.IsDict() && Get(value, "Type").IsName("Catalog") && UsableCatalog(value)) return &value;
        }
        return nullptr;
    }

    // ---------------------------------------------------------------- encryption

    OpenStatus SetUpEncryption(std::string_view password) {
        if (encrypted) return OpenStatus::Ok;
        const PdfValue* entry = trailer.Find("Encrypt");
        if (!entry || entry->IsNull()) return OpenStatus::Ok;
        encryptNum = entry->IsRef() ? entry->refNum : 0;
        const PdfValue& dict = Resolve(*entry);
        if (!dict.IsDict()) return OpenStatus::Corrupt;
        encryptDict = &dict;
        Crypt::EncryptParams params;
        params.filter = Get(dict, "Filter").Name();
        params.v = static_cast<int>(Get(dict, "V").Int(0));
        params.r = static_cast<int>(Get(dict, "R").Int(0));
        params.lengthBits = static_cast<int>(Get(dict, "Length").Int(40));
        params.o = Get(dict, "O").String();
        params.u = Get(dict, "U").String();
        params.oe = Get(dict, "OE").String();
        params.ue = Get(dict, "UE").String();
        params.perms = Get(dict, "Perms").String();
        params.p = static_cast<int32_t>(static_cast<uint32_t>(Get(dict, "P").Int(0) & 0xFFFFFFFF));
        params.encryptMetadata = Get(dict, "EncryptMetadata").Bool(true);
        if (params.v >= 4) {
            if (!CryptFilterFor(Get(dict, "StmF").Name(), params.stream) ||
                !CryptFilterFor(Get(dict, "StrF").Name(), params.string)) {
                return OpenStatus::Unsupported;
            }
        }
        const PdfValue& id = Get(trailer, "ID");
        if (id.IsArray() && !id.Array().empty()) params.id0 = Resolve(id.Array()[0]).String();
        switch (security.Open(params, password)) {
            case Crypt::Status::Ok: break;
            case Crypt::Status::PasswordRequired: return OpenStatus::PasswordRequired;
            case Crypt::Status::Unsupported: return OpenStatus::Unsupported;
            case Crypt::Status::Corrupt: return OpenStatus::Corrupt;
        }
        encrypted = true;
        report.encrypted = true;
        return OpenStatus::Ok;
    }

    // ---------------------------------------------------------------- pages

    static int NormalizeRotate(int64_t rotate) {
        int64_t r = rotate % 360;
        if (r < 0) r += 360;
        return r % 90 == 0 ? static_cast<int>(r) : 0;
    }

    static Inherited Own(const PdfValue& node, Inherited inherited) {
        if (const PdfValue* v = node.Find("Resources")) inherited.resources = v;
        if (const PdfValue* v = node.Find("MediaBox")) inherited.mediaBox = v;
        if (const PdfValue* v = node.Find("CropBox")) inherited.cropBox = v;
        if (const PdfValue* v = node.Find("Rotate")) inherited.rotate = v;
        return inherited;
    }

    void AddPage(const PdfValue& node, const Inherited& inherited) {
        Page page;
        page.dict = &node;
        page.inherited = inherited;
        pages.push_back(page);
    }

    // Page `index` (in range) with its attributes resolved (under the parse lock while shared).
    const Page& ResolvedPage(size_t index) {
        if (RAYOPDF_UNLIKELY(shared)) {
            std::lock_guard<std::recursive_mutex> lock(parseMutex);
            return ResolvePage(index);
        }
        return ResolvePage(index);
    }

    const Page& ResolvePage(size_t index) {
        if (pages[index].resolved) return pages[index];
        // Resolving parses objects but never adds pages: `pages` stays put.
        const Inherited inherited = pages[index].inherited;
        const PdfValue* resources = nullptr;
        if (inherited.resources) {
            const PdfValue& value = Resolve(*inherited.resources);
            if (value.IsDict()) resources = &value;
        }
        PdfRect box;
        PdfRect mediaBox{0, 0, 612, 792};
        if (inherited.mediaBox && Rect(*inherited.mediaBox, box) && box.Width() > 0 && box.Height() > 0) {
            mediaBox = box;
        }
        PdfRect cropBox = mediaBox;
        if (inherited.cropBox && Rect(*inherited.cropBox, box)) {
            const PdfRect clipped = Intersect(box, mediaBox);
            if (clipped.Width() > 0 && clipped.Height() > 0) cropBox = clipped;
        }
        const int rotate = inherited.rotate ? NormalizeRotate(Resolve(*inherited.rotate).Int(0)) : 0;
        Page& page = pages[index];
        page.resources = resources;
        page.mediaBox = mediaBox;
        page.cropBox = cropBox;
        page.rotate = rotate;
        page.resolved = true;
        if (shared) undoPages.push_back(index);
        return page;
    }

    bool IsLeaf(const PdfValue& node) {
        const PdfValue& type = Get(node, "Type");
        if (type.IsName("Page")) return true;
        if (type.IsName("Pages")) return false;
        return !Get(node, "Kids").IsArray();
    }

    // The page tree, iteratively: Pages nodes are visited once (a cycle or a node reached twice
    // is skipped), at most kMaxTreeDepth deep; a leaf may be listed twice.
    void WalkPageTree(const PdfValue& root) {
        const PdfValue* pagesEntry = root.Find("Pages");
        if (!pagesEntry) return;
        const PdfValue& top = Resolve(*pagesEntry);
        if (!top.IsDict()) return;
        if (IsLeaf(top)) {
            AddPage(top, Own(top, Inherited()));
            return;
        }
        std::vector<uint8_t> visited(slots.size(), 0);
        if (pagesEntry->IsRef() && pagesEntry->refNum < visited.size()) visited[pagesEntry->refNum] = 1;
        struct Frame {
            PdfSpan<PdfValue> kids;
            size_t next;
            Inherited inherited;
        };
        std::vector<Frame> stack;
        stack.push_back({Get(top, "Kids").Array(), 0, Own(top, Inherited())});
        while (!stack.empty()) {
            Frame& frame = stack.back();
            if (frame.next >= frame.kids.size()) {
                stack.pop_back();
                continue;
            }
            const PdfValue& kid = frame.kids[frame.next++];
            const Inherited inherited = frame.inherited;
            const PdfValue& child = Resolve(kid);
            if (!child.IsDict()) continue;
            if (IsLeaf(child)) {
                if (pages.size() >= kMaxPages) {
                    report.limitHits++;
                    return;
                }
                AddPage(child, Own(child, inherited));
                continue;
            }
            if (kid.IsRef() && kid.refNum < visited.size()) {
                if (visited[kid.refNum]) {
                    report.limitHits++;
                    continue;
                }
                visited[kid.refNum] = 1;
            }
            if (stack.size() >= kMaxTreeDepth) {
                report.limitHits++;
                continue;
            }
            stack.push_back({Get(child, "Kids").Array(), 0, Own(child, inherited)});
        }
    }

    bool BuildPages() {
        pages.clear();
        const PdfValue& root = Get(trailer, "Root");
        catalog = UsableCatalog(root) ? &root : nullptr;
        if (!catalog && fullRepair) catalog = FindCatalog();
        if (!catalog && root.IsDict()) catalog = &root;
        if (catalog) WalkPageTree(*catalog);
        return !pages.empty();
    }

    // The fallback when the page tree gives no page: every /Type /Page object in object-number
    // order, with what it inherits through /Parent.
    void PagesFromObjects() {
        for (size_t num = 1; num < slots.size() && pages.size() < kMaxPages; num++) {
            const PdfValue& value = Object(static_cast<uint32_t>(num));
            if (!value.IsDict() || !Get(value, "Type").IsName("Page")) continue;
            Inherited inherited = Own(value, Inherited());
            const PdfValue* node = &value;
            for (size_t depth = 0; depth < kMaxTreeDepth; depth++) {
                const PdfValue& parent = Get(*node, "Parent");
                if (!parent.IsDict() || &parent == node) break;
                Inherited up = Own(parent, Inherited());
                if (!inherited.resources) inherited.resources = up.resources;
                if (!inherited.mediaBox) inherited.mediaBox = up.mediaBox;
                if (!inherited.cropBox) inherited.cropBox = up.cropBox;
                if (!inherited.rotate) inherited.rotate = up.rotate;
                node = &parent;
            }
            AddPage(value, inherited);
        }
        if (!pages.empty()) report.repaired = true;
    }

    void Finish() {
        version = headerVersion;
        if (catalog) {
            const std::string_view declared = Get(*catalog, "Version").Name();
            const auto parse = [](std::string_view v, int& major, int& minor) {
                major = minor = 0;
                size_t i = 0;
                bool any = false;
                for (; i < v.size() && IsDigit(v[i]) && major < 1000; i++, any = true) {
                    major = major * 10 + (v[i] - '0');
                }
                if (!any || i >= v.size() || v[i] != '.') return false;
                for (i++; i < v.size() && IsDigit(v[i]) && minor < 1000; i++) minor = minor * 10 + (v[i] - '0');
                return true;
            };
            int a = 0, b = 0, c = 0, d = 0;
            if (parse(declared, c, d) && (!parse(headerVersion, a, b) || c > a || (c == a && d > b))) {
                version = declared;
            }
        }
        if (headerPos != std::string_view::npos) {
            // A linearized file's first object, right after the header, holds /Linearized. It is
            // parsed whatever it holds: a broken one counts in the report (--report shows it).
            Cursor c{bytes, headerPos};
            Syntax::SkipSpace(c);
            const size_t at = c.pos;
            uint64_t num = 0;
            if (ReadUnsigned(Syntax::ReadToken(c), num) && num < slots.size() && slots[num].kind == SlotKind::Offset &&
                slots[num].pos == at) {
                report.linearized = Object(static_cast<uint32_t>(num)).Find("Linearized") != nullptr;
            }
        }
    }
};

Document::Document(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Document::~Document() = default;

OpenStatus Document::Open(std::string_view bytes, std::string_view password, std::unique_ptr<Document>& out) {
    std::unique_ptr<Impl> impl(new Impl(bytes));
    Impl& d = *impl;
    d.numLimit = std::min<uint64_t>(kMaxObjects, bytes.size() / 2 + 65536);
    d.FindHeader();
    const bool chain = d.ReadXrefChain();
    if (d.tooLarge) return OpenStatus::TooLarge;
    if (!chain) {
        if (!d.FullRepair()) return d.headerPos == std::string_view::npos ? OpenStatus::NotPdf : OpenStatus::Corrupt;
    } else if (d.chainIncomplete) {
        d.FillFromScan();
    }
    OpenStatus status = d.SetUpEncryption(password);
    if (status != OpenStatus::Ok) return status;
    if (d.fullRepair) d.RegisterObjectStreams();
    if (!d.BuildPages()) {
        if (!d.fullRepair && d.FullRepair()) {
            status = d.SetUpEncryption(password);
            if (status != OpenStatus::Ok) return status;
            d.RegisterObjectStreams();
            d.BuildPages();
        }
        if (d.pages.empty()) d.PagesFromObjects();
        if (d.pages.empty()) {
            d.EnsureScan();
            return d.headerPos == std::string_view::npos && d.hits.empty() ? OpenStatus::NotPdf : OpenStatus::Corrupt;
        }
    }
    d.Finish();
    out.reset(new Document(std::move(impl)));
    return OpenStatus::Ok;
}

std::string_view Document::Bytes() const { return impl_->bytes; }
std::string_view Document::Version() const { return impl_->version; }
const PdfValue& Document::Trailer() const { return impl_->trailer; }
const PdfValue& Document::Catalog() const { return impl_->catalog ? *impl_->catalog : Null(); }

const PdfValue& Document::Info() const {
    const PdfValue& info = impl_->Get(impl_->trailer, "Info");
    return info.IsDict() ? info : Null();
}

uint32_t Document::ObjectCount() const { return static_cast<uint32_t>(impl_->slots.size()); }
const DocumentReport& Document::Report() const { return impl_->report; }
const PdfValue& Document::Object(uint32_t num) const { return impl_->Object(num); }
const PdfValue& Document::Resolve(const PdfValue& value) const { return impl_->Resolve(value); }
const PdfValue& Document::Get(const PdfValue& dict, std::string_view key) const { return impl_->Get(dict, key); }

bool Document::Number(const PdfValue& value, double& out) const {
    const PdfValue& resolved = impl_->Resolve(value);
    if (!resolved.IsNumber() || !std::isfinite(resolved.Number())) return false;
    out = resolved.Number();
    return true;
}

bool Document::Rect(const PdfValue& value, PdfRect& out) const { return impl_->Rect(value, out); }

StreamStatus Document::StreamData(const PdfValue& stream, std::string& out, size_t limit) const {
    return impl_->StreamData(stream, out, limit);
}

Document::StreamReader::~StreamReader() {
    if (reading_ && !counted_) Finish();
}

StreamStatus Document::StreamReader::Open(const Document& document, const PdfValue& stream, size_t limit) {
    if (reading_ && !counted_) Finish();
    impl_ = document.impl_.get();
    reading_ = false;
    counted_ = false;
    Stage stages[kMaxFilters];
    size_t count = 0;
    status_ = impl_->Prepare(stream, limit, stages, count, decrypted_, raw_, start_);
    if (status_ != StreamStatus::Ok) {
        counted_ = true;
        return status_;
    }
    stages_.assign(stages, stages + count);
    limit_ = limit;
    chain_.Start(stages_.data(), stages_.size(), raw_, limit_);
    reading_ = true;
    impl_->HoldBack(this);
    return StreamStatus::Ok;
}

bool Document::StreamReader::Next(std::string_view& piece) {
    if (!reading_) return false;
    if (chain_.Next(piece)) return true;
    if (!counted_) Count(Worse(start_, chain_.Status()), chain_.Size());
    return false;
}

void Document::StreamReader::Restart() {
    if (reading_) chain_.Restart();
}

StreamStatus Document::StreamReader::Finish() {
    if (!reading_) return status_;
    std::string_view piece;
    while (Next(piece)) {}
    return Worse(start_, chain_.Status());
}

void Document::StreamReader::Count(StreamStatus status, uint64_t size) {
    counted_ = true;
    impl_->Uncount(this, status, size);
}

int Document::PageCount() const { return static_cast<int>(impl_->pages.size()); }

const PdfValue& Document::PageDict(int index) const {
    if (index < 0 || static_cast<size_t>(index) >= impl_->pages.size()) return Null();
    return *impl_->pages[static_cast<size_t>(index)].dict;
}

PdfRect Document::MediaBox(int index) const {
    if (index < 0 || static_cast<size_t>(index) >= impl_->pages.size()) return PdfRect{0, 0, 612, 792};
    return impl_->ResolvedPage(static_cast<size_t>(index)).mediaBox;
}

PdfRect Document::CropBox(int index) const {
    if (index < 0 || static_cast<size_t>(index) >= impl_->pages.size()) return PdfRect{0, 0, 612, 792};
    return impl_->ResolvedPage(static_cast<size_t>(index)).cropBox;
}

int Document::Rotate(int index) const {
    if (index < 0 || static_cast<size_t>(index) >= impl_->pages.size()) return 0;
    return impl_->ResolvedPage(static_cast<size_t>(index)).rotate;
}

const PdfValue& Document::Resources(int index) const {
    if (index < 0 || static_cast<size_t>(index) >= impl_->pages.size()) return Null();
    const PdfValue* resources = impl_->ResolvedPage(static_cast<size_t>(index)).resources;
    return resources ? *resources : Null();
}

StreamStatus Document::PageContent(int index, std::string_view& out) const {
    out = {};
    Impl& d = *impl_;
    if (index < 0 || static_cast<size_t>(index) >= d.pages.size()) return StreamStatus::NotStream;
    PageBuffer& buffer = PerThread<PageBuffer>();
    if (buffer.bytes.Capacity() > kKeptContent) buffer.bytes.Release();
    const PdfValue& contents = d.Get(*d.pages[static_cast<size_t>(index)].dict, "Contents");
    if (contents.IsNull()) return StreamStatus::Ok;  // a blank page
    size_t size = 0;
    StreamStatus status = StreamStatus::Ok;
    if (!contents.IsArray()) {
        status = d.StreamDataInto(contents, buffer.bytes, 0, kDefaultStreamLimit, buffer.piece, size);
    } else {
        bool first = true;
        for (const PdfValue& item : contents.Array()) {
            if (size >= kDefaultStreamLimit) {
                status = Worse(status, StreamStatus::TooLarge);
                d.Hit(&DocumentReport::limitHits);
                break;
            }
            // A newline before every piece but the first, whatever the piece holds.
            const size_t at = first ? size : size + 1;
            size_t length = 0;
            const StreamStatus piece =
                d.StreamDataInto(item, buffer.bytes, at, kDefaultStreamLimit - size, buffer.piece, length);
            status = Worse(status, piece);
            if (!first) buffer.bytes.Grow(at)[size] = '\n';
            first = false;
            size = at + length;
        }
    }
    if (size > 0) out = std::string_view(reinterpret_cast<const char*>(buffer.bytes.Data()), size);
    return status;
}

bool Document::BeginShared() const {
    Impl& d = *impl_;
    if (!d.uncounted.empty()) return false;
    d.shared = true;
    d.orderSensitive.store(false, std::memory_order_relaxed);
    d.maxHeld = 0;
    d.savedReport = d.report;
    d.savedDecoded = d.decoded;
    return true;
}

bool Document::EndShared() const {
    Impl& d = *impl_;
    d.shared = false;
    PerThread<StreamScratch>() = StreamScratch();  // this thread's buffers go with the section, as the document's would
    // `decoded` + `maxHeld` below the budget: no check of a one-thread reading could have cut,
    // refused or counted held-back streams (Impl::Budget).
    const bool keep = !d.orderSensitive.load(std::memory_order_relaxed) && d.decoded < kDocumentDecodeBudget &&
                      d.maxHeld < kDocumentDecodeBudget - d.decoded;
    if (!keep) {
        for (auto it = d.undoSlots.rbegin(); it != d.undoSlots.rend(); ++it) d.slots[it->first] = it->second;
        for (const uint32_t container : d.undoStreams) d.objectStreams.erase(container);
        for (const size_t index : d.undoPages) d.pages[index].resolved = false;
        d.report = d.savedReport;
        d.decoded = d.savedDecoded;
    }
    d.undoSlots.clear();
    d.undoStreams.clear();
    d.undoPages.clear();
    return keep;
}

bool Document::SharedIntact() const {
    return !impl_->orderSensitive.load(std::memory_order_relaxed);
}

} // namespace RayoPdf
