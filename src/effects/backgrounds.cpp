#include "effects/backgrounds.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ixc::effects {

namespace {

const std::vector<BuiltinBackground> kBuiltins = {
    {"office", L"Office"},
    {"modern-office", L"Modern office"},
    {"gaming-room", L"Gaming room"},
    {"studio-light", L"Studio"},
    {"studio-dark", L"Dark studio"},
    {"minimal-room", L"Minimal room"},
    {"nature", L"Nature"},
    {"abstract", L"Abstract"},
    {"gradient", L"Gradient"},
};

struct Rgb {
    float r, g, b;
};
Rgb Mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

// A small float RGB canvas with the few soft primitives the scenes need. Coordinates are
// normalized (0..1) so scenes don't depend on the render size.
class Canvas {
public:
    Canvas(int w, int h) : w_(w), h_(h), px_(static_cast<size_t>(w) * h) {}

    void VerticalGradient(Rgb top, Rgb bottom) {
        for (int y = 0; y < h_; ++y) {
            const Rgb c = Mix(top, bottom, static_cast<float>(y) / static_cast<float>(h_ - 1));
            std::fill(px_.begin() + static_cast<std::ptrdiff_t>(y) * w_, px_.begin() + static_cast<std::ptrdiff_t>(y + 1) * w_, c);
        }
    }
    template <typename F>
    void Shade(F f) {  // f(x, y) -> Rgb, normalized coordinates
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) At(x, y) = f((static_cast<float>(x) + 0.5f) / static_cast<float>(w_), (static_cast<float>(y) + 0.5f) / static_cast<float>(h_));
    }
    void Rect(float x0, float y0, float x1, float y1, Rgb c, float a = 1) {
        const int px0 = std::max(0, static_cast<int>(x0 * static_cast<float>(w_))), px1 = std::min(w_, static_cast<int>(x1 * static_cast<float>(w_)));
        const int py0 = std::max(0, static_cast<int>(y0 * static_cast<float>(h_))), py1 = std::min(h_, static_cast<int>(y1 * static_cast<float>(h_)));
        for (int y = py0; y < py1; ++y)
            for (int x = px0; x < px1; ++x) At(x, y) = Mix(At(x, y), c, a);
    }
    // Disk with a soft edge; `rim` brightens the edge like out-of-focus light (bokeh).
    void Disk(float cx, float cy, float r, Rgb c, float a = 1, float rim = 0) {
        const float pcx = cx * static_cast<float>(w_), pcy = cy * static_cast<float>(h_), pr = r * static_cast<float>(h_);
        const int x0 = std::max(0, static_cast<int>(pcx - pr - 2)), x1 = std::min(w_ - 1, static_cast<int>(pcx + pr + 2));
        const int y0 = std::max(0, static_cast<int>(pcy - pr - 2)), y1 = std::min(h_ - 1, static_cast<int>(pcy + pr + 2));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float d = std::hypot(static_cast<float>(x) + 0.5f - pcx, static_cast<float>(y) + 0.5f - pcy);
                const float cover = std::clamp(pr - d + 0.5f, 0.0f, 1.0f);
                if (cover <= 0) continue;
                const float edge = rim * std::clamp(1 - (pr - d) / (pr * 0.25f + 1), 0.0f, 1.0f);
                At(x, y) = Mix(At(x, y), {c.r + edge, c.g + edge, c.b + edge}, cover * a);
            }
    }
    void Vignette(float strength) {
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f) / static_cast<float>(w_) - 0.5f, dy = (static_cast<float>(y) + 0.5f) / static_cast<float>(h_) - 0.5f;
                const float k = 1 - strength * (dx * dx + dy * dy) * 2;
                Rgb& p = At(x, y);
                p = {p.r * k, p.g * k, p.b * k};
            }
    }
    // Three box passes ≈ Gaussian (defocus).
    void Blur(int radius) {
        if (radius < 1) return;
        std::vector<Rgb> tmp(px_.size());
        for (int pass = 0; pass < 3; ++pass) {
            BoxPass(px_.data(), tmp.data(), radius, true);
            BoxPass(tmp.data(), px_.data(), radius, false);
        }
    }
    std::vector<std::uint8_t> ToRgb8() const {
        std::vector<std::uint8_t> out(px_.size() * 3);
        for (size_t i = 0; i < px_.size(); ++i) {
            out[i * 3] = static_cast<std::uint8_t>(std::clamp(px_[i].r * 255.0f + 0.5f, 0.0f, 255.0f));
            out[i * 3 + 1] = static_cast<std::uint8_t>(std::clamp(px_[i].g * 255.0f + 0.5f, 0.0f, 255.0f));
            out[i * 3 + 2] = static_cast<std::uint8_t>(std::clamp(px_[i].b * 255.0f + 0.5f, 0.0f, 255.0f));
        }
        return out;
    }

