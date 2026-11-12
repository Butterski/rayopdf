// SPDX-License-Identifier: Apache-2.0
// One kordoc conversion as kordoc's own opendataloader-bench driver makes it (bench/odl-bench.mjs
// at v4.24.66: `parse(buffer, options)`, then `result.markdown`; an empty file when it fails).
//
//   node run.mjs IN.pdf OUT.md off|default
//
// `off` passes { ocr: false }; `default` passes no options, so kordoc reads pages without a
// text layer and large images with its OCR when the models are in KORDOC_MODEL_CACHE.
import { readFile, writeFile } from "node:fs/promises"
import { parse } from "/opt/kordoc/node_modules/kordoc/dist/index.js"

const [input, output, mode] = process.argv.slice(2)
if (!input || !output || (mode !== "off" && mode !== "default")) {
  process.stderr.write("usage: node run.mjs IN.pdf OUT.md off|default\n")
  process.exit(2)
}
const result = await parse(await readFile(input), mode === "off" ? { ocr: false } : undefined)
await writeFile(output, result.success ? (result.markdown ?? "") : "")
if (!result.success) {
  process.stderr.write(`kordoc: ${result.code ?? ""} ${result.error ?? "failed"}\n`)
  process.exit(1)
}
