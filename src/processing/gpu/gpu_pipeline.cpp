#include "processing/gpu/gpu_pipeline.h"

#include "shaders/CSColorUV.h"
#include "shaders/CSColorY.h"
#include "shaders/CSScaleUV.h"
#include "shaders/CSScaleY.h"
#include "shaders/CSSharpen.h"

#include <dxgi1_2.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace ixc::processing {

namespace {

struct alignas(16) ConstantData {
    std::uint32_t width, height, mirror;
    std::int32_t amount, threshold, halo;
    std::uint32_t pad[2];
};
static_assert(sizeof(ConstantData) == 32);

double Ms(LARGE_INTEGER a, LARGE_INTEGER b) {
    static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart); }();
    return 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / freq;
}

HRESULT MakeTexture(ID3D11Device* dev, int w, int h, DXGI_FORMAT fmt, D3D11_USAGE usage, UINT bind, UINT cpu, ComPtr<ID3D11Texture2D>& out) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(w);
    d.Height = static_cast<UINT>(h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = usage;
    d.BindFlags = bind;
    d.CPUAccessFlags = cpu;
    return dev->CreateTexture2D(&d, nullptr, &out);
}

HRESULT MakeTypedBuffer(ID3D11Device* dev, UINT elements, DXGI_FORMAT fmt, const void* init, ComPtr<ID3D11Buffer>& buf,
                        ComPtr<ID3D11ShaderResourceView>& srv) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = elements * 4;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{init, 0, 0};
    HRESULT hr = dev->CreateBuffer(&d, init ? &data : nullptr, &buf);
    if (FAILED(hr)) return hr;
    D3D11_SHADER_RESOURCE_VIEW_DESC s{};
    s.Format = fmt;
    s.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    s.Buffer.NumElements = elements;
    return dev->CreateShaderResourceView(buf.Get(), &s, &srv);
}

}  // namespace

HRESULT GpuNv12Processor::Initialize(const Options& o) {
    Release();
    ComPtr<IDXGIAdapter1> adapter;
    if (!o.useWarp && o.adapterIndex >= 0) {
        ComPtr<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) hr = factory->EnumAdapters1(static_cast<UINT>(o.adapterIndex), &adapter);
        if (FAILED(hr)) return hr;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const D3D_DRIVER_TYPE type = o.useWarp ? D3D_DRIVER_TYPE_WARP : (adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE);
    // Single-threaded device: all calls come from one thread per processor (cheaper, no locks).
    HRESULT hr = D3D11CreateDevice(adapter.Get(), type, nullptr, D3D11_CREATE_DEVICE_SINGLETHREADED, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
    if (FAILED(hr)) {
        Release();
        return hr;
    }

    // Typed UAV stores for the plane formats are required by the kernels.
    for (DXGI_FORMAT f : {DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8G8_UINT}) {
        UINT support = 0;
        if (FAILED(device_->CheckFormatSupport(f, &support)) || !(support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)) {
            Release();
            return DXGI_ERROR_UNSUPPORTED;
        }
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> used;
    ComPtr<IDXGIAdapter1> used1;
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(device_.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&used)) && SUCCEEDED(used.As(&used1)) &&
        SUCCEEDED(used1->GetDesc1(&desc))) {
        adapterName_ = desc.Description;
        // Without a real GPU driver, "hardware" can mean the Microsoft Basic Render Driver: a
        // CPU rasterizer that would only add CPU and RAM cost. Never use it outside tests.
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && !o.useWarp) {
            Release();
            return DXGI_ERROR_UNSUPPORTED;
        }
    }

    struct Blob {
        const BYTE* data;
        size_t size;
        ComPtr<ID3D11ComputeShader>* out;
    } blobs[] = {
        {g_CSColorY, sizeof(g_CSColorY), &csColorY_},   {g_CSColorUV, sizeof(g_CSColorUV), &csColorUV_},
        {g_CSScaleY, sizeof(g_CSScaleY), &csScaleY_},   {g_CSScaleUV, sizeof(g_CSScaleUV), &csScaleUV_},
        {g_CSSharpen, sizeof(g_CSSharpen), &csSharpen_},
    };
    for (auto& b : blobs) {
        hr = device_->CreateComputeShader(b.data, b.size, nullptr, &*b.out);
        if (FAILED(hr)) {
            Release();
            return hr;
        }
    }

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(ConstantData);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = device_->CreateBuffer(&cb, nullptr, &constants_);
    if (SUCCEEDED(hr)) hr = MakeTypedBuffer(device_.Get(), 768, DXGI_FORMAT_R32_UINT, nullptr, lut_, lutSrv_);

    D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    if (SUCCEEDED(hr)) hr = device_->CreateQuery(&q, &disjoint_);
    q.Query = D3D11_QUERY_TIMESTAMP;
    if (SUCCEEDED(hr)) hr = device_->CreateQuery(&q, &tsBegin_);
    if (SUCCEEDED(hr)) hr = device_->CreateQuery(&q, &tsEnd_);
    if (FAILED(hr)) Release();
    return hr;
}

