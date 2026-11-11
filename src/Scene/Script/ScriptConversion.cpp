module;

#include <rstd/enum.hpp>
#include "quickjs.h"

module wescene.script;
import :conversion;
import rstd;
import rstd.cppstd;
import rstd.parse;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::parse::regex::compile;

namespace owe::script::conversion
{

static bool IsFinite(double value) { return f64(value).is_finite(); }

static JSValue MakeVecValue(JSContext* ctx, double x, double y, double z, int n) {
    JSValue global  = JS_GetGlobalObject(ctx);
    JSValue ctor    = JS_GetPropertyStr(ctx, global, n == 2 ? "Vec2" : "Vec3");
    JSValue argv[3] = { JS_NewFloat64(ctx, x), JS_NewFloat64(ctx, y), JS_NewFloat64(ctx, z) };
    JSValue obj     = JS_CallConstructor(ctx, ctor, n, argv);
    for (int i = 0; i < n; ++i) JS_FreeValue(ctx, argv[i]);
    if (n < 3) JS_FreeValue(ctx, argv[2]);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    return obj;
}

static JSValue MakeVec4Value(JSContext* ctx, double x, double y, double z, double w) {
    JSValue global  = JS_GetGlobalObject(ctx);
    JSValue ctor    = JS_GetPropertyStr(ctx, global, "Vec4");
    JSValue argv[4] = {
        JS_NewFloat64(ctx, x), JS_NewFloat64(ctx, y), JS_NewFloat64(ctx, z), JS_NewFloat64(ctx, w)
    };
    JSValue obj = JS_CallConstructor(ctx, ctor, 4, argv);
    for (auto& arg : argv) JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    return obj;
}

// JSValue→ScriptValue coercion. Mirrors the table in the API doc; never
// throws, returns monostate for unrecognised shapes.
ScriptValue CoerceReturn(JSContext* ctx, JSValue ret, FieldKind kind) {
    if (JS_IsUndefined(ret) || JS_IsNull(ret)) return {};

    auto read_field = [&](JSValue obj, const char* name, double& out) -> bool {
        JSValue v = JS_GetPropertyStr(ctx, obj, name);
        if (JS_IsUndefined(v)) {
            JS_FreeValue(ctx, v);
            return false;
        }
        double d  = 0.0;
        int    rc = JS_ToFloat64(ctx, &d, v);
        JS_FreeValue(ctx, v);
        if (rc < 0 || ! IsFinite(d)) return false;
        out = d;
        return true;
    };
    auto read_index = [&](JSValue arr, rstd::uint32_t i, double& out) -> bool {
        JSValue v = JS_GetPropertyUint32(ctx, arr, i);
        if (JS_IsUndefined(v)) {
            JS_FreeValue(ctx, v);
            return false;
        }
        double d  = 0.0;
        int    rc = JS_ToFloat64(ctx, &d, v);
        JS_FreeValue(ctx, v);
        if (rc < 0 || ! IsFinite(d)) return false;
        out = d;
        return true;
    };

    switch (kind) {
    case FieldKind::Bool: {
        if (! JS_IsBool(ret)) return {};
        int b = JS_ToBool(ctx, ret);
        return BoolValue { b > 0 };
    }
    case FieldKind::Scalar: {
        if (JS_IsBool(ret)) {
            int b = JS_ToBool(ctx, ret);
            return ScalarValue { b > 0 ? 1.0 : 0.0 };
        }
        double d = 0.0;
        if (JS_ToFloat64(ctx, &d, ret) >= 0 && IsFinite(d)) return ScalarValue { d };
        return {};
    }
    case FieldKind::Vec2: {
        Vec2Value v;
        if (JS_IsArray(ret)) {
            read_index(ret, 0, v.x);
            read_index(ret, 1, v.y);
        } else if (JS_IsObject(ret)) {
            read_field(ret, "x", v.x);
            read_field(ret, "y", v.y);
        } else if (JS_IsNumber(ret)) {
            double scalar = 0.0;
            if (JS_ToFloat64(ctx, &scalar, ret) < 0 || ! IsFinite(scalar)) return {};
            v.x = scalar;
            v.y = scalar;
        } else {
            return {};
        }
        return v;
    }
    case FieldKind::Vec3: {
        Vec3Value v;
        if (JS_IsArray(ret)) {
            read_index(ret, 0, v.x);
            read_index(ret, 1, v.y);
            read_index(ret, 2, v.z);
        } else if (JS_IsObject(ret)) {
            read_field(ret, "x", v.x);
            read_field(ret, "y", v.y);
            read_field(ret, "z", v.z);
        } else if (JS_IsNumber(ret)) {
            // Many audio-response scripts return a *scalar* even when bound
            // to scale (vec3). Splat into all three components.
            double d = 0.0;
            JS_ToFloat64(ctx, &d, ret);
            if (! IsFinite(d)) return {};
            return Vec3Value { d, d, d };
        } else {
            return {};
        }
        return v;
    }
    case FieldKind::Vec4: {
        Vec4Value v;
        if (JS_IsArray(ret)) {
            read_index(ret, 0, v.x);
            read_index(ret, 1, v.y);
            read_index(ret, 2, v.z);
            read_index(ret, 3, v.w);
        } else if (JS_IsObject(ret)) {
            read_field(ret, "x", v.x);
            read_field(ret, "y", v.y);
            read_field(ret, "z", v.z);
            read_field(ret, "w", v.w);
        } else if (JS_IsNumber(ret)) {
            double d = 0.0;
            JS_ToFloat64(ctx, &d, ret);
            if (! IsFinite(d)) return {};
            return Vec4Value { d, d, d, d };
        } else {
            return {};
        }
        return v;
    }
    case FieldKind::Color: {
        ColorValue v;
        if (JS_IsArray(ret)) {
            read_index(ret, 0, v.r);
            read_index(ret, 1, v.g);
            read_index(ret, 2, v.b);
        } else if (JS_IsObject(ret)) {
            read_field(ret, "r", v.r);
            read_field(ret, "g", v.g);
            read_field(ret, "b", v.b);
        } else {
            return {};
        }
        return v;
    }
    case FieldKind::String: {
        const char* s = JS_ToCString(ctx, ret);
        if (! s) return {};
        StringValue sv { rstd::into(rstd::cppstd::as_str(s).unwrap()) };
        JS_FreeCString(ctx, s);
        return rstd::move(sv);
    }
    case FieldKind::Unknown: return {};
    }
    return {};
}

JSValue ScriptValueToJs(JSContext* ctx, const ScriptValue& value) {
    if (auto* p = (value.is_Scalar() ? &value.as_Scalar().value : nullptr))
        return JS_NewFloat64(ctx, p->v);
    if (auto* p = (value.is_Bool() ? &value.as_Bool().value : nullptr))
        return JS_NewBool(ctx, p->v);
    if (auto* p = (value.is_Vec2() ? &value.as_Vec2().value : nullptr))
        return MakeVecValue(ctx, p->x, p->y, 0.0, 2);
    if (auto* p = (value.is_Vec3() ? &value.as_Vec3().value : nullptr))
        return MakeVecValue(ctx, p->x, p->y, p->z, 3);
    if (auto* p = (value.is_Vec4() ? &value.as_Vec4().value : nullptr))
        return MakeVec4Value(ctx, p->x, p->y, p->z, p->w);
    if (auto* p = (value.is_Color() ? &value.as_Color().value : nullptr)) {
        JSValue arr = JS_NewArray(ctx);
        JS_DefinePropertyValueUint32(ctx, arr, 0, JS_NewFloat64(ctx, p->r), JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, arr, 1, JS_NewFloat64(ctx, p->g), JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, arr, 2, JS_NewFloat64(ctx, p->b), JS_PROP_C_W_E);
        return arr;
    }
    if (auto* p = (value.is_String() ? &value.as_String().value : nullptr))
        return JS_NewStringLen(
            ctx, rstd::cppstd::as_string_view(p->s.as_str()).data(), p->s.len().to_primitive());
    return JS_UNDEFINED;
}

// JSON → JSValue conversion for the initial-value seed. Recursive but
// scenescript values are tiny (numbers, short strings, small objects).
JSValue JsonToJs(JSContext* ctx, const Json& value) {
    RSTD_MATCH(value) {
        RSTD_CASE(Null) { return JS_NULL; }
        RSTD_CASE(Bool, boolean) { return JS_NewBool(ctx, boolean); }
        RSTD_CASE(Number, number) {
            if (auto integer = number.as_i64(); integer.is_some())
                return JS_NewInt64(ctx, integer->to_primitive());
            if (auto integer = number.as_u64(); integer.is_some())
                return JS_NewInt64(ctx, static_cast<rstd::int64_t>(integer->to_primitive()));
            return JS_NewFloat64(ctx, number.as_f64()->to_primitive());
        }
        RSTD_CASE(String, string) {
            const auto view = rstd::cppstd::as_string_view(string.as_str());
            return JS_NewStringLen(ctx, view.data(), view.size());
        }
        RSTD_CASE(Array, values) {
            JSValue        array = JS_NewArray(ctx);
            rstd::uint32_t index = 0;
            for (const auto& item : values) {
                JS_DefinePropertyValueUint32(
                    ctx, array, index++, JsonToJs(ctx, item), JS_PROP_C_W_E);
            }
            return array;
        }
        RSTD_CASE(Object, values) {
            JSValue object = JS_NewObject(ctx);
            values.iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                auto owned_key                = Vec<u8>::from(entry_key->as_str().as_bytes());
                owned_key.push(u8());
                JS_DefinePropertyValueStr(ctx,
                                          object,
                                          reinterpret_cast<const char*>(
                                              rstd::as_bytes(owned_key.as_slice()).as_raw_ptr()),
                                          JsonToJs(ctx, *entry_value),
                                          JS_PROP_C_W_E);
            });
            return object;
        }
    }
    rstd::unreachable();
}

