# -*- coding: utf-8 -*-
"""Compare the 4 probe markers (probe_markers.py) between two shot folders, pixel for pixel.
Box of +-40 px around each marker centre (NDC (+-0.85, +-0.80) at 1280x720 -> (96,72) (1184,72) (96,648) (1184,648)).
The window corners (8 px) are not in any box (the window's rounded corners show what is behind it; 2-13). For each scene and each marker, the shots are compared in pairs
(base shot i vs new shot i). Also prints the marker's pixel count (non-background) so an empty box is visible.
usage: python -I compare_markers.py <base dir> <new dir>   -> exit 0 if every box is identical"""
import os, sys
import numpy as np
from PIL import Image
base, new = sys.argv[1], sys.argv[2]
CENTRES = [(96, 72), (1184, 72), (96, 648), (1184, 648)]
NAMES = ["cyan r.04", "magenta r.03", "yellow r.02", "orange r.05 a.5"]
R = 40
ok = True
for scene in ("boids", "survivor"):
    for i in (1, 2, 3):
        a = np.asarray(Image.open(os.path.join(base, "%s_%d.png" % (scene, i))).convert("RGB")).astype(int)
        b = np.asarray(Image.open(os.path.join(new, "%s_%d.png" % (scene, i))).convert("RGB")).astype(int)
        if a.shape != b.shape:
            print("%s_%d: sizes differ %s %s" % (scene, i, a.shape, b.shape)); ok = False; continue
        bg = a[360, 5]
        cells = []
        for (cx, cy), name in zip(CENTRES, NAMES):
            ba = a[cy - R:cy + R, cx - R:cx + R]; bb = b[cy - R:cy + R, cx - R:cx + R]
            diff = np.abs(ba - bb).max(axis=2)
            ndiff = int((diff > 0).sum()); mx = int(diff.max())
            npx = int((np.abs(ba - bg).sum(axis=2) > 30).sum())
            cells.append("%s: px=%d diff=%d max=%d" % (name, npx, ndiff, mx))
            if ndiff != 0 or npx == 0:
                ok = False
        print("%s_%d  %s" % (scene, i, " | ".join(cells)))
print("ALL MARKERS IDENTICAL" if ok else "DIFFERENCES FOUND")
sys.exit(0 if ok else 1)
