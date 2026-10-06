#include "effects/background_image.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <new>

namespace ixc::effects {

namespace {

constexpr char kMagic[4] = {'I', 'X', 'B', 'G'};
constexpr std::uint32_t kVersion = 1;

void PutU32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint32_t GetU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

bool ValidSize(std::uint32_t w, std::uint32_t h) {
    return w >= 16 && h >= 16 && w <= static_cast<std::uint32_t>(kMaxBackgroundW) && h <= static_cast<std::uint32_t>(kMaxBackgroundH) &&
           w % 2 == 0 && h % 2 == 0;
}

}  // namespace

std::shared_ptr<BackgroundImage> MakeBackgroundImage(const std::uint8_t* rgb, int width, int height, int stride, int maxW, int maxH) {
    if (!rgb || width < 2 || height < 2 || stride < width * 3 || maxW < 16 || maxH < 16) return nullptr;
    // Output size: fit within max, keep aspect, even dimensions.
    const double s = std::min({1.0, static_cast<double>(maxW) / width, static_cast<double>(maxH) / height});
    const int ow = std::max(16, static_cast<int>(width * s) & ~1), oh = std::max(16, static_cast<int>(height * s) & ~1);
    auto img = std::make_shared<BackgroundImage>();
    try {
        img->width = ow;
        img->height = oh;
        img->y.resize(static_cast<size_t>(ow) * oh);
        img->uv.resize(static_cast<size_t>(ow) * oh / 2);
        // Area average into an RGB buffer at output size, then convert (BT.709, video range).
        std::vector<float> acc(static_cast<size_t>(ow) * 3);
        std::vector<std::uint8_t> out(static_cast<size_t>(ow) * oh * 3);
        for (int oy = 0; oy < oh; ++oy) {
            const int y0 = static_cast<int>(static_cast<long long>(oy) * height / oh);
            const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<long long>(oy + 1) * height / oh));
            std::fill(acc.begin(), acc.end(), 0.0f);
            for (int ox = 0; ox < ow; ++ox) {
                const int x0 = static_cast<int>(static_cast<long long>(ox) * width / ow);
                const int x1 = std::max(x0 + 1, static_cast<int>(static_cast<long long>(ox + 1) * width / ow));
                float r = 0, g = 0, b = 0;
                for (int sy = y0; sy < y1; ++sy) {
                    const std::uint8_t* row = rgb + static_cast<size_t>(sy) * stride;
                    for (int sx = x0; sx < x1; ++sx) {
                        r += row[sx * 3];
                        g += row[sx * 3 + 1];
                        b += row[sx * 3 + 2];
                    }
                }
                const float inv = 1.0f / static_cast<float>((y1 - y0) * (x1 - x0));
                std::uint8_t* o = out.data() + (static_cast<size_t>(oy) * ow + ox) * 3;
                o[0] = static_cast<std::uint8_t>(r * inv + 0.5f);
                o[1] = static_cast<std::uint8_t>(g * inv + 0.5f);
                o[2] = static_cast<std::uint8_t>(b * inv + 0.5f);
            }
        }
        for (int yy = 0; yy < oh; ++yy) {
            for (int xx = 0; xx < ow; ++xx) {
                const std::uint8_t* p = out.data() + (static_cast<size_t>(yy) * ow + xx) * 3;
                const float v = 16 + 0.1826f * p[0] + 0.6142f * p[1] + 0.0620f * p[2];
                img->y[static_cast<size_t>(yy) * ow + xx] = static_cast<std::uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
            }
        }
        for (int yy = 0; yy < oh / 2; ++yy) {
            for (int xx = 0; xx < ow / 2; ++xx) {
                float r = 0, g = 0, b = 0;
                for (int k = 0; k < 4; ++k) {
                    const std::uint8_t* p = out.data() + (static_cast<size_t>(yy * 2 + k / 2) * ow + xx * 2 + k % 2) * 3;
                    r += p[0];
                    g += p[1];
                    b += p[2];
                }
                r *= 0.25f, g *= 0.25f, b *= 0.25f;
                const float u = 128 - 0.1006f * r - 0.3386f * g + 0.4392f * b, v = 128 + 0.4392f * r - 0.3989f * g - 0.0403f * b;
                std::uint8_t* d = img->uv.data() + static_cast<size_t>(yy) * ow + xx * 2;
                d[0] = static_cast<std::uint8_t>(std::clamp(u + 0.5f, 0.0f, 255.0f));
                d[1] = static_cast<std::uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
            }
        }
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
    return img;
}

bool SaveBackgroundImage(const std::filesystem::path& file, const BackgroundImage& image) {
    if (!ValidSize(static_cast<std::uint32_t>(image.width), static_cast<std::uint32_t>(image.height)) ||
        image.y.size() != static_cast<size_t>(image.width) * image.height || image.uv.size() != image.y.size() / 2) {
        return false;
    }
    std::uint8_t header[16];
    std::memcpy(header, kMagic, 4);
    PutU32(header + 4, kVersion);
    PutU32(header + 8, static_cast<std::uint32_t>(image.width));
    PutU32(header + 12, static_cast<std::uint32_t>(image.height));
    // Write to a temporary name, then rename: a reader never sees a half-written file.
    std::filesystem::path tmp = file;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(header), sizeof header);
        out.write(reinterpret_cast<const char*>(image.y.data()), static_cast<std::streamsize>(image.y.size()));
        out.write(reinterpret_cast<const char*>(image.uv.data()), static_cast<std::streamsize>(image.uv.size()));
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    if (ec) std::filesystem::remove(tmp, ec);
    return !ec;
}

std::shared_ptr<const BackgroundImage> LoadBackgroundImage(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return nullptr;
    std::uint8_t header[16];
    if (!in.read(reinterpret_cast<char*>(header), sizeof header)) return nullptr;
    if (std::memcmp(header, kMagic, 4) != 0 || GetU32(header + 4) != kVersion) return nullptr;
    const std::uint32_t w = GetU32(header + 8), h = GetU32(header + 12);
    if (!ValidSize(w, h)) return nullptr;
    const size_t ySize = static_cast<size_t>(w) * h;
    std::error_code ec;
    const auto fileSize = std::filesystem::file_size(file, ec);
    if (ec || fileSize != sizeof header + ySize + ySize / 2) return nullptr;
    auto img = std::make_shared<BackgroundImage>();
    try {
        img->width = static_cast<int>(w);
        img->height = static_cast<int>(h);
        img->y.resize(ySize);
        img->uv.resize(ySize / 2);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
    if (!in.read(reinterpret_cast<char*>(img->y.data()), static_cast<std::streamsize>(ySize)) ||
        !in.read(reinterpret_cast<char*>(img->uv.data()), static_cast<std::streamsize>(ySize / 2))) {
        return nullptr;
    }
    return img;
}

}  // namespace ixc::effects
