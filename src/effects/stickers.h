#pragma once

// Face stickers: original vector artwork (shapes defined in code; no image files, nothing loaded)
// drawn onto the NV12 output, anchored to the eyes and nose. They scale with the face, tilt with
// the head and follow mirror/zoom (anchors are given in output pixels).
//
// Rendering: each sticker is a short list of signed-distance shapes in face-local units (1 unit =
// the distance between the eyes; origin between the eyes; x along the eye line, y downward).
// Per pixel inside the sticker's bounding box the shapes are composited with 1-pixel
// anti-aliasing; chroma is composited once per 2x2 block. No memory is allocated.

#include "face/face_types.h"
#include "processing/image_pipeline.h"

namespace ixc::effects::stickers {

enum class Id { Shades, HeartEyes, Crown, Puppy };

struct Anchor {
    face::PointF eyeLeft, eyeRight;  // output pixels; "left" = smaller x in the output
    face::PointF nose;               // output pixels
};

// Draws a sticker with the given opacity (0..1). fullRange: the frame's YUV range.
void Draw(const processing::Nv12Frame& f, Id id, const Anchor& a, float opacity, bool fullRange);

}  // namespace ixc::effects::stickers
