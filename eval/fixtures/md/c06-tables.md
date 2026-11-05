Sentinel start C06.

## Simple table

| Name | Role | City | Year |
| --- | --- | --- | --- |
| Anna | Engineer | Kraków | 2019 |
| Ben | Designer | Berlin | 2021 |
| Chloé | Manager | Lyon | 2017 |

## Aligned numbers

| Left | Center | Right |
| :--- | :---: | ---: |
| 1 | 10.5 | 1,000 |
| 22 | 3.25 | 25,500 |
| 333 | 0.125 | 7 |

## Formatted cells

| Feature | Example | Reference |
| --- | --- | --- |
| Bold | **important** | [spec](https://spec.commonmark.org/) |
| Code | `x = 1` | [pandoc](https://pandoc.org/) |
| Mixed | *soft* and `hard` | none |

## Empty cells

| A | B | C |
| --- | --- | --- |
| 1 |  | 3 |
|  | 5 |  |
| 7 | 8 |  |

## Long table

| # | Name | Value | Group |
| ---: | --- | ---: | --- |
| 1 | Item 01 | 37 | Beta |
| 2 | Item 02 | 74 | Gamma |
| 3 | Item 03 | 111 | Delta |
| 4 | Item 04 | 148 | Alpha |
| 5 | Item 05 | 185 | Beta |
| 6 | Item 06 | 222 | Gamma |
| 7 | Item 07 | 259 | Delta |
| 8 | Item 08 | 296 | Alpha |
| 9 | Item 09 | 333 | Beta |
| 10 | Item 10 | 370 | Gamma |
| 11 | Item 11 | 407 | Delta |
| 12 | Item 12 | 444 | Alpha |
| 13 | Item 13 | 481 | Beta |
| 14 | Item 14 | 518 | Gamma |
| 15 | Item 15 | 555 | Delta |
| 16 | Item 16 | 592 | Alpha |
| 17 | Item 17 | 629 | Beta |
| 18 | Item 18 | 666 | Gamma |
| 19 | Item 19 | 703 | Delta |
| 20 | Item 20 | 740 | Alpha |
| 21 | Item 21 | 777 | Beta |
| 22 | Item 22 | 814 | Gamma |
| 23 | Item 23 | 851 | Delta |
| 24 | Item 24 | 888 | Alpha |
| 25 | Item 25 | 925 | Beta |
| 26 | Item 26 | 962 | Gamma |
| 27 | Item 27 | 999 | Delta |
| 28 | Item 28 | 36 | Alpha |
| 29 | Item 29 | 73 | Beta |
| 30 | Item 30 | 110 | Gamma |
| 31 | Item 31 | 147 | Delta |
| 32 | Item 32 | 184 | Alpha |
| 33 | Item 33 | 221 | Beta |
| 34 | Item 34 | 258 | Gamma |
| 35 | Item 35 | 295 | Delta |
| 36 | Item 36 | 332 | Alpha |
| 37 | Item 37 | 369 | Beta |
| 38 | Item 38 | 406 | Gamma |
| 39 | Item 39 | 443 | Delta |
| 40 | Item 40 | 480 | Alpha |

## Wide table

| Component | Owner | Description | Inputs | Outputs | Risks | Status |
| --- | --- | --- | --- | --- | --- | --- |
| Reader | Core team | Parses the file structure and cross-reference tables of the document | Raw bytes from disk | Object graph | Malformed cross-reference sections in damaged files | Planned |
| Interpreter | Core team | Executes page content streams and emits positioned glyph runs | Object graph | Glyph events | Unusual operators and deeply nested graphics state | Planned |
| Layout | Layout team | Groups glyphs into words, lines, blocks and reading order | Glyph events | Ordered blocks | Multi-column pages with floating figures and sidebars | Research |

Sentinel end C06.
