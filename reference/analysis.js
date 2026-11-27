// Reference copy of js/analysis.js from Wake (the browser version), for tests/parity.sh.
// Turns small, mirrored webcam frames into abstract fields for the scenes to draw from. No picture of the
// room leaves this module, only these fields (all w x h, row 0 at the top, values 0..1 unless noted):
//   level   heavily blurred, auto-levelled brightness: the rough shape of whoever is in front of the camera
//   motion  where things are moving now (decays fast)
//   trail   where things moved recently (decays slowly)
//   gx, gy  gradient of level, per pixel
//   flow    optical flow (Lucas-Kanade) at half resolution, interleaved x,y, in image widths/heights per second
// plus energy (fraction of the image moving), cx/cy (centre of the movement) and hot (indices of moving pixels),
// and coarse zones (cols x rows, row 0 at the top) with the mean motion, level and flow in each, for the sound.
// Pure code with no DOM, so it can be unit tested in Node.

export function boxBlur(src, dst, tmp, w, h, r) {
  const k = 1 / (2 * r + 1);
  for (let y = 0; y < h; y++) {
    const o = y * w;
    let s = 0;
    for (let i = -r; i <= r; i++) s += src[o + Math.min(Math.max(i, 0), w - 1)];
    for (let x = 0; x < w; x++) {
      tmp[o + x] = s * k;
      s += src[o + Math.min(x + r + 1, w - 1)] - src[o + Math.max(x - r, 0)];
    }
  }
  for (let x = 0; x < w; x++) {
    let s = 0;
    for (let i = -r; i <= r; i++) s += tmp[Math.min(Math.max(i, 0), h - 1) * w + x];
    for (let y = 0; y < h; y++) {
      dst[y * w + x] = s * k;
      s += tmp[Math.min(y + r + 1, h - 1) * w + x] - tmp[Math.max(y - r, 0) * w + x];
    }
  }
}

// Value below which the given fraction of arr lies, for arr values in [lo, hi), from a histogram.
export function percentile(arr, frac, lo = 0, hi = 1, bins = 128, hist = new Uint32Array(bins)) {
  hist.fill(0);
  const s = bins / (hi - lo);
  for (let i = 0; i < arr.length; i++) hist[Math.min(bins - 1, Math.max(0, ((arr[i] - lo) * s) | 0))]++;
  const target = frac * arr.length;
  let acc = 0;
  for (let b = 0; b < bins; b++) {
    acc += hist[b];
    if (acc >= target) return lo + (b + 0.5) / s;
  }
  return hi;
}

const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);

export class Analyzer {
  constructor(w = 128, h = 72) {
    this.w = w; this.h = h;
    const n = w * h, F = (k = n) => new Float32Array(k);
    this.gray = F(); this.cur = F(); this.prev = F(); this.tmp = F(); this.tmp2 = F();
    this.level = F(); this.motion = F(); this.trail = F(); this.fresh = F(); this.gx = F(); this.gy = F();
    this.fw = w >> 1; this.fh = h >> 1;
    const fn = this.fw * this.fh;
    this.hc = F(fn); this.hp = F(fn);
    this.sxx = F(fn); this.syy = F(fn); this.sxy = F(fn); this.sxt = F(fn); this.syt = F(fn);
    this.flow = F(fn * 2);
    this.packed = F(n * 4);   // RGBA for the GPU: level, motion, trail, 0
    this.hot = new Int32Array(n); this.hotN = 0;
    this.hist = new Uint32Array(128);
    this.lo = 0; this.hi = 1; this.noise = 0.01;
    this.energy = 0; this.cx = 0.5; this.cy = 0.5;
    this.frames = 0;
    this.cols = 6; this.rows = 3;
    const z = this.cols * this.rows;
    this.zMotion = F(z); this.zLevel = F(z); this.zFlowX = F(z); this.zFlowY = F(z);
  }

