
function v2(x, y) { return new globalThis.Vec2(x, y); }
export function angleVector2(angle) {
    const r = angle * Math.PI / 180;
    return v2(Math.cos(r), Math.sin(r));
}
export function vectorAngle2(v) {
    return Math.atan2(v.y, v.x) * 180 / Math.PI;
}
export function magnitude(v) { return Math.sqrt(v.x*v.x + v.y*v.y); }
export function normalize(v) {
    const m = Math.sqrt(v.x*v.x + v.y*v.y) || 1;
    return v2(v.x / m, v.y / m);
}
export function dot(a, b) { return a.x*b.x + a.y*b.y; }
export function distance(a, b) {
    const dx = a.x - b.x, dy = a.y - b.y;
    return Math.sqrt(dx*dx + dy*dy);
}
