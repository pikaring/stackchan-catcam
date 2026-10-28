"""LottieFiles の Claude アニメーション (GIF) を、ファームに埋め込むコマ (src/clawd_frames.h) に変換する。

元: "Free claude Animation" by Fausto Villalba on LottieFiles
    https://lottiefiles.com/free-animation/claude-h8K0CcrGcz (Lottie Simple License)
GIF: https://assets-v2.lottiefiles.com/a/454115a8-f911-4533-ab7a-f7523c240d61/ds8ix4bge1.gif

使い方: python make_clawd_frames.py claude.gif ../src/clawd_frames.h
ffmpeg が必要。PIL は使わない (この環境に無いため)。

各コマを 5 色のパレットに丸め (白い背景は黒にする)、行ごとの連長 (長さ, 色) で圧縮する。
"""
import subprocess
import sys

SRC_W, SRC_H = 640, 430
# 全コマで絵がある範囲 (下端まで絵がある)
CROP_X, CROP_Y, CROP_W, CROP_H = 168, 160, 404, 270
OUT_W, OUT_H = 202, 135  # 1/2 に縮小
# 元の色 → 画面の色 (RGB)。背景の白は画面では黒にする
PALETTE = [
    ((255, 255, 255), (0, 0, 0)),  # 背景
    ((212, 116, 84), (212, 116, 84)),  # 体
    ((186, 101, 74), (186, 101, 74)),  # 体の影
    ((0, 0, 0), (17, 17, 17)),  # 目
    ((135, 136, 136), (135, 136, 136)),  # レバー
]


def nearest(px):
    return min(range(len(PALETTE)), key=lambda i: sum((a - b) ** 2 for a, b in zip(px, PALETTE[i][0])))


def main(gif, out):
    vf = f"crop={CROP_W}:{CROP_H}:{CROP_X}:{CROP_Y},scale={OUT_W}:{OUT_H}:flags=neighbor"
    raw = subprocess.run(
        ["ffmpeg", "-loglevel", "error", "-i", gif, "-vf", vf, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        check=True,
        capture_output=True,
    ).stdout
    frame_bytes = OUT_W * OUT_H * 3
    n = len(raw) // frame_bytes
    cache = {}
    frames = []
    for f in range(n):
        runs = []
        base = f * frame_bytes
        for y in range(OUT_H):
            prev, length = None, 0
            for x in range(OUT_W):
                p = base + (y * OUT_W + x) * 3
                px = raw[p : p + 3]
                idx = cache.get(px)
                if idx is None:
                    idx = cache[px] = nearest(tuple(px))
                if idx == prev and length < 255:
                    length += 1
                else:
                    if prev is not None:
                        runs += [length, prev]
                    prev, length = idx, 1
            runs += [length, prev]
        frames.append(runs)

    def rgb565(c):
        r, g, b = c
        return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

    with open(out, "w") as fp:
        fp.write("// make_clawd_frames.py で生成。手で編集しない。\n")
        fp.write('// "Free claude Animation" by Fausto Villalba on LottieFiles\n')
        fp.write("// https://lottiefiles.com/free-animation/claude-h8K0CcrGcz\n")
        fp.write("// Lottie Simple License (https://lottiefiles.com/page/license) の下で縮小・減色して使っている\n")
        fp.write("#pragma once\n#include <stdint.h>\n\nnamespace clawd {\n\n")
        fp.write(f"static const int ANIM_W = {OUT_W};\nstatic const int ANIM_H = {OUT_H};\n")
        fp.write(f"static const int ANIM_FRAMES = {n};\n")
        fp.write("static const uint16_t ANIM_PALETTE[] = {" + ", ".join(f"0x{rgb565(c[1]):04X}" for c in PALETTE) + "};\n\n")
        fp.write("// 1コマは左上から行ごとに (長さ, 色番号) の並び。長さは行をまたがない\n")
        for i, runs in enumerate(frames):
            fp.write(f"static const uint8_t ANIM_FRAME_{i}[] = {{")
            fp.write(",".join(str(v) for v in runs))
            fp.write("};\n")
        fp.write("\nstatic const uint8_t* const ANIM_FRAME_DATA[] = {" + ", ".join(f"ANIM_FRAME_{i}" for i in range(n)) + "};\n")
        fp.write("static const uint16_t ANIM_FRAME_LEN[] = {" + ", ".join(str(len(r)) for r in frames) + "};\n")
        fp.write("\n}  // namespace clawd\n")
    total = sum(len(r) for r in frames)
    print(f"{n} frames, {total} bytes")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
