#include "common/fileio.h"
#include "ixc_test.h"
#include "profiles/profile.h"
#include "profiles/profile_store.h"
#include "test_util.h"

#include <algorithm>
#include <string>

using namespace ixc;

namespace {

bool HasWarningContaining(const ProfileLoadResult& r, const std::string& needle) {
    return std::any_of(r.warnings.begin(), r.warnings.end(),
                       [&](const std::string& w) { return w.find(needle) != std::string::npos; });
}

Profile SampleProfile() {
    Profile p;
    p.name = "Stream \xE2\x80\x94 evening";  // em dash: non-ASCII names must survive
    p.sourceCameraId = "\\\\?\\usb#vid_17ef&pid_4831&mi_00#7&36250dec&0&0000#{e5323777-f976-4f5b-9b55-b94699c46e44}\\global";
    p.width = 1920;
    p.height = 1080;
    p.fpsNumerator = 30000;
    p.fpsDenominator = 1001;
    p.mirror = true;
    p.zoom = 1.25;
    p.crop = {0.1, 0.05, 0.8, 0.9};
    p.image.brightness = 5;
    p.image.gamma = 1.1;
    p.image.temperature = -12.5;
    p.effects = {{"ixc.blush-tone", 40}, {"ixc.color.warm", 100}};
    p.faceTracking = {true, 2, 6};
    p.tier = PerformanceTier::UltraLow;
    p.gpu = GpuMode::Off;
    p.smoothMotion = false;
    p.antiFlicker = AntiFlicker::Hz60;
    p.autoFraming = true;
    p.background.mode = BackgroundMode::Replace;
    p.background.builtin = "nature";
    p.background.fit = BackgroundFit::Fit;
    p.background.posY = 0.2;
    p.hotkeys = {{"effects.toggle", "Ctrl+Alt+F8"}};
    return p;
}

}  // namespace

IXC_TEST(Profile_RoundTripPreservesEverything) {
    const Profile original = SampleProfile();
    const std::string text = ProfileToJson(original);
    ProfileLoadResult r = ProfileFromJson(text);
    IXC_REQUIRE(r.ok);
    IXC_CHECK(r.warnings.empty());
    IXC_CHECK(r.profile == original);
    IXC_CHECK_EQ(ProfileToJson(r.profile), text);
}

IXC_TEST(Profile_DefaultIsValidWithoutWarnings) {
    Profile p;
    IXC_CHECK(Validate(p).empty());
}

IXC_TEST(Profile_ClampsOutOfRangeValuesAndReportsThem) {
    ProfileLoadResult r = ProfileFromJson(R"({
        "schemaVersion": 1, "name": "x",
        "width": 99999, "height": 1, "zoom": 50,
        "crop": {"x": 0.9, "y": 0, "width": 0.5, "height": 1},
        "image": {"brightness": 500, "gamma": 0, "sharpness": -3},
        "faceTracking": {"maxFaces": 40},
        "fpsNumerator": 60, "fpsDenominator": 1
    })");
    IXC_REQUIRE(r.ok);
    IXC_CHECK_EQ(r.profile.width, 7680u);
    IXC_CHECK_EQ(r.profile.height, 16u);
    IXC_CHECK_EQ(r.profile.zoom, 4.0);
    IXC_CHECK_EQ(r.profile.image.brightness, 100.0);
    IXC_CHECK_EQ(r.profile.image.gamma, 0.2);
    IXC_CHECK_EQ(r.profile.image.sharpness, 0.0);
    IXC_CHECK_EQ(r.profile.faceTracking.maxFaces, kMaxTrackedFaces);
    IXC_CHECK(r.profile.crop.x + r.profile.crop.width <= 1.0 + 1e-12);
    IXC_CHECK(HasWarningContaining(r, "image.brightness"));
    IXC_CHECK(HasWarningContaining(r, "crop"));
    IXC_CHECK(HasWarningContaining(r, "zoom"));
}

IXC_TEST(Profile_GpuModeDefaultsToAutoAndRejectsUnknown) {
    ProfileLoadResult r = ProfileFromJson(R"({"schemaVersion": 1})");  // profile saved before the field existed
    IXC_REQUIRE(r.ok);
    IXC_CHECK(r.profile.gpu == GpuMode::Auto);
    r = ProfileFromJson(R"({"schemaVersion": 1, "gpu": "turbo"})");
    IXC_REQUIRE(r.ok);
    IXC_CHECK(r.profile.gpu == GpuMode::Auto);
    IXC_CHECK(HasWarningContaining(r, "gpu"));
    r = ProfileFromJson(R"({"schemaVersion": 1, "gpu": "off"})");
    IXC_REQUIRE(r.ok);
    IXC_CHECK(r.profile.gpu == GpuMode::Off);
}

