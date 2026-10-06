#pragma once

// Turns the network's coarse person probabilities (256x144) into the mask the background effects
// use (512x288): edge-aware upsampling plus temporal stabilization. Pure computation, no threads;
// all buffers are allocated once (Init) and reused.
//
//   1. Bilinear upsampling of the probabilities to mask resolution.
//   2. Guided filter (He et al., "Guided Image Filtering") with the frame's luma as the guide: the
//      mask edge snaps to real image edges (hair, shoulders, ears) instead of the network's blocky
//      grid, without blurring it.
//   3. A soft threshold keeps confident regions solid and the uncertain band narrow.
//   4. Motion-adaptive temporal smoothing: where the new mask agrees with the previous one, it's
//      averaged (no edge flicker); where it changes a lot (someone moved), the new value wins at
//      once (no ghost trails).

#include <cstdint>
#include <vector>

namespace ixc::seg {

class MaskRefiner {
public:
    // Sizes: probabilities netW x netH, guide and mask outW x outH.
    bool Init(int netW, int netH, int outW, int outH);
    void Reset() { havePrevious_ = false; }  // next mask starts fresh (no temporal blend)
    size_t MemoryBytes() const;

    // prob: netW*netH floats 0..1. guide: outW*outH luma bytes. mask: outW*outH bytes (in/out:
    // holds the previous mask for temporal smoothing).
    void Refine(const float* prob, const std::uint8_t* guide, std::uint8_t* mask);
    // temporal 0..1: how strongly a steady edge is averaged over time (0.5 = default).
    // hair 0..1: how closely the edge follows fine image detail (guided-filter sensitivity).
    void SetParams(float temporal, float hair);

    // Individual steps, exposed for tests.
    void Upsample(const float* prob, float* out) const;
    void GuidedFilter(const std::uint8_t* guide, float* p);  // in place on p (outW*outH)
    static std::uint8_t Temporal(int previous, int current, float strength = 0.5f);

    // Mean over a (2r+1)^2 window, windows clipped at the borders. Safe in place (in == out): the
    // horizontal pass reads `in` and writes only the scratch buffer; the vertical pass reads only
    // the scratch buffer and writes `out` (proven in test_segmentation.cpp).
    void BoxFilter(const float* in, float* out, int r);

private:

    int netW_ = 0, netH_ = 0, w_ = 0, h_ = 0;
    float temporal_ = 0.5f, eps_ = 0.004f;
    std::vector<float> p_, i_, meanI_, meanP_, varI_, covIp_, tmp_;
    bool havePrevious_ = false;
};

}  // namespace ixc::seg
