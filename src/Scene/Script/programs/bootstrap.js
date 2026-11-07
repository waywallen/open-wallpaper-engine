
(() => {
  const nativeExec = RegExp.prototype.exec;
  const defineRegExpStatic = (name, value) => {
    Object.defineProperty(RegExp, name, {
      value,
      writable: true,
      configurable: true,
      enumerable: false,
    });
  };
  for (let i = 1; i <= 9; ++i) defineRegExpStatic('$' + i, '');
  defineRegExpStatic('$_', '');
  defineRegExpStatic('input', '');
  defineRegExpStatic('$&', '');
  defineRegExpStatic('lastMatch', '');
  defineRegExpStatic('$+', '');
  defineRegExpStatic('lastParen', '');
  defineRegExpStatic('$`', '');
  defineRegExpStatic('leftContext', '');
  defineRegExpStatic("$'", '');
  defineRegExpStatic('rightContext', '');

  const updateRegExpStatics = (match) => {
    if (!match) return;
    const input = String(match.input ?? '');
    const index = match.index ?? 0;
    const text = match[0] ?? '';
    for (let i = 1; i <= 9; ++i) RegExp['$' + i] = match[i] ?? '';
    RegExp.$_ = input;
    RegExp.input = input;
    RegExp['$&'] = text;
    RegExp.lastMatch = text;
    RegExp['$+'] = '';
    RegExp.lastParen = '';
    for (let i = match.length - 1; i >= 1; --i) {
      if (match[i] !== undefined) {
        RegExp['$+'] = match[i];
        RegExp.lastParen = match[i];
        break;
      }
    }
    RegExp['$`'] = input.slice(0, index);
    RegExp.leftContext = RegExp['$`'];
    RegExp["$'"] = input.slice(index + text.length);
    RegExp.rightContext = RegExp["$'"];
  };

  RegExp.prototype.exec = function(str) {
    const match = nativeExec.call(this, str);
    updateRegExpStatics(match);
    return match;
  };
  RegExp.prototype.test = function(str) {
    return this.exec(str) !== null;
  };
})();

globalThis.createScriptProperties = function () {
  const _props = [];
  const _byName = new Map();
  const builder = {
    _byName,
    _props,
  };
  const adder = (kind) => (opts) => {
    const d = Object.assign({ kind }, opts);
    _props.push(d);
    if (d && d.name) _byName.set(d.name, d);
    return builder;
  };
  builder.addSlider    = adder('Slider');
  builder.addCheckbox  = adder('Checkbox');
  builder.addText      = adder('Text');
  builder.addCombo     = adder('Combo');
  builder.addColor     = adder('Color');
  builder.addDelimiter = adder('Delimiter');
  // Stubs for the long tail surfaced by wpscriptdump (Animation,
  // Interpolator, AniMapper, Task, ChangedUserProperty, Listener,
  // SpaceToTimeDelimiter, SpaceToDateDelimiter, Value): no-op, returns
  // builder so `.addX().addY().finish()` chains keep parsing.
  for (const k of ['Animation','Interpolator','AniMapper','Task',
                   'ChangedUserProperty','Listener',
                   'SpaceToTimeDelimiter','SpaceToDateDelimiter','Value']) {
    builder['add' + k] = adder(k);
  }
  // .finish() returns a Proxy. Property reads:
  //   - scriptProperties.<name> : look up in _hostValues (filled by C++),
  //                               else default value from descriptor.
  //   When _hostValues[name] is a {user, value} pair, resolve at access
  //   time against engine.userProperties so SetUserProperty calls made
  //   after parse propagate.
  builder.finish = function () {
    const _hostValues = builder._hostValues || {};
    const target = {};
    const userValue = (u) => {
      if (typeof u === 'object' && u !== null && 'value' in u) return u.value;
      return u;
    };
    const sameScalar = (a, b) => String(a) === String(b);
    const unwrapUserProp = (h) => {
      if (h === undefined || h === null) return undefined;
      if (typeof h !== 'object' || !('user' in h) || !('value' in h)) return h;
      if (typeof h.user === 'object') {
        const gate = h.user;
        if (gate && typeof gate.name === 'string') {
          const u = engine.userProperties[gate.name];
          if (u !== undefined) return sameScalar(userValue(u), gate.condition);
        }
        return unwrapUserProp(h.value);
      }
      const u = engine.userProperties[h.user];
      if (u !== undefined) {
        // project.json stores user props as { type, value, ... }; pluck
        // .value when present, else use the bare value directly.
        return userValue(u);
      }
      return h.value;
    };
    const coerceDescriptorValue = (descriptor, value) => {
      if (!descriptor || descriptor.kind !== 'Color') return value;
      if (typeof value === 'string') {
        const components = value.trim().split(/\s+/).map(Number);
        return new globalThis.Vec3(components[0] ?? 0, components[1] ?? 0, components[2] ?? 0);
      }
      if (Array.isArray(value)) {
        return new globalThis.Vec3(value[0] ?? 0, value[1] ?? 0, value[2] ?? 0);
      }
      return value;
    };
    // WE substitutes user-prop values verbatim, even when the user's
    // slider range is wider than the script's declared range — corpus
    // wallpapers (e.g. workshop 3327063360) wire `min:-1,max:1` sliders
    // into scripts declaring `min:0,max:1` and rely on the negative
    // values reaching the formula to shift origin off-parent.
    for (const d of _props) {
      if (d && d.name) {
        Object.defineProperty(target, d.name, {
          enumerable: true,
          configurable: true,
          get() {
            if (Object.prototype.hasOwnProperty.call(_hostValues, d.name))
              return coerceDescriptorValue(d, unwrapUserProp(_hostValues[d.name]));
            return d.value;
          },
        });
      }
    }
    target.__descriptors = _props;
    target.__hostValues  = _hostValues;
    return target;
  };
  // Host writes here before evaluating the script body (per FieldScript)
  // to override defaults.
  builder._hostValues = {};
  return builder;
};
// WE editor exposes a real console; renderer scripts that log diagnostics
// touch it from init/update. Provide a no-op shim so unguarded calls
// don't throw ReferenceError mid module-body — that would leave the
// remaining const/let declarations in TDZ and break unrelated callbacks.
if (! globalThis.console) {
    const __noop = function() {};
    globalThis.console = {
        log: __noop, info: __noop, warn: __noop, error: __noop,
        debug: __noop, trace: __noop, dir: __noop, assert: __noop,
        group: __noop, groupCollapsed: __noop, groupEnd: __noop,
    };
}

// engine.userProperties is a plain object the host can mutate.
if (! globalThis.engine) globalThis.engine = {};
globalThis.engine.userProperties = {};
globalThis.engine.AUDIO_RESOLUTION_16 = 16;
globalThis.engine.AUDIO_RESOLUTION_32 = 32;
globalThis.engine.AUDIO_RESOLUTION_64 = 64;
// WE exposes these as zero-arg query functions; some scripts call them
// (`engine.isRunningInEditor()`), others read as boolean. Provide a
// callable that also coerces to false when accessed as a value (the
// function object is truthy, but scripts that use `if (engine.isRunningInEditor)`
// still see truthy → they branch into the "running in editor" path. The
// corpus only ever calls it, so callable-form is the safer default).
globalThis.engine.isRunningInEditor = function() { return false; };
globalThis.engine.isScreensaver     = function() { return false; };

// --- SceneScript Core Foundational Math Classes & Host State ---
const _Epsilon = 0.00001;
const deg2rad = Math.PI / 180;
const rad2deg = 180 / Math.PI;

