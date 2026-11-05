Sentinel start D04.

# Fast Layout Analysis of Born-Digital Documents with Projection Profiles

Maria Kowalska, Jonas Becker and Li Wei — Institute for Document Engineering, Example University

**Abstract.** Converting born-digital documents into structured text requires recovering the reading order of text blocks from their positions on the page. Learned models achieve high accuracy on this task but are expensive to run. We revisit recursive projection-profile segmentation and show that, with a small set of geometric rules, it reaches an accuracy within 2 % of a learned baseline on three public benchmarks while running more than two orders of magnitude faster. We describe the algorithm, analyse its complexity, and report results on 1,200 annotated pages.

## 1. Introduction

Documents are increasingly produced, exchanged and archived in fixed-layout formats. A fixed layout preserves the visual appearance of a page but discards most of its logical structure: the order in which text should be read, the hierarchy of headings, and the boundaries of tables and figures. Recovering this structure is a prerequisite for search, accessibility and the reuse of content in other formats.[^1]

Two families of methods dominate the literature. Geometric methods segment the page using white space and alignment, while learned methods classify regions with neural networks trained on annotated pages. Learned methods are more robust on complex layouts, but they require substantial computation, often a graphics processor, and they can fail unpredictably on layouts unlike their training data.

### 1.1 Contributions

This paper makes three contributions. First, we describe a variant of recursive XY-cut that handles spanning elements such as titles and wide figures. Second, we give a linear-time implementation based on sorted interval lists. Third, we evaluate the method against a learned baseline on three benchmarks and analyse the remaining errors.

## 2. Related Work

Recursive XY-cut was introduced by Nagy and Seth [1] and later formalised as a tree decomposition of the page [2]. Breuel proposed an alternative based on maximal white-space rectangles and a set of ordering constraints [3]. Learned approaches include region detectors trained on large synthetic corpora [4, 5] and graph neural networks that predict reading-order relations between text lines [6].

### 2.1 Evaluation benchmarks

Reading order is commonly evaluated with edit distance on the extracted text [7] or with the tree-edit distance on the predicted structure [8]. We use both, together with a heading-level metric introduced in recent work [9].

## 3. Method

Let a page contain $n$ text blocks $b_1, \ldots, b_n$, each with a bounding box $(x_0, y_0, x_1, y_1)$. The projection profile on the horizontal axis is the function

$$P_x(t) = \sum_{i=1}^{n} \mathbf{1}[x_0^{(i)} \le t \le x_1^{(i)}],$$

and a vertical cut is possible wherever $P_x(t) = 0$ over an interval of width at least $\tau$. The analogous definition holds for the vertical axis.[^2]

### 3.1 Spanning elements

Elements that span several columns prevent any vertical cut. We detect them as blocks whose width exceeds $\rho$ times the median column width, remove them temporarily, segment the remaining blocks, and reinsert each spanning element above the first block whose top edge lies below it.

### 3.2 Complexity

Sorting the block boundaries costs $O(n \log n)$. Each level of recursion scans the sorted lists once, and the recursion depth is bounded by the number of distinct cuts, so the total cost is

$$T(n) = O(n \log n + n d),$$

where $d$ is the depth of the resulting tree. In practice $d \le 6$ on all pages we examined.

### 3.3 Parameters

We use $\tau = 0.6\,\bar{h}$, where $\bar{h}$ is the median line height, and $\rho = 1.3$. Both values were chosen on a development set and kept fixed for all experiments.[^3]

## 4. Experiments

We evaluate on three public benchmarks containing a total of 1,200 pages: scientific articles, business reports and textbooks. All timings were taken on a single core of a desktop processor.

### 4.1 Results

| Method | Text NID | Order accuracy | Heading MHS | ms/page |
| --- | ---: | ---: | ---: | ---: |
| Plain text extraction | 0.842 | 0.711 | 0.000 | 1.9 |
| Learned detector | 0.913 | 0.902 | 0.774 | 412.0 |
| Graph network | 0.921 | 0.915 | 0.781 | 655.0 |
| XY-cut (ours) | 0.905 | 0.889 | 0.752 | 2.6 |

The proposed method reaches 98 % of the learned detector’s order accuracy while being about 160 times faster. Figure 1 shows the distribution of per-page processing times.

![chart](img/chart.png)

Figure 1. Per-page processing time for the five benchmark subsets, in milliseconds.

### 4.2 Error analysis

Most remaining errors occur on pages with side notes, where the narrow margin column is merged with the main text, and on pages where a figure caption sits between two columns. Together these cases account for 71 % of order errors.[^4]

### 4.3 Ablation

Removing the spanning-element step lowers order accuracy from 0.889 to 0.812, mostly on the scientific articles, where titles and abstracts span both columns. Replacing the adaptive threshold $\tau$ with a fixed value of 12 points lowers accuracy to 0.861, because documents set in small type have narrow gutters that a fixed threshold misses. Disabling the header and footer filter has a smaller effect on order accuracy but reduces text NID by 0.014, since running heads are then interleaved with body text.

We also measured sensitivity to $\rho$. Values between 1.2 and 1.5 give accuracy within 0.005 of the best value; below 1.1, ordinary paragraphs in wide single-column layouts are misclassified as spanning elements, and above 1.8 two-column titles are no longer detected.

### 4.4 Throughput

Because the method uses only block geometry, its cost is dominated by text extraction rather than by segmentation. On the full benchmark the segmentation step accounts for 0.4 ms of the 2.6 ms per page; the rest is spent decoding content streams and resolving fonts. Pages can be processed independently, so throughput scales almost linearly with the number of cores up to the memory bandwidth limit.

## 5. Conclusion

Projection-profile segmentation remains a strong baseline for born-digital documents. With simple handling of spanning elements it approaches the accuracy of learned methods at a fraction of the cost. Future work will address side notes and evaluate the method on documents with right-to-left scripts.

## References

1. G. Nagy and S. Seth. Hierarchical representation of optically scanned documents. In *Proc. ICPR*, 1984.
2. J. Ha, R. Haralick and I. Phillips. Recursive X-Y cut using bounding boxes of connected components. In *Proc. ICDAR*, 1995.
3. T. Breuel. High performance document layout analysis. In *Proc. SDIUT*, 2003.
4. X. Zhong, J. Tang and A. Jimeno Yepes. PubLayNet: largest dataset ever for document layout analysis. In *Proc. ICDAR*, 2019.
5. B. Pfitzmann et al. DocLayNet: a large human-annotated dataset for document-layout analysis. In *Proc. KDD*, 2022.
6. Z. Wang et al. LayoutReader: pre-training of text and layout for reading order detection. In *Proc. EMNLP*, 2021.
7. V. Levenshtein. Binary codes capable of correcting deletions, insertions and reversals. *Soviet Physics Doklady*, 1966.
8. X. Zhong, E. ShafieiBavani and A. Jimeno Yepes. Image-based table recognition: data, model, and evaluation. In *Proc. ECCV*, 2020.
9. Z. Li et al. READoc: a unified benchmark for realistic document structured extraction. arXiv:2409.05137, 2024.
10. K. Zhang and D. Shasha. Simple fast algorithms for the editing distance between trees. *SIAM J. Computing*, 1989.

Sentinel end D04.

[^1]: Accessibility regulations increasingly require structured content.

[^2]: Profiles are computed on block boxes, not on individual glyphs.

[^3]: The development set is disjoint from all test sets.

[^4]: Counted over all three benchmarks combined.
