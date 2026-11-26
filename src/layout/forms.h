// SPDX-License-Identifier: Apache-2.0
// A document's form fields (ISO 32000-1 §12.7.3-12.7.4) for the JSON output's "fields": the
// terminal fields of /AcroForm /Fields, each with its full name (the partial names /T from the
// root down, joined with "."), its type, its value (/V; /FT, /Ff and /V inherited through
// /Parent), its flags and its first widget on a converted page (a field with none is left out).
//
// Values: a text field's text (none for a password), a check box's state, a radio group's chosen
// option (an index into /Opt resolved to the option), a choice field's selected options (one, or
// a list when /V is an array); signatures and pushbuttons have none.
//
// Hostile input: the tree is walked once with a visited set of object numbers, at most
// kMaxFieldTreeDepth levels deep and kMaxFieldNodes nodes; at most kMaxFormFields fields and
// kMaxFieldValueText bytes of a value or name are kept.
#pragma once

#include "pdf/document.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RayoPdf::Layout {

constexpr uint32_t kMaxFormFields = 1u << 16;
constexpr uint32_t kMaxFieldNodes = 1u << 20;
constexpr int kMaxFieldTreeDepth = 32;
constexpr size_t kMaxFieldValueText = 65536;

enum class FieldType : uint8_t { Text, Checkbox, Radio, Choice, Signature, Button };

// "text", "checkbox", "radio", "choice", "signature", "button".
const char* FieldTypeName(FieldType type);

enum class FieldValue : uint8_t {
    None,  // null: no value, a password, a signature or a pushbutton
    Text,  // texts[0]
    List,  // texts: a choice field's selected options
    Bool,  // checked: a check box
};

struct FormField {
    std::string name;
    FieldType type = FieldType::Text;
    FieldValue value = FieldValue::None;
    std::vector<std::string> texts;  // UTF-8
    bool checked = false;
    int page = 0;                    // 1-based: the page of its first widget on a converted page
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // that widget's /Rect on the upright page
    bool readOnly = false, required = false;
};

// The fields of `document` with a widget on pages `first` to `last` (0-based), in the order of
// the field tree (depth first), into `out` (replaced).
void ReadFields(const Document& document, int first, int last, std::vector<FormField>& out);

// Whether `document` is an XFA form (ISO 32000-1 §12.7.8) whose fields this conversion does not
// read: its catalog sets /NeedsRendering true (a dynamic form, whose pages are a placeholder), or
// its /AcroForm holds /XFA and no widget annotation stands on pages `first` to `last` (0-based):
// the fields live in the XFA data only. At most kMaxFieldNodes annotations are looked at.
bool XfaForm(const Document& document, int first, int last);

} // namespace RayoPdf::Layout
