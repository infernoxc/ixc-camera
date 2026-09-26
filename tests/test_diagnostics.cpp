#include "common/fileio.h"
#include "common/strings.h"
#include "diagnostics/error.h"
#include "diagnostics/log.h"
#include "ixc_test.h"
#include "test_util.h"

#include <mferror.h>

#include <string>

using namespace ixc;

IXC_TEST(Error_FormatsHResultReadably) {
    Error e{E_ACCESSDENIED, "RegisterVirtualCamera", "IXC Camera could not register the system camera."};
    const std::string s = e.Describe();
    IXC_CHECK(s.find("IXC Camera could not register the system camera.") == 0);
    IXC_CHECK(s.find("HRESULT: 0x80070005") != std::string::npos);
    IXC_CHECK(s.find("Stage: RegisterVirtualCamera.") != std::string::npos);
    IXC_CHECK(s.find("Unknown error") == std::string::npos);  // system text was found
}

IXC_TEST(Error_UnknownCodeStillProducesMessage) {
    const std::string s = HResultMessage(static_cast<HRESULT>(0xA0DE0001));
    IXC_CHECK(!s.empty());
}

IXC_TEST(Error_ClassifiesCameraFailures) {
    IXC_CHECK(ClassifyHResult(E_ACCESSDENIED) == ErrorClass::AccessDenied);
    IXC_CHECK(ClassifyHResult(MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED) == ErrorClass::DeviceLost);
    IXC_CHECK(ClassifyHResult(MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED) == ErrorClass::Transient);
    IXC_CHECK(ClassifyHResult(MF_E_INVALIDMEDIATYPE) == ErrorClass::Unsupported);
    IXC_CHECK(ClassifyHResult(E_POINTER) == ErrorClass::Fatal);
    IXC_CHECK_EQ(ToString(ErrorClass::DeviceLost), std::string_view("DeviceLost"));
}

IXC_TEST(Strings_Utf8RoundTrip) {
    const std::string s = "IXC \xE2\x80\x94 \xF0\x9F\x93\xB7";
    IXC_CHECK_EQ(WideToUtf8(Utf8ToWide(s)), s);
    IXC_CHECK(IsValidUtf8(s));
    IXC_CHECK(!IsValidUtf8("\xC3"));
    IXC_CHECK(!IsValidUtf8("\xF4\x90\x80\x80"));  // > U+10FFFF
    IXC_CHECK(Utf8ToWide("").empty());
}

namespace {
std::string ReadLog(const std::filesystem::path& p) {
    std::string text;
    ReadFileLimited(p, 16u << 20, text);
    return text;
}
}  // namespace

IXC_TEST(Log_RespectsModeAndStripsNewlines) {
    test::TempDir dir;
    log::Config cfg;
    cfg.directory = dir.path();
    cfg.baseName = L"t";
    IXC_REQUIRE(log::Init(cfg));
    log::Info("test", "visible\r\nforged line");
    log::Debug("test", "hidden in normal mode");
    log::SetMode(log::Mode::Debug);
    log::Debug("test", "visible in debug mode");
    log::SetMode(log::Mode::Normal);
    log::Shutdown();

    const std::string text = ReadLog(dir.path() / L"t.log");
    IXC_CHECK(text.find("test: visible  forged line") != std::string::npos);
    IXC_CHECK(text.find("hidden in normal mode") == std::string::npos);
    IXC_CHECK(text.find("visible in debug mode") != std::string::npos);
}

IXC_TEST(Log_RotationBoundsDiskUse) {
    test::TempDir dir;
    log::Config cfg;
    cfg.directory = dir.path();
    cfg.baseName = L"r";
    cfg.maxFileBytes = 8192;
    cfg.maxBackups = 2;
    IXC_REQUIRE(log::Init(cfg));
    const std::string msg(200, 'x');
    for (int i = 0; i < 2000; ++i) log::Info("rot", msg);
    log::Shutdown();

    std::uintmax_t total = 0;
    int files = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir.path())) {
        ++files;
        total += e.file_size();
        IXC_CHECK(e.file_size() <= cfg.maxFileBytes);
    }
    IXC_CHECK_EQ(files, 3);  // r.log, r.1.log, r.2.log
    IXC_CHECK(total <= 3 * cfg.maxFileBytes);
    IXC_CHECK(!std::filesystem::exists(dir.path() / L"r.3.log"));
}

IXC_TEST(FileIo_ReadLimitRejectsLargeFiles) {
    test::TempDir dir;
    const auto p = dir.path() / L"big.bin";
    IXC_REQUIRE(SUCCEEDED(WriteFileAtomic(p, std::string(5000, 'a'))));
    std::string out;
    IXC_CHECK(ReadFileLimited(p, 4999, out) == HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE));
    IXC_CHECK(SUCCEEDED(ReadFileLimited(p, 5000, out)));
    IXC_CHECK_EQ(out.size(), size_t{5000});
    IXC_CHECK(!std::filesystem::exists(dir.path() / L"big.bin.tmp"));
    IXC_CHECK(FAILED(ReadFileLimited(dir.path() / L"missing", 10, out)));
}
