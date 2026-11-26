// SPDX-License-Identifier: Apache-2.0
// RayoPDF's C ABI: PDF to Markdown (or JSON, chunks, the inspector's HTML) in the calling process,
// for languages that call C: the Python package (ctypes, over librayopdf.so or rayopdf.dll), the
// WebAssembly module and C programs. C99; the functions keep no state between calls and may run on
// several threads at once.
//
//   rayopdf_output out;
//   const char* args[] = {"--format=json"};
//   if (rayopdf_convert(pdf, pdf_size, args, 1, &out) == 0) fwrite(out.text, 1, out.text_size, stdout);
//   else fprintf(stderr, "%s\n", out.error);
//   rayopdf_output_free(&out);
#ifndef RAYOPDF_RAYOPDF_C_H
#define RAYOPDF_RAYOPDF_C_H

#include <stddef.h>

#if defined(_WIN32)
#if defined(RAYOPDF_C_BUILD)
#define RAYOPDF_API __declspec(dllexport)
#else
#define RAYOPDF_API __declspec(dllimport)
#endif
#else
#define RAYOPDF_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// What a conversion gives back. Every string is NUL-terminated and allocated with malloc;
// rayopdf_output_free frees them.
typedef struct rayopdf_output {
    int status;       // the command line's exit code: 0 converted, 2 usage error, 3 unreadable
                      // input, 10 corrupt beyond repair, 11 password required, 12 unwritable
                      // output, 13 unsupported encryption
    int pages;        // pages converted
    char* text;       // the output (Markdown, JSON, JSON Lines or HTML); "" on failure
    size_t text_size; // its bytes, without the NUL
    char* report;     // the --report counters as one JSON object; "" on failure
    char* error;      // why it failed; "" on success
} rayopdf_output;

// Converts the `pdf_size` bytes at `pdf` with the flags `args` (`arg_count` of them): those of
// `rayopdf --convert` (--password=PW, --pages=A-B, --format=..., --math=..., --chunk-chars=N,
// --json-detail=..., --page-markers, --front-matter, --furniture=..., --plain, --tables=...,
// --quotes=..., --small-caps=..., --figure-text=..., --no-exact, --no-annotations,
// --no-structure, --ocr=..., --ocr-addon=PATH), --workers=N (1-64 page threads, default 1; always
// 1 in the WebAssembly module) and --images=DIR (image files written to DIR and linked as
// DIR/NAME; default and --images=none: no images). An unknown flag gives status 2 and "unknown
// option: X". Fills `out` (overwritten, never read) and returns its status.
RAYOPDF_API int rayopdf_convert(const void* pdf, size_t pdf_size, const char* const* args, size_t arg_count,
                                rayopdf_output* out);

// Frees the strings of `out` and zeroes it; a second call (or a null `out`) does nothing.
RAYOPDF_API void rayopdf_output_free(rayopdf_output* out);

// The library's version, "MAJOR.MINOR.PATCH" (static storage).
RAYOPDF_API const char* rayopdf_version(void);

#ifdef __cplusplus
}
#endif

#endif // RAYOPDF_RAYOPDF_C_H