class Vec2 {
	constructor(x,y){
		if(typeof x === 'string'){
			x=x.split(' ');
			this.x=parseFloat(x[0]);
			this.y=parseFloat(x[1]);
		}else if(x instanceof Vec3){
			this.x=x.x;
			this.y=x.y;
		}else if(x instanceof Vec2){
			this.x=x.x;
			this.y=x.y;
		}else if(typeof x === 'object' && x !== null){
			this.x=Number(x.x ?? 0);
			this.y=Number(x.y ?? 0);
		}else if(typeof x!=='undefined'){
			this.x=x;
			this.y=(typeof y==='number')?y:x;
		}else{
			this.x = 0;
			this.y = 0;
		}
	}
	length(){
		return Math.sqrt(this.x*this.x+this.y*this.y);
	}
	lengthSqr(){
		return this.x*this.x+this.y*this.y;
	}
	distance(v){
		const dx = this.x-v.x, dy = this.y-v.y;
		return Math.sqrt(dx*dx+dy*dy);
	}
	distanceSqr(v){
		const dx = this.x-v.x, dy = this.y-v.y;
		return dx*dx+dy*dy;
	}
	normalize(){
		return this.divide(this.length() || 1);
	}
	copy(){
		return new Vec2(
			this.x,
			this.y
		);
	}
	clone(){
		return this.copy();
	}
	equals(f){
		if(!(f instanceof Vec2)){ return false; }
		return Math.abs(this.x-f.x) < _Epsilon &&
			Math.abs(this.y-f.y) < _Epsilon;
	}
	isFinite(){
		return Number.isFinite(this.x) && Number.isFinite(this.y);
	}
	negate(){
		return new Vec2(-this.x, -this.y);
	}
	add(f){
		if(typeof f === 'number'){
			return new Vec2(
				this.x+f,
				this.y+f
			);
		}else{
			return new Vec2(
				this.x+(f.x ?? f),
				this.y+(f.y ?? f)
			);
		}
	}
	subtract(f){
		if(typeof f === 'number'){
			return new Vec2(
				this.x-f,
				this.y-f
			);
		}else{
			return new Vec2(
				this.x-(f.x ?? f),
				this.y-(f.y ?? f)
			);
		}
	}
	multiply(f){
		if(typeof f === 'number'){
			return new Vec2(
				this.x*f,
				this.y*f
			);
		}else{
			return new Vec2(
				this.x*(f.x ?? f),
				this.y*(f.y ?? f)
			);
		}
	}
	divide(f){
		if(typeof f === 'number'){
			return new Vec2(
				this.x/f,
				this.y/f
			);
		}else{
			return new Vec2(
				this.x/(f.x ?? f),
				this.y/(f.y ?? f)
			);
		}
	}
	dot(f){
		return this.x*f.x+this.y*f.y;
	}
	reflect(f){
		return this.subtract(f.multiply(2*this.dot(f)));
	}
	perpendicular(){
		return new Vec2(this.y, -this.x);
	}
	project(v){
		const d = v.lengthSqr();
		if(d === 0){ return new Vec2(0, 0); }
		return v.multiply(this.dot(v)/d);
	}
	angle(){
		return Math.atan2(this.y, this.x) * rad2deg;
	}
	angleBetween(v){
		return Math.atan2(this.x*v.y - this.y*v.x, this.x*v.x + this.y*v.y) * rad2deg;
	}
	rotate(angle){
		const r = angle * deg2rad;
		const c = Math.cos(r), s = Math.sin(r);
		return new Vec2(c*this.x - s*this.y, s*this.x + c*this.y);
	}
	mix(v, a){
		if(typeof a === 'number'){
			return new Vec2(
				this.x+((v.x ?? v)-this.x)*a,
				this.y+((v.y ?? v)-this.y)*a
			);
		}else{
			return new Vec2(
				this.x+(v.x-this.x)*a.x,
				this.y+(v.y-this.y)*a.y
			);
		}
	}
	min(v){
		if(typeof v === 'number'){
			return new Vec2(
				Math.min(this.x, v),
				Math.min(this.y, v)
			);
		}else{
			return new Vec2(
				Math.min(this.x, v.x),
				Math.min(this.y, v.y)
			);
		}
	}
	max(v){
		if(typeof v === 'number'){
			return new Vec2(
				Math.max(this.x, v),
				Math.max(this.y, v)
			);
		}else{
			return new Vec2(
				Math.max(this.x, v.x),
				Math.max(this.y, v.y)
			);
		}
	}
	clamp(min, max){
		const minX = (typeof min === 'number') ? min : min.x;
		const minY = (typeof min === 'number') ? min : min.y;
		const maxX = (typeof max === 'number') ? max : max.x;
		const maxY = (typeof max === 'number') ? max : max.y;
		return new Vec2(
			Math.max(minX, Math.min(maxX, this.x)),
			Math.max(minY, Math.min(maxY, this.y))
		);
	}
	abs(){
		return new Vec2(Math.abs(this.x), Math.abs(this.y));
	}
	sign(){
		return new Vec2(Math.sign(this.x), Math.sign(this.y));
	}
	round(){
		return new Vec2(Math.round(this.x), Math.round(this.y));
	}
	floor(){
		return new Vec2(Math.floor(this.x), Math.floor(this.y));
	}
	ceil(){
		return new Vec2(Math.ceil(this.x), Math.ceil(this.y));
	}
	fract(){
		return new Vec2(this.x - Math.floor(this.x), this.y - Math.floor(this.y));
	}
	mod(v){
		if(typeof v === 'number'){
			return new Vec2(
				this.x - v*Math.floor(this.x/v),
				this.y - v*Math.floor(this.y/v)
			);
		}else{
			return new Vec2(
				this.x - v.x*Math.floor(this.x/v.x),
				this.y - v.y*Math.floor(this.y/v.y)
			);
		}
	}
	step(edge){
		const ex = (typeof edge === 'number') ? edge : edge.x;
		const ey = (typeof edge === 'number') ? edge : edge.y;
		return new Vec2(this.x < ex ? 0 : 1, this.y < ey ? 0 : 1);
	}
	smoothStep(min, max){
		const e0x = (typeof min === 'number') ? min : min.x;
		const e0y = (typeof min === 'number') ? min : min.y;
		const e1x = (typeof max === 'number') ? max : max.x;
		const e1y = (typeof max === 'number') ? max : max.y;
		let tx = Math.max(0, Math.min(1, (this.x - e0x) / (e1x - e0x)));
		let ty = Math.max(0, Math.min(1, (this.y - e0y) / (e1y - e0y)));
		tx = tx*tx*(3 - 2*tx);
		ty = ty*ty*(3 - 2*ty);
		return new Vec2(tx, ty);
	}
	toString(){
		return this.x+' '+this.y;
	}
	toConfigString(){
		return this.toString();
	}
}

