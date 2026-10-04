# MediaPipe selfie segmenter (vendored as data)

- Upstream model: MediaPipe Selfie Segmentation, **landscape** variant (input 256×144 RGB, output a 256×144 person mask), float16.
  Downloaded from `https://storage.googleapis.com/mediapipe-models/image_segmenter/selfie_segmenter_landscape/float16/latest/selfie_segmenter_landscape.tflite`.
  SHA-256 of that file: `490e9ea734313e0de10fa0cd9e3c6133e36ea4db2b7a49bde9ef019f72796b8e`.
- Authors (model card, 2021): Tingbo Hou, Siargey Pisarchyk, Karthik Raveendran, Google.
- License: Apache License 2.0 (model card "Licensed under"; `LICENSE` here is the MediaPipe repository's copy, also reproduced in `THIRD_PARTY_LICENSES.md`).
- Used for: the Background Blur and Studio Backdrop effects (`src/segmentation`).

## What's here
| File | SHA-256 |
|---|---|
| `selfie_segmenter_landscape.inc` | `beec5241486c2ad93a96ddd79a6aec6ccadce64d85657c2d48254a579d55b4b4` |
| `LICENSE` | `8707eef0533987efc5b155d64761eeb6e20793f50b9bd1a68dad1cf4719d0ed8` |

`selfie_segmenter_landscape.inc` is **generated** by `scripts/convert-selfie-model.py` from the `.tflite` file above. It holds:
- the network as a fixed list of operations (convolution, depthwise convolution, mean, add, multiply, activations, 2× bilinear resize, 2× transposed convolution), with activations fused into the preceding convolution;
- the model's float16 weights, unchanged in value, rearranged into the layouts IXC's kernels read;
- offsets of every intermediate tensor in one activation arena (2.2 MB).

The `.tflite` file itself isn't in the repository, and nothing reads a model file at run time. `src/segmentation/selfie_net.cpp` runs the operations with IXC's own C++/SSE2 kernels. There is no TFLite or other inference runtime.

## Verification
`Segmentation_NetworkMatchesReference` (unit test) checks IXC's output on a synthetic image against values computed with the reference TFLite runtime (LiteRT). During conversion, IXC's output was also compared with LiteRT on two photos and on random noise: largest difference 7.8e-5, identical person/background masks.

The CMake configure step verifies the SHA-256 of the `.inc` file.

## Updating
1. Download the new `.tflite` (float16 landscape model) and run `python3 scripts/convert-selfie-model.py <model.tflite> third_party/mediapipe_selfie_segmenter/selfie_segmenter_landscape.inc` (needs `pip install tflite numpy`).
2. Update the hashes here and in `src/CMakeLists.txt`.
3. Recompute the reference values in `tests/test_segmentation.cpp` with LiteRT (`pip install ai-edge-litert`), then run the unit tests and `ixc_probe --bench-effects`.
