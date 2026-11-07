
function v3(x, y, z) { return new globalThis.Vec3(x, y, z); }
function c(v, k, i) {
    if (v && typeof v === 'object') return Number(v[k] ?? v[i] ?? 0);
    return Number(v ?? 0);
}
function clamp01(x) { return Math.max(0, Math.min(1, x)); }
export function normalizeColor(rgb) {
    return v3(c(rgb, 'x', 0) / 255, c(rgb, 'y', 1) / 255, c(rgb, 'z', 2) / 255);
}
export function expandColor(rgb) {
    return v3(c(rgb, 'x', 0) * 255, c(rgb, 'y', 1) * 255, c(rgb, 'z', 2) * 255);
}
export function rgb2hsv(rgb) {
    const r = clamp01(c(rgb, 'x', 0));
    const g = clamp01(c(rgb, 'y', 1));
    const b = clamp01(c(rgb, 'z', 2));
    const max = Math.max(r, g, b);
    const min = Math.min(r, g, b);
    const d = max - min;
    let h = 0;
    if (d !== 0) {
        if (max === r) h = ((g - b) / d) % 6;
        else if (max === g) h = (b - r) / d + 2;
        else h = (r - g) / d + 4;
        h /= 6;
        if (h < 0) h += 1;
    }
    const s = max === 0 ? 0 : d / max;
    return v3(h, s, max);
}
export function hsv2rgb(hsv) {
    let h = c(hsv, 'x', 0) % 1;
    if (h < 0) h += 1;
    const s = clamp01(c(hsv, 'y', 1));
    const v = clamp01(c(hsv, 'z', 2));
    const i = Math.floor(h * 6);
    const f = h * 6 - i;
    const p = v * (1 - s);
    const q = v * (1 - f * s);
    const t = v * (1 - (1 - f) * s);
    switch (i % 6) {
    case 0: return v3(v, t, p);
    case 1: return v3(q, v, p);
    case 2: return v3(p, v, t);
    case 3: return v3(p, q, v);
    case 4: return v3(t, p, v);
    default: return v3(v, p, q);
    }
}
