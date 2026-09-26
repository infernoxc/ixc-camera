#pragma once

// Direct3D 11 compute implementation of the IXC image pipeline.
//
// Same interface and byte-identical output as the CPU Nv12Processor (unit-tested), so it can
// replace the CPU path only where it's measurably faster. It is never required: callers keep
// the CPU path as the fallback, and nothing here runs unless explicitly initialized.
//
// Frames arrive from the camera in system memory, so each frame is uploaded, processed and read
// back. The per-frame breakdown (Timing) lets the benchmark weigh that transfer cost honestly.

#include "processing/image_pipeline.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <string>

namespace ixc::processing {

class GpuNv12Processor {
public:
    struct Options {
        bool useWarp = false;   // software rasterizer (tests/CI only; never for production use)
        int adapterIndex = -1;  // DXGI adapter index; -1 = the system default adapter
    };
    struct Timing {
        double uploadMs = 0;    // CPU time issuing the uploads
        double gpuMs = 0;       // GPU execution of the dispatches (timestamp queries)
        double readbackMs = 0;  // copy to staging + map (waits for the GPU) + copy out
        double totalMs = 0;
    };

    // Creates the device, shaders and constant state. Returns a failure (and leaves the object
    // unusable) when no suitable Direct3D 11 hardware is available.
    HRESULT Initialize(const Options& options);
    bool Ready() const { return device_ != nullptr; }
    const std::wstring& AdapterName() const { return adapterName_; }

    // Same contract as Nv12Processor::Process.
    bool Process(const Nv12Planes& src, const Nv12Frame& dst, const PipelineParams& params, Timing* timing = nullptr);

    void Release();  // frees every GPU object (device included)

private:
    HRESULT EnsureResources(int width, int height);
    void UploadLut(const PipelineParams& p);
    void UploadGeometry(int width, int height, const PipelineParams& p);

    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    std::wstring adapterName_;
    ComPtr<ID3D11ComputeShader> csColorY_, csColorUV_, csScaleY_, csScaleUV_, csSharpen_;
    ComPtr<ID3D11Buffer> constants_;

    int width_ = 0, height_ = 0;
    ComPtr<ID3D11Texture2D> srcY_, srcUV_, midY_, dstY_, dstUV_, upY_, upUV_, stageY_, stageUV_;
    ComPtr<ID3D11ShaderResourceView> srcYSrv_, srcUVSrv_, midYSrv_;
    ComPtr<ID3D11UnorderedAccessView> midYUav_, dstYUav_, dstUVUav_;

    ComPtr<ID3D11Buffer> lut_;
    ComPtr<ID3D11ShaderResourceView> lutSrv_;
    std::array<std::uint8_t, 768> lutCache_{};
    bool lutValid_ = false;

    ComPtr<ID3D11Buffer> yX_, yY_, uvX_, uvY_;
    ComPtr<ID3D11ShaderResourceView> yXSrv_, yYSrv_, uvXSrv_, uvYSrv_;
    GeometryTables tables_;
    double geoKey_[5] = {-1, -1, -1, -1, -1};

    ComPtr<ID3D11Query> disjoint_, tsBegin_, tsEnd_;
};

}  // namespace ixc::processing
