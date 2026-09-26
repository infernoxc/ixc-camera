#pragma once

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <string>

namespace ixc::test {

// Unique temporary directory, removed (recursively) on destruction.
class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                (L"ixc-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(counter++));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace ixc::test
