// SPDX-License-Identifier: Apache-2.0
// RayoPDF's C++ API in a program of its own: converts the PDF named on the command line and
// prints its Markdown. Built against the library archive:
//
//   g++ -std=c++17 example.cpp -Iinclude lib/librayopdf.a -pthread -o example
//   ./example document.pdf
#include <rayopdf/rayopdf.h>

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s IN.pdf\n", argv[0]);
        return 2;
    }
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
