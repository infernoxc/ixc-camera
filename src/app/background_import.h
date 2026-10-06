#pragma once

// Custom backgrounds: the app decodes the user's picture (JPG, PNG, WEBP, BMP via Windows
// Imaging Component), downscales it to at most 1920x1080 and stores it as a prepared .ixbg in the
// backgrounds folder, where IXC Camera (inside the camera service) reads it. The service never
// decodes image files itself. The folder keeps the 12 most recently used pictures.

#include <filesystem>
#include <string>
#include <vector>

namespace ixc::app {

inline constexpr size_t kMaxRecentBackgrounds = 12;

// Imports `file`; returns the background name (for profile.background.image) or "" with error set.
std::string ImportBackground(const std::filesystem::path& file, std::wstring& error);
// Prepared backgrounds, most recently used first (at most kMaxRecentBackgrounds; older ones are deleted).
std::vector<std::string> RecentBackgrounds();
// Marks a background as just used (keeps it at the top of the recent list).
void TouchBackground(const std::string& name);
bool RemoveBackground(const std::string& name);

}  // namespace ixc::app
