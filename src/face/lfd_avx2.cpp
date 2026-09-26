// libfacedetection compiled into its own namespace. AVX2 build: called only when the CPU and OS support AVX2 (face/detector.cpp).
// The vendored sources are unmodified; see third_party/libfacedetection/README.IXC.md.
#define _ENABLE_AVX2  // intrinsics only; this file is NOT compiled with /arch:AVX2 (see README.IXC.md)
#include <immintrin.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <typeinfo>
#include <vector>

#include <stdint.h>
#include <string.h>

namespace lfd_avx2 {
#include "facedetectcnn.h"
#include "facedetectcnn.cpp"
#include "facedetectcnn-model.cpp"
#include "facedetectcnn-data.cpp"
}  // namespace lfd_avx2
