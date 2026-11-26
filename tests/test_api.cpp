// SPDX-License-Identifier: Apache-2.0
// Core tests of src/api, the C++ API of include/rayopdf/rayopdf.h (codes 1950-1999).
#include "check.h"
#include "pdf_builder.h"
#include "rayopdf/rayopdf.h"

#include <string>

namespace {

using namespace RayoPdf;

const std::string& OnePagePdf() {
    static const std::string pdf = Test::OnePage("BT /F1 12 Tf 72 700 Td (Hello from the API) Tj ET");
    return pdf;
}

void TestMarkdown() {
    std::string out;
    const Api::Result result = Api::ConvertMemory(OnePagePdf(), Api::Options(), out);
    RAYOPDF_CHECK(result.status == Api::Status::Ok && result.pages == 1 &&
                      out.find("Hello from the API") != std::string::npos,
                  1950);
}

void TestJson() {
    Api::Options options;
    options.format = Api::Format::Json;
    std::string out;
    const Api::Result result = Api::ConvertMemory(OnePagePdf(), options, out);
    RAYOPDF_CHECK(result.status == Api::Status::Ok && out.compare(0, 12, "{\"rayopdf\":\"") == 0 &&
                      out.find("Hello from the API") != std::string::npos,
                  1951);
    // The JSON output's lines and words (an output format's check: code from 2650-2699).
    options.jsonDetail = Api::JsonDetail::Words;
    Api::ConvertMemory(OnePagePdf(), options, out);
    RAYOPDF_CHECK(out.find("\"lines\":[{\"page\":1,\"box\":[") != std::string::npos &&
                      out.find("\"text\":\"Hello\"") != std::string::npos,
                  2655);
    // Plain text (an output format's check: code from 2650-2699).
    options = Api::Options();
    options.format = Api::Format::Text;
    Api::ConvertMemory(OnePagePdf(), options, out);
    RAYOPDF_CHECK(out == "Hello from the API\n", 2673);
}

void TestFailures() {
    std::string out = "left over";
    Api::Result result = Api::ConvertMemory("not a pdf", Api::Options(), out);
    RAYOPDF_CHECK(result.status == Api::Status::Corrupt && !result.error.empty() && out.empty(), 1952);
    result = Api::ConvertFile("no-such-folder/missing.pdf", Api::Options(), out);
    RAYOPDF_CHECK(result.status == Api::Status::Unreadable && !result.error.empty() && out.empty(), 1953);
}

void TestReport() {
    std::string out;
    const Api::Result result = Api::ConvertMemory(OnePagePdf(), Api::Options(), out);
    RAYOPDF_CHECK(result.report.compare(0, 22, "{\"pages_without_text\":") == 0 && result.report.back() == '}' &&
                      result.report.find("\"glyphs\":") != std::string::npos,
                  1954);
}

} // namespace

int main() {
    TestMarkdown();
    TestJson();
    TestFailures();
    TestReport();
    return 0;
}
