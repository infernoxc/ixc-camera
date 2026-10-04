#!/usr/bin/env python3
"""Converts MediaPipe's selfie_segmenter_landscape.tflite into a C++ include for src/segmentation.

Usage (development only, not part of the build):
    pip install tflite numpy
    python3 scripts/convert-selfie-model.py selfie_segmenter_landscape.tflite \
        third_party/mediapipe_selfie_segmenter/selfie_segmenter_landscape.inc

The output is data only: a fixed list of operations (built-in kernels in selfie_net.cpp) and the
model's fp16 weights, pre-arranged for those kernels. Activations that directly follow a
convolution are fused into it. Intermediate tensors get offsets in one float arena, reused once
a tensor is dead (greedy first fit), so inference needs exactly one allocation, made at start.
"""

import hashlib
import sys

import numpy as np
import tflite

# Op codes shared with src/segmentation/selfie_net.h (enum class OpType).
CONV, DWCONV, MEAN, MUL, ADD, LOGISTIC, RELU, HSWISH, RESIZE2X, DECONV2X = range(10)
ACT_NONE, ACT_RELU, ACT_HSWISH, ACT_LOGISTIC = range(4)


def builtin_name(code):
    for k, v in tflite.BuiltinOperator.__dict__.items():
        if v == code and not k.startswith("_"):
            return k
    return str(code)


