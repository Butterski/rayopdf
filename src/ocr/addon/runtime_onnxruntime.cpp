// SPDX-License-Identifier: Apache-2.0
// The ONNX Runtime runtime of rayopdf-ocr (ONNX Runtime 1.31.1, MIT, a minimal static build with
// the operators of the PP-OCRv6 models only, by docker/ocr.Dockerfile): ORT-format networks
// (det.ort, rec.ort, optimised at level "extended" when converted), through ORT's C API (no C++
// exceptions), one intra-op and one inter-op thread, sequential execution.
#include "ocr/addon/runtime.h"

#include "common/file.h"

#include "onnxruntime_c_api.h"

namespace RayoPdf::Ocr {

const char* const kRuntimeName = "onnxruntime";
const char* const kRuntimeVersion = "1.31.1";
const char* const kRuntimeFiles[] = {"det.ort", "rec.ort"};
const size_t kRuntimeFileCount = 2;

namespace {

constexpr uint64_t kMaxNetworkBytes = uint64_t(1) << 30;

class OrtRuntime final : public Runtime {
public:
    ~OrtRuntime() override {
        if (!api_) return;
        if (det_) api_->ReleaseSession(det_);
        if (rec_) api_->ReleaseSession(rec_);
        if (memory_) api_->ReleaseMemoryInfo(memory_);
        if (options_) api_->ReleaseSessionOptions(options_);
        if (env_) api_->ReleaseEnv(env_);
    }

    bool Load(const ModelSet& set, Networks networks, std::string& detail) {
        api_ = OrtGetApiBase()->GetApi(ORT_API_VERSION);
        detail = "onnxruntime: API version";
        if (!api_) return false;
        detail = "onnxruntime: environment";
        if (!Ok(api_->CreateEnv(ORT_LOGGING_LEVEL_FATAL, "rayopdf-ocr", &env_))) return false;
        if (!Ok(api_->CreateSessionOptions(&options_)) || !Ok(api_->SetIntraOpNumThreads(options_, 1)) ||
            !Ok(api_->SetInterOpNumThreads(options_, 1)) ||
            !Ok(api_->SetSessionExecutionMode(options_, ORT_SEQUENTIAL)) ||
            !Ok(api_->AddSessionConfigEntry(options_, "session.load_model_format", "ORT")) ||
            !Ok(api_->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_))) {
            return false;
        }
        if (networks == Networks::Classify) {
            detail = set.directory + ": the orientation classifier comes for ncnn only";
            return false;
        }
        return (networks == Networks::RecognizeOnly || LoadSession(set, "det.ort", det_, detail)) &&
               LoadSession(set, "rec.ort", rec_, detail);
    }

    RuntimeStatus Detect(const float* planes, uint32_t width, uint32_t height, std::vector<float>& prob) override {
        const int64_t shape[4] = {1, 3, int64_t(height), int64_t(width)};
        int64_t dims[4] = {0, 0, 0, 0};
        size_t count = 0;
        if (!det_ || !Run(det_, planes, shape, prob, dims, count) || count != 4 || dims[0] != 1 || dims[1] != 1 ||
            dims[2] != int64_t(height) || dims[3] != int64_t(width)) {
            return RuntimeStatus::Failed;
        }
        return RuntimeStatus::Ok;
    }

    RuntimeStatus Recognize(const float* planes, uint32_t width, std::vector<float>& probs, uint32_t& steps,
                            uint32_t& classes) override {
        const int64_t shape[4] = {1, 3, int64_t(kRecHeight), int64_t(width)};
        int64_t dims[4] = {0, 0, 0, 0};
        size_t count = 0;
        if (!Run(rec_, planes, shape, probs, dims, count) || count != 3 || dims[0] != 1 || dims[1] <= 0 ||
            dims[2] <= 0) {
            return RuntimeStatus::Failed;
        }
        steps = uint32_t(dims[1]);
        classes = uint32_t(dims[2]);
        return RuntimeStatus::Ok;
    }

private:
    bool Ok(OrtStatus* status) {
        if (!status) return true;
        api_->ReleaseStatus(status);
        return false;
    }

    bool LoadSession(const ModelSet& set, const char* name, OrtSession*& session, std::string& detail) {
        detail = set.directory + "/" + name;
        File::InputFile file;
        if (file.Open(detail, kMaxNetworkBytes) != File::ReadStatus::Ok) return false;
        const std::string_view bytes = file.Bytes();
        if (!Ok(api_->CreateSessionFromArray(env_, bytes.data(), bytes.size(), options_, &session))) return false;
        detail.clear();
        return true;
    }

    // Runs `session` on one float tensor of `shape`; its one output goes to `out` with its shape.
    bool Run(OrtSession* session, const float* input, const int64_t shape[4], std::vector<float>& out,
             int64_t dims[4], size_t& count) {
        const size_t elements = size_t(shape[0] * shape[1] * shape[2] * shape[3]);
        OrtValue* tensor = nullptr;
        if (!Ok(api_->CreateTensorWithDataAsOrtValue(memory_, const_cast<float*>(input), elements * sizeof(float),
                                                     shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &tensor))) {
            return false;
        }
        const char* inputName = "x";
        const char* outputName = "fetch_name_0";
        OrtValue* result = nullptr;
        const bool ran = Ok(api_->Run(session, nullptr, &inputName, &tensor, 1, &outputName, 1, &result));
        api_->ReleaseValue(tensor);
        if (!ran || !result) return false;
        bool good = false;
        OrtTensorTypeAndShapeInfo* info = nullptr;
        if (Ok(api_->GetTensorTypeAndShape(result, &info))) {
            size_t total = 0;
            ONNXTensorElementDataType type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
            if (Ok(api_->GetDimensionsCount(info, &count)) && count <= 4 && Ok(api_->GetDimensions(info, dims, count)) &&
                Ok(api_->GetTensorShapeElementCount(info, &total)) && Ok(api_->GetTensorElementType(info, &type)) &&
                type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                float* data = nullptr;
                if (Ok(api_->GetTensorMutableData(result, reinterpret_cast<void**>(&data))) && data) {
                    out.assign(data, data + total);
                    good = true;
                }
            }
            api_->ReleaseTensorTypeAndShapeInfo(info);
        }
        api_->ReleaseValue(result);
        return good;
    }

    const OrtApi* api_ = nullptr;
    OrtEnv* env_ = nullptr;
    OrtSessionOptions* options_ = nullptr;
    OrtMemoryInfo* memory_ = nullptr;
    OrtSession* det_ = nullptr;
    OrtSession* rec_ = nullptr;
};

} // namespace

std::unique_ptr<Runtime> CreateRuntime(const ModelSet& set, Networks networks, std::string& detail) {
    std::unique_ptr<OrtRuntime> runtime(new OrtRuntime());
    if (!runtime->Load(set, networks, detail)) return nullptr;
    return runtime;
}

bool RuntimeReads(const ModelSet& set, Networks networks) {
    switch (networks) {
    case Networks::DetectAndRecognize: return !ManifestHash(set, "det.ort").empty() && !ManifestHash(set, "rec.ort").empty();
    case Networks::RecognizeOnly: return !ManifestHash(set, "rec.ort").empty();
    case Networks::Classify: return false;
    }
    return false;
}

} // namespace RayoPdf::Ocr
