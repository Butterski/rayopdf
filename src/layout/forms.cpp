// SPDX-License-Identifier: Apache-2.0
#include "layout/forms.h"

#include "layout/interpreter.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace RayoPdf::Layout {

namespace {

// Where a widget sits: its page (1-based) and its /Rect on the upright page.
struct WidgetPlace {
    int page = 0;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// (x, y) of default user space on page `crop`, `rotate` as it is shown upright (the interpreter's
// page matrix, §7.7.3.3).
void Upright(double x, double y, const PdfRect& crop, int rotate, double& ux, double& uy) {
    const double w = crop.Width(), h = crop.Height();
    x -= crop.x0;
    y -= crop.y0;
    switch (rotate) {
    case 90: ux = y, uy = w - x; break;
    case 180: ux = w - x, uy = h - y; break;
    case 270: ux = h - y, uy = x; break;
    default: ux = x, uy = y; break;
    }
}

// A field value's text: a string (a text string, §7.9.2.2), a name's bytes or a text stream's.
bool ValueText(const Document& document, const PdfValue& value, std::string& out) {
    out.clear();
    if (value.IsString()) {
        AppendTextString(value.String().substr(0, kMaxFieldValueText), out);
    } else if (value.IsName()) {
        out.assign(value.Name().substr(0, kMaxFieldValueText));
    } else if (value.IsStream()) {
        std::string data;
        (void)document.StreamData(value, data, kMaxFieldValueText);
        AppendTextString(std::string_view(data).substr(0, kMaxFieldValueText), out);
    } else {
        return false;
    }
    if (out.size() > kMaxFieldValueText) out.resize(kMaxFieldValueText);
    return true;
}

// The option index a radio state names ("0", "12"), or SIZE_MAX.
size_t StateIndex(std::string_view text) {
    if (text.empty() || text.size() > 9) return SIZE_MAX;
    size_t index = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return SIZE_MAX;
        index = index * 10 + static_cast<size_t>(c - '0');
    }
    return index;
}

} // namespace

const char* FieldTypeName(FieldType type) {
    switch (type) {
    case FieldType::Text: return "text";
    case FieldType::Checkbox: return "checkbox";
    case FieldType::Radio: return "radio";
    case FieldType::Choice: return "choice";
    case FieldType::Signature: return "signature";
    case FieldType::Button: return "button";
    }
    return "text";
}