void GpuNv12Processor::Release() {
    if (ctx_) {
        ctx_->ClearState();
        ctx_->Flush();
    }
    *this = GpuNv12Processor{};
}

HRESULT GpuNv12Processor::EnsureResources(int w, int h) {
    if (w == width_ && h == height_ && srcY_) return S_OK;
    width_ = height_ = 0;
    ID3D11Device* d = device_.Get();
    const UINT srv = D3D11_BIND_SHADER_RESOURCE, uav = D3D11_BIND_UNORDERED_ACCESS;
    HRESULT hr = MakeTexture(d, w, h, DXGI_FORMAT_R8_UINT, D3D11_USAGE_DEFAULT, srv, 0, srcY_);
    if (SUCCEEDED(hr)) hr = MakeTexture(d, w / 2, h / 2, DXGI_FORMAT_R8G8_UINT, D3D11_USAGE_DEFAULT, srv, 0, srcUV_);
    if (SUCCEEDED(hr)) hr = MakeTexture(d, w, h, DXGI_FORMAT_R8_UINT, D3D11_USAGE_DEFAULT, srv | uav, 0, midY_);
    if (SUCCEEDED(hr)) hr = MakeTexture(d, w, h, DXGI_FORMAT_R8_UINT, D3D11_USAGE_DEFAULT, uav, 0, dstY_);
    if (SUCCEEDED(hr)) hr = MakeTexture(d, w / 2, h / 2, DXGI_FORMAT_R8G8_UINT, D3D11_USAGE_DEFAULT, uav, 0, dstUV_);
    // Explicit upload textures (CPU write → CopyResource). UpdateSubresource would let the driver
    // grow hidden upload buffers: measured +155 MB after 300 frames, not returned on release.
    if (SUCCEEDED(hr)) hr = MakeTexture(d, w, h, DXGI_FORMAT_R8_UINT, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_WRITE, upY_);
    if (SUCCEEDED(hr)) hr = MakeTexture(d, w / 2, h / 2, DXGI_FORMAT_R8G8_UINT, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_WRITE, upUV_);
    if (SUCCEEDED(hr)) hr = MakeTexture(d, w, h, DXGI_FORMAT_R8_UINT, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, stageY_);
    if (SUCCEEDED(hr)) hr = MakeTexture(d, w / 2, h / 2, DXGI_FORMAT_R8G8_UINT, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, stageUV_);
    if (SUCCEEDED(hr)) hr = d->CreateShaderResourceView(srcY_.Get(), nullptr, &srcYSrv_);
    if (SUCCEEDED(hr)) hr = d->CreateShaderResourceView(srcUV_.Get(), nullptr, &srcUVSrv_);
    if (SUCCEEDED(hr)) hr = d->CreateShaderResourceView(midY_.Get(), nullptr, &midYSrv_);
    if (SUCCEEDED(hr)) hr = d->CreateUnorderedAccessView(midY_.Get(), nullptr, &midYUav_);
    if (SUCCEEDED(hr)) hr = d->CreateUnorderedAccessView(dstY_.Get(), nullptr, &dstYUav_);
    if (SUCCEEDED(hr)) hr = d->CreateUnorderedAccessView(dstUV_.Get(), nullptr, &dstUVUav_);
    if (FAILED(hr)) return hr;
    width_ = w;
    height_ = h;
    std::fill(std::begin(geoKey_), std::end(geoKey_), -1.0);  // tables must be rebuilt for the new size
    return S_OK;
}

