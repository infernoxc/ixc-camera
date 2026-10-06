#pragma once

// IXC camera profile: everything needed to reproduce a configured camera look.
//
// Profiles are human-readable, versioned JSON. Loading never silently resets a profile:
// every value that had to be clamped, defaulted or dropped is reported as a warning so the UI
// can tell the user what changed.

#include "common/json.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ixc {

inline constexpr int kProfileSchemaVersion = 1;
inline constexpr size_t kMaxProfileNameChars = 64;
inline constexpr size_t kMaxEnabledEffects = 16;
inline constexpr int kMaxTrackedFaces = 4;

enum class PerformanceTier { Auto, UltraLow, Low, Balanced, High };

// GPU use for processing. Auto = only where it's measured to help on this PC (see
// processing/backend_selector.h); Off = always the CPU path.
// Where the heavy work runs: the picture pipeline (processing/gpu) and the segmentation network
// (segmentation/gpu). Auto measures on this PC and picks; Gpu uses the GPU whenever it works
// (any Direct3D 11 GPU: NVIDIA, AMD or Intel) and falls back to the CPU if it doesn't; Cpu never
// touches the GPU. Saved as "processing"; profiles from 0.11 used "gpu": "auto" | "off".
enum class ProcessingMode { Auto, Cpu, Gpu };
std::string_view ToString(ProcessingMode m);  // "auto", "cpu", "gpu"

std::string_view ToString(PerformanceTier t);
bool ParsePerformanceTier(std::string_view s, PerformanceTier& out);

// Software image adjustments. Ranges are enforced by Validate().
struct ImageSettings {
    double brightness = 0;      // -100..100
    double contrast = 0;        // -100..100
    double saturation = 0;      // -100..100
    double gamma = 1.0;         // 0.2..3.0
    double sharpness = 15;      // 0..100, subtle default
    double temperature = 0;     // -100..100 (cool..warm)
    double tint = 0;            // -100..100 (green..magenta)
    double highlights = 0;      // -100..100
    double shadows = 0;         // -100..100
    double exposureEv = 0;      // -2..2
    double denoise = 0;         // 0..100
    double lowLight = 0;        // 0..100

    bool operator==(const ImageSettings&) const = default;
};

// Normalized crop rectangle in source-frame coordinates (0..1).
struct CropRect {
    double x = 0, y = 0, width = 1, height = 1;
    bool operator==(const CropRect&) const = default;
};

struct EffectEntry {
    std::string id;        // [a-z0-9._-], 1..64 chars; never a path
    double strength = 100; // 0..100
    bool operator==(const EffectEntry&) const = default;
};

// Background behind the person (needs person segmentation, see segmentation/).
enum class BackgroundMode { Original, Blur, Replace, Color, Custom };
enum class BlurLevel { Low, Medium, High };
enum class BackgroundFit { Fill, Fit };  // Fill: cover the frame (crop); Fit: whole image, edges extended
enum class BlurStyle { Standard, Bokeh };  // Bokeh: highlight bloom and a stronger focus falloff (DSLR-like)
// Blur presets (the app's Style list); Custom = the advanced values were changed by hand.
enum class BlurPreset { Soft, Standard, Dslr, Strong, Custom };

struct BackgroundSettings {
    BackgroundMode mode = BackgroundMode::Original;
    BlurLevel blur = BlurLevel::Medium;    // 0.12 setting, kept for older apps; 0.13 uses `strength`
    // 0.13 blur controls (0..100 each).
    BlurPreset preset = BlurPreset::Standard;
    BlurStyle style = BlurStyle::Standard;
    double strength = 55;        // 0 = no blur, 25 subtle, 50 natural, 65 DSLR-like, 100 maximum
    double falloff = 50;         // focus falloff: how far from you the blur reaches full strength
    double feather = 35;         // edge softness (0 = crisp cut-out, 100 = very soft)
    double edgeProtection = 60;  // keeps face, ears and hairline sharp (face tracking)
    double temporal = 50;        // mask stability over time (more = steadier, slower to follow)
    double hair = 50;            // edge detail: how closely the mask follows hair and fine edges
    std::string builtin = "studio-light";  // built-in background id ([a-z0-9-], see effects/backgrounds.h)
    std::uint32_t color = 0x3A4A5C;        // Color mode, 0xRRGGBB
    std::string image;                     // Custom mode: name of a prepared image in the backgrounds folder; never a path
    BackgroundFit fit = BackgroundFit::Fill;
    double posX = 0.5, posY = 0.5;         // 0..1: which part of the image stays visible when cropped
    double scale = 1.0;                    // 1..3: extra zoom into the image
    bool operator==(const BackgroundSettings&) const = default;
};