private:
    Rgb& At(int x, int y) { return px_[static_cast<size_t>(y) * w_ + x]; }
    void BoxPass(const Rgb* in, Rgb* out, int r, bool horizontal) {
        const int len = horizontal ? w_ : h_, lines = horizontal ? h_ : w_;
        for (int line = 0; line < lines; ++line) {
            auto idx = [&](int i) { return horizontal ? static_cast<size_t>(line) * w_ + i : static_cast<size_t>(i) * w_ + line; };
            Rgb sum{0, 0, 0};
            for (int k = -r; k <= r; ++k) {
                const Rgb& p = in[idx(std::clamp(k, 0, len - 1))];
                sum = {sum.r + p.r, sum.g + p.g, sum.b + p.b};
            }
            const float inv = 1.0f / static_cast<float>(2 * r + 1);
            for (int i = 0; i < len; ++i) {
                out[idx(i)] = {sum.r * inv, sum.g * inv, sum.b * inv};
                const Rgb& add = in[idx(std::min(i + r + 1, len - 1))];
                const Rgb& sub = in[idx(std::max(i - r, 0))];
                sum = {sum.r + add.r - sub.r, sum.g + add.g - sub.g, sum.b + add.b - sub.b};
            }
        }
    }

    int w_, h_;
    std::vector<Rgb> px_;
};

// Deterministic pseudo-random numbers (scenes look the same every time).
struct Lcg {
    std::uint32_t s;
    float Next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(s >> 8) / 16777216.0f;
    }
};

void Office(Canvas& c, int w) {
    c.VerticalGradient({0.80f, 0.73f, 0.64f}, {0.60f, 0.53f, 0.45f});
    c.Rect(0.60f, 0.06f, 0.97f, 0.66f, {0.98f, 0.96f, 0.90f});  // window
    for (float x : {0.72f, 0.85f}) c.Rect(x, 0.06f, x + 0.012f, 0.66f, {0.55f, 0.50f, 0.44f});
    c.Rect(0.60f, 0.35f, 0.97f, 0.36f, {0.55f, 0.50f, 0.44f});
    c.Rect(0.03f, 0.14f, 0.32f, 0.82f, {0.30f, 0.21f, 0.14f});  // bookshelf
    Lcg r{7};
    for (float shelf : {0.17f, 0.34f, 0.51f, 0.68f}) {
        c.Rect(0.03f, shelf + 0.13f, 0.32f, shelf + 0.145f, {0.22f, 0.15f, 0.10f});
        for (float x = 0.05f; x < 0.29f;) {
            const float bw = 0.012f + r.Next() * 0.02f, bh = 0.08f + r.Next() * 0.045f;
            const Rgb book = Mix(Mix({0.55f, 0.20f, 0.18f}, {0.20f, 0.32f, 0.50f}, r.Next()), {0.85f, 0.80f, 0.65f}, r.Next() * 0.5f);
            c.Rect(x, shelf + 0.13f - bh, x + bw, shelf + 0.13f, book);
            x += bw + 0.003f;
        }
    }
    c.Rect(0.37f, 0.66f, 0.47f, 0.82f, {0.42f, 0.36f, 0.30f});  // plant pot + leaves
    for (int i = 0; i < 14; ++i) c.Disk(0.36f + r.Next() * 0.12f, 0.46f + r.Next() * 0.2f, 0.035f + r.Next() * 0.03f, Mix({0.22f, 0.38f, 0.18f}, {0.40f, 0.55f, 0.25f}, r.Next()));
    c.Rect(0.0f, 0.82f, 1.0f, 1.0f, {0.38f, 0.27f, 0.18f});  // desk
    c.Blur(std::max(2, w / 60));
    for (int i = 0; i < 10; ++i) c.Disk(0.62f + r.Next() * 0.34f, 0.08f + r.Next() * 0.5f, 0.03f + r.Next() * 0.03f, {1.0f, 0.97f, 0.88f}, 0.35f, 0.08f);
    c.Blur(std::max(1, w / 320));
    c.Vignette(0.25f);
}

