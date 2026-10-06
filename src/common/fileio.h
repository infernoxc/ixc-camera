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
// durable = false skips the disk flush (for frequently rewritten, regenerable files such as the
// live settings shared with the camera service); the replace is atomic either way. If `path` is
// briefly held open by another process, the replace is retried for up to ~250 ms.
HRESULT WriteFileAtomic(const std::filesystem::path& path, std::string_view data, bool durable = true);

}  // namespace ixc
