// Differential GPU fixture for the actual provider Densify/reset code.
// The runner extracts both candidate code and the unchanged baseline shader.
#include <windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <winrt/base.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "AmdConfidenceShadersUnderTest.h"

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void Check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        char message[256]{};
        std::snprintf(message, sizeof(message), "%s: HRESULT 0x%08lx", operation,
            static_cast<unsigned long>(result));
        throw std::runtime_error(message);
    }
}

// This observes API calls while forwarding all work to the real D3D11 device.
struct ObservedContext {
    ID3D11DeviceContext* raw = nullptr;
    ID3D11UnorderedAccessView* confidence = nullptr;
    uint32_t confidenceClears = 0;
#define FORWARD_CONTEXT(name) template<class... Args> decltype(auto) name(Args&&... args) { \
    return raw->name(std::forward<Args>(args)...); }
    FORWARD_CONTEXT(Map)
    FORWARD_CONTEXT(Unmap)
    FORWARD_CONTEXT(CSSetShader)
    FORWARD_CONTEXT(CSSetShaderResources)
    FORWARD_CONTEXT(CSSetConstantBuffers)
    FORWARD_CONTEXT(Dispatch)
#undef FORWARD_CONTEXT
    void CSSetUnorderedAccessViews(UINT start, size_t count, ID3D11UnorderedAccessView* const* views, const UINT* counts) {
        raw->CSSetUnorderedAccessViews(start, static_cast<UINT>(count), views, counts);
    }
    void ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView* view, const FLOAT color[4]) {
        if (view == confidence) ++confidenceClears;
        raw->ClearUnorderedAccessViewFloat(view, color);
    }
};

struct Extent { uint32_t width = 0, height = 0; };
enum class FrameGuidanceResetReason { ProviderFailure };

struct Resources {
    ID3D11Device* device = nullptr;
    ObservedContext observed;
    ObservedContext* context11 = &observed;
    Extent extent, opticalFlowExtent, sparseExtent;
    winrt::com_ptr<ID3D11Texture2D> sparse, motion, confidence;
    winrt::com_ptr<ID3D11ShaderResourceView> sparseFlowSrv;
    winrt::com_ptr<ID3D11UnorderedAccessView> denseMotionUav, denseConfidenceUav;
    winrt::com_ptr<ID3D11Buffer> densifyParams;
    winrt::com_ptr<ID3D11ComputeShader> densifyShader;
    bool historyValid = false;
    FrameGuidanceResetReason resetReason = FrameGuidanceResetReason::ProviderFailure;

    Resources(ID3D11Device* inputDevice, ID3D11DeviceContext* context) : device(inputDevice) { observed.raw = context; }

    winrt::com_ptr<ID3D11Texture2D> Texture(Extent size, DXGI_FORMAT format, UINT binds) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = size.width; desc.Height = size.height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = format; desc.SampleDesc.Count = 1;
        desc.BindFlags = binds;
        winrt::com_ptr<ID3D11Texture2D> result;
        Check(device->CreateTexture2D(&desc, nullptr, result.put()), "Create fixture texture");
        return result;
    }

    void Recreate(Extent size, bool performance, const char* shader) {
        observed.raw->ClearState();
        sparseFlowSrv = nullptr; denseMotionUav = nullptr; denseConfidenceUav = nullptr;
        sparse = nullptr; motion = nullptr; confidence = nullptr;
        densifyParams = nullptr; densifyShader = nullptr;
        extent = size;
        opticalFlowExtent = performance ? Extent{(size.width + 1) / 2, (size.height + 1) / 2} : size;
        sparseExtent = {(opticalFlowExtent.width + 7) / 8, (opticalFlowExtent.height + 7) / 8};
        sparse = Texture(sparseExtent, DXGI_FORMAT_R16G16_SINT, D3D11_BIND_SHADER_RESOURCE);
        motion = Texture(extent, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
        confidence = Texture(extent, DXGI_FORMAT_R8_UNORM, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE);
        Check(device->CreateShaderResourceView(sparse.get(), nullptr, sparseFlowSrv.put()), "Sparse flow SRV");
        Check(device->CreateUnorderedAccessView(motion.get(), nullptr, denseMotionUav.put()), "Dense motion UAV");
        Check(device->CreateUnorderedAccessView(confidence.get(), nullptr, denseConfidenceUav.put()), "Confidence UAV");
        observed.confidence = denseConfidenceUav.get();
        observed.confidenceClears = 0;
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = 32; buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER; buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Check(device->CreateBuffer(&buffer, nullptr, densifyParams.put()), "Densify constants");
        winrt::com_ptr<ID3DBlob> blob, errors;
        const auto result = D3DCompile(shader, std::strlen(shader), "production-amd-densify", nullptr,
            nullptr, "Densify", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, blob.put(), errors.put());
        if (errors) std::fputs(static_cast<const char*>(errors->GetBufferPointer()), stderr);
        Check(result, "Compile production Densify");
        Check(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
            densifyShader.put()), "Create production Densify shader");
    }

    void Input(uint32_t frame) {
        std::vector<int16_t> vectors(size_t(sparseExtent.width) * sparseExtent.height * 2);
        for (uint32_t y = 0; y < sparseExtent.height; ++y) {
            for (uint32_t x = 0; x < sparseExtent.width; ++x) {
                const auto index = (size_t(y) * sparseExtent.width + x) * 2;
                vectors[index] = static_cast<int16_t>((int(x * 7 + frame * 3) % 23) - 11);
                vectors[index + 1] = static_cast<int16_t>((int(y * 5 + frame * 7) % 19) - 9);
            }
        }
        observed.raw->UpdateSubresource(sparse.get(), 0, nullptr, vectors.data(), sparseExtent.width * 4, 0);
    }

    std::vector<uint8_t> Read(ID3D11Texture2D* texture, uint32_t bytesPerPixel) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.BindFlags = desc.MiscFlags = 0;
        winrt::com_ptr<ID3D11Texture2D> staging;
        Check(device->CreateTexture2D(&desc, nullptr, staging.put()), "Create staging texture");
        observed.raw->CopyResource(staging.get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(observed.raw->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped), "Read actual GPU pixels");
        std::vector<uint8_t> result(size_t(desc.Width) * desc.Height * bytesPerPixel);
        for (uint32_t y = 0; y < desc.Height; ++y)
            std::memcpy(result.data() + size_t(y) * desc.Width * bytesPerPixel,
                static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                size_t(desc.Width) * bytesPerPixel);
        observed.raw->Unmap(staging.get(), 0);
        return result;
    }
};

