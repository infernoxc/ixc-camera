#include "profiles/profile.h"

#include "common/strings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace ixc {

namespace {

std::string Fmt(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

// Clamp helpers record a warning whenever the stored value changes.
void ClampD(double& v, double lo, double hi, double fallback, const char* field, std::vector<std::string>& w) {
    if (!std::isfinite(v)) {
        w.push_back(std::string(field) + ": not a finite number, reset to " + Fmt(fallback));
        v = fallback;
        return;
    }
    const double c = std::clamp(v, lo, hi);
    if (c != v) {
        w.push_back(std::string(field) + ": " + Fmt(v) + " is outside " + Fmt(lo) + ".." + Fmt(hi) + ", clamped to " + Fmt(c));
        v = c;
    }
}

template <typename T>
void ClampI(T& v, T lo, T hi, const char* field, std::vector<std::string>& w) {
    const T c = std::clamp(v, lo, hi);
    if (c != v) {
        w.push_back(std::string(field) + ": " + std::to_string(v) + " is outside " + std::to_string(lo) + ".." +
                    std::to_string(hi) + ", clamped to " + std::to_string(c));
        v = c;
    }
}

bool HasControlChars(std::string_view s) {
    return std::any_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; });
}

size_t CodePointCount(std::string_view s) {
    return static_cast<size_t>(std::count_if(s.begin(), s.end(), [](char c) {
        return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    }));
}

bool IsValidHotkeyAction(std::string_view a) {
    if (a.empty() || a.size() > 32) return false;
    return std::all_of(a.begin(), a.end(), [](char c) { return (c >= 'a' && c <= 'z') || c == '.' || c == '_'; });
}

bool IsValidHotkeyKeys(std::string_view k) {
    if (k.empty() || k.size() > 32) return false;
    return std::all_of(k.begin(), k.end(), [](char c) { return c > 0x20 && c < 0x7F; });
}

// ---- JSON field readers ---------------------------------------------------------------------

class Reader {
public:
    Reader(const json::Value& obj, std::string prefix, std::vector<std::string>& warnings)
        : obj_(obj), prefix_(std::move(prefix)), w_(warnings) {}

    void Num(const char* key, double& out) {
        if (const json::Value* v = Get(key)) {
            if (v->IsNumber()) out = v->AsNumber();
            else TypeWarning(key, "a number");
        }
    }

    template <typename T>
    void Int(const char* key, T& out, long long lo, long long hi) {
        if (const json::Value* v = Get(key)) {
            if (!v->IsNumber() || v->AsNumber() != std::floor(v->AsNumber())) { TypeWarning(key, "an integer"); return; }
            const double d = v->AsNumber();
            const double c = std::clamp(d, static_cast<double>(lo), static_cast<double>(hi));
            if (c != d) w_.push_back(Path(key) + ": " + Fmt(d) + " is outside " + std::to_string(lo) + ".." + std::to_string(hi) + ", clamped");
            out = static_cast<T>(c);
        }
    }

    void Bool(const char* key, bool& out) {
        if (const json::Value* v = Get(key)) {
            if (v->IsBool()) out = v->AsBool();
            else TypeWarning(key, "true or false");
        }
    }

    void Str(const char* key, std::string& out) {
        if (const json::Value* v = Get(key)) {
            if (v->IsString()) out = v->AsString();
            else TypeWarning(key, "a string");
        }
    }

    const json::Value* Get(const char* key) const { return obj_.Find(key); }
    std::string Path(const char* key) const { return prefix_.empty() ? key : prefix_ + "." + key; }

    void TypeWarning(const char* key, const char* expected) {
        w_.push_back(Path(key) + ": expected " + expected + ", kept previous/default value");
    }

private:
    const json::Value& obj_;
    std::string prefix_;
    std::vector<std::string>& w_;
};