  // rgba: w*h*4 bytes (already mirrored), dt: seconds since the previous frame.
  update(rgba, dt = 1 / 30) {
    const { w, h, gray, cur, prev, tmp } = this, n = w * h;
    dt = Math.min(Math.max(dt, 1 / 120), 0.25);
    for (let i = 0, j = 0; i < n; i++, j += 4) gray[i] = (0.299 * rgba[j] + 0.587 * rgba[j + 1] + 0.114 * rgba[j + 2]) / 255;
    boxBlur(gray, cur, tmp, w, h, 1);

    // Auto levels, so a dim room still gives a full-range field.
    const lo = percentile(cur, 0.02, 0, 1, 128, this.hist), hi = percentile(cur, 0.98, 0, 1, 128, this.hist);
    const first = this.frames++ === 0;
    const ka = first ? 1 : 1 - Math.exp(-dt * 2);
    this.lo += (lo - this.lo) * ka;
    this.hi += (hi - this.hi) * ka;
    const span = Math.max(this.hi - this.lo, 0.08);

    if (first) prev.set(cur);

    // Motion: frame difference with the global change (auto exposure, flicker) removed, over a noise floor.
    let md = 0;
    for (let i = 0; i < n; i++) md += cur[i] - prev[i];
    md /= n;
    const d = this.tmp2;
    for (let i = 0; i < n; i++) d[i] = Math.abs(cur[i] - prev[i] - md) / span;
    const med = percentile(d, 0.5, 0, 0.25, 128, this.hist);
    this.noise += (med - this.noise) * (first ? 1 : 0.05);
    const thr = Math.max(0.03, this.noise * 3);
    const fresh = this.fresh;
    for (let i = 0; i < n; i++) fresh[i] = clamp01((d[i] - thr) / 0.12);
    boxBlur(fresh, fresh, tmp, w, h, 1);
    const fastK = Math.pow(0.55, dt * 30), slowK = Math.pow(0.93, dt * 30);
    let e = 0, sx = 0, sy = 0, hotN = 0;
    for (let y = 0, i = 0; y < h; y++) {
      for (let x = 0; x < w; x++, i++) {
        const m = fresh[i];
        this.motion[i] = Math.max(m, this.motion[i] * fastK);
        this.trail[i] = Math.max(m, this.trail[i] * slowK);
        if (m > 0.05) { e += m; sx += m * x; sy += m * y; }
        if (m > 0.35) this.hot[hotN++] = i;
      }
    }
    this.hotN = hotN;
    this.energy = e / n;
    if (e > 0.5) {
      const kc = 1 - Math.exp(-dt * 6);
      this.cx += (sx / e / (w - 1) - this.cx) * kc;
      this.cy += (sy / e / (h - 1) - this.cy) * kc;
    }

    // Level: normalised, blurred hard enough to lose all detail, and smoothed over time.
    const t2 = this.tmp2;
    for (let i = 0; i < n; i++) t2[i] = clamp01((cur[i] - this.lo) / span);
    boxBlur(t2, t2, tmp, w, h, 2);
    boxBlur(t2, t2, tmp, w, h, 2);
    const kl = first ? 1 : 1 - Math.exp(-dt * 8);
    const L = this.level;
    for (let i = 0; i < n; i++) L[i] += (t2[i] - L[i]) * kl;
    for (let y = 0, i = 0; y < h; y++) {
      for (let x = 0; x < w; x++, i++) {
        this.gx[i] = (L[y * w + Math.min(x + 1, w - 1)] - L[y * w + Math.max(x - 1, 0)]) * 0.5;
        this.gy[i] = (L[Math.min(y + 1, h - 1) * w + x] - L[Math.max(y - 1, 0) * w + x]) * 0.5;
      }
    }

    this.computeFlow(md, span, dt, first);
    prev.set(cur);

    this.computeZones();
    const P = this.packed;
    for (let i = 0, j = 0; i < n; i++, j += 4) { P[j] = L[i]; P[j + 1] = this.motion[i]; P[j + 2] = this.trail[i]; }
  }

