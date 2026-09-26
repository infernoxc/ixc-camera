#pragma once

#include <windows.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace ixc {

// Reads a whole file, refusing files larger than maxBytes (hostile or wrong file).
HRESULT ReadFileLimited(const std::filesystem::path& path, size_t maxBytes, std::string& out);

// Writes to "<path>.tmp", flushes, then atomically replaces `path`. A crash never leaves a
// half-written file at `path`.
HRESULT WriteFileAtomic(const std::filesystem::path& path, std::string_view data);

}  // namespace ixc