void ReadImage(const json::Value& o, ImageSettings& im, std::vector<std::string>& w) {
    Reader r(o, "image", w);
    r.Num("brightness", im.brightness);
    r.Num("contrast", im.contrast);
    r.Num("saturation", im.saturation);
    r.Num("gamma", im.gamma);
    r.Num("sharpness", im.sharpness);
    r.Num("temperature", im.temperature);
    r.Num("tint", im.tint);
    r.Num("highlights", im.highlights);
    r.Num("shadows", im.shadows);
    r.Num("exposureEv", im.exposureEv);
    r.Num("denoise", im.denoise);
    r.Num("lowLight", im.lowLight);
}

// Accepts one of `names` (index = enum value); anything else keeps the default with a warning.
template <typename E, size_t N>
void ReadEnum(Reader& r, const char* key, E& out, const char* const (&names)[N]) {
    const json::Value* v = r.Get(key);
    if (!v) return;
    if (v->IsString()) {
        for (size_t i = 0; i < N; ++i) {
            if (v->AsString() == names[i]) { out = static_cast<E>(i); return; }
        }
    }
    r.TypeWarning(key, "one of the documented values");
}

void ReadBackground(const json::Value& o, BackgroundSettings& b, std::vector<std::string>& w) {
    Reader r(o, "background", w);
    static constexpr const char* kModes[] = {"original", "blur", "replace", "color", "custom"};
    static constexpr const char* kLevels[] = {"low", "medium", "high"};
    static constexpr const char* kFits[] = {"fill", "fit"};
    ReadEnum(r, "mode", b.mode, kModes);
    ReadEnum(r, "blur", b.blur, kLevels);
    if (!r.Get("strength")) b.strength = b.blur == BlurLevel::Low ? 30 : b.blur == BlurLevel::High ? 85 : 55;  // 0.12 profile
    static constexpr const char* kPresets[] = {"soft", "standard", "dslr", "strong", "custom"};
    static constexpr const char* kStyles[] = {"standard", "bokeh"};
    ReadEnum(r, "preset", b.preset, kPresets);
    ReadEnum(r, "style", b.style, kStyles);
    r.Num("strength", b.strength);
    r.Num("falloff", b.falloff);
    r.Num("feather", b.feather);
    r.Num("edgeProtection", b.edgeProtection);
    r.Num("temporal", b.temporal);
    r.Num("hair", b.hair);
    ReadEnum(r, "fit", b.fit, kFits);
    r.Str("builtin", b.builtin);
    r.Str("image", b.image);
    r.Int("color", b.color, 0, 0x7FFFFFFF);
    r.Num("posX", b.posX);
    r.Num("posY", b.posY);
    r.Num("scale", b.scale);
}

// 0.11 had the background as two effects, and face stickers that 0.12 retires. Profiles keep
// working: the background effects become the background setting (unless the profile already has
// one), the stickers are dropped. Every change is reported, never silent.
void MigrateRetiredEffects(Profile& p, bool hasBackground, std::vector<std::string>& w) {
    std::vector<EffectEntry> kept;
    for (auto& e : p.effects) {
        if (e.id == "background.blur" || e.id == "background.studio") {
            if (!hasBackground && p.background.mode == BackgroundMode::Original) {
                if (e.id == "background.blur") {
                    p.background.mode = BackgroundMode::Blur;
                    p.background.blur = e.strength < 40 ? BlurLevel::Low : e.strength < 75 ? BlurLevel::Medium : BlurLevel::High;
                    p.background.strength = p.background.blur == BlurLevel::Low ? 30 : p.background.blur == BlurLevel::High ? 85 : 55;
                } else {
                    p.background.mode = BackgroundMode::Replace;
                    p.background.builtin = "studio-light";
                }
                w.push_back("effects: \"" + e.id + "\" moved to the new Background setting");
            } else {
                w.push_back("effects: \"" + e.id + "\" dropped (the Background setting replaces it)");
            }
            continue;
        }
        if (e.id.rfind("sticker.", 0) == 0) {
            w.push_back("effects: \"" + e.id + "\" removed (face stickers were retired in 0.12)");
            continue;
        }
        kept.push_back(std::move(e));
    }
    p.effects = std::move(kept);
}

