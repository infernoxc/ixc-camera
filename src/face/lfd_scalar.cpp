// libfacedetection compiled into its own namespace. Portable build (no AVX2).
// The vendored sources are unmodified; see third_party/libfacedetection/README.IXC.md.
// Portable build: SSE2 baseline, runs on every x64 CPU.
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

namespace lfd_scalar {
#include "facedetectcnn.h"
#include "facedetectcnn.cpp"
#include "facedetectcnn-model.cpp"
#include "facedetectcnn-data.cpp"
}  // namespace lfd_scalar
