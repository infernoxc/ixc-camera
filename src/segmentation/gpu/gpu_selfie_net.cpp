#include "segmentation/gpu/gpu_selfie_net.h"

#include "common/strings.h"
#include "segmentation/segmentation_engine.h"
#include "segmentation/selfie_net.h"
#include <d3d11.h>  // before the shader blobs (they use BYTE)
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "shaders/SegCSActivate.h"
#include "shaders/SegCSBinary.h"
#include "shaders/SegCSConv.h"
#include "shaders/SegCSDeconv2x.h"
#include "shaders/SegCSDwConv.h"
#include "shaders/SegCSMean.h"
#include "shaders/SegCSResize2x.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <new>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace ixc::seg {

namespace {

struct alignas(16) OpConstants {  // cbuffer OpParams in selfie_net.hlsl
    std::uint32_t in0, in1, outOff, act;
    std::uint32_t h, w, c, k;
    std::uint32_t oh, ow, oc, stride;
    std::int32_t padT, padL;
    std::uint32_t wOff, bOff;
    std::uint32_t total, mode, pad0, pad1;
};
static_assert(sizeof(OpConstants) == 80);

std::string Hr(const char* what, HRESULT hr) {
    char b[96];
    std::snprintf(b, sizeof b, "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    return b;
}

class GpuRunner final : public NetRunner {
public:
    bool Initialize(bool allowSoftware, std::string& error);
    float* Input() override { return input_.data(); }
    const float* Run() override;
    size_t MemoryBytes() const override {
        // Host buffers plus what this runner holds in video memory.
        return (input_.capacity() + output_.capacity()) * sizeof(float) + (arenaFloats_ + weightCount_) * sizeof(float) +
               plan_.size() * sizeof(OpConstants);
    }
    SegBackend Backend() const override { return SegBackend::Gpu; }
    std::string Device() const override { return adapter_; }

private:
    struct Step {
        ID3D11ComputeShader* cs;
        ComPtr<ID3D11Buffer> constants;
        UINT groups;
    };
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11ComputeShader> conv_, dwConv_, deconv_, resize_, mean_, binary_, activate_;
    ComPtr<ID3D11Buffer> arena_, weights_, staging_;
    ComPtr<ID3D11UnorderedAccessView> arenaUav_;
    ComPtr<ID3D11ShaderResourceView> weightsSrv_;
    std::vector<Step> plan_;
    std::vector<float> input_, output_;
    std::string adapter_;
    size_t arenaFloats_ = 0, weightCount_ = 0;
    std::uint32_t inputOffset_ = 0, outputOffset_ = 0;
};

bool GpuRunner::Initialize(bool allowSoftware, std::string& error) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const D3D_DRIVER_TYPE type = D3D_DRIVER_TYPE_HARDWARE;
    // Single-threaded device: created and used on the segmentation worker thread only.
    HRESULT hr = D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_SINGLETHREADED, levels, static_cast<UINT>(std::size(levels)),
                                   D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
    if (FAILED(hr) && allowSoftware) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_SINGLETHREADED, levels,
                               static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
    }
    if (FAILED(hr)) {
        error = "no Direct3D 11 GPU (" + Hr("D3D11CreateDevice", hr) + ")";
        return false;
    }
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIAdapter1> adapter1;
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(device_.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter.As(&adapter1)) &&
        SUCCEEDED(adapter1->GetDesc1(&desc))) {
        adapter_ = WideToUtf8(desc.Description);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && !allowSoftware) {
            error = "only a software renderer is available (" + adapter_ + ")";
            return false;
        }
    }

    struct Blob {
        const BYTE* data;
        size_t size;
        ComPtr<ID3D11ComputeShader>* out;
    } blobs[] = {
        {g_SegCSConv, sizeof(g_SegCSConv), &conv_},           {g_SegCSDwConv, sizeof(g_SegCSDwConv), &dwConv_},
        {g_SegCSDeconv2x, sizeof(g_SegCSDeconv2x), &deconv_}, {g_SegCSResize2x, sizeof(g_SegCSResize2x), &resize_},
        {g_SegCSMean, sizeof(g_SegCSMean), &mean_},           {g_SegCSBinary, sizeof(g_SegCSBinary), &binary_},
        {g_SegCSActivate, sizeof(g_SegCSActivate), &activate_},
    };
    for (auto& b : blobs) {
        hr = device_->CreateComputeShader(b.data, b.size, nullptr, &*b.out);
        if (FAILED(hr)) {
            error = Hr("CreateComputeShader", hr);
            return false;
        }
    }

    const ModelInfo m = Model();
    arenaFloats_ = m.arenaFloats;
    weightCount_ = m.weightCount;
    inputOffset_ = m.inputOffset;
    outputOffset_ = m.outputOffset;
    std::vector<float> weights;
    try {
        if (!DecodeWeights(weights)) throw std::bad_alloc();
        input_.assign(static_cast<size_t>(kNetW) * kNetH * 3, 0.0f);
        output_.assign(static_cast<size_t>(kNetW) * kNetH, 0.0f);
        plan_.reserve(m.opCount);
    } catch (const std::bad_alloc&) {
        error = "out of memory";
        return false;
    }

    auto structured = [&](size_t floats, UINT bind, D3D11_USAGE usage, UINT cpu, const void* init, ComPtr<ID3D11Buffer>& out) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = static_cast<UINT>(floats * sizeof(float));
        d.Usage = usage;
        d.BindFlags = bind;
        d.CPUAccessFlags = cpu;
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        d.StructureByteStride = sizeof(float);
        D3D11_SUBRESOURCE_DATA data{init, 0, 0};
        return device_->CreateBuffer(&d, init ? &data : nullptr, &out);
    };
    hr = structured(arenaFloats_, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, nullptr, arena_);
    if (SUCCEEDED(hr)) hr = structured(weightCount_, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_IMMUTABLE, 0, weights.data(), weights_);
    if (SUCCEEDED(hr)) hr = structured(output_.size(), 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, nullptr, staging_);
    if (SUCCEEDED(hr)) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = DXGI_FORMAT_UNKNOWN;
        u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        u.Buffer.NumElements = static_cast<UINT>(arenaFloats_);
        hr = device_->CreateUnorderedAccessView(arena_.Get(), &u, &arenaUav_);
    }
    if (SUCCEEDED(hr)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = DXGI_FORMAT_UNKNOWN;
        s.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        s.Buffer.NumElements = static_cast<UINT>(weightCount_);
        hr = device_->CreateShaderResourceView(weights_.Get(), &s, &weightsSrv_);
    }
    if (FAILED(hr)) {
        error = "out of video memory (" + Hr("CreateBuffer", hr) + ")";
        return false;
    }

    // One immutable constant buffer per op: a mask is then 94 binds + dispatches, no updates.
    for (size_t i = 0; i < m.opCount; ++i) {
        const Op& op = m.ops[i];
        OpConstants k{};
        k.in0 = static_cast<std::uint32_t>(op.in0);
        k.in1 = op.in1 >= 0 ? static_cast<std::uint32_t>(op.in1) : 0;
        k.outOff = static_cast<std::uint32_t>(op.out);
        k.act = static_cast<std::uint32_t>(op.act);
        k.h = static_cast<std::uint32_t>(op.h);
        k.w = static_cast<std::uint32_t>(op.w);
        k.c = static_cast<std::uint32_t>(op.c);
        k.k = static_cast<std::uint32_t>(op.k);
        k.oh = static_cast<std::uint32_t>(op.oh);
        k.ow = static_cast<std::uint32_t>(op.ow);
        k.oc = static_cast<std::uint32_t>(op.oc);
        k.stride = static_cast<std::uint32_t>(op.stride);
        k.padT = op.padT;
        k.padL = op.padL;
        k.wOff = op.wOff >= 0 ? static_cast<std::uint32_t>(op.wOff) : 0;
        k.bOff = op.bOff >= 0 ? static_cast<std::uint32_t>(op.bOff) : 0;
        k.total = static_cast<std::uint32_t>(op.oh * op.ow * op.oc);
        std::uint32_t threads = k.total;
        ID3D11ComputeShader* cs = nullptr;
        switch (static_cast<OpType>(op.type)) {
            case OpType::Conv: cs = conv_.Get(); break;
            case OpType::DwConv: cs = dwConv_.Get(); break;
            case OpType::Deconv2x: cs = deconv_.Get(); break;
            case OpType::Resize2x: cs = resize_.Get(); break;
            case OpType::Mean:
                cs = mean_.Get();
                threads = k.c;
                break;
            case OpType::Mul: cs = binary_.Get(); k.mode = 0; break;
            case OpType::Add: cs = binary_.Get(); k.mode = 1; break;
            case OpType::Logistic: cs = activate_.Get(); k.act = static_cast<std::uint32_t>(Act::Logistic); break;
            case OpType::Relu: cs = activate_.Get(); k.act = static_cast<std::uint32_t>(Act::Relu); break;
            case OpType::HardSwish: cs = activate_.Get(); k.act = static_cast<std::uint32_t>(Act::HardSwish); break;
        }
        const UINT groups = (threads + 63) / 64;
        if (!cs || groups == 0 || groups > D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION) {
            error = "unexpected model layout";
            return false;
        }
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = sizeof(OpConstants);
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA data{&k, 0, 0};
        Step s{cs, nullptr, groups};
        hr = device_->CreateBuffer(&d, &data, &s.constants);
        if (FAILED(hr)) {
            error = Hr("CreateBuffer", hr);
            return false;
        }
        plan_.push_back(std::move(s));
    }
    return true;
}