class Vec3{
	constructor(x,y,z){
		if(typeof x === 'string'){
			x=x.split(' ');
			this.x=parseFloat(x[0]);
			this.y=parseFloat(x[1]);
			this.z=parseFloat(x[2]);
		}else if(x instanceof Vec3){
			this.x=x.x;
			this.y=x.y;
			this.z=x.z;
		}else if(x instanceof Vec2){
			this.x=x.x;
			this.y=x.y;
			this.z=0;
		}else if(typeof x === 'object' && x !== null){
			this.x=Number(x.x ?? 0);
			this.y=Number(x.y ?? 0);
			this.z=Number(x.z ?? 0);
		}else if(typeof x!=='undefined'){
			this.x=x;
			this.y=(typeof y==='number')?y:x;
			this.z=(typeof z==='number')?z:((typeof y==='number')?0:x);
		}else{
			this.x = 0;
			this.y = 0;
			this.z = 0;
		}
	}
	static fromSpherical(r, theta, phi){
		const t = theta * deg2rad;
		const p = phi * deg2rad;
		const st = Math.sin(t);
		return new Vec3(
			r*st*Math.cos(p),
			r*Math.cos(t),
			r*st*Math.sin(p)
		);
	}
	length(){
		return Math.sqrt(this.x*this.x+this.y*this.y+this.z*this.z);
	}
	lengthSqr(){
		return this.x*this.x+this.y*this.y+this.z*this.z;
	}
	distance(v){
		const dx = this.x-v.x, dy = this.y-v.y, dz = this.z-v.z;
		return Math.sqrt(dx*dx+dy*dy+dz*dz);
	}
	distanceSqr(v){
		const dx = this.x-v.x, dy = this.y-v.y, dz = this.z-v.z;
		return dx*dx+dy*dy+dz*dz;
	}
	normalize(){
		return this.divide(this.length() || 1);
	}
	copy(){
		return new Vec3(
			this.x,
			this.y,
			this.z
		);
	}
	clone(){
		return this.copy();
	}
	equals(f){
		if(!(f instanceof Vec3)){ return false; }
		return Math.abs(this.x-f.x) < _Epsilon &&
			Math.abs(this.y-f.y) < _Epsilon &&
			Math.abs(this.z-f.z) < _Epsilon;
	}
	isFinite(){
		return Number.isFinite(this.x) && Number.isFinite(this.y) && Number.isFinite(this.z);
	}
	negate(){
		return new Vec3(-this.x, -this.y, -this.z);
	}
	add(f){
		if(typeof f === 'number'){
			return new Vec3(
				this.x+f,
				this.y+f,
				this.z+f
			);
		}else if(f instanceof Vec2){
			return new Vec3(
				this.x+f.x,
				this.y+f.y,
				this.z
			);
		}else{
			return new Vec3(
				this.x+(f.x ?? f),
				this.y+(f.y ?? f),
				this.z+(f.z ?? f)
			);
		}
	}
	subtract(f){
		if(typeof f === 'number'){
			return new Vec3(
				this.x-f,
				this.y-f,
				this.z-f
			);
		}else if(f instanceof Vec2){
			return new Vec3(
				this.x-f.x,
				this.y-f.y,
				this.z
			);
		}else{
			return new Vec3(
				this.x-(f.x ?? f),
				this.y-(f.y ?? f),
				this.z-(f.z ?? f)
			);
		}
	}
	multiply(f){
		if(typeof f === 'number'){
			return new Vec3(
				this.x*f,
				this.y*f,
				this.z*f
			);
		}else if(f instanceof Vec2){
			return new Vec3(
				this.x*f.x,
				this.y*f.y,
				this.z
			);
		}else{
			return new Vec3(
				this.x*(f.x ?? f),
				this.y*(f.y ?? f),
				this.z*(f.z ?? f)
			);
		}
	}
	divide(f){
		if(typeof f === 'number'){
			return new Vec3(
				this.x/f,
				this.y/f,
				this.z/f
			);
		}else if(f instanceof Vec2){
			return new Vec3(
				this.x/f.x,
				this.y/f.y,
				this.z
			);
		}else{
			return new Vec3(
				this.x/(f.x ?? f),
				this.y/(f.y ?? f),
				this.z/(f.z ?? f)
			);
		}
	}
	cross(f){
		return new Vec3(
			this.y*f.z-this.z*f.y,
			this.z*f.x-this.x*f.z,
			this.x*f.y-this.y*f.x
		);
	}
	dot(f){
		return this.x*f.x+this.y*f.y+this.z*f.z;
	}
	reflect(f){
		return this.subtract(f.multiply(2*this.dot(f)));
	}
	refract(normal, eta){
		const NdotI = normal.dot(this);
		const k = 1 - eta*eta * (1 - NdotI*NdotI);
		if(k < 0){ return new Vec3(0, 0, 0); }
		return this.multiply(eta).subtract(normal.multiply(eta*NdotI + Math.sqrt(k)));
	}
	project(v){
		const d = v.lengthSqr();
		if(d === 0){ return new Vec3(0, 0, 0); }
		return v.multiply(this.dot(v)/d);
	}
	angleBetween(v){
		const denom = Math.sqrt(this.lengthSqr() * v.lengthSqr());
		if(denom === 0){ return 0; }
		return Math.acos(Math.max(-1, Math.min(1, this.dot(v) / denom))) * rad2deg;
	}
	toSpherical(){
		const r = this.length();
		if(r === 0){ return new Vec3(0, 0, 0); }
		return new Vec3(r, Math.acos(this.y / r) * rad2deg, Math.atan2(this.z, this.x) * rad2deg);
	}
	mix(v, a){
		if(typeof a === 'number'){
			return new Vec3(
				this.x+((v.x ?? v)-this.x)*a,
				this.y+((v.y ?? v)-this.y)*a,
				this.z+((v.z ?? v)-this.z)*a
			);
		}else{
			return new Vec3(
				this.x+(v.x-this.x)*a.x,
				this.y+(v.y-this.y)*a.y,
				this.z+(v.z-this.z)*a.z
			);
		}
	}
	min(v){
		if(typeof v === 'number'){
			return new Vec3(
				Math.min(this.x, v),
				Math.min(this.y, v),
				Math.min(this.z, v)
			);
		}else{
			return new Vec3(
				Math.min(this.x, v.x),
				Math.min(this.y, v.y),
				Math.min(this.z, v.z)
			);
		}
	}
	max(v){
		if(typeof v === 'number'){
			return new Vec3(
				Math.max(this.x, v),
				Math.max(this.y, v),
				Math.max(this.z, v)
			);
		}else{
			return new Vec3(
				Math.max(this.x, v.x),
				Math.max(this.y, v.y),
				Math.max(this.z, v.z)
			);
		}
	}
	clamp(min, max){
		const minX = (typeof min === 'number') ? min : min.x;
		const minY = (typeof min === 'number') ? min : min.y;
		const minZ = (typeof min === 'number') ? min : min.z;
		const maxX = (typeof max === 'number') ? max : max.x;
		const maxY = (typeof max === 'number') ? max : max.y;
		const maxZ = (typeof max === 'number') ? max : max.z;
		return new Vec3(
			Math.max(minX, Math.min(maxX, this.x)),
			Math.max(minY, Math.min(maxY, this.y)),
			Math.max(minZ, Math.min(maxZ, this.z))
		);
	}
	abs(){
		return new Vec3(Math.abs(this.x), Math.abs(this.y), Math.abs(this.z));
	}
	sign(){
		return new Vec3(Math.sign(this.x), Math.sign(this.y), Math.sign(this.z));
	}
	round(){
		return new Vec3(Math.round(this.x), Math.round(this.y), Math.round(this.z));
	}
	floor(){
		return new Vec3(Math.floor(this.x), Math.floor(this.y), Math.floor(this.z));
	}
	ceil(){
		return new Vec3(Math.ceil(this.x), Math.ceil(this.y), Math.ceil(this.z));
	}
	fract(){
		return new Vec3(
			this.x - Math.floor(this.x),
			this.y - Math.floor(this.y),
			this.z - Math.floor(this.z)
		);
	}
	mod(v){
		if(typeof v === 'number'){
			return new Vec3(
				this.x - v*Math.floor(this.x/v),
				this.y - v*Math.floor(this.y/v),
				this.z - v*Math.floor(this.z/v)
			);
		}else{
			return new Vec3(
				this.x - v.x*Math.floor(this.x/v.x),
				this.y - v.y*Math.floor(this.y/v.y),
				this.z - v.z*Math.floor(this.z/v.z)
			);
		}
	}
	step(edge){
		const ex = (typeof edge === 'number') ? edge : edge.x;
		const ey = (typeof edge === 'number') ? edge : edge.y;
		const ez = (typeof edge === 'number') ? edge : edge.z;
		return new Vec3(this.x < ex ? 0 : 1, this.y < ey ? 0 : 1, this.z < ez ? 0 : 1);
	}
	smoothStep(min, max){
		const e0x = (typeof min === 'number') ? min : min.x;
		const e0y = (typeof min === 'number') ? min : min.y;
		const e0z = (typeof min === 'number') ? min : min.z;
		const e1x = (typeof max === 'number') ? max : max.x;
		const e1y = (typeof max === 'number') ? max : max.y;
		const e1z = (typeof max === 'number') ? max : max.z;
		let tx = Math.max(0, Math.min(1, (this.x - e0x) / (e1x - e0x)));
		let ty = Math.max(0, Math.min(1, (this.y - e0y) / (e1y - e0y)));
		let tz = Math.max(0, Math.min(1, (this.z - e0z) / (e1z - e0z)));
		tx = tx*tx*(3 - 2*tx);
		ty = ty*ty*(3 - 2*ty);
		tz = tz*tz*(3 - 2*tz);
		return new Vec3(tx, ty, tz);
	}
	toString(){
		return this.x+' '+this.y+' '+this.z;
	}
	toConfigString(){
		return this.toString();
	}
}

