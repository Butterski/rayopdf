// SPDX-License-Identifier: Apache-2.0
// Types of rayopdf.js: RayoPDF's converter in WebAssembly.

export interface ConvertOptions {
  /** markdown (default), json (the JSON document, schema 1), chunks (JSON Lines) or inspect (HTML). */
  format?: "markdown" | "json" | "chunks" | "inspect";
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
  /** OCR needs the native add-on, which WebAssembly cannot start: "auto" and "force" find none. */
  ocr?: "off" | "auto" | "force";
  /** User or owner password of an encrypted PDF. */
  password?: string;
  /** 3, "2-5", "-4", or [first, last] with null for an open end. */
  pages?: number | string | [number | null, number | null];
  /** Chunks: at most this many bytes of Markdown a chunk, 256 to 1,000,000 (default 4000). */
  chunkChars?: number;
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
  /** Turn link annotations into Markdown links (default true). */
  annotations?: boolean;
  /** Write a verified RayoMD source payload as it is (default true). */
  exact?: boolean;
}

export interface Chunk {
  chunk: number;
  heading_path: string[];
  pages: [number, number];
  boxes: { page: number; bbox: [number, number, number, number] }[];
  blocks: number[];
  markdown: string;
}

export interface ConvertResult {
  /** The Markdown, or the JSON, JSON Lines or HTML of `format`. */
  text: string;
  /** The JSON document for format "json", the chunks for "chunks", else null. */
  data: Record<string, unknown> | Chunk[] | null;
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
