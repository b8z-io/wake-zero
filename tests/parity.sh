#!/bin/sh
# Checks the C analysis against reference/analysis.js on the same synthetic frames. Needs python3 + numpy, node, cc.
set -e
cd "$(dirname "$0")"
T=$(mktemp -d)
python3 gen_frames.py 90 > "$T/frames.f32"
cc -O2 -o "$T/parity_c" parity_c.c ../src/analysis.c -lm
node parity.mjs "$T/frames.f32" > "$T/js.json"
"$T/parity_c" "$T/frames.f32" > "$T/c.json"
python3 - "$T/js.json" "$T/c.json" <<'PY'
import json, sys
a, b = (json.load(open(p)) for p in sys.argv[1:3])
worst = 0
for k in a:
    x, y = a[k], b[k]
    x, y = (x, y) if isinstance(x, list) else ([x], [y])
    d = max(abs(p - q) for p, q in zip(x, y)); scale = max(1e-6, max(abs(p) for p in x))
    print(f"{k:8s} max |js - c| = {d:.2e}  (largest value {scale:.3g})")
    worst = max(worst, d / max(scale, 1e-3))
print("PASS" if worst < 2e-3 else "FAIL", f"(worst relative {worst:.2e})")
sys.exit(0 if worst < 2e-3 else 1)
PY
rm -rf "$T"
