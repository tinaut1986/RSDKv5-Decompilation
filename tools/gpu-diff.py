#!/usr/bin/env python3
"""Compares two screenshots of the same frozen frame, one drawn by the GPU and one by the CPU (software) renderer.
  tools/gpu-diff.py gpu.png cpu.png [diff.png]  -> prints "<bad blocks> <worst block %> <% pixels differing>"
Blocks are 16x16 px. The debug overlay (top right) and the HUD (top left, its clock ticks) are left out. A block is
bad when more than 40 % of its pixels differ by more than 48 in some channel: the GPU's line rounding and ink
approximations stay under that, a missing sprite, tile or layer does not."""
import sys
import numpy as np
from PIL import Image

a = np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(int)
b = np.asarray(Image.open(sys.argv[2]).convert("RGB")).astype(int)
h, w, _ = a.shape
d = (np.abs(a - b).max(axis=2) > 48)
d[:36, :80] = False   # HUD: score, time, rings
d[:36, w - 70:] = False  # FPS overlay
bad, worst = 0, 0.0
mask = np.zeros_like(d)
for y in range(0, h, 16):
    for x in range(0, w, 16):
        f = d[y:y + 16, x:x + 16].mean()
        worst = max(worst, f)
        if f > 0.4:
            bad += 1
            mask[y:y + 16, x:x + 16] = True
print(bad, "%.0f" % (worst * 100), "%.1f" % (d.mean() * 100))
if len(sys.argv) > 3:
    out = np.concatenate([a, b, np.stack([mask * 255, d * 255, np.zeros_like(d)], axis=2)], axis=1).astype(np.uint8)
    Image.fromarray(out).save(sys.argv[3])
