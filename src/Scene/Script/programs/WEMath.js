
export function mix(a, b, t) { return a + (b - a) * t; }
export function lerp(a, b, t) { return a + (b - a) * t; }
export function clamp(x, lo, hi) {
    return Math.max(lo, Math.min(hi, x));
}
export function saturate(x) { return Math.max(0, Math.min(1, x)); }
function smoothstep_impl(edge0, edge1, x) {
    const t = Math.max(0, Math.min(1, (x - edge0) / (edge1 - edge0)));
    return t * t * (3 - 2 * t);
}
// Corpus uses both casings; smoothStep (camelCase) is by far the more
// common form (~165 callsites vs lowercase).
export const smoothstep = smoothstep_impl;
export const smoothStep = smoothstep_impl;
export function step(edge, x) { return x < edge ? 0 : 1; }
export function sign(x) { return Math.sign(x); }
export function fract(x) { return x - Math.floor(x); }
export function deg2rad(d) { return d * (Math.PI / 180); }
export function rad2deg(r) { return r * (180 / Math.PI); }
deg2rad.valueOf = () => Math.PI / 180;
rad2deg.valueOf = () => 180 / Math.PI;
