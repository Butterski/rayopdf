# Layout and math algorithms: principled replacements for fixed thresholds in RayoPDF

Research note, 2026-11-13. Plan mode: the repository was only read, nothing was changed.

## 0. Where RayoPDF's fixed thresholds are now

| Decision | Current rule (file, symbol, line) |
|---|---|
| Word break | `kWordGap = 0.13` em, `kWordGapWithSpaces = 0.3` em (`src/layout/lines.h:17-18`). `DroppedSpace` breaks a word at ≥ `kSpaceShare = 0.85` × the font's space width (`lines.cpp:1105,1156`). |
| Line fragments and joins | `kFragmentGap 3.0`, `kJoinGap 0.8`, `kGutterCandidate 0.6`, `kMarkerGap 4.0` em (`lines.h:19-24`) |
| Column cut | `SplitX(items, Fmax(3.0f, 0.5f*size))` (`reading_order.cpp:475`). Breuel partial order up to `kTopologicalMax = 200` lines (`:202`). |
| Running lines | Exact `RunningKey` match (digits and roman numerals → `#`, `:46-86`). A line is running when it repeats on ≥ ⌈n/2⌉ pages, or on half the odd or even pages (`RemoveRunningLines`, `:1153-1157`). There are also single-page band rules: `kMarginBand 0.06`, `kWideGap 2.5`, … (`:798-814`). |
| Headings | `kHeadingSize 1.12`, `kTitleSize 1.38`, `kSmallTitleSize 1.25` (`semantics.cpp:19-25`). The style key is the half-point size `lround(size*2)` (`semantics.cpp`, `style` lambda, ≈ line 2617). The body size is the mode of the character-weighted half-point keys (`:1630`). |
| Paragraph leading | Already a histogram, in twentieths of the body size (`WideLeading`, `semantics.cpp:790-792`). |
| Tables | Ruling snaps `kSnap 2.5`, `kJoin 3`, `kMinEdge 3` pt (`tables.cpp:18-25`). Borderless tables go through `Borderless(gapFactor)` (`:3246`) and the "two thirds of rows" and "an em wide" rules (`:1877-1884`). |
| Math | Scripts: `small && d > 0.12em` → superscript, `small && d < −0.08em` → subscript, with `small = size < 0.92em` (`math.cpp:1219-1230`). Fraction bar: height ≤ 0.3 em and width ≥ 0.25 em (`:784`). Radical gap ≤ 0.4 em (`:774,863`). Limits use 0.45/0.65 em (`:1297`). About 60 em-ratio literals in all. |

## 1. Spacing statistics

### 1.1 What the engines do (all fixed fractions of the font size)

- **pdfminer.six** (MIT): adds a space when the gap exceeds `word_margin × max(w, h)` of the character (`layout.py:488-491`). `halign` needs a vertical overlap greater than `line_overlap × min height` (`:695-698`). Source: https://github.com/pdfminer/pdfminer.six/blob/master/pdfminer/layout.py
- **pdf.js** (Apache-2.0, copyable): `TRACKING_SPACE_FACTOR 0.102`, `NOT_A_SPACE_FACTOR 0.03`, `SPACE_IN_FLOW_MAX_FACTOR 0.6`, `NEGATIVE_SPACE_FACTOR −0.2`, `VERTICAL_SHIFT_RATIO 0.25`.
  - It adapts to the font: when the font's space is ≤ 1/3 em, `trackingFactor = max(0.5·spaceEm, 0.102)`.
  - It adapts to letter-spacing: when every gap in a run is above the threshold, the threshold becomes `min(1.3 × smallest gap, 0.4 em)` (`WIDE_SPACE_MULT`, `WIDE_SPACE_MAX_FACTOR`).
  - Type3 fonts use `0.25 × min(adjacent advances)` (`src/core/evaluator.js` ≈ lines 2486-2749).
  - Source: https://github.com/mozilla/pdf.js/blob/master/src/core/evaluator.js
- **MuPDF** (AGPL, read only): `SPACE_DIST 0.15`, `SPACE_MAX_DIST 0.8`, `PARAGRAPH_DIST 1.5`, `FAKE_BOLD_MAX_DIST 0.1` em. It adds a space for gaps in (0.15, 0.8) em, and two spaces when the gap is > 0.3 em (`stext-device.c:85-89,922-983`). Source: https://github.com/ArtifexSoftware/mupdf/blob/master/source/fitz/stext-device.c
- **PDFBox** (Apache-2.0): the next word starts at `endOfLastTextX + min(spaceWidth × 0.5, avgCharWidth × 0.3)`. The space width is averaged with the previous one (`PDFTextStripper.java:596-634`). Source: https://github.com/apache/pdfbox/blob/trunk/pdfbox/src/main/java/org/apache/pdfbox/text/PDFTextStripper.java
- **Tesseract** (Apache-2.0): the median gap of a block estimates the non-space gap, "This RELIES on the assumption that there are more gaps WITHIN words than BETWEEN words". It then estimates the real space above a crude threshold and keeps a 1:3 kern:space ratio as a sanity bound (`tospace.cpp:63-67,200-206`). This is the only engine here that fits gap statistics per block. Source: https://zdenop.github.io/tesseract-doc/tospace_8cpp_source.html

### 1.2 Two-class thresholds on a gap histogram

Input: gaps $g_i$ between consecutive glyphs on one baseline, in units of the font size, binned into $B$ integer bins (for example 1/100 em over [−0.1, 1.0]).