class Vec4 {
	constructor(x,y,z,w){
		if(typeof x === 'string'){
			x=x.split(' ');
			this.x=parseFloat(x[0]);
			this.y=parseFloat(x[1]);
			this.z=parseFloat(x[2]);
			this.w=parseFloat(x[3]);
		}else if(x instanceof Vec4){
			this.x=x.x; this.y=x.y; this.z=x.z; this.w=x.w;
		}else if(x instanceof Vec3){
			this.x=x.x; this.y=x.y; this.z=x.z; this.w=0;
		}else if(x instanceof Vec2){
			this.x=x.x; this.y=x.y; this.z=0; this.w=0;
		}else if(typeof x === 'object' && x !== null){
			this.x=Number(x.x ?? 0);
			this.y=Number(x.y ?? 0);
			this.z=Number(x.z ?? 0);
			this.w=Number(x.w ?? 0);
		}else if(typeof x!=='undefined'){
			this.x=x;
			this.y=(typeof y==='number')?y:x;
			this.z=(typeof z==='number')?z:((typeof y==='number')?0:x);
			this.w=(typeof w==='number')?w:((typeof z==='number')?z:((typeof y==='number')?0:x));
		}else{
			this.x = 0;
			this.y = 0;
			this.z = 0;
			this.w = 0;
		}
	}
	length(){
		return Math.sqrt(this.x*this.x+this.y*this.y+this.z*this.z+this.w*this.w);
	}
	lengthSqr(){
		return this.x*this.x+this.y*this.y+this.z*this.z+this.w*this.w;
	}
	distance(v){
		const dx = this.x-v.x, dy = this.y-v.y, dz = this.z-v.z, dw = this.w-v.w;
		return Math.sqrt(dx*dx+dy*dy+dz*dz+dw*dw);
	}
	distanceSqr(v){
		const dx = this.x-v.x, dy = this.y-v.y, dz = this.z-v.z, dw = this.w-v.w;
		return dx*dx+dy*dy+dz*dz+dw*dw;
	}
	normalize(){
		return this.divide(this.length() || 1);
	}
	copy(){
		return new Vec4(this.x, this.y, this.z, this.w);
	}
	clone(){
		return this.copy();
	}
	equals(f){
		if(!(f instanceof Vec4)){ return false; }
		return Math.abs(this.x-f.x) < _Epsilon &&
			Math.abs(this.y-f.y) < _Epsilon &&
			Math.abs(this.z-f.z) < _Epsilon &&
			Math.abs(this.w-f.w) < _Epsilon;
	}
	isFinite(){
		return Number.isFinite(this.x) && Number.isFinite(this.y) &&
			Number.isFinite(this.z) && Number.isFinite(this.w);
	}
	negate(){
		return new Vec4(-this.x, -this.y, -this.z, -this.w);
	}
	add(f){
		if(typeof f === 'number'){
			return new Vec4(this.x+f, this.y+f, this.z+f, this.w+f);
		}else{
			return new Vec4(this.x+(f.x ?? f), this.y+(f.y ?? f), this.z+(f.z ?? f), this.w+(f.w ?? f));
		}
	}
	subtract(f){
		if(typeof f === 'number'){
			return new Vec4(this.x-f, this.y-f, this.z-f, this.w-f);
		}else{
			return new Vec4(this.x-(f.x ?? f), this.y-(f.y ?? f), this.z-(f.z ?? f), this.w-(f.w ?? f));
		}
	}
	multiply(f){
		if(typeof f === 'number'){
			return new Vec4(this.x*f, this.y*f, this.z*f, this.w*f);
		}else{
			return new Vec4(this.x*(f.x ?? f), this.y*(f.y ?? f), this.z*(f.z ?? f), this.w*(f.w ?? f));
		}
	}
	divide(f){
		if(typeof f === 'number'){
			return new Vec4(this.x/f, this.y/f, this.z/f, this.w/f);
		}else{
			return new Vec4(this.x/(f.x ?? f), this.y/(f.y ?? f), this.z/(f.z ?? f), this.w/(f.w ?? f));
		}
	}
	dot(f){
		return this.x*f.x + this.y*f.y + this.z*f.z + this.w*f.w;
	}
	reflect(f){
		return this.subtract(f.multiply(2*this.dot(f)));
	}
	project(v){
		const d = v.lengthSqr();
		if(d === 0){ return new Vec4(0, 0, 0, 0); }
		return v.multiply(this.dot(v)/d);
	}
	mix(v, a){
		if(typeof a === 'number'){
			return new Vec4(
				this.x+((v.x ?? v)-this.x)*a,
				this.y+((v.y ?? v)-this.y)*a,
				this.z+((v.z ?? v)-this.z)*a,
				this.w+((v.w ?? v)-this.w)*a
			);
		}else{
			return new Vec4(
				this.x+(v.x-this.x)*a.x,
				this.y+(v.y-this.y)*a.y,
				this.z+(v.z-this.z)*a.z,
				this.w+(v.w-this.w)*a.w
			);
		}
	}
	min(v){
		if(typeof v === 'number'){
			return new Vec4(
				Math.min(this.x, v), Math.min(this.y, v),
				Math.min(this.z, v), Math.min(this.w, v)
			);
		}else{
			return new Vec4(
				Math.min(this.x, v.x), Math.min(this.y, v.y),
				Math.min(this.z, v.z), Math.min(this.w, v.w)
			);
		}
	}
	max(v){
		if(typeof v === 'number'){
			return new Vec4(
				Math.max(this.x, v), Math.max(this.y, v),
				Math.max(this.z, v), Math.max(this.w, v)
			);
		}else{
			return new Vec4(
				Math.max(this.x, v.x), Math.max(this.y, v.y),
				Math.max(this.z, v.z), Math.max(this.w, v.w)
			);
		}
	}
	clamp(min, max){
		const minX = (typeof min === 'number') ? min : min.x;
		const minY = (typeof min === 'number') ? min : min.y;
		const minZ = (typeof min === 'number') ? min : min.z;
		const minW = (typeof min === 'number') ? min : min.w;
		const maxX = (typeof max === 'number') ? max : max.x;
		const maxY = (typeof max === 'number') ? max : max.y;
		const maxZ = (typeof max === 'number') ? max : max.z;
		const maxW = (typeof max === 'number') ? max : max.w;
		return new Vec4(
			Math.max(minX, Math.min(maxX, this.x)),
			Math.max(minY, Math.min(maxY, this.y)),
			Math.max(minZ, Math.min(maxZ, this.z)),
			Math.max(minW, Math.min(maxW, this.w))
		);
	}
	abs(){
		return new Vec4(Math.abs(this.x), Math.abs(this.y), Math.abs(this.z), Math.abs(this.w));
	}
	sign(){
		return new Vec4(Math.sign(this.x), Math.sign(this.y), Math.sign(this.z), Math.sign(this.w));
	}
	round(){
		return new Vec4(Math.round(this.x), Math.round(this.y), Math.round(this.z), Math.round(this.w));
	}
	floor(){
		return new Vec4(Math.floor(this.x), Math.floor(this.y), Math.floor(this.z), Math.floor(this.w));
	}
	ceil(){
		return new Vec4(Math.ceil(this.x), Math.ceil(this.y), Math.ceil(this.z), Math.ceil(this.w));
	}
	fract(){
		return new Vec4(
			this.x - Math.floor(this.x),
			this.y - Math.floor(this.y),
			this.z - Math.floor(this.z),
			this.w - Math.floor(this.w)
		);
	}
	mod(v){
		if(typeof v === 'number'){
			return new Vec4(
				this.x - v*Math.floor(this.x/v),
				this.y - v*Math.floor(this.y/v),
				this.z - v*Math.floor(this.z/v),
				this.w - v*Math.floor(this.w/v)
			);
		}else{
			return new Vec4(
				this.x - v.x*Math.floor(this.x/v.x),
				this.y - v.y*Math.floor(this.y/v.y),
				this.z - v.z*Math.floor(this.z/v.z),
				this.w - v.w*Math.floor(this.w/v.w)
			);
		}
	}
	step(edge){
		const ex = (typeof edge === 'number') ? edge : edge.x;
		const ey = (typeof edge === 'number') ? edge : edge.y;
		const ez = (typeof edge === 'number') ? edge : edge.z;
		const ew = (typeof edge === 'number') ? edge : edge.w;
		return new Vec4(
			this.x < ex ? 0 : 1,
			this.y < ey ? 0 : 1,
			this.z < ez ? 0 : 1,
			this.w < ew ? 0 : 1
		);
	}
	smoothStep(min, max){
		const e0x = (typeof min === 'number') ? min : min.x;
		const e0y = (typeof min === 'number') ? min : min.y;
		const e0z = (typeof min === 'number') ? min : min.z;
		const e0w = (typeof min === 'number') ? min : min.w;
		const e1x = (typeof max === 'number') ? max : max.x;
		const e1y = (typeof max === 'number') ? max : max.y;
		const e1z = (typeof max === 'number') ? max : max.z;
		const e1w = (typeof max === 'number') ? max : max.w;
		let tx = Math.max(0, Math.min(1, (this.x - e0x) / (e1x - e0x)));
		let ty = Math.max(0, Math.min(1, (this.y - e0y) / (e1y - e0y)));
		let tz = Math.max(0, Math.min(1, (this.z - e0z) / (e1z - e0z)));
		let tw = Math.max(0, Math.min(1, (this.w - e0w) / (e1w - e0w)));
		tx = tx*tx*(3 - 2*tx);
		ty = ty*ty*(3 - 2*ty);
		tz = tz*tz*(3 - 2*tz);
		tw = tw*tw*(3 - 2*tw);
		return new Vec4(tx, ty, tz, tw);
	}
	toString(){
		return this.x+' '+this.y+' '+this.z+' '+this.w;
	}
	toConfigString(){
		return this.toString();
	}
}

