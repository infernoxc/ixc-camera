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
enum class GpuMode { Auto, Off };

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
    FaceTrackingSettings faceTracking;
    PerformanceTier tier = PerformanceTier::Auto;
    GpuMode gpu = GpuMode::Auto;
    // Keep the camera at its full frame rate in low light (fixed exposure + brightness
    // compensation) instead of letting auto exposure slow it down. See camera/exposure_governor.h.
    bool smoothMotion = true;
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
