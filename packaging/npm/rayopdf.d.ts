// SPDX-License-Identifier: Apache-2.0
// Types of rayopdf.js: RayoPDF's converter in WebAssembly.

export interface ConvertOptions {
  /** markdown (default), json (the JSON document, schema 1), chunks (JSON Lines), inspect (HTML), text (plain text, pages separated by form feeds) or html (the document as one HTML5 file). */
  format?: "markdown" | "json" | "chunks" | "inspect" | "text" | "html";
  /** Formulas as Unicode text (default) or rebuilt as LaTeX. */
  math?: "unicode" | "latex";
  /** Running headers, footers and page numbers: left out (default) or kept. */
  furniture?: "drop" | "keep";
  /** Pipe tables unless a span needs HTML (default), every table as HTML, every table as a pipe table (spans flattened), or as one whose cells' % # & outside math are escaped for a LaTeX reader (READoc's convention). */
  tables?: "auto" | "html" | "pipe" | "pipe-latex";
  /** Quotation marks as printed (default), or the typographic ones (curly, low) as ' and ". */
  quotes?: "keep" | "ascii";
  /** Headings set in small capitals in the source's case (default), or as printed, in capitals. */
  smallCaps?: "lower" | "keep";
  /** A figure's own text (tick labels, legends, the labels of a diagram) as paragraphs (default), or left out. */
  figureText?: "keep" | "drop";
  /** OCR needs the native add-on, which WebAssembly cannot start: "auto" and "force" find none. */
  ocr?: "off" | "auto" | "force";
  /** User or owner password of an encrypted PDF. */
  password?: string;
  /** 3, "2-5", "-4", or [first, last] with null for an open end. */
  pages?: number | string | [number | null, number | null];
  /** Chunks: at most this many bytes of Markdown a chunk, 256 to 1,000,000 (default 4000). */
  chunkChars?: number;
  /** JSON: each block's boxes only (default), its lines too (page, box, text), or its lines with their words. */
  jsonDetail?: "blocks" | "lines" | "words";
  /** Write the images: true (linked as images/NAME) or the folder name the Markdown links them under. */
  images?: boolean | string;
  /** <!-- page N --> before each page's Markdown. */
  pageMarkers?: boolean;
  /** YAML front matter from the document's information. */
  frontMatter?: boolean;
  /** No inline markup: no emphasis, code spans, links, <sup>/<sub>, footnote marks as numbers. */
  plain?: boolean;
  /** Use a tagged PDF's structure tree (default true). */
  structure?: boolean;
  /** Annotations: true or "all" (default; links become Markdown links, and the text of form fields and FreeText annotations is read), "links" (links only), false or "none" (no annotation read). */
  annotations?: boolean | "all" | "links" | "none";
  /** Write a verified RayoMD source payload as it is (default true). */
  exact?: boolean;
}

/** x0, y0, x1, y1 in points, origin at the bottom left of the page. */
export type Rect = [number, number, number, number];

export interface Box {
  page: number;
  bbox: Rect;
}

export interface Chunk {
  chunk: number;
  heading_path: string[];
  pages: [number, number];
  boxes: Box[];
  blocks: number[];
  markdown: string;
}

/** A word of a line (jsonDetail "words"). */
export interface JsonWord {
  box: Rect;
  text: string;
}

/** A line of a block (jsonDetail "lines" or "words"). */
export interface JsonLine {
  page: number;
  box: Rect;
  text: string;
  words?: JsonWord[];
}

export interface JsonCell {
  row: number;
  col: number;
  row_span: number;
  col_span: number;
  text: string;
}

/** A block of the JSON document (schema 1); `markdown` is its last field. */
export interface JsonBlock {
  type: "heading" | "paragraph" | "list_item" | "code" | "table" | "image" | "footnote" | "comment";
  page: number;
  boxes: Box[];
  role?: "caption" | "formula" | "page_header" | "page_footer" | "page_number";
  level?: number;
  depth?: number;
  ordered?: boolean;
  number?: number;
  task?: "open" | "done";
  rows?: number;
  cols?: number;
  header_rows?: number;
  cells?: JsonCell[];
  file?: string | null;
  alt?: string;
  quote?: number;
  alert?: "note" | "tip" | "important" | "warning" | "caution";
  lines?: JsonLine[];
  markdown: string;
}

/** The JSON document of format "json" (schema 1). */
export interface JsonDocument {
  rayopdf: string;
  schema: 1;
  repaired: boolean;
  /** title, author, subject, keywords, creator, producer, created, modified, language: those the PDF has. */
  metadata: Record<string, string>;
  /** label only when the PDF has page labels. */
  pages: { page: number; width: number; height: number; label?: string }[];
  /** Only when the PDF has bookmarks; page null when the destination leads to none. */
  outline?: { level: number; title: string; page: number | null }[];
  blocks: JsonBlock[];
}

export interface ConvertResult {
  /** The Markdown, or the JSON, JSON Lines, HTML or plain text of `format`. */
  text: string;
  /** The JSON document for format "json", the chunks for "chunks", else null. */
  data: JsonDocument | Chunk[] | null;
  /** The --report counters. */
  report: Record<string, unknown>;
  /** Pages converted. */
  pages: number;
  /** The image files by name (empty unless options.images). */
  images: Map<string, Uint8Array>;
}

export class RayoPdfError extends Error {
  /** The command line's exit code: 2 usage, 3 unreadable, 10 corrupt, 11 password required, 12 unwritable, 13 unsupported encryption. */
  status: number;
  constructor(status: number, message: string);
}

/** Converts the PDF in `pdf`; rejects with a RayoPdfError. */
export function convert(pdf: Uint8Array, options?: ConvertOptions): Promise<ConvertResult>;

/** The converter's version, "MAJOR.MINOR.PATCH". */
export function version(): Promise<string>;