class Mat3 {
	constructor(arg){
		if(arg instanceof Mat3){
			this.m = arg.m.slice();
		}else if(Array.isArray(arg) && arg.length === 9){
			this.m = arg.slice();
		}else if(typeof arg === 'string'){
			const p = arg.split(' ').map(parseFloat);
			this.m = (p.length === 9) ? p : [1,0,0, 0,1,0, 0,0,1];
		}else{
			this.m = [1, 0, 0,
					0, 1, 0,
					0, 0, 1];
		}
	}
	static identity(){
		return new Mat3();
	}
	static fromTranslation(v){
		const r = new Mat3();
		r.m[6] = v.x;
		r.m[7] = v.y;
		return r;
	}
	static fromScale(v){
		const r = new Mat3();
		if(typeof v === 'number'){
			r.m[0] = v; r.m[4] = v;
		}else{
			r.m[0] = v.x; r.m[4] = v.y;
		}
		return r;
	}
	static fromRotation(angle){
		const a = angle * deg2rad;
		const c = Math.cos(a), s = Math.sin(a);
		const r = new Mat3();
		r.m[0] = c; r.m[1] = s;
		r.m[3] = -s; r.m[4] = c;
		return r;
	}
	static fromBasis(right, up){
		const r = new Mat3();
		r.m[0] = right.x; r.m[1] = right.y; r.m[2] = 0;
		r.m[3] = up.x;    r.m[4] = up.y;    r.m[5] = 0;
		r.m[6] = 0;       r.m[7] = 0;       r.m[8] = 1;
		return r;
	}
	static fromMat4(m){
		const a = m.m;
		const r = new Mat3();
		r.m[0] = a[0]; r.m[1] = a[1]; r.m[2] = a[2];
		r.m[3] = a[4]; r.m[4] = a[5]; r.m[5] = a[6];
		r.m[6] = a[8]; r.m[7] = a[9]; r.m[8] = a[10];
		return r;
	}
	static compose(translation, rotation, scale){
		return Mat3.fromTranslation(translation).rotate(rotation).scale(scale);
	}
	right(){
		return new Vec3(this.m[0], this.m[1], this.m[2]);
	}
	up(){
		return new Vec3(this.m[3], this.m[4], this.m[5]);
	}
	forward(){
		return new Vec3(this.m[6], this.m[7], this.m[8]);
	}
	translation(v){
		if(v instanceof Vec2){
			this.m[6]=v.x;
			this.m[7]=v.y;
			return this;
		}else{
			return new Vec2(this.m[6], this.m[7]);
		}
	}
	angle(){
		return Math.atan2(this.m[0], -this.m[1]) * rad2deg;
	}
	add(f){
		const r = new Mat3();
		for(let i = 0; i < 9; i++){ r.m[i] = this.m[i] + f.m[i]; }
		return r;
	}
	subtract(f){
		const r = new Mat3();
		for(let i = 0; i < 9; i++){ r.m[i] = this.m[i] - f.m[i]; }
		return r;
	}
	multiply(f){
		if(f instanceof Mat3){
			const a = this.m, b = f.m;
			const r = new Mat3();
			for(let col = 0; col < 3; col++){
				for(let row = 0; row < 3; row++){
					let s = 0;
					for(let k = 0; k < 3; k++){
						s += a[k*3 + row] * b[col*3 + k];
					}
					r.m[col*3 + row] = s;
				}
			}
			return r;
		}else if(f instanceof Vec3){
			const m = this.m;
			return new Vec3(
				m[0]*f.x + m[3]*f.y + m[6]*f.z,
				m[1]*f.x + m[4]*f.y + m[7]*f.z,
				m[2]*f.x + m[5]*f.y + m[8]*f.z
			);
		}else if(typeof f === 'number'){
			const r = new Mat3();
			for(let i = 0; i < 9; i++){ r.m[i] = this.m[i] * f; }
			return r;
		}
	}
	translate(v){
		const m = this.m;
		const r = new Mat3();
		r.m[0] = m[0]; r.m[1] = m[1]; r.m[2] = m[2];
		r.m[3] = m[3]; r.m[4] = m[4]; r.m[5] = m[5];
		r.m[6] = m[0]*v.x + m[3]*v.y + m[6];
		r.m[7] = m[1]*v.x + m[4]*v.y + m[7];
		r.m[8] = m[2]*v.x + m[5]*v.y + m[8];
		return r;
	}
	rotate(angle){
		const a = angle * deg2rad;
		const c = Math.cos(a), s = Math.sin(a);
		const m = this.m;
		const r = new Mat3();
		r.m[0] =  c*m[0] + s*m[3];
		r.m[1] =  c*m[1] + s*m[4];
		r.m[2] =  c*m[2] + s*m[5];
		r.m[3] = -s*m[0] + c*m[3];
		r.m[4] = -s*m[1] + c*m[4];
		r.m[5] = -s*m[2] + c*m[5];
		r.m[6] = m[6]; r.m[7] = m[7]; r.m[8] = m[8];
		return r;
	}
	scale(v){
		const m = this.m;
		const sx = (typeof v === 'number') ? v : v.x;
		const sy = (typeof v === 'number') ? v : v.y;
		const r = new Mat3();
		r.m[0] = m[0]*sx; r.m[1] = m[1]*sx; r.m[2] = m[2]*sx;
		r.m[3] = m[3]*sy; r.m[4] = m[4]*sy; r.m[5] = m[5]*sy;
		r.m[6] = m[6];    r.m[7] = m[7];    r.m[8] = m[8];
		return r;
	}
	transformPoint(v){
		const m = this.m;
		return new Vec2(
			m[0]*v.x + m[3]*v.y + m[6],
			m[1]*v.x + m[4]*v.y + m[7]
		);
	}
	transformDirection(v){
		const m = this.m;
		return new Vec2(
			m[0]*v.x + m[3]*v.y,
			m[1]*v.x + m[4]*v.y
		);
	}
	transpose(){
		const m = this.m;
		const r = new Mat3();
		r.m[0] = m[0]; r.m[1] = m[3]; r.m[2] = m[6];
		r.m[3] = m[1]; r.m[4] = m[4]; r.m[5] = m[7];
		r.m[6] = m[2]; r.m[7] = m[5]; r.m[8] = m[8];
		return r;
	}
	determinant(){
		const m = this.m;
		return m[0] * (m[4]*m[8] - m[7]*m[5])
			- m[3] * (m[1]*m[8] - m[7]*m[2])
			+ m[6] * (m[1]*m[5] - m[4]*m[2]);
	}
	inverse(){
		const m = this.m;
		const c00 =  (m[4]*m[8] - m[7]*m[5]);
		const c01 = -(m[1]*m[8] - m[7]*m[2]);
		const c02 =  (m[1]*m[5] - m[4]*m[2]);
		const c10 = -(m[3]*m[8] - m[6]*m[5]);
		const c11 =  (m[0]*m[8] - m[6]*m[2]);
		const c12 = -(m[0]*m[5] - m[3]*m[2]);
		const c20 =  (m[3]*m[7] - m[6]*m[4]);
		const c21 = -(m[0]*m[7] - m[6]*m[1]);
		const c22 =  (m[0]*m[4] - m[3]*m[1]);
		const det = m[0]*c00 + m[3]*c01 + m[6]*c02;
		const inv = 1 / det;
		const r = new Mat3();
		r.m[0] = c00*inv; r.m[1] = c01*inv; r.m[2] = c02*inv;
		r.m[3] = c10*inv; r.m[4] = c11*inv; r.m[5] = c12*inv;
		r.m[6] = c20*inv; r.m[7] = c21*inv; r.m[8] = c22*inv;
		return r;
	}
	decompose(){
		const m = this.m;
		const translation = new Vec2(m[6], m[7]);
		const sx = Math.sqrt(m[0]*m[0] + m[1]*m[1]);
		const sy = Math.sqrt(m[3]*m[3] + m[4]*m[4]);
		const det2 = m[0]*m[4] - m[3]*m[1];
		const sxSigned = (det2 < 0) ? -sx : sx;
		const rotation = (sxSigned !== 0) ? Math.atan2(m[1]/sxSigned, m[0]/sxSigned) * rad2deg : 0;
		return { translation: translation, rotation: rotation, scale: new Vec2(sxSigned, sy) };
	}
	copy(){
		return new Mat3(this);
	}
	clone(){
		return this.copy();
	}
	equals(f){
		if(!(f instanceof Mat3)){ return false; }
		for(let i = 0; i < 9; i++){
			if(Math.abs(this.m[i] - f.m[i]) >= _Epsilon){ return false; }
		}
		return true;
	}
	toString(){
		return this.m[0]+' '+this.m[1]+' '+this.m[2]+' '+
			this.m[3]+' '+this.m[4]+' '+this.m[5]+' '+
			this.m[6]+' '+this.m[7]+' '+this.m[8];
	}
	toConfigString(){
		return this.toString();
	}
}

