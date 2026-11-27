// Reference copy of js/presence.js from Wake (the browser version), for tests/parity.sh.
// Presence: whoever is in view, as opposed to the room behind them. The background's level is taken from the left
// and right edges of the frame and its top corners, which are nearly always wall (the middle of the top edge is
// often your head, with a laptop camera); anything that differs from it, lighter or darker, counts as presence. So dark hair or a lamp behind you works as well as a lit face against a dark room.
// Pure code with no DOM (shaders have the same rule as presenceOf() in gl.js).

export function backgroundLevel(an) {
  const { w, h, level } = an, vals = [];
  const side = Math.max(2, Math.round(w * 0.1)), corner = Math.round(w * 0.2);
  for (let y = 0; y < h * 0.85; y += 2) for (let x = 0; x < side; x += 2) vals.push(level[y * w + x], level[y * w + w - 1 - x]);
  for (let x = 0; x < corner; x += 2) vals.push(level[x], level[w - 1 - x], level[w + x], level[2 * w - 1 - x]);
  vals.sort((a, b) => a - b);
  return vals[vals.length >> 1];
}

// Presence (0..1) at index k of the analysis grid.
export function presence(an, bg, k) {
  const t = Math.min(1, Math.max(0, (Math.abs(an.level[k] - bg) - 0.12) / 0.28));
  return t * t * (3 - 2 * t);
}
