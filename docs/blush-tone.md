# Blush Tone: reproducing a reference look

"Blush Tone" (`blush.tone`) reproduces the visible look of the "Blush Tone Time" lens the user has in Snap Camera. It's an independent reimplementation matched against frames of the user's own camera. No Snap Camera files, lens packages, code or assets were opened, copied or decoded. The reference was observed only through the "Snap Camera" virtual webcam output.

## How the reference was measured (2026-09-26)
- Lens-on frames: the "Snap Camera" virtual camera, 1280×720 (`ixc_probe --dshow-capture … --snapshot`).
- Lens-off frames: the Lenovo webcam directly, same position and light. Snap Camera outputs only its logo while no lens is active.
- **Global grade:** fitted from about 68,000 aligned background pixels. The person and overlay areas were excluded.
- **Face treatment:** compared at landmark-relative points (the detector's eyes, nose and mouth), side by side.
- The frames were used only on the user's PC for this comparison and deleted afterwards.

## What the lens does, and how IXC reproduces it

| Visible component | Measured on the reference | IXC implementation |
|---|---|---|
| Luma | shadows −5…−7 levels, mid-tones +6…+8, highlights rolled off (228 → 212) | Measured 28-point curve in the Y lookup table |
| Colour | blue–yellow axis ×0.78, red–green axis ×1.25: rosy, less yellow, stronger reds and pinks | U/V lookup tables (the same one-lookup-per-pixel pass as the colour looks) |
| Skin | noticeably smoother, softer and slightly glowing; beard, brows and eyes stay sharp | Edge-preserving smoothing over the face (wider radius than Basic Beauty) |
| Skin tone | even peach warmth over the face, no distinct cheek patches | Whole-face soft tint (more warmth, a little pink) |
| Under-eyes, nose tip | faint warm pink | Soft, gentle tints anchored to the landmarks |
| Lips | deep coral-pink fill (R/G ≈ 1.8) | Lip-shaped tint from the mouth corners. Dark pixels (beard, open mouth) are excluded |
| Timestamp ("LOCAL TIME …") | camcorder overlay | **Left out on the user's request** |
| Tracking | follows the face | The IXC face tracker (smoothing and prediction). The face treatment fades in and out with the face; the grade always applies |

## Matching iterations (IXC on the lens-off frame vs the lens-on reference)
1. The first pass had distinct under-eye and cheek patches and orange lips, and the skin wasn't soft enough. The reference is even and paler-coral.
2. Removed the cheek patches, added an even whole-face tint, and made the lips lighter. Skin then read too pink-magenta and too bright (a lit cheek clipped at 255), with lips too pale.
3. Measured the reference at landmark points (lit cheek 237/202/194, under-eye 202/165/156, lips 162/90/97). Retuned: peach skin tint (blue down), less luma, deeper coral lips. Lip R/G is now 1.89 vs 1.8 on the reference.
4. Stronger, wider skin smoothing for this look. The texture now matches the reference's softness.

Remaining differences: the reference applies its own exposure (brightness varies with the camera's automatic exposure in both), and smoothing strength differs slightly with face size.

## Cost
`ixc_probe --bench-effects`: 1.5 ms per 720p frame, 3.4 ms per 1080p frame (Ryzen 5 5600G), with 459 KB of reused scratch memory. The grade shares the colour-look lookup pass, and the face work is limited to the face region. Without a detected face, only the grade runs.