class Mat4 {
	constructor(arg){
		if(arg instanceof Mat4){
			this.m = arg.m.slice();
		}else if(Array.isArray(arg) && arg.length === 16){
			this.m = arg.slice();
		}else if(typeof arg === 'string'){
			const p = arg.split(' ').map(parseFloat);
			this.m = (p.length === 16) ? p : [
				1, 0, 0, 0,
				0, 1, 0, 0,
				0, 0, 1, 0,
				0, 0, 0, 1
			];
		}else{
			this.m = [1, 0, 0, 0,
					0, 1, 0, 0,
					0, 0, 1, 0,
					0, 0, 0, 1];
		}
	}
	static identity(){
		return new Mat4();
	}
	static fromTranslation(v){
		const r = new Mat4();
		if(v instanceof Vec3){
			r.m[12] = v.x; r.m[13] = v.y; r.m[14] = v.z;
		}else if(v instanceof Vec2){
			r.m[12] = v.x; r.m[13] = v.y;
		}
		return r;
	}
	static fromScale(v){
		const r = new Mat4();
		if(typeof v === 'number'){
			r.m[0] = v; r.m[5] = v; r.m[10] = v;
		}else{
			r.m[0] = v.x; r.m[5] = v.y; r.m[10] = v.z;
		}
		return r;
	}
	static fromRotation(angle, axis){
		const ang = angle * deg2rad;
		const a = axis.normalize();
		const c = Math.cos(ang);
		const s = Math.sin(ang);
		const t = 1 - c;
		const x = a.x, y = a.y, z = a.z;
		const r = new Mat4();
		r.m[0] = c + x*x*t;
		r.m[1] = x*y*t + z*s;
		r.m[2] = x*z*t - y*s;
		r.m[3] = 0;
		r.m[4] = y*x*t - z*s;
		r.m[5] = c + y*y*t;
		r.m[6] = y*z*t + x*s;
		r.m[7] = 0;
		r.m[8] = z*x*t + y*s;
		r.m[9] = z*y*t - x*s;
		r.m[10] = c + z*z*t;
		r.m[11] = 0;
		r.m[12] = 0; r.m[13] = 0; r.m[14] = 0; r.m[15] = 1;
		return r;
	}
	static fromEuler(x, y, z){
		if(x instanceof Vec3){
			z = x.z; y = x.y; x = x.x;
		}
		const rx = x * deg2rad;
		const ry = y * deg2rad;
		const rz = z * deg2rad;
		const c1 = Math.cos(-rz), s1 = Math.sin(-rz);
		const c2 = Math.cos(-ry), s2 = Math.sin(-ry);
		const c3 = Math.cos(-rx), s3 = Math.sin(-rx);
		const r = new Mat4();
		r.m[0] = c1*c2;
		r.m[1] = -c2*s1;
		r.m[2] = s2;
		r.m[3] = 0;
		r.m[4] = c3*s1 + c1*s2*s3;
		r.m[5] = c1*c3 - s1*s2*s3;
		r.m[6] = -c2*s3;
		r.m[7] = 0;
		r.m[8] = s1*s3 - c1*c3*s2;
		r.m[9] = c1*s3 + c3*s1*s2;
		r.m[10] = c2*c3;
		r.m[11] = 0;
		r.m[12] = 0; r.m[13] = 0; r.m[14] = 0; r.m[15] = 1;
		return r;
	}
	static fromBasis(right, up, forward){
		const r = new Mat4();
		r.m[0] = right.x;   r.m[1] = right.y;   r.m[2] = right.z;   r.m[3] = 0;
		r.m[4] = up.x;      r.m[5] = up.y;      r.m[6] = up.z;      r.m[7] = 0;
		r.m[8] = forward.x; r.m[9] = forward.y; r.m[10] = forward.z; r.m[11] = 0;
		r.m[12] = 0; r.m[13] = 0; r.m[14] = 0; r.m[15] = 1;
		return r;
	}
	static lookAt(eye, center, up){
		const f = center.subtract(eye).normalize();
		const s = f.cross(up).normalize();
		const u = s.cross(f);
		const r = new Mat4();
		r.m[0] = s.x;  r.m[1] = u.x;  r.m[2] = -f.x;  r.m[3] = 0;
		r.m[4] = s.y;  r.m[5] = u.y;  r.m[6] = -f.y;  r.m[7] = 0;
		r.m[8] = s.z;  r.m[9] = u.z;  r.m[10] = -f.z; r.m[11] = 0;
		r.m[12] = -s.dot(eye);
		r.m[13] = -u.dot(eye);
		r.m[14] = f.dot(eye);
		r.m[15] = 1;
		return r;
	}
	static compose(translation, rotation, scale){
		return Mat4.fromTranslation(translation).multiply(Mat4.fromEuler(rotation)).scale(scale);
	}
	translation(v){
		if(v instanceof Vec3){
			this.m[12]=v.x;
			this.m[13]=v.y;
			this.m[14]=v.z;
			return this;
		}else if(v instanceof Vec2){
			this.m[12]=v.x;
			this.m[13]=v.y;
			this.m[14]=0;
			return this;
		}else{
			return new Vec3(this.m[12], this.m[13], this.m[14]);
		}
	}
	right(){
		return new Vec3(this.m[0], this.m[1], this.m[2]);
	}
	up(){
		return new Vec3(this.m[4], this.m[5], this.m[6]);
	}
	forward(){
		return new Vec3(this.m[8], this.m[9], this.m[10]);
	}
	add(f){
		const r = new Mat4();
		for(let i = 0; i < 16; i++){ r.m[i] = this.m[i] + f.m[i]; }
		return r;
	}
	subtract(f){
		const r = new Mat4();
		for(let i = 0; i < 16; i++){ r.m[i] = this.m[i] - f.m[i]; }
		return r;
	}
	multiply(f){
		if(f instanceof Mat4){
			const a = this.m, b = f.m;
			const r = new Mat4();
			for(let col = 0; col < 4; col++){
				for(let row = 0; row < 4; row++){
					let s = 0;
					for(let k = 0; k < 4; k++){
						s += a[k*4 + row] * b[col*4 + k];
					}
					r.m[col*4 + row] = s;
				}
			}
			return r;
		}else if(f instanceof Vec4){
			const m = this.m;
			return new Vec4(
				m[0]*f.x + m[4]*f.y + m[8]*f.z + m[12]*f.w,
				m[1]*f.x + m[5]*f.y + m[9]*f.z + m[13]*f.w,
				m[2]*f.x + m[6]*f.y + m[10]*f.z + m[14]*f.w,
				m[3]*f.x + m[7]*f.y + m[11]*f.z + m[15]*f.w
			);
		}else if(typeof f === 'number'){
			const r = new Mat4();
			for(let i = 0; i < 16; i++){ r.m[i] = this.m[i] * f; }
			return r;
		}
	}
	translate(v){
		const m = this.m;
		const x = (v instanceof Vec2) ? v.x : v.x;
		const y = (v instanceof Vec2) ? v.y : v.y;
		const z = (v instanceof Vec2) ? 0   : v.z;
		const r = new Mat4(this);
		r.m[12] = m[0]*x + m[4]*y + m[8]*z  + m[12];
		r.m[13] = m[1]*x + m[5]*y + m[9]*z  + m[13];
		r.m[14] = m[2]*x + m[6]*y + m[10]*z + m[14];
		r.m[15] = m[3]*x + m[7]*y + m[11]*z + m[15];
		return r;
	}
	rotate(angle, axis){
		return this.multiply(Mat4.fromRotation(angle, axis));
	}
	scale(v){
		const m = this.m;
		const sx = (typeof v === 'number') ? v : v.x;
		const sy = (typeof v === 'number') ? v : v.y;
		const sz = (typeof v === 'number') ? v : v.z;
		const r = new Mat4();
		r.m[0]  = m[0]*sx;  r.m[1]  = m[1]*sx;  r.m[2]  = m[2]*sx;  r.m[3]  = m[3]*sx;
		r.m[4]  = m[4]*sy;  r.m[5]  = m[5]*sy;  r.m[6]  = m[6]*sy;  r.m[7]  = m[7]*sy;
		r.m[8]  = m[8]*sz;  r.m[9]  = m[9]*sz;  r.m[10] = m[10]*sz; r.m[11] = m[11]*sz;
		r.m[12] = m[12];    r.m[13] = m[13];    r.m[14] = m[14];    r.m[15] = m[15];
		return r;
	}
	transformPoint(v){
		const m = this.m;
		return new Vec3(
			m[0]*v.x + m[4]*v.y + m[8]*v.z  + m[12],
			m[1]*v.x + m[5]*v.y + m[9]*v.z  + m[13],
			m[2]*v.x + m[6]*v.y + m[10]*v.z + m[14]
		);
	}
	transformDirection(v){
		const m = this.m;
		return new Vec3(
			m[0]*v.x + m[4]*v.y + m[8]*v.z,
			m[1]*v.x + m[5]*v.y + m[9]*v.z,
			m[2]*v.x + m[6]*v.y + m[10]*v.z
		);
	}
	transpose(){
		const m = this.m;
		const r = new Mat4();
		for(let i = 0; i < 4; i++){
			for(let j = 0; j < 4; j++){
				r.m[i*4 + j] = m[j*4 + i];
			}
		}
		return r;
	}
	inverse(){
		const m = this.m;
		const c00 = m[10]*m[15] - m[14]*m[11];
		const c02 = m[6]*m[15]  - m[14]*m[7];
		const c03 = m[6]*m[11]  - m[10]*m[7];
 
		const c04 = m[9]*m[15]  - m[13]*m[11];
		const c06 = m[5]*m[15]  - m[13]*m[7];
		const c07 = m[5]*m[11]  - m[9]*m[7];
 
		const c08 = m[9]*m[14]  - m[13]*m[10];
		const c10 = m[5]*m[14]  - m[13]*m[6];
		const c11 = m[5]*m[10]  - m[9]*m[6];
 
		const c12 = m[8]*m[15]  - m[12]*m[11];
		const c14 = m[4]*m[15]  - m[12]*m[7];
		const c15 = m[4]*m[11]  - m[8]*m[7];
 
		const c16 = m[8]*m[14]  - m[12]*m[10];
		const c18 = m[4]*m[14]  - m[12]*m[6];
		const c19 = m[4]*m[10]  - m[8]*m[6];
 
		const c20 = m[8]*m[13]  - m[12]*m[9];
		const c22 = m[4]*m[13]  - m[12]*m[5];
		const c23 = m[4]*m[9]   - m[8]*m[5];
 
		const r = new Mat4();
		r.m[0]  =  (m[5]*c00 - m[6]*c04 + m[7]*c08);
		r.m[1]  = -(m[1]*c00 - m[2]*c04 + m[3]*c08);
		r.m[2]  =  (m[1]*c02 - m[2]*c06 + m[3]*c10);
		r.m[3]  = -(m[1]*c03 - m[2]*c07 + m[3]*c11);
 
		r.m[4]  = -(m[4]*c00 - m[6]*c12 + m[7]*c16);
		r.m[5]  =  (m[0]*c00 - m[2]*c12 + m[3]*c16);
		r.m[6]  = -(m[0]*c02 - m[2]*c14 + m[3]*c18);
		r.m[7]  =  (m[0]*c03 - m[2]*c15 + m[3]*c19);
 
		r.m[8]  =  (m[4]*c04 - m[5]*c12 + m[7]*c20);
		r.m[9]  = -(m[0]*c04 - m[1]*c12 + m[3]*c20);
		r.m[10] =  (m[0]*c06 - m[1]*c14 + m[3]*c22);
		r.m[11] = -(m[0]*c07 - m[1]*c15 + m[3]*c23);
 
		r.m[12] = -(m[4]*c08 - m[5]*c16 + m[6]*c20);
		r.m[13] =  (m[0]*c08 - m[1]*c16 + m[2]*c20);
		r.m[14] = -(m[0]*c10 - m[1]*c18 + m[2]*c22);
		r.m[15] =  (m[0]*c11 - m[1]*c19 + m[2]*c23);
 
		const det = m[0]*r.m[0] + m[1]*r.m[4] + m[2]*r.m[8] + m[3]*r.m[12];
		const inv = 1 / det;
		for(let i = 0; i < 16; i++){ r.m[i] *= inv; }
		return r;
	}
	determinant(){
		const m = this.m;
		const c00 = m[10]*m[15] - m[14]*m[11];
		const c04 = m[9]*m[15]  - m[13]*m[11];
		const c08 = m[9]*m[14]  - m[13]*m[10];
		const c12 = m[8]*m[15]  - m[12]*m[11];
		const c16 = m[8]*m[14]  - m[12]*m[10];
		const c20 = m[8]*m[13]  - m[12]*m[9];
		const i0 =  (m[5]*c00 - m[6]*c04 + m[7]*c08);
		const i1 = -(m[4]*c00 - m[6]*c12 + m[7]*c16);
		const i2 =  (m[4]*c04 - m[5]*c12 + m[7]*c20);
		const i3 = -(m[4]*c08 - m[5]*c16 + m[6]*c20);
		return m[0]*i0 + m[1]*i1 + m[2]*i2 + m[3]*i3;
	}
	extractEuler(){
		const m = this.m;
		const z = Math.atan2(m[1], m[0]);
		const c2 = Math.sqrt(m[6]*m[6] + m[10]*m[10]);
		const y = Math.atan2(-m[2], c2);
		const s1 = Math.sin(z), c1 = Math.cos(z);
		const x = Math.atan2(s1*m[8] - c1*m[9], c1*m[5] - s1*m[4]);
		return new Vec3(x * rad2deg, y * rad2deg, z * rad2deg);
	}
	normalMatrix(){
		return Mat3.fromMat4(this).inverse().transpose();
	}
	decompose(){
		const m = this.m;
		const translation = new Vec3(m[12], m[13], m[14]);
		let sx = Math.sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
		const sy = Math.sqrt(m[4]*m[4] + m[5]*m[5] + m[6]*m[6]);
		const sz = Math.sqrt(m[8]*m[8] + m[9]*m[9] + m[10]*m[10]);
		const det3 =
			m[0]*(m[5]*m[10] - m[9]*m[6]) -
			m[4]*(m[1]*m[10] - m[9]*m[2]) +
			m[8]*(m[1]*m[6]  - m[5]*m[2]);
		if(det3 < 0){ sx = -sx; }
		const scale = new Vec3(sx, sy, sz);
		const rot = new Mat4(this);
		if(sx !== 0){ rot.m[0] /= sx; rot.m[1] /= sx; rot.m[2] /= sx; }
		if(sy !== 0){ rot.m[4] /= sy; rot.m[5] /= sy; rot.m[6] /= sy; }
		if(sz !== 0){ rot.m[8] /= sz; rot.m[9] /= sz; rot.m[10] /= sz; }
		rot.m[12] = 0; rot.m[13] = 0; rot.m[14] = 0;
		const rotation = rot.extractEuler();
		return { translation: translation, rotation: rotation, scale: scale };
	}
	copy(){
		return new Mat4(this);
	}
	clone(){
		return this.copy();
	}
	equals(f){
		if(!(f instanceof Mat4)){ return false; }
		for(let i = 0; i < 16; i++){
			if(Math.abs(this.m[i] - f.m[i]) >= _Epsilon){ return false; }
		}
		return true;
	}
	toString(){
		return this.m[0]+' '+this.m[1]+' '+this.m[2]+' '+this.m[3]+' '+
				this.m[4]+' '+this.m[5]+' '+this.m[6]+' '+this.m[7]+' '+
				this.m[8]+' '+this.m[9]+' '+this.m[10]+' '+this.m[11]+' '+
				this.m[12]+' '+this.m[13]+' '+this.m[14]+' '+this.m[15];
	}
	toConfigString(){
		return this.toString();
	}
}

