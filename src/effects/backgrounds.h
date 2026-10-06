#pragma once

// Built-in backgrounds for the Replace mode: original artwork generated in code (no image files),
// drawn as softly defocused scenes so they read like a real room behind a person with a shallow
// depth of field. Each is rendered once at 640x360 when selected (~350 KB) and released when no
// longer used.

#include "effects/background_image.h"
#include "profiles/profile.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ixc::effects {

struct BuiltinBackground {
    const char* id;        // stored in profiles (background.builtin)
    const wchar_t* name;   // shown in the app
};

const std::vector<BuiltinBackground>& BuiltinBackgrounds();
bool IsBuiltinBackground(std::string_view id);

// Renders a built-in background. Null for unknown ids.
std::shared_ptr<const BackgroundImage> RenderBuiltinBackground(std::string_view id, int width = 640, int height = 360);

// Resolves the picture the Background setting needs and keeps exactly one cached: switching
// between settings that use the same picture costs nothing, a different picture replaces the old
// one, and modes without a picture release it. Custom pictures are re-read only when their file
// changes. Not thread-safe (one per settings owner).
class BackgroundSource {
public:
    // folder: the backgrounds folder (custom pictures are <folder>/<name>.ixbg).
    std::shared_ptr<const BackgroundImage> Resolve(const BackgroundSettings& settings, const std::filesystem::path& folder);
    void Clear();

private:
    std::string key_;
    std::filesystem::file_time_type time_{};
    std::shared_ptr<const BackgroundImage> image_;
};

// File for a custom picture name inside the backgrounds folder.
std::filesystem::path CustomBackgroundFile(const std::filesystem::path& folder, std::string_view name);

}  // namespace ixc::effects