void GpuNv12Processor::UploadLut(const PipelineParams& p) {
    std::array<std::uint8_t, 768> lut;
    std::copy(p.yLut.begin(), p.yLut.end(), lut.begin());
    std::copy(p.uLut.begin(), p.uLut.end(), lut.begin() + 256);
    std::copy(p.vLut.begin(), p.vLut.end(), lut.begin() + 512);
    if (lutValid_ && lut == lutCache_) return;
    std::array<std::uint32_t, 768> wide;
    for (size_t i = 0; i < 768; ++i) wide[i] = lut[i];
    ctx_->UpdateSubresource(lut_.Get(), 0, nullptr, wide.data(), 0, 0);
    lutCache_ = lut;
    lutValid_ = true;
}

void GpuNv12Processor::UploadGeometry(int w, int h, const PipelineParams& p) {
    const double key[5] = {p.srcX, p.srcY, p.srcW, p.srcH, p.mirror ? 1.0 : 0.0};
    if (std::equal(std::begin(key), std::end(key), std::begin(geoKey_))) return;
    BuildGeometryTables(w, h, p, tables_);  // the exact tables the CPU uses
    auto make = [&](const std::vector<std::int32_t>& t, ComPtr<ID3D11Buffer>& b, ComPtr<ID3D11ShaderResourceView>& s) {
        b.Reset();
        s.Reset();
        MakeTypedBuffer(device_.Get(), static_cast<UINT>(t.size()), DXGI_FORMAT_R32_SINT, t.data(), b, s);
    };
    make(tables_.yX, yX_, yXSrv_);
    make(tables_.yY, yY_, yYSrv_);
    make(tables_.uvX, uvX_, uvXSrv_);
    make(tables_.uvY, uvY_, uvYSrv_);
    std::copy(std::begin(key), std::end(key), std::begin(geoKey_));
}

