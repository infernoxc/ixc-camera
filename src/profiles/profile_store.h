#pragma once

#include "profiles/profile.h"

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace ixc {

// Stores profiles as individual "<stem>.json" files in one directory.
// File stems are derived from profile names but restricted to [a-z0-9-] so a profile name can
// never escape the directory or collide with reserved Windows device names.
class ProfileStore {
public:
    explicit ProfileStore(std::filesystem::path directory);

    const std::filesystem::path& directory() const { return dir_; }

    struct Entry {
        std::string stem;
        std::filesystem::path path;
    };
    std::vector<Entry> List() const;

    ProfileLoadResult Load(const std::string& stem) const;
    // Saves under `stem` (created from the profile name when empty); returns the stem used.
    HRESULT Save(const Profile& profile, std::string& stem) const;
    HRESULT Remove(const std::string& stem) const;

    static std::string MakeStem(std::string_view profileName);
    static bool IsValidStem(std::string_view stem);

    static constexpr size_t kMaxProfileFileBytes = 256 * 1024;

private:
    std::filesystem::path PathFor(const std::string& stem) const;
    std::filesystem::path dir_;
};

}  // namespace ixc