  // Lucas-Kanade at half resolution: per pixel, solve the 2x2 system over a 5x5 window. Only kept where
  // something is actually moving, so camera noise doesn't make the art twitch.
  computeFlow(md, span, dt, first) {
    const { w, fw, fh, cur, prev, hc, hp, tmp } = this, fn = fw * fh;
    const inv = 1 / (4 * span);
    for (let y = 0; y < fh; y++) {
      for (let x = 0; x < fw; x++) {
        const a = 2 * y * w + 2 * x, b = a + w;
        hc[y * fw + x] = (cur[a] + cur[a + 1] + cur[b] + cur[b + 1]) * inv;
        hp[y * fw + x] = (prev[a] + prev[a + 1] + prev[b] + prev[b + 1] - 4 * md) * inv;
      }
    }
    boxBlur(hc, hc, tmp, fw, fh, 1);
    boxBlur(hp, hp, tmp, fw, fh, 1);
    const { sxx, syy, sxy, sxt, syt } = this;
    for (let y = 0; y < fh; y++) {
      for (let x = 0; x < fw; x++) {
        const i = y * fw + x;
        const xl = y * fw + Math.max(x - 1, 0), xr = y * fw + Math.min(x + 1, fw - 1);
        const yu = Math.max(y - 1, 0) * fw + x, yd = Math.min(y + 1, fh - 1) * fw + x;
        const ix = (hc[xr] - hc[xl] + hp[xr] - hp[xl]) * 0.25;
        const iy = (hc[yd] - hc[yu] + hp[yd] - hp[yu]) * 0.25;
        const it = hc[i] - hp[i];
        sxx[i] = ix * ix; syy[i] = iy * iy; sxy[i] = ix * iy; sxt[i] = ix * it; syt[i] = iy * it;
      }
    }
    for (const s of [sxx, syy, sxy, sxt, syt]) boxBlur(s, s, tmp, fw, fh, 2);
    const kf = first ? 0 : 0.45, MAXV = 3;
    for (let y = 0; y < fh; y++) {
      for (let x = 0; x < fw; x++) {
        const i = y * fw + x;
        const a = sxx[i], b = sxy[i], c = syy[i];
        const det = a * c - b * b, tr = a + c;
        const lmin = (tr - Math.sqrt(Math.max(tr * tr - 4 * det, 0))) / 2;
        let vx = 0, vy = 0;
        const j = 2 * y * w + 2 * x;
        const m = Math.max(this.fresh[j], this.fresh[j + 1], this.fresh[j + w], this.fresh[j + w + 1]);
        if (lmin > 2e-6 && m > 0.02) {
          vx = (-c * sxt[i] + b * syt[i]) / det;
          vy = (b * sxt[i] - a * syt[i]) / det;
          const mag = Math.hypot(vx, vy);
          if (mag > MAXV) { vx *= MAXV / mag; vy *= MAXV / mag; }
          vx *= Math.min(1, m * 2); vy *= Math.min(1, m * 2);
        }
        // half-res px per frame -> image widths (heights) per second
        vx = vx / fw / dt; vy = vy / fh / dt;
        this.flow[2 * i] = this.flow[2 * i] * kf + vx * (1 - kf);
        this.flow[2 * i + 1] = this.flow[2 * i + 1] * kf + vy * (1 - kf);
      }
    }
  }

  computeZones() {
    const { w, h, fw, fh, cols, rows, zMotion, zLevel, zFlowX, zFlowY } = this;
    zMotion.fill(0); zLevel.fill(0); zFlowX.fill(0); zFlowY.fill(0);
    for (let y = 0, i = 0; y < h; y++) {
      const zr = Math.min(rows - 1, ((y * rows) / h) | 0) * cols;
      for (let x = 0; x < w; x++, i++) {
        const k = zr + Math.min(cols - 1, ((x * cols) / w) | 0);
        zMotion[k] += this.motion[i]; zLevel[k] += this.level[i];
      }
    }
    for (let y = 0, i = 0; y < fh; y++) {
      const zr = Math.min(rows - 1, ((y * rows) / fh) | 0) * cols;
      for (let x = 0; x < fw; x++, i++) {
        const k = zr + Math.min(cols - 1, ((x * cols) / fw) | 0);
        zFlowX[k] += this.flow[2 * i]; zFlowY[k] += this.flow[2 * i + 1];
      }
    }
    const a = (cols * rows) / (w * h), b = (cols * rows) / (fw * fh);
    for (let k = 0; k < cols * rows; k++) { zMotion[k] *= a; zLevel[k] *= a; zFlowX[k] *= b; zFlowY[k] *= b; }
  }
}
