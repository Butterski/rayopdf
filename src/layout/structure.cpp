// SPDX-License-Identifier: Apache-2.0
#include "layout/structure.h"

#include "common/minmax.h"
#include "layout/interpreter.h"
#include "layout/lines.h"
#include "layout/tables.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <unordered_map>

namespace RayoPdf::Layout {

const char* StructureUseName(StructureUse use) {
    switch (use) {
    case StructureUse::None: return "none";
    case StructureUse::Used: return "used";
    case StructureUse::Broken: return "broken";
    case StructureUse::RoleCycle: return "role-cycle";
    case StructureUse::Coverage: return "coverage";
    case StructureUse::Generic: return "generic";
    case StructureUse::Order: return "order";
    }
    return "none";
}

namespace {

constexpr uint32_t kNone = UINT32_MAX;
constexpr size_t kMaxRoleMapEntries = 4096;   // RoleMap and ClassMap entries read
constexpr size_t kMaxAttributeObjects = 64;   // attribute objects read per element
constexpr size_t kMaxTextString = 4096;       // bytes of one /Alt or /ActualText read

// Glyph flags inline elements imply.
enum : uint8_t {
    kStyleBold = 1u << 0,
    kStyleItalic = 1u << 1,
    kStyleMono = 1u << 2,
    kStyleStrike = 1u << 3,
};

struct StandardRole {
    std::string_view name;
    StructRole role;
    uint8_t level;
};

// ISO 32000-1 §14.8.4 and ISO 32000-2 §14.8.4 standard structure types. "Sub" is PDF 2.0's
// sub-division of a block (a line), not a subscript. Sect, Part and Art carry level 1: headings
// of type H take their level from how many of them enclose them.
constexpr StandardRole kStandardRoles[] = {
    {"Document", StructRole::Group, 0},  {"DocumentFragment", StructRole::Group, 0},
    {"Part", StructRole::Group, 1},      {"Art", StructRole::Group, 1},
    {"Sect", StructRole::Group, 1},      {"Div", StructRole::Group, 0},
    {"NonStruct", StructRole::Group, 0}, {"Private", StructRole::Group, 0},
    {"TOC", StructRole::Group, 0},       {"Index", StructRole::Group, 0},
    {"Aside", StructRole::Group, 0},     {"BlockQuote", StructRole::BlockQuote, 0},
    {"P", StructRole::Paragraph, 0},     {"Caption", StructRole::Paragraph, 0},
    {"TOCI", StructRole::Paragraph, 0},  {"BibEntry", StructRole::Paragraph, 0},
    {"Title", StructRole::Paragraph, 0}, {"H", StructRole::Heading, 0},
    {"H1", StructRole::Heading, 1},      {"H2", StructRole::Heading, 2},
    {"H3", StructRole::Heading, 3},      {"H4", StructRole::Heading, 4},
    {"H5", StructRole::Heading, 5},      {"H6", StructRole::Heading, 6},
    {"L", StructRole::List, 0},          {"LI", StructRole::ListItem, 0},
    {"Lbl", StructRole::Label, 0},       {"LBody", StructRole::ListBody, 0},
    {"Table", StructRole::Table, 0},     {"THead", StructRole::TableHead, 0},
    {"TBody", StructRole::TableBody, 0}, {"TFoot", StructRole::TableBody, 0},
    {"TR", StructRole::TableRow, 0},     {"TH", StructRole::TableHeader, 0},
    {"TD", StructRole::TableCell, 0},    {"Span", StructRole::Inline, 0},
    {"Quote", StructRole::Inline, 0},    {"Reference", StructRole::Inline, 0},
    {"Annot", StructRole::Inline, 0},    {"Ruby", StructRole::Inline, 0},
    {"RB", StructRole::Inline, 0},       {"RT", StructRole::Inline, 0},
    {"RP", StructRole::Inline, 0},       {"Warichu", StructRole::Inline, 0},
    {"WT", StructRole::Inline, 0},       {"WP", StructRole::Inline, 0},
    {"Sub", StructRole::Inline, 0},      {"Em", StructRole::Emphasis, 0},
    {"Strong", StructRole::Strong, 0},   {"Link", StructRole::Link, 0},
    {"Code", StructRole::Code, 0},       {"Note", StructRole::Note, 0},
    {"FENote", StructRole::Note, 0},     {"Figure", StructRole::Figure, 0},
    {"Formula", StructRole::Formula, 0}, {"Form", StructRole::Form, 0},
    {"Artifact", StructRole::Artifact, 0},
};

bool Standard(std::string_view name, StructRole& role, uint8_t& level) {
    for (const StandardRole& s : kStandardRoles) {
        if (s.name == name) {
            role = s.role;
            level = s.level;
            return true;
        }
    }
    // PDF 2.0 allows headings deeper than H6.
    if (name.size() >= 2 && name.size() <= 4 && name[0] == 'H') {
        bool digits = true;
        for (size_t i = 1; i < name.size(); ++i) digits = digits && name[i] >= '0' && name[i] <= '9';
        if (digits && name[1] != '0') {
            role = StructRole::Heading;
            level = 6;
            return true;
        }
    }
    return false;
}

class Reader {
public:
    Reader(const Document& document, int first, int last, Structure& out)
        : doc_(document), first_(first), last_(last), out_(out) {}
    void Run();

private:
    struct Frame {
        const PdfValue* kids;  // the K value: an array, or one kid
        size_t count;
        size_t next;
        uint32_t node;
        const PdfValue* page;  // the inherited /Pg, resolved (nullptr when none)
    };
    bool ResolveRole(std::string_view name, StructRole& role, uint8_t& level, int& cycle);
    void ReadAttributes(const PdfValue& element, StructNode& node);
    void ReadAttributeObject(const PdfValue& value, StructNode& node, size_t& budget);
    void AppendText(const PdfValue& value, uint32_t& at, uint32_t& length);
    uint32_t PageOf(const PdfValue* page) const;
    void AddItem(const PdfValue* page, uint32_t stream, int32_t mcid, uint32_t annotation, uint32_t node);
    void Visit(const PdfValue& kid, const Frame& frame, std::vector<Frame>& stack);
    void ResolvePending();