- **Otsu (1979).** Choose $t^\ast = \arg\max_t \omega_0(t)\,\omega_1(t)\,(\mu_0(t)-\mu_1(t))^2$.
  - Cost: O(n) to bin, then O(B) with prefix sums.
  - Failure modes: it assumes comparable class variances and is biased when the class sizes are very unequal, and word gaps are much rarer than letter gaps [INFERENCE, the standard criticism that motivated Kittler–Illingworth].
  - DOI 10.1109/TSMC.1979.4310076 (from memory, unverified).
- **Kittler–Illingworth minimum-error thresholding (1986).** Fit a Gaussian to each side of $t$ and minimise $J(t)=1+2[P_0\ln\sigma_0+P_1\ln\sigma_1]-2[P_0\ln P_0+P_1\ln P_1]$. This is the Bayes error of a two-Gaussian mixture.
  - Cost: O(B) with prefix sums of $h$, $xh$ and $x^2h$.
  - It handles unequal priors and variances, which fits the case of rare, wide word gaps and many tight letter gaps.
  - Failure modes: few samples (σ → 0) and one-class lines; J(t) has spurious minima at the edges of the histogram, so restrict $t$ to a plausible range.
  - Paper: Pattern Recognition 19(1):41-47, https://www.academia.edu/download/85632603/1986Kittler.pdf. DOI 10.1016/0031-3203(86)90030-0 (from memory, unverified).
- **Two-component GMM by EM.** $p(g)=\pi_0\mathcal N(\mu_0,\sigma_0^2)+\pi_1\mathcal N(\mu_1,\sigma_1^2)$. The boundary is where $\pi_0\mathcal N_0=\pi_1\mathcal N_1$ (a quadratic).
  - Louloudis et al. classify word versus inter-character gaps of handwritten lines with a Gaussian mixture, fitted with the unsupervised CLUSTER tool. Pattern Recognition 42(12):3169-3183, 2009, DOI 10.1016/j.patcog.2008.12.016, https://www.sciencedirect.com/science/article/abs/pii/S0031320308005335 (thesis summary: https://www.di.uoa.gr/sites/default/files/documents/grad/phdbook/louloudis.pdf).
  - Cost: O(n · iterations).
  - Failure modes: depends on initialisation, and can degenerate to a single component.
- **KDE antimode.** Place the threshold at the density minimum between the two leading modes of $\hat f_h$, and test bimodality with Silverman's critical bandwidth (§2). Silverman, JRSS B 43(1):97-99, 1981, DOI 10.1111/j.2517-6161.1981.tb01155.x, https://academic.oup.com/jrsssb/article/43/1/97/7028128
- **Labelled samples from the PDF itself [INFERENCE].** When a line draws its own space glyphs, gaps before or after a space glyph are labelled word gaps, and gaps inside space-delimited words are labelled letter gaps. A two-class threshold fitted on these labels (Kittler–Illingworth on labelled data, or an LDA boundary) carries over to runs of the same font that dropped their spaces. RayoPDF's `DroppedSpace` already uses the font's space width as a prior (`lines.cpp:1142-1157`).

### 1.3 Docstrum (O'Gorman 1993)

- Method: find the k nearest neighbours (k = 5) of every component.
  - The histogram of neighbour angles gives the skew.
  - The distance histogram of near-0° pairs peaks at the within-line (character) spacing, and that of near-90° pairs at the between-line spacing.
  - Lines are the transitive closure of within-line pairs whose distance is ≤ $f_t$ × the within-line spacing. Blocks merge with parallel and perpendicular thresholds $f_{pa}$ and $f_{pe}$.
- The tuned values on UW-III were K = 8, $f_t$ = 2.578, $f_{pe}$ = 0.6, $f_{pa}$ = 2.345 (Shafait et al., Table 1).
- Cost: O(n log n) with a grid or k-d tree.
- Results (Shafait, Keysers, Breuel, DAS 2006; UW-III, 878 test pages, text-line error %): Docstrum 11.2 with default parameters and 6.0 with tuned ones. Docstrum had no error at all on more than 50% of pages.
- Failure modes: "spacing variations due to different font sizes and styles within one page result in split errors" (same paper, §5), because the spacing is estimated once per page.
- Sources: TPAMI 15(11):1162-1173, https://ieeexplore.ieee.org/document/244677. Comparison: http://www.keysers.net/daniel/files/Shafait--Layout-Analysis-Comparison--DAS2006.pdf (journal version TPAMI 30(6):941-954, 2008, https://pubmed.ncbi.nlm.nih.gov/18421102).
- Lesson for RayoPDF [INFERENCE]: estimate spacing per (font, size) cluster, not per page.

## 2. Font-size and weight clustering for heading levels

- **Exact 1-D k-means: Fisher (1958) and Jenks natural breaks.** Sort the sizes $x_{(1)}\le\dots\le x_{(n)}$ with weights $w$ (characters).
  - Recurrence: $D[k][i]=\min_{j\le i} D[k-1][j-1]+\mathrm{SSE}(j,i)$, where SSE comes from O(1) prefix sums of $w$, $wx$ and $wx^2$.
  - Cost: O(k n²), or O(k n) with monotone-matrix (SMAWK) methods.
  - Jenks's natural breaks optimise the same objective (goodness of variance fit = 1 − SDCM/SDAM).
  - Sources: Fisher, JASA 53(284):789-798, https://doi.org/10.1080/01621459.1958.10501479. Grønlund et al. 2017 (survey and O(kn) algorithms, also k-medians and any Bregman divergence), https://arxiv.org/abs/1701.07204. Jenks 1967, International Yearbook of Cartography 7, https://cir.nii.ac.jp/crid/1573668925394541312
  - For RayoPDF n is the number of distinct sizes (≤ ~50), so the DP costs microseconds per document [INFERENCE].
