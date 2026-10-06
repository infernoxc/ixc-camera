#pragma once

// SHA-256 (FIPS 180-4), used to verify downloaded updates against the release's SHA256SUMS.
// Small portable implementation (no OS crypto dependency), tested against the standard vectors.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ixc {

class Sha256 {
public:
    Sha256();
    void Update(const void* data, size_t size);
    std::array<std::uint8_t, 32> Final();

    static std::string Hex(const void* data, size_t size);  // lowercase hex digest of a buffer

private:
    void Block(const std::uint8_t* p);
    std::uint32_t h_[8];
    std::uint8_t buf_[64];
    size_t used_ = 0;
    std::uint64_t bits_ = 0;
};

}  // namespace ixc