bool GpuNv12Processor::Process(const Nv12Planes& src, const Nv12Frame& dst, const PipelineParams& p, Timing* timing) {
    if (!device_ || !src.y || !src.uv || !dst.y || !dst.uv) return false;
    if (src.width != dst.width || src.height != dst.height || src.width < 2 || src.height < 2) return false;
    if ((src.width & 1) || (src.height & 1)) return false;
    if (src.yStride < src.width || src.uvStride < src.width || dst.yStride < dst.width || dst.uvStride < dst.width) return false;
    const int w = src.width, h = src.height;

    LARGE_INTEGER t0, t1, t2, t3;
    QueryPerformanceCounter(&t0);
    if (FAILED(EnsureResources(w, h))) return false;

    // Upload the frame through the fixed upload textures (bounded memory), then any changed
    // parameters (tiny, and only when they change).
    auto upload = [&](ID3D11Texture2D* staging, ID3D11Texture2D* target, const std::uint8_t* data, int stride, int rows) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx_->Map(staging, 0, D3D11_MAP_WRITE, 0, &m))) return false;
        for (int y = 0; y < rows; ++y) {
            std::memcpy(static_cast<std::uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, data + static_cast<std::ptrdiff_t>(y) * stride,
                        static_cast<size_t>(w));
        }
        ctx_->Unmap(staging, 0);
        ctx_->CopyResource(target, staging);
        return true;
    };
    if (!upload(upY_.Get(), srcY_.Get(), src.y, src.yStride, h) || !upload(upUV_.Get(), srcUV_.Get(), src.uv, src.uvStride, h / 2)) return false;
    UploadLut(p);
    if (!p.geometryIdentity) UploadGeometry(w, h, p);
    const ConstantData cd{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), p.mirror ? 1u : 0u,
                          p.sharpenAmount, p.sharpenThreshold, p.sharpenHaloLimit, {0, 0}};
    ctx_->UpdateSubresource(constants_.Get(), 0, nullptr, &cd, 0, 0);
    QueryPerformanceCounter(&t1);

    if (timing) {
        ctx_->Begin(disjoint_.Get());
        ctx_->End(tsBegin_.Get());
    }
    ID3D11Buffer* cbs[] = {constants_.Get()};
    ctx_->CSSetConstantBuffers(0, 1, cbs);
    const bool sharpen = p.sharpenAmount > 0;
    const UINT gx = (static_cast<UINT>(w) + 15) / 16, gy = (static_cast<UINT>(h) + 15) / 16;
    const UINT cgx = (static_cast<UINT>(w / 2) + 15) / 16, cgy = (static_cast<UINT>(h / 2) + 15) / 16;
    ID3D11UnorderedAccessView* nullUav[2] = {nullptr, nullptr};
    ID3D11ShaderResourceView* nullSrv[7] = {};

    // Pass 1: colour (or scale) into the luma target (the intermediate when sharpening) + chroma.
    ID3D11ShaderResourceView* srvs[7] = {lutSrv_.Get(), srcYSrv_.Get(), srcUVSrv_.Get(), yXSrv_.Get(), yYSrv_.Get(), uvXSrv_.Get(), uvYSrv_.Get()};
    ctx_->CSSetShaderResources(0, p.geometryIdentity ? 3 : 7, srvs);
    ID3D11UnorderedAccessView* uavs[2] = {sharpen ? midYUav_.Get() : dstYUav_.Get(), dstUVUav_.Get()};
    ctx_->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    ctx_->CSSetShader(p.geometryIdentity ? csColorY_.Get() : csScaleY_.Get(), nullptr, 0);
    ctx_->Dispatch(gx, gy, 1);
    ctx_->CSSetShader(p.geometryIdentity ? csColorUV_.Get() : csScaleUV_.Get(), nullptr, 0);
    ctx_->Dispatch(cgx, cgy, 1);

    // Pass 2: sharpen the intermediate luma into the final plane.
    if (sharpen) {
        ctx_->CSSetUnorderedAccessViews(0, 2, nullUav, nullptr);
        ID3D11ShaderResourceView* s2[2] = {lutSrv_.Get(), midYSrv_.Get()};
        ctx_->CSSetShaderResources(0, 2, s2);
        ID3D11UnorderedAccessView* u2[1] = {dstYUav_.Get()};
        ctx_->CSSetUnorderedAccessViews(0, 1, u2, nullptr);
        ctx_->CSSetShader(csSharpen_.Get(), nullptr, 0);
        ctx_->Dispatch(gx, gy, 1);
    }
    ctx_->CSSetUnorderedAccessViews(0, 2, nullUav, nullptr);
    ctx_->CSSetShaderResources(0, 7, nullSrv);
    if (timing) {
        ctx_->End(tsEnd_.Get());
        ctx_->End(disjoint_.Get());
    }

    // Read back into the caller's frame.
    ctx_->CopyResource(stageY_.Get(), dstY_.Get());
    ctx_->CopyResource(stageUV_.Get(), dstUV_.Get());
    QueryPerformanceCounter(&t2);
    D3D11_MAPPED_SUBRESOURCE my{}, muv{};
    if (FAILED(ctx_->Map(stageY_.Get(), 0, D3D11_MAP_READ, 0, &my))) return false;
    for (int y = 0; y < h; ++y) {
        std::memcpy(dst.y + static_cast<std::ptrdiff_t>(y) * dst.yStride, static_cast<const std::uint8_t*>(my.pData) + static_cast<size_t>(y) * my.RowPitch,
                    static_cast<size_t>(w));
    }
    ctx_->Unmap(stageY_.Get(), 0);
    if (FAILED(ctx_->Map(stageUV_.Get(), 0, D3D11_MAP_READ, 0, &muv))) return false;
    for (int y = 0; y < h / 2; ++y) {
        std::memcpy(dst.uv + static_cast<std::ptrdiff_t>(y) * dst.uvStride, static_cast<const std::uint8_t*>(muv.pData) + static_cast<size_t>(y) * muv.RowPitch,
                    static_cast<size_t>(w));
    }
    ctx_->Unmap(stageUV_.Get(), 0);
    QueryPerformanceCounter(&t3);

    if (timing) {
        timing->uploadMs = Ms(t0, t1);
        timing->readbackMs = Ms(t2, t3);
        timing->totalMs = Ms(t0, t3);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 b = 0, e = 0;
        while (ctx_->GetData(disjoint_.Get(), &dj, sizeof(dj), 0) == S_FALSE) {}  // already complete after Map
        ctx_->GetData(tsBegin_.Get(), &b, sizeof(b), 0);
        ctx_->GetData(tsEnd_.Get(), &e, sizeof(e), 0);
        timing->gpuMs = (!dj.Disjoint && dj.Frequency) ? 1000.0 * static_cast<double>(e - b) / static_cast<double>(dj.Frequency) : -1;
    }
    return true;
}

}  // namespace ixc::processing
