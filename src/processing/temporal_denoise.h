#pragma once

// Motion-adaptive temporal denoise for NV12 frames (low light).
//
// Camera noise is different in every frame; the picture mostly isn't. Each pixel is blended with
// its own history (a recursive average) only where the frame hasn't changed:
//   * the change is measured on a 1-2-1 smoothed difference between the frame and the history,
//     so single noisy pixels don't count as motion;
//   * a small change (noise) gets a strong history weight; a change above the threshold
//     (motion, a new object, a lighting step) gets none, so moving parts never leave ghosts:
//     they show the current frame, just with its noise;
//   * the threshold follows the camera's measured noise: the 25th percentile of each frame's
//     smoothed change (a statistic moving areas barely affect) sets the next frame's threshold,
//     so a noisier picture in darker light is still recognised as noise;
//   * chroma uses the stricter of its own change and the change of the luma pixels it covers,
//     so colour never trails behind a moving edge.
// History is kept with 4 fractional bits, so slow brightness changes don't stick at rounding.
//
// Memory: 2 bytes per pixel of history (Y and UV, i.e. 3 bytes per frame pixel: 6.2 MB at
// 1080p) plus a few row buffers. Allocated on the first frame (or a size change) and released by
// Release(); nothing is allocated per frame.

#include "processing/image_pipeline.h"

#include <array>
#include <cstdint>
#include <vector>

namespace ixc::processing {

class TemporalDenoiser {
public:
    // strength 0..100 (0 = do nothing and keep no history). Works in place.
    void Apply(const Nv12Frame& f, double strength);
    // Forget the history (e.g. after a pause, a camera switch, or a jump in the view).
    void Reset() { histW_ = histH_ = 0; }
    void Release();
    size_t MemoryBytes() const;

    // Denoise strength Smooth motion asks for when it brightens the picture in software by ev
    // stops (gain multiplies the noise as well): 0 below a tenth of a stop, 25..60 above.
    static double AutoStrengthForGain(double ev);

    // Tests: run the scalar reference instead of the SSE2 rows (both must agree bit for bit).
    void SetScalarForTesting(bool scalar) { scalar_ = scalar; }

private:
    void Configure(double strength, double noise);
    void Row(std::uint8_t* row, std::uint16_t* hist, int n, int step, const std::uint8_t* kLimit, std::uint8_t* kOut, bool sample);

    std::vector<std::uint16_t> histY_, histUV_;
    int histW_ = 0, histH_ = 0;
    std::vector<std::uint8_t> cur_, prev_, kRow_, kRow2_;
    std::vector<std::uint16_t> dRow_;  // smoothed differences of a sampled row (noise estimate)
    // History weight (/16) of a pixel with smoothed difference d4 (x4):
    //   k = min(kmax, (thr4 - d4)+ * 16 * slope >> 16)  -- full up to thr/2, falling to 0 at thr.
    int thr4_ = 12, slope_ = 0, kmax_ = 0;
    std::array<std::uint32_t, 1024> hist_{};  // sampled luma differences, for the noise estimate
    double noise_ = 0;  // measured: smoothed-difference level typical of noise (8-bit levels)
    bool scalar_ = false;
};

}  // namespace ixc::processing
