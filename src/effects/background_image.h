#pragma once

// Background pictures for the Replace and Custom background modes.
//
// A picture is stored and kept in memory as NV12 (BT.709, video range) at most 1920x1080: the
// format the camera frames use, so compositing needs no colour conversion per frame.
//
// Custom pictures (JPG/PNG/WebP/...) are decoded by the IXC app (Windows Imaging Component, in
// the user's own process), downscaled and written as a ".ixbg" file to the shared backgrounds
// folder. The IXC Camera service only ever reads that simple, strictly validated format: it never
// decodes arbitrary image files.
//
// .ixbg layout (little endian): "IXBG", u32 version = 1, u32 width, u32 height, then the Y plane
// (width*height bytes) and the interleaved UV plane (width*height/2 bytes). Width and height are
// even, 16..1920 x 16..1080, and the file size must match exactly.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace ixc::effects {

inline constexpr int kMaxBackgroundW = 1920, kMaxBackgroundH = 1080;

struct BackgroundImage {
    int width = 0, height = 0;    // even
    std::vector<std::uint8_t> y;  // width * height
    std::vector<std::uint8_t> uv; // width * height / 2, Cb,Cr interleaved
    size_t Bytes() const { return y.capacity() + uv.capacity(); }
};

// Converts an RGB picture (rows of `stride` bytes, 3 bytes per pixel, R,G,B) into a background,
// area-downscaled to fit within maxW x maxH with its aspect ratio kept. Null on bad input.
std::shared_ptr<BackgroundImage> MakeBackgroundImage(const std::uint8_t* rgb, int width, int height, int stride,
                                                     int maxW = kMaxBackgroundW, int maxH = kMaxBackgroundH);

bool SaveBackgroundImage(const std::filesystem::path& file, const BackgroundImage& image);
// Null when the file is missing, too large or not a valid .ixbg.
std::shared_ptr<const BackgroundImage> LoadBackgroundImage(const std::filesystem::path& file);

}  // namespace ixc::effects
