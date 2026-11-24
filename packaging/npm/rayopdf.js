// SPDX-License-Identifier: Apache-2.0
// RayoPDF in WebAssembly: PDF to Markdown (or JSON, chunks, the layout inspector's HTML) in the
// browser or Node.js, with the converter of the command line and its options.
//
//   import { convert } from "rayopdf";
//   const { text } = await convert(new Uint8Array(pdfBytes));
//
// rayopdf-core.mjs and rayopdf-core.wasm are the C ABI (include/rayopdf/rayopdf_c.h) built with
// Emscripten; one module instance serves every call, one call at a time, on one thread.
import createModule from "./rayopdf-core.mjs";

export class RayoPdfError extends Error {
  constructor(status, message) {
    super(message);
    this.name = "RayoPdfError";
    this.status = status; // the command line's exit code: 2 usage, 3 unreadable, 10 corrupt, ...
  }
}

let instance = null;

function load() {
  if (instance === null) instance = createModule();
  return instance;
}

// The flags of `rayopdf --convert` for `options` (Python's option names in camelCase).
function flags(options, imageFolder) {
  const out = [
    `--format=${options.format ?? "markdown"}`,
    `--math=${options.math ?? "unicode"}`,
    `--furniture=${options.furniture ?? "drop"}`,
    `--tables=${options.tables ?? "auto"}`,
    `--quotes=${options.quotes ?? "keep"}`,
    `--small-caps=${options.smallCaps ?? "lower"}`,
    `--figure-text=${options.figureText ?? "keep"}`,
    `--ocr=${options.ocr ?? "off"}`,
  ];
  if (options.password != null) out.push(`--password=${options.password}`);
  if (options.pages != null) {
    const pages = options.pages;
    out.push(`--pages=${Array.isArray(pages) ? `${pages[0] ?? ""}-${pages[1] ?? ""}` : pages}`);
  }
  if (options.chunkChars != null) out.push(`--chunk-chars=${options.chunkChars}`);
  if (imageFolder !== null) out.push(`--images=${imageFolder}`);
  if (options.pageMarkers) out.push("--page-markers");
  if (options.frontMatter) out.push("--front-matter");
  if (options.plain) out.push("--plain");
  if (options.structure === false) out.push("--no-structure");
  if (options.annotations === false) out.push("--no-annotations");
  if (options.exact === false) out.push("--no-exact");
  return out;
}

function writeString(m, text) {
  const size = m.lengthBytesUTF8(text) + 1;
  const pointer = m._malloc(size);
  m.stringToUTF8(text, pointer, size);
  return pointer;
}

// Moves the image files of MEMFS `folder` into a Map (file name -> bytes) and removes them.
function takeImages(m, folder) {
  const images = new Map();
  if (!m.FS.analyzePath(folder).exists) return images;
  for (const name of m.FS.readdir(folder).sort()) {
    if (name === "." || name === "..") continue;
    images.set(name, m.FS.readFile(`${folder}/${name}`));
    m.FS.unlink(`${folder}/${name}`);
  }
  m.FS.rmdir(folder);
  return images;
}

const decoder = new TextDecoder();

// Converts the PDF in `pdf` (a Uint8Array). Resolves to { text, data, report, pages, images }:
// data is the JSON document (format "json") or the array of chunks ("chunks"), else null; images
// is a Map of file name -> bytes, empty unless options.images (true, or the folder name the
// Markdown links them under; true: "images"). Rejects with a RayoPdfError.
export async function convert(pdf, options = {}) {
  if (!(pdf instanceof Uint8Array)) throw new TypeError("convert takes the PDF as a Uint8Array");
  const m = await load();
  const imageFolder = options.images ? (typeof options.images === "string" ? options.images : "images") : null;
  const args = flags(options, imageFolder);
  const pointers = args.map((arg) => writeString(m, arg));
  const argv = m._malloc(4 * Math.max(args.length, 1));
  pointers.forEach((pointer, i) => { m.HEAPU32[(argv >> 2) + i] = pointer; });
  const input = m._malloc(Math.max(pdf.length, 1));
  m.HEAPU8.set(pdf, input);
  const out = m._malloc(24); // rayopdf_output: six 32-bit fields
  if (imageFolder !== null) {
    if (!m.FS.analyzePath("/out").exists) m.FS.mkdir("/out");
    m.FS.chdir("/out");
  }
  let images = new Map();
  try {
    const status = m._rayopdf_convert(input, pdf.length, argv, args.length, out);
    if (imageFolder !== null) images = takeImages(m, `/out/${imageFolder}`);
    const field = (i) => m.HEAPU32[(out >> 2) + i];
    const text = decoder.decode(m.HEAPU8.slice(field(2), field(2) + field(3)));
    const report = m.UTF8ToString(field(4));
    const error = m.UTF8ToString(field(5));
    const pages = field(1);
    m._rayopdf_output_free(out);
    if (status !== 0) throw new RayoPdfError(status, error);
    const format = options.format ?? "markdown";
    let data = null;
    if (format === "json") data = JSON.parse(text);
    else if (format === "chunks") data = text.split("\n").filter((line) => line !== "").map((line) => JSON.parse(line));
    return { text, data, report: report === "" ? {} : JSON.parse(report), pages, images };
  } finally {
    if (imageFolder !== null) m.FS.chdir("/");
    for (const pointer of [...pointers, argv, input, out]) m._free(pointer);
  }
}

// The converter's version, "MAJOR.MINOR.PATCH".
export async function version() {
  const m = await load();
  return m.UTF8ToString(m._rayopdf_version());
}