    const Document& doc_;
    const int first_, last_;
    Structure& out_;
    const PdfValue* roleMap_ = nullptr;
    const PdfValue* classMap_ = nullptr;
    std::unordered_map<std::string_view, std::pair<StructRole, uint8_t>> roles_;
    std::vector<std::pair<const PdfValue*, uint32_t>> pages_;  // page dictionary -> converted page index
    std::vector<uint8_t> visited_;                              // element object numbers seen
    std::vector<uint32_t> nodeObject_;                          // per node: its object number (0: direct)
    struct Pending {
        uint32_t node;
        int32_t mcid;
        uint32_t seq;
    };
    std::vector<Pending> pending_;  // MCIDs whose page /Pg does not say
    uint32_t seq_ = 0;
    bool limit_ = false;
};

bool Reader::ResolveRole(std::string_view name, StructRole& role, uint8_t& level, int& cycle) {
    const auto hit = roles_.find(name);
    if (hit != roles_.end()) {
        role = hit->second.first;
        level = hit->second.second;
        return true;
    }
    // Standard names first: PDF/UA forbids remapping them, and a RoleMap entry for one (typst maps
    // PDF 2.0's Strong and Em to Span for PDF 1.7 readers) would only lose meaning.
    std::string_view current = name;
    std::string_view seen[kMaxRoleMapChain + 1];
    uint32_t hops = 0;
    role = StructRole::Unknown;
    level = 0;
    while (true) {
        if (Standard(current, role, level)) break;
        if (!roleMap_) break;
        const PdfValue& next = doc_.Get(*roleMap_, current);
        if (!next.IsName()) break;
        seen[hops++] = current;
        const std::string_view target = next.Name();
        for (uint32_t i = 0; i < hops; ++i)
            if (seen[i] == target) cycle = 1;
        if (cycle || hops >= kMaxRoleMapChain) {
            cycle = 1;
            role = StructRole::Unknown;
            level = 0;
            return false;
        }
        current = target;
    }
    if (roles_.size() < kMaxRoleMapEntries * 4) roles_.emplace(name, std::make_pair(role, level));
    return true;
}

void Reader::AppendText(const PdfValue& value, uint32_t& at, uint32_t& length) {
    const PdfValue& v = doc_.Resolve(value);
    if (!v.IsString() || out_.text.size() >= kMaxStructureText) return;
    at = static_cast<uint32_t>(out_.text.size());
    AppendTextString(v.String().substr(0, kMaxTextString), out_.text);
    length = static_cast<uint32_t>(out_.text.size() - at);
}

void Reader::ReadAttributeObject(const PdfValue& value, StructNode& node, size_t& budget) {
    const PdfValue& a = doc_.Resolve(value);
    if (!a.IsDict() || budget == 0) return;
    budget--;
    if (const PdfValue* v = a.Find("RowSpan")) {
        const int64_t n = doc_.Resolve(*v).Int(1);
        node.rowSpan = static_cast<uint16_t>(n >= 1 && n <= 1000 ? n : 1);
    }
    if (const PdfValue* v = a.Find("ColSpan")) {
        const int64_t n = doc_.Resolve(*v).Int(1);
        node.colSpan = static_cast<uint16_t>(n >= 1 && n <= 1000 ? n : 1);
    }
    if (const PdfValue* v = a.Find("TextDecorationType")) {
        if (doc_.Resolve(*v).IsName("LineThrough")) node.flags |= kNodeLineThrough;
    }
    if (const PdfValue* v = a.Find("Placement")) {
        const PdfValue& p = doc_.Resolve(*v);
        if (p.IsName("Block")) node.flags |= kNodeBlock;
        if (p.IsName("Inline")) node.flags |= kNodeInline;
    }
    if (const PdfValue* v = a.Find("StartIndent")) {
        if (doc_.Resolve(*v).Number(0) > 0) node.flags |= kNodeStartIndent;
    }
    if (const PdfValue* v = a.Find("EndIndent")) {
        if (doc_.Resolve(*v).Number(0) > 0) node.flags |= kNodeEndIndent;
    }
    if (const PdfValue* v = a.Find("ListNumbering")) {
        const std::string_view n = doc_.Resolve(*v).Name();
        if (n == "Decimal" || n == "UpperRoman" || n == "LowerRoman" || n == "UpperAlpha" || n == "LowerAlpha")
            node.flags |= kNodeOrdered;
        else if (n == "Disc" || n == "Circle" || n == "Square")
            node.flags |= kNodeUnordered;
    }
}

void Reader::ReadAttributes(const PdfValue& element, StructNode& node) {
    size_t budget = kMaxAttributeObjects;
    // Attribute objects (/A): one, or an array of them, each maybe followed by a revision number.
    if (const PdfValue* a = element.Find("A")) {
        const PdfValue& v = doc_.Resolve(*a);
        if (v.IsArray()) {
            for (const PdfValue& item : v.Array()) ReadAttributeObject(item, node, budget);
        } else {
            ReadAttributeObject(v, node, budget);
        }
    }
    // Attribute classes (/C) through the ClassMap.
    if (const PdfValue* c = element.Find("C"); c && classMap_) {
        const PdfValue& v = doc_.Resolve(*c);
        const auto one = [&](const PdfValue& name) {
            if (!name.IsName()) return;
            const PdfValue& objects = doc_.Get(*classMap_, name.Name());
            if (objects.IsArray()) {
                for (const PdfValue& item : objects.Array()) ReadAttributeObject(item, node, budget);
            } else {
                ReadAttributeObject(objects, node, budget);
            }
        };
        if (v.IsArray()) {
            for (const PdfValue& item : v.Array()) one(doc_.Resolve(item));
        } else {
            one(v);
        }
    }
}

uint32_t Reader::PageOf(const PdfValue* page) const {
    if (!page) return kNone;
    const auto it = std::lower_bound(pages_.begin(), pages_.end(), page,
                                     [](const std::pair<const PdfValue*, uint32_t>& e, const PdfValue* p) {
                                         return std::less<const PdfValue*>()(e.first, p);
                                     });
    return it != pages_.end() && it->first == page ? it->second : kNone;
}

void Reader::AddItem(const PdfValue* page, uint32_t stream, int32_t mcid, uint32_t annotation, uint32_t node) {
    if (out_.items.size() >= kMaxStructureItems) {
        limit_ = true;
        return;
    }
    const uint32_t seq = seq_++;
    if (!page) {
        // The page comes from the ParentTree, for page content only.
        if (mcid >= 0 && stream == 0) pending_.push_back(Pending{node, mcid, seq});
        return;
    }
    const uint32_t p = PageOf(page);
    if (p == kNone) return;  // a page outside the converted range
    StructItem item;
    item.page = p;
    item.stream = stream;
    item.mcid = mcid;
    item.annotation = annotation;
    item.node = node;
    item.seq = seq;
    out_.items.push_back(item);
}

void Reader::Visit(const PdfValue& kid, const Frame& frame, std::vector<Frame>& stack) {
    if (kid.type == PdfValue::Type::Int) {
        if (kid.integer >= 0 && kid.integer < INT32_MAX && frame.node != kNone)
            AddItem(frame.page, 0, static_cast<int32_t>(kid.integer), 0, frame.node);
        return;
    }
    const PdfValue& value = doc_.Resolve(kid);
    if (!value.IsDict()) return;
    const PdfValue& type = doc_.Get(value, "Type");
    if (type.IsName("MCR")) {
        const PdfValue& mcid = doc_.Get(value, "MCID");
        if (mcid.type != PdfValue::Type::Int || mcid.integer < 0 || mcid.integer >= INT32_MAX || frame.node == kNone)
            return;
        const PdfValue* page = frame.page;
        if (const PdfValue* pg = value.Find("Pg")) page = &doc_.Resolve(*pg);
        uint32_t stream = 0;
        if (const PdfValue* stm = value.Find("Stm")) {
            if (!stm->IsRef()) return;
            stream = stm->refNum;
        }
        AddItem(page, stream, static_cast<int32_t>(mcid.integer), 0, frame.node);
        return;
    }
    if (type.IsName("OBJR")) {
        const PdfValue* obj = value.Find("Obj");
        if (!obj || !obj->IsRef() || frame.node == kNone) return;
        const PdfValue* page = frame.page;
        if (const PdfValue* pg = value.Find("Pg")) page = &doc_.Resolve(*pg);
        if (page) AddItem(page, 0, -1, obj->refNum, frame.node);
        return;
    }
    const PdfValue& s = doc_.Get(value, "S");
    if (!s.IsName()) return;
    // An element reached a second time (a shared kid, or a cycle) is read once.
    if (kid.IsRef()) {
        const uint32_t num = kid.refNum;
        if (num >= visited_.size() * 8) return;
        uint8_t& bits = visited_[num >> 3];
        const uint8_t bit = static_cast<uint8_t>(1u << (num & 7));
        if (bits & bit) return;
        bits |= bit;
    }
    if (out_.nodes.size() >= kMaxStructureNodes) {
        limit_ = true;
        return;
    }
    StructNode node;
    node.parent = frame.node;
    int cycle = 0;
    ResolveRole(s.Name(), node.role, node.level, cycle);
    if (cycle) out_.use = StructureUse::RoleCycle;
    ReadAttributes(value, node);
    if (const PdfValue* alt = value.Find("Alt")) AppendText(*alt, node.alt, node.altLength);
    if (const PdfValue* actual = value.Find("ActualText")) AppendText(*actual, node.actual, node.actualLength);
    const uint32_t index = static_cast<uint32_t>(out_.nodes.size());
    out_.nodes.push_back(node);
    nodeObject_.push_back(kid.IsRef() ? kid.refNum : 0);
    const PdfValue* page = frame.page;
    if (const PdfValue* pg = value.Find("Pg")) page = &doc_.Resolve(*pg);
    const PdfValue* k = value.Find("K");
    if (!k) return;
    if (stack.size() >= kMaxStructureDepth) {
        limit_ = true;
        return;
    }
    const PdfValue& kids = doc_.Resolve(*k);
    // An array of kids, or one kid (kept unresolved: an integer, a dictionary or a reference).
    if (kids.IsArray())
        stack.push_back(Frame{kids.Array().begin(), kids.Array().size(), 0, index, page});
    else
        stack.push_back(Frame{k, 1, 0, index, page});
}

void Reader::ResolvePending() {
    if (pending_.empty()) return;
    const PdfValue& root = doc_.Get(doc_.Catalog(), "StructTreeRoot");
    const PdfValue& parentTree = doc_.Get(root, "ParentTree");
    if (!parentTree.IsDict()) return;
    // The converted pages by their /StructParents key.
    std::unordered_map<int64_t, uint32_t> keys;
    for (int i = first_; i <= last_; ++i) {
        const PdfValue& key = doc_.Get(doc_.PageDict(i), "StructParents");
        if (key.type == PdfValue::Type::Int) keys.emplace(key.integer, static_cast<uint32_t>(i - first_));
    }
    if (keys.empty()) return;
    // (element object number, mcid) -> page, from the ParentTree arrays of those pages.
    std::unordered_map<uint64_t, uint32_t> owners;
    size_t budget = kMaxStructureItems;
    struct Level {
        const PdfValue* node;
        uint32_t depth;
    };
    std::vector<Level> todo{{&parentTree, 0}};
    while (!todo.empty() && budget > 0) {
        const Level level = todo.back();
        todo.pop_back();
        const PdfValue& nums = doc_.Get(*level.node, "Nums");
        if (nums.IsArray()) {
            const PdfSpan<PdfValue> entries = nums.Array();
            for (size_t i = 0; i + 1 < entries.size() && budget > 0; i += 2) {
                const PdfValue& key = doc_.Resolve(entries[i]);
                if (key.type != PdfValue::Type::Int) continue;
                const auto page = keys.find(key.integer);
                if (page == keys.end()) continue;
                const PdfValue& array = doc_.Resolve(entries[i + 1]);
                const PdfSpan<PdfValue> elements = array.Array();
                for (size_t m = 0; m < elements.size() && budget > 0; ++m, --budget) {
                    if (!elements[m].IsRef()) continue;
                    owners.emplace((static_cast<uint64_t>(elements[m].refNum) << 32) | m, page->second);
                }
            }
        }
        const PdfValue& kids = doc_.Get(*level.node, "Kids");
        if (level.depth < kMaxNumberTreeDepth) {
            for (const PdfValue& kid : kids.Array()) {
                const PdfValue& child = doc_.Resolve(kid);
                if (child.IsDict() && budget > 0) {
                    budget--;
                    todo.push_back(Level{&child, level.depth + 1});
                }
            }
        }
    }
    for (const Pending& p : pending_) {
        const uint32_t object = nodeObject_[p.node];
        if (object == 0) continue;
        const auto it = owners.find((static_cast<uint64_t>(object) << 32) | static_cast<uint32_t>(p.mcid));
        if (it == owners.end()) continue;
        StructItem item;
        item.page = it->second;
        item.mcid = p.mcid;
        item.node = p.node;
        item.seq = p.seq;
        out_.items.push_back(item);
    }
}

void Reader::Run() {
    const PdfValue& root = doc_.Get(doc_.Catalog(), "StructTreeRoot");
    if (!root.IsDict()) return;
    out_.use = StructureUse::Used;
    const PdfValue& roleMap = doc_.Get(root, "RoleMap");
    if (roleMap.IsDict()) roleMap_ = &roleMap;
    const PdfValue& classMap = doc_.Get(root, "ClassMap");
    if (classMap.IsDict()) classMap_ = &classMap;
    // Check 4: no RoleMap entry leads into a cycle.
    if (roleMap_) {
        size_t n = 0;
        for (const PdfDictEntry& e : roleMap_->Entries()) {
            if (++n > kMaxRoleMapEntries) break;
            StructRole role;
            uint8_t level;
            int cycle = 0;
            ResolveRole(e.key, role, level, cycle);
            if (cycle) {
                out_.use = StructureUse::RoleCycle;
                return;
            }
        }
    }
    for (int i = first_; i <= last_; ++i) pages_.emplace_back(&doc_.PageDict(i), static_cast<uint32_t>(i - first_));
    std::sort(pages_.begin(), pages_.end(), [](const std::pair<const PdfValue*, uint32_t>& a,
                                               const std::pair<const PdfValue*, uint32_t>& b) {
        return std::less<const PdfValue*>()(a.first, b.first);
    });
    visited_.assign((static_cast<size_t>(doc_.ObjectCount()) + 7) / 8, 0);

    std::vector<Frame> stack;
    const PdfValue* k = root.Find("K");
    if (!k) {
        out_.use = StructureUse::Broken;
        return;
    }
    const PdfValue& kids = doc_.Resolve(*k);
    if (kids.IsArray())
        stack.push_back(Frame{kids.Array().begin(), kids.Array().size(), 0, kNone, nullptr});
    else
        stack.push_back(Frame{k, 1, 0, kNone, nullptr});
    // Depth-first, kids in their order: the logical order (§14.8.2.3.1). Every element is read once
    // and the node count is capped, so the walk ends.
    while (!stack.empty() && !limit_) {
        Frame& top = stack.back();
        if (top.next >= top.count) {
            stack.pop_back();
            continue;
        }
        const PdfValue& kid = top.kids[top.next++];
        const Frame frame = top;  // Visit may grow the stack
        Visit(kid, frame, stack);
    }
    if (out_.use == StructureUse::RoleCycle) return;
    if (limit_) {
        out_.use = StructureUse::Broken;
        return;
    }
    ResolvePending();
    // One element per content item: the first in the tree's order.
    std::sort(out_.items.begin(), out_.items.end(), [](const StructItem& a, const StructItem& b) {
        if (a.page != b.page) return a.page < b.page;
        if (a.stream != b.stream) return a.stream < b.stream;
        if (a.mcid != b.mcid) return a.mcid < b.mcid;
        return a.seq < b.seq;
    });
    size_t kept = 0;
    for (size_t i = 0; i < out_.items.size(); ++i) {
        const StructItem& it = out_.items[i];
        if (kept > 0 && it.mcid >= 0) {
            const StructItem& last = out_.items[kept - 1];
            if (last.page == it.page && last.stream == it.stream && last.mcid == it.mcid) continue;
        }
        out_.items[kept++] = it;
    }
    out_.items.resize(kept);
    const uint32_t pageCount = last_ >= first_ ? static_cast<uint32_t>(last_ - first_ + 1) : 0;
    out_.pageItems.assign(pageCount + 1, 0);
    size_t at = 0;
    for (uint32_t p = 0; p <= pageCount; ++p) {
        while (at < out_.items.size() && out_.items[at].page < p) ++at;
        out_.pageItems[p] = static_cast<uint32_t>(at);
    }
    if (out_.items.empty()) out_.use = StructureUse::Broken;
}

}  // namespace

void ReadStructure(const Document& document, int first, int last, Structure& out) {
    out = Structure();
    Reader reader(document, first, last, out);
    reader.Run();
}

// ------------------------------------------------------------------------------------------------
// The tagged path.

namespace {

enum class OwnerKind : uint8_t { Paragraph, Heading, ListItem, Code, Cell, Figure, Note, Table };

constexpr uint32_t kLabelBit = 0x80000000u;
constexpr uint32_t kNoOwner = 0x7FFFFFFFu;

// The item of (stream, mcid) on page `p`, or nullptr.
const StructItem* FindItem(const Structure& s, uint32_t p, uint32_t stream, int32_t mcid) {
    if (p + 1 >= s.pageItems.size()) return nullptr;
    const StructItem* begin = s.items.data() + s.pageItems[p];
    const StructItem* end = s.items.data() + s.pageItems[p + 1];
    const StructItem* it = std::lower_bound(begin, end, std::make_pair(stream, mcid),
                                            [](const StructItem& a, const std::pair<uint32_t, int32_t>& k) {
                                                return a.stream != k.first ? a.stream < k.first : a.mcid < k.second;
                                            });
    return it != end && it->stream == stream && it->mcid == mcid ? it : nullptr;
}

// The stream object number a glyph's or image's mcid counts in; kNone when unknown.
uint32_t StreamOf(const PageData& page, uint8_t mcStream) {
    if (mcStream == 0) return 0;
    return mcStream <= page.markStreams.size() ? page.markStreams[mcStream - 1] : kNone;
}

// Glyphs that check 1 counts: visible text.
bool Counted(const Glyph& g) {
    return !(g.flags & (kGlyphInvisible | kGlyphClipped | kGlyphDropped | kGlyphSpace | kGlyphImage | kGlyphArtifact));
}

// A glyph that goes with the text around it (a space, an inline image) rather than splitting it.
bool Neutral(const Glyph& g) { return (g.flags & (kGlyphSpace | kGlyphImage)) != 0; }

void MeasureLine(const PageData& page, Line& line) {
    uint16_t all = kGlyphBold | kGlyphItalic | kGlyphMonospace | kGlyphArtifact | kGlyphInvisible | kGlyphOcr;
    line.chars = line.count;
    for (uint32_t k = 0; k < line.count; ++k) {
        const Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
        if (k == 0) line.u0 = g.u0, line.u1 = g.u1, line.vb = g.vb, line.vt = g.vt;
        line.u0 = Fmin(line.u0, g.u0), line.u1 = Fmax(line.u1, g.u1);
        line.vb = Fmin(line.vb, g.vb), line.vt = Fmax(line.vt, g.vt);
        all &= g.flags;
    }
    line.flags = static_cast<uint16_t>(line.flags & ~(kLineAllBold | kLineAllItalic | kLineAllMono | kLineArtifact |
                                                     kLineInvisible));
    if (all & kGlyphBold) line.flags |= kLineAllBold;
    if (all & kGlyphItalic) line.flags |= kLineAllItalic;
    if (all & kGlyphMonospace) line.flags |= kLineAllMono;
    if (all & kGlyphArtifact) line.flags |= kLineArtifact;
    // As in lines.cpp: the add-on's OCR layer (kGlyphOcr) carries typography; a PDF's own
    // invisible layer does not.
    if ((all & (kGlyphInvisible | kGlyphOcr)) == kGlyphInvisible) line.flags |= kLineInvisible;
}

// The order of a glyph inside its owner: its content item's place in the tree, then its place in
// the content (one marked-content sequence often runs over several lines).
inline uint64_t GlyphKey(uint32_t seq, uint32_t glyph) { return (static_cast<uint64_t>(seq) << 32) | glyph; }

// The piece of `line` from glyph `k` on that one owner holds: neutral glyphs (spaces, inline
// images) go with the piece around them. Returns where it ends; `o` is its owner (kNoOwner for
// untagged text), `key` the smallest GlyphKey of its glyphs, `box` (when given) the upright box of
// the owner's glyphs.
uint32_t Piece(const PageData& page, const std::vector<uint32_t>& owner, const std::vector<uint32_t>& seq,
               const Line& line, uint32_t k, uint32_t& o, uint64_t& key, float* box) {
    o = kNoOwner;
    key = UINT64_MAX;
    bool decided = false;
    uint32_t end = k;
    if (box) box[0] = box[1] = INFINITY, box[2] = box[3] = -INFINITY;
    while (end < line.count) {
        const uint32_t gi = page.lineGlyphs[line.first + end];
        const Glyph& g = page.glyphs[gi];
        const uint32_t go = owner[gi] & ~kLabelBit;
        if (!Neutral(g) || go != kNoOwner) {
            if (!decided) {
                o = go;
                decided = true;
            } else if (go != o && !Neutral(g)) {
                break;
            }
            if (go == o) {
                key = std::min(key, GlyphKey(seq[gi], gi));
                if (box) {
                    box[0] = Fmin(box[0], g.x0), box[1] = Fmin(box[1], g.y0);
                    box[2] = Fmax(box[2], g.x1), box[3] = Fmax(box[3], g.y1);
                }
            }
        }
        ++end;
    }
    return end;
}

// Markdown inline text of an image's alternate text: brackets and backslashes escaped, line
// breaks as spaces.
void AppendAlt(std::string_view alt, std::string& out) {
    for (char c : alt) {
        if (c == '\n' || c == '\r' || c == '\t') {
            out.push_back(' ');
            continue;
        }
        if (c == '[' || c == ']' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
}

}  // namespace

struct TaggedLayout::Owner {
    uint32_t node = 0;
    OwnerKind kind = OwnerKind::Paragraph;
    uint8_t level = 0;            // headings
    uint8_t depth = 0;            // list items and their further paragraphs
    uint8_t quote = 0;            // block-quote depth
    bool listParagraph = false;   // a further paragraph of a list item
    bool claimed = false;         // list items: a paragraph has joined them
    bool anonymous = false;       // text right inside a grouping element: takes in inline elements only
    bool header = false;          // cells: in a header row
    bool hasImage = false;        // has an image of its own (figures leave their text out then)
    uint32_t table = kNone;       // cells and tables: index into tables_
    uint32_t label = kNone;       // list items: their Lbl element
    bool labelContent = false;    // list items: their Lbl has marked content (glyphs or a drawn bullet)
    uint32_t list = kNone;        // list items: their list element
    uint16_t row = 0, col = 0, rowSpan = 1, colSpan = 1;  // cells
    uint32_t number = 0;          // list items: position among their list's items, from 1
    uint8_t numbering = 0;        // list items: their list's kNodeOrdered / kNodeUnordered
};

struct TaggedLayout::TableInfo {
    uint32_t owner = 0;            // its Table owner
    uint32_t rows = 0, cols = 0;
    bool valid = true;
    bool placed = false;           // rowPage computed
    uint32_t curRow = kNone;       // while reading: the row being filled
    uint32_t curCol = 0;
    std::vector<uint16_t> pending; // while reading: rows each column stays taken by a row span
    std::vector<uint8_t> headerRow;  // per row: inside THead
    std::vector<uint8_t> rowHasData; // per row: has a TD cell
    std::vector<uint32_t> cells;   // cell owners in tree order (row by row)
    std::vector<uint32_t> rowPage; // per row: the page it is written on
};

TaggedLayout::TaggedLayout() = default;
TaggedLayout::~TaggedLayout() = default;

void TaggedLayout::FindOwners() {
    const std::vector<StructNode>& nodes = structure_->nodes;
    const size_t n = nodes.size();
    nodeOwner_.assign(n, kNone);
    nodeStyle_.assign(n, 0);
    nodeFigure_.assign(n, kNone);
    nodeLabel_.assign(n, 0);
    owners_.clear();
    tables_.clear();
    std::vector<uint8_t> direct(n, 0);  // the element has content items of its own
    for (const StructItem& it : structure_->items)
        if (it.mcid >= 0 && it.node < n) direct[it.node] = 1;
    // A code block: Code placed as a block, or right inside a grouping element or a quote (a code
    // span sits in a paragraph).
    std::vector<uint8_t> codeBlock(n, 0);
    for (uint32_t i = 0; i < n; ++i) {
        if (nodes[i].role != StructRole::Code) continue;
        const uint32_t p = nodes[i].parent < i ? nodes[i].parent : kNone;
        const StructRole pr = p != kNone ? nodes[p].role : StructRole::Group;
        codeBlock[i] = (nodes[i].flags & kNodeBlock) || pr == StructRole::Group || pr == StructRole::BlockQuote;
    }
    // Whether an element holds block elements (paragraphs, headings, lists, tables, quotes,
    // notes, code blocks): children come after their parents, so one backward pass suffices.
    std::vector<uint8_t> blocks(n, 0);
    for (uint32_t i = static_cast<uint32_t>(n); i-- > 0;) {
        const StructNode& node = nodes[i];
        const StructRole r = node.role;
        const bool block = r == StructRole::Paragraph || r == StructRole::Heading || r == StructRole::List ||
                           r == StructRole::ListItem || r == StructRole::Table || r == StructRole::TableRow ||
                           r == StructRole::TableHeader || r == StructRole::TableCell || r == StructRole::BlockQuote ||
                           r == StructRole::Note || r == StructRole::TableHead || r == StructRole::TableBody ||
                           codeBlock[i] || (r == StructRole::Unknown && (node.flags & kNodeBlock));
        if (node.parent < i && (block || blocks[i])) blocks[node.parent] = 1;
    }
    std::vector<uint8_t> quote(n, 0), lists(n, 0), sects(n, 0), head(n, 0), artifact(n, 0);
    std::vector<uint32_t> table(n, kNone), row(n, kNone), childItems(n, 0);
    const auto add = [&](uint32_t node, OwnerKind kind) {
        Owner o;
        o.node = node;
        o.kind = kind;
        o.quote = quote[node];
        owners_.push_back(o);
        return static_cast<uint32_t>(owners_.size() - 1);
    };
    for (uint32_t i = 0; i < n; ++i) {
        const StructNode& node = nodes[i];
        const uint32_t p = node.parent < i ? node.parent : kNone;  // parents come first (depth-first)
        StructRole r = node.role;
        // A list's kids are its items, whatever their type says (Chromium wraps an item's Lbl and
        // text in NonStruct).
        if (p != kNone && nodes[p].role == StructRole::List && (r == StructRole::Group || r == StructRole::Unknown))
            r = StructRole::ListItem;
        if (p != kNone) {
            quote[i] = quote[p];
            lists[i] = lists[p];
            sects[i] = sects[p];
            head[i] = head[p];
            artifact[i] = artifact[p];
            table[i] = table[p];
            row[i] = row[p];
            nodeStyle_[i] = nodeStyle_[p];
            nodeFigure_[i] = nodeFigure_[p];
            nodeLabel_[i] = nodeLabel_[p];
        }
        if (r == StructRole::BlockQuote && quote[i] < 8) quote[i]++;
        if (r == StructRole::List && lists[i] < 64) lists[i]++;
        if (r == StructRole::Group && node.level && sects[i] < 6) sects[i]++;
        if (r == StructRole::Artifact) artifact[i] = 1;
        if (r == StructRole::Strong) nodeStyle_[i] |= kStyleBold;
        if (r == StructRole::Emphasis) nodeStyle_[i] |= kStyleItalic;
        if (node.flags & kNodeLineThrough) nodeStyle_[i] |= kStyleStrike;
        if (r == StructRole::Figure) nodeFigure_[i] = i;
        if (artifact[i]) continue;
        uint32_t above = p != kNone ? nodeOwner_[p] : kNone;
        // Text right inside a grouping element (an anonymous paragraph) takes in the inline elements
        // beside it, and grouping elements that hold no blocks (Chromium's NonStruct around each
        // piece of a formula); block elements beside it are blocks of their own.
        if (above != kNone && owners_[above].anonymous) {
            const bool unknown = r == StructRole::Unknown || r == StructRole::Formula || r == StructRole::Form;
            const bool spanLike = r == StructRole::Inline || r == StructRole::Emphasis || r == StructRole::Strong ||
                                  r == StructRole::Link || r == StructRole::Figure ||
                                  (r == StructRole::Code && !codeBlock[i]) || (unknown && !(node.flags & kNodeBlock)) ||
                                  (r == StructRole::Group && !blocks[i]);
            if (!spanLike) above = kNone;
        }
        const OwnerKind aboveKind = above != kNone ? owners_[above].kind : OwnerKind::Paragraph;
        // Elements of no standard type: by their placement, else inline inside a block.
        if (r == StructRole::Unknown || r == StructRole::Formula || r == StructRole::Form) {
            if (node.flags & kNodeBlock)
                r = StructRole::Paragraph;
            else if ((node.flags & kNodeInline) || above != kNone)
                r = StructRole::Inline;
            else
                r = StructRole::Paragraph;
        }
        if (r == StructRole::Code && above != kNone && !(node.flags & kNodeBlock)) {
            nodeStyle_[i] |= kStyleMono;  // a code span
            r = StructRole::Inline;
        }
        const bool inlineRole = r == StructRole::Inline || r == StructRole::Emphasis || r == StructRole::Strong ||
                                r == StructRole::Link || r == StructRole::Label || r == StructRole::ListBody ||
                                r == StructRole::Group || r == StructRole::Figure;
        const bool breakOut = r == StructRole::List || r == StructRole::Table || r == StructRole::BlockQuote ||
                              r == StructRole::Note || r == StructRole::ListItem || r == StructRole::TableRow ||
                              r == StructRole::TableHeader || r == StructRole::TableCell || r == StructRole::Code;
        if (above != kNone) {
            if (aboveKind == OwnerKind::Cell || aboveKind == OwnerKind::Heading || aboveKind == OwnerKind::Code ||
                (aboveKind == OwnerKind::Note && r != StructRole::Note)) {
                nodeOwner_[i] = above;  // a cell, heading, code block or note holds all of its content
                continue;
            }
            if (aboveKind == OwnerKind::ListItem) {
                if (r == StructRole::Label) nodeLabel_[i] = 1;
                if (inlineRole) {
                    nodeOwner_[i] = above;
                    if (r == StructRole::Label && owners_[above].label == kNone) {
                        owners_[above].label = i;
                        owners_[above].labelContent = direct[i] != 0;
                    }
                    continue;
                }
                if (!breakOut && r != StructRole::Heading && !owners_[above].claimed) {
                    owners_[above].claimed = true;  // the item's first paragraph
                    nodeOwner_[i] = above;
                    continue;
                }
                if (!breakOut) {
                    const uint32_t o = add(i, r == StructRole::Heading ? OwnerKind::Heading : OwnerKind::Paragraph);
                    if (r == StructRole::Heading) {
                        owners_[o].level = node.level ? node.level : static_cast<uint8_t>(std::max(1, int(sects[i])));
                    } else {
                        owners_[o].listParagraph = true;
                        owners_[o].depth = owners_[above].depth;
                    }
                    nodeOwner_[i] = o;
                    continue;
                }
                // A nested list, table, quote, note or code block starts blocks of its own.
            } else if (!breakOut) {
                nodeOwner_[i] = above;  // inside a paragraph or a figure
                continue;
            }
        }
        switch (r) {
        case StructRole::Group:
        case StructRole::BlockQuote:
        case StructRole::List:
        case StructRole::TableHead:
        case StructRole::TableBody:
            if (r == StructRole::TableHead) head[i] = 1;
            // Text right inside a grouping element is a paragraph of its own; so is a grouping
            // element that holds no blocks (a paragraph of untyped pieces).
            if (direct[i] || (r == StructRole::Group && !blocks[i])) {
                nodeOwner_[i] = add(i, OwnerKind::Paragraph);
                owners_[nodeOwner_[i]].anonymous = true;
            }
            break;
        case StructRole::Table: {
            const uint32_t o = add(i, OwnerKind::Table);
            TableInfo t;
            t.owner = o;
            owners_[o].table = static_cast<uint32_t>(tables_.size());
            table[i] = static_cast<uint32_t>(tables_.size());
            head[i] = 0;
            row[i] = kNone;
            tables_.push_back(std::move(t));
            break;
        }
        case StructRole::TableRow: {
            if (table[i] == kNone) {
                if (direct[i]) {
                    nodeOwner_[i] = add(i, OwnerKind::Paragraph);
                    owners_[nodeOwner_[i]].anonymous = true;
                }
                break;
            }
            TableInfo& t = tables_[table[i]];
            row[i] = t.rows;
            t.curRow = t.rows;
            t.curCol = 0;
            for (uint16_t& left : t.pending)
                if (left > 0) left--;
            t.rows++;
            t.headerRow.push_back(head[i]);
            t.rowHasData.push_back(0);
            if (t.rows > static_cast<uint32_t>(kMaxTableRows)) t.valid = false;
            break;
        }
        case StructRole::TableHeader:
        case StructRole::TableCell: {
            if (table[i] == kNone || row[i] == kNone || !tables_[table[i]].valid) {
                nodeOwner_[i] = add(i, OwnerKind::Paragraph);
                break;
            }
            TableInfo& t = tables_[table[i]];
            if (t.curRow != row[i]) {
                // A cell after a nested row ended: it belongs to the row read last (malformed).
                nodeOwner_[i] = add(i, OwnerKind::Paragraph);
                break;
            }
            const uint32_t o = add(i, OwnerKind::Cell);
            Owner& c = owners_[o];
            uint32_t col = t.curCol;
            while (col < t.pending.size() && t.pending[col] > 0) ++col;
            const uint32_t span = std::min<uint32_t>(node.colSpan, static_cast<uint32_t>(kMaxTableCols));
            if (col + span > static_cast<uint32_t>(kMaxTableCols)) {
                t.valid = false;
                c.kind = OwnerKind::Paragraph;
                nodeOwner_[i] = o;
                break;
            }
            if (t.pending.size() < col + span) t.pending.resize(col + span, 0);
            for (uint32_t k = col; k < col + span; ++k) t.pending[k] = node.rowSpan;
            c.table = table[i];
            c.row = static_cast<uint16_t>(row[i]);
            c.col = static_cast<uint16_t>(col);
            c.rowSpan = node.rowSpan;
            c.colSpan = static_cast<uint16_t>(span);
            c.header = head[i] != 0;
            t.curCol = col + span;
            t.cols = std::max(t.cols, col + span);
            t.cells.push_back(o);
            if (r == StructRole::TableCell) t.rowHasData[row[i]] = 1;
            nodeOwner_[i] = o;
            break;
        }
        case StructRole::Heading: {
            const uint32_t o = add(i, OwnerKind::Heading);
            owners_[o].level = node.level ? node.level : static_cast<uint8_t>(std::max(1, int(sects[i])));
            nodeOwner_[i] = o;
            break;
        }
        case StructRole::Code: nodeOwner_[i] = add(i, OwnerKind::Code); break;
        case StructRole::Note: nodeOwner_[i] = add(i, OwnerKind::Note); break;
        case StructRole::Figure: nodeOwner_[i] = add(i, OwnerKind::Figure); break;
        case StructRole::ListItem: {
            const uint32_t o = add(i, OwnerKind::ListItem);
            Owner& item = owners_[o];
            item.depth = static_cast<uint8_t>(std::min(8, std::max(0, int(lists[i]) - 1)));
            if (p != kNone) {
                item.number = ++childItems[p];
                item.list = p;
                item.numbering = nodes[p].flags & (kNodeOrdered | kNodeUnordered);
            } else {
                item.number = 1;
            }
            nodeOwner_[i] = o;
            break;
        }
        default: nodeOwner_[i] = add(i, OwnerKind::Paragraph); break;
        }
    }
    // Tables past the limits are no tables: their cells are paragraphs.
    for (TableInfo& t : tables_) {
        if (t.valid && t.rows > 0 && t.cols > 0) continue;
        t.valid = false;
        for (uint32_t c : t.cells) owners_[c].kind = OwnerKind::Paragraph;
        t.cells.clear();
    }
}

StructureUse TaggedLayout::Prepare(std::vector<PageData>& pages, const Structure& structure) {
    structure_ = &structure;
    if (structure.use != StructureUse::Used) return structure.use;
    FindOwners();
    const size_t n = pages.size();
    glyphOwner_.assign(n, {});
    glyphSeq_.assign(n, {});
    lineOwner_.assign(n, {});
    lineKey_.assign(n, {});

    // Every glyph's element: its owner, whether it is a list label, its place in the tree.
    uint64_t visible = 0, mapped = 0, semantic = 0;
    for (size_t p = 0; p < n; ++p) {
        const PageData& page = pages[p];
        std::vector<uint32_t>& owner = glyphOwner_[p];
        std::vector<uint32_t>& seq = glyphSeq_[p];
        owner.assign(page.glyphs.size(), kNoOwner);
        seq.assign(page.glyphs.size(), UINT32_MAX);
        int32_t lastMcid = -2;
        uint8_t lastStream = 0;
        const StructItem* item = nullptr;
        for (size_t gi = 0; gi < page.glyphs.size(); ++gi) {
            const Glyph& g = page.glyphs[gi];
            if (g.mcid != lastMcid || g.mcStream != lastStream) {
                lastMcid = g.mcid;
                lastStream = g.mcStream;
                const uint32_t stream = StreamOf(page, g.mcStream);
                item = g.mcid >= 0 && stream != kNone ? FindItem(structure, static_cast<uint32_t>(p), stream, g.mcid)
                                                      : nullptr;
            }
            uint32_t o = kNoOwner;
            if (item && nodeOwner_[item->node] != kNone) {
                o = nodeOwner_[item->node];
                if (nodeLabel_[item->node]) o |= kLabelBit;
                seq[gi] = item->seq;
            }
            owner[gi] = o;
            if (!Counted(g)) continue;
            // Artifacts by structure (an Artifact element, PDF 2.0) are not counted either.
            if (item && nodeOwner_[item->node] == kNone) continue;
            visible++;
            if (o == kNoOwner) continue;
            mapped++;
            const Owner& own = owners_[o & ~kLabelBit];
            if (own.kind != OwnerKind::Paragraph || own.quote > 0 || own.listParagraph || nodeStyle_[item->node] != 0)
                semantic++;
        }
        // Figures with their images (alternate text) say more than "paragraph" too.
        for (const ImageBox& image : page.images) {
            const uint32_t stream = StreamOf(page, image.mcStream);
            const StructItem* it = image.mcid >= 0 && stream != kNone
                                        ? FindItem(structure, static_cast<uint32_t>(p), stream, image.mcid)
                                        : nullptr;
            if (it && nodeFigure_[it->node] != kNone && nodeOwner_[it->node] != kNone) semantic++;
        }
    }
    // Check 1: coverage. Check 2: elements that say more than "paragraph".
    if (visible == 0 || static_cast<double>(mapped) < kMinStructureCoverage * static_cast<double>(visible))
        return StructureUse::Coverage;
    if (semantic == 0) return StructureUse::Generic;

    // Check 3: Breuel's rule 1 over the pieces of each line that one element owns, in tree order.
    struct Unit {
        uint32_t owner;
        uint64_t key;
        float box[4];  // x0, y0, x1, y1
    };
    std::vector<Unit> units;
    uint64_t pairs = 0, violations = 0;
    for (size_t p = 0; p < n; ++p) {
        const PageData& page = pages[p];
        units.clear();
        for (const Line& line : page.lines) {
            uint32_t k = 0;
            while (k < line.count) {
                Unit u;
                k = Piece(page, glyphOwner_[p], glyphSeq_[p], line, k, u.owner, u.key, u.box);
                const uint32_t o = u.owner;
                // Notes are out of the flow: the tree puts them where they are cited.
                if (o == kNoOwner || owners_[o].kind == OwnerKind::Note) continue;
                if (units.size() < kMaxOrderUnits) units.push_back(u);
            }
        }
        std::sort(units.begin(), units.end(), [](const Unit& a, const Unit& b) {
            return a.owner != b.owner ? a.owner < b.owner : a.key < b.key;
        });
        for (size_t i = 0; i < units.size(); ++i) {
            const Unit& a = units[i];
            for (size_t j = i + 1; j < units.size(); ++j) {
                const Unit& b = units[j];
                const float overlap = Fmin(a.box[2], b.box[2]) - Fmax(a.box[0], b.box[0]);
                if (!(overlap > 1.0f)) continue;
                pairs++;
                // b comes later in the tree but sits wholly above a.
                const float tolerance = 0.25f * Fmin(a.box[3] - a.box[1], b.box[3] - b.box[1]);
                if (b.box[1] >= a.box[3] - tolerance) violations++;
            }
        }
    }
    if (pairs > 0 && static_cast<double>(violations) > kMaxOrderViolations * static_cast<double>(pairs))
        return StructureUse::Order;

    // Trusted. Lines split where the owner changes; untagged and artifact pieces leave the text.
    for (size_t p = 0; p < n; ++p) {
        PageData& page = pages[p];
        const std::vector<uint32_t>& owner = glyphOwner_[p];
        std::vector<uint32_t>& lineOwner = lineOwner_[p];
        std::vector<uint64_t>& lineKey = lineKey_[p];
        const uint32_t count = static_cast<uint32_t>(page.lines.size());
        lineOwner.assign(count, kNoOwner);
        lineKey.assign(count, UINT64_MAX);
        for (uint32_t li = 0; li < count; ++li) {
            const Line original = page.lines[li];
            if (original.count == 0) continue;
            uint32_t k = 0;
            bool firstPiece = true;
            while (k < original.count) {
                uint32_t o = kNoOwner;
                uint64_t key = UINT64_MAX;
                const uint32_t end = Piece(page, owner, glyphSeq_[p], original, k, o, key, nullptr);
                Line piece = original;
                piece.first = original.first + k;
                piece.count = end - k;
                MeasureLine(page, piece);
                if (o == kNoOwner) piece.flags |= kLineRemoved;
                uint32_t index = li;
                if (firstPiece) {
                    page.lines[li] = piece;
                } else {
                    index = static_cast<uint32_t>(page.lines.size());
                    page.lines.push_back(piece);
                    lineOwner.push_back(kNoOwner);
                    lineKey.push_back(UINT64_MAX);
                }
                lineOwner[index] = o;
                lineKey[index] = key;
                firstPiece = false;
                k = end;
            }
        }
        // What inline elements say about their glyphs.
        int32_t lastMcid = -2;
        uint8_t lastStream = 0;
        uint8_t style = 0;
        for (size_t gi = 0; gi < page.glyphs.size(); ++gi) {
            Glyph& g = page.glyphs[gi];
            if (g.mcid != lastMcid || g.mcStream != lastStream) {
                lastMcid = g.mcid;
                lastStream = g.mcStream;
                const uint32_t stream = StreamOf(page, g.mcStream);
                const StructItem* item =
                    g.mcid >= 0 && stream != kNone ? FindItem(structure, static_cast<uint32_t>(p), stream, g.mcid) : nullptr;
                style = item ? nodeStyle_[item->node] : 0;
            }
            if (style & kStyleBold) g.flags |= kGlyphBold;
            if (style & kStyleItalic) g.flags |= kGlyphItalic;
            if (style & kStyleMono) g.flags |= kGlyphMonospace;
            if (style & kStyleStrike) g.flags |= kGlyphStrike;
        }
        // Images outside the tree are decoration (PDF/UA: real content is tagged).
        for (ImageBox& image : page.images) {
            const uint32_t stream = StreamOf(page, image.mcStream);
            const StructItem* item = image.mcid >= 0 && stream != kNone
                                         ? FindItem(structure, static_cast<uint32_t>(p), stream, image.mcid)
                                         : nullptr;
            if (!item || nodeOwner_[item->node] == kNone) image.state = ImageState::Skipped;
        }
    }
    ownerPage_.assign(owners_.size(), kNone);
    for (size_t p = n; p-- > 0;)
        for (uint32_t o : lineOwner_[p])
            if (o != kNoOwner) ownerPage_[o] = static_cast<uint32_t>(p);
    return StructureUse::Used;
}

void TaggedLayout::LayOutTables(PageData& page, uint32_t p) {
    std::vector<uint32_t>& lineOwner = lineOwner_[p];
    std::vector<uint64_t>& lineKey = lineKey_[p];
    // The page's lines by owner, in order inside each owner.
    struct Entry {
        uint32_t owner;
        uint64_t key;
        uint32_t line;
    };
    std::vector<Entry> entries;
    for (uint32_t li = 0; li < lineOwner.size(); ++li) {
        if (lineOwner[li] == kNoOwner || (page.lines[li].flags & (kLineRemoved | kLineAnchor))) continue;
        if (owners_[lineOwner[li]].kind != OwnerKind::Cell) continue;
        entries.push_back(Entry{lineOwner[li], lineKey[li], li});
    }
    if (entries.empty()) return;
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.owner != b.owner) return a.owner < b.owner;
        return a.key != b.key ? a.key < b.key : a.line < b.line;
    });
    // The tables with cells on this page.
    std::vector<uint32_t> touched;
    for (const Entry& e : entries)
        if (touched.empty() || touched.back() != owners_[e.owner].table) touched.push_back(owners_[e.owner].table);
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    for (uint32_t ti : touched) {
        if (ti >= tables_.size()) continue;
        TableInfo& t = tables_[ti];
        if (!t.valid || t.cells.empty()) continue;
        if (!t.placed) {
            // Each row is written on the page where its text starts; rows without text go with the
            // row before them (the row after them at the start).
            t.placed = true;
            t.rowPage.assign(t.rows, kNone);
            for (uint32_t c : t.cells) {
                const Owner& cell = owners_[c];
                t.rowPage[cell.row] = std::min(t.rowPage[cell.row], ownerPage_[c]);
            }
            uint32_t last = kNone;
            for (uint32_t r = 0; r < t.rows; ++r) {
                if (t.rowPage[r] == kNone) t.rowPage[r] = last;
                else last = t.rowPage[r];
            }
            for (uint32_t r = t.rows; r-- > 0;) {
                if (t.rowPage[r] == kNone && r + 1 < t.rows) t.rowPage[r] = t.rowPage[r + 1];
            }
        }
        // This page's rows, renumbered from 0. A header row is a THead row or a row of TH cells.
        const auto header = [&](uint32_t r) { return t.headerRow[r] || !t.rowHasData[r]; };
        std::vector<int32_t> partRow(t.rows, -1);
        uint32_t rows = 0;
        uint16_t headerRows = 0;
        bool leading = true;
        for (uint32_t r = 0; r < t.rows; ++r) {
            if (t.rowPage[r] != p) continue;
            partRow[r] = static_cast<int32_t>(rows++);
            if (leading && header(r)) headerRows++;
            else leading = false;
        }
        // Header rows a page repeats (a browser repeats THead on every page) are written once,
        // with the rest of their row.
        for (uint32_t c : t.cells) {
            const Owner& o = owners_[c];
            if (partRow[o.row] >= 0 || !header(o.row)) continue;
            const auto at = std::lower_bound(entries.begin(), entries.end(), c,
                                             [](const Entry& e, uint32_t owner) { return e.owner < owner; });
            for (auto e = at; e != entries.end() && e->owner == c; ++e) page.lines[e->line].flags |= kLineTableCell;
        }
        if (rows == 0) continue;
        Table table;
        table.rows = static_cast<uint16_t>(rows);
        table.cols = static_cast<uint16_t>(t.cols);
        table.headerRows = headerRows;
        table.kind = static_cast<uint8_t>(TableKind::Lattice);
        table.firstCell = static_cast<uint32_t>(page.tableCells.size());
        table.firstLine = static_cast<uint32_t>(page.lines.size());
        table.x0 = INFINITY, table.y0 = INFINITY, table.x1 = -INFINITY, table.y1 = -INFINITY;
        float size = 0;
        uint8_t dir = 0;
        bool any = false;
        for (uint32_t c : t.cells) {
            const Owner& o = owners_[c];
            if (partRow[o.row] < 0) continue;
            TableCell cell;
            cell.row = static_cast<uint16_t>(partRow[o.row]);
            cell.col = o.col;
            cell.rowSpan = static_cast<uint16_t>(std::max<uint32_t>(1, std::min<uint32_t>(o.rowSpan, rows - cell.row)));
            cell.colSpan = o.colSpan;
            cell.first = static_cast<uint32_t>(page.lines.size());
            const auto at = std::lower_bound(entries.begin(), entries.end(), c,
                                             [](const Entry& e, uint32_t owner) { return e.owner < owner; });
            for (auto e = at; e != entries.end() && e->owner == c; ++e) {
                Line& src = page.lines[e->line];
                Line copy = src;
                copy.flags = static_cast<uint16_t>(kLineTableCell |
                                                   (src.flags & (kLineArtifact | kLineInvisible | kLineAllBold |
                                                                 kLineAllItalic | kLineAllMono | kLineRtl)));
                src.flags |= kLineTableCell;
                table.x0 = Fmin(table.x0, src.u0), table.x1 = Fmax(table.x1, src.u1);
                table.y0 = Fmin(table.y0, src.vb), table.y1 = Fmax(table.y1, src.vt);
                if (!any) size = src.size, dir = src.dir;
                any = true;
                page.lines.push_back(copy);
                lineOwner.push_back(kNoOwner);  // written by its table only
                lineKey.push_back(UINT64_MAX);
            }
            cell.count = static_cast<uint32_t>(page.lines.size()) - cell.first;
            page.tableCells.push_back(cell);
        }
        table.cellCount = static_cast<uint32_t>(page.tableCells.size()) - table.firstCell;
        table.lineCount = static_cast<uint32_t>(page.lines.size()) - table.firstLine;
        if (!any) {
            page.tableCells.resize(table.firstCell);
            continue;
        }
        Line anchor;
        anchor.u0 = table.x0, anchor.u1 = table.x1, anchor.vb = table.y0, anchor.vt = table.y1;
        anchor.size = size;
        anchor.baseline = table.y1 - size;
        anchor.first = static_cast<uint32_t>(page.tables.size());
        anchor.count = 0;
        anchor.flags = kLineTableAnchor;
        anchor.dir = dir;
        page.tables.push_back(table);
        page.lines.push_back(anchor);
        lineOwner.push_back(t.owner);
        lineKey.push_back(p);
    }
}

