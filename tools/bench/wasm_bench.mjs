// SPDX-License-Identifier: Apache-2.0
// In-process speed of the WebAssembly module (tools/bench.py wasm runs it in rayopdf-wasm:1). It
// calls _rayopdf_convert as packaging/npm/rayopdf.js does, with --format=markdown --ocr=off and
// the --opt flags after them, one module instance for every file. Per file: one untimed
// conversion (its text written as DIR/<5-digit index>.md with --out, the index counting the files
// from 0), then RUNS timed conversions (performance.now() around the call and the freeing of its
// output), keeping the fastest.
//
//   node tools/bench/wasm_bench.mjs --module PATH/rayopdf-core.mjs --runs N [--out DIR] [--opt FLAG]... -- FILE...
//
// Prints one JSON line {files, runs, docs: [{file, pages, status, best_ms}]}: status is the C ABI's
// (the command line's exit code), or "trap: MESSAGE" when the module aborted (a new instance then
// serves the next file); best_ms is null when RUNS is 0 or the conversion failed.
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { performance } from "node:perf_hooks";
import { pathToFileURL } from "node:url";

const usage = "usage: node wasm_bench.mjs --module PATH/rayopdf-core.mjs --runs N [--out DIR] [--opt FLAG]... " +
  "-- FILE...";
let modulePath = null;
let runs = null;
let outDir = null;
const opts = [];
const files = [];
const argv = process.argv.slice(2);
for (let i = 0; i < argv.length; i++) {
  const arg = argv[i];
  if (arg === "--") {
    files.push(...argv.slice(i + 1));
    break;
  }
  if (i + 1 >= argv.length) {
    console.error(usage);
    process.exit(2);
  }
  const value = argv[++i];
  if (arg === "--module") modulePath = value;
  else if (arg === "--runs") runs = Number.parseInt(value, 10);
  else if (arg === "--out") outDir = value;
  else if (arg === "--opt") opts.push(value);
  else {
    console.error(usage);
    process.exit(2);
  }
}
if (modulePath === null || !Number.isInteger(runs) || runs < 0 || files.length === 0) {
  console.error(usage);
  process.exit(2);
}
if (outDir !== null) mkdirSync(outDir, { recursive: true });

const createModule = (await import(pathToFileURL(resolve(modulePath)).href)).default;
const args = ["--format=markdown", "--ocr=off", ...opts];

function writeString(m, text) {
  const size = m.lengthBytesUTF8(text) + 1;
  const pointer = m._malloc(size);
  m.stringToUTF8(text, pointer, size);
  return pointer;
}

// A module instance with the flags and the output record in its memory.
async function instance() {
  const m = await createModule();
  const argvPointer = m._malloc(4 * args.length);
  args.forEach((arg, i) => { m.HEAPU32[(argvPointer >> 2) + i] = writeString(m, arg); });
  return { m, argv: argvPointer, out: m._malloc(24) }; // rayopdf_output: six 32-bit fields
}

let current = await instance();
const docs = [];
for (const [index, file] of files.entries()) {
  const pdf = readFileSync(file);
  const { m, argv: argvPointer, out } = current;
  const input = m._malloc(Math.max(pdf.length, 1));
  m.HEAPU8.set(pdf, input);
  const doc = { file, pages: 0, status: 0, best_ms: null };
  try {
    const status = m._rayopdf_convert(input, pdf.length, argvPointer, args.length, out);
    const field = (i) => m.HEAPU32[(out >> 2) + i];
    doc.status = status;
    doc.pages = field(1);
    if (outDir !== null) {
      writeFileSync(`${outDir}/${String(index).padStart(5, "0")}.md`, m.HEAPU8.subarray(field(2), field(2) + field(3)));
    }
    m._rayopdf_output_free(out);
    if (status === 0) {
      for (let run = 0; run < runs; run++) {
        const start = performance.now();
        m._rayopdf_convert(input, pdf.length, argvPointer, args.length, out);
        m._rayopdf_output_free(out);
        const ms = performance.now() - start;
        if (doc.best_ms === null || ms < doc.best_ms) doc.best_ms = ms;
      }
    }
    m._free(input);
  } catch (error) {
    doc.status = `trap: ${error && error.message ? error.message : String(error)}`;
    doc.pages = 0;
    doc.best_ms = null;
    current = await instance();
  }
  docs.push(doc);
}
console.log(JSON.stringify({ files: files.length, runs, docs }));