def main(src, dst):
    data = open(src, "rb").read()
    model = tflite.Model.GetRootAsModel(data, 0)
    g = model.Subgraphs(0)

    def shape(t):
        return [int(x) for x in g.Tensors(t).ShapeAsNumpy()]

    def const(t):
        tensor = g.Tensors(t)
        raw = model.Buffers(tensor.Buffer()).DataAsNumpy()
        if not isinstance(raw, np.ndarray) or raw.size == 0:
            return None
        dt = {0: np.float32, 1: np.float16, 2: np.int32}[tensor.Type()]
        return np.frombuffer(raw.tobytes(), dt).reshape(shape(t))

    # Collect ops; DEQUANTIZE (fp16 -> fp32 constants) is resolved at conversion time.
    consts = {}
    ops = []
    for i in range(g.OperatorsLength()):
        op = g.Operators(i)
        oc = model.OperatorCodes(op.OpcodeIndex())
        name = builtin_name(max(oc.BuiltinCode(), oc.DeprecatedBuiltinCode()))
        if name == "CUSTOM":
            name = oc.CustomCode().decode()
        ins = [int(x) for x in op.InputsAsNumpy()]
        outs = [int(x) for x in op.OutputsAsNumpy()]
        if name == "DEQUANTIZE":
            consts[outs[0]] = const(ins[0]).astype(np.float16)
            continue
        ops.append((name, ins, outs, op))

    def weights(t):
        if t in consts:
            return consts[t]
        c = const(t)
        if c is None:
            raise SystemExit(f"tensor {t} is not constant")
        return c

    # Fuse an activation into the preceding conv when the conv output feeds only that activation.
    uses = {}
    for name, ins, outs, _ in ops:
        for t in ins:
            uses[t] = uses.get(t, 0) + 1
    act_of = {"RELU": ACT_RELU, "HARD_SWISH": ACT_HSWISH, "LOGISTIC": ACT_LOGISTIC}
    fused = []
    skip = set()
    for i, (name, ins, outs, op) in enumerate(ops):
        if i in skip:
            continue
        act = ACT_NONE
        out = outs[0]
        if name in ("CONV_2D", "DEPTHWISE_CONV_2D", "Convolution2DTransposeBias") and i + 1 < len(ops):
            nname, nins, nouts, _ = ops[i + 1]
            if nname in act_of and nins[0] == out and uses.get(out, 0) == 1:
                act = act_of[nname]
                out = nouts[0]
                skip.add(i + 1)
        fused.append((name, ins, out, op, act))

    blob = []  # fp16 weights
    def put(arr):
        off = sum(a.size for a in blob)
        blob.append(np.ascontiguousarray(arr, dtype=np.float16).ravel())
        return off

    graph_in = int(g.Inputs(0))
    graph_out = int(g.Outputs(0))
    rows = []
    tensors = []  # activations, in order of production
    for name, ins, out, op, act in fused:
        bo = op.BuiltinOptions()
        a = shape(ins[0])
        o = shape(out)
        row = dict(type=None, act=act, in0=ins[0], in1=-1, out=out, h=a[1], w=a[2], c=a[3], oh=o[1], ow=o[2], oc=o[3],
                   k=0, stride=1, padT=0, padL=0, wOff=-1, bOff=-1)
        if name == "CONV_2D":
            opt = tflite.Conv2DOptions(); opt.Init(bo.Bytes, bo.Pos)
            assert opt.DilationWFactor() == 1 and opt.DilationHFactor() == 1 and opt.FusedActivationFunction() == 0
            w = weights(ins[1])  # [O, kh, kw, I]
            kh, kw = w.shape[1], w.shape[2]
            assert kh == kw
            row.update(type=CONV, k=kh, stride=opt.StrideW())
            assert opt.StrideW() == opt.StrideH()
            row["wOff"] = put(np.transpose(w, (1, 2, 3, 0)))  # -> [kh, kw, I, O]: inner loop over O
            row["bOff"] = put(weights(ins[2]))
            same = opt.Padding() == tflite.Padding.SAME
        elif name == "DEPTHWISE_CONV_2D":
            opt = tflite.DepthwiseConv2DOptions(); opt.Init(bo.Bytes, bo.Pos)
            assert opt.DepthMultiplier() == 1 and opt.FusedActivationFunction() == 0
            w = weights(ins[1])  # [1, kh, kw, C]
            row.update(type=DWCONV, k=w.shape[1], stride=opt.StrideW())
            assert opt.StrideW() == opt.StrideH()
            row["wOff"] = put(w[0])
            row["bOff"] = put(weights(ins[2]))
            same = opt.Padding() == tflite.Padding.SAME
        elif name == "Convolution2DTransposeBias":
            w = weights(ins[1])  # [O, 2, 2, I]
            assert w.shape[1] == 2 and w.shape[2] == 2 and o[1] == 2 * a[1] and o[2] == 2 * a[2]
            row.update(type=DECONV2X, k=2, stride=2)
            row["wOff"] = put(np.transpose(w, (1, 2, 3, 0)))  # -> [2, 2, I, O]
            row["bOff"] = put(weights(ins[2]))
            same = False
        elif name == "MEAN":
            axes = list(weights(ins[1]).ravel())
            assert axes == [1, 2] and o[1] == 1 and o[2] == 1
            row["type"] = MEAN
            same = False
        elif name in ("MUL", "ADD"):
            opt = tflite.MulOptions() if name == "MUL" else tflite.AddOptions()
            opt.Init(bo.Bytes, bo.Pos)
            assert opt.FusedActivationFunction() == 0
            b = shape(ins[1])
            row["type"] = MUL if name == "MUL" else ADD
            row["in1"] = ins[1]
            # in1 is either the same shape or a [1,1,1,C] per-channel vector.
            row["k"] = 1 if b[1] * b[2] == 1 and a[1] * a[2] > 1 else 0
            assert row["k"] == 1 or b == a
            same = False
        elif name in ("LOGISTIC", "RELU", "HARD_SWISH"):
            row["type"] = {"LOGISTIC": LOGISTIC, "RELU": RELU, "HARD_SWISH": HSWISH}[name]
            same = False
        elif name == "RESIZE_BILINEAR":
            opt = tflite.ResizeBilinearOptions(); opt.Init(bo.Bytes, bo.Pos)
            assert not opt.AlignCorners() and opt.HalfPixelCenters() and o[1] == 2 * a[1] and o[2] == 2 * a[2]
            row["type"] = RESIZE2X
            same = False
        else:
            raise SystemExit(f"unsupported op {name}")
        if same and row["k"] > 1:
            s = row["stride"]
            pad_h = max((o[1] - 1) * s + row["k"] - a[1], 0)
            pad_w = max((o[2] - 1) * s + row["k"] - a[2], 0)
            row["padT"], row["padL"] = pad_h // 2, pad_w // 2
        rows.append(row)
        tensors.append(out)

    # Arena: liveness-based first-fit over activation tensors (the graph input is live first).
    def size(t):
        return int(np.prod(shape(t)))
    last_use = {}
    for i, r in enumerate(rows):
        for t in (r["in0"], r["in1"]):
            if t >= 0:
                last_use[t] = i
    last_use[graph_out] = len(rows)
    offset = {}
    live = []  # (start, end, tensor)

    def alloc(t):
        n = (size(t) + 15) // 16 * 16  # 64-byte aligned
        pos = 0
        for s, e, _ in sorted(live):
            if pos + n <= s:
                break
            pos = max(pos, e)
        offset[t] = pos
        live.append((pos, pos + n, t))

    alloc(graph_in)
    for i, r in enumerate(rows):
        alloc(r["out"])  # inputs stay live during the op: output never aliases them
        live[:] = [(s, e, t) for s, e, t in live if last_use.get(t, -1) > i or t == graph_out]
    # High-water mark over every tensor ever placed.
    high = 0
    for t in [graph_in] + [r["out"] for r in rows]:
        high = max(high, offset[t] + size(t))
    arena = (high + 15) // 16 * 16

    w = np.concatenate(blob)
    digest = hashlib.sha256(data).hexdigest()
    with open(dst, "w", newline="\n") as f:
        f.write("// Generated by scripts/convert-selfie-model.py. Do not edit.\n")
        f.write(f"// Source: MediaPipe selfie_segmenter_landscape.tflite (float16), sha256 {digest}\n")
        f.write("// License: Apache License 2.0 (see LICENSE in this directory).\n\n")
        f.write(f"inline constexpr int kInputW = {shape(graph_in)[2]}, kInputH = {shape(graph_in)[1]};\n")
        f.write(f"inline constexpr std::uint32_t kArenaFloats = {arena};\n")
        f.write(f"inline constexpr std::uint32_t kInputOffset = {offset[graph_in]}, kOutputOffset = {offset[graph_out]};\n")
        f.write(f"inline constexpr std::uint32_t kWeightCount = {w.size};\n\n")
        f.write("// type, act, in0, in1, out, h, w, c, oh, ow, oc, k, stride, padT, padL, wOff, bOff\n")
        f.write("inline constexpr Op kOps[] = {\n")
        for r in rows:
            in1 = offset[r["in1"]] if r["in1"] >= 0 else -1
            f.write("    {%d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d},\n" % (
                r["type"], r["act"], offset[r["in0"]], in1, offset[r["out"]], r["h"], r["w"], r["c"], r["oh"], r["ow"], r["oc"],
                r["k"], r["stride"], r["padT"], r["padL"], r["wOff"], r["bOff"]))
        f.write("};\n\n")
        f.write("// IEEE half-precision weights, in the layouts the kernels read.\n")
        f.write("inline constexpr std::uint16_t kWeightsF16[] = {\n")
        bits = w.view(np.uint16)
        for i in range(0, bits.size, 16):
            f.write("    " + ",".join("0x%04x" % b for b in bits[i:i + 16]) + ",\n")
        f.write("};\n")
    print(f"{len(rows)} ops, {w.size} weights, arena {arena} floats ({arena * 4 / 1e6:.2f} MB)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
