#include "common/sha256.h"
#include "common/update_check.h"
#include "ixc_test.h"
#include "profiles/settings_sync.h"

#include <string>

using namespace ixc;

IXC_TEST(Update_Sha256StandardVectors) {
    IXC_CHECK_EQ(Sha256::Hex("", 0), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    IXC_CHECK_EQ(Sha256::Hex("abc", 3), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";  // 448 bits: padding spills a block
    IXC_CHECK_EQ(Sha256::Hex(two.data(), two.size()), std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    const std::string million(1000000, 'a');
    Sha256 s;  // streamed in uneven pieces
    for (size_t i = 0; i < million.size(); i += 777) s.Update(million.data() + i, std::min<size_t>(777, million.size() - i));
    const auto d = s.Final();
    IXC_CHECK(d[0] == 0xcd && d[1] == 0xc7 && d[31] == 0xd0);  // cdc76e5c...c7112cd0
}

IXC_TEST(Update_SemVerComparesNumerically) {
    IXC_CHECK(ParseSemVer("v0.14.0").ok && ParseSemVer("0.14.0").ok);
    IXC_CHECK(CompareSemVer(ParseSemVer("0.9.0"), ParseSemVer("0.10.0")) < 0);  // not string order
    IXC_CHECK(CompareSemVer(ParseSemVer("0.14.0"), ParseSemVer("v0.13.9")) > 0);
    IXC_CHECK(CompareSemVer(ParseSemVer("1.0.0"), ParseSemVer("1.0.0")) == 0);
    IXC_CHECK(CompareSemVer(ParseSemVer("1.0.10"), ParseSemVer("1.0.9")) > 0);
    for (const char* bad : {"", "v", "1.2", "1.2.3.4", "1.2.3-beta", "a.b.c", "1..3", "9999999.0.0"}) IXC_CHECK(!ParseSemVer(bad).ok);
}

namespace {
std::string Release(const std::string& tag, const std::string& base, bool prerelease = false) {
    return R"({"tag_name":")" + tag + R"(","name":"IXC Camera","draft":false,"prerelease":)" + (prerelease ? "true" : "false") +
           R"(,"body":"Notes","assets":[{"name":"IXC-Camera-Setup-x64.exe","browser_download_url":")" + base + tag +
           R"(/IXC-Camera-Setup-x64.exe"},{"name":"SHA256SUMS.txt","browser_download_url":")" + base + tag + R"(/SHA256SUMS.txt"}]})";
}
const std::string kBase = "https://github.com/infernoxc/ixc-camera/releases/download/";
}  // namespace

IXC_TEST(Update_ReleaseParsingAcceptsOnlyTheOfficialInstaller) {
    ReleaseInfo r = ParseLatestRelease(Release("v0.15.0", kBase));
    IXC_REQUIRE(r.ok);
    IXC_CHECK_EQ(r.version, std::string("0.15.0"));
    IXC_CHECK_EQ(r.installerUrl, kBase + "v0.15.0/IXC-Camera-Setup-x64.exe");
    IXC_CHECK_EQ(r.checksumUrl, kBase + "v0.15.0/SHA256SUMS.txt");
    IXC_CHECK(!ParseLatestRelease(Release("v0.15.0", kBase, true)).ok);                                              // pre-release
    IXC_CHECK(!ParseLatestRelease(Release("v0.15.0", "https://evil.example/infernoxc/ixc-camera/releases/download/")).ok);  // other host
    IXC_CHECK(!ParseLatestRelease(Release("v0.15.0", "https://github.com/someone/ixc-camera/releases/download/")).ok);   // other repo
    IXC_CHECK(!ParseLatestRelease(Release("v0.15.0", "http://github.com/infernoxc/ixc-camera/releases/download/")).ok);  // not https
    IXC_CHECK(!ParseLatestRelease(Release("0.15.0", kBase)).ok);                                                         // tag format
    IXC_CHECK(!ParseLatestRelease(Release("v0.15.0-rc1", kBase)).ok);
    IXC_CHECK(!ParseLatestRelease("not json").ok);
    // An asset whose URL points at another release's file is ignored (then no installer: rejected).
    std::string mixed = Release("v0.15.0", kBase);
    const auto at = mixed.find("v0.15.0/IXC-Camera-Setup-x64.exe");
    mixed.replace(at, 7, "v0.14.0");
    IXC_CHECK(!ParseLatestRelease(mixed).ok);
}

IXC_TEST(Update_ChecksumLookup) {
    const std::string sums =
        "1111111111111111111111111111111111111111111111111111111111111111  IXC-Camera-0.15.0-source.zip\n"
        "ABCDEFabcdef0123456789abcdef0123456789abcdef0123456789abcdef0123  IXC-Camera-Setup-x64.exe\r\n";
    IXC_CHECK_EQ(FindChecksum(sums, "IXC-Camera-Setup-x64.exe"), std::string("abcdefabcdef0123456789abcdef0123456789abcdef0123456789abcdef0123"));
    IXC_CHECK(FindChecksum(sums, "IXC-Camera-Setup-x64").empty());       // exact name only
    IXC_CHECK(FindChecksum(sums, "missing.exe").empty());
    IXC_CHECK(FindChecksum("zz  IXC-Camera-Setup-x64.exe\n", "IXC-Camera-Setup-x64.exe").empty());
    // End to end: a buffer verifies against its own listed digest, and a modified one doesn't.
    const std::string payload = "installer bytes";
    const std::string list = Sha256::Hex(payload.data(), payload.size()) + "  IXC-Camera-Setup-x64.exe\n";
    IXC_CHECK_EQ(FindChecksum(list, "IXC-Camera-Setup-x64.exe"), Sha256::Hex(payload.data(), payload.size()));
    const std::string tampered = payload + "!";
    IXC_CHECK(FindChecksum(list, "IXC-Camera-Setup-x64.exe") != Sha256::Hex(tampered.data(), tampered.size()));
}

IXC_TEST(Settings_PublishCoalescesAndNeverLosesTheLastValue) {
    SettingsSync s;
    IXC_CHECK(s.OnChange());   // first change of a drag: arm the publish timer
    IXC_CHECK(!s.OnChange());  // further moves ride on it (no timer storm)
    IXC_CHECK(!s.OnChange());
    IXC_CHECK(s.PublishArmed() && s.SavePending());
    s.OnPublished();           // timer fired: the current (newest) profile went out
    IXC_CHECK(!s.PublishArmed() && s.SavePending());  // the save still waits for the control to settle
    IXC_CHECK(s.OnChange());   // a move after the publish arms it again: the final value is sent too
    s.OnPublished();
    s.OnSaved();
    IXC_CHECK(!s.PublishArmed() && !s.SavePending());
}

IXC_TEST(Settings_WheelLatching) {
    IXC_CHECK(WheelAdjustsSlider(10'000, 0, false));       // panel never scrolled: the slider takes the wheel
    IXC_CHECK(!WheelAdjustsSlider(10'000, 9'800, false));  // the panel is being scrolled: keep scrolling past
    IXC_CHECK(WheelAdjustsSlider(10'000, 9'000, false));   // scrolling ended a while ago
    IXC_CHECK(!WheelAdjustsSlider(10'000, 0, true));       // Shift: always scroll the panel
}
