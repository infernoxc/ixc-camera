#pragma once

// The segmentation network on the GPU: Direct3D 11 compute (any vendor: NVIDIA, AMD, Intel),
// kernels in selfie_net.hlsl, the same op table and weights as the CPU network.
//
// Per mask: one 442 KB upload (the 256x144 RGB input), 94 small dispatches, one 147 KB
// readback (the probabilities). The weights (426 KB) and the activation arena (2.2 MB) stay in
// video memory. Before a runner is handed out it is checked against the CPU network on a test
// picture; a mismatch, a software-only adapter or any Direct3D failure means "no GPU runner"
// (the engine stays on the CPU and reports why).

#include "segmentation/net_runner.h"

#include <memory>
#include <string>

namespace ixc::seg {

// Default hardware adapter only (software rasterizers are refused: they'd only cost CPU).
std::unique_ptr<NetRunner> MakeGpuRunner(std::string& error);
// Tests/CI: also accepts the WARP software device (no GPU on the build machines).
std::unique_ptr<NetRunner> MakeGpuRunnerAllowWarp(std::string& error);

}  // namespace ixc::seg