struct Candidate : Resources {
    using Resources::Resources;
#include "AmdConfidenceStateUnderTest.inl"
#include "AmdConfidenceDensifyUnderTest.inl"
    void ResetAfterResourceDestroy() {
#include "AmdConfidenceDestroyUnderTest.inl"
    }
    bool Frame(bool resetFrame) {
        auto& impl = *this;
#include "AmdConfidenceResetUnderTest.inl"
        return true;
    }
};

struct Reference : Resources {
    using Resources::Resources;
#include "AmdConfidenceReferenceDensify.inl"
    bool Frame(bool resetFrame) {
        auto& impl = *this;
#include "AmdConfidenceReferenceReset.inl"
        return true;
    }
};

static void CheckDebugErrors(ID3D11Device* device) {
    winrt::com_ptr<ID3D11InfoQueue> info;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(info.put())))) return;
    for (UINT64 index = 0; index < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index) {
        SIZE_T bytes = 0;
        Check(info->GetMessage(index, nullptr, &bytes), "Debug message length");
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        Check(info->GetMessage(index, message, &bytes), "Debug message");
        if (message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION || message->Severity == D3D11_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(std::string("D3D11 debug error: ") + message->pDescription);
    }
}

static void Run(D3D_DRIVER_TYPE driver) {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    auto result = D3D11CreateDevice(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_DEBUG,
        nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put());
    bool debug = SUCCEEDED(result);
    if (result == DXGI_ERROR_SDK_COMPONENT_MISSING)
        result = D3D11CreateDevice(nullptr, driver, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
            device.put(), nullptr, context.put());
    Check(result, "Create test D3D11 device");
    Candidate candidate(device.get(), context.get());
    Reference reference(device.get(), context.get());
    uint32_t frames = 0;
    uint64_t pixels = 0;
    // The same provider state is reused through three real texture rebuilds.
    for (const auto size : {Extent{128, 128}, Extent{129, 131}, Extent{257, 193}}) {
        for (bool performance : {true, false}) {
            candidate.Recreate(size, performance, AMD_CONFIDENCE_CANDIDATE_SHADER);
            candidate.ResetAfterResourceDestroy();
            reference.Recreate(size, performance, AMD_CONFIDENCE_REFERENCE_SHADER);
            uint32_t expectedClears = 0;
            bool wasValid = false;
            for (bool reset : {true, true, false, false, false, false, false, true, false, false,
                               true, true, false, false, false, false, false}) {
                candidate.Input(frames + 1);
                reference.Input(frames + 1);
                Require(candidate.Frame(reset) && reference.Frame(reset), "Extracted provider output processing failed");
                const auto actualConfidence = candidate.Read(candidate.confidence.get(), 1);
                const auto oldConfidence = reference.Read(reference.confidence.get(), 1);
                Require(actualConfidence == oldConfidence, "R8 confidence differs from original per-pixel shader");
                Require(!actualConfidence.empty(), "Confidence surface is empty");
                const uint8_t expected = reset ? 0u : 166u; // R8_UNORM of the original shader's 0.65f.
                for (auto pixel : actualConfidence) Require(pixel == expected, "Reset/valid confidence quantization changed");
                Require(candidate.Read(candidate.motion.get(), 4) == reference.Read(reference.motion.get(), 4),
                    "Removing confidence stores changed motion pixels");
                if (reset || !wasValid) ++expectedClears;
                wasValid = !reset;
                Require(candidate.observed.confidenceClears == expectedClears,
                    "Confidence must clear on reset and first valid only, never on consecutive valid frames");
                ++frames;
                pixels += uint64_t(size.width) * size.height;
            }
            std::printf("PASS %s %ux%u %s reset/valid/recovery confidence, motion bytes, clears=%u\n",
                driver == D3D_DRIVER_TYPE_HARDWARE ? "hardware" : "WARP", size.width, size.height,
                performance ? "Performance-50%" : "Quality-100%", expectedClears);
        }
    }
    context->ClearState();
    context->Flush();
    Check(device->GetDeviceRemovedReason(), "D3D11 device health");
    CheckDebugErrors(device.get());
    std::printf("AMDOF confidence %s: %u frames, %llu pixel comparisons, 6 texture generations, debug=%u; no performance claim.\n",
        driver == D3D_DRIVER_TYPE_HARDWARE ? "hardware" : "WARP", frames,
        static_cast<unsigned long long>(pixels), debug ? 1u : 0u);
}

int main() try {
    Run(D3D_DRIVER_TYPE_WARP);
    Run(D3D_DRIVER_TYPE_HARDWARE);
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "AMDOF confidence FAILED: %s\n", error.what());
    return 1;
}