class MediaPlaybackEvent {
	static PLAYBACK_STOPPED = 0;
	static PLAYBACK_PLAYING = 1;
	static PLAYBACK_PAUSED = 2;
}
MediaPlaybackEvent.PLAYBACK_STOPPED = 0;
MediaPlaybackEvent.PLAYBACK_PLAYING = 1;
MediaPlaybackEvent.PLAYBACK_PAUSED = 2;

function stringifyAdapter(key, value){
	if (value && typeof value.toConfigString === 'function'){
		return value.toConfigString();
	}
	return value;
}

class IModelData {
	static POSITION = 'position';
	static NORMAL = 'normal';
	static UV = 'uv';
	static TANGENT_SIGNED = 'tangentSigned';
	static COLOR = 'color';
	toConfigString() { return this.__modelDataToken; }
}
IModelData.POSITION = 'position';
IModelData.NORMAL = 'normal';
IModelData.UV = 'uv';
IModelData.TANGENT_SIGNED = 'tangentSigned';
IModelData.COLOR = 'color';

const _Internal = {
	updateScriptProperties(script, vars) {
		vars = JSON.parse(vars);
		Object.keys(vars).forEach((key) => {
			if (script.scriptProperties && script.scriptProperties.hasOwnProperty(key)) {
				if (script.scriptProperties[key] instanceof Vec3){
					script.scriptProperties[key] = new Vec3(vars[key]);
				} else {
					script.scriptProperties[key] = vars[key];
				}
			}
		});
	},
	convertUserProperties(p) {
		p = JSON.parse(p);
		let r = {};
		for (var k in p) {
			switch (p[k].type){
			default:
				r[k] = p[k].value;
				break;
			case 'usershortcut':
				r[k] = {isbound: p[k].isbound, commandtype: p[k].commandtype, file:p[k].file};
				break;
			case 'color':
				r[k] = new Vec3(p[k].value);
				break;
			}
		}
		return r;
	},
	stringifyConfig(obj) {
		return JSON.stringify(obj, stringifyAdapter);
	}
};