void ModernOffice(Canvas& c, int w) {
    c.VerticalGradient({0.88f, 0.90f, 0.92f}, {0.70f, 0.74f, 0.78f});
    for (float x = 0.05f; x < 0.95f; x += 0.3f) c.Rect(x, 0.02f, x + 0.2f, 0.045f, {1.0f, 1.0f, 0.98f});  // ceiling lights
    c.Rect(0.08f, 0.15f, 0.92f, 0.72f, {0.74f, 0.82f, 0.88f}, 0.65f);  // glass partition
    for (float x = 0.08f; x < 0.93f; x += 0.21f) c.Rect(x, 0.15f, x + 0.008f, 0.72f, {0.55f, 0.58f, 0.62f});
    Lcg r{11};
    for (int i = 0; i < 6; ++i) {  // people and desks beyond the glass, very out of focus
        const float x = 0.12f + r.Next() * 0.75f;
        c.Disk(x, 0.42f, 0.05f, Mix({0.35f, 0.38f, 0.45f}, {0.55f, 0.45f, 0.40f}, r.Next()), 0.7f);
        c.Rect(x - 0.06f, 0.55f, x + 0.06f, 0.72f, {0.40f, 0.43f, 0.48f}, 0.7f);
    }
    c.Rect(0.0f, 0.72f, 1.0f, 1.0f, {0.52f, 0.54f, 0.57f});
    for (int i = 0; i < 5; ++i) c.Disk(0.1f + r.Next() * 0.8f, 0.6f + r.Next() * 0.2f, 0.05f, {0.30f, 0.50f, 0.32f});  // plants
    c.Blur(std::max(2, w / 55));
    for (int i = 0; i < 12; ++i) c.Disk(r.Next(), 0.05f + r.Next() * 0.3f, 0.025f + r.Next() * 0.03f, {1.0f, 1.0f, 1.0f}, 0.3f, 0.06f);
    c.Blur(std::max(1, w / 320));
}

void GamingRoom(Canvas& c, int w) {
    c.VerticalGradient({0.07f, 0.06f, 0.12f}, {0.03f, 0.03f, 0.06f});
    c.Rect(0.0f, 0.10f, 1.0f, 0.125f, {0.85f, 0.15f, 0.75f});  // LED strips
    c.Rect(0.0f, 0.74f, 1.0f, 0.755f, {0.10f, 0.80f, 0.95f});
    c.Rect(0.30f, 0.30f, 0.70f, 0.68f, {0.12f, 0.20f, 0.45f});  // monitor glow
    Lcg r{23};
    for (int i = 0; i < 3; ++i) {
        const float y = 0.25f + static_cast<float>(i) * 0.15f;
        c.Rect(0.04f, y, 0.24f, y + 0.012f, {0.20f, 0.18f, 0.26f});  // shelves
        c.Rect(0.76f, y, 0.96f, y + 0.012f, {0.20f, 0.18f, 0.26f});
    }
    c.Blur(std::max(2, w / 70));
    const Rgb leds[] = {{0.95f, 0.25f, 0.85f}, {0.20f, 0.85f, 1.0f}, {0.55f, 0.35f, 1.0f}, {1.0f, 0.55f, 0.25f}};
    for (int i = 0; i < 22; ++i)
        c.Disk(r.Next() < 0.5f ? 0.04f + r.Next() * 0.2f : 0.76f + r.Next() * 0.2f, 0.15f + r.Next() * 0.5f, 0.015f + r.Next() * 0.025f,
               leds[static_cast<int>(r.Next() * 3.99f)], 0.6f, 0.1f);
    c.Blur(std::max(1, w / 200));
    c.Vignette(0.4f);
}

void StudioLight(Canvas& c, int) {
    c.Shade([](float x, float y) {
        const float dx = x - 0.5f, dy = y - 0.45f;
        const float v = (0.62f - 0.2f * y) * (1 - 0.35f * (dx * dx + dy * dy));
        return Rgb{v, v * 0.985f, v * 0.96f};
    });
}

void StudioDark(Canvas& c, int) {
    c.Shade([](float x, float y) {
        const float dx = (x - 0.5f) * 1.4f, dy = y - 0.35f;
        const float spot = std::exp(-(dx * dx + dy * dy) * 4.0f);
        const float v = 0.07f + 0.22f * spot;
        return Rgb{v, v * 1.0f, v * 1.08f};
    });
}

void MinimalRoom(Canvas& c, int w) {
    c.Shade([](float x, float y) {
        const float light = 0.92f - 0.18f * x;  // window light from the left
        if (y > 0.78f) return Rgb{0.70f * light, 0.58f * light, 0.45f * light};  // floor
        return Rgb{0.90f * light, 0.86f * light, 0.80f * light};
    });
    c.Rect(0.16f, 0.22f, 0.38f, 0.50f, {0.30f, 0.28f, 0.26f});  // framed picture
    c.Rect(0.175f, 0.24f, 0.365f, 0.48f, {0.62f, 0.70f, 0.72f});
    c.Rect(0.74f, 0.60f, 0.82f, 0.80f, {0.75f, 0.72f, 0.66f});  // plant
    Lcg r{31};
    for (int i = 0; i < 10; ++i) c.Disk(0.73f + r.Next() * 0.10f, 0.40f + r.Next() * 0.2f, 0.03f + r.Next() * 0.03f, Mix({0.25f, 0.42f, 0.22f}, {0.42f, 0.58f, 0.30f}, r.Next()));
    c.Blur(std::max(2, w / 80));
}

