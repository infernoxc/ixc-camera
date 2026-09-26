#pragma once

// libfacedetection (YuNet, BSD-3-Clause, third_party/libfacedetection) behind a small interface.
//
// Two builds of the detector are linked in: a portable one (any x64 CPU, including old
// Pentium/Celeron) and an AVX2 one. They return identical results (Phase 7 benchmark); AVX2 is
// ~3x faster. The choice is made at runtime from CPUID/XGETBV.
//
// Not thread-safe per instance. Calls from different instances are serialized internally
// (the library initializes shared weights on first use).

#include "face/face_types.h"

#include <cstdint>
#include <memory>

namespace ixc::face {

enum class SimdPath { Portable, Avx2 };

// True when face tracking was compiled in (CMake IXC_WITH_FACE_TRACKING).
bool DetectorAvailable();
// Best path this CPU supports.
SimdPath BestSimdPath();
const char* ToString(SimdPath p);

class Detector {
public:
    explicit Detector(SimdPath path);
    ~Detector();
    Detector(const Detector&) = delete;
    Detector& operator=(const Detector&) = delete;

    // bgr: w*h pixels, 3 bytes each, row pitch `step` bytes. Writes up to maxOut detections,
    // highest confidence first, with normalized coordinates. Returns the number written
    // (0 when not available or on invalid input).
    int Detect(const std::uint8_t* bgr, int w, int h, int step, float minConfidence, Detection* out, int maxOut);

    SimdPath path() const { return path_; }

private:
    SimdPath path_;
    std::unique_ptr<unsigned char[]> result_;  // FACEDETECTION_RESULT_BUFFER_SIZE bytes
};

}  // namespace ixc::face
