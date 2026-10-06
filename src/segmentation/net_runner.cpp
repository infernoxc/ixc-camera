#include "segmentation/net_runner.h"

#include "segmentation/selfie_net.h"

namespace ixc::seg {

const char* ToString(SegBackend b) {
    switch (b) {
        case SegBackend::None: return "none";
        case SegBackend::Cpu: return "CPU";
        case SegBackend::Gpu: return "GPU";
    }
    return "?";
}

namespace {
class CpuRunner final : public NetRunner {
public:
    bool Init() { return net_.Init(); }
    float* Input() override { return net_.Input(); }
    const float* Run() override { return net_.Run(); }
    size_t MemoryBytes() const override { return net_.MemoryBytes(); }
    SegBackend Backend() const override { return SegBackend::Cpu; }
    std::string Device() const override { return "CPU"; }

private:
    SelfieNet net_;
};
}  // namespace

std::unique_ptr<NetRunner> MakeCpuRunner() {
    auto r = std::make_unique<CpuRunner>();
    if (!r->Init()) return nullptr;
    return r;
}

}  // namespace ixc::seg