void TaggedLayout::LayOut(std::vector<PageData>& pages, uint32_t p) {
    PageData& page = pages[p];
    std::vector<uint32_t>& lineOwner = lineOwner_[p];
    std::vector<uint64_t>& lineKey = lineKey_[p];
    const Structure& structure = *structure_;
    // Glyphs added since Prepare (PlaceImages sets an inline image into its line as a glyph) belong
    // to no element of their own: they go with the line they sit in.
    glyphOwner_[p].resize(page.glyphs.size(), kNoOwner);
    glyphSeq_[p].resize(page.glyphs.size(), UINT32_MAX);
    // Image anchors PlaceImages added: their owner from their image's element; the alternate text
    // of the Figure around it.
    const auto imageItem = [&](const ImageBox& image) -> const StructItem* {
        const uint32_t stream = StreamOf(page, image.mcStream);
        return image.mcid >= 0 && stream != kNone ? FindItem(structure, p, stream, image.mcid) : nullptr;
    };
    for (ImageBox& image : page.images) {
        const StructItem* item = imageItem(image);
        if (!item) continue;
        const uint32_t figure = nodeFigure_[item->node];
        if (figure == kNone) continue;
        const StructNode& node = structure.nodes[figure];
        if (node.altLength == 0 || page.text.size() > (size_t(1) << 30)) continue;
        image.alt = static_cast<uint32_t>(page.text.size());
        page.text.append(structure.text, node.alt, node.altLength);
        image.altLength = node.altLength;
    }
    for (uint32_t li = static_cast<uint32_t>(lineOwner.size()); li < page.lines.size(); ++li) {
        Line& line = page.lines[li];
        uint32_t o = kNoOwner;
        uint64_t key = UINT64_MAX;
        if (line.flags & kLineImage) {
            const StructItem* item = imageItem(page.images[line.first]);
            if (item && nodeOwner_[item->node] != kNone) {
                o = nodeOwner_[item->node];
                key = GlyphKey(item->seq, 0);
                owners_[o].hasImage = true;
            }
        }
        if (o == kNoOwner) line.flags |= kLineRemoved;
        lineOwner.push_back(o);
        lineKey.push_back(key);
    }
    // Inline images: the link glyph PlaceImages set into the line gets the alternate text.
    std::string link;
    for (const ImageBox& image : page.images) {
        if (image.state != ImageState::Written || image.line < 0 || image.altLength == 0) continue;
        const Line& line = page.lines[static_cast<size_t>(image.line)];
        for (uint32_t k = 0; k < line.count; ++k) {
            Glyph& g = page.glyphs[page.lineGlyphs[line.first + k]];
            if (!(g.flags & kGlyphImage) || g.x0 != image.x0 || g.y0 != image.y0) continue;
            const std::string_view old = page.GlyphText(g);
            if (old.size() < 4 || old.substr(0, 4) != "![](") break;
            link = "![";
            AppendAlt(page.ImageAlt(image), link);
            link.append(old.substr(2));
            if (link.size() > UINT16_MAX || page.text.size() > (size_t(1) << 30)) break;
            g.text = static_cast<uint32_t>(page.text.size());
            g.textLength = static_cast<uint16_t>(link.size());
            page.text += link;
            const uint32_t o = lineOwner[static_cast<size_t>(image.line)];
            if (o != kNoOwner) owners_[o].hasImage = true;
            break;
        }
    }
    LayOutTables(page, p);
    // The reading order: owners in tree order, each owner's lines in the order of their content.
    struct Entry {
        uint32_t owner;
        uint64_t key;
        uint32_t line;
    };
    std::vector<Entry> entries;
    for (uint32_t li = 0; li < page.lines.size(); ++li) {
        const Line& line = page.lines[li];
        if (lineOwner[li] == kNoOwner || (line.flags & (kLineRemoved | kLineTableCell))) continue;
        entries.push_back(Entry{lineOwner[li], lineKey[li], li});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.owner != b.owner) return a.owner < b.owner;
        return a.key != b.key ? a.key < b.key : a.line < b.line;
    });
    page.order.clear();
    for (const Entry& e : entries) page.order.push_back(e.line);
}