const float* GpuRunner::Run() {
    if (!device_) return nullptr;
    const D3D11_BOX in{inputOffset_ * 4u, 0, 0, static_cast<UINT>((inputOffset_ + input_.size()) * 4), 1, 1};
    ctx_->UpdateSubresource(arena_.Get(), 0, &in, input_.data(), 0, 0);
    ID3D11UnorderedAccessView* uav = arenaUav_.Get();
    ID3D11ShaderResourceView* srv = weightsSrv_.Get();
    ctx_->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx_->CSSetShaderResources(0, 1, &srv);
    for (const Step& s : plan_) {
        ID3D11Buffer* cb = s.constants.Get();
        ctx_->CSSetShader(s.cs, nullptr, 0);
        ctx_->CSSetConstantBuffers(0, 1, &cb);
        ctx_->Dispatch(s.groups, 1, 1);
    }
    ID3D11UnorderedAccessView* none = nullptr;
    ctx_->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
    const D3D11_BOX out{outputOffset_ * 4u, 0, 0, static_cast<UINT>((outputOffset_ + output_.size()) * 4), 1, 1};
    ctx_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, arena_.Get(), 0, &out);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &map)) || !map.pData) return nullptr;  // e.g. device removed
    std::memcpy(output_.data(), map.pData, output_.size() * sizeof(float));
    ctx_->Unmap(staging_.Get(), 0);
    if (FAILED(device_->GetDeviceRemovedReason())) return nullptr;
    return output_.data();
}

