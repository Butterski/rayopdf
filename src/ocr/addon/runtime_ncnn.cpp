// SPDX-License-Identifier: Apache-2.0
// The ncnn runtime of rayopdf-ocr (ncnn 20261020, BSD-3-Clause, built by docker/ocr.Dockerfile
// without OpenMP and Vulkan): networks converted by pnnx (det.ncnn.param/.bin, rec.ncnn.param/.bin;
// input blob in0, output out0), one thread, float32 storage and arithmetic throughout (no fp16 or
// bf16 shortcuts), so results match the ONNX models' to float rounding. Winograd and im2col-sgemm
// convolution are off: measured on scanned pages (small tier, detection side 1280) they raise the
// peak RSS from about 660 MB to 960 MB for a 6 % gain in time (docs/development/ocr-engine.md).
#include "ocr/addon/runtime.h"

#include "common/file.h"

#include <cstring>

#include "net.h"

namespace RayoPdf::Ocr {

const char* const kRuntimeName = "ncnn";
const char* const kRuntimeVersion = "20261020";
const char* const kRuntimeFiles[] = {"det.ncnn.param", "det.ncnn.bin", "rec.ncnn.param", "rec.ncnn.bin"};
const size_t kRuntimeFileCount = 4;

namespace {

constexpr uint64_t kMaxNetworkBytes = uint64_t(1) << 30;

class NcnnRuntime final : public Runtime {
public:
    bool Load(const ModelSet& set, std::string& detail) {
        return LoadNet(set, "det", detParam_, detBin_, det_, detail) &&
               LoadNet(set, "rec", recParam_, recBin_, rec_, detail);
    }

    RuntimeStatus Detect(const float* planes, uint32_t width, uint32_t height, std::vector<float>& prob) override {
        ncnn::Mat in(int(width), int(height), 3);
        if (in.empty()) return RuntimeStatus::Failed;
        const size_t plane = size_t(width) * height;
        for (int c = 0; c < 3; ++c) std::memcpy(in.channel(c), planes + plane * c, plane * sizeof(float));
        ncnn::Extractor ex = det_.create_extractor();
        ncnn::Mat out;
        if (ex.input("in0", in) != 0 || ex.extract("out0", out) != 0) return RuntimeStatus::Failed;
        if (out.w != int(width) || out.h != int(height) || out.c != 1 || out.elemsize != sizeof(float)) {
            return RuntimeStatus::Failed;
        }
        const float* data = out.channel(0);
        prob.assign(data, data + plane);
        return RuntimeStatus::Ok;
    }

    RuntimeStatus Recognize(const float* planes, uint32_t width, std::vector<float>& probs, uint32_t& steps,
                            uint32_t& classes) override {
        ncnn::Mat in(int(width), int(kRecHeight), 3);
        if (in.empty()) return RuntimeStatus::Failed;
        const size_t plane = size_t(width) * kRecHeight;
        for (int c = 0; c < 3; ++c) std::memcpy(in.channel(c), planes + plane * c, plane * sizeof(float));
        ncnn::Extractor ex = rec_.create_extractor();
        ncnn::Mat out;
        if (ex.input("in0", in) != 0 || ex.extract("out0", out) != 0) return RuntimeStatus::Failed;
        if (out.dims != 2 || out.w <= 0 || out.h <= 0 || out.elemsize != sizeof(float)) return RuntimeStatus::Failed;
        steps = uint32_t(out.h);
        classes = uint32_t(out.w);
        probs.resize(size_t(steps) * classes);
        for (uint32_t t = 0; t < steps; ++t) {
            const float* row = out.row(int(t));
            std::memcpy(probs.data() + size_t(t) * classes, row, classes * sizeof(float));
        }
        return RuntimeStatus::Ok;
    }

private:
    static bool LoadNet(const ModelSet& set, const char* kind, std::string& param, File::InputFile& bin, ncnn::Net& net,
                        std::string& detail) {
        const std::string base = set.directory + "/" + kind;
        net.opt.num_threads = 1;
        net.opt.use_vulkan_compute = false;
        net.opt.lightmode = true;
        net.opt.use_fp16_packed = false;
        net.opt.use_fp16_storage = false;
        net.opt.use_fp16_arithmetic = false;
        net.opt.use_bf16_storage = false;
        net.opt.use_int8_inference = false;
        net.opt.use_winograd_convolution = false;
        net.opt.use_sgemm_convolution = false;
        File::InputFile text;
        detail = base + ".ncnn.param";
        if (text.Open(detail, kMaxNetworkBytes) != File::ReadStatus::Ok) return false;
        param.assign(text.Bytes());  // ncnn reads the text up to a NUL
        if (net.load_param_mem(param.c_str()) != 0) return false;
        detail = base + ".ncnn.bin";
        if (bin.Open(detail, kMaxNetworkBytes) != File::ReadStatus::Ok) return false;
        const std::string_view bytes = bin.Bytes();
        // The weights are read in place (the mapping lives as long as the runtime).
        const size_t used = net.load_model(reinterpret_cast<const unsigned char*>(bytes.data()));
        if (used == 0 || used > bytes.size()) return false;
        detail.clear();
        return true;
    }

    // The files first: the networks may point into their mappings and go before them.
    std::string detParam_, recParam_;
    File::InputFile detBin_, recBin_;
    ncnn::Net det_, rec_;
};

} // namespace

std::unique_ptr<Runtime> CreateRuntime(const ModelSet& set, std::string& detail) {
    std::unique_ptr<NcnnRuntime> runtime(new NcnnRuntime());
    if (!runtime->Load(set, detail)) return nullptr;
    return runtime;
}

} // namespace RayoPdf::Ocr