JSValue UserPropertyValueToJs(JSContext* ctx, const Json& property) {
    if (auto value = property.get("value"_str); value.is_some()) return JsonToJs(ctx, **value);
    return JsonToJs(ctx, property);
}

// Resolve a config value. {"user":"name","value":X} stays as-is — the
// bootstrap getter resolves it lazily against engine.userProperties at
// access time, so SetUserProperty calls after parse propagate.
// Everything else passes through.
JSValue ResolveConfigValue(JSContext* ctx, const Json& v) { return JsonToJs(ctx, v); }

// Coerce a binding's initial-value JSON into the JS shape the script's
// `init(value)` expects, given the bound field kind. Audio-response,
// parallax, and color scripts all assume `value` is already a Vec2/Vec3,
// not a raw string or array.
//   - Numbers: passthrough for scalar; for Vec2/Vec3 we splat (matching WE's
//     uniform vector behaviour observed in the corpus).
//   - Strings: WE serialises vec values as space-separated floats —
//     "1.0 2.0 3.0" → Vec3(1,2,3). Arrays accept the same shape.
//   - Arrays / objects with x,y[,z]: construct a Vec2 / Vec3.
//   - Color: returns an array — most "color" scripts use `[r,g,b]` access.
//
// Falls back to JsonToJs for unknown shapes; better to pass garbage than
// to fail to call init().
JSValue CoerceInitialValue(JSContext* ctx, const Json& v, FieldKind kind) {
    auto parse_floats = [](ref<str> s) -> Vec<double> {
        Vec<double>    out;
        constexpr auto number = compile<
            R"([+-]?([0-9]+(\.[0-9]*)?|\.[0-9]+)([eE][+-]?[0-9]+)?|[+-]?(infinity|inf|NaN))">;
        while (! s.is_empty()) {
            usize begin;
            while (begin < s.len() && rstd::ascii::is_space(s[begin])) ++begin;
            s          = s.get(begin, s.len()).unwrap();
            auto token = number.prefix(s);
            if (token.is_none()) break;
            auto value = rstd::from_str<f64>(token->text());
            if (value.is_err()) break;
            out.push(value.unwrap().to_primitive());
            s = s.get(token->end(), s.len()).unwrap();
        }
        return out;
    };
    switch (kind) {
    case FieldKind::Vec2: {
        if (v.is_string()) {
            auto source = *v.as_str();
            auto fs     = parse_floats(source);
            return MakeVecValue(ctx,
                                fs.len().to_primitive() > 0 ? fs[usize(0)] : 0.0,
                                fs.len().to_primitive() > 1 ? fs[usize(1)] : 0.0,
                                0.0,
                                2);
        }
        if (auto values = v.as_array(); values.is_some() && (*values)->len() >= usize(2)) {
            auto x = (**values)[usize(0)].as_f64();
            auto y = (**values)[usize(1)].as_f64();
            if (x.is_some() && y.is_some())
                return MakeVecValue(ctx, x->to_primitive(), y->to_primitive(), 0.0, 2);
        }
        if (v.is_number()) {
            auto number = v.as_f64();
            if (number.is_some()) {
                const auto scalar = number->to_primitive();
                return MakeVecValue(ctx, scalar, scalar, 0.0, 2);
            }
        }
        break;
    }
    case FieldKind::Vec3: {
        if (v.is_string()) {
            auto   source = *v.as_str();
            auto   fs     = parse_floats(source);
            double x      = fs.len().to_primitive() > 0 ? fs[usize(0)] : 0.0;
            double y      = fs.len().to_primitive() > 1 ? fs[usize(1)] : x; // splat single scalar
            double z      = fs.len().to_primitive() > 2 ? fs[usize(2)]
                                                        : (fs.len().to_primitive() > 1 ? 0.0 : x);
            return MakeVecValue(ctx, x, y, z, 3);
        }
        if (auto values = v.as_array(); values.is_some() && (*values)->len() >= usize(3)) {
            auto x = (**values)[usize(0)].as_f64();
            auto y = (**values)[usize(1)].as_f64();
            auto z = (**values)[usize(2)].as_f64();
            if (x.is_some() && y.is_some() && z.is_some())
                return MakeVecValue(
                    ctx, x->to_primitive(), y->to_primitive(), z->to_primitive(), 3);
        }
        if (v.is_number()) {
            auto number = v.as_f64();
            if (number.is_some()) {
                const auto scalar = number->to_primitive();
                return MakeVecValue(ctx, scalar, scalar, scalar, 3);
            }
        }
        break;
    }
    case FieldKind::Vec4: {
        if (v.is_string()) {
            auto   source = *v.as_str();
            auto   fs     = parse_floats(source);
            double x      = fs.len().to_primitive() > 0 ? fs[usize(0)] : 0.0;
            double y      = fs.len().to_primitive() > 1 ? fs[usize(1)] : x;
            double z      = fs.len().to_primitive() > 2 ? fs[usize(2)] : x;
            double w      = fs.len().to_primitive() > 3 ? fs[usize(3)] : x;
            return MakeVec4Value(ctx, x, y, z, w);
        }
        if (auto values = v.as_array(); values.is_some() && (*values)->len() >= usize(4)) {
            auto x = (**values)[usize(0)].as_f64();
            auto y = (**values)[usize(1)].as_f64();
            auto z = (**values)[usize(2)].as_f64();
            auto w = (**values)[usize(3)].as_f64();
            if (x.is_some() && y.is_some() && z.is_some() && w.is_some())
                return MakeVec4Value(ctx,
                                     x->to_primitive(),
                                     y->to_primitive(),
                                     z->to_primitive(),
                                     w->to_primitive());
        }
        if (v.is_number()) {
            auto number = v.as_f64();
            if (number.is_some()) {
                const auto scalar = number->to_primitive();
                return MakeVec4Value(ctx, scalar, scalar, scalar, scalar);
            }
        }
        break;
    }
    case FieldKind::Color: {
        if (v.is_string()) {
            auto    source = *v.as_str();
            auto    fs     = parse_floats(source);
            JSValue arr    = JS_NewArray(ctx);
            for (rstd::uint32_t i = 0; i < fs.len().to_primitive() && i < 3; ++i)
                JS_DefinePropertyValueUint32(
                    ctx, arr, i, JS_NewFloat64(ctx, fs[usize(i)]), JS_PROP_C_W_E);
            return arr;
        }
        break;
    }
    case FieldKind::Scalar:
    case FieldKind::Bool:
    case FieldKind::String:
    case FieldKind::Unknown: break;
    }
    return JsonToJs(ctx, v);
}

} // namespace owe::script::conversion