// Power-line frequency of the room's lighting, for the camera's anti-flicker control.
enum class AntiFlicker { Auto, Hz50, Hz60, Off };
std::string_view ToString(AntiFlicker a);  // "auto", "50hz", "60hz", "off"

// Applies a preset's values (strength, style, falloff); Custom leaves them.
void ApplyBlurPreset(BackgroundSettings& b, BlurPreset preset);

inline constexpr size_t kMaxBackgroundNameChars = 64;
bool IsValidBackgroundName(std::string_view name);  // [a-z0-9-], 1..64
std::string_view ToString(BackgroundMode m);
std::string_view ToString(BlurLevel b);

struct FaceTrackingSettings {
    bool enabled = false;
    int maxFaces = 1;             // 1..kMaxTrackedFaces
    int detectionIntervalFrames = 0;  // 0 = automatic by tier; otherwise 1..120
    bool operator==(const FaceTrackingSettings&) const = default;
};

struct HotkeyBinding {
    std::string action;  // e.g. "effects.toggle"
    std::string keys;    // e.g. "Ctrl+Alt+F8"
    bool operator==(const HotkeyBinding&) const = default;
};

struct Profile {
    int schemaVersion = kProfileSchemaVersion;
    std::string name = "Default";
    std::string sourceCameraId;  // Media Foundation symbolic link; empty = first available
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t fpsNumerator = 30;
    std::uint32_t fpsDenominator = 1;
    bool mirror = false;
    double zoom = 1.0;  // digital zoom 1..4
    CropRect crop;
    ImageSettings image;
    std::vector<EffectEntry> effects;
    BackgroundSettings background;
    FaceTrackingSettings faceTracking;
    PerformanceTier tier = PerformanceTier::Auto;
    ProcessingMode processing = ProcessingMode::Auto;
    // Keep the camera at its full frame rate in low light (fixed exposure + brightness
    // compensation) instead of letting auto exposure slow it down. See camera/exposure_governor.h.
    bool smoothMotion = true;
    // Mains-light flicker (the camera's power-line frequency control). Auto = the frequency of the
    // user's region (left as the camera has it where the region doesn't say). See camera/power_line.h.
    AntiFlicker antiFlicker = AntiFlicker::Auto;
    // Keep the tracked face framed: zoom and pan smoothly inside the crop/zoom rectangle (uses
    // face tracking). See face/auto_framer.h.
    bool autoFraming = false;
    bool effectsEnabled = true;  // master switch (hotkey), keeps the effect list intact
    std::vector<HotkeyBinding> hotkeys;

    bool operator==(const Profile&) const = default;
};

struct ProfileLoadResult {
    bool ok = false;
    Profile profile;
    std::string error;                  // set when ok == false
    std::vector<std::string> warnings;  // values clamped/defaulted/ignored
};

// Parses and validates a profile document. Newer schema versions are rejected rather than
// guessed at; older versions are migrated forward.
ProfileLoadResult ProfileFromJson(std::string_view text);
std::string ProfileToJson(const Profile& p);

// Clamps every field into its documented range; returns a description of each change.
std::vector<std::string> Validate(Profile& p);

bool IsValidEffectId(std::string_view id);
bool IsValidProfileName(std::string_view name);

}  // namespace ixc
