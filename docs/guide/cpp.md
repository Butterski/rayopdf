# C and C++

RayoPDF's converter can run inside your program through a C++ API or a C ABI. Both give the
output of `rayopdf --convert`, keep no state between calls, and may run on several threads at
once.

## C++ API

The archives `rayopdf-lib-VERSION-linux-x64.tar.gz` and `rayopdf-lib-VERSION-windows-x64.zip`
(MinGW-w64), attached to each [release](https://github.com/Butterski/rayopdf/releases/latest),
hold the converter as a static library, `lib/librayopdf.a`, with its one header,
`include/rayopdf/rayopdf.h`, and an example, `example.cpp`:

```cpp
#include <rayopdf/rayopdf.h>

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    RayoPdf::Api::Options options;  // Markdown, no images, OCR off
    std::string markdown;
    const RayoPdf::Api::Result result = RayoPdf::Api::ConvertFile(argv[1], options, markdown);
    if (result.status != RayoPdf::Api::Status::Ok) {
        std::fprintf(stderr, "rayopdf %s: %s\n", RayoPdf::Api::Version(), result.error.c_str());
        return static_cast<int>(result.status);
    }
    std::fwrite(markdown.data(), 1, markdown.size(), stdout);
    return 0;
}
```

```sh
g++ -std=c++17 example.cpp -Iinclude lib/librayopdf.a -pthread -o example
./example paper.pdf > paper.md
```

- `ConvertFile(path, options, output)` converts a file (path in UTF-8);
  `ConvertMemory(pdf, options, output)` converts the bytes of a PDF.
- `Options` has the options of `--convert`: `password`, `firstPage` and `lastPage`, `format`
  (`Markdown`, `Json`, `Chunks`, `Inspect`, `Text`), `math`, `ocr` and `ocrAddon`, `annotations`,
  `structure`, `exact`, `pageMarkers`, `frontMatter`, `furniture`, `plain`, `tables`, `quotes`,
  `smallCaps`, `figureText`, `chunkChars`, `jsonDetail` (`Blocks`, `Lines`, `Words`),
  `imageDirectory` and `imageLinkPrefix` (no images when the directory is empty) and `workers`
  (page threads; 0 is automatic). The header documents each one.
- `Result` holds the `status` (the command line's exit code), the `error`, the `pages` converted
  and the `report`: the counters of `--report` as one JSON object
  ([report record](formats.md#the-report-record)).
- `Version()` returns the library's version.

The library throws no exceptions: a failure is reported in `Result::status`.

## C ABI

`include/rayopdf/rayopdf_c.h` is a C99 interface to the same converter, for C programs and other
languages; the Python package and the WebAssembly build use it. It is not in the `rayopdf-lib`
archives. The header is in this repository, and the shared library that exports it
(`librayopdf.so` on Linux, `rayopdf.dll` on Windows) is inside the Python wheels, under
`rayopdf/_lib/`, or is built with the CMake option `RAYOPDF_SHARED`
([CONTRIBUTING.md](../../CONTRIBUTING.md)).

```c
#include <rayopdf/rayopdf_c.h>

rayopdf_output out;
const char* args[] = {"--format=json"};
if (rayopdf_convert(pdf, pdf_size, args, 1, &out) == 0) fwrite(out.text, 1, out.text_size, stdout);
else fprintf(stderr, "%s\n", out.error);
rayopdf_output_free(&out);
```

- `rayopdf_convert(pdf, pdf_size, args, arg_count, &out)` converts the PDF in memory with the
  options of `rayopdf --convert` given as strings (`--format=...`, `--pages=A-B`, `--plain`, …),
  plus `--workers=N` (page threads, default 1) and `--images=DIR` (image files written to DIR and
  linked as `DIR/NAME`; by default no images). It returns the status, which is the command
  line's exit code; an unknown option gives status 2 and the error "unknown option: X".
- `out` receives `status`, `pages`, `text` and `text_size` (the output), `report` (the counters
  as JSON) and `error` (empty on success). Every string is NUL-terminated and allocated with
  `malloc`.
- `rayopdf_output_free(&out)` frees the strings and zeroes `out`; a second call does nothing.
- `rayopdf_version()` returns the version string.