// Trust but verify: the GPU must produce the CPU network's mask on a test picture.
bool SelfCheck(NetRunner& gpu, std::string& error) {
    SelfieNet cpu;
    if (!cpu.Init()) {
        error = "out of memory";
        return false;
    }
    float* a = gpu.Input();
    float* b = cpu.Input();
    for (int y = 0; y < kNetH; ++y) {
        for (int x = 0; x < kNetW; ++x) {
            // A head-and-shoulders silhouette on a textured background.
            const double dx = x - kNetW / 2.0, dy = (y - kNetH * 0.42) * 1.3;
            const bool person = std::sqrt(dx * dx + dy * dy) < kNetH * 0.28 || (y > kNetH * 0.68 && std::abs(dx) < kNetW * 0.28);
            for (int c = 0; c < 3; ++c) {
                const float v = person ? 0.72f - 0.14f * static_cast<float>(c) : 0.25f + 0.25f * static_cast<float>(((x / 16) + (y / 16)) % 2);
                a[(y * kNetW + x) * 3 + c] = b[(y * kNetW + x) * 3 + c] = v;
            }
        }
    }
    const float* pg = gpu.Run();
    const float* pc = cpu.Run();
    if (!pg || !pc) {
        error = "GPU run failed";
        return false;
    }
    float worst = 0;
    for (int i = 0; i < kNetW * kNetH; ++i) worst = std::max(worst, std::abs(pg[i] - pc[i]));
    if (!(worst < 0.02f)) {
        char note[96];
        std::snprintf(note, sizeof note, "GPU results differ from the CPU (%.3f)", static_cast<double>(worst));
        error = note;
        return false;
    }
    return true;
}

std::unique_ptr<NetRunner> Make(bool allowSoftware, std::string& error) {
    auto r = std::make_unique<GpuRunner>();
    if (!r->Initialize(allowSoftware, error) || !SelfCheck(*r, error)) return nullptr;
    return r;
}

}  // namespace

std::unique_ptr<NetRunner> MakeGpuRunner(std::string& error) { return Make(false, error); }
std::unique_ptr<NetRunner> MakeGpuRunnerAllowWarp(std::string& error) { return Make(true, error); }

}  // namespace ixc::seg
