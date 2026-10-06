#pragma once

// Something that runs the segmentation network (segmentation/selfie_net.h): on the CPU
// (SelfieNet) or on the GPU (segmentation/gpu/gpu_selfie_net.h, Windows only). The engine owns
// exactly one at a time, created when needed and destroyed when the mode changes or it stops.

#include <cstddef>
#include <memory>
#include <string>

namespace ixc::seg {

enum class SegBackend { None, Cpu, Gpu };
const char* ToString(SegBackend b);

class NetRunner {
public:
    virtual ~NetRunner() = default;
    // kNetW x kNetH RGB (0..1), interleaved: write the frame here before Run().
    virtual float* Input() = 0;
    // kNetW x kNetH person probabilities, valid until the next Run(); nullptr on failure (e.g.
    // the GPU was removed or reset): the engine then falls back to the CPU.
    virtual const float* Run() = 0;
    virtual size_t MemoryBytes() const = 0;
    virtual SegBackend Backend() const = 0;
    virtual std::string Device() const = 0;  // "CPU" or the GPU adapter name (UTF-8)
};

std::unique_ptr<NetRunner> MakeCpuRunner();  // nullptr when out of memory

// Creates a GPU runner, or returns nullptr and a short reason (no Direct3D 11 hardware, a
// software-only adapter, out of video memory, self-check mismatch). Provided by the binaries
// that link the GPU path; the engine never depends on Direct3D itself.
using GpuRunnerFactory = std::unique_ptr<NetRunner> (*)(std::string& error);

}  // namespace ixc::seg