void Nature(Canvas& c, int w) {
    c.VerticalGradient({0.60f, 0.78f, 0.95f}, {0.90f, 0.93f, 0.88f});
    Lcg r{41};
    for (int layer = 0; layer < 3; ++layer) {
        const float base = 0.35f + static_cast<float>(layer) * 0.15f;
        const Rgb dark = Mix({0.18f, 0.35f, 0.15f}, {0.30f, 0.48f, 0.22f}, static_cast<float>(layer) / 2.0f);
        for (int i = 0; i < 24; ++i) c.Disk(r.Next(), base + r.Next() * 0.35f, 0.05f + r.Next() * 0.08f, Mix(dark, {0.55f, 0.70f, 0.30f}, r.Next() * 0.5f));
    }
    c.Rect(0.0f, 0.86f, 1.0f, 1.0f, {0.35f, 0.50f, 0.22f});
    c.Blur(std::max(3, w / 45));
    for (int i = 0; i < 14; ++i) c.Disk(r.Next(), 0.1f + r.Next() * 0.5f, 0.02f + r.Next() * 0.03f, {1.0f, 0.98f, 0.85f}, 0.4f, 0.05f);
    c.Blur(std::max(1, w / 300));
}

void Abstract(Canvas& c, int) {
    c.Shade([](float x, float y) {
        const float a = 0.5f + 0.5f * std::sin(x * 5.0f + std::sin(y * 4.0f) * 1.5f);
        const float b = 0.5f + 0.5f * std::sin(y * 6.0f - x * 2.5f + 1.0f);
        const Rgb c1{0.16f, 0.22f, 0.52f}, c2{0.55f, 0.24f, 0.58f}, c3{0.95f, 0.55f, 0.45f};
        return Mix(Mix(c1, c2, a), c3, b * 0.45f);
    });
}

void Gradient(Canvas& c, int) {
    c.Shade([](float x, float y) { return Mix({0.12f, 0.24f, 0.48f}, {0.46f, 0.30f, 0.58f}, std::clamp(x * 0.7f + y * 0.3f, 0.0f, 1.0f)); });
}

}  // namespace

const std::vector<BuiltinBackground>& BuiltinBackgrounds() { return kBuiltins; }

bool IsBuiltinBackground(std::string_view id) {
    return std::any_of(kBuiltins.begin(), kBuiltins.end(), [&](const BuiltinBackground& b) { return id == b.id; });
}

std::shared_ptr<const BackgroundImage> RenderBuiltinBackground(std::string_view id, int width, int height) {
    if (!IsBuiltinBackground(id) || width < 16 || height < 16) return nullptr;
    Canvas c(width, height);
    if (id == "office") Office(c, width);
    else if (id == "modern-office") ModernOffice(c, width);
    else if (id == "gaming-room") GamingRoom(c, width);
    else if (id == "studio-light") StudioLight(c, width);
    else if (id == "studio-dark") StudioDark(c, width);
    else if (id == "minimal-room") MinimalRoom(c, width);
    else if (id == "nature") Nature(c, width);
    else if (id == "abstract") Abstract(c, width);
    else Gradient(c, width);
    const std::vector<std::uint8_t> rgb = c.ToRgb8();
    return MakeBackgroundImage(rgb.data(), width, height, width * 3, width, height);
}

std::filesystem::path CustomBackgroundFile(const std::filesystem::path& folder, std::string_view name) {
    return folder / (std::string(name) + ".ixbg");
}

void BackgroundSource::Clear() {
    key_.clear();
    image_.reset();
}

std::shared_ptr<const BackgroundImage> BackgroundSource::Resolve(const BackgroundSettings& s, const std::filesystem::path& folder) {
    if (s.mode == BackgroundMode::Replace) {
        const std::string key = "builtin:" + s.builtin;
        if (key != key_ || !image_) {
            image_ = RenderBuiltinBackground(s.builtin);
            key_ = image_ ? key : std::string();
        }
        return image_;
    }
    if (s.mode == BackgroundMode::Custom && IsValidBackgroundName(s.image) && !folder.empty()) {
        const std::filesystem::path file = CustomBackgroundFile(folder, s.image);
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(file, ec);
        const std::string key = "custom:" + s.image;
        if (ec) {
            Clear();
            return nullptr;
        }
        if (key != key_ || t != time_ || !image_) {
            image_.reset();  // release the old picture before loading the new one (peak memory)
            image_ = LoadBackgroundImage(file);
            key_ = image_ ? key : std::string();
            time_ = t;
        }
        return image_;
    }
    Clear();
    return nullptr;
}

}  // namespace ixc::effects