// Hook for future schema changes: each step upgrades a document by exactly one version.
bool MigrateToCurrent(json::Value& /*doc*/, int& version, std::string& error) {
    if (version == kProfileSchemaVersion) return true;
    // No older schema versions exist yet.
    error = "unsupported profile schemaVersion " + std::to_string(version);
    return false;
}

}  // namespace

std::string_view ToString(PerformanceTier t) {
    switch (t) {
        case PerformanceTier::Auto: return "auto";
        case PerformanceTier::UltraLow: return "ultraLow";
        case PerformanceTier::Low: return "low";
        case PerformanceTier::Balanced: return "balanced";
        case PerformanceTier::High: return "high";
    }
    return "auto";
}

bool ParsePerformanceTier(std::string_view s, PerformanceTier& out) {
    for (auto t : {PerformanceTier::Auto, PerformanceTier::UltraLow, PerformanceTier::Low, PerformanceTier::Balanced,
                   PerformanceTier::High}) {
        if (s == ToString(t)) { out = t; return true; }
    }
    return false;
}

void ApplyBlurPreset(BackgroundSettings& b, BlurPreset preset) {
    b.preset = preset;
    struct V {
        double strength;
        BlurStyle style;
        double falloff;
    };
    static constexpr V kValues[] = {{30, BlurStyle::Standard, 40}, {55, BlurStyle::Standard, 50}, {65, BlurStyle::Bokeh, 65}, {85, BlurStyle::Bokeh, 75}};
    if (preset == BlurPreset::Custom) return;
    const V& v = kValues[static_cast<size_t>(preset)];
    b.strength = v.strength;
    b.style = v.style;
    b.falloff = v.falloff;
    b.blur = v.strength < 40 ? BlurLevel::Low : v.strength < 75 ? BlurLevel::Medium : BlurLevel::High;
}

std::string_view ToString(ProcessingMode m) {
    switch (m) {
        case ProcessingMode::Auto: return "auto";
        case ProcessingMode::Cpu: return "cpu";
        case ProcessingMode::Gpu: return "gpu";
    }
    return "auto";
}

std::string_view ToString(AntiFlicker a) {
    switch (a) {
        case AntiFlicker::Auto: return "auto";
        case AntiFlicker::Hz50: return "50hz";
        case AntiFlicker::Hz60: return "60hz";
        case AntiFlicker::Off: return "off";
    }
    return "auto";
}

std::string_view ToString(BackgroundMode m) {
    switch (m) {
        case BackgroundMode::Original: return "original";
        case BackgroundMode::Blur: return "blur";
        case BackgroundMode::Replace: return "replace";
        case BackgroundMode::Color: return "color";
        case BackgroundMode::Custom: return "custom";
    }
    return "original";
}

std::string_view ToString(BlurLevel b) {
    switch (b) {
        case BlurLevel::Low: return "low";
        case BlurLevel::Medium: return "medium";
        case BlurLevel::High: return "high";
    }
    return "medium";
}

