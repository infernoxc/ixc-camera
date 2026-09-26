#include "face/detector.h"

#include <intrin.h>

#include <algorithm>
#include <mutex>

#if IXC_FACE_TRACKING
// Defined by src/face/lfd_scalar.cpp and lfd_avx2.cpp (the vendored library compiled twice).
namespace lfd_scalar {
int* facedetect_cnn(unsigned char* result_buffer, unsigned char* bgr, int width, int height, int step);
}
namespace lfd_avx2 {
int* facedetect_cnn(unsigned char* result_buffer, unsigned char* bgr, int width, int height, int step);
}
#endif

namespace ixc::face {

namespace {
constexpr size_t kResultBytes = 0x9000;  // FACEDETECTION_RESULT_BUFFER_SIZE
constexpr int kStrideShorts = 16;        // FACEDETECTION_RESULT_STRIDE_SHORTS

std::mutex& LibraryMutex() {
    static std::mutex m;  // the library's lazy weight initialization isn't thread-safe
    return m;
}

bool CpuHasAvx2() {
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7) return false;
    __cpuid(r, 1);
    const bool osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
    if (!osxsave || !avx) return false;
    if ((_xgetbv(0) & 6) != 6) return false;  // OS saves the YMM registers
    __cpuidex(r, 7, 0);
    return (r[1] >> 5) & 1;
}
}  // namespace

bool DetectorAvailable() { return IXC_FACE_TRACKING != 0; }

SimdPath BestSimdPath() {
    static const SimdPath best = CpuHasAvx2() ? SimdPath::Avx2 : SimdPath::Portable;
    return best;
}

const char* ToString(SimdPath p) { return p == SimdPath::Avx2 ? "AVX2" : "portable"; }

Detector::Detector(SimdPath path) : path_(path == SimdPath::Avx2 && BestSimdPath() != SimdPath::Avx2 ? SimdPath::Portable : path) {
    if (DetectorAvailable()) result_ = std::make_unique<unsigned char[]>(kResultBytes);
}

Detector::~Detector() = default;

int Detector::Detect(const std::uint8_t* bgr, int w, int h, int step, float minConfidence, Detection* out, int maxOut) {
#if IXC_FACE_TRACKING
    if (!result_ || !bgr || !out || maxOut <= 0 || w < 32 || h < 32 || step < w * 3) return 0;
    int* r = nullptr;
    {
        std::lock_guard lock(LibraryMutex());
        auto* in = const_cast<unsigned char*>(bgr);  // the library reads only
        r = path_ == SimdPath::Avx2 ? lfd_avx2::facedetect_cnn(result_.get(), in, w, h, step)
                                    : lfd_scalar::facedetect_cnn(result_.get(), in, w, h, step);
    }
    if (!r || r[0] <= 0) return 0;
    const int total = std::min(r[0], static_cast<int>((kResultBytes - 4) / (kStrideShorts * sizeof(short))));
    const short* base = reinterpret_cast<const short*>(result_.get() + 4);

    int n = 0;
    for (int i = 0; i < total; ++i) {
        const short* p = base + static_cast<size_t>(kStrideShorts) * i;
        const float conf = p[0] / 100.0f;
        if (conf < minConfidence || p[3] <= 0 || p[4] <= 0) continue;
        Detection d;
        d.confidence = std::clamp(conf, 0.0f, 1.0f);
        const float iw = 1.0f / w, ih = 1.0f / h;
        d.box = {p[1] * iw, p[2] * ih, p[3] * iw, p[4] * ih};
        PointF* pts[5] = {&d.lm.leftEye, &d.lm.rightEye, &d.lm.nose, &d.lm.mouthLeft, &d.lm.mouthRight};
        for (int k = 0; k < 5; ++k) *pts[k] = {p[5 + 2 * k] * iw, p[6 + 2 * k] * ih};
        d.landmarksPlausible = IsPlausible(d);
        // Keep the best maxOut (insertion into a small sorted array).
        int pos = n < maxOut ? n : maxOut;
        while (pos > 0 && out[pos - 1].confidence < d.confidence) {
            if (pos < maxOut) out[pos] = out[pos - 1];
            --pos;
        }
        if (pos < maxOut) {
            out[pos] = d;
            if (n < maxOut) ++n;
        }
    }
    return n;
#else
    (void)bgr, (void)w, (void)h, (void)step, (void)minConfidence, (void)out, (void)maxOut;
    return 0;
#endif
}

}  // namespace ixc::face
