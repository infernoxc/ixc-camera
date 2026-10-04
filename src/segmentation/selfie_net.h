#pragma once

// Person segmentation network: MediaPipe's selfie segmenter (landscape, 256x144), run by IXC's own
// kernels. The model is compiled in (third_party/mediapipe_selfie_segmenter): a fixed list of
// operations and its weights. Nothing is loaded from disk or interpreted from external data.
//
// Init() makes the only allocations (weights decoded to float, one activation arena, ~3 MB).
// Run() allocates nothing. Not thread-safe: one instance per worker thread.

#include <cstdint>
#include <vector>

namespace ixc::seg {

// Operation kinds and fused activations (the converter script writes these numbers).
enum class OpType : std::uint8_t { Conv, DwConv, Mean, Mul, Add, Logistic, Relu, HardSwish, Resize2x, Deconv2x };
enum class Act : std::uint8_t { None, Relu, HardSwish, Logistic };

struct Op {
    int type, act;
    int in0, in1, out;     // float offsets in the arena (in1 = -1 when unused)
    int h, w, c;           // input shape (NHWC, N = 1)
    int oh, ow, oc;        // output shape
    int k, stride;         // kernel size (Mul/Add: 1 = in1 is per-channel), stride
    int padT, padL;        // SAME padding before the first row/column
    int wOff, bOff;        // weight/bias offsets in the weight table (-1 when unused)
};

class SelfieNet {
public:
    static constexpr int kWidth = 256, kHeight = 144;

    bool Init();  // false if the model is inconsistent (never expected) or out of memory
    bool Ready() const { return !arena_.empty(); }

    // Input: kWidth x kHeight RGB, interleaved, values 0..1. Write it here before Run().
    float* Input();
    // Runs the network. Returns kWidth x kHeight person probabilities (0..1), valid until the next Run.
    const float* Run();

    size_t MemoryBytes() const { return (arena_.capacity() + weights_.capacity()) * sizeof(float); }

private:
    std::vector<float> weights_;
    std::vector<float> arena_;
};

}  // namespace ixc::seg
