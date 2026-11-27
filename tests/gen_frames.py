# Synthetic camera frames for the parity test: a lit room with a "person" (a soft blob) that walks across,
# waves an arm, plus sensor noise and a brightness flicker. Writes N frames of W*H float32 grey to stdout.
import sys, numpy as np
W, H, N = 128, 72, int(sys.argv[1]) if len(sys.argv) > 1 else 60
rng = np.random.default_rng(7)
y, x = np.mgrid[0:H, 0:W].astype(np.float32)
out = []
for f in range(N):
    t = f / 30
    room = 0.35 + 0.15 * (x / W) + 0.05 * np.sin(y / 9)
    cx = 30 + 60 * (0.5 - 0.5 * np.cos(t * 1.3)); cy = 40
    body = np.exp(-(((x - cx) / 14) ** 2 + ((y - cy) / 22) ** 2))
    ax = cx + 18 + 10 * np.sin(t * 6); ay = 24 + 6 * np.cos(t * 6)
    arm = np.exp(-(((x - ax) / 5) ** 2 + ((y - ay) / 5) ** 2))
    img = room + 0.45 * body + 0.4 * arm + 0.03 * np.sin(t * 9) + rng.normal(0, 0.01, (H, W))
    out.append(np.clip(img, 0, 1).astype(np.float32))
sys.stdout.buffer.write(np.stack(out).tobytes())
