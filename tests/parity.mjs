// Runs reference/analysis.js (Wake's js/analysis.js) over the frames file and prints the last frame's fields as JSON.
import { readFileSync } from "node:fs";
import { Analyzer } from "../reference/analysis.js";
import { backgroundLevel } from "../reference/presence.js";
const W = 128, H = 72, buf = readFileSync(process.argv[2]);
const all = new Float32Array(buf.buffer, buf.byteOffset, buf.byteLength / 4), n = W * H, N = all.length / n;
const an = new Analyzer(W, H), rgba = new Uint8ClampedArray(n * 4);
for (let f = 0; f < N; f++) {
  for (let i = 0; i < n; i++) { const v = all[f * n + i] * 255; rgba[4 * i] = rgba[4 * i + 1] = rgba[4 * i + 2] = v; }
  an.update(rgba, 1 / 30);
}
const pick = (a) => Array.from(a);
console.log(JSON.stringify({ level: pick(an.level), motion: pick(an.motion), trail: pick(an.trail), flow: pick(an.flow),
  energy: an.energy, cx: an.cx, cy: an.cy, zMotion: pick(an.zMotion), bg: backgroundLevel(an) }));