void TaggedLayout::Blocks(const std::vector<PageData>& pages, BlockList& list) {
    const BlockList geo = std::move(list);
    list = BlockList();
    list.monoDocument = geo.monoDocument;
    const size_t n = pages.size();
    // Per line, its geometric block.
    std::vector<std::vector<int32_t>> geoBlock(n);
    for (size_t p = 0; p < n; ++p) geoBlock[p].assign(pages[p].lines.size(), -1);
    for (size_t b = 0; b < geo.blocks.size(); ++b) {
        const Block& block = geo.blocks[b];
        if (block.kind == BlockKind::Comment) continue;
        for (uint32_t i = block.first; i < block.first + block.count; ++i) {
            const BlockLine& bl = geo.lines[i];
            if (bl.page < n && bl.line < geoBlock[bl.page].size()) geoBlock[bl.page][bl.line] = static_cast<int32_t>(b);
        }
    }
    bool treeQuotes = false;
    for (const StructNode& node : structure_->nodes) treeQuotes = treeQuotes || node.role == StructRole::BlockQuote;

    // Runs: consecutive lines of the reading order with one owner (an image or table anchor is a
    // run of its own). A paragraph or list item whose marked content changes where the geometric
    // pass starts a new paragraph or list item is split there: the tree put two paragraphs in one
    // element (a list item's second paragraph right in its LBody).
    struct Run {
        uint32_t owner;
        uint32_t first, count;  // into `lines`
        int32_t geoCode;        // the geometric Code block holding all its lines, else -1
        int32_t geoItem;        // the geometric ListItem block its first line starts, else -1
        bool note;              // all its lines are in geometric footnotes
        bool anchor;
        bool continued;         // a further paragraph of its owner
        bool text;              // holds text beyond list labels
    };
    std::vector<BlockLine> lines;
    std::vector<Run> runs;
    const auto startsItem = [&](int32_t g, uint32_t p, uint32_t li) {
        if (g < 0) return false;
        const Block& b = geo.blocks[static_cast<size_t>(g)];
        return b.kind == BlockKind::ListItem && !b.listParagraph && b.count > 0 && geo.lines[b.first].page == p &&
               geo.lines[b.first].line == li;
    };
    // Whether a line holds text beyond its list label.
    const auto hasText = [&](uint32_t p, uint32_t li) {
        const PageData& page = pages[p];
        const Line& line = page.lines[li];
        for (uint32_t k = 0; k < line.count; ++k) {
            const uint32_t gi = page.lineGlyphs[line.first + k];
            if (!(glyphOwner_[p][gi] & kLabelBit) && !(page.glyphs[gi].flags & kGlyphSpace)) return true;
        }
        return false;
    };
    for (uint32_t p = 0; p < n; ++p) {
        const PageData& page = pages[p];
        int32_t lastGeo = -1;
        uint64_t lastKey = UINT64_MAX;
        for (uint32_t li : page.order) {
            const Line& line = page.lines[li];
            const uint32_t o = lineOwner_[p][li];
            const int32_t g = geoBlock[p][li];
            const bool anchor = (line.flags & kLineAnchor) != 0;
            bool joins = !runs.empty() && runs.back().owner == o && !anchor && !runs.back().anchor;
            bool continued = false;
            if (joins && o != kNoOwner && g >= 0 && lastGeo >= 0 && g != lastGeo && runs.back().text &&
                (owners_[o].kind == OwnerKind::Paragraph || owners_[o].kind == OwnerKind::ListItem)) {
                const BlockKind a = geo.blocks[static_cast<size_t>(lastGeo)].kind;
                const BlockKind b = geo.blocks[static_cast<size_t>(g)].kind;
                const bool prose = (a == BlockKind::Paragraph || a == BlockKind::ListItem) &&
                                   (b == BlockKind::Paragraph || b == BlockKind::ListItem);
                const bool newContent = (lineKey_[p][li] >> 32) != (lastKey >> 32);
                if (prose && (newContent || startsItem(g, p, li))) {
                    joins = false;
                    continued = true;
                }
            }
            if (!joins) {
                // Inside a list item the tree has the last word on what is an item, unless its
                // labels are text of its body (an LI without a Lbl of its own).
                const bool open = o != kNoOwner && (owners_[o].kind == OwnerKind::Paragraph ||
                                                    (owners_[o].kind == OwnerKind::ListItem && !owners_[o].labelContent));
                const bool adopt = open && startsItem(g, p, li);
                runs.push_back(Run{o, static_cast<uint32_t>(lines.size()), 0, -2, adopt ? g : -1, true, anchor,
                                   continued, false});
            }
            Run& run = runs.back();
            lines.push_back(BlockLine{p, li, 0});
            run.count++;
            run.text = run.text || (!anchor && hasText(p, li));
            const bool code = g >= 0 && geo.blocks[static_cast<size_t>(g)].kind == BlockKind::Code;
            if (run.geoCode == -2) run.geoCode = code ? g : -1;
            else if (run.geoCode != g || !code) run.geoCode = -1;
            run.note = run.note && !anchor && g >= 0 && geo.blocks[static_cast<size_t>(g)].kind == BlockKind::Footnote;
            lastGeo = g;
            lastKey = lineKey_[p][li];
        }
    }
    // Footnotes: runs the geometric pass read as notes leave the flow; its notes whose every line
    // left come back as footnote definitions, with their references.
    std::vector<std::vector<uint8_t>> left(n);
    for (size_t p = 0; p < n; ++p) left[p].assign(pages[p].lines.size(), 0);
    for (Run& run : runs) {
        if (!run.note) continue;
        const OwnerKind kind = run.owner != kNoOwner ? owners_[run.owner].kind : OwnerKind::Paragraph;
        if (kind == OwnerKind::Heading || kind == OwnerKind::Cell || kind == OwnerKind::Table) {
            run.note = false;
            continue;
        }
        for (uint32_t i = run.first; i < run.first + run.count; ++i) left[lines[i].page][lines[i].line] = 1;
    }
    std::vector<uint32_t> keptNotes;
    for (const Block& block : geo.blocks) {
        if (block.kind != BlockKind::Footnote) continue;
        bool all = block.count > 0;
        for (uint32_t i = block.first; i < block.first + block.count && all; ++i) {
            const BlockLine& bl = geo.lines[i];
            all = bl.page < n && bl.line < left[bl.page].size() && left[bl.page][bl.line];
        }
        if (all) keptNotes.push_back(block.number);
    }
    std::sort(keptNotes.begin(), keptNotes.end());
    for (const NoteRef& ref : geo.noteRefs)
        if (std::binary_search(keptNotes.begin(), keptNotes.end(), ref.number)) list.noteRefs.push_back(ref);

    // Page comments of the geometric pass, written before the first block of their page.
    std::vector<const Block*> comments;
    for (const Block& block : geo.blocks)
        if (block.kind == BlockKind::Comment) comments.push_back(&block);
    size_t nextComment = 0;
    const auto commentsUpTo = [&](uint32_t pageNumber) {
        while (nextComment < comments.size() && comments[nextComment]->number <= pageNumber) {
            Block c = *comments[nextComment++];
            c.first = static_cast<uint32_t>(list.lines.size());
            c.count = 0;
            list.blocks.push_back(c);
        }
    };

    int32_t lastCode = -1;  // the geometric code block the last block came from
    uint32_t lastTableOwner = kNoOwner;
    for (size_t r = 0; r < runs.size(); ++r) {
        const Run& run = runs[r];
        if (run.note) continue;
        const BlockLine& head = lines[run.first];
        const PageData& page = pages[head.page];
        const Line& firstLine = page.lines[head.line];
        commentsUpTo(static_cast<uint32_t>(page.number));
        const Owner* owner = run.owner != kNoOwner ? &owners_[run.owner] : nullptr;
        // A table broken across pages: its parts follow each other in one block.
        if (firstLine.flags & kLineTableAnchor) {
            const bool continues = !list.blocks.empty() && list.blocks.back().kind == BlockKind::Table &&
                                   lastTableOwner == run.owner;
            if (!continues) {
                Block b;
                b.kind = BlockKind::Table;
                b.number = firstLine.first;
                b.first = static_cast<uint32_t>(list.lines.size());
                b.quote = owner ? owner->quote : 0;
                b.size = firstLine.size;
                list.blocks.push_back(b);
            }
            const Table& table = page.tables[firstLine.first];
            list.lines.push_back(head);
            for (uint32_t li = table.firstLine; li < table.firstLine + table.lineCount; ++li)
                list.lines.push_back(BlockLine{head.page, li, 0});
            list.blocks.back().count = static_cast<uint32_t>(list.lines.size()) - list.blocks.back().first;
            lastTableOwner = run.owner;
            lastCode = -1;
            continue;
        }
        lastTableOwner = kNoOwner;
        if (firstLine.flags & kLineImage) {
            Block b;
            b.kind = BlockKind::Image;
            b.number = static_cast<uint32_t>(page.number);
            b.quote = owner ? owner->quote : 0;
            b.first = static_cast<uint32_t>(list.lines.size());
            b.count = 1;
            list.lines.push_back(head);
            list.blocks.push_back(b);
            lastCode = -1;
            continue;
        }
        if (!owner) continue;
        // A figure's text (chart labels) is left out when the figure has its image.
        if (owner->kind == OwnerKind::Figure && owner->hasImage) continue;
        Block b;
        b.quote = owner->quote;
        if (!treeQuotes) {
            const int32_t g = geoBlock[head.page][head.line];
            if (g >= 0) b.quote = geo.blocks[static_cast<size_t>(g)].quote;
            // A paragraph set in from both sides is a block quote (how CSS and word processors set
            // one).
            const uint8_t f = structure_->nodes[owner->node].flags;
            if (owner->kind == OwnerKind::Paragraph && !owner->listParagraph && (f & kNodeStartIndent) &&
                (f & kNodeEndIndent) && b.quote == 0)
                b.quote = 1;
        }
        b.size = firstLine.size;
        bool fromTree = true;  // a list item marked by the tree (else by the geometric pass)
        switch (owner->kind) {
        case OwnerKind::Heading:
            b.kind = BlockKind::Heading;
            b.level = static_cast<uint8_t>(std::max(1, std::min(6, int(owner->level))));
            break;
        case OwnerKind::Code: b.kind = BlockKind::Code; break;
        case OwnerKind::ListItem:
            if (!run.continued) {
                b.kind = BlockKind::ListItem;
                break;
            }
            b.kind = BlockKind::Paragraph;
            b.listParagraph = true;
            b.depth = owner->depth;
            break;
        default:
            b.kind = BlockKind::Paragraph;
            if (owner->listParagraph) {
                b.listParagraph = true;
                b.depth = owner->depth;
            }
            break;
        }
        // A list item the tree calls a paragraph (or leaves inside another item's text): the
        // geometric pass found its marker.
        if ((b.kind == BlockKind::Paragraph || b.kind == BlockKind::ListItem) && run.geoItem >= 0) {
            const Block& g = geo.blocks[static_cast<size_t>(run.geoItem)];
            b.kind = BlockKind::ListItem;
            b.listParagraph = false;
            b.depth = g.depth;
            b.ordered = g.ordered;
            b.number = g.number;
            b.task = g.task;
            fromTree = false;
        }
        // Code the tree does not mark: a plain paragraph the geometric pass read as code.
        if (b.kind == BlockKind::Paragraph && !b.listParagraph && run.geoCode >= 0) {
            if (lastCode == run.geoCode && !list.blocks.empty() && list.blocks.back().kind == BlockKind::Code) {
                for (uint32_t i = run.first; i < run.first + run.count; ++i) list.lines.push_back(lines[i]);
                list.blocks.back().count += run.count;
                continue;
            }
            b.kind = BlockKind::Code;
        }
        // A paragraph element that goes on in the line the block before it ends in (a formula
        // tagged as a block inside a sentence) belongs to that block.
        if (b.kind == BlockKind::Paragraph && !list.blocks.empty() && list.blocks.back().count > 0 &&
            (list.blocks.back().kind == BlockKind::Paragraph || list.blocks.back().kind == BlockKind::ListItem) &&
            list.blocks.back().quote == b.quote) {
            const BlockLine& prev = list.lines[list.blocks.back().first + list.blocks.back().count - 1];
            const Line& before = pages[prev.page].lines[prev.line];
            const float em = Fmax(1.0f, Fmax(before.size, firstLine.size));
            const float gap = firstLine.u0 - before.u1;
            if (prev.page == head.page && before.dir == firstLine.dir && before.count > 0 &&
                std::fabs(before.baseline - firstLine.baseline) <= 0.3f * em && gap > -0.5f * em && gap < 0.6f * em) {
                for (uint32_t i = run.first; i < run.first + run.count; ++i) list.lines.push_back(lines[i]);
                list.blocks.back().count += run.count;
                lastCode = -1;
                continue;
            }
        }
        lastCode = b.kind == BlockKind::Code ? run.geoCode : -1;
        b.first = static_cast<uint32_t>(list.lines.size());
        b.count = run.count;
        for (uint32_t i = run.first; i < run.first + run.count; ++i) list.lines.push_back(lines[i]);
        if (b.kind == BlockKind::ListItem && !fromTree) {
            list.lines[b.first].skip = geo.lines[geo.blocks[static_cast<size_t>(run.geoItem)].first].skip;
        } else if (b.kind == BlockKind::ListItem) {
            b.depth = owner->depth;
            // The marker: the item's label glyphs at the start of its first line, else the label's
            // /ActualText, else the list's numbering.
            BlockLine& first = list.lines[b.first];
            const std::vector<uint32_t>& glyphOwner = glyphOwner_[first.page];
            std::string marker;
            uint32_t skip = 0;
            while (skip < firstLine.count) {
                const uint32_t gi = page.lineGlyphs[firstLine.first + skip];
                if (!(glyphOwner[gi] & kLabelBit) && !(skip > 0 && (page.glyphs[gi].flags & kGlyphSpace) &&
                                                      skip + 1 < firstLine.count &&
                                                      (glyphOwner[page.lineGlyphs[firstLine.first + skip + 1]] & kLabelBit)))
                    break;
                if (!(page.glyphs[gi].flags & kGlyphSpace)) marker.append(page.GlyphText(page.glyphs[gi]));
                ++skip;
            }
            if (marker.empty() && owner->label != kNone) {
                const StructNode& label = structure_->nodes[owner->label];
                marker.assign(structure_->text, label.actual, label.actualLength);
                while (!marker.empty() && marker.back() == ' ') marker.pop_back();
            }
            uint32_t value = 0;
            const Marker m = marker.empty() ? Marker::None : ParseMarker(marker, value);
            if (m == Marker::Number || m == Marker::Letter || m == Marker::Roman) {
                b.ordered = true;
                b.number = value;
            } else if (m == Marker::TaskOpen || m == Marker::TaskDone) {
                b.task = m == Marker::TaskOpen ? 1 : 2;
            } else if (m == Marker::None && (owner->numbering & kNodeOrdered)) {
                b.ordered = true;
                b.number = owner->number;
            }
            // A task box right after the marker.
            if (b.task == 0 && skip < firstLine.count) {
                std::string word;
                uint32_t end = skip;
                while (end < firstLine.count) {
                    const Glyph& g = page.glyphs[page.lineGlyphs[firstLine.first + end]];
                    if (end > skip && (g.flags & kGlyphSpaceBefore)) break;
                    if (g.flags & kGlyphSpace) {
                        if (end > skip) break;
                    } else {
                        word.append(page.GlyphText(g));
                    }
                    ++end;
                }
                uint32_t unused = 0;
                const Marker task = word.size() <= 8 ? ParseMarker(word, unused) : Marker::None;
                if (task == Marker::TaskOpen || task == Marker::TaskDone) {
                    b.task = task == Marker::TaskOpen ? 1 : 2;
                    skip = end;
                }
            }
            first.skip = skip;
            // An item with no label at all that follows another item is that item's next paragraph
            // (a word processor tags a list paragraph's continuation as an item of its own).
            if (marker.empty() && !owner->labelContent && b.task == 0 && !b.ordered && !list.blocks.empty()) {
                const Block& prev = list.blocks.back();
                if ((prev.kind == BlockKind::ListItem || prev.listParagraph) && prev.quote == b.quote) {
                    b.kind = BlockKind::Paragraph;
                    b.listParagraph = true;
                    b.depth = prev.depth;
                }
            }
        }
        list.blocks.push_back(b);
    }
    commentsUpTo(UINT32_MAX);

    // PDF/UA forbids skipped heading levels, so writers renumber a document whose first heading is
    // a "##" to H1 and leave the rest. A lone H1 that opens the document, set in the style of a
    // later heading of a deeper level, takes that level.
    {
        Block* first = nullptr;
        uint32_t ones = 0;
        for (Block& b : list.blocks) {
            if (b.kind != BlockKind::Heading) continue;
            if (!first) first = &b;
            if (b.level == 1) ones++;
        }
        if (first && first->level == 1 && ones == 1) {
            const auto style = [&](const Block& b) {
                const BlockLine& bl = list.lines[b.first];
                return pages[bl.page].lines[bl.line].flags & (kLineAllBold | kLineAllItalic);
            };
            for (const Block& b : list.blocks) {
                if (&b == first || b.kind != BlockKind::Heading || b.level == 1) continue;
                if (std::fabs(b.size - first->size) <= 0.5f && style(b) == style(*first)) {
                    first->level = b.level;
                    break;
                }
            }
        }
    }

    // Lists the tree keeps flat (one L per item, nesting only by indentation): depth from where
    // the items' text starts (right-aligned numbers move their markers), by rank of the distinct
    // positions.
    for (size_t i = 0; i < list.blocks.size();) {
        if (list.blocks[i].kind != BlockKind::ListItem) {
            ++i;
            continue;
        }
        size_t end = i;
        bool flat = true;
        while (end < list.blocks.size() &&
               (list.blocks[end].kind == BlockKind::ListItem || list.blocks[end].listParagraph) &&
               list.blocks[end].quote == list.blocks[i].quote) {
            if (list.blocks[end].kind == BlockKind::ListItem && list.blocks[end].depth != 0) flat = false;
            ++end;
        }
        if (flat && end - i >= 2) {
            std::vector<float> xs;
            // Where the item's text starts: its first glyph that is no label or space (a label set
            // apart from the text is a line of its own).
            const auto markerX = [&](const Block& b) {
                for (uint32_t i2 = b.first; i2 < b.first + b.count; ++i2) {
                    const BlockLine& bl = list.lines[i2];
                    const PageData& page = pages[bl.page];
                    const Line& line = page.lines[bl.line];
                    for (uint32_t k = i2 == b.first ? bl.skip : 0; k < line.count; ++k) {
                        const uint32_t gi = page.lineGlyphs[line.first + k];
                        if (!(glyphOwner_[bl.page][gi] & kLabelBit) && !(page.glyphs[gi].flags & kGlyphSpace))
                            return page.glyphs[gi].u0;
                    }
                }
                const BlockLine& bl = list.lines[b.first];
                return pages[bl.page].lines[bl.line].u0;
            };
            // The items of one list element share its depth: its position is where its items'
            // text starts furthest left.
            const auto listOf = [&](const Block& b) {
                const BlockLine& bl = list.lines[b.first];
                const uint32_t o = lineOwner_[bl.page][bl.line];
                return o != kNoOwner && owners_[o].kind == OwnerKind::ListItem ? owners_[o].list : kNone;
            };
            std::vector<std::pair<uint32_t, float>> groups;  // list element, position
            const auto groupX = [&](uint32_t l, float x) -> float& {
                // The list elements of a run come one after another: the last few are enough.
                for (size_t g = groups.size(); g-- > 0 && g + 64 >= groups.size();)
                    if (groups[g].first == l && l != kNone) return groups[g].second;
                groups.emplace_back(l, x);
                return groups.back().second;
            };
            std::vector<float> itemX(end - i, 0);
            for (size_t k = i; k < end; ++k) {
                if (list.blocks[k].kind != BlockKind::ListItem) continue;
                itemX[k - i] = markerX(list.blocks[k]);
                float& gx = groupX(listOf(list.blocks[k]), itemX[k - i]);
                gx = Fmin(gx, itemX[k - i]);
            }
            for (const auto& g : groups) xs.push_back(g.second);
            std::sort(xs.begin(), xs.end());
            std::vector<float> levels;
            for (float x : xs)
                if (levels.empty() || x - levels.back() > 3.0f) levels.push_back(x);
            if (levels.size() >= 2 && groups.size() <= 4096) {
                uint8_t depth = 0;
                for (size_t k = i; k < end; ++k) {
                    Block& b = list.blocks[k];
                    if (b.kind == BlockKind::ListItem) {
                        const float x = groupX(listOf(b), itemX[k - i]);
                        size_t level = 0;
                        while (level + 1 < levels.size() && x - levels[level + 1] > -3.0f) ++level;
                        depth = static_cast<uint8_t>(std::min<size_t>(8, level));
                    }
                    b.depth = depth;
                }
            }
        }
        i = end;
    }

    // Footnote definitions after the last block, in the geometric pass's order.
    for (const Block& block : geo.blocks) {
        if (block.kind != BlockKind::Footnote ||
            !std::binary_search(keptNotes.begin(), keptNotes.end(), block.number))
            continue;
        Block b = block;
        b.first = static_cast<uint32_t>(list.lines.size());
        for (uint32_t i = block.first; i < block.first + block.count; ++i) list.lines.push_back(geo.lines[i]);
        list.blocks.push_back(b);
    }
}

} // namespace RayoPdf::Layout
