# libfacedetection (vendored)

- Upstream: https://github.com/ShiqiYu/libfacedetection
- Commit: `acf7b254121927e7dced30e233a2e03119f28ea2`
- License: BSD 3-Clause (`LICENSE`, also reproduced in `THIRD_PARTY_LICENSES.md`)
- Used for: face detection with five landmarks (YuNet CNN, weights compiled into the code; no model file, no runtime)

## What's here
`src/` holds the four upstream files, **unmodified**: byte-identical to the git blobs at the commit above (LF line endings), SHA-256:

| File | SHA-256 |
|---|---|
| `src/facedetectcnn.h` | `896ad9cae317d1ebce947662ea44d7d1c3f12cd77606da977c0adea9c5625605` |
| `src/facedetectcnn.cpp` | `e86cbb3f4080762197d0a43b98f3534e08fe890a7eed5c5904e395b8bed4a55d` |
| `src/facedetectcnn-model.cpp` | `0c5689b7070c070429a2c45eb3ae7f02018e492db94b0a3eb660681e9ca0aee6` |
| `src/facedetectcnn-data.cpp` | `cf019963f413528540d6de741e72eea4f2f0b572ca85b22dd4baa8fc71b42bb6` |
| `LICENSE` | `95b286c39b1444594e18672e6092321e39cc11f2f1daf994cadfd95af7c91dcc` |

`ixc/facedetection_export.h` is IXC's replacement for the header that upstream generates with CMake.

## How IXC builds it
`src/face/lfd_scalar.cpp` and `src/face/lfd_avx2.cpp` compile these files twice, into separate namespaces:
- a portable build (SSE2 baseline, for every x64 CPU, including old Pentium/Celeron);
- an AVX2 build (`_ENABLE_AVX2`, intrinsics only; no `/arch:AVX2`, so shared inline code stays portable).

`face/detector.cpp` picks one at runtime (CPUID + XGETBV). OpenMP and AVX-512 aren't enabled.

## Updating or removing
- **Update:** replace `src/` and `LICENSE` from a new upstream commit, update the hashes above and the commit in `THIRD_PARTY_LICENSES.md`, then run the unit tests and `ixc_probe --bench-face`.
- **Remove:** configure with `-DIXC_WITH_FACE_TRACKING=OFF`. Face tracking then reports "not included in this build", and nothing from this folder is compiled. The folder can then be deleted.