void ReadFields(const Document& document, int first, int last, std::vector<FormField>& out) {
    out.clear();
    const PdfValue& form = document.Get(document.Catalog(), "AcroForm");
    const PdfValue& fields = document.Get(form, "Fields");
    if (!fields.IsArray() || first > last) return;
    uint32_t budget = kMaxFieldNodes;  // annotations and field nodes looked at
    // The widgets of the converted pages, by object number (the first page that lists one).
    std::unordered_map<uint32_t, WidgetPlace> places;
    for (int p = first; p <= last && budget > 0; ++p) {
        const PdfValue& annots = document.Get(document.PageDict(p), "Annots");
        if (!annots.IsArray()) continue;
        const PdfRect crop = document.CropBox(p);
        const int rotate = document.Rotate(p);
        for (const PdfValue& item : annots.Array()) {
            if (budget == 0) break;
            budget--;
            if (!item.IsRef() || places.count(item.refNum)) continue;
            const PdfValue& annot = document.Resolve(item);
            if (!document.Get(annot, "Subtype").IsName("Widget")) continue;
            PdfRect rect;
            if (!document.Rect(document.Get(annot, "Rect"), rect)) continue;
            double ax, ay, bx, by;
            Upright(rect.x0, rect.y0, crop, rotate, ax, ay);
            Upright(rect.x1, rect.y1, crop, rotate, bx, by);
            WidgetPlace place;
            place.page = p + 1;
            place.x0 = float(std::min(ax, bx)), place.x1 = float(std::max(ax, bx));
            place.y0 = float(std::min(ay, by)), place.y1 = float(std::max(ay, by));
            places.emplace(item.refNum, place);
        }
    }
    if (places.empty()) return;

    // The field tree, depth first in /Kids order. A node whose kids have no /T of their own is a
    // terminal field: its kids are its widgets (or it is its own widget, a merged dictionary).
    struct Pending {
        const PdfValue* entry;
        int depth;
        uint32_t name;  // its parent's full name: an index into `names`
    };
    std::vector<std::string> names(1);
    std::vector<Pending> work;
    for (size_t i = fields.Array().size(); i-- > 0;) work.push_back(Pending{&fields.Array()[i], 1, 0});
    std::unordered_set<uint32_t> seen;
    std::string part, text;
    while (!work.empty() && out.size() < kMaxFormFields && budget > 0) {
        budget--;
        const Pending w = work.back();
        work.pop_back();
        if (w.entry->IsRef() && !seen.insert(w.entry->refNum).second) continue;
        const PdfValue& node = document.Resolve(*w.entry);
        if (!node.IsDict()) continue;
        std::string name = names[w.name];
        const PdfValue& partial = document.Get(node, "T");
        if (partial.IsString()) {
            part.clear();
            AppendTextString(partial.String().substr(0, kMaxFieldValueText), part);
            if (!name.empty()) name.push_back('.');
            name += part;
            if (name.size() > kMaxFieldValueText) name.resize(kMaxFieldValueText);
        }
        const PdfValue& kids = document.Get(node, "Kids");
        bool fieldKids = false;
        if (kids.IsArray()) {
            for (const PdfValue& kid : kids.Array()) {
                if (budget == 0) break;
                budget--;
                if (document.Resolve(kid).Find("T")) {
                    fieldKids = true;
                    break;
                }
            }
        }
        if (fieldKids) {
            if (w.depth >= kMaxFieldTreeDepth) continue;
            names.push_back(std::move(name));
            const uint32_t index = static_cast<uint32_t>(names.size() - 1);
            for (size_t i = kids.Array().size(); i-- > 0;) work.push_back(Pending{&kids.Array()[i], w.depth + 1, index});
            continue;
        }
        // Its first widget on a converted page.
        const WidgetPlace* place = nullptr;
        if (kids.IsArray()) {
            for (const PdfValue& kid : kids.Array()) {
                if (!kid.IsRef()) continue;
                const auto it = places.find(kid.refNum);
                if (it != places.end()) {
                    place = &it->second;
                    break;
                }
            }
        } else if (w.entry->IsRef()) {
            const auto it = places.find(w.entry->refNum);
            if (it != places.end()) place = &it->second;
        }
        if (!place) continue;
        // The entries a field inherits from its parents (§12.7.3.1).
        const auto inherited = [&](std::string_view key) -> const PdfValue* {
            const PdfValue* at = &node;
            for (int depth = 0; depth < kMaxFieldTreeDepth && at->IsDict(); ++depth) {
                if (const PdfValue* v = at->Find(key)) return &document.Resolve(*v);
                at = &document.Get(*at, "Parent");
            }
            return nullptr;
        };
        const PdfValue* type = inherited("FT");
        const PdfValue* flags = inherited("Ff");
        const PdfValue* value = inherited("V");
        const int64_t ff = flags ? flags->Int() : 0;
        const std::string_view ft = type ? type->Name() : std::string_view();
        FormField field;
        if (ft == "Tx") {
            field.type = FieldType::Text;
            // A password's value is never written (flag bit 14).
            if (value && !(ff & (int64_t(1) << 13)) && ValueText(document, *value, text)) {
                field.value = FieldValue::Text;
                field.texts.push_back(text);
            }
        } else if (ft == "Btn") {
            if (ff & (int64_t(1) << 16)) {
                field.type = FieldType::Button;
            } else if (ff & (int64_t(1) << 15)) {
                // A radio group's value is the chosen button's state, which may be an index into
                // /Opt (the export values of buttons that share a state name).
                field.type = FieldType::Radio;
                const std::string_view state = value ? value->Name() : std::string_view();
                if (!state.empty() && state != "Off") {
                    const PdfValue* options = inherited("Opt");
                    const size_t index = StateIndex(state);
                    if (options && options->IsArray() && index < options->Array().size() &&
                        ValueText(document, document.Resolve(options->Array()[index]), text)) {
                        field.texts.push_back(text);
                    } else {
                        field.texts.emplace_back(state.substr(0, kMaxFieldValueText));
                    }
                    field.value = FieldValue::Text;
                }
            } else {
                field.type = FieldType::Checkbox;
                field.value = FieldValue::Bool;
                field.checked = value && value->IsName() && !value->Name().empty() && value->Name() != "Off";
            }
        } else if (ft == "Ch") {
            field.type = FieldType::Choice;
            if (value && value->IsArray()) {
                field.value = FieldValue::List;
                for (const PdfValue& item : value->Array()) {
                    if (budget == 0) break;
                    budget--;
                    if (ValueText(document, document.Resolve(item), text)) field.texts.push_back(text);
                }
            } else if (value && ValueText(document, *value, text)) {
                field.value = FieldValue::Text;
                field.texts.push_back(text);
            }
        } else if (ft == "Sig") {
            field.type = FieldType::Signature;
        } else {
            continue;  // no field type: no field
        }
        field.name = std::move(name);
        field.page = place->page;
        field.x0 = place->x0, field.y0 = place->y0, field.x1 = place->x1, field.y1 = place->y1;
        field.readOnly = (ff & 1) != 0;
        field.required = (ff & 2) != 0;
        out.push_back(std::move(field));
    }
}

} // namespace RayoPdf::Layout
