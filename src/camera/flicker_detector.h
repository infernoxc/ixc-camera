#pragma once

// Detects mains-light banding: dark horizontal bands across the picture.
//
// Lamps on mains power flicker at twice the mains frequency (100 Hz / 120 Hz). A camera with a
// rolling shutter reads the picture row by row, so if its exposure isn't a whole number of
// flicker periods, rows exposed during the dim part of the cycle come out darker: horizontal
// bands, periodic down the frame. The camera's automatic exposure avoids this (it respects the
// power-line setting), but a fixed exposure doesn't, and UVC fixed exposures are powers of two
// (1/32 s = 31.25 ms: neither 30 ms nor 33.3 ms). So Smooth motion asks this detector, while its
// fixed exposure is on, whether bands appeared, and gives the exposure back to auto if they did.
//
// Two signals, from the mean brightness of sampled rows (no per-pixel work beyond that):
//   * moving bands (the usual case: 30 FPS under 100 Hz light): the frame-to-frame difference
//     of the row profile is a sinusoid down the frame. Scene motion isn't periodic like that.
//   * standing bands (frame rate locked to the flicker, e.g. 30 FPS under 120 Hz): the row
//     profile under fixed exposure divided by the profile under auto exposure (just before the
//     switch) is a sinusoid.
// A sinusoid's "share" of the signal is measured by a small DFT over 1.5..8 cycles per frame
// height (the range a rolling shutter can produce at 15..60 FPS), after removing a linear trend,
// and it must explain the signal in every quarter of the frame (bands cover the whole picture;
// a person moving changes one part of it).
//
// Pure logic (rows in, verdict out), no allocations: fixed arrays.

#include <array>
#include <cstdint>

namespace ixc::camera {

class FlickerDetector {
public:
    static constexpr int kMaxRows = 256;

    // Mean luma of up to kMaxRows evenly spaced rows (every 16th pixel of each). Returns the row
    // count written to out.
    static int RowMeans(const std::uint8_t* y, int stride, int width, int height, float* out);

    // Strength of the strongest sinusoid in d (n values): its share of the detrended signal's
    // variance (0..1; 0 when it doesn't hold in every quarter) and its amplitude.
    static double PeriodicShare(const float* d, int n, double& amplitude);

    void Reset();
    // Rows of a frame under the camera's automatic exposure (the reference for standing bands).
    void AddReference(const float* rows, int n);
    // Rows of a frame under fixed exposure. Returns true once banding is established; stays true
    // until Reset().
    bool AddFixed(const float* rows, int n);
    bool Banding() const { return banding_; }

private:
    std::array<float, kMaxRows> ref_{}, fixed_{}, prev_{};
    std::array<float, kMaxRows> work_{};
    int n_ = 0;            // row count of the current stream
    int refCount_ = 0, fixedCount_ = 0;
    bool havePrev_ = false;
    bool standingChecked_ = false;
    std::uint32_t hits_ = 0;  // last 32 frames: 1 = moving bands seen
    int seen_ = 0;
    bool banding_ = false;
};

}  // namespace ixc::camera