globalThis._Vec2 = Vec2.prototype;
globalThis._Vec3 = Vec3.prototype;
globalThis._Vec4 = Vec4.prototype;
globalThis._Mat4 = Mat4.prototype;
globalThis._Mat3 = Mat3.prototype;
globalThis.Vec2 = Vec2;
globalThis.Vec3 = Vec3;
globalThis.Vec4 = Vec4;
globalThis.Mat3 = Mat3;
globalThis.Mat4 = Mat4;
globalThis.IModelData = IModelData;
globalThis._Internal = _Internal;
globalThis.MediaPlaybackEvent = MediaPlaybackEvent;

globalThis.__wwSerializeLayerConfig = function(config) {
  return JSON.stringify(config, function(_key, value) {
    if (value && typeof value.toConfigString === 'function')
      return value.toConfigString();
    if (value instanceof Vec2 || value instanceof Vec3 || value instanceof Vec4 || value instanceof Mat3 || value instanceof Mat4)
      return value.toString();
    return value;
  });
};

// --- thisLayer / thisScene stub ---------------------------------------------
function __wwGetStubLayer(name) {
    if (typeof name === 'number') return null;
    return __wwCreateNodeStub();
}

// Stand-in for the per-script SceneNode binding. Property reads return
// sensible defaults; writes are silently accepted. getTransformMatrix returns
// a shaped value so matrix accesses don't TypeError.
function __wwCreateNodeStub() {
    const props = {
        origin:         new Vec3(0, 0, 0),
        scale:          new Vec3(1, 1, 1),
        angles:         new Vec3(0, 0, 0),
        size:           new Vec3(100, 100, 0),
        perspective:    false,
        visible:        true,
        verticalalign:  'center',
        horizontalalign:'center',
        alpha:          1,
        brightness:     1,
        color:          new Vec3(1, 1, 1),
        text:           '',
        pointsize:      1,
        font:           '',
    };
    // Identity 4x4 column-major matrix. m[13] is the y-translation slot
    // some clock scripts read.
    const identity = [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1];
    const handler = {
        get(target, key) {
            if (key === 'getParent')           return () => undefined;
            if (key === 'getTransformMatrix')  return () => ({ m: identity.slice() });
            if (key === 'getChildren')         return () => [];
            if (key === 'getName')             return () => '';
            if (key === 'getLayer')            return (_n) => __wwGetStubLayer(_n);
            if (key === 'getLayerCount')       return () => 0;
            if (key === 'enumerateLayers')     return () => [];
            if (key === 'getEffect')           return (_n) => __wwCreateEffectStub();
            if (key === 'getTextureAnimation') return () => __wwCreateTexAnimStub();
            if (key === 'getVideoTexture')     return () => __wwCreateVideoTextureStub();
            if (key === 'getAnimation')        return ()   => __wwCreateAnimationStub();
            if (key === 'getAnimationLayer')   return (_n) => __wwCreateAnimationStub();
            if (key === 'getBoneTransform')    return (_b) => new Mat4();
            if (key === 'setBoneTransform')    return (_b, _m) => undefined;
            if (key === 'destroyLayer')        return (_layer) => undefined;
            if (key in target) return target[key];
            return undefined;
        },
        set(target, key, value) { target[key] = value; return true; },
        has(target, key) { return key in target; },
    };
    return new Proxy(props, handler);
}

function __wwCreateEffectStub() {
    return { visible: true };
}

function __wwCreateTexAnimStub() {
    let frame = 0, playing = false;
    return {
        play()     { playing = true;  },
        stop()     { playing = false; },
        pause()    { playing = false; },
        setFrame(n){ frame = n | 0;   },
        getFrame() { return frame;    },
        isPlaying(){ return playing;  },
    };
}

function __wwCreateVideoTextureStub() {
    let current = 0, playing = false;
    return {
        duration: 0,
        rate: 1,
        volume: 1,
        play()     { playing = true;  },
        stop()     { playing = false; current = 0; },
        pause()    { playing = false; },
        setCurrentTime(t) {
            t = Number(t);
            current = Number.isFinite(t) && t > 0 ? t : 0;
        },
        getCurrentTime() { return current; },
        isPlaying()     { return playing; },
    };
}

// Sprite-image / puppet-bone animation handle. Scripts commonly adjust
// playback rate and manually drive the current frame from init/update.
function __wwCreateAnimationStub() {
    let frame = 0, playing = false;
    const o = {
        rate: 1,
        frameCount: 1,
        play()     { playing = true;  },
        stop()     { playing = false; },
        pause()    { playing = false; },
        setFrame(n){ frame = Math.max(0, n | 0); },
        getFrame() { return frame;    },
        isPlaying(){ return playing;  },
    };
    return o;
}
globalThis.__wwCreateAnimationStub = __wwCreateAnimationStub;
globalThis.__wwCreateVideoTextureStub = __wwCreateVideoTextureStub;
globalThis.thisLayer = __wwCreateNodeStub();
globalThis.thisObject = globalThis.thisLayer;
globalThis.thisScene = __wwCreateNodeStub();

// `input` is the WE-global cursor / input state. Scripts often guard with
// `if (input && input.cursorWorldPosition)` so a populated stub is fine;
// values stay at zero until the host wires real cursor data.
globalThis.input = {
    cursorWorldPosition:  new Vec3(0, 0, 0),
    cursorLocalPosition:  new Vec3(0, 0, 0),
    cursorScreenPosition: new Vec2(0, 0),
    mouseButtonsDown:     0,
    mouseButtonsPressed:  0,
    mouseButtonsReleased: 0,
    cursorLeftDown:       false,
    cursorRightDown:      false,
    cursorMiddleDown:     false,
    inWindow:             false,
};

// Hook used by the C++ side to swap the stub for a real per-script binding.
globalThis.__wwBindLayer = function(obj) { globalThis.thisLayer = obj; globalThis.thisObject = obj; };
globalThis.__wwBindScene = function(obj) { globalThis.thisScene = obj; };

// --- shared --- cross-script object scripts mutate freely.
if (! globalThis.shared) globalThis.shared = {};

// localStorage is installed from C++ in InstallEngineGlobal so it can
// optionally persist to a JSON file under cache_path keyed by scene_id.
