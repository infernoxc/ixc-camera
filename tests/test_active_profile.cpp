#include "common/fileio.h"
#include "ixc_test.h"
#include "profiles/active_profile.h"
#include "test_util.h"

#include <windows.h>

#include <string>

using namespace ixc;

namespace {

// Points %ProgramData% at a temp folder for the duration of a test (the real one is untouched).
class ProgramDataOverride {
public:
    explicit ProgramDataOverride(const std::filesystem::path& p) {
        wchar_t buf[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
        if (n && n < MAX_PATH) saved_ = buf;
        SetEnvironmentVariableW(L"ProgramData", p.c_str());
    }
    ~ProgramDataOverride() { SetEnvironmentVariableW(L"ProgramData", saved_.empty() ? nullptr : saved_.c_str()); }

private:
    std::wstring saved_;
};

}  // namespace

IXC_TEST(ActiveProfile_MissingFileIsReportedAsMissing) {
    test::TempDir dir;
    ProgramDataOverride o(dir.path());
    bool missing = false;
    const ProfileLoadResult r = LoadActiveProfile(&missing);
    IXC_CHECK(!r.ok);
    IXC_CHECK(missing);
}

IXC_TEST(ActiveProfile_PublishThenLoadRoundTrips) {
    test::TempDir dir;
    ProgramDataOverride o(dir.path());
    std::filesystem::create_directories(ActiveProfileDirectory());
    Profile p;
    p.image.brightness = 33;
    p.mirror = true;
    IXC_REQUIRE(SUCCEEDED(PublishActiveProfile(p)));
    bool missing = true;
    const ProfileLoadResult r = LoadActiveProfile(&missing);
    IXC_REQUIRE(r.ok);
    IXC_CHECK(!missing);
    IXC_CHECK(r.profile == p);
    IXC_CHECK(!std::filesystem::exists(ActiveProfilePath().wstring() + L".tmp"));  // atomic write left no temp file
}

IXC_TEST(ActiveProfile_InvalidOrOversizedFileIsRejectedButNotMissing) {
    test::TempDir dir;
    ProgramDataOverride o(dir.path());
    std::filesystem::create_directories(ActiveProfileDirectory());

    IXC_REQUIRE(SUCCEEDED(WriteFileAtomic(ActiveProfilePath(), "{\"schemaVersion\": 1, \"image\": {")));  // truncated
    bool missing = true;
    IXC_CHECK(!LoadActiveProfile(&missing).ok);
    IXC_CHECK(!missing);  // present-but-invalid: the source keeps its last good settings

    IXC_REQUIRE(SUCCEEDED(WriteFileAtomic(ActiveProfilePath(), std::string(kMaxActiveProfileBytes + 1, ' '))));
    IXC_CHECK(!LoadActiveProfile(&missing).ok);
    IXC_CHECK(!missing);
}

IXC_TEST(ActiveProfile_HostileValuesAreClamped) {
    test::TempDir dir;
    ProgramDataOverride o(dir.path());
    std::filesystem::create_directories(ActiveProfileDirectory());
    IXC_REQUIRE(SUCCEEDED(WriteFileAtomic(ActiveProfilePath(),
        R"({"schemaVersion":1,"zoom":1e300,"width":4294967296,"image":{"brightness":-1e9,"gamma":0,"sharpness":1e9}})")));
    const ProfileLoadResult r = LoadActiveProfile();
    IXC_REQUIRE(r.ok);
    IXC_CHECK_EQ(r.profile.zoom, 4.0);
    IXC_CHECK_EQ(r.profile.image.brightness, -100.0);
    IXC_CHECK_EQ(r.profile.image.gamma, 0.2);
    IXC_CHECK_EQ(r.profile.image.sharpness, 100.0);
    IXC_CHECK(!r.warnings.empty());
}

// The camera service reads active-profile.json right when the app replaces it (the temp file
// the app writes first already wakes the service). A reader holding the file must not make the
// publish fail: that left IXC Camera on stale settings in other apps (0.14.1 and earlier).
IXC_TEST(ActiveProfile_PublishSucceedsWhileTheServiceReadsTheFile) {
    test::TempDir dir;
    ProgramDataOverride o(dir.path());
    CreateDirectoryW((dir.path() / L"IXC Camera").c_str(), nullptr);
    Profile p;
    p.background.mode = BackgroundMode::Blur;
    p.background.strength = 20;
    IXC_CHECK(SUCCEEDED(PublishActiveProfile(p)));

    // An older service build opened it with read sharing only: replacing waits until it lets go.
    HANDLE oldReader = CreateFileW(ActiveProfilePath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    IXC_CHECK(oldReader != INVALID_HANDLE_VALUE);
    HANDLE closer = CreateThread(
        nullptr, 0,
        [](void* h) -> DWORD {
            Sleep(60);
            CloseHandle(static_cast<HANDLE>(h));
            return 0;
        },
        oldReader, 0, nullptr);
    p.background.strength = 90;
    IXC_CHECK(SUCCEEDED(PublishActiveProfile(p)));  // retried until the reader let go
    if (closer) {
        WaitForSingleObject(closer, INFINITE);
        CloseHandle(closer);
    }
    const ProfileLoadResult r = LoadActiveProfile();
    IXC_CHECK(r.ok);
    IXC_CHECK_EQ(r.profile.background.strength, 90.0);

    // A reader that shares delete access never blocks the replace.
    HANDLE reader = CreateFileW(ActiveProfilePath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE closer2 = CreateThread(
        nullptr, 0,
        [](void* h) -> DWORD {
            Sleep(60);
            CloseHandle(static_cast<HANDLE>(h));
            return 0;
        },
        reader, 0, nullptr);
    p.background.strength = 45;
    IXC_CHECK(SUCCEEDED(PublishActiveProfile(p)));
    if (closer2) {
        WaitForSingleObject(closer2, INFINITE);
        CloseHandle(closer2);
    }
    IXC_CHECK_EQ(LoadActiveProfile().profile.background.strength, 45.0);
}
