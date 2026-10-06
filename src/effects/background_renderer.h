#pragma once

// Background modes (Blur, Replace, Color, Custom), driven by the person mask from
// segmentation/segmentation_engine.h. Applied in place to the processed NV12 output frame.
//
// Pipeline per frame:
//   1. Effective mask: the refined person mask, plus a face guard (a soft head-and-neck shape
//      from the face tracker, when available) so the face, ears, nose and hairline are never cut
//      even when the network is unsure (side light, low light, glasses, headsets).
//   2. Background source:
//        Blur    - "depth-like": the frame is reduced to 1/8 scale, background-weighted (the
//                  person's colours don't bleed into the blur: no halo), and blurred twice: a
//                  near level and a far level. The blur grows with the distance from the person,
//                  like a real lens focused on the subject.
//        Replace/Custom - a picture "plate" at frame size (cover or fit, position, scale), built
//                  once per picture/size/setting and reused every frame.
//        Color   - a constant colour.
//   3. Compositing: the mask, upsampled through the crop/zoom/mirror mapping, blends source and
//      frame per pixel. Rows that are all person are skipped.
// Memory: scratch at 1/8 scale (tens of KB), the 512x288 mask copy, and the plate (frame size,
// only for pictures). Nothing is allocated per frame once sizes are stable.

#include "effects/background_image.h"
#include "face/face_types.h"
#include "processing/image_pipeline.h"
#include "profiles/profile.h"
#include "segmentation/segmentation_engine.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace ixc::effects {

struct BackgroundConfig {
    BackgroundMode mode = BackgroundMode::Original;
    BlurLevel blur = BlurLevel::Medium;
    std::uint32_t color = 0x3A4A5C;
    BackgroundFit fit = BackgroundFit::Fill;
    float posX = 0.5f, posY = 0.5f, scale = 1.0f;
    std::shared_ptr<const BackgroundImage> image;  // Replace (built-in) or Custom; null = not available
    bool Active() const { return mode != BackgroundMode::Original; }
};

struct BackgroundContext {
    const seg::SegMask* mask = nullptr;         // person mask (source coordinates); may be null/empty
    const face::FaceSnapshot* faces = nullptr;  // optional face guard (source coordinates)
    face::OutputMapping map;                    // source → output (crop/zoom/mirror)
    bool fullRange = false;
};

class BackgroundRenderer {
public:
    // Applies cfg to the frame. Without a mask yet, the frame is left unchanged (the effect fades
    // in over a few frames once the first mask arrives).
    void Apply(const processing::Nv12Frame& f, const BackgroundConfig& cfg, const BackgroundContext& ctx);
    size_t ScratchBytes() const;
    void ReleasePlate();  // frees the picture plate (when the mode stops using it)
    void ReleaseBlur();   // frees the blur buffers (when the mode stops using them)

    // Exposed for tests: the effective mask after the face guard (kMaskW x kMaskH).
    const std::vector<std::uint8_t>& EffectiveMask() const { return mask_; }

private:
    void BuildMask(const BackgroundContext& ctx);
    void BuildBlurSource(const processing::Nv12Frame& f, const BackgroundConfig& cfg, const BackgroundContext& ctx);
    void BuildPlate(const processing::Nv12Frame& f, const BackgroundConfig& cfg, bool fullRange);
    void Composite(const processing::Nv12Frame& f, const BackgroundConfig& cfg, const BackgroundContext& ctx, float mix);

    float presence_ = 0;                       // 0..1 fade-in after the first mask
    std::vector<std::uint8_t> mask_;           // effective mask
    // Blur: 1/8-scale frame planes and the blurred result.
    int lw_ = 0, lh_ = 0;
    std::vector<std::uint8_t> lowY_, lowUV_;   // result: lw*lh, lw*lh*2
    std::vector<float> work_;                  // weighted planes and blur scratch
    // Picture plate at frame size, and what it was built from.
    std::vector<std::uint8_t> plateY_, plateUV_;
    const BackgroundImage* plateSource_ = nullptr;
    std::shared_ptr<const BackgroundImage> plateHold_;  // keeps the source alive while cached
    int plateW_ = 0, plateH_ = 0;
    BackgroundFit plateFit_ = BackgroundFit::Fill;
    float platePos_[3] = {-1, -1, -1};
    bool plateFull_ = false;
    // Per-row compositing tables.
    std::vector<int> colIdx_, colW_, maskCol_, maskColW_;
    std::vector<std::int16_t> rowWeight_, lowRow_;
};

}  // namespace ixc::effects
