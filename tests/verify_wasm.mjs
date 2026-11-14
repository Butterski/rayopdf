// SPDX-License-Identifier: Apache-2.0
// Checks of the npm package (packaging/npm/rayopdf.js) over the built WebAssembly module: a
// one-page PDF to Markdown, the JSON document and chunks; a page with an image gives one image in
// the Map; corrupt input rejects with status 10; a bad option with status 2. The package's files
// and the module are copied into a temporary folder and imported from there; the PDFs are built
// by the script.
//
//   node tests/verify_wasm.mjs --module build/wasm/rayopdf-core.mjs
import { copyFileSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { deflateSync } from "node:zlib";

const repo = join(dirname(fileURLToPath(import.meta.url)), "..");
const at = process.argv.indexOf("--module");
if (at < 0 || at + 1 >= process.argv.length) {
  console.error("usage: node tests/verify_wasm.mjs --module build/wasm/rayopdf-core.mjs");
  process.exit(2);
}
const core = process.argv[at + 1];

// A one-page PDF showing `text` in Helvetica, with a 64x64 RGB image drawn when `image`.
function onePagePdf(text, image = false) {
  const draw = image ? "q 128 0 0 128 72 500 cm /Im1 Do Q " : "";
  const content = deflateSync(Buffer.from(`${draw}BT /F1 12 Tf 72 700 Td (${text}) Tj ET`, "latin1"));
  const parts = [
    Buffer.from("<< /Type /Catalog /Pages 2 0 R >>"),
    Buffer.from("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
    Buffer.from("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >>" +
      (image ? " /XObject << /Im1 6 0 R >>" : "") + " >> /Contents 4 0 R >>"),
    Buffer.concat([Buffer.from(`<< /Length ${content.length} /Filter /FlateDecode >>\nstream\n`), content,
      Buffer.from("\nendstream")]),
    Buffer.from("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"),
  ];
  if (image) {
    const raw = Buffer.alloc(64 * 64 * 3);
    for (let i = 0; i < raw.length; i++) raw[i] = (i * 7) & 255;
    const pixels = deflateSync(raw);
    parts.push(Buffer.concat([Buffer.from("<< /Type /XObject /Subtype /Image /Width 64 /Height 64 /ColorSpace " +
      `/DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode /Length ${pixels.length} >>\nstream\n`), pixels,
      Buffer.from("\nendstream")]));
  }
  const chunks = [Buffer.from("%PDF-1.7\n")];
  let size = chunks[0].length;
  const offsets = [];
  parts.forEach((body, i) => {
    offsets.push(size);
    const object = Buffer.concat([Buffer.from(`${i + 1} 0 obj\n`), body, Buffer.from("\nendobj\n")]);
    chunks.push(object);
    size += object.length;
  });
  let xref = `xref\n0 ${parts.length + 1}\n0000000000 65535 f \n`;
  for (const offset of offsets) xref += `${String(offset).padStart(10, "0")} 00000 n \n`;
  xref += `trailer\n<< /Size ${parts.length + 1} /Root 1 0 R >>\nstartxref\n${size}\n%%EOF\n`;
  chunks.push(Buffer.from(xref));
  return new Uint8Array(Buffer.concat(chunks));
}

let failures = 0;
function check(condition, what) {
  if (!condition) {
    failures++;
    console.error(`FAIL ${what}`);
  }
}

const work = mkdtempSync(join(tmpdir(), "rayopdf-wasm-"));
try {
  for (const name of ["rayopdf.js", "rayopdf.d.ts", "package.json"]) {
    copyFileSync(join(repo, "packaging", "npm", name), join(work, name));
  }
  copyFileSync(core, join(work, "rayopdf-core.mjs"));
  copyFileSync(core.replace(/\.mjs$/, ".wasm"), join(work, "rayopdf-core.wasm"));
  const rayopdf = await import(pathToFileURL(join(work, "rayopdf.js")).href);

  const expected = readFileSync(join(repo, "VERSION"), "utf8").trim();
  check((await rayopdf.version()) === expected, `version() is VERSION ${expected}`);
  const pdf = onePagePdf("Hello from WebAssembly");
  let result = await rayopdf.convert(pdf);
  check(result.text.includes("Hello from WebAssembly") && result.pages === 1 && result.data === null &&
    typeof result.report.glyphs === "number" && result.images.size === 0, "Markdown");
  result = await rayopdf.convert(pdf, { format: "json" });
  check(result.text.startsWith('{"rayopdf":') && result.data.schema === 1, "format json");
  result = await rayopdf.convert(pdf, { format: "chunks", chunkChars: 256, plain: true, quotes: "ascii", pages: [1, null] });
  check(Array.isArray(result.data) && result.data.length === 1 &&
    result.data[0].markdown.includes("Hello from WebAssembly") && result.data[0].pages[1] === 1, "format chunks");
  result = await rayopdf.convert(onePagePdf("A page with a picture", true), { images: true });
  const names = [...result.images.keys()];
  check(names.length === 1 && result.images.get(names[0]).length > 8 && result.text.includes(`](images/${names[0]})`),
    `one image in the Map, linked as images/NAME: ${names}`);
  result = await rayopdf.convert(onePagePdf("A page with a picture", true));
  check(result.images.size === 0 && !result.text.includes("]("), "no images without options.images");
  for (const [input, options, status, what] of [
    [new TextEncoder().encode("not a pdf"), {}, 10, "corrupt input"],
    [pdf, { math: "tex" }, 2, "a bad option"],
  ]) {
    try {
      await rayopdf.convert(input, options);
      check(false, `${what} rejects`);
    } catch (error) {
      check(error instanceof rayopdf.RayoPdfError && error.status === status && error.message !== "",
        `${what} rejects with status ${status}: ${error.status} ${error.message}`);
    }
  }
} finally {
  rmSync(work, { recursive: true, force: true });
}
if (failures > 0) {
  console.error(`${failures} check(s) failed`);
  process.exit(1);
}
console.log("wasm: all checks passed");
