#pragma once

// IXC effect framework (Phase 8): original, built-in effects applied in place to the processed
// NV12 output frame, after the picture pipeline.
//
// * Effects are data-described (EffectInfo: cost class, face needs, fallback). They're compiled
//   once per settings change (EffectConfig) and rendered per frame by an EffectRenderer that
//   owns fixed scratch memory (grown only when a larger face/frame appears, never per frame).
// * Face-aware effects read the face tracker's snapshot (face/face_engine.h). They never wait
//   for it: without a face they fade out (blush, beauty) or use a centred subject (portrait).
// * Nothing here loads or executes external content. Unknown effect ids are ignored.

#include "face/face_types.h"
#include "processing/image_pipeline.h"
#include "profiles/profile.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace ixc::effects {

enum class Cost { VeryLow, Low, Moderate };  // spec tiers A/B (no tier C effects yet)

struct EffectInfo {
    const char* id;
    const wchar_t* name;
    const char* category;     // "face", "portrait", "color", "lighting"
    bool needsFace;           // uses the tracked face (box)
    bool needsLandmarks;      // anchored to landmarks (falls back to box estimates)
    Cost cost;
    const char* fallback;     // behaviour when no face / weak hardware
};

// Built-in catalog, in display order.
const std::vector<EffectInfo>& Catalog();
const EffectInfo* Find(std::string_view id);

enum class Kind { Blush, Beauty, Portrait, Grade };

// Compiled settings for one frame size-independent chain.
struct EffectConfig {
    struct Face {
        Kind kind;
        float strength;  // 0..1
    };
    std::vector<Face> faceEffects;   // blush, beauty, portrait (in application order)
    bool grade = false;              // colour/lighting LUTs below are not identity
    std::array<std::uint8_t, 256> yLut{}, uLut{}, vLut{};
    bool needsFaces = false;
    bool Active() const { return grade || !faceEffects.empty(); }
};

// Compiles the profile's enabled effects (unknown ids ignored, strengths clamped).
// Compiles the profile's effects; an empty (inactive) config when profile.effectsEnabled is off.
std::shared_ptr<const EffectConfig> CompileEffects(const Profile& profile, bool fullRange);
std::shared_ptr<const EffectConfig> CompileEffects(const std::vector<EffectEntry>& effects, bool fullRange);

struct FrameContext {
    const face::FaceSnapshot* faces = nullptr;  // may be null (tracking unavailable)
    face::OutputMapping map;                    // source → output (crop/zoom/mirror)
    bool fullRange = false;
};

class EffectRenderer {
public:
    // Applies cfg to frame in place. frame.width/height even.
    void Apply(const processing::Nv12Frame& frame, const EffectConfig& cfg, const FrameContext& ctx);
    size_t ScratchBytes() const;

private:
    struct Region {  // a face in output pixel coordinates, remembered for fade-out
        float x = 0, y = 0, w = 0, h = 0;  // box
        face::PointF cheekL, cheekR;       // output pixels
        float cheekRx = 0, cheekRy = 0;
        bool valid = false;
    };
    void UpdateFace(const processing::Nv12Frame& f, const FrameContext& ctx);
    void Blush(const processing::Nv12Frame& f, float a);
    void Beauty(const processing::Nv12Frame& f, float a);
    void Portrait(const processing::Nv12Frame& f, float a);
    void Grade(const processing::Nv12Frame& f, const EffectConfig& cfg);

    Region face_;
    float presence_ = 0;  // 0..1 fade for face-anchored effects
    std::vector<std::uint16_t> tmp_;   // beauty: horizontal box sums
    std::vector<std::uint8_t> blur_;   // beauty: blurred ROI
    std::vector<std::uint8_t> lowY_, lowUV_;  // portrait: 1/8-scale background
    std::vector<float> colDx2_;               // portrait: per-column tables
    std::vector<std::int16_t> upRow_;
    std::vector<int> colIdx_, colW_;
    std::vector<std::int16_t> lowRow_;
};

}  // namespace ixc::effects
