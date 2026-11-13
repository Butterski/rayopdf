// SPDX-License-Identifier: Apache-2.0
// Core tests of the C ABI of include/rayopdf/rayopdf_c.h (src/api/c/, codes 1955-1959).
#include "check.h"
#include "pdf_builder.h"
#include "rayopdf/rayopdf_c.h"

#include <cstring>
#include <string>

namespace {

using namespace RayoPdf;

const std::string& OnePagePdf() {
    static const std::string pdf = Test::OnePage("BT /F1 12 Tf 72 700 Td (Hello from the C ABI) Tj ET");
    return pdf;
}

void TestMarkdown() {
    rayopdf_output out;
    const int status = rayopdf_convert(OnePagePdf().data(), OnePagePdf().size(), nullptr, 0, &out);
    RAYOPDF_CHECK(status == 0 && out.status == 0 && out.pages == 1 && out.text != nullptr &&
                      std::strstr(out.text, "Hello from the C ABI") != nullptr &&
                      std::strlen(out.text) == out.text_size && out.error != nullptr && out.error[0] == '\0' &&
                      out.report != nullptr && out.report[0] == '{' && std::strlen(rayopdf_version()) >= 5,
                  1955);
    rayopdf_output_free(&out);
}

void TestJson() {
    const char* args[] = {"--format=json", "--workers=2"};
    rayopdf_output out;
    const int status = rayopdf_convert(OnePagePdf().data(), OnePagePdf().size(), args, 2, &out);
    RAYOPDF_CHECK(status == 0 && std::strncmp(out.text, "{\"rayopdf\":", 11) == 0 &&
                      std::strstr(out.text, "Hello from the C ABI") != nullptr,
                  1956);
    rayopdf_output_free(&out);
}

void TestUnknownFlag() {
    const char* args[] = {"--plain", "--bogus"};
    rayopdf_output out;
    const int status = rayopdf_convert(OnePagePdf().data(), OnePagePdf().size(), args, 2, &out);
    RAYOPDF_CHECK(status == 2 && out.status == 2 && std::strcmp(out.error, "unknown option: --bogus") == 0 &&
                      out.text != nullptr && out.text_size == 0,
                  1957);
    rayopdf_output_free(&out);
}

void TestCorrupt() {
    const char bytes[] = "not a pdf";
    rayopdf_output out;
    const int status = rayopdf_convert(bytes, sizeof bytes - 1, nullptr, 0, &out);
    RAYOPDF_CHECK(status == 10 && out.status == 10 && out.error[0] != '\0' && out.text_size == 0, 1958);
    rayopdf_output_free(&out);
}

void TestFreeTwice() {
    rayopdf_output out;
    rayopdf_convert(OnePagePdf().data(), OnePagePdf().size(), nullptr, 0, &out);
    rayopdf_output_free(&out);
    RAYOPDF_CHECK(out.text == nullptr && out.report == nullptr && out.error == nullptr && out.text_size == 0 &&
                      out.status == 0 && out.pages == 0,
                  1959);
    rayopdf_output_free(&out);  // a no-op: AddressSanitizer would report a double free
    rayopdf_output_free(nullptr);
}

} // namespace

int main() {
    TestMarkdown();
    TestJson();
    TestUnknownFlag();
    TestCorrupt();
    TestFreeTwice();
    return 0;
}