- **Choosing k.**
  - GMM with BIC: $\mathrm{BIC}=-2\ln\hat L+(3k-1)\ln n$. Schwarz 1978, Ann. Stat. 6(2) (DOI 10.1214/aos/1176344136 from memory, unverified).
  - Gap statistic: Tibshirani, Walther, Hastie 2001, JRSS B 63(2) (unverified DOI 10.1111/1467-9868.00293).
  - Silverman's critical bandwidth: $h_k=\inf\{h:\hat f_h \text{ has} \le k \text{ modes}\}$. With a Gaussian kernel the mode count is monotone in $h$, so $k$ is read off where $h_k$ jumps (Silverman 1981).
  - Hartigan's dip test for unimodality: Ann. Stat. 13(1), 1985 (unverified DOI 10.1214/aos/1176346577).
- **Domain.** Cluster on $\log s$, because heading sizes step by ratios. RayoPDF's own comments measure LaTeX at 1.44 and 1.20, typst at 1.40 and 1.20, and LibreOffice at 1.67 and 1.33 (`semantics.cpp:20-22`). Add weight as a second key (bold beside regular) and rank bold above regular within one size cluster, as RayoPDF already does.
- **Tie with the outline.** The outline gives depths, not levels.
  - RayoPDF already lets outline-matched headings win and maps unmatched styles to the level of the last matched heading of that style (`semantics.cpp:2531-2569`).
  - docling-pdf (Rust port of docling's `HeadingHierarchyModel`; licence not checked) uses the same order: bookmarks first, then numbering, then style. Its style step merges sizes within a relative difference of `style_size_tolerance = 0.05` (https://docs.rs/docling-pdf/latest/src/docling_pdf/heading_hierarchy.rs.html, lines 1-86).
- **Failure modes.**
  - Templates that set two levels in the same style. RayoPDF refines these by section numbering (`semantics.cpp:2578`).
  - A title that occurs once forms a cluster of one; it needs a variance floor or a minimum cluster weight.
  - Small-capitals and all-capitals headings at body size.
- **Published results.** I found no non-learned heading-level paper with a benchmark number. FinTOC 2021 (https://aclanthology.org/2021.fnp-1.17.pdf) notes that "A font size of 12 points might be an attribute of the paragraph style in one document, but of header style in another", which argues for clustering per document.

## 3. Column and region segmentation as optimisation

- **Whitespace cover (Baird 1990/1994).**
  - Sort maximal white rectangles by $K(c)=\sqrt{\mathrm{area}(c)}\,W(|\log_2(h/w)|)$ and add them until the stopping rule $K(s_j)-W_s F(s_j)\le T_s$.
  - It is sensitive to the stopping rule: stopping early gives merges, late gives splits.
  - UW-III text-line error 12.2% (default) and 9.8% (tuned) (Shafait 2006, §2.4 and Table 2).
- **Breuel (2002) maximal empty rectangles.**
  - Branch and bound: maximise $Q(r)$ over $r\subseteq r_b$ disjoint from obstacle boxes. Pick an obstacle inside the bound, split into the 4 sub-rectangles around it, and keep a priority queue on the upper bound of $Q$. Solutions come out best first, and the code is under 100 lines.
  - Evaluation: a gutter must be bounded by at least a minimum number of components on each major side. Breuel reports "an estimated error rate of less than 0.5% on the UW3 database with a single set of parameters". On 221 UW3 pages (73 multi-column) there were no segmentation errors by visual inspection.
  - Sources: DAS 2002, LNCS 2423:188-199, https://link.springer.com/chapter/10.1007/3-540-45869-7_23. Summary: https://www.dfki.de/fileadmin/user_upload/import/2000_HighPerfDocLayoutAna.pdf
  - Failure modes: noise components and figures, which need masking first.
- **Kise area Voronoi (1998).**
  - Build the area Voronoi diagram of components from boundary samples (sampling rate $s_r$ = 6). Delete Voronoi edges by inter-component distance and area ratio; the distance thresholds come from the peaks of the page's distance histogram, which is what adapts it to the data. The exact deletion criterion is from memory and was not re-read [unverified].
  - UW-III text-line error 7.5% (default) and 5.5% (tuned), the best average in Shafait 2006. Its failure is the same as Docstrum's: mixed font sizes on one page split lines.
  - Sources: CVIU 70(3):370-382, DOI 10.1006/cviu.1998.0684, https://www.sciencedirect.com/science/article/pii/S1077314298906841. Re-analysis: http://www.ipol.im/pub/art/2026/591/article.pdf
- **Lee and Ryu (2001), parameter-free analysis.** Parameters are estimated from the periodicity of projection profiles; I read only the abstract [method details unverified]. TPAMI 23(11):1240-1256, https://www.computer.org/csdl/journal/tp/2001/11/i1240/13rRUx0xPUO
- **Meunier (2005), optimised XY-cut with dynamic programming.**
  - Enumerate the horizontal cuts and, inside each band, the vertical cuts. Choose the set of horizontal cuts that maximises score = Σ heights of bands that share a vertical cut (that is, columns) + 1/distance as a tie-break. Columns narrower than 1/5 of the page earn nothing.
  - DP: $BC(i,X)=\text{share}+\max(BC(i{+}1,X\cap X_i),\,BC(i{+}1,\emptyset))$, memoised.
  - Results: 98% of blocks in order on about 800 pages (repair manuals and Word documents), under 1 s per page. Without the DP a page could take "hours or days". It becomes insensitive to the minimum horizontal gap.
  - Failure modes: L-shapes.
  - Sources: ICDAR 2005, DOI 10.1109/ICDAR.2005.182, https://static.aminer.org/pdf/PDF/000/295/221/optimized_xy_cut_for_determining_a_page_reading_order.pdf
- **XY-Cut++ (Liu, Li, Wei 2025).** Masks cross-layout elements first, then segments at several granularities and matches across modalities. Claimed 98.8 BLEU on DocBench-100, up to 24% over baselines. https://arxiv.org/abs/2504.10258. The Rust port https://github.com/home-still/xycut-plus-plus is GPL-3.0 and excluded.
- **Breuel's reading-order partial order.** (1) a precedes b when their x-ranges overlap and a is above b. (2) a precedes b when a is wholly left of b and no line c lies between them in y while overlapping both in x. A topological sort finishes the order. The pair test is O(n²), O(n³) with the "no c between" check. Already in RayoPDF (`reading_order.cpp:522-526`).
- **Speed ranking** (Shafait 2006): X-Y cut is fastest, then Voronoi, Docstrum and whitespace. Their verdict: "no single algorithm outperforms all other algorithms".

## 4. Tables as optimisation

- **Tabula, basic extraction** (MIT): columns are the connected components of the union of the words' x-intervals over all rows, merging every chunk that overlaps horizontally. The gaps between components are column boundaries (`BasicExtractionAlgorithm.columnPositions`, lines 112-152, https://github.com/tabulapdf/tabula-java/blob/master/src/main/java/technology/tabula/extractors/BasicExtractionAlgorithm.java). Cost: O(n log n). It fails on spanning headers, which merge columns.
- **Nurminen text edges** (tabula-java `NurminenDetectionAlgorithm`, MIT): left, right and middle text alignments become edges when ≥ `REQUIRED_TEXT_LINES_FOR_EDGE = 4` lines share them. A table needs ≥ `REQUIRED_CELLS_FOR_TABLE = 4` cells. The ruling snap is 8 (`:33-41`). Thesis: https://www.semanticscholar.org/paper/a9b167a86fb189bfcd366c3839f33f0404db9c10
- **Camelot stream** (MIT):
  - Rows come from y-overlap. The number of columns is the **mode** of words per row, and columns are taken from rows with that count and then extended.
  - The network parser keeps elements aligned on both axes (top, centre or bottom with left, middle or right), prunes unconnected ones, seeds at the most-connected element, and grows the table using "plausible gaps".
  - Camelot's own FinTabNet.c numbers (545 PDFs, proxy TEDS): network F1 0.725 and TEDS 0.200, against the learned `ml` flavour at 0.750 and 0.371.
  - Docs: https://github.com/camelot-dev/camelot/blob/master/docs/user/how-it-works.rst
- **pdfplumber "text" strategy** (MIT): imaginary lines through word left, right or centre edges where at least `min_words_vertical = 3` words align. `text_x_tolerance = 3` pt (README, lines 401-438), https://github.com/jsvine/pdfplumber
- **Hu, Kashi, Lopresti, Wilfong.**
  - "Medium-independent table detection", SPIE DRR VII, 2000, pp. 291-302: an **optimal partition of a document into tables by dynamic programming** over line-block scores (ASCII input).
  - "Table structure recognition and its evaluation", SPIE DRR VIII, 2001: **hierarchical clustering** of word intervals for columns, then spatial and lexical criteria for headers.
  - Both are known only from the citations in Wang, Phillips, Haralick (ICDAR 2001), http://www.haralick.org/conferences/12630528.pdf, refs [4]-[5]; full texts unread. Patent: US 7,054,871.
- **Wang, Phillips, Haralick 2004.** Table structure understanding framed as optimisation, Pattern Recognition 37(7) (https://www.sciencedirect.com/science/article/pii/S0031320304000494; contents unread, details unverified).
- **T-Recs (Kieninger and Dengel, DAS 1998).** Builds word "segmentation graphs" from horizontal overlaps between adjacent lines and clusters them into blocks bottom-up. The PDF text was not extractable, so this description is from memory [unverified]: https://cdn.aaai.org/Symposia/Fall/1999/FS-99-04/FS99-04-007.pdf
- **Header detection.** Nagy and Seth's "minimum indexing point" splits the stub and header from the value cells, so that every value cell is indexed by a unique header and stub path. ICPR 2016, https://sites.ecse.rpi.edu/~nagy/PDF_chrono/2016_ICPR_Table_headers_7_29_SS.pdf. Also Seth and Nagy, ICDAR 2013, https://digitalcommons.unl.edu/cseconfwork/230
- **ILP and min-cut.** I found no primary source for a non-learned ILP or min-cut formulation of born-digital table structure (absence unverified).
- **A penalised-likelihood formulation for RayoPDF [INFERENCE].**
  - Candidates are the zero-ink x-intervals $G_1<\dots<G_m$ of a block of $R$ rows. Choose a subset as boundaries to maximise $\sum_{c}\big[\ell_{\text{align}}(c)\big]-\lambda K$.
  - $\ell_{\text{align}}$ is a robust alignment score of each column's cell left, right or centre edges, for example $-\min_{e\in\{L,R,C\}}\mathrm{MAD}(e)$, plus the share of filled cells. The column count penalty $\lambda$ uses a BIC-like $\tfrac12\ln R$.
  - Exact by DP over sorted gaps in O(m²) with m ≤ 64.
  - Rows: 1-D clustering of baselines with a break where the gap exceeds median pitch + 3·1.4826·MAD.

## 5. Page furniture across pages

- **Lin (2003), page association.**
  - Candidates: the top 5 and bottom 3 lines of each page.
  - Score: $\mathrm{Score}(L_{j,i})=\sum_{k=j-W}^{j+W} w_i\,\mathrm{Sim}(L_{j,i},L_{k,i})$ with W = 8. Weights are 1.0, 0.75, 0.5, 0.5, 0.5 from the top, mirrored at the bottom.
  - $\mathrm{Sim}=\mathrm{BaseSim}\cdot\mathrm{GeomSim}$. BaseSim = matched characters / longer length, from an edit-distance DP after replacing every digit with `@`. GeomSim compares the line boxes.
  - Results: 9 OCR'd documents, 1,156 pages; precision 98.0%, recall 92.7%.
  - Failure mode: documents made of short articles whose running heads appear on only 1 or 2 pages.
  - Sources: SPIE 5010:164-171, DOI 10.1117/12.472833, http://shiftleft.com/mirrors/www.hpl.hp.com/techreports/2002/HPL-2002-129.pdf
- **Déjean and Meunier (2008), "Versatile page numbering analysis".** Detects page numbering as *sequences* across pages (a numbering scheme rather than one number per page). DRR XV, https://europe.naverlabs.com/research/publications/versatile-page-numbering-analysis/. Details unread [unverified]; patent US 7,797,622.
- **RayoPDF today.** Exact normalised keys, counted over the whole document (≥ ⌈n/2⌉ pages, or odd or even halves). Lin's windowed score also catches running heads that change per chapter, which never reach n/2 [INFERENCE].
- **Probabilistic threshold [INFERENCE].** Among the 2W neighbours, count matches $m$ with Sim > θ. Under the null hypothesis of body text, $m\sim\mathrm{Bin}(2W,p_0)$ with $p_0$ estimated from the similarity of body lines at the same position. Call a line furniture when $P(M\ge m)<\alpha$. This replaces "half the pages" with a test that also works for short documents.
- **Bounded edit distance.** Ukkonen's banded DP is O(k·n) (Information and Control 64, 1985; DOI unverified). Myers' bit-parallel algorithm is O(⌈m/64⌉·n) (JACM 46(3), 1999; DOI 10.1145/316542.316550 unverified). Lines are capped at about 150 characters (`kEdgeMaxChars`).

## 6. Math formulas from born-digital glyphs

### 6.1 Representations

- **Symbol Layout Tree (SLT):** symbols on writing lines, with edges RIGHT, SUP, SUB, ABOVE, BELOW and INSIDE.
- **Baseline Structure Tree (BST):** symbol nodes and region nodes alternate.
- **Operator tree:** the semantics.
- Survey: Zanibbi and Blostein, IJDAR 15(4), 2012, https://www.cs.rit.edu/~rlaz/files/n_t_sf.pdf

### 6.2 BST and DRACULAE (Zanibbi, Blostein, Cordy 2002)

- **Layout pass.** Sort the symbols by minX. `Start` finds the dominant-baseline start by operator dominance. `Hor` walks the baseline, and regions are partitioned per symbol class: ABOVE, BELOW, SUPER, SUBSC, UPPER, LOWER, TLEFT, BLEFT, CONTAINS. Then recurse into each region.
- **Region thresholds** are set by a centroid ratio $c$ and a threshold ratio $t$ of the bounding-box height H, both in [0, 0.5] (Table 1). Tested with t = 1/6 and c = 1/4.
- **Later passes.** The lexical pass groups "sin", decimals and "=" and labels fractions and accents; the Lexed BST then goes to LaTeX. The expression pass builds the operator tree.
- **Complexity:** worst case O(n² log n), near linear in practice.
- **Results:** UW-III, 73 expressions with 1,917 symbols, 86-90% of tokens placed correctly. The whole-expression rate is "low".
- **Failure modes:** kerned subscripts read as BELOW, and a bracket whose centroid falls below a fraction bar.
- Sources: TPAMI 24(11):1455-1467, DOI 10.1109/TPAMI.2002.1046157, https://research.cs.queensu.ca/home/cordy/Papers/TPAMI_Math.pdf

### 6.3 Virtual link network and minimum-cost spanning tree (Eto and Suzuki 2001, INFTY)

- **Normalised plane.** Each pair (parent P, child C) maps to $H=1000\,h_2/h_1$ and $D=1000\,(c_1-c_2)/h_1$. Here $h$ is the normalised size, including the ascender and descender parts in the measured ratio 28 : 51 : 21, and $c$ is the normalised centre.
- **Edge costs.** Per relation type (Horizontal, R/L Sup, R/L Sub, Upper, Under, InRoot, InAccent), polygons from 32 journals mark a "strict" and a "wide" area. An edge costs 30 inside the strict area and 70 inside only the wide one. A script on operators that rarely carry one costs 100 more.
- **Search.** Find admissible spanning trees: no node has two children with the same label, the candidates are consistent, and scripts lie left of the horizontal child. Build them right to left with a beam of S candidates.
- **Re-scoring.** Add global penalties: a script ≥ its base size (+100 when equal, +200 when larger), a script at a baseline glyph's size and centre (+100), and unequal baseline glyphs (+100).
- **Results:** 123 lines from 12 journals with 4,268 links: 99.16% correct links and 110 lines perfect. Links between "undetermined" characters were 99.33% correct.
- Source: ICDAR 2001, https://www.inftyproject.org/articles/2001_Icdar_Etou.pdf
- **INFTY** overall: DocEng 2003, DOI 10.1145/958220.958239. Later work fed InftyReader with PDF characters from PDFMiner (as described in Shah et al. 2021, §2).

### 6.4 MaxTract (Baker, Sexton, Sorge)

- **Method.** Glyph names, fonts and sizes from the PDF, combined with exact boxes from rendering, are linearised into tokens such as `<parenleftbigg, CMEX10, 9.963>`. An LALR **linear grammar** parses them, and drivers emit LaTeX or MathML. Compound glyphs (radicals, big braces) are merged by box overlap. Later versions add whole-page projection-profile cutting.
- **Results** (DML 2011 slides, joint ICDAR 2011 comparison with Infty; 10 pages from 5 papers, 628 expressions):
  - Characters: MaxTract misrecognised 0 of 17,830 characters; Infty misrecognised 67 of 20,849 objects.
  - Formulas: Infty got 550 correct, MaxTract 235.
  - MaxTract's losses were segmentation, not structure: 172 split expressions, 103 spacing differences, 102 extra characters.
- Sources: MKM 2009 (https://research.birmingham.ac.uk/en/publications/a-linear-grammar-approach-to-mathematical-formula-recognition-fro/), DAS 2010 "Faithful mathematical formula recognition from PDF documents", thesis https://etheses.bham.ac.uk/id/eprint/3377, slides https://www.fi.muni.cz/~sojka/dml-2011-baker-sexton-sorge.pdf, ICDAR 2011 https://www.computer.org/csdl/proceedings-article/icdar/2011/4520a463/12OmNvF83o4
- **Lesson [INFERENCE]:** structure from glyph geometry is the easy part; finding where a formula starts and ends is the hard part.

### 6.5 Line-of-sight graphs and maximum spanning trees (RIT DPRL, MathSeer)

- **Method.** The candidate edges are a line-of-sight (LOS) graph between symbols (Hu and Zanibbi, ICFHR 2016). Learned CNN classifiers label nodes and edges. The SLT is the maximum spanning arborescence, found with Edmonds' algorithm (Edmonds 1967; O(E + V log V) per Gabow et al. 1986, unverified DOIs).
- **QD-GGA results** (learned): on InftyMCCDB-2 (6,830 formulas), structure rate 92.56% and expression rate 85.94%.
- **LGAP** gains +6.55% expression rate over QD-GGA (ablation row: symbol F1 98.23/95.21, relation F1 94.63/94.28, expression rate 89.21/81.77): https://shahayush.com/files/Shah2023_LGAP.pdf
- **SymbolScraper** (Apache-2.0, https://github.com/zanibbi/SymbolScraper): exact glyph boxes from outlines; merges intersecting glyphs into compound characters; reads horizontal rules from `strokepath`. Of 100 pages, 62 had every box right, and 94.7% of born-digital pages without Type3 fonts were error-free; about 1.7 s per page.
- **Pipeline paper:** ICDAR 2021, https://www.cs.rit.edu/~rlaz/files/ICDAR2021_MathSeer_Pipeline.pdf
- **Excluded:** the full extraction pipeline is GPL-3.0 (https://gitlab.com/dprl/graphics-extraction). LgEval 0.3.4 is Creative Commons with "No commercial use is permitted" (https://github.com/michaelyin/lgeval). Its metrics can be reimplemented from the paper (Zanibbi, Mouchère, Viard-Gaudin, SPIE 8658, 2013).
- **What transfers to RayoPDF:** LOS candidate edges (O(n) per symbol after pruning) and a spanning arborescence. Hand-made or TeX-derived edge costs can stand in for the learned classifiers, which is the Eto–Suzuki approach.

### 6.6 Typesetting priors: TeX Appendix G and the OpenType MATH table

The layout rules a formula was set with are known, so thresholds can be derived from them instead of tuned.

- **TeX font parameters,** mapped to OpenType names (https://mathml-refresh.github.io/math-constants.html), with Firefox's fallbacks in units of x-height = 430.556/1000 em, which equal Computer Modern's values:

  | Constant | TeX parameter | Value |
  |---|---|---|
  | superscriptShiftUp | σ13 | 412.892/430.556 x-height ≈ **0.413 em** |
  | superscriptShiftUpCramped | σ15 | 288.889/430.556 x-height ≈ 0.289 em |
  | subscriptShiftDown | σ16 (σ17 with a superscript) | 150/430.556 x-height ≈ **0.15 em** (0.247 em) |
  | axisHeight | σ22 | 250/430.556 x-height ≈ **0.25 em** |
  | fractionRuleThickness | ξ8 | default rule thickness |
  | numerator and denominator shifts | σ8/σ9, σ11/σ12 | per font |
  | stack gaps | 3ξ8 and 7ξ8 | |

- **OpenType MATH** (https://learn.microsoft.com/en-us/typography/opentype/spec/math): `scriptPercentScaleDown` 80% and `scriptScriptPercentScaleDown` 60% are suggested values. `subscriptTopMax` = 4/5 x-height, `superscriptBottomMin` = 1/4 x-height, `fractionNumeratorGapMin` = default rule thickness (3× in display style). Plain TeX's 10/7/5 pt script sizes (ratios 0.7 and 0.5) are from memory [unverified].
- **Derived classifier [INFERENCE].** Take the script's baseline offset d in units of the base size, at a script size ratio of about 0.7. The three class centres are 0 (same baseline), −0.15/−0.247 (subscript) and +0.289/+0.413 (superscript). The nearest-centre boundaries are ≈ −0.075 and ≈ +0.145. RayoPDF's tuned −0.08 and +0.12 (`math.cpp:1229-1230`) sit almost exactly on these midpoints, so the TeX model explains the current values and also extends to other fonts' MATH tables.
- **Fractions.** A filled rule of height about ξ8 whose midline sits at baseline + σ22·size, with glyphs centred above and below, is a fraction bar. RayoPDF's ≤ 0.3 em height test is about 7× ξ8.
- **Font-encoded classes.** CMMI is math italic, CMSY holds symbols and calligraphic capitals, CMEX holds big operators and delimiter pieces, which hang from their origin. RayoPDF already classifies these (`MathFontOf`, `PrepareMathGlyphs`, `math.h`). MaxTract's tokens carry the font name for the same reason.

### 6.7 Formula detection in PDFs

Lin, Gao, Tang, Lin, Hu, ICDAR 2011 (http://www.iapr-tc11.org/archive/icdar2011/fileup/PDF/4520b419.pdf):

| | Rule-based F1 | Hybrid F1 |
|---|---|---|
| Isolated (display) formulas | 90.60% | 96.14% (learned) |
| Embedded (inline) formulas | 83.61% | — |

Journal version with Baker and Sorge: IJDAR 17(3):239-255, 2014.

### 6.8 Producing LaTeX, and evaluation

- **LaTeX from an SLT or BST** is a tree walk: emit the symbol, then `^{SUP}` and `_{SUB}`. A fraction line's ABOVE and BELOW children give `\frac{A}{B}`. A big operator's children give `\sum_{B}^{A}`, and a radical gives `\sqrt[TLEFT]{CONTAINS}`. Then emit the RIGHT sibling. DRACULAE goes Lexed BST → LaTeX, and QD-GGA goes SLT → LaTeX or MathML.
- **Metrics.**
  - Expression rate (exact match) and structure rate.
  - Symbol, relation and segmentation F1 on label graphs (Zanibbi et al., SPIE 8658, 2013).
  - CDM: render both LaTeX strings and match characters spatially. It agrees with human judgement better than BLEU or edit distance. CVPR 2025, https://arxiv.org/abs/2409.03643; code Apache-2.0 at https://github.com/opendatalab/UniMERNet/tree/main/cdm. It needs a LaTeX renderer, so it belongs in an eval image only.
  - BLEU and edit distance are sensitive to equivalent notations.

## 7. Robust statistics for thresholds, and keeping fits deterministic and bounded

- **Median and MAD.** $\hat\sigma=1.4826\,\mathrm{med}_i|x_i-\mathrm{med}\,x|$, with 50% breakdown. $Q_n$ and $S_n$ are more efficient scale estimators with the same breakdown, computable in O(n log n). Rousseeuw and Croux, JASA 88(424):1273-1283, https://www.tandfonline.com/doi/abs/10.1080/01621459.1993.10476408
- **Hodges–Lehmann.** $\mathrm{med}\{(x_i+x_j)/2\}$, in O(n log n) by Monahan's algorithm (ACM TOMS 10(3), 1984, algorithm 616; see https://aakinshin.net/posts/acm616). Original paper: Ann. Math. Stat. 34(2), 1963 (DOI 10.1214/aoms/1177704172, unverified). Its breakdown of about 29% is from memory [unverified].
- **Trimmed means and Huber M-estimators** are cheap when the outlier share is bounded.
- **Selection.** BFPRT gives a worst-case O(n) median (JCSS 7(4), 1973; DOI unverified). `std::nth_element` is linear on average.
- **Determinism and time bounds** [INFERENCE, applying the sources above]:
  1. Bin in integer fixed-point units (1/100 em for gaps, 1/20 body for pitch, as `WideLeading` already does) and use int64 prefix sums. The result is then independent of floating-point summation order and thread count.
  2. Use closed-form or O(B) threshold searches (Otsu, Kittler–Illingworth) instead of EM. If EM is used: deterministic quantile initialisation, a fixed iteration cap (≤ 20), and ties broken towards the lower bin.
  3. Cap the sample: deterministic stride subsampling above N_max (for example 4,096 gaps per cluster), so the time is O(min(n, N_max) + B).
  4. Gate on evidence: fall back to the current constant unless n ≥ n_min and two components beat one by BIC (or the dip test rejects unimodality). Clamp the fitted value to a plausible interval, for example word gap ∈ [0.08, 0.40] em.
  5. Fit per document, per (font, size) cluster, and pool pages in page order.

## 8. For RayoPDF

These all share one new component [INFERENCE]: a small deterministic `RobustFit` utility (integer histogram, Otsu and Kittler–Illingworth thresholds, median/MAD, Fisher DP with BIC, a binomial tail) used by items 1, 2, 4 and 5. It costs O(n + B) per call. Every expected effect below is [INFERENCE] until measured. Each item must be tuned on readoc-dev and checked against readoc-val aggregates only (AGENTS.md rule 7).

| # | Decision | Statistic or model | Expected effect | Cost per page | Risk |
|---|---|---|---|---|---|
| 1 | **Word break** (replaces `kWordGap 0.13`, `kWordGapWithSpaces 0.3`, `kSpaceShare 0.85`; `lines.h:17-18`, `lines.cpp:1105,1433-1437`) | Per (font, size) cluster: Kittler–Illingworth on the integer gap histogram. Where the line draws space glyphs, use them as labels (§1.2). Prior: pdf.js `max(0.5·spaceEm, 0.102)`, Apache-2.0 so reusable. Clamp to [0.08, 0.40] em and fall back below 20 samples. | Fewer split and merged words in kerned or letter-spaced runs; NID and olmOCR long_tiny_text. | One extra pass over the glyph gaps, O(g + B·clusters); ≈ 1-3 µs at 2k glyphs, B = 110. | Short documents give no fit (fallback). Two passes over the line loop add code. Letter-spaced headings are already handled by "every gap is as wide". |
| 2 | **Heading levels and candidates** (replaces the half-point style key `lround(size*2)`, and puts `kHeadingSize 1.12` on a data-driven scale) | Fisher DP on character-weighted log-size of non-body lines, k ≤ 6 chosen by BIC with a variance floor. Candidate if log s − log body > 3.5 × 1.4826·MAD of body-line log sizes, still ≥ 1.05×. The outline and numbering stay authoritative. | Steadier levels when sizes jitter (12.24 vs 12.26 pt now split into two keys); odl MHS and READoc heading levels. | O(k·d²) per document, d = distinct sizes ≤ 50: under 10 µs per document. | Same-style levels need the numbering refinement, which is kept. Titles that occur once need the minimum-weight rule. |
| 3 | **Running headers and footers** (replaces the global "≥ ⌈n/2⌉ pages" count, `reading_order.cpp:1153-1157`) | Lin's windowed score (W = 8, digit→`#` keys already exist), using bounded Myers edit distance × box-overlap similarity, then a binomial tail test (§5). Add a Déjean–Meunier step that accepts a lone number forming a +1 sequence across pages. | Running heads that change per chapter get removed: olmOCR headers_footers 0.849 towards docling's 0.898. The odl ground truth *keeps* furniture, so odl NID may drop; report both. | O(P·2W·8 comparisons), ≈ 100 ns each: about 13 µs per page. | Repeated table headers or section titles at page tops (gate with position bands and geometry). Two-page documents (W shrinks). |
| 4 | **Column gutter and fragment split** (replaces `kGutterCandidate 0.6`, `kJoinGap 0.8`, `Fmax(3, 0.5·size)`; `lines.h:20-22`, `reading_order.cpp:475`) | Gutter if gap > μ̂_word + 4σ̂_word, from item 1's word-gap component (median/MAD), floored at 0.4 em. Keep Breuel's "bounded by text lines on both sides". | Fewer false column cuts in wide-spaced justified text and fewer missed narrow gutters; olmOCR multi_column 0.710. | O(1) per decision; reuses item 1's statistics. | Rivers in justified text (`WordSpaced` check exists). Mixed fonts in one line. |
| 5 | **Script, limit and fraction geometry in math** (replaces the +0.12/−0.08 em, 0.92 size ratio, 0.3 em bar height and 0.45/0.65 em limits in `math.cpp:784,1219-1230,1297`) | TeX or OpenType constants (σ13, σ15, σ16, σ17, σ22, ξ8, script scale) as class centres in Eto–Suzuki's (H, D) plane, with a per-document scale fitted as the median offset of confidently classed pairs. Read the MATH table of an embedded OpenType math font when present. | Better scripts and fractions for non-CM fonts (Word/OMML, STIX); arxiv_math, which is already the best of the measured tools. | O(n) per formula. | Producers that fake scripts with Ts; cramped versus display style; the risk of regressing a tuned path. Gate behind `--math=latex` (already the case). |
| 6 | **Math structure as a min-cost arborescence** (replaces the greedy passes for fractions, radicals, limits, matrices and scripts) | LOS candidate edges with TeX-derived costs (§6.3, §6.6), then Edmonds/Tarjan. The `Unsure` status stays as the fallback. | Fewer `Unsure` fallbacks to Unicode on nested structures; arxiv_math. | O(E log V) with E ≈ 8n after LOS pruning, n ≤ `kMaxFormulaGlyphs = 1024`: about 10-50 µs per formula. | The biggest change on the list; hard to keep byte-identical output; needs the label-graph metric reimplemented (LgEval is non-commercial). |
| 7 | **Borderless table columns** (replaces `gapFactor`, "two thirds of rows" and "an em wide" in `tables.cpp:1877-1884,3246`) | Penalised alignment likelihood over candidate gaps, solved exactly by DP (§4): robust cell-edge MAD per column, fill share, λ = ½ ln R per column. Row breaks at median pitch + 3·MAD. | Fewer merged or split columns; olmOCR table_tests 0.631 and READoc table_teds 0.618. | O(m²) with m ≤ 64 gaps per candidate block: about 5 µs per block. | Prose rejected as non-tables (`Texty`, `ListLike`) must stay ahead of the DP; spanning header labels need `HeaderOverColumns` to stay. |

**Order [INFERENCE]:** 1, then 4 (same statistics), then 3, 2, 5, 7, 6. Items 1-4 add under 20 µs per page against about 450 µs per page now (2,230 pages/s), so the speed claims hold.

**Licences:**
- May be copied or used to train shipped models: pdf.js and PDFBox (Apache-2.0); pdfminer.six, tabula-java, Camelot and pdfplumber (MIT); Tesseract and SymbolScraper (Apache-2.0); CDM code (Apache-2.0, eval only).
- Excluded:
  - MuPDF (AGPL; read only).
  - The DPRL graphics-extraction pipeline and the xycut-plus-plus Rust port (GPL-3.0).
  - LgEval (CC non-commercial).
  - READoc code (no licence; run from a clone only).
  - docling-pdf (licence not checked).

### Critical Files for Implementation
- `src/layout/lines.h` / `src/layout/lines.cpp` — word-gap, fragment and gutter constants and the `BuildLines` gap loop (items 1, 4)
- `src/layout/reading_order.cpp` — `RemoveRunningLines`, `RunningKey`, `SplitX` cut width (items 3, 4)
- `src/layout/semantics.cpp` — body size, `kHeadingSize`/`kTitleSize`, style keys and the heading-level assignment (item 2)
- `src/layout/math.cpp` — script, fraction, radical and limit thresholds and the greedy structure passes (items 5, 6)
- `src/layout/tables.cpp` — `Borderless`, `Columns`, `SplitColumnsByText`, header detection (item 7)