IXC_TEST(Profile_RejectsNewerSchema) {
    ProfileLoadResult r = ProfileFromJson(R"({"schemaVersion": 2, "name": "future"})");
    IXC_CHECK(!r.ok);
    IXC_CHECK(r.error.find("newer version") != std::string::npos);
}

IXC_TEST(Profile_RejectsMissingOrBogusSchema) {
    IXC_CHECK(!ProfileFromJson(R"({"name": "x"})").ok);
    IXC_CHECK(!ProfileFromJson(R"({"schemaVersion": "1"})").ok);
    IXC_CHECK(!ProfileFromJson(R"({"schemaVersion": 1.5})").ok);
    IXC_CHECK(!ProfileFromJson(R"({"schemaVersion": -1})").ok);
    IXC_CHECK(!ProfileFromJson(R"({"schemaVersion": 0})").ok);
    IXC_CHECK(!ProfileFromJson("[1,2,3]").ok);
    IXC_CHECK(!ProfileFromJson("not json").ok);
}

IXC_TEST(Profile_WrongTypesKeepDefaultsWithWarnings) {
    ProfileLoadResult r = ProfileFromJson(R"({
        "schemaVersion": 1, "name": 7, "mirror": "yes", "width": 12.5,
        "image": [1,2], "effects": {"id": "x"}, "performanceTier": "turbo", "extra": 1
    })");
    IXC_REQUIRE(r.ok);
    const Profile def;
    IXC_CHECK_EQ(r.profile.name, def.name);
    IXC_CHECK_EQ(r.profile.mirror, def.mirror);
    IXC_CHECK_EQ(r.profile.width, def.width);
    IXC_CHECK(r.profile.image == def.image);
    IXC_CHECK(r.profile.effects.empty());
    IXC_CHECK(r.profile.tier == PerformanceTier::Auto);
    IXC_CHECK(HasWarningContaining(r, "unknown field \"extra\""));
    IXC_CHECK(HasWarningContaining(r, "performanceTier"));
    IXC_CHECK(HasWarningContaining(r, "mirror"));
}

IXC_TEST(Profile_RejectsPathLikeEffectIds) {
    ProfileLoadResult r = ProfileFromJson(R"({
        "schemaVersion": 1,
        "effects": [
            {"id": "../../windows/system32/evil"},
            {"id": "..\\evil"},
            {"id": "C:/evil"},
            {"id": ".hidden"},
            {"id": "a..b"},
            {"id": ""},
            {"id": "UPPER"},
            {"id": "ok.effect-1", "strength": 250},
            {"id": "ok.effect-1"}
        ]
    })");
    IXC_REQUIRE(r.ok);
    IXC_REQUIRE(r.profile.effects.size() == 1);
    IXC_CHECK_EQ(r.profile.effects[0].id, std::string("ok.effect-1"));
    IXC_CHECK_EQ(r.profile.effects[0].strength, 100.0);
    IXC_CHECK(HasWarningContaining(r, "duplicate"));
}

IXC_TEST(Profile_CapsEffectAndHotkeyCounts) {
    std::string doc = R"({"schemaVersion": 1, "effects": [)";
    for (int i = 0; i < 40; ++i) doc += (i ? "," : "") + std::string(R"({"id": "fx)") + std::to_string(i) + "\"}";
    doc += "]}";
    ProfileLoadResult r = ProfileFromJson(doc);
    IXC_REQUIRE(r.ok);
    IXC_CHECK_EQ(r.profile.effects.size(), kMaxEnabledEffects);
}

IXC_TEST(Profile_NameValidation) {
    IXC_CHECK(IsValidProfileName("Gaming"));
    IXC_CHECK(IsValidProfileName("\xE6\x97\xA5\xE6\x9C\xAC"));  // CJK
    IXC_CHECK(!IsValidProfileName(""));
    IXC_CHECK(!IsValidProfileName(" leading"));
    IXC_CHECK(!IsValidProfileName("trailing "));
    IXC_CHECK(!IsValidProfileName("tab\there"));
    IXC_CHECK(!IsValidProfileName(std::string(65, 'a')));
    IXC_CHECK(IsValidProfileName(std::string(64, 'a')));
    IXC_CHECK(!IsValidProfileName("\xFF"));
}