bool IsValidBackgroundName(std::string_view name) {
    if (name.empty() || name.size() > kMaxBackgroundNameChars || name.front() == '-') return false;
    return std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

bool IsValidEffectId(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    if (id.front() == '.' || id.find("..") != std::string_view::npos) return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}

bool IsValidProfileName(std::string_view name) {
    if (name.empty() || !IsValidUtf8(name) || HasControlChars(name)) return false;
    if (CodePointCount(name) > kMaxProfileNameChars) return false;
    return name.front() != ' ' && name.back() != ' ';
}

std::vector<std::string> Validate(Profile& p) {
    std::vector<std::string> w;

    if (!IsValidProfileName(p.name)) {
        w.push_back("name: invalid profile name, renamed to \"Unnamed profile\"");
        p.name = "Unnamed profile";
    }
    if (p.sourceCameraId.size() > 512 || HasControlChars(p.sourceCameraId) || !IsValidUtf8(p.sourceCameraId)) {
        w.push_back("sourceCameraId: invalid camera identifier, cleared (first available camera will be used)");
        p.sourceCameraId.clear();
    }

    ClampI<std::uint32_t>(p.width, 16, 7680, "width", w);
    ClampI<std::uint32_t>(p.height, 16, 4320, "height", w);
    ClampI<std::uint32_t>(p.fpsNumerator, 1, 240000, "fpsNumerator", w);
    ClampI<std::uint32_t>(p.fpsDenominator, 1, 1001, "fpsDenominator", w);
    if (static_cast<double>(p.fpsNumerator) / p.fpsDenominator > 240.0) {
        w.push_back("fps: frame rate above 240 FPS, reset to 30");
        p.fpsNumerator = 30;
        p.fpsDenominator = 1;
    }

    ClampD(p.zoom, 1.0, 4.0, 1.0, "zoom", w);

    CropRect& c = p.crop;
    ClampD(c.x, 0.0, 0.95, 0.0, "crop.x", w);
    ClampD(c.y, 0.0, 0.95, 0.0, "crop.y", w);
    ClampD(c.width, 0.05, 1.0, 1.0, "crop.width", w);
    ClampD(c.height, 0.05, 1.0, 1.0, "crop.height", w);
    if (c.x + c.width > 1.0) { c.width = 1.0 - c.x; w.push_back("crop: extends past the right edge, width reduced"); }
    if (c.y + c.height > 1.0) { c.height = 1.0 - c.y; w.push_back("crop: extends past the bottom edge, height reduced"); }

    ImageSettings& im = p.image;
    ClampD(im.brightness, -100, 100, 0, "image.brightness", w);
    ClampD(im.contrast, -100, 100, 0, "image.contrast", w);
    ClampD(im.saturation, -100, 100, 0, "image.saturation", w);
    ClampD(im.gamma, 0.2, 3.0, 1.0, "image.gamma", w);
    ClampD(im.sharpness, 0, 100, 15, "image.sharpness", w);
    ClampD(im.temperature, -100, 100, 0, "image.temperature", w);
    ClampD(im.tint, -100, 100, 0, "image.tint", w);
    ClampD(im.highlights, -100, 100, 0, "image.highlights", w);
    ClampD(im.shadows, -100, 100, 0, "image.shadows", w);
    ClampD(im.exposureEv, -2, 2, 0, "image.exposureEv", w);
    ClampD(im.denoise, 0, 100, 0, "image.denoise", w);
    ClampD(im.lowLight, 0, 100, 0, "image.lowLight", w);

    std::vector<EffectEntry> effects;
    for (auto& e : p.effects) {
        if (!IsValidEffectId(e.id)) { w.push_back("effects: invalid effect id dropped"); continue; }
        if (std::any_of(effects.begin(), effects.end(), [&](const EffectEntry& x) { return x.id == e.id; })) {
            w.push_back("effects: duplicate effect \"" + e.id + "\" dropped");
            continue;
        }
        if (effects.size() >= kMaxEnabledEffects) {
            w.push_back("effects: more than " + std::to_string(kMaxEnabledEffects) + " effects, extra entries dropped");
            break;
        }
        ClampD(e.strength, 0, 100, 100, "effects.strength", w);
        effects.push_back(std::move(e));
    }
    p.effects = std::move(effects);

    BackgroundSettings& bg = p.background;
    if (!IsValidBackgroundName(bg.builtin)) {
        w.push_back("background.builtin: invalid name, reset to \"studio-light\"");
        bg.builtin = "studio-light";
    }
    if (!bg.image.empty() && !IsValidBackgroundName(bg.image)) {
        w.push_back("background.image: invalid name, cleared");
        bg.image.clear();
    }
    if (bg.mode == BackgroundMode::Custom && bg.image.empty()) {
        w.push_back("background: custom mode without an image, switched to blur");
        bg.mode = BackgroundMode::Blur;
    }
    if (bg.color > 0xFFFFFFu) {
        w.push_back("background.color: not an RGB colour, reset");
        bg.color = 0x3A4A5C;
    }
    ClampD(bg.posX, 0, 1, 0.5, "background.posX", w);
    ClampD(bg.posY, 0, 1, 0.5, "background.posY", w);
    ClampD(bg.scale, 1, 3, 1, "background.scale", w);
    ClampD(bg.strength, 0, 100, 55, "background.strength", w);
    ClampD(bg.falloff, 0, 100, 50, "background.falloff", w);
    ClampD(bg.feather, 0, 100, 35, "background.feather", w);
    ClampD(bg.edgeProtection, 0, 100, 60, "background.edgeProtection", w);
    ClampD(bg.temporal, 0, 100, 50, "background.temporal", w);
    ClampD(bg.hair, 0, 100, 50, "background.hair", w);

    ClampI(p.faceTracking.maxFaces, 1, kMaxTrackedFaces, "faceTracking.maxFaces", w);
    ClampI(p.faceTracking.detectionIntervalFrames, 0, 120, "faceTracking.detectionIntervalFrames", w);

    std::vector<HotkeyBinding> hotkeys;
    for (auto& h : p.hotkeys) {
        if (!IsValidHotkeyAction(h.action) || !IsValidHotkeyKeys(h.keys)) { w.push_back("hotkeys: invalid binding dropped"); continue; }
        if (hotkeys.size() >= 32) { w.push_back("hotkeys: more than 32 bindings, extra entries dropped"); break; }
        hotkeys.push_back(std::move(h));
    }
    p.hotkeys = std::move(hotkeys);

    return w;
}

ProfileLoadResult ProfileFromJson(std::string_view text) {
    ProfileLoadResult result;
    json::ParseLimits limits;
    limits.maxInputBytes = 256 * 1024;  // a profile is a few KB; anything larger is not a profile
    limits.maxDepth = 8;
    json::ParseResult parsed = json::Parse(text, limits);
    if (!parsed) {
        result.error = "not a valid profile file (" + parsed.error.message + " at byte " + std::to_string(parsed.error.offset) + ")";
        return result;
    }
    json::Value& doc = *parsed.value;
    if (!doc.IsObject()) { result.error = "profile must be a JSON object"; return result; }

    const json::Value* ver = doc.Find("schemaVersion");
    if (!ver || !ver->IsNumber() || ver->AsNumber() != std::floor(ver->AsNumber()) || ver->AsNumber() < 0 ||
        ver->AsNumber() > 1'000'000) {
        result.error = "profile has no valid schemaVersion";
        return result;
    }
    int version = static_cast<int>(ver->AsNumber());
    if (version > kProfileSchemaVersion) {
        result.error = "profile was created by a newer version of IXC Camera (schemaVersion " + std::to_string(version) +
                       "); update IXC Camera to open it";
        return result;
    }
    if (!MigrateToCurrent(doc, version, result.error)) return result;

    Profile& p = result.profile;
    auto& w = result.warnings;
    Reader r(doc, "", w);

    static constexpr const char* kKnown[] = {"schemaVersion", "name", "sourceCameraId", "width", "height", "fpsNumerator",
                                             "fpsDenominator", "mirror", "zoom", "crop", "image", "effects",
                                             "faceTracking", "performanceTier", "gpu", "processing", "smoothMotion", "autoFraming", "effectsEnabled",
                                             "hotkeys", "background", "antiFlicker"};
    for (const auto& [k, v] : doc.AsObject()) {
        if (std::find(std::begin(kKnown), std::end(kKnown), k) == std::end(kKnown)) {
            w.push_back("unknown field \"" + k.substr(0, 64) + "\" ignored");
        }
    }

    r.Str("name", p.name);
    r.Str("sourceCameraId", p.sourceCameraId);
    r.Int("width", p.width, 0, 100000);
    r.Int("height", p.height, 0, 100000);
    r.Int("fpsNumerator", p.fpsNumerator, 0, 10'000'000);
    r.Int("fpsDenominator", p.fpsDenominator, 0, 10'000'000);
    r.Bool("mirror", p.mirror);
    r.Bool("smoothMotion", p.smoothMotion);
    {
        static constexpr const char* kFlicker[] = {"auto", "50hz", "60hz", "off"};
        ReadEnum(r, "antiFlicker", p.antiFlicker, kFlicker);
    }
    r.Bool("autoFraming", p.autoFraming);
    r.Bool("effectsEnabled", p.effectsEnabled);
    r.Num("zoom", p.zoom);

    if (const json::Value* c = r.Get("crop")) {
        if (c->IsObject()) {
            Reader cr(*c, "crop", w);
            cr.Num("x", p.crop.x);
            cr.Num("y", p.crop.y);
            cr.Num("width", p.crop.width);
            cr.Num("height", p.crop.height);
        } else {
            r.TypeWarning("crop", "an object");
        }
    }

    if (const json::Value* im = r.Get("image")) {
        if (im->IsObject()) ReadImage(*im, p.image, w);
        else r.TypeWarning("image", "an object");
    }

    if (const json::Value* fx = r.Get("effects")) {
        if (fx->IsArray()) {
            for (const auto& e : fx->AsArray()) {
                if (!e.IsObject()) { w.push_back("effects: non-object entry dropped"); continue; }
                EffectEntry entry;
                Reader er(e, "effects[]", w);
                er.Str("id", entry.id);
                er.Num("strength", entry.strength);
                p.effects.push_back(std::move(entry));
                if (p.effects.size() > kMaxEnabledEffects * 4) break;  // bound work on hostile input
            }
        } else {
            r.TypeWarning("effects", "an array");
        }
    }

    bool hasBackground = false;
    if (const json::Value* b = r.Get("background")) {
        if (b->IsObject()) {
            hasBackground = true;
            ReadBackground(*b, p.background, w);
        } else {
            r.TypeWarning("background", "an object");
        }
    }
    MigrateRetiredEffects(p, hasBackground, w);

    if (const json::Value* ft = r.Get("faceTracking")) {
        if (ft->IsObject()) {
            Reader fr(*ft, "faceTracking", w);
            fr.Bool("enabled", p.faceTracking.enabled);
            fr.Int("maxFaces", p.faceTracking.maxFaces, 0, 1000);
            fr.Int("detectionIntervalFrames", p.faceTracking.detectionIntervalFrames, 0, 100000);
        } else {
            r.TypeWarning("faceTracking", "an object");
        }
    }

    if (const json::Value* t = r.Get("performanceTier")) {
        if (!t->IsString() || !ParsePerformanceTier(t->AsString(), p.tier)) {
            w.push_back("performanceTier: unknown value, using \"auto\"");
            p.tier = PerformanceTier::Auto;
        }
    }

    if (const json::Value* g = r.Get("gpu"); g && !r.Get("processing")) {  // 0.11: "auto" | "off"
        if (g->IsString() && g->AsString() == "off") p.processing = ProcessingMode::Cpu;
        else if (g->IsString() && g->AsString() == "auto") p.processing = ProcessingMode::Auto;
        else w.push_back("gpu: expected \"auto\" or \"off\", using \"auto\"");
    }
    {
        static constexpr const char* kModes[] = {"auto", "cpu", "gpu"};
        ReadEnum(r, "processing", p.processing, kModes);
    }

    if (const json::Value* hk = r.Get("hotkeys")) {
        if (hk->IsArray()) {
            for (const auto& h : hk->AsArray()) {
                if (!h.IsObject()) { w.push_back("hotkeys: non-object entry dropped"); continue; }
                HotkeyBinding b;
                Reader hr(h, "hotkeys[]", w);
                hr.Str("action", b.action);
                hr.Str("keys", b.keys);
                p.hotkeys.push_back(std::move(b));
                if (p.hotkeys.size() > 128) break;
            }
        } else {
            r.TypeWarning("hotkeys", "an array");
        }
    }

    auto clampWarnings = Validate(p);
    w.insert(w.end(), clampWarnings.begin(), clampWarnings.end());
    result.ok = true;
    return result;
}

std::string ProfileToJson(const Profile& p) {
    using json::Value;
    json::Object image = {
        {"brightness", p.image.brightness}, {"contrast", p.image.contrast},   {"saturation", p.image.saturation},
        {"gamma", p.image.gamma},           {"sharpness", p.image.sharpness}, {"temperature", p.image.temperature},
        {"tint", p.image.tint},             {"highlights", p.image.highlights}, {"shadows", p.image.shadows},
        {"exposureEv", p.image.exposureEv}, {"denoise", p.image.denoise},     {"lowLight", p.image.lowLight},
    };
    json::Array effects;
    for (const auto& e : p.effects) effects.emplace_back(json::Object{{"id", e.id}, {"strength", e.strength}});
    json::Array hotkeys;
    for (const auto& h : p.hotkeys) hotkeys.emplace_back(json::Object{{"action", h.action}, {"keys", h.keys}});

    json::Object doc = {
        {"schemaVersion", kProfileSchemaVersion},
        {"name", p.name},
        {"sourceCameraId", p.sourceCameraId},
        {"width", static_cast<double>(p.width)},
        {"height", static_cast<double>(p.height)},
        {"fpsNumerator", static_cast<double>(p.fpsNumerator)},
        {"fpsDenominator", static_cast<double>(p.fpsDenominator)},
        {"mirror", p.mirror},
        {"zoom", p.zoom},
        {"crop", json::Object{{"x", p.crop.x}, {"y", p.crop.y}, {"width", p.crop.width}, {"height", p.crop.height}}},
        {"image", std::move(image)},
        {"effects", std::move(effects)},
        {"background", json::Object{{"mode", std::string(ToString(p.background.mode))},
                                    {"blur", std::string(ToString(p.background.blur))},
                                    {"preset", std::string(std::array<const char*, 5>{"soft", "standard", "dslr", "strong", "custom"}[static_cast<size_t>(p.background.preset)])},
                                    {"style", p.background.style == BlurStyle::Bokeh ? "bokeh" : "standard"},
                                    {"strength", p.background.strength},
                                    {"falloff", p.background.falloff},
                                    {"feather", p.background.feather},
                                    {"edgeProtection", p.background.edgeProtection},
                                    {"temporal", p.background.temporal},
                                    {"hair", p.background.hair},
                                    {"builtin", p.background.builtin},
                                    {"color", static_cast<double>(p.background.color)},
                                    {"image", p.background.image},
                                    {"fit", p.background.fit == BackgroundFit::Fit ? "fit" : "fill"},
                                    {"posX", p.background.posX},
                                    {"posY", p.background.posY},
                                    {"scale", p.background.scale}}},
        {"faceTracking", json::Object{{"enabled", p.faceTracking.enabled},
                                      {"maxFaces", p.faceTracking.maxFaces},
                                      {"detectionIntervalFrames", p.faceTracking.detectionIntervalFrames}}},
        {"performanceTier", std::string(ToString(p.tier))},
        {"processing", std::string(ToString(p.processing))},
        {"smoothMotion", p.smoothMotion},
        {"antiFlicker", std::string(ToString(p.antiFlicker))},
        {"autoFraming", p.autoFraming},
        {"effectsEnabled", p.effectsEnabled},
        {"hotkeys", std::move(hotkeys)},
    };
    return json::Serialize(Value(std::move(doc)));
}

}  // namespace ixc
