// SPDX-License-Identifier: Apache-2.0
// Smoke test of rayopdf.dll, as a program written in C loads it: LoadLibrary, GetProcAddress, one
// conversion of a one-page PDF, status 0 and its words. The windows config builds it with the DLL
// and runs it under Wine (ctest c_api_smoke).
#include "rayopdf/rayopdf_c.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

typedef int (*ConvertFunction)(const void*, size_t, const char* const*, size_t, rayopdf_output*);
typedef void (*FreeFunction)(rayopdf_output*);
typedef const char* (*VersionFunction)(void);

static const char kPdf[] =
    "%PDF-1.4\n"
    "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
    "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
    "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R"
    " /Resources << /Font << /F1 5 0 R >> >> >>\nendobj\n"
    "4 0 obj\n<< /Length 43 >>\nstream\nBT /F1 12 Tf 72 700 Td (Hello from C) Tj ET\nendstream\nendobj\n"
    "5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n"
    "trailer\n<< /Root 1 0 R >>\n%%EOF\n";

int main(void) {
    HMODULE library = LoadLibraryA("rayopdf.dll");
    if (library == NULL) {
        fprintf(stderr, "c_api_smoke: cannot load rayopdf.dll\n");
        return 1;
    }
    ConvertFunction convert = (ConvertFunction)(void (*)(void))GetProcAddress(library, "rayopdf_convert");
    FreeFunction release = (FreeFunction)(void (*)(void))GetProcAddress(library, "rayopdf_output_free");
    VersionFunction version = (VersionFunction)(void (*)(void))GetProcAddress(library, "rayopdf_version");
    if (convert == NULL || release == NULL || version == NULL) {
        fprintf(stderr, "c_api_smoke: rayopdf.dll lacks a function of the C ABI\n");
        return 1;
    }
    const char* args[] = {"--format=markdown"};
    rayopdf_output out;
    const int status = convert(kPdf, sizeof kPdf - 1, args, 1, &out);
    const int ok = status == 0 && out.text != NULL && strstr(out.text, "Hello from C") != NULL;
    if (!ok) fprintf(stderr, "c_api_smoke: status %d: %s\n", status, out.error != NULL ? out.error : "");
    release(&out);
    printf("c_api_smoke: rayopdf %s, status %d\n", version(), status);
    FreeLibrary(library);
    return ok ? 0 : 1;
}