IXC_TEST(Profile_RejectsOversizedDocument) {
    std::string doc = R"({"schemaVersion": 1, "name": ")" + std::string(300 * 1024, 'a') + "\"}";
    IXC_CHECK(!ProfileFromJson(doc).ok);
}

IXC_TEST(ProfileStore_MakeStemIsSafe) {
    IXC_CHECK_EQ(ProfileStore::MakeStem("My Stream Profile!"), std::string("my-stream-profile"));
    IXC_CHECK_EQ(ProfileStore::MakeStem("../../etc/passwd"), std::string("etc-passwd"));
    IXC_CHECK_EQ(ProfileStore::MakeStem("CON"), std::string("profile-con"));
    IXC_CHECK_EQ(ProfileStore::MakeStem("\xE6\x97\xA5\xE6\x9C\xAC"), std::string("profile"));
    IXC_CHECK(ProfileStore::IsValidStem(ProfileStore::MakeStem(std::string(500, 'x'))));
    IXC_CHECK(!ProfileStore::IsValidStem("..\\x"));
    IXC_CHECK(!ProfileStore::IsValidStem("nul"));
    IXC_CHECK(!ProfileStore::IsValidStem("a/b"));
}

IXC_TEST(ProfileStore_SaveLoadListRemove) {
    test::TempDir dir;
    ProfileStore store(dir.path() / L"profiles");

    const Profile p = SampleProfile();
    std::string stem;
    IXC_REQUIRE(SUCCEEDED(store.Save(p, stem)));
    IXC_CHECK_EQ(stem, std::string("stream-evening"));

    auto listed = store.List();
    IXC_REQUIRE(listed.size() == 1);
    IXC_CHECK_EQ(listed[0].stem, stem);

    ProfileLoadResult r = store.Load(stem);
    IXC_REQUIRE(r.ok);
    IXC_CHECK(r.profile == p);

    IXC_CHECK(!store.Load("../escape").ok);
    IXC_CHECK(!store.Load("missing").ok);

    IXC_CHECK(SUCCEEDED(store.Remove(stem)));
    IXC_CHECK(store.List().empty());
}

IXC_TEST(ProfileStore_SaveClampsInvalidValues) {
    test::TempDir dir;
    ProfileStore store(dir.path());
    Profile p;
    p.image.brightness = 1e9;
    std::string stem = "clamped";
    IXC_REQUIRE(SUCCEEDED(store.Save(p, stem)));
    ProfileLoadResult r = store.Load(stem);
    IXC_REQUIRE(r.ok);
    IXC_CHECK(r.warnings.empty());
    IXC_CHECK_EQ(r.profile.image.brightness, 100.0);
}

IXC_TEST(ProfileStore_CorruptFileReportsErrorAndIsNotOverwritten) {
    test::TempDir dir;
    ProfileStore store(dir.path());
    IXC_REQUIRE(SUCCEEDED(WriteFileAtomic(dir.path() / L"broken.json", "{\"schemaVersion\": 1,")));
    ProfileLoadResult r = store.Load("broken");
    IXC_CHECK(!r.ok);
    IXC_CHECK(!r.error.empty());
    std::string text;
    IXC_REQUIRE(SUCCEEDED(ReadFileLimited(dir.path() / L"broken.json", 1024, text)));
    IXC_CHECK_EQ(text, std::string("{\"schemaVersion\": 1,"));
}

#include "profiles/app_settings.h"

IXC_TEST(AppSettings_RoundTripAndLenientParsing) {
    ixc::AppSettings s;
    s.activeProfile = "evening-call";
    s.hotkeysEnabled = false;
    s.showFaceMarkers = false;
    IXC_CHECK(ixc::AppSettingsFromJson(ixc::AppSettingsToJson(s)) == s);
    IXC_CHECK(ixc::AppSettingsFromJson("not json") == ixc::AppSettings{});                         // damaged: defaults
    IXC_CHECK(ixc::AppSettingsFromJson(R"({"activeProfile": "..\evil"})").activeProfile == "default");  // never a path
    IXC_CHECK(ixc::AppSettingsFromJson(R"({"hotkeysEnabled": "yes"})").hotkeysEnabled);             // wrong type: default
}
