module;

#include <rstd/macro.hpp>

module wescene.pkg.parse;
import wescene.pkg.spec_names;
import wescene.core;
import wescene.types;
import rstd;
import rstd.log;
import rstd.cppstd;
import wescene.shader_compile;
import wescene.scene;
import wescene.utils;
import :shader_lex;

#define SHADER_DIR    "spvs03"
#define SHADER_SUFFIX "spvs"

using namespace owe;
using namespace rstd::prelude;
using rstd::tuple;
using rstd::collections::BTreeMap;
using rstd::collections::BTreeSet;
using rstd::collections::HashSet;
using rstd::fs::OpenOptions;
using rstd::hash::DefaultHasher;
using rstd::hash::hash_into;
using rstd::path::Path;
using rstd::slice_::sort_unstable_by;
using rstd::sync::Arc;
using namespace rstd::literals;
using rstd::sync::atomic::Atomic;
using rstd::sync::atomic::Ordering;

static constexpr ref<str> SHADER_PLACEHOLD { "__SHADER_PLACEHOLD__"_str };

namespace
{
// Decl scanners over GLSL declaration lines. Each WE shader decl is
// line-scoped; the Cursor primitives in :shader_lex do all char-level work.

struct DeclMatch {
    rstd::size_t start;       // offset of leading newline (or 0 at file start)
    rstd::size_t end;         // one past trailing `;`
    rstd::size_t keep_prefix; // number of bytes from start to preserve when stripping
    ref<str>     storage;     // attribute/varying/in/out/uniform
    ref<str>     type;
    ref<str>     name;
    ref<str>     array; // "[N]" or empty
};

constexpr array<ref<str>, 4> kStorageKeywords {
    "attribute"_str, "varying"_str, "in"_str, "out"_str
};
constexpr array<ref<str>, 1> kUniformKeyword { "uniform"_str };

// Try to match `[ws]<storage_kw> <type> <name>[opt-array][ws];` on the line
// starting at `line_start`. Anchored — leading non-whitespace fails it.
inline Option<DeclMatch> TryParseDeclLine(ref<str> src, usize line_start,
                                          slice<ref<str>> storage_kws) {
    shader_lex::Cursor source(src, line_start);
    auto               line_end = source.LineEnd();
    shader_lex::Cursor c(*src.get(line_start, line_end));
    c.SkipHSpace();

    ref<str> kw;
    for (auto k : storage_kws) {
        auto s = c.Save();
        if (c.MatchKeyword(k)) {
            kw = k;
            break;
        }
        c.Restore(s);
    }
    if (kw.is_empty()) return None();
    c.SkipHSpace();
    auto tn = shader_lex::ReadTypeName(c);
    if (! tn) return None();
    c.SkipHSpace();
    auto array = c.ReadArraySuffix();
    c.SkipHSpace();
    if (! c.MatchChar(';')) return None();

    DeclMatch m;
    m.start       = line_start.to_primitive();
    m.end         = (line_start + c.Pos()).to_primitive();
    m.keep_prefix = 0;
    m.storage     = kw;
    m.type        = tn->type;
    m.name        = tn->name;
    m.array       = array.unwrap_or(ref<str> {});
    return Some(m);
}

// Iterate every line; yield one DeclMatch per matching line. `keep_prefix`
// is 1 when a leading newline exists (so callers stripping decl lines keep
// the newline as a paragraph anchor).
template<typename Fn>
inline void ForEachDeclLine(ref<str> src, slice<ref<str>> storage_kws, Fn&& fn) {
    shader_lex::LineWalker w(src);
    for (; ! w.Done(); w.Step()) {
        if (auto m = TryParseDeclLine(src, w.LineStart(), storage_kws)) {
            DeclMatch out = *m;
            if (w.LineStart() > rstd::usize()) {
                out.start       = (w.LineStart() - rstd::usize(1)).to_primitive();
                out.keep_prefix = 1;
            } else {
                out.start       = w.LineStart().to_primitive();
                out.keep_prefix = 0;
            }
            fn(out);
        }
    }
}

inline bool IsSamplerType(ref<str> t) {
    return t == "sampler2D"_str || t == "sampler3D"_str || t == "samplerCube"_str ||
           t == "sampler2DComparison"_str || t == "sampler2DShadow"_str;
}

// Replace every occurrence of `needle` in `body` with `repl`. The placeholder
// names used by the shader synth pipeline are unique tokens
// (`__SHADER_PLACEHOLD__`), so naive substring substitution is safe.
inline String ReplaceAll(ref<str> body, ref<str> needle, ref<str> repl) {
    if (needle.is_empty()) return String::make(body);
    String out;
    out.reserve(body.len());
    while (auto parts = body.split_once(needle)) {
        auto [head, tail] = *parts;
        out.push_str(head);
        out.push_str(repl);
        body = tail;
    }
    out.push_str(body);
    return out;
}

// HLSL prologue. WE shaders are written in a hybrid dialect that already
// uses HLSL idioms (mul, texSample2D, float2/3/4, saturate, lerp, frac,
// [maxvertexcount], OUT.Append); only the residual GLSL bits (vec*, mat*,
// attribute, varying, gl_*) need bridging. Routing VS/FS through glslang's
// HLSL frontend lets the parser handle implicit conversions HLSL allows
// (scalar→vec broadcast on assignment, bool→float, etc.) — which would
// otherwise fault in glslang's strict GLSL mode.
//
// Pipeline: this prologue + the user source is fed through glslang's own
// preprocessor (TShader::preprocess) which expands every #if / #include /
// #define. Then a regex pass extracts the surviving `attribute`/`varying`/
// `uniform` declarations (live code only — combo-gated dead branches are
// gone) and Finalprocessor strips them, then re-emits canonical
// `static TYPE NAME;` decls + a paired Texture2D/SamplerState block + a
// shared cbuffer ww_Uniforms + an HLSL entry-point wrapper (main_vs /
// main_ps) that shuffles between the static globals and the
// SV_*-annotated entry struct.
static constexpr ref<str> pre_shader_code = R"(// auto-generated WE→HLSL prologue
#define HLSL 1
#define GLSL 0
#define highp
#define mediump
#define lowp

#define vec2 float2
#define vec3 float3
#define vec4 float4
#define ivec2 int2
#define ivec3 int3
#define ivec4 int4
#define uvec2 uint2
#define uvec3 uint3
#define uvec4 uint4
#define bvec2 bool2
#define bvec3 bool3
#define bvec4 bool4
#define mat2 float2x2
#define mat3 float3x3
#define mat4 float4x4
#define mat2x2 float2x2
#define mat3x3 float3x3
#define mat4x4 float4x4
#define mat2x3 float2x3
#define mat2x4 float2x4
#define mat3x2 float3x2
#define mat3x4 float3x4
#define mat4x2 float4x2
#define mat4x3 float4x3

#define CAST2(x) ((float2)(x))
#define CAST3(x) ((float3)(x))
#define CAST4(x) ((float4)(x))
#define CAST3X3(x) ((float3x3)(x))

#define mix(a,b,t) lerp((a),(b),(t))
#define fract frac
#define dFdx ddx
#define dFdy(x) (-ddy(x))

float  atan(float  y, float  x) { return atan2(y, x); }
float2 atan(float2 y, float2 x) { return atan2(y, x); }
float3 atan(float3 y, float3 x) { return atan2(y, x); }
float4 atan(float4 y, float4 x) { return atan2(y, x); }

// GLSL `mod(a, b)` is `a - b * floor(a / b)` and isn't an HLSL builtin
// (HLSL has `fmod`, but it uses trunc for the quotient — different sign
// behavior for negative args). Provide a `mod` function so shaders that
// call it without supplying their own definition still compile. A few WE
// shaders ship their own `float mod(float, float)`; PreShaderHeader scans
// the user source and `#define`s `WW_USER_MOD` before this block when so,
// so our definitions are skipped to avoid redefinition errors.
#ifndef WW_USER_MOD
float  mod(float  a, float  b) { return a - b * floor(a / b); }
float2 mod(float2 a, float2 b) { return a - b * floor(a / b); }
float3 mod(float3 a, float3 b) { return a - b * floor(a / b); }
float4 mod(float4 a, float4 b) { return a - b * floor(a / b); }
float2 mod(float2 a, float  b) { return a - b * floor(a / b); }
float3 mod(float3 a, float  b) { return a - b * floor(a / b); }
float4 mod(float4 a, float  b) { return a - b * floor(a / b); }
#endif
// HLSL has saturate, mul, lerp, frac, ddx/ddy, fwidth, max, min, clip, log10,
// pow as builtins — most of the C++-side overload workarounds needed for the
// GLSL frontend disappear here.

// `uniform`, `attribute`, `varying` are intentionally NOT #define'd here.
// glslang's preprocess pass runs over this prologue; if any of them were
// stripped to empty, the post-preprocess regex in Finalprocessor wouldn't
// find live declarations. We let the keywords survive preprocess, strip
// the matching lines, and re-emit canonical `static TYPE NAME;` decls +
// `cbuffer ww_Uniforms` + Texture2D/SamplerState pairs at the placeholder.

// WE-dialect texture sampling. Each `uniform sampler2D NAME` becomes a
// `Texture2D<float4> NAME;` + paired `SamplerState NAME_ww_sampler;` in
// the Finalprocessor synth block; texSample2D thus expands to
// `NAME.Sample(NAME_ww_sampler, uv)`. The `texture()` overloads accept
// vec2/vec3/vec4 UV (HLSL Sample takes float2 — the auto-truncation
// matches what WE shaders rely on for `texture(g_T, v_TexCoord)` when
// v_TexCoord is vec4).
#define texSample2D(t, uv)         ((t).Sample(t##_ww_sampler, (uv)))
#define texSample2DLod(t, uv, lod) ((t).SampleLevel(t##_ww_sampler, (uv), (lod)))
#define DECLARE_SAMPLER2D_PARAMETER(t) Texture2D<float4> t, SamplerState t##_ww_sampler
#define MAKE_SAMPLER2D_ARGUMENT(t)     t, t##_ww_sampler
// SampleCmpLevelZero handles the depth-compare semantics that `sampler2DComparison`
// implies in GLSL; the paired sampler is a SamplerComparisonState (see
// HLSLSamplerStateType in ShaderParser.cpp). uv.xy is the atlas coord,
// uv.z is the depth to compare against.
#define texSample2DCompare(t, uv, ref) ((t).SampleCmpLevelZero(t##_ww_sampler, (uv), (ref)))
#define texture(t, uv)             texSample2D((t), (uv))
#define textureLod(t, uv, lod)     texSample2DLod((t), (uv), (lod))

__SHADER_TAIL__
__SHADER_PLACEHOLD__

)"_str;

static constexpr ref<str> lighting_v1_source = R"(
uniform vec3 g_LightsPosition[4];
uniform vec4 g_LightsColorRadius[4];
uniform vec4 g_LightsDirectionType[4];
uniform vec4 g_LightsConeExponent[4];
uniform float g_LightsCastShadow[4];
#if LIGHTS_SHADOW_MAPPING
uniform mat4 g_ViewportViewProjectionMatrices[6];
uniform vec4 g_ShadowAtlasTransforms[3];
#endif

float3 PerformLighting_V1(float3 worldPos, float3 albedo, float3 normal, float3 viewVector,
                          float3 specularTint, float3 f0, float roughness, float metallic) {
    float3 light = float3(0.0, 0.0, 0.0);
    for (int i = 0; i < 4; ++i) {
        float type = g_LightsDirectionType[i].w;
        float3 color = g_LightsColorRadius[i].rgb;
        float shadowFactor = 1.0;
#if !LIGHTS_SHADOW_MAPPING && OWE_IMAGE_LAYER && SCENE_ORTHO
        // Shadow-atlas rendering is not available yet. Suppress shadow-casting
        // lights on 2D layers instead of leaking them across the whole quad.
        shadowFactor = 1.0 - step(0.5, g_LightsCastShadow[i]);
#endif
        if (type < -0.5 || dot(color, color) <= 0.0)
            continue;

        if (type > 1.5) {
#if LIGHTS_SHADOW_MAPPING
            if (g_LightsCastShadow[i] > 0.5) {
                float selectedCascade = 0.0;
                for (int cascade = 0; cascade < 3; ++cascade) {
                    float4 projected = CalculateProjectedCoordsCascades(
                        worldPos, g_ViewportViewProjectionMatrices[cascade]);
                    float active = (1.0 - projected.w) * (1.0 - selectedCascade);
                    float sampled = PerformShadowMapping(
                        projected.xyz, g_ShadowAtlasTransforms[cascade]);
                    shadowFactor = lerp(shadowFactor, sampled, active);
                    selectedCascade = saturate(selectedCascade + active);
                }
            }
#endif
            light += ComputePBRLightShadowInfinite(
                normal, g_LightsDirectionType[i].xyz, viewVector, albedo, color,
                specularTint, f0, roughness, metallic, shadowFactor);
            continue;
        }

        float3 toLight = g_LightsPosition[i] - worldPos;
        if (type > 0.5) {
            float3 lightToSurface = -normalize(toLight);
            float cone = smoothstep(g_LightsConeExponent[i].y,
                                    g_LightsConeExponent[i].x,
                                    dot(lightToSurface, g_LightsDirectionType[i].xyz));
            color *= cone;
        }
        light += ComputePBRLightShadow(
            normal, toLight, viewVector, albedo, color,
            max(g_LightsColorRadius[i].w, 0.0001),
            max(g_LightsConeExponent[i].z, 0.0), specularTint, f0,
            roughness, metallic, shadowFactor);
    }
    return light;
}

float3 PerformLighting_V1(float3 worldPos, float3 albedo, float3 normal, float3 viewVector,
                          float3 specularTint, float3 f0, float roughness, float metallic,
                          float ao) {
    return PerformLighting_V1(worldPos, albedo, normal, viewVector, specularTint, f0,
                              roughness, metallic) * ao;
}
)"_str;

// VS/FS tail: stage I/O is plumbed by the Finalprocessor synthesizer. It
// strips every `attribute|varying TYPE NAME;` line and re-emits canonical
// `static TYPE NAME;` decls; combo-gated `#if` branches drop their decls
// at preprocess time, so vert/frag stages get a matching live name set.
// The keywords MUST NOT be #define'd here; if they were, the regex would
// see unsubstituted text but the HLSL parser would see the substituted
// text, drifting the two views apart.
static constexpr ref<str> pre_shader_tail_vert = R"(
static float4 gl_Position;
// Rename the user's main() so a synthesized HLSL entry point can wrap it.
// The wrapper (main_vs) is appended in Finalprocessor.
#define main shader_main
)"_str;

static constexpr ref<str> pre_shader_tail_frag = R"(
static float4 gl_FragCoord;
static float4 glOutColor;
#define gl_FragColor glOutColor
#define main shader_main
)"_str;

static constexpr ref<str> pre_shader_tail_geom = R"()"_str;

// HLSL prologue used when type==GEOMETRY. WE's .geom source is a hybrid:
// GLSL-flavoured top-level `in vec4 X;` / `out vec4 X;` decls + HLSL-style
// `[maxvertexcount] void main() { ... IN[0].X ... v.Y = ...; OUT.Append(v); }`
// body. We feed it to glslang's HLSL frontend (EShSourceHlsl); this prologue
// bridges GLSL types/builtins to HLSL and Finalprocessor strips the `in`/`out`
// lines + emits `struct WW_VSOut/WW_PSIn` + `cbuffer ww_Uniforms` + replaces
// `void main()` with the GS entry signature.
static constexpr ref<str> pre_shader_code_gs_hlsl = R"(// auto-generated WE→HLSL prologue (GS)
#define HLSL 1
#define GLSL 0
#define highp
#define mediump
#define lowp
#define vec2 float2
#define vec3 float3
#define vec4 float4
#define ivec2 int2
#define ivec3 int3
#define ivec4 int4
#define mat2 float2x2
#define mat3 float3x3
#define mat4 float4x4
#define mat2x2 float2x2
#define mat3x3 float3x3
#define mat4x4 float4x4
#define mat2x3 float2x3
#define mat2x4 float2x4
#define mat3x2 float3x2
#define mat3x4 float3x4
#define mat4x2 float4x2
#define mat4x3 float4x3
#define CAST2(x)   ((float2)(x))
#define CAST3(x)   ((float3)(x))
#define CAST4(x)   ((float4)(x))
#define CAST3X3(x) ((float3x3)(x))
#define mix(a,b,t) lerp((a),(b),(t))
#define fract      frac
#define dFdx       ddx
#define dFdy(x)    (-ddy(x))

float  atan(float  y, float  x) { return atan2(y, x); }
float2 atan(float2 y, float2 x) { return atan2(y, x); }
float3 atan(float3 y, float3 x) { return atan2(y, x); }
float4 atan(float4 y, float4 x) { return atan2(y, x); }

// `gl_Position` is the SV_Position struct field's GLSL name; rename to the
// canonical struct field name so `IN[0].gl_Position` / `v.gl_Position` both
// resolve correctly.
#define gl_Position _ww_sv_position
#define VS_OUTPUT   WW_VSOut
#define PS_INPUT    WW_PSIn

__SHADER_PLACEHOLD__

)"_str;

inline bool IsShaderTrivia(shader_lex::TokenKind kind) {
    return kind == shader_lex::TokenKind::HSpace || kind == shader_lex::TokenKind::Newline ||
           kind == shader_lex::TokenKind::LineComment ||
           kind == shader_lex::TokenKind::BlockComment;
}

inline shader_lex::Token NextShaderToken(shader_lex::Lexer& lx) {
    return lx.NextSkip(IsShaderTrivia);
}

inline bool PunctIs(shader_lex::Token token, char c) {
    return token.kind == shader_lex::TokenKind::Punct && token.text.size() == rstd::usize(1) &&
           static_cast<char>(token.text[rstd::usize()].to_primitive()) == c;
}

// Legacy WE shaders sometimes address audio float arrays as std140 vec4 groups.
inline bool IsAudioSpectrumName(rstd::ref<rstd::str> name) {
    return name == G_AUDIO_SPEC_16_L || name == G_AUDIO_SPEC_16_R || name == G_AUDIO_SPEC_32_L ||
           name == G_AUDIO_SPEC_32_R || name == G_AUDIO_SPEC_64_L || name == G_AUDIO_SPEC_64_R;
}

struct ShaderBracketExpr {
    usize    close_end;
    ref<str> expr;
};

inline Option<ShaderBracketExpr> ReadBracketExpr(shader_lex::Lexer& lx, ref<str> src) {
    auto open = NextShaderToken(lx);
    if (! PunctIs(open, '[')) return None();

    int          depth      = 1;
    rstd::size_t expr_start = (open.offset + open.text.size()).to_primitive();
    for (;;) {
        auto t = lx.Next();
        if (t.kind == shader_lex::TokenKind::Eof) return None();
        if (! PunctIs(t, '[') && ! PunctIs(t, ']')) continue;

        if (PunctIs(t, '[')) {
            ++depth;
            continue;
        }

        --depth;
        if (depth == 0) {
            return Some(ShaderBracketExpr {
                .close_end = t.offset + t.text.size(),
                .expr      = *src.get(usize(expr_start), t.offset),
            });
        }
    }
}

inline Vec<shader_lex::Token> ExprTokens(ref<str> expr) {
    Vec<shader_lex::Token> tokens;
    shader_lex::Lexer      lx(expr);
    for (;;) {
        auto t = NextShaderToken(lx);
        if (t.kind == shader_lex::TokenKind::Eof) break;
        tokens.push(rstd::move(t));
    }
    return tokens;
}

inline Option<String> TryFlattenPackedAudioIndex(ref<str> group, ref<str> component) {
    auto g = ExprTokens(group);
    auto c = ExprTokens(component);
    if (g.len() == usize(3) && c.len() == usize(3) &&
        g[usize()].kind == shader_lex::TokenKind::Ident &&
        c[usize()].kind == shader_lex::TokenKind::Ident && g[usize()].text == c[usize()].text &&
        PunctIs(g[usize(1)], '/') && PunctIs(c[usize(1)], '%') &&
        g[usize(2)].kind == shader_lex::TokenKind::Int &&
        c[usize(2)].kind == shader_lex::TokenKind::Int && g[usize(2)].text == "4"_str &&
        c[usize(2)].text == "4"_str) {
        auto out = "(int)("_Str;
        out.push_str(g[usize()].text);
        out.push_ascii(u8(')'));
        return Some(rstd::move(out));
    }
    return None();
}

inline String FlattenAudioSpectrumAccess(ref<str> group, ref<str> component) {
    if (auto exact = TryFlattenPackedAudioIndex(group, component)) return rstd::move(*exact);

    auto out = String::make();
    out.reserve(group.size() + component.size() + usize(32));
    out.push_str("((int)("_str);
    out.push_str(group);
    out.push_str(") * 4 + (int)("_str);
    out.push_str(component);
    out.push_str("))"_str);
    return out;
}

inline String NormalizePackedAudioSpectrumAccess(ref<str> src) {
    shader_lex::Lexer lx(src);
    auto              out = String::make();
    usize             copied {};
    bool              changed { false };

    for (;;) {
        auto name = lx.Next();
        if (name.kind == shader_lex::TokenKind::Eof) break;
        if (name.kind != shader_lex::TokenKind::Ident || ! IsAudioSpectrumName(name.text)) continue;

        auto                      save      = lx.Save();
        auto                      group     = ReadBracketExpr(lx, src);
        Option<ShaderBracketExpr> component = group.is_some() ? ReadBracketExpr(lx, src) : None();
        if (! group || ! component) {
            lx.Restore(save);
            continue;
        }

        out.push_str(*src.get(copied, name.offset));
        out.push_str(name.text);
        out.push_ascii(u8('['));
        auto flattened = FlattenAudioSpectrumAccess(group->expr, component->expr);
        out.push_str(flattened.as_str());
        out.push_ascii(u8(']'));
        copied  = component->close_end;
        changed = true;
    }

    if (! changed) return String::make(src);
    out.push_str(*src.get(copied, src.size()));
    return out;
}

// glslang cannot resolve HLSL mul overloads when the leading scalar is an
// untyped integer literal. WE shaders use these literals as float scalars.
inline String NormalizeLeadingIntegerMulLiteral(ref<str> src) {
    shader_lex::Lexer lx(src);
    auto              out = String::make();
    usize             copied {};
    bool              changed { false };

    for (;;) {
        auto name = lx.Next();
        if (name.kind == shader_lex::TokenKind::Eof) break;
        if (name.kind != shader_lex::TokenKind::Ident || name.text != "mul"_str) continue;

        auto save    = lx.Save();
        auto open    = NextShaderToken(lx);
        auto literal = PunctIs(open, '(') ? NextShaderToken(lx) : shader_lex::Token {};
        auto comma =
            literal.kind == shader_lex::TokenKind::Int ? NextShaderToken(lx) : shader_lex::Token {};
        if (! PunctIs(open, '(') || literal.kind != shader_lex::TokenKind::Int ||
            ! PunctIs(comma, ',')) {
            lx.Restore(save);
            continue;
        }

        auto literal_end = literal.offset + literal.text.size();
        out.push_str(*src.get(copied, literal_end));
        out.push_str(".0"_str);
        copied  = literal_end;
        changed = true;
    }

    if (! changed) return String::make(src);
    out.push_str(*src.get(copied, src.size()));
    return out;
}

inline bool LineDefinesMacro(ref<str> src, usize line_start, ref<str> macro_name) {
    shader_lex::Cursor c(src);
    c.SeekTo(line_start);
    if (! c.MatchHashDirective("define"_str)) return false;
    c.SkipHSpace();
    auto ident = c.ReadIdent();
    return ident && *ident == macro_name;
}

inline String UndefBeforeUserMacroDefines(ref<str> src, ref<str> macro_name) {
    bool changed = false;
    auto out     = String::make();
    out.reserve(src.size() + usize(64));
    shader_lex::LineWalker w(src);
    for (; ! w.Done(); w.Step()) {
        if (LineDefinesMacro(src, w.LineStart(), macro_name)) {
            out.push_str("#ifdef "_str);
            out.push_str(macro_name);
            out.push_str("\n#undef "_str);
            out.push_str(macro_name);
            out.push_str("\n#endif\n"_str);
            changed = true;
        }
        out.push_str(*src.get(w.LineStart(), w.LineEnd()));
        if (w.LineEnd() < src.size()) out.push_ascii(u8('\n'));
    }
    return changed ? rstd::move(out) : String::make(src);
}

inline String UndefBeforeConflictingMacroDefines(ref<str> src) {
    auto out = String::make(src);
    for (auto macro_name : rstd::array<ref<str>, 3> { "M_PI_2"_str, "dFdx"_str, "dFdy"_str }) {
        out = UndefBeforeUserMacroDefines(out.as_str(), macro_name);
    }
    return out;
}

inline String LoadGlslInclude(fs::VFS& vfs, ref<str> input) {
    String output;
    output.reserve(input.len());
    usize                  pos {};
    shader_lex::LineWalker w(input);
    for (; ! w.Done(); w.Step()) {
        shader_lex::Cursor c(input);
        c.SeekTo(w.LineStart());
        if (! c.MatchHashDirective("include"_str)) continue;

        // Emit everything up to the directive line, then resolve the include
        // and append the recursively-expanded body. Bytes after the directive
        // on the same line (rare in practice) are skipped — matching the
        // original behavior.
        output.push_str(input.get(pos, w.LineStart()).unwrap());
        auto line  = input.get(w.LineStart(), w.LineEnd()).unwrap();
        auto first = line->find("\""_str);
        auto last  = line->rfind("\""_str);
        if (first.is_none() || last.is_none() || *last <= *first) {
            output.push_str(line);
            pos = w.LineEnd();
            continue;
        }
        auto   include_name = line->get(*first + usize(1), *last).unwrap();
        auto   include_path = rstd::format("/assets/shaders/{}", include_name);
        auto   include      = fs::ReadFileContent(vfs, fs::Path(include_path.as_str()));
        String includeSrc;
        if (include.is_ok()) {
            includeSrc = rstd::move(include).unwrap_unchecked();
        } else {
            rstd_error("Can't read shader include {}", include_name);
        }
        output.push_str("\n//-----include "_str);
        output.push_str(include_name);
        output.push_str("\n"_str);
        output.push_str(LoadGlslInclude(vfs, includeSrc.as_str()).as_str());
        // WE shaders routinely pass a vector opacity (opacity * mask) to the
        // scalar ApplyBlending, relying on fxc's implicit vector->scalar
        // truncation. glslang's HLSL frontend won't truncate at the call, so
        // emit forwarding overloads right after the definition. Gate on the
        // directly-loaded file (not the recursively-expanded body) so a parent
        // header that nests common_blending.h doesn't re-inject the overloads.
        if (includeSrc.as_str()->contains("ApplyBlending(const int"_str)) {
            output.push_str("\nvec3 ApplyBlending(const int bm, in vec3 A, in vec3 B, in vec2 o) { "
                            "return ApplyBlending(bm, A, B, o.x); }"
                            "\nvec3 ApplyBlending(const int bm, in vec3 A, in vec3 B, in vec3 o) { "
                            "return ApplyBlending(bm, A, B, o.x); }"
                            "\nvec3 ApplyBlending(const int bm, in vec3 A, in vec3 B, in vec4 o) { "
                            "return ApplyBlending(bm, A, B, o.x); }\n"_str);
        }
        output.push_str("\n//-----include end\n"_str);
        pos = w.LineEnd();
    }
    output.push_str(input.get(pos, input.len()).unwrap());
    return output;
}

// ParseShader implementation moved to ShaderParser_Pegtl.cpp.
// Declaration is reachable through wescene.pkg.parse via the same module.

// Find a safe spot in `src` to splice an `#include` line into. The chosen
// position lies after every top-level `attribute/varying/uniform/struct`
// declaration, before `void main(`, and outside any `#if/#endif` block.
// Returns 0 when no preceding decls are found or the source has multiple
// entry points (post-include shaders we can't reason about).
inline usize FindIncludeInsertPos(ref<str> src, usize startPos) {
    using shader_lex::PpKind;
    (void)startPos;
    auto main = src.find("void main("_str);
    if (main.is_none()) return usize();
    const auto main_pos = *main;
    if (src.get(main_pos + usize(2), src.len())->contains("void main("_str)) return usize();
    Option<usize>            after_pos;
    Vec<array<usize, 2>>     if_ranges;
    Vec<usize>               if_stack;
    const array<ref<str>, 4> keywords {
        "attribute"_str, "varying"_str, "uniform"_str, "struct"_str
    };
    shader_lex::LineWalker w(src);
    for (; ! w.Done(); w.Step()) {
        if (w.LineStart() >= main_pos) break;
        auto               line_end = rstd::cmp::min(w.LineEnd(), main_pos);
        shader_lex::Cursor c(src, w.LineStart());
        c.SkipHSpace();
        if (c.Eof() || c.Pos() >= line_end) continue;
        auto end = w.LineEnd() < src.len() ? w.LineEnd() + usize(1) : w.LineEnd();
        if (c.Peek() == '#') {
            shader_lex::Cursor cc(src, w.LineStart());
            auto               kind = shader_lex::ClassifyPreproc(cc);
            if (kind == PpKind::If || kind == PpKind::Ifdef || kind == PpKind::Ifndef) {
                if_stack.push(w.LineStart());
            } else if (kind == PpKind::Endif) {
                if (auto start = if_stack.pop(); start.is_some())
                    if_ranges.push(array<usize, 2> { *start, end });
            }
        } else {
            for (auto keyword : keywords) {
                shader_lex::Cursor probe(src, c.Pos());
                if (probe.MatchKeyword(keyword) && probe.Pos() < line_end &&
                    shader_lex::IsHSpace(probe.Peek())) {
                    after_pos = Some(end);
                    break;
                }
            }
        }
    }
    auto pos = after_pos.is_some() ? rstd::cmp::min(*after_pos, main_pos) : usize();
    for (const auto& range : if_ranges)
        if (pos > range[usize()] && pos <= range[usize(1)]) pos = range[usize(1)];
    return rstd::cmp::min(pos, main_pos);
}

// Comment out stray `#endif` directives with no matching `#if`. A class of
// WE-shipped community shader templates (audio_bars / dot_matrix / sine_wave
// variants — 244 of the corpus failures pre-fix) has one extra `#endif`
// past the file's last `#if`. WE's HLSL toolchain tolerates this; glslang
// rejects it as a preprocess error. Stack-walk the source, and when
// `#endif` would pop an empty stack, comment the line instead.
inline String BalanceConditionals(String src) {
    using shader_lex::PpKind;
    auto   source = src.as_str();
    int    depth  = 0;
    String out;
    out.reserve(usize(src.len().to_primitive() + 32));
    shader_lex::LineWalker w(source);
    for (; ! w.Done(); w.Step()) {
        shader_lex::Cursor c(source);
        c.SeekTo(w.LineStart());
        auto kind        = shader_lex::ClassifyPreproc(c);
        bool stray_endif = false;
        switch (kind) {
        case PpKind::If:
        case PpKind::Ifdef:
        case PpKind::Ifndef: ++depth; break;
        case PpKind::Endif:
            if (depth == 0)
                stray_endif = true;
            else
                --depth;
            break;
        default: break;
        }
        if (stray_endif) out.push_str("// (ww stray-endif) "_str);
        out.push_str(*source.get(w.LineStart(), w.LineEnd()));
        if (w.LineEnd().to_primitive() < src.len().to_primitive()) out.push_ascii('\n');
    }
    return out;
}

inline String Preprocessor(ref<str> in_src, ShaderType type, const Combos& combos,
                           PreprocessorInfo& process_info) {
    String with_prologue = owe::ShaderParser::PreShaderHeader(in_src, combos, type);

    // `#require` is a WE-specific marker, not a real preprocessor directive.
    // Prefix `//` to neutralize it. Allowed leading horizontal whitespace.
    {
        String out;
        out.reserve(with_prologue.len());
        auto                   source = with_prologue.as_str();
        shader_lex::LineWalker w(source);
        for (; ! w.Done(); w.Step()) {
            shader_lex::Cursor c(source);
            c.SeekTo(w.LineStart());
            if (c.MatchHashDirective("require"_str)) {
                c.SkipHSpace();
                auto requirement = c.ReadIdent();
                if (requirement.is_some() && *requirement == "LightingV1"_str) {
                    out.push_str(lighting_v1_source);
                    if (w.LineEnd().to_primitive() < with_prologue.len().to_primitive())
                        out.push_ascii('\n');
                    continue;
                }
                out.push_str("//"_str);
            }
            out.push_str(*source.get(w.LineStart(), w.LineEnd()));
            if (w.LineEnd().to_primitive() < with_prologue.len().to_primitive())
                out.push_ascii('\n');
        }
        with_prologue = rstd::move(out);
    }

    with_prologue = BalanceConditionals(rstd::move(with_prologue));

    // Run glslang's own preprocessor: every `#if SKINNING` / `#if FOG_COMPUTED
    // && (...)` / `#if BLENDMODE == 0` block resolves, combo names (BONECOUNT,
    // …) expand, and `#include`s (already inlined in PreShaderSrc, but
    // harmless to re-run) get handled. The regex extraction below then sees
    // only live declarations.
    // All stages route through glslang's HLSL frontend. Bridging macros in
    // the prologue turn GLSL types/intrinsics into HLSL equivalents.
    vulkan::SourceLang lang = vulkan::SourceLang::Hlsl;
    String             src;
    const auto         input = with_prologue.as_str();
    if (! vulkan::Preprocess(input, type, lang, src)) {
        // Fall through: subsequent compile will fail loudly with the same
        // diagnostics. Keep with_prologue so the failing path matches what
        // a developer would see if they bypassed the preprocess step.
        src = rstd::into(input);
    }
    auto source = src.as_str();

    // GS source uses `in`/`out` storage classes; VS/FS use `attribute`/`varying`.
    ForEachDeclLine(source, kStorageKeywords.as_slice(), [&](const DeclMatch& m) {
        // `in`/`out` keep their GLSL storage direction in every stage.
        // Legacy `attribute` is a VS input; `varying` is produced by VS and
        // consumed by FS.
        bool is_input = (m.storage == "attribute"_str) || (m.storage == "in"_str) ||
                        (m.storage == "varying"_str && type == ShaderType::FRAGMENT);
        auto line     = String::make(source.get(usize(m.start), usize(m.end)).unwrap());
        auto name     = String::make(m.name);
        if (is_input)
            (void)process_info.input.insert(rstd::move(name), rstd::move(line));
        else
            (void)process_info.output.insert(rstd::move(name), rstd::move(line));
    });

    // Non-sampler uniform decls feed Finalprocessor's shared cbuffer.
    // Sampler-typed uniforms are emitted as Texture/SamplerState pairs and
    // captured in active_tex_slots instead.
    ForEachDeclLine(source, kUniformKeyword.as_slice(), [&](const DeclMatch& m) {
        if (IsSamplerType(m.type)) {
            // Track active sampler slot if it's a `g_TextureN`.
            const ref<str> kTex { "g_Texture"_str };
            if (m.name.size() > kTex.size() && m.name.starts_with(kTex)) {
                auto               num = *m.name.get(kTex.len(), m.name.len());
                shader_lex::Cursor cursor(num);
                if (auto digits = cursor.ReadInt(); digits.is_some()) {
                    auto slot = rstd::from_str<u32>(*digits);
                    if (slot.is_ok()) (void)process_info.active_tex_slots.insert(slot.unwrap());
                }
            }
            return;
        }
        auto type = String::make(m.type);
        type.push_str(m.array);
        (void)process_info.uniforms.insert(rstd::into(m.name), rstd::move(type));
    });
    return src;
}

// Pass GLSL type names through unchanged; aliases like `float`/`float2` get
// re-emitted as is for HLSL-flavoured leftovers.
inline ref<str> ToGLSLType(ref<str> t) {
    if (t == "float2"_str) return "vec2"_str;
    if (t == "float3"_str) return "vec3"_str;
    if (t == "float4"_str) return "vec4"_str;
    if (t == "int2"_str) return "ivec2"_str;
    if (t == "int3"_str) return "ivec3"_str;
    if (t == "int4"_str) return "ivec4"_str;
    if (t == "uint2"_str) return "uvec2"_str;
    if (t == "uint3"_str) return "uvec3"_str;
    if (t == "uint4"_str) return "uvec4"_str;
    if (t == "float2x2"_str) return "mat2"_str;
    if (t == "float3x3"_str) return "mat3"_str;
    if (t == "float4x4"_str) return "mat4"_str;
    return t;
}

// Inverse of ToGLSLType: bridge GLSL aliases back to HLSL canonical names
// (used by the GS synth which feeds HLSL to glslang's HLSL frontend).
inline ref<str> ToHLSLType(ref<str> t) {
    if (t == "vec2"_str) return "float2"_str;
    if (t == "vec3"_str) return "float3"_str;
    if (t == "vec4"_str) return "float4"_str;
    if (t == "ivec2"_str) return "int2"_str;
    if (t == "ivec3"_str) return "int3"_str;
    if (t == "ivec4"_str) return "int4"_str;
    if (t == "uvec2"_str) return "uint2"_str;
    if (t == "uvec3"_str) return "uint3"_str;
    if (t == "uvec4"_str) return "uint4"_str;
    if (t == "mat2"_str || t == "mat2x2"_str) return "float2x2"_str;
    if (t == "mat3"_str || t == "mat3x3"_str) return "float3x3"_str;
    if (t == "mat4"_str || t == "mat4x4"_str) return "float4x4"_str;
    if (t == "mat2x3"_str) return "float3x2"_str;
    if (t == "mat2x4"_str) return "float4x2"_str;
    if (t == "mat3x2"_str) return "float2x3"_str;
    if (t == "mat3x4"_str) return "float4x3"_str;
    if (t == "mat4x2"_str) return "float2x4"_str;
    if (t == "mat4x3"_str) return "float3x4"_str;
    return t;
}

// Declarations borrow sources kept alive throughout final synthesis.
struct IODecl {
    char     storage; // 'a' for attribute, 'v' for varying, 'i' for GS `in`, 'o' for GS `out'
    ref<str> type;    // GLSL type as captured (vec2/vec4/mat3/...)
    ref<str> name;
    ref<str> array; // "[N]" or empty
};

inline char StorageCharFor(ref<str> storage_word) {
    if (storage_word == "attribute"_str) return 'a';
    if (storage_word == "in"_str) return 'i';
    if (storage_word == "out"_str) return 'o';
    return 'v'; // varying
}

struct SamplerDecl {
    ref<str> sampler_type; // "sampler2D" / "samplerCube" / ...
    ref<str> name;
};

inline tuple<Vec<SamplerDecl>, String> ScanAndStripSamplers(ref<str> src) {
    Vec<SamplerDecl> decls;
    String           out;
    out.reserve(src.len());
    rstd::size_t cursor = 0;
    ForEachDeclLine(src, kUniformKeyword.as_slice(), [&](const DeclMatch& m) {
        if (! IsSamplerType(m.type)) return;
        out.push_str(*src.get(usize(cursor), usize(m.start)));
        out.push_str(*src.get(usize(m.start), usize(m.start + m.keep_prefix)));
        cursor = m.end;
        decls.push({ m.type, m.name });
    });
    out.push_str(*src.get(usize(cursor), src.len()));
    return { rstd::move(decls), rstd::move(out) };
}

inline ref<str> HLSLSamplerType(ref<str> glsl) {
    if (glsl == "sampler2D"_str) return "Texture2D<float4>"_str;
    if (glsl == "sampler3D"_str) return "Texture3D<float4>"_str;
    if (glsl == "samplerCube"_str) return "TextureCube<float4>"_str;
    // GLSL shadow / comparison samplers: scalar-result texture with a
    // SamplerComparisonState. We bind a Texture2D<float> and a paired
    // SamplerComparisonState (the latter chosen via HLSLSamplerStateType).
    if (glsl == "sampler2DComparison"_str || glsl == "sampler2DShadow"_str)
        return "Texture2D<float>"_str;
    return "Texture2D<float4>"_str;
}

inline ref<str> HLSLSamplerStateType(ref<str> glsl) {
    if (glsl == "sampler2DComparison"_str || glsl == "sampler2DShadow"_str)
        return "SamplerComparisonState"_str;
    return "SamplerState"_str;
}

inline bool IsSamplerCombinedImage(ref<str> glsl) {
    // All sampler types in WE are combined image samplers from the
    // descriptor-set side. The HLSL sampling intrinsic differs (Sample
    // vs SampleCmp) but binding semantics are identical.
    (void)glsl;
    return true;
}

// Strip every `uniform TYPE NAME;` declaration (including samplers — already
// stripped by ScanAndStripSamplers when called in sequence, idempotent). The
// caller re-emits them as members of a shared cbuffer.
inline String StripUniforms(ref<str> src) {
    String out;
    out.reserve(src.len());
    rstd::size_t cursor = 0;
    ForEachDeclLine(src, kUniformKeyword.as_slice(), [&](const DeclMatch& m) {
        out.push_str(*src.get(usize(cursor), usize(m.start)));
        out.push_str(*src.get(usize(m.start), usize(m.start + m.keep_prefix)));
        cursor = m.end;
    });
    out.push_str(*src.get(usize(cursor), src.len()));
    return out;
}

inline bool IsGlobalVariableQualifier(ref<str> token) {
    return token == "const"_str || token == "precise"_str || token == "row_major"_str ||
           token == "column_major"_str || token == "static"_str;
}

// GLSL file-scope variables are private shader state. HLSL instead places an
// unqualified file-scope variable in an implicit $Global uniform block.
inline String QualifyGlobalVariablesForHlsl(ref<str> src) {
    auto              source = src;
    shader_lex::Lexer lexer(source);
    Vec<usize>        insertions;
    int               brace_depth { 0 };
    bool              statement_start { true };
    bool              directive { false };

    for (;;) {
        auto token = lexer.Next();
        if (token.kind == shader_lex::TokenKind::Eof) break;
        if (token.kind == shader_lex::TokenKind::Newline) {
            directive = false;
            continue;
        }
        if (directive || token.kind == shader_lex::TokenKind::HSpace ||
            token.kind == shader_lex::TokenKind::LineComment ||
            token.kind == shader_lex::TokenKind::BlockComment) {
            continue;
        }
        if (token.kind == shader_lex::TokenKind::Hash && brace_depth == 0) {
            directive = true;
            continue;
        }
        if (PunctIs(token, '{')) {
            ++brace_depth;
            statement_start = false;
            continue;
        }
        if (PunctIs(token, '}')) {
            if (brace_depth > 0) --brace_depth;
            statement_start = brace_depth == 0;
            continue;
        }
        if (brace_depth != 0) continue;
        if (PunctIs(token, ';')) {
            statement_start = true;
            continue;
        }
        if (! statement_start) continue;
        if (token.kind != shader_lex::TokenKind::Ident) continue;
        if (token.text == SHADER_PLACEHOLD) continue;
        statement_start = false;

        const auto declaration_start = token.offset;
        const auto probe             = lexer.Save();
        bool       has_static        = false;
        bool       variable          = false;
        auto       type              = token;
        while (type.kind == shader_lex::TokenKind::Ident && IsGlobalVariableQualifier(type.text)) {
            has_static = has_static || type.text == "static"_str;
            type       = NextShaderToken(lexer);
        }
        if (type.kind == shader_lex::TokenKind::Ident) {
            auto name = NextShaderToken(lexer);
            if (name.kind == shader_lex::TokenKind::Ident) {
                auto suffix = NextShaderToken(lexer);
                variable = PunctIs(suffix, ';') || PunctIs(suffix, '=') || PunctIs(suffix, '[') ||
                           PunctIs(suffix, ',') || PunctIs(suffix, ':');
            }
        }
        lexer.Restore(probe);
        if (variable && ! has_static) insertions.push(usize(declaration_start));
    }

    if (insertions.is_empty()) return String::make(src);
    String out;
    out.reserve(usize(src.len().to_primitive() + insertions.len().to_primitive() * 7));
    rstd::size_t copied {};
    for (auto offset : insertions) {
        auto pos = offset.to_primitive();
        out.push_str(*src.get(usize(copied), usize(pos)));
        out.push_str("static "_str);
        copied = pos;
    }
    out.push_str(*src.get(usize(copied), src.len()));
    return out;
}

inline Option<IODecl> ParseIODecl(ref<str> line) {
    // Skip leading newline / CR that the capture loops preserved as an anchor.
    rstd::size_t start = 0;
    auto         bytes = line.as_bytes();
    while (start < bytes.len().to_primitive() &&
           shader_lex::IsVSpace(static_cast<char>(bytes[usize(start)].to_primitive())))
        ++start;
    auto m = TryParseDeclLine(line, usize(start), kStorageKeywords.as_slice());
    if (m.is_none()) return None();
    return Some(IODecl { StorageCharFor(m->storage), m->type, m->name, m->array });
}

enum class IODeclPrecedence
{
    KeepExisting,
    PreferIncoming,
};

inline void AddIODecl(Vec<IODecl>& decls, const IODecl& decl, IODeclPrecedence precedence) {
    for (auto& existing : decls) {
        if (existing.name != decl.name) continue;
        if (precedence == IODeclPrecedence::PreferIncoming) {
            existing.type  = decl.type;
            existing.array = decl.array;
        }
        return;
    }
    decls.push(IODecl(decl));
}

// Pull all `attribute|varying|in|out TYPE NAME;` decls out, return them
// structured + a copy of the source with the lines removed. Stripping is
// essential: `attribute`/`varying` are not HLSL keywords; the entry-point
// synthesizer re-emits canonical `static TYPE NAME;` decls so it never
// drifts from what DXC's preprocessor actually compiled.
inline tuple<Vec<IODecl>, String> ScanAndStripIO(ref<str> src) {
    Vec<IODecl> decls;
    String      out;
    out.reserve(usize(src.len().to_primitive()));
    rstd::size_t cursor = 0;
    ForEachDeclLine(src, kStorageKeywords.as_slice(), [&](const DeclMatch& m) {
        out.push_str(*src.get(usize(cursor), usize(cursor + (m.start - cursor))));
        out.push_str(*src.get(usize(m.start), usize(m.start + (m.keep_prefix))));
        cursor = m.end;
        decls.push({ StorageCharFor(m.storage), m.type, m.name, m.array });
    });
    out.push_str(*src.get(usize(cursor), src.len()));
    return { rstd::move(decls), rstd::move(out) };
}

// Synthesizer output split in two: `pre` is `static TYPE NAME;` decls
// that must precede the user `void main()` (HLSL needs identifiers
// declared before use). `post` is the HLSL entry-point wrapper that
// must follow `void main()` so it can call the renamed `shader_main()`.
struct SynthOutput {
    String pre;
    String post;
};

inline rstd::size_t ArraySlots(ref<str> arr) {
    const auto bytes = arr.as_bytes();
    if (bytes.len() < usize(3) || bytes.first().unwrap().get() != u8('[') ||
        bytes.last().unwrap().get() != u8(']'))
        return 1;
    rstd::size_t n = 0;
    for (usize i(1); i + usize(1) < bytes.len(); ++i) {
        auto c = bytes[i];
        if (c < u8('0') || c > u8('9')) return 1;
        n = n * 10 + static_cast<rstd::size_t>((c - u8('0')).to_primitive());
    }
    return n > 0 ? n : 1;
}

struct PackedIOArray {
    ref<str>     vector_type;
    rstd::size_t elements;
    rstd::size_t components;
    rstd::size_t slots;
};

inline Option<PackedIOArray> BuildPackedIOArray(const IODecl& decl) {
    if (decl.array.is_empty()) return None();

    const auto elements = ArraySlots(decl.array);
    if (elements <= 1) return None();

    const auto   type = ToHLSLType(decl.type);
    ref<str>     vector_type;
    rstd::size_t components {};
    if (type == "float"_str) {
        vector_type = "float4"_str;
        components  = 1;
    } else if (type == "float2"_str) {
        vector_type = "float4"_str;
        components  = 2;
    } else if (type == "float3"_str) {
        vector_type = "float4"_str;
        components  = 3;
    } else if (type == "int"_str) {
        vector_type = "int4"_str;
        components  = 1;
    } else if (type == "int2"_str) {
        vector_type = "int4"_str;
        components  = 2;
    } else if (type == "int3"_str) {
        vector_type = "int4"_str;
        components  = 3;
    } else if (type == "uint"_str) {
        vector_type = "uint4"_str;
        components  = 1;
    } else if (type == "uint2"_str) {
        vector_type = "uint4"_str;
        components  = 2;
    } else if (type == "uint3"_str) {
        vector_type = "uint4"_str;
        components  = 3;
    } else {
        return None();
    }

    if (elements > rstd::usize::MAX.to_primitive() / components) return None();
    const auto scalar_count = elements * components;
    return Some(PackedIOArray {
        .vector_type = vector_type,
        .elements    = elements,
        .components  = components,
        .slots       = (scalar_count + 3) / 4,
    });
}

inline String PackedIOField(const IODecl& decl) { return rstd::format("_ww_packed_{}", decl.name); }

inline void EmitPackedIOCopy(String& out, const IODecl& decl, ref<str> packed_owner, bool pack) {
    auto layout = BuildPackedIOArray(decl);
    if (layout.is_none()) return;

    static constexpr ref<str> components { "xyzw"_str };
    const auto                field = PackedIOField(decl);
    for (rstd::size_t element = 0; element < layout->elements; ++element) {
        for (rstd::size_t component = 0; component < layout->components; ++component) {
            const auto scalar = element * layout->components + component;
            String     source = rstd::format("{}[{}]", decl.name, element);
            if (layout->components > 1)
                source.push_str(
                    rstd::format(".{}", *components.get(usize(component), usize(component + 1))));
            String packed = rstd::format("{}.{}[{}].{}",
                                         packed_owner,
                                         field,
                                         scalar / 4,
                                         *components.get(usize(scalar % 4), usize(scalar % 4 + 1)));
            out.push_str(
                rstd::format("    {} = {};\n", (pack ? packed : source), (pack ? source : packed)));
        }
    }
}

// Build `layout(location=N) in/out TYPE NAME[arr];` declarations from a
// list of IO decls, with locations assigned alphabetically so neighbouring
// stages agree without explicit coordination. `is_input` picks the storage
// qualifier (in vs out). Returns the joined block.
inline String EmitStageIOLayout(Vec<IODecl> decls, bool is_input) {
    // gl_Position is a GLSL builtin; never re-declare it. _ww_sv_position is
    // the GS-side macro alias for the same slot.
    decls.retain([](const IODecl& d) {
        return d.name != "gl_Position"_str && d.name != "_ww_sv_position"_str;
    });
    sort_unstable_by(decls.as_mut_slice().as_mut_ref(), [](const IODecl& a, const IODecl& b) {
        return a.name.bytes().cmp(b.name.bytes()) < 0;
    });
    ref<str>     qual = is_input ? "in"_str : "out"_str;
    String       out;
    rstd::size_t loc = 0;
    for (const auto& d : decls) {
        out.push_str(rstd::format(
            "layout(location = {}) {} {} {}{};\n", loc, qual, ToGLSLType(d.type), d.name, d.array));
        loc += ArraySlots(d.array);
    }
    return out;
}

// HLSL-side struct emission for the GS synth. Drops `[[vk::location(N)]]`
// for the same reason as EmitVSFSStruct — glslang's HLSL frontend collapses
// every element of an explicitly-located array onto the same Location.
inline String EmitGSHLSLStruct(ref<str> name, Vec<IODecl> decls) {
    decls.retain([](const IODecl& d) {
        return d.name != "gl_Position"_str && d.name != "_ww_sv_position"_str;
    });
    sort_unstable_by(decls.as_mut_slice().as_mut_ref(), [](const IODecl& a, const IODecl& b) {
        return a.name.bytes().cmp(b.name.bytes()) < 0;
    });
    String out;
    out.push_str("struct "_str);
    out.push_str(name);
    out.push_str(" {\n"_str);
    out.push_str("    float4 _ww_sv_position : SV_Position;\n"_str);
    for (const auto& d : decls) {
        out.push_str(
            rstd::format("    {} {}{} : {};\n", ToHLSLType(d.type), d.name, d.array, d.name));
    }
    out.push_str("};\n"_str);
    return out;
}

inline ref<str> HLSLSystemSemantic(ref<str> name) {
    if (name == "gl_VertexID"_str) return "SV_VertexID"_str;
    if (name == "gl_InstanceID"_str) return "SV_InstanceID"_str;
    if (name == "gl_ViewportIndex"_str) return "SV_ViewportArrayIndex"_str;
    return {};
}

// HLSL-side struct emission for VS/FS entry points. Same as the GS variant
// but the SV_Position field is included only when the struct represents a
// VS output / FS input (HLSL needs it for rasterizer setup); attributes
// (VS input) don't carry it.
//
// No `[[vk::location(N)]]` is emitted. glslang's HLSL frontend has a known
// bug: `[[vk::location(N)]] TYPE FIELD[K]` puts every element of the array
// at Location N (instead of N, N+1, …, N+K-1). Dropping the explicit
// attribute lets glslang auto-assign sequential locations in declaration
// order. Both VS and FS sort fields alphabetically over the same
// cross-stage union, so the location assignment is stable across stages.
// Direct VS/FS interfaces pack sub-vec4 arrays because glslang otherwise
// consumes one full location per element and can exceed Vulkan's component
// limit even when the scalar payload itself fits.
inline String EmitVSFSStruct(ref<str> name, Vec<IODecl> decls, bool include_sv_position,
                             bool pack_arrays = false) {
    decls.retain([](const IODecl& d) {
        return d.name != "gl_Position"_str && d.name != "_ww_sv_position"_str;
    });
    sort_unstable_by(decls.as_mut_slice().as_mut_ref(), [](const IODecl& a, const IODecl& b) {
        return a.name.bytes().cmp(b.name.bytes()) < 0;
    });
    String out;
    out.push_str("struct "_str);
    out.push_str(name);
    out.push_str(" {\n"_str);
    if (include_sv_position) {
        out.push_str("    float4 _ww_sv_position : SV_Position;\n"_str);
    }
    for (const auto& d : decls) {
        const auto semantic = HLSLSystemSemantic(d.name);
        auto       packed   = pack_arrays ? BuildPackedIOArray(d) : None();
        if (packed.is_some()) {
            out.push_str(rstd::format("    {} {}[{}] : {};\n",
                                      packed->vector_type,
                                      PackedIOField(d),
                                      packed->slots,
                                      d.name));
        } else {
            out.push_str(rstd::format("    {} {}{} : {};\n",
                                      ToHLSLType(d.type),
                                      d.name,
                                      d.array,
                                      (semantic.is_empty() ? d.name : semantic)));
        }
    }
    out.push_str("};\n"_str);
    return out;
}

// Emit the HLSL synth block (pre = decls / structs / cbuffer / samplers,
// post = entry-point wrapper) for VS or FS. Locations are alphabetical so
// vert/frag stages agree without explicit coordination — both are called
// with the same cross-stage varying union.
inline SynthOutput SynthesizeHLSLEntry(ShaderType stage, Vec<IODecl> attrs, Vec<IODecl> varyings,
                                       bool pack_varying_arrays) {
    SynthOutput so;
    if (stage == ShaderType::GEOMETRY) return so;

    // gl_Position propagates via the SV_Position field, not a regular slot.
    // Filter both names (the GS prologue rewrites `gl_Position` to
    // `_ww_sv_position`, so its post-preprocess form needs filtering too).
    auto drop_position = [](Vec<IODecl>& v) {
        v.retain([](const IODecl& d) {
            return d.name != "gl_Position"_str && d.name != "_ww_sv_position"_str;
        });
    };
    drop_position(attrs);
    drop_position(varyings);

    auto by_name = [](const IODecl& a, const IODecl& b) {
        return a.name.bytes().cmp(b.name.bytes()) < 0;
    };
    sort_unstable_by(attrs.as_mut_slice().as_mut_ref(), by_name);
    sort_unstable_by(varyings.as_mut_slice().as_mut_ref(), by_name);

    // Static globals so the user shader body resolves `a_Position`,
    // `v_TexCoord`, etc. regardless of #if-branch visibility — the wrapper
    // copies from/to the entry struct.
    so.pre.push_str("\n// === auto-generated stage I/O statics ===\n"_str);
    for (const auto& d : attrs) {
        so.pre.push_str(rstd::format("static {} {}{};\n", ToHLSLType(d.type), d.name, d.array));
    }
    for (const auto& d : varyings) {
        so.pre.push_str(rstd::format("static {} {}{};\n", ToHLSLType(d.type), d.name, d.array));
    }

    String& out = so.post;
    out.push_str("\n// === auto-generated entry point ===\n"_str);
    if (stage == ShaderType::VERTEX) {
        out.push_str(EmitVSFSStruct("WW_VSIn"_str, attrs.clone(), /*sv_pos=*/false));
        out.push_str(
            EmitVSFSStruct("WW_VSOut"_str, varyings.clone(), /*sv_pos=*/true, pack_varying_arrays));
        out.push_str("WW_VSOut main_vs(WW_VSIn _ww_in) {\n"_str);
        for (const auto& a : attrs) {
            out.push_str(rstd::format("    {} = _ww_in.{};\n", a.name, a.name));
        }
        out.push_str("    shader_main();\n"_str);
        out.push_str("    WW_VSOut _ww_out;\n"_str);
        out.push_str("    _ww_out._ww_sv_position = gl_Position;\n"_str);
        for (const auto& v : varyings) {
            if (v.name == "gl_Position"_str || v.name == "_ww_sv_position"_str) continue;
            if (pack_varying_arrays && BuildPackedIOArray(v).is_some()) {
                EmitPackedIOCopy(out, v, "_ww_out"_str, true);
            } else {
                out.push_str(rstd::format("    _ww_out.{} = {};\n", v.name, v.name));
            }
        }
        out.push_str("    return _ww_out;\n"_str);
        out.push_str("}\n"_str);
    } else { // FRAGMENT
        out.push_str(
            EmitVSFSStruct("WW_PSIn"_str, varyings.clone(), /*sv_pos=*/true, pack_varying_arrays));
        out.push_str("float4 main_ps(WW_PSIn _ww_in) : SV_Target0 {\n"_str);
        out.push_str("    gl_FragCoord = _ww_in._ww_sv_position;\n"_str);
        for (const auto& v : varyings) {
            if (v.name == "gl_Position"_str || v.name == "_ww_sv_position"_str) continue;
            if (pack_varying_arrays && BuildPackedIOArray(v).is_some()) {
                EmitPackedIOCopy(out, v, "_ww_in"_str, false);
            } else {
                out.push_str(rstd::format("    {} = _ww_in.{};\n", v.name, v.name));
            }
        }
        out.push_str("    shader_main();\n"_str);
        out.push_str("    return glOutColor;\n"_str);
        out.push_str("}\n"_str);
    }
    return so;
}

// Find a literal `void main()` call in `src` (no regex). Replace with the GS
// entry-point signature. Returns the modified source unchanged if no match.
inline String RewriteGSMain(String src) {
    static constexpr ref<str> marker { "void main()"_str };
    static constexpr ref<str> repl {
        "void main_gs(point WW_VSOut IN[1], inout TriangleStream<WW_PSIn> OUT)"_str
    };
    if (auto pos = src.as_str().find(marker); pos.is_some()) {
        src.replace_range(*pos, *pos + marker.len(), repl);
    }
    return src;
}

// std140 base alignment + size for one element (not the array — caller
// scales). HLSL form (`floatRxC`, `floatN`, scalars). Unknown types fall back
// to vec4-equivalent which is always safely-aligned, never under-padded.
struct Std140Layout {
    rstd::size_t align;
    rstd::size_t size;
};
inline Std140Layout Std140Base(ref<str> hlsl_base) {
    if (hlsl_base == "float"_str || hlsl_base == "int"_str || hlsl_base == "uint"_str ||
        hlsl_base == "bool"_str)
        return { 4, 4 };
    if (hlsl_base == "float2"_str || hlsl_base == "int2"_str || hlsl_base == "uint2"_str)
        return { 8, 8 };
    if (hlsl_base == "float3"_str || hlsl_base == "int3"_str || hlsl_base == "uint3"_str)
        return { 16, 12 };
    if (hlsl_base == "float4"_str || hlsl_base == "int4"_str || hlsl_base == "uint4"_str)
        return { 16, 16 };
    // column_major float<R>x<C> = C columns of vec<R>, each padded to 16
    // bytes by std140 → 16*C bytes total.
    if (hlsl_base.len() == usize(8) && *hlsl_base.get(usize(), usize(5)) == "float"_str &&
        static_cast<char>(hlsl_base.as_bytes()[usize(6)].to_primitive()) == 'x' &&
        static_cast<char>(hlsl_base.as_bytes()[usize(5)].to_primitive()) >= '2' &&
        static_cast<char>(hlsl_base.as_bytes()[usize(5)].to_primitive()) <= '4' &&
        static_cast<char>(hlsl_base.as_bytes()[usize(7)].to_primitive()) >= '2' &&
        static_cast<char>(hlsl_base.as_bytes()[usize(7)].to_primitive()) <= '4') {
        rstd::size_t cols =
            (rstd::size_t)(static_cast<char>(hlsl_base.as_bytes()[usize(7)].to_primitive()) - '0');
        return { 16, cols * 16 };
    }
    return { 16, 16 };
}

inline tuple<ref<str>, ref<str>> SplitUniformType(ref<str> ty) {
    if (auto pos = ty.find("["_str); pos.is_some())
        return { *ty.get(usize(), *pos), *ty.get(*pos, ty.len()) };
    return { ty, ref<str> {} };
}

struct UniformLayout {
    ref<str>     array;
    ref<str>     hlsl_ty;
    rstd::size_t align;
    rstd::size_t size;
};

inline rstd::size_t ParseArrayCount(ref<str> arr) {
    auto bytes = arr.as_bytes();
    if (bytes.len() < usize(3) || bytes.first().unwrap().get() != u8('[') ||
        bytes.last().unwrap().get() != u8(']'))
        return 1;
    rstd::size_t n = 0;
    for (usize i(1); i + usize(1) < bytes.len(); ++i) {
        auto c = bytes[i];
        if (c == u8(' ') || c == u8('\t')) continue;
        if (c < u8('0') || c > u8('9')) return 1;
        n = n * 10 + static_cast<rstd::size_t>((c - u8('0')).to_primitive());
    }
    return n == 0 ? 1 : n;
}

inline UniformLayout LayoutUniform(ref<str> ty) {
    const auto [base_ty, array] = SplitUniformType(ty);
    auto       hlsl_ty          = ToHLSLType(base_ty);
    const auto n                = ParseArrayCount(array);
    const auto L                = Std140Base(hlsl_ty);
    return {
        .array   = array,
        .hlsl_ty = rstd::move(hlsl_ty),
        .align   = (n > 1) ? rstd::size_t(16) : L.align,
        .size    = (n > 1) ? ((L.size + 15) & ~rstd::size_t(15)) * n : L.size,
    };
}

inline void MergeUniform(BTreeMap<String, String>& uniforms_union, ref<str> name, ref<str> ty) {
    auto current = uniforms_union.get_mut(name);
    if (current.is_none()) {
        (void)uniforms_union.insert(rstd::into(name), rstd::into(ty));
        return;
    }
    const auto old_layout = LayoutUniform((**current).as_str());
    const auto new_layout = LayoutUniform(ty);
    if (new_layout.size > old_layout.size ||
        (new_layout.size == old_layout.size && new_layout.align > old_layout.align))
        **current = rstd::into(ty);
}

BTreeMap<String, String> BuildUniformUnion(slice<ShaderUnit> units) {
    BTreeMap<String, String> uniforms;
    for (const auto& unit : units) {
        for (const auto& [name, ty] : unit.preprocess_info.uniforms.iter()) {
            MergeUniform(uniforms, name->as_str(), ty->as_str());
        }
    }
    return uniforms;
}

String CanonicalizeGlobalUniformAliases(String source) {
    auto              view = source.as_str();
    shader_lex::Lexer lexer(view);
    String            out;
    usize             copied {};
    for (auto token = lexer.Next(); token.kind != shader_lex::TokenKind::Eof;
         token      = lexer.Next()) {
        if (token.kind != shader_lex::TokenKind::Ident) continue;
        auto field = FindGlobalUniform(token.text);
        if (field.is_none() || (**field).alias.is_empty() || token.text != (**field).alias)
            continue;
        out.push_str(view.get(copied, token.offset).unwrap());
        out.push_str((**field).name);
        copied = token.offset + token.text.len();
    }
    if (copied == usize()) return source;
    out.push_str(view.get(copied, view.len()).unwrap());
    return out;
}

HashSet<String> ReferencedUniformNames(slice<ShaderUnit> units) {
    HashSet<String> names;
    for (const auto& unit : units) {
        auto              body = StripUniforms(unit.src.as_str());
        shader_lex::Lexer lexer(body);
        for (auto token = lexer.Next(); token.kind != shader_lex::TokenKind::Eof;
             token      = lexer.Next()) {
            if (token.kind == shader_lex::TokenKind::Ident) {
                (void)names.insert(rstd::into(token.text));
            }
        }
    }
    return names;
}

struct UniformCompileInterface {
    bool                        legacy { false };
    Vec<GlobalUniformBlockKind> global_blocks;
    BTreeMap<String, String>    local;
};

UniformCompileInterface BuildUniformInterface(mut_ref<ShaderUnit[]> units) {
    UniformCompileInterface result;
    for (const auto& unit : units) {
        for (const auto& [name, type] : unit.preprocess_info.uniforms.iter()) {
            auto field = FindGlobalUniform(name->as_str());
            if (field.is_none()) continue;
            const auto expected = LayoutUniform((**field).type);
            const auto authored = LayoutUniform(type->as_str());
            if (expected.hlsl_ty != authored.hlsl_ty || expected.array != authored.array) {
                rstd_warn("uniform {} type {} does not match canonical type {}; using legacy ABI",
                          name->as_str(),
                          type->as_str(),
                          (**field).type);
                result.legacy = true;
            }
        }
    }
    if (result.legacy) {
        result.local = BuildUniformUnion(units.as_ref());
        return result;
    }

    for (auto& unit : units) {
        unit.src = CanonicalizeGlobalUniformAliases(rstd::move(unit.src));
        BTreeMap<String, String> canonical;
        for (const auto& [name, type] : unit.preprocess_info.uniforms.iter()) {
            auto field = FindGlobalUniform(name->as_str());
            if (field.is_none()) {
                MergeUniform(canonical, name->as_str(), type->as_str());
                continue;
            }
            MergeUniform(canonical, (**field).name, (**field).type);
        }
        unit.preprocess_info.uniforms = rstd::move(canonical);
    }

    const auto referenced = ReferencedUniformNames(units.as_ref());
    for (const auto& block : GlobalUniformBlocks()) {
        bool active = false;
        for (const auto& field : GlobalUniformFields()) {
            if (GlobalUniformBlockFor(field.producer) != block.kind) continue;
            if (referenced.contains(field.name)) {
                active = true;
                break;
            }
        }
        if (active) result.global_blocks.push(GlobalUniformBlockKind(block.kind));
    }
    for (const auto& unit : units) {
        for (const auto& [name, type] : unit.preprocess_info.uniforms.iter()) {
            if (FindGlobalUniform(name->as_str()).is_some() ||
                ! referenced.contains(name->as_str())) {
                continue;
            }
            MergeUniform(result.local, name->as_str(), type->as_str());
        }
    }
    return result;
}

usize LinearUniformElementCount(ref<str> ty) {
    const auto [base_ty, array] = SplitUniformType(ty);
    if (! array.is_empty()) return usize();

    const auto hlsl_ty = ToHLSLType(base_ty);
    if (hlsl_ty == "float"_str || hlsl_ty == "int"_str || hlsl_ty == "uint"_str ||
        hlsl_ty == "bool"_str) {
        return usize(1);
    }
    for (ref<str> prefix : { "float"_str, "int"_str, "uint"_str, "bool"_str }) {
        if (! hlsl_ty.starts_with(prefix) || hlsl_ty.size() != prefix.len() + usize(1)) continue;
        const char width =
            static_cast<char>(hlsl_ty.as_bytes().last().unwrap().get().to_primitive());
        if (width >= '2' && width <= '4') return usize(static_cast<rstd::size_t>(width - '0'));
    }
    return usize();
}

void ShapeShaderValues(ShaderValues& values, const BTreeMap<String, String>& uniforms) {
    for (auto [name, value] : values.iter_mut()) {
        if (value->size() != usize(1)) continue;
        auto uniform = uniforms.get(name->as_str());
        if (uniform.is_none()) continue;
        const auto elements = LinearUniformElementCount((**uniform).as_str());
        if (elements <= usize(1) || elements > usize(4)) continue;

        auto shaped = array<float, 4>::repeat((*value)[usize()]);
        *value      = ShaderValue(shaped.data(), elements);
    }
}

void ShapeShaderDefaults(slice<ShaderUnit> units, ShaderInfo& info) {
    const auto uniforms = BuildUniformUnion(units);
    ShapeShaderValues(info.svs, uniforms);
    ShapeShaderValues(info.baseConstSvs, uniforms);
}

// Emit a cbuffer with explicit std140 `:packoffset` per member.
// glslang's HLSL frontend hard-codes HLSL cbuffer packing on
// HLSL sources (see ShaderLang.cpp `setHlslOffsets` when EShSourceHlsl);
// without `packoffset`, scalars get packed into the trailing padding of
// vec3 / vec3[] members. Explicit offsets keep the shared block layout
// identical across stages and leave physical padding to reflected serialization.
inline String EmitCBufferStd140(const BTreeMap<String, String>& uniforms_union, ref<str> block_name,
                                u32 set, u32 binding) {
    String out;
    out.push_str(rstd::format("[[vk::binding({}, {})]] cbuffer {} {{\n",
                              binding.to_primitive(),
                              set.to_primitive(),
                              block_name));
    rstd::size_t offset = 0;
    for (const auto& [name, ty] : uniforms_union.iter()) {
        const auto layout   = LayoutUniform(ty->as_str());
        ref<str>   array    = layout.array;
        offset              = (offset + layout.align - 1) & ~(layout.align - 1);
        rstd::size_t reg    = offset / 16;
        rstd::size_t comp   = (offset % 16) / 4;
        const char   letter = "xyzw"[comp];
        const bool   is_matrix =
            layout.hlsl_ty == "float2x2"_str || layout.hlsl_ty == "float3x3"_str ||
            layout.hlsl_ty == "float4x4"_str || layout.hlsl_ty == "float2x3"_str ||
            layout.hlsl_ty == "float2x4"_str || layout.hlsl_ty == "float3x2"_str ||
            layout.hlsl_ty == "float3x4"_str || layout.hlsl_ty == "float4x2"_str ||
            layout.hlsl_ty == "float4x3"_str;
        out.push_str("    "_str);
        if (is_matrix) out.push_str("column_major "_str);
        out.push_str(rstd::format("{} {}{}", layout.hlsl_ty, name->as_str(), array));
        out.push_str(rstd::format(" : packoffset(c{}", reg));
        if (comp != 0) {
            out.push_str("."_str);
            out.push_ascii(letter);
        }
        out.push_str(");\n"_str);
        offset += layout.size;
    }
    out.push_str("};\n"_str);
    return out;
}

inline String EmitGlobalCBufferStd140(const GlobalUniformBlockSchema& block) {
    String out;
    out.push_str(rstd::format("[[vk::binding({}, {})]] cbuffer {} {{\n",
                              block.binding.to_primitive(),
                              kGlobalUniformSet.to_primitive(),
                              block.name));
    for (const auto& field : GlobalUniformFields()) {
        if (GlobalUniformBlockFor(field.producer) != block.kind) continue;
        const auto layout = LayoutUniform(field.type);
        const auto offset = field.offset.to_primitive();
        const auto reg    = offset / 16;
        const auto comp   = (offset % 16) / 4;
        out.push_str(rstd::format(
            "    {} {}{} : packoffset(c{}", layout.hlsl_ty, field.name, layout.array, reg));
        if (comp != 0)
            out.push_str(rstd::format(".{}", *"xyzw"_str.get(usize(comp), usize(comp + 1))));
        out.push_str(");\n"_str);
    }
    out.push_str("};\n"_str);
    return out;
}

inline String EmitGlobalCBuffersStd140(const UniformCompileInterface& interface) {
    String out;
    for (const auto kind : interface.global_blocks) {
        auto block = FindGlobalUniformBlock(kind);
        if (block.is_some()) out.push_str(EmitGlobalCBufferStd140(**block));
    }
    return out;
}

inline String Finalprocessor(const ShaderUnit& unit, const PreprocessorInfo* pre,
                             const PreprocessorInfo* next, const UniformCompileInterface* interface,
                             bool pack_varying_arrays) {
    // GS: feed glslang's HLSL frontend. Strip GLSL-style top-level `in`/`out`
    // decls, emit HLSL structs (WW_VSOut/WW_PSIn) + ww_Uniforms cbuffer, and
    // rewrite `void main()` to the entry signature `point WW_VSOut IN[1],
    // inout TriangleStream<WW_PSIn> OUT`.
    if (unit.stage == ShaderType::GEOMETRY) {
        auto [io_decls, stripped] = ScanAndStripIO(unit.src.as_str());
        String body               = QualifyGlobalVariablesForHlsl(StripUniforms(stripped));

        Vec<IODecl> in_decls, out_decls;
        auto        add_to = [](Vec<IODecl>&     v,
                                const IODecl&    d,
                                IODeclPrecedence precedence = IODeclPrecedence::KeepExisting) {
            AddIODecl(v, d, precedence);
        };
        auto add_in = [&](const IODecl& d) {
            add_to(in_decls, d);
        };
        auto add_out = [&](const IODecl& d) {
            add_to(out_decls, d);
        };
        for (const auto& d : io_decls) {
            if (d.storage == 'i')
                add_in(d);
            else if (d.storage == 'o')
                add_out(d);
        }
        if (pre)
            for (const auto& [k, v] : pre->output.iter()) {
                if (auto d = ParseIODecl(v->as_str()); d) {
                    add_to(in_decls, *d, IODeclPrecedence::PreferIncoming);
                }
            }
        if (next)
            for (const auto& [k, v] : next->input.iter()) {
                if (auto d = ParseIODecl(v->as_str()); d) add_out(*d);
            }

        String synth;
        synth.push_str("\n// === auto-generated GS stage I/O (HLSL) ===\n"_str);
        synth.push_str(EmitGSHLSLStruct("WW_VSOut"_str, rstd::move(in_decls)));
        synth.push_str(EmitGSHLSLStruct("WW_PSIn"_str, rstd::move(out_decls)));

        // Legacy callers still synthesize one cross-stage uniform block.
        BTreeMap<String, String> uniforms_union_local;
        if (! interface) {
            auto absorb = [&](const BTreeMap<String, String>& m) {
                for (const auto& [k, v] : m.iter())
                    MergeUniform(uniforms_union_local, k->as_str(), v->as_str());
            };
            absorb(unit.preprocess_info.uniforms);
            if (pre) absorb(pre->uniforms);
            if (next) absorb(next->uniforms);
        }
        const BTreeMap<String, String>& uniforms_union =
            interface ? interface->local : uniforms_union_local;
        if (interface && ! interface->legacy) synth.push_str(EmitGlobalCBuffersStd140(*interface));
        if (! uniforms_union.is_empty()) {
            synth.push_str(
                "\n// === auto-generated draw uniforms (HLSL, std140 via packoffset) ===\n"_str);
            synth.push_str(EmitCBufferStd140(
                uniforms_union,
                interface && ! interface->legacy ? kDrawUniformBlockName : "ww_Uniforms"_str,
                kDrawUniformSet,
                u32(0)));
        }

        body = RewriteGSMain(rstd::move(body));
        return ReplaceAll(rstd::move(body), SHADER_PLACEHOLD, synth);
    }

    // Strip `attribute/varying` lines and collect them as structured decls.
    auto [io_decls, stage1] = ScanAndStripIO(unit.src.as_str());

    // Strip sampler declarations; they are re-emitted with explicit bindings.
    auto [sampler_decls, stage2] = ScanAndStripSamplers(stage1);

    // Strip non-sampler declarations; they are re-emitted through the selected ABI.
    String stage3 = QualifyGlobalVariablesForHlsl(StripUniforms(stage2));

    // Partition IO decls into VS inputs and varyings
    // (everything else). The producing stage owns each cross-stage interface
    // type; consumers only contribute names missing from that interface.
    Vec<IODecl> attrs, varyings;
    auto add = [&](const IODecl& d, IODeclPrecedence precedence = IODeclPrecedence::KeepExisting) {
        const bool vertex_input =
            unit.stage == ShaderType::VERTEX && (d.storage == 'a' || d.storage == 'i');
        Vec<IODecl>& v = vertex_input ? attrs : varyings;
        AddIODecl(v, d, precedence);
    };
    for (const auto& d : io_decls) add(d);
    auto add_varying_from_line = [&](ref<str> line, IODeclPrecedence precedence) {
        if (auto d = ParseIODecl(line); d) AddIODecl(varyings, *d, precedence);
    };
    if (unit.stage == ShaderType::VERTEX && next) {
        for (const auto& [k, v] : next->input.iter()) {
            add_varying_from_line(v->as_str(), IODeclPrecedence::KeepExisting);
        }
    } else if (unit.stage == ShaderType::FRAGMENT && pre) {
        for (const auto& [k, v] : pre->output.iter()) {
            add_varying_from_line(v->as_str(), IODeclPrecedence::PreferIncoming);
        }
    }

    // Synthesize the HLSL entry point: static globals for every attr /
    // varying, WW_VSIn/WW_VSOut/WW_PSIn structs, and a main_vs / main_ps
    // wrapper that copies between the struct and the statics.
    SynthOutput synth = SynthesizeHLSLEntry(
        unit.stage, rstd::move(attrs), rstd::move(varyings), pack_varying_arrays);

    // Legacy callers still synthesize one cross-stage uniform block.
    BTreeMap<String, String> uniforms_union_local;
    if (! interface) {
        auto absorb = [&](const BTreeMap<String, String>& m) {
            for (const auto& [k, v] : m.iter())
                MergeUniform(uniforms_union_local, k->as_str(), v->as_str());
        };
        absorb(unit.preprocess_info.uniforms);
        if (pre) absorb(pre->uniforms);
        if (next) absorb(next->uniforms);
    }
    const BTreeMap<String, String>& uniforms_union =
        interface ? interface->local : uniforms_union_local;

    String uniform_block;
    if (interface && ! interface->legacy)
        uniform_block.push_str(EmitGlobalCBuffersStd140(*interface));
    if (! uniforms_union.is_empty()) {
        uniform_block.push_str(
            "\n// === auto-generated draw uniforms (HLSL, std140 via packoffset) ===\n"_str);
        uniform_block.push_str(EmitCBufferStd140(
            uniforms_union,
            interface && ! interface->legacy ? kDrawUniformBlockName : "ww_Uniforms"_str,
            kDrawUniformSet,
            u32(0)));
    }

    // Binding 0 stays reserved for the draw uniform block. `vk::combinedImageSampler`
    // joins each texture and sampler pair into one Vulkan descriptor.
    HashSet<String> sampler_seen;
    String          sampler_block;
    if (! sampler_decls.is_empty())
        sampler_block.push_str("\n// === auto-generated samplers (HLSL) ===\n"_str);
    rstd::size_t sampler_idx = 1;
    for (const auto& s : sampler_decls) {
        if (! sampler_seen.insert(String::make(s.name))) continue;
        ref<str> tex_ty   = HLSLSamplerType(s.sampler_type);
        ref<str> state_ty = HLSLSamplerStateType(s.sampler_type);
        sampler_block.push_str(
            rstd::format("[[vk::combinedImageSampler]][[vk::binding({}, {})]] {} {};\n",
                         sampler_idx,
                         kDrawUniformSet.to_primitive(),
                         tex_ty,
                         s.name));
        sampler_block.push_str(
            rstd::format("[[vk::combinedImageSampler]][[vk::binding({}, {})]] {} {}_ww_sampler;\n",
                         sampler_idx,
                         kDrawUniformSet.to_primitive(),
                         state_ty,
                         s.name));
        ++sampler_idx;
    }

    // Splice synth.pre into the placeholder slot, then append synth.post
    // (which contains the entry-point wrapper that has to follow the user's
    // shader_main()).
    String with_decls = ReplaceAll(
        stage3, SHADER_PLACEHOLD, rstd::format("{}{}{}", synth.pre, uniform_block, sampler_block));
    return rstd::format("{}{}", with_decls, synth.post);
}

using ShaderCacheDigest = array<rstd::uint8_t, 20>;

constexpr array<rstd::uint8_t, 8> kShaderCacheMagic { rstd::uint8_t('O'), rstd::uint8_t('W'),
                                                      rstd::uint8_t('E'), rstd::uint8_t('S'),
                                                      rstd::uint8_t('P'), rstd::uint8_t('V'),
                                                      rstd::uint8_t('3'), rstd::uint8_t(0) };
constexpr rstd::uint32_t          kShaderCacheFormatVersion = 3;
constexpr rstd::uint32_t          kShaderCacheAbiVersion    = 19;
// 8-byte magic, six u32 fields, and four SHA-1 digests total 112 bytes.
constexpr rstd::uint32_t kShaderCacheHeaderSize = static_cast<rstd::uint32_t>(
    kShaderCacheMagic.len().to_primitive() + 6 * sizeof(rstd::uint32_t) +
    4 * ShaderCacheDigest {}.len().to_primitive());
constexpr rstd::uint32_t kMaxShaderCacheStages      = 16;
constexpr rstd::uint32_t kMaxShaderCacheMapEntries  = 4096;
constexpr rstd::uint32_t kMaxShaderCacheSlots       = 1024;
constexpr rstd::uint32_t kMaxShaderCacheStringSize  = 32 * 1024 * 1024;
constexpr rstd::uint32_t kMaxShaderCachePayloadSize = 256 * 1024 * 1024;

class ShaderCacheByteWriter {
public:
    void U32(rstd::uint32_t value) {
        m_bytes.push(static_cast<rstd::uint8_t>(value));
        m_bytes.push(static_cast<rstd::uint8_t>(value >> 8));
        m_bytes.push(static_cast<rstd::uint8_t>(value >> 16));
        m_bytes.push(static_cast<rstd::uint8_t>(value >> 24));
    }

    bool Bytes(slice<rstd::uint8_t> value) {
        if (m_bytes.len().to_primitive() > rstd::u32::MAX.to_primitive() ||
            value.len().to_primitive() >
                rstd::u32::MAX.to_primitive() - m_bytes.len().to_primitive()) {
            return false;
        }
        for (auto byte : value) m_bytes.push(rstd::uint8_t(byte));
        return true;
    }

    bool String(ref<str> value) {
        if (value.len() > usize(kMaxShaderCacheStringSize)) return false;
        U32(static_cast<rstd::uint32_t>(value.len().to_primitive()));
        return Bytes(slice<rstd::uint8_t>::from_raw_parts(
            reinterpret_cast<const rstd::uint8_t*>(value.data()), value.len()));
    }

    const Vec<rstd::uint8_t>& bytes() const noexcept { return m_bytes; }
    Vec<rstd::uint8_t>        Take() noexcept { return rstd::move(m_bytes); }

private:
    Vec<rstd::uint8_t> m_bytes;
};

class ShaderCacheByteReader {
public:
    explicit ShaderCacheByteReader(slice<rstd::uint8_t> bytes): m_bytes(bytes) {}

    bool U32(rstd::uint32_t& value) {
        slice<rstd::uint8_t> bytes;
        if (! Bytes(4, bytes)) return false;
        value = static_cast<rstd::uint32_t>(bytes[usize(0)]) |
                (static_cast<rstd::uint32_t>(bytes[usize(1)]) << 8) |
                (static_cast<rstd::uint32_t>(bytes[usize(2)]) << 16) |
                (static_cast<rstd::uint32_t>(bytes[usize(3)]) << 24);
        return true;
    }

    bool Bytes(rstd::size_t size, slice<rstd::uint8_t>& value) {
        if (size > remaining()) return false;
        value =
            slice<rstd::uint8_t>::from_raw_parts(m_bytes.as_raw_ptr() + m_position, usize(size));
        m_position += size;
        return true;
    }

    bool String(::alloc::string::String& value) {
        rstd::uint32_t size {};
        if (! U32(size) || size > kMaxShaderCacheStringSize) return false;
        slice<rstd::uint8_t> bytes;
        if (! Bytes(size, bytes)) return false;
        auto text = rstd::str_::from_utf8(slice<u8>::from_raw_parts(
            reinterpret_cast<const byte*>(bytes.as_raw_ptr()), bytes.len()));
        if (text.is_err()) return false;
        value = rstd::into(text.unwrap());
        return true;
    }

    rstd::size_t remaining() const noexcept { return m_bytes.len().to_primitive() - m_position; }
    bool         done() const noexcept { return m_position == m_bytes.len().to_primitive(); }

private:
    slice<rstd::uint8_t> m_bytes;
    rstd::size_t         m_position { 0 };
};

int HexDigit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

Option<ShaderCacheDigest> DecodeShaderCacheDigest(ref<str> value) {
    if (value.len() != usize(40)) return None();
    ShaderCacheDigest digest {};
    for (rstd::size_t i = 0; i < digest.len().to_primitive(); ++i) {
        const int hi = HexDigit(static_cast<char>(value.as_bytes()[usize(i * 2)].to_primitive()));
        const int lo =
            HexDigit(static_cast<char>(value.as_bytes()[usize(i * 2 + 1)].to_primitive()));
        if (hi < 0 || lo < 0) return None();
        digest[usize(i)] = static_cast<rstd::uint8_t>((hi << 4) | lo);
    }
    return Some(digest);
}

Option<ShaderCacheDigest> HashShaderCacheBytes(slice<rstd::uint8_t> bytes) {
    auto digest = utils::genSha1(slice<rstd::byte>::from_raw_parts(
        reinterpret_cast<const rstd::byte*>(bytes.as_raw_ptr()), bytes.len()));
    return DecodeShaderCacheDigest(digest.as_str());
}

struct ShaderCacheIdentity {
    ShaderCacheDigest cache_key;
    ShaderCacheDigest source;
    ShaderCacheDigest combos;
    String            cache_key_hex;
};

Option<ShaderCacheIdentity> MakeShaderCacheIdentity(slice<ShaderUnit> units, const Combos& combos) {
    if (units.len().to_primitive() > kMaxShaderCacheStages ||
        combos.len().to_primitive() > kMaxShaderCacheMapEntries) {
        return None();
    }

    ShaderCacheByteWriter source;
    source.U32(static_cast<rstd::uint32_t>(units.len().to_primitive()));
    for (const auto& unit : units) {
        source.U32(static_cast<rstd::uint32_t>(unit.stage));
        if (! source.String(unit.src.as_str())) return None();
    }

    ShaderCacheByteWriter combo;
    combo.U32(static_cast<rstd::uint32_t>(combos.len().to_primitive()));
    for (const auto& [name, value] : combos.iter()) {
        if (! combo.String(name->as_str()) || ! combo.String(value->as_str())) return None();
    }

    auto source_digest = HashShaderCacheBytes(slice<rstd::uint8_t>::from_raw_parts(
        source.bytes().data(), usize(source.bytes().len().to_primitive())));
    auto combo_digest  = HashShaderCacheBytes(slice<rstd::uint8_t>::from_raw_parts(
        combo.bytes().data(), usize(combo.bytes().len().to_primitive())));
    if (source_digest.is_none() || combo_digest.is_none()) return None();

    ShaderCacheByteWriter key;
    key.String("owe.shader-cache.v3"_str);
    key.U32(kShaderCacheAbiVersion);
    key.Bytes(slice<rstd::uint8_t>::from_raw_parts(source_digest->data(),
                                                   usize(source_digest->len().to_primitive())));
    key.Bytes(slice<rstd::uint8_t>::from_raw_parts(combo_digest->data(),
                                                   usize(combo_digest->len().to_primitive())));
    key.String("vulkan-1.1"_str);
    key.String("hlsl"_str);
    key.U32(0);

    auto key_hex    = utils::genSha1(slice<rstd::byte>::from_raw_parts(
        reinterpret_cast<const rstd::byte*>(key.bytes().data()), key.bytes().len()));
    auto key_digest = DecodeShaderCacheDigest(key_hex.as_str());
    if (key_digest.is_none()) return None();
    return Some(ShaderCacheIdentity {
        .cache_key     = *key_digest,
        .source        = *source_digest,
        .combos        = *combo_digest,
        .cache_key_hex = rstd::move(key_hex),
    });
}

inline rstd::path::PathBuf GetCachePath(ref<rstd::path::Path> cache_dir, ref<str> scene_id,
                                        ref<str> filename) {
    auto path = rstd::path::PathBuf::from(cache_dir);
    path.push(ref<rstd::path::Path>(scene_id));
    path.push(ref<rstd::path::Path>(SHADER_DIR ""_str));
    const auto cache_filename = rstd::format("{}.{}", filename, SHADER_SUFFIX);
    path.push(ref<rstd::path::Path>(cache_filename.as_str()));
    return path;
}

bool WriteCacheMap(ShaderCacheByteWriter& writer, const BTreeMap<String, String>& values) {
    if (values.len() > usize(kMaxShaderCacheMapEntries)) return false;
    writer.U32(static_cast<rstd::uint32_t>(values.len().to_primitive()));
    for (const auto& [name, value] : values.iter()) {
        if (! writer.String(name->as_str()) || ! writer.String(value->as_str())) return false;
    }
    return true;
}

bool ReadCacheMap(ShaderCacheByteReader& reader, BTreeMap<String, String>& values) {
    rstd::uint32_t count = 0;
    if (! reader.U32(count) || count > kMaxShaderCacheMapEntries) return false;
    values.clear();
    for (rstd::uint32_t i = 0; i < count; ++i) {
        String name;
        String value;
        if (! reader.String(name) || ! reader.String(value) || values.contains_key(name.as_str()))
            return false;
        (void)values.insert(rstd::move(name), rstd::move(value));
    }
    return true;
}

bool WriteCacheSlots(ShaderCacheByteWriter& writer, const BTreeSet<u32>& slots) {
    if (slots.len().to_primitive() > kMaxShaderCacheSlots) return false;
    writer.U32(static_cast<rstd::uint32_t>(slots.len().to_primitive()));
    for (const auto slot : slots.iter()) writer.U32(slot->to_primitive());
    return true;
}

bool ReadCacheSlots(ShaderCacheByteReader& reader, BTreeSet<u32>& slots) {
    rstd::uint32_t count = 0;
    if (! reader.U32(count) || count > kMaxShaderCacheSlots) return false;
    slots.clear();
    for (rstd::uint32_t i = 0; i < count; ++i) {
        rstd::uint32_t slot = 0;
        if (! reader.U32(slot) || ! slots.insert(u32(slot))) return false;
    }
    return true;
}

struct ShaderCacheArtifact {
    Vec<ShaderUnit> units;
    Vec<ShaderCode> codes;
};

enum class ShaderCacheReadStatus
{
    Hit,
    Miss,
    Invalid,
};

struct ShaderCacheReadResult {
    ShaderCacheReadStatus status { ShaderCacheReadStatus::Invalid };
    ShaderCacheArtifact   artifact;
    String                reason;
};

class ShaderCacheArtifactCodec {
public:
    static Option<Vec<rstd::uint8_t>> Encode(const ShaderCacheIdentity& identity,
                                             slice<ShaderUnit> units, slice<ShaderCode> codes) {
        if (units.is_empty() || units.len().to_primitive() != codes.len().to_primitive() ||
            units.len().to_primitive() > kMaxShaderCacheStages) {
            return None();
        }

        ShaderCacheByteWriter payload;
        for (rstd::size_t i = 0; i < units.len().to_primitive(); ++i) {
            if (static_cast<rstd::uint32_t>(units[usize(i)].stage) >
                static_cast<rstd::uint32_t>(ShaderType::FRAGMENT)) {
                return None();
            }
            ShaderCacheByteWriter record;
            record.U32(static_cast<rstd::uint32_t>(units[usize(i)].stage));
            if (! record.String(units[usize(i)].src.as_str()) ||
                ! WriteCacheMap(record, units[usize(i)].preprocess_info.input) ||
                ! WriteCacheMap(record, units[usize(i)].preprocess_info.output) ||
                ! WriteCacheMap(record, units[usize(i)].preprocess_info.uniforms) ||
                ! WriteCacheSlots(record, units[usize(i)].preprocess_info.active_tex_slots) ||
                codes[usize(i)].len().to_primitive() > rstd::u32::MAX.to_primitive() / 4) {
                return None();
            }

            record.U32(static_cast<rstd::uint32_t>(codes[usize(i)].len().to_primitive() * 4));
            for (const auto word : codes[usize(i)]) record.U32(word);
            if (record.bytes().len().to_primitive() >
                    kMaxShaderCachePayloadSize - sizeof(rstd::uint32_t) ||
                payload.bytes().len().to_primitive() > kMaxShaderCachePayloadSize -
                                                           sizeof(rstd::uint32_t) -
                                                           record.bytes().len().to_primitive()) {
                return None();
            }
            payload.U32(static_cast<rstd::uint32_t>(record.bytes().len().to_primitive()));
            if (! payload.Bytes(slice<rstd::uint8_t>::from_raw_parts(
                    record.bytes().data(), usize(record.bytes().len().to_primitive())))) {
                return None();
            }
        }
        if (payload.bytes().len().to_primitive() > kMaxShaderCachePayloadSize) return None();

        auto payload_digest = HashShaderCacheBytes(slice<rstd::uint8_t>::from_raw_parts(
            payload.bytes().data(), usize(payload.bytes().len().to_primitive())));
        if (payload_digest.is_none()) return None();

        ShaderCacheByteWriter artifact;
        artifact.Bytes(slice<rstd::uint8_t>::from_raw_parts(
            kShaderCacheMagic.data(), usize(kShaderCacheMagic.len().to_primitive())));
        artifact.U32(kShaderCacheFormatVersion);
        artifact.U32(kShaderCacheAbiVersion);
        artifact.U32(kShaderCacheHeaderSize);
        artifact.U32(static_cast<rstd::uint32_t>(payload.bytes().len().to_primitive()));
        artifact.U32(static_cast<rstd::uint32_t>(units.len().to_primitive()));
        artifact.U32(0);
        artifact.Bytes(slice<rstd::uint8_t>::from_raw_parts(
            identity.cache_key.data(), usize(identity.cache_key.len().to_primitive())));
        artifact.Bytes(slice<rstd::uint8_t>::from_raw_parts(
            identity.source.data(), usize(identity.source.len().to_primitive())));
        artifact.Bytes(slice<rstd::uint8_t>::from_raw_parts(
            identity.combos.data(), usize(identity.combos.len().to_primitive())));
        artifact.Bytes(slice<rstd::uint8_t>::from_raw_parts(
            payload_digest->data(), usize(payload_digest->len().to_primitive())));
        if (artifact.bytes().len().to_primitive() != kShaderCacheHeaderSize ||
            ! artifact.Bytes(slice<rstd::uint8_t>::from_raw_parts(
                payload.bytes().data(), usize(payload.bytes().len().to_primitive())))) {
            return None();
        }
        return Some(artifact.Take());
    }

    static ShaderCacheReadResult Decode(const ShaderCacheIdentity& identity,
                                        slice<ShaderUnit>          expected_units,
                                        slice<rstd::uint8_t>       bytes) {
        if (bytes.len().to_primitive() < kShaderCacheHeaderSize)
            return Invalid("truncated header"_str);
        if (bytes.len().to_primitive() > kShaderCacheHeaderSize + kMaxShaderCachePayloadSize) {
            return Invalid("artifact exceeds size limit"_str);
        }

        ShaderCacheByteReader reader(bytes);
        slice<rstd::uint8_t>  magic;
        rstd::uint32_t        format_version = 0;
        rstd::uint32_t        shader_abi     = 0;
        rstd::uint32_t        header_size    = 0;
        rstd::uint32_t        payload_size   = 0;
        rstd::uint32_t        stage_count    = 0;
        rstd::uint32_t        flags          = 0;
        if (! reader.Bytes(kShaderCacheMagic.len().to_primitive(), magic) ||
            magic != kShaderCacheMagic.as_slice() || ! reader.U32(format_version) ||
            ! reader.U32(shader_abi) || ! reader.U32(header_size) || ! reader.U32(payload_size) ||
            ! reader.U32(stage_count) || ! reader.U32(flags)) {
            return Invalid("invalid header"_str);
        }
        if (format_version != kShaderCacheFormatVersion || shader_abi != kShaderCacheAbiVersion ||
            header_size != kShaderCacheHeaderSize || flags != 0) {
            return Invalid("unsupported format or ABI"_str);
        }
        if (stage_count == 0 || stage_count > kMaxShaderCacheStages ||
            stage_count != expected_units.len().to_primitive()) {
            return Invalid("stage count mismatch"_str);
        }

        ShaderCacheDigest cache_key {};
        ShaderCacheDigest source {};
        ShaderCacheDigest combos {};
        ShaderCacheDigest payload_digest {};
        if (! ReadDigest(reader, cache_key) || ! ReadDigest(reader, source) ||
            ! ReadDigest(reader, combos) || ! ReadDigest(reader, payload_digest)) {
            return Invalid("truncated identity"_str);
        }
        if (cache_key != identity.cache_key || source != identity.source ||
            combos != identity.combos) {
            return Invalid("identity mismatch"_str);
        }
        if (payload_size > kMaxShaderCachePayloadSize || payload_size != reader.remaining()) {
            return Invalid("payload size mismatch"_str);
        }

        slice<rstd::uint8_t> payload;
        if (! reader.Bytes(payload_size, payload) || ! reader.done()) {
            return Invalid("truncated payload"_str);
        }
        auto actual_payload_digest = HashShaderCacheBytes(payload);
        if (! actual_payload_digest || *actual_payload_digest != payload_digest) {
            return Invalid("payload digest mismatch"_str);
        }

        ShaderCacheArtifact artifact;
        artifact.units.reserve(usize(stage_count));
        artifact.codes.reserve(usize(stage_count));
        ShaderCacheByteReader payload_reader(payload);
        for (rstd::uint32_t i = 0; i < stage_count; ++i) {
            rstd::uint32_t       record_size = 0;
            slice<rstd::uint8_t> record_bytes;
            if (! payload_reader.U32(record_size) || record_size > payload_reader.remaining() ||
                ! payload_reader.Bytes(record_size, record_bytes)) {
                return Invalid("invalid stage record size"_str);
            }

            ShaderCacheByteReader record(record_bytes);
            rstd::uint32_t        stage_value = 0;
            if (! record.U32(stage_value) ||
                stage_value > static_cast<rstd::uint32_t>(ShaderType::FRAGMENT)) {
                return Invalid("invalid shader stage"_str);
            }
            const auto stage = static_cast<ShaderType>(stage_value);
            if (stage != expected_units[usize(i)].stage)
                return Invalid("shader stage mismatch"_str);

            ShaderUnit unit { .stage = stage };
            if (! record.String(unit.src) || ! ReadCacheMap(record, unit.preprocess_info.input) ||
                ! ReadCacheMap(record, unit.preprocess_info.output) ||
                ! ReadCacheMap(record, unit.preprocess_info.uniforms) ||
                ! ReadCacheSlots(record, unit.preprocess_info.active_tex_slots)) {
                return Invalid("invalid shader metadata"_str);
            }

            rstd::uint32_t spirv_size = 0;
            if (! record.U32(spirv_size) || spirv_size % 4 != 0 ||
                spirv_size > record.remaining()) {
                return Invalid("invalid SPIR-V size"_str);
            }
            ShaderCode code;
            code.reserve(usize(spirv_size / 4));
            for (rstd::uint32_t word = 0; word < spirv_size / 4; ++word) {
                rstd::uint32_t value = 0;
                if (! record.U32(value)) return Invalid("truncated SPIR-V"_str);
                code.push(rstd::uint32_t(value));
            }
            if (! record.done()) return Invalid("unexpected stage data"_str);
            artifact.units.push(rstd::move(unit));
            artifact.codes.push(rstd::move(code));
        }
        if (! payload_reader.done()) return Invalid("unexpected payload data"_str);
        return ShaderCacheReadResult {
            .status   = ShaderCacheReadStatus::Hit,
            .artifact = rstd::move(artifact),
        };
    }

private:
    static bool ReadDigest(ShaderCacheByteReader& reader, ShaderCacheDigest& digest) {
        slice<rstd::uint8_t> bytes;
        if (! reader.Bytes(digest.len().to_primitive(), bytes)) return false;
        for (usize i {}; i < bytes.len(); ++i) digest[i] = bytes[i];
        return true;
    }

    static ShaderCacheReadResult Invalid(ref<str> reason) {
        return ShaderCacheReadResult {
            .status = ShaderCacheReadStatus::Invalid,
            .reason = String::make(reason),
        };
    }
};

bool PublishShaderCacheArtifact(ref<rstd::path::Path> path, ref<str> cache_key,
                                slice<rstd::uint8_t> bytes) {
    auto parent = path.parent();
    if (parent.is_none()) return false;

    auto created = rstd::fs::create_dir_all(*parent);
    if (created.is_err()) {
        rstd_warn("cannot create shader cache directory '{}': {}",
                  *parent,
                  rstd::move(created).unwrap_err_unchecked());
        return false;
    }

    static Atomic<u64> temporary_sequence { u64() };
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        const auto temporary_name =
            rstd::format("{}.{}.{}.tmp",
                         cache_key,
                         rstd::process::id(),
                         temporary_sequence.fetch_add(u64(1), Ordering::Relaxed));
        auto temporary_path = rstd::path::PathBuf::from(*parent);
        temporary_path.push(ref<rstd::path::Path>(temporary_name.as_str()));

        auto opened = rstd::fs::File::create_new(temporary_path.as_path());
        if (opened.is_err()) {
            auto error = rstd::move(opened).unwrap_err_unchecked();
            if (error.kind().code == rstd::io::error::ErrorKind::AlreadyExists) continue;
            rstd_warn("cannot create shader cache temporary file '{}': {}",
                      temporary_path.as_path(),
                      error);
            return false;
        }

        bool write_ok = false;
        {
            auto file = rstd::move(opened).unwrap_unchecked();
            auto data =
                rstd::slice<u8>::from_raw_parts(reinterpret_cast<const byte*>(bytes.as_raw_ptr()),
                                                rstd::usize(bytes.len().to_primitive()));
            auto written = file.write_all(data);
            if (written.is_err()) {
                rstd_warn("cannot write shader cache temporary file '{}': {}",
                          temporary_path.as_path(),
                          rstd::move(written).unwrap_err_unchecked());
            } else {
                auto flushed = file.flush();
                if (flushed.is_err()) {
                    rstd_warn("cannot flush shader cache temporary file '{}': {}",
                              temporary_path.as_path(),
                              rstd::move(flushed).unwrap_err_unchecked());
                } else {
                    write_ok = true;
                }
            }
        }

        if (! write_ok) {
            static_cast<void>(rstd::fs::remove_file(temporary_path.as_path()));
            return false;
        }

        auto published = rstd::fs::rename(temporary_path.as_path(), path);
        if (published.is_ok()) return true;
        rstd_warn("cannot publish shader cache '{}': {}",
                  path,
                  rstd::move(published).unwrap_err_unchecked());
        static_cast<void>(rstd::fs::remove_file(temporary_path.as_path()));
        return false;
    }

    rstd_warn("cannot reserve a shader cache temporary file for '{}'", path);
    return false;
}

} // namespace

namespace
{

Option<String> MakeShaderSourceCacheKey(ref<str> source, slice<ShaderTexInfo> texinfos) {
    ShaderCacheByteWriter key;
    if (! key.String(source) || texinfos.len().to_primitive() > kMaxShaderCacheMapEntries)
        return None();
    key.U32(u32(texinfos.len().to_primitive()).to_primitive());
    for (const auto& texinfo : texinfos) {
        u32 bits { texinfo.enabled ? 1u : 0u };
        for (usize index {}; index < texinfo.composEnabled.len(); ++index) {
            if (texinfo.composEnabled[index]) bits |= u32(1u << (index.to_primitive() + 1));
        }
        key.U32(bits.to_primitive());
    }
    auto digest = utils::genSha1(slice<rstd::byte>::from_raw_parts(
        reinterpret_cast<const rstd::byte*>(key.bytes().data()), key.bytes().len()));
    return Some(rstd::move(digest));
}

usize EstimateShaderAnnotations(const ShaderInfo& info) {
    usize bytes { sizeof(ShaderInfo) };
    bytes += info.combos.len() * usize(128);
    for (const auto& [name, value] : info.combos.iter()) bytes += name->len() + value->len();
    bytes += info.alias.len() * usize(128);
    for (const auto& [name, value] : info.alias.iter()) bytes += name->len() + value->len();
    for (const auto& [name, value] : info.svs.iter()) {
        bytes += name->len() + usize(sizeof(ShaderValue) + 96);
    }
    for (const auto& [slot, texture] : info.defTexs) {
        static_cast<void>(slot);
        bytes += texture.len() + usize(64);
    }
    bytes += (info.combo_defs.len() + info.texture_uniforms.len() + info.scalar_uniforms.len()) *
             usize(512);
    bytes += info.shadow_pass.len();
    return bytes;
}

void MergeShaderAnnotations(ShaderInfo& target, const ShaderInfo& source) {
    for (const auto& [name, value] : source.combos.iter())
        (void)target.combos.insert(name->clone(), value->clone());
    for (const auto& [name, value] : source.svs.iter())
        (void)target.svs.insert(name->clone(), value->clone());
    for (const auto& [name, value] : source.alias.iter())
        (void)target.alias.insert(name->clone(), value->clone());
    for (const auto& texture : source.defTexs) target.defTexs.push(texture.clone());
    for (const auto& combo : source.combo_defs) target.combo_defs.push(combo.clone());
    for (const auto& uniform : source.texture_uniforms) {
        target.texture_uniforms.push(uniform.clone());
    }
    for (const auto& uniform : source.scalar_uniforms) {
        target.scalar_uniforms.push(uniform.clone());
    }
    if (! source.shadow_pass.is_empty()) target.shadow_pass = source.shadow_pass.clone();
}

} // namespace

Combos ShaderParser::ResolveShaderCombos(const ShaderInfo& info, const Combos& input_combos) {
    auto                                    resolved = input_combos.clone();
    BTreeMap<String, const wpscene::Combo*> definitions;
    BTreeMap<String, bool>                  active;

    for (const auto& combo : info.combo_defs) {
        (void)definitions.insert(combo.combo.clone(), &combo);
        (void)active.insert(combo.combo.clone(), true);
        if (! resolved.contains_key(combo.combo.as_str()))
            (void)resolved.insert(combo.combo.clone(), rstd::format("{}", combo.default_));
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [name, definition] : definitions.iter()) {
            if (! **active.get(name->as_str())) continue;
            const auto* combo = *definition;
            for (const auto& [required_name, required_value] : combo->require.iter()) {
                auto value      = resolved.get(required_name->as_str());
                auto dependency = active.get(required_name->as_str());
                if (value.is_none() ||
                    (**value).as_str() != rstd::format("{}", *required_value).as_str() ||
                    (dependency.is_some() && ! **dependency)) {
                    **active.get_mut(name->as_str()) = false;
                    changed                          = true;
                    break;
                }
            }
        }
    }

    // Saved materials retain values for hidden editor controls. Compile hidden combos at their
    // declared defaults; leave a zero default undefined so shader-side aliases can define it.
    for (const auto& [name, definition] : definitions.iter()) {
        if (**active.get(name->as_str())) continue;
        const auto* combo = *definition;
        if (combo->default_ == i32())
            (void)resolved.remove(name->as_str());
        else
            (void)resolved.insert(name->clone(), rstd::format("{}", combo->default_));
    }
    return resolved;
}

String ShaderParser::PreShaderSrc(fs::VFS& vfs, ref<str> src, ShaderInfo* pShaderInfo,
                                  slice<ShaderTexInfo> texinfos, ShaderCache* cache) {
    // Expand `#include "FILE"` in place: replace each include line with its
    // resolved content (recursively expanded). Preserves the include's
    // original position so a `struct Grid { ... }; #include "common.h"`
    // pattern doesn't end up nesting the include's functions inside the
    // struct body. ParseShader still runs over the resolved include text
    // (for `// [COMBO]` / `uniform NAME // {json}` extraction) and over the
    // user source (sans include directives).
    Option<String> source_cache_key;
    if (cache != nullptr) {
        source_cache_key = MakeShaderSourceCacheKey(src, texinfos);
        if (source_cache_key.is_some()) {
            auto cached = cache->m_source_entries.get(source_cache_key->as_str());
            if (cached.is_some()) {
                MergeShaderAnnotations(*pShaderInfo, (*cached)->annotations);
                return (*cached)->source.clone();
            }
        }
    }

    String newsrc;
    newsrc.reserve(src.len());
    String all_includes;

    usize                  cursor {};
    auto                   source = src;
    shader_lex::LineWalker w(source);
    for (; ! w.Done(); w.Step()) {
        shader_lex::Cursor c(source);
        c.SeekTo(w.LineStart());
        if (! c.MatchHashDirective("include"_str)) continue;

        // Copy bytes up to this line, then splice in the recursively-expanded
        // include body. The newline after the directive stays as part of the
        // splice (we step the outer cursor to LineEnd).
        newsrc.push_str(src.get(cursor, w.LineStart()).unwrap());
        auto include_line = String::make(src.get(w.LineStart(), w.LineEnd()).unwrap());
        include_line.push_ascii(u8('\n'));
        auto expanded = LoadGlslInclude(vfs, include_line.as_str());
        newsrc.push_str(expanded.as_str());
        all_includes.push_str(expanded.as_str());
        cursor = w.LineEnd();
    }
    newsrc.push_str(src.get(cursor, src.len()).unwrap());
    if (cache == nullptr) {
        ParseShader(all_includes.as_str(), pShaderInfo, texinfos);
        ParseShader(newsrc.as_str(), pShaderInfo, texinfos);
        return newsrc;
    }

    ShaderInfo annotations;
    ParseShader(all_includes.as_str(), &annotations, texinfos);
    ParseShader(newsrc.as_str(), &annotations, texinfos);
    MergeShaderAnnotations(*pShaderInfo, annotations);
    if (source_cache_key.is_some()) {
        auto bytes = newsrc.len() * usize(4) + EstimateShaderAnnotations(annotations) +
                     source_cache_key->len() * usize(2) + usize(128);
        if (cache->ReserveSource(bytes)) {
            auto order_key = source_cache_key->clone();
            cache->m_source_entries.insert(source_cache_key.take().unwrap_unchecked(),
                                           ShaderCache::SourceEntry {
                                               .source      = newsrc.clone(),
                                               .annotations = rstd::move(annotations),
                                               .bytes       = bytes,
                                           });
            cache->m_source_order.push(rstd::move(order_key));
            cache->m_source_bytes += bytes;
        }
    }
    return newsrc;
}

String ShaderParser::PreShaderHeader(ref<str> src, const Combos& combos, ShaderType type) {
    // Some workshop shaders contain full-width semicolons, which glslang rejects
    // while compiling the Vulkan shader source.
    auto compatible = String::make(src);
    for (;;) {
        auto pos = compatible.as_str().find("\xEF\xBC\x9B"_str);
        if (pos.is_none()) break;
        compatible.replace_range(*pos, *pos + usize(3), ";"_str);
    }
    auto undefined        = UndefBeforeConflictingMacroDefines(compatible.as_str());
    auto normalized_audio = NormalizePackedAudioSpectrumAccess(undefined.as_str());
    auto normalized_mul   = NormalizeLeadingIntegerMulLiteral(normalized_audio.as_str());
    auto user_src         = normalized_mul.as_str();

    // All stages route through glslang's HLSL frontend.
    String pre;
    if (type == ShaderType::GEOMETRY) {
        pre = rstd::into(pre_shader_code_gs_hlsl);
    } else {
        pre = rstd::into(pre_shader_code);
        ref<str> tail =
            (type == ShaderType::FRAGMENT) ? pre_shader_tail_frag : pre_shader_tail_vert;
        if (auto pos = pre.as_str().find("__SHADER_TAIL__"_str); pos.is_some()) {
            pre.replace_range(*pos, *pos + "__SHADER_TAIL__"_str.len(), tail);
        }
    }

    // If user shader defines its own `mod(...)` at file scope, gate out the
    // prologue's mod overloads to avoid redefinition errors. Substring scan
    // is good enough — function decls always start with one of these tokens
    // followed by a space and `mod(`.
    static constexpr ref<str> kModSentinels[] = {
        "\nfloat mod("_str, "\nfloat2 mod("_str, "\nfloat3 mod("_str, "\nfloat4 mod("_str,
        "\nvec2 mod("_str,  "\nvec3 mod("_str,   "\nvec4 mod("_str,
    };
    bool user_mod = false;
    for (auto needle : kModSentinels) {
        if (user_src.contains(needle) ||
            user_src.starts_with(needle.get(usize(1), needle.len()).unwrap())) {
            user_mod = true;
            break;
        }
    }
    if (user_mod) {
        // Inject #define ahead of the prologue text so the #ifndef guard
        // around our `mod` overloads sees it during glslang preprocess.
        pre.insert_str(usize(), "#define WW_USER_MOD 1\n"_str);
    }

    String combo_defines;
    for (const auto& [name, value] : combos.iter()) {
        auto bytes = Vec<u8>::with_capacity(name->len());
        for (auto character : name->as_str().as_bytes())
            bytes.push(u8(static_cast<rstd::uint8_t>(std::toupper(character.to_primitive()))));
        auto cup = String::from_utf8(rstd::move(bytes)).unwrap();
        if (value->is_empty()) {
            rstd_error("combo '{}' can't be empty", cup);
            continue;
        }
        combo_defines.push_str("#define "_str);
        combo_defines.push_str(cup.as_str());
        combo_defines.push_str(" "_str);
        combo_defines.push_str(value->as_str());
        combo_defines.push_ascii(u8(10));
    }

    // Combo `#define`s land before __SHADER_PLACEHOLD__ so they're visible
    // throughout the user source during the DXC -P pass. The placeholder
    // slot itself is filled by Finalprocessor *after* preprocessing, so
    // the synthesized cbuffer always sees combo references already
    // expanded to literal numbers (e.g. `g_Bones[BONECOUNT]` → `[4]`).
    if (auto pos = pre.as_str().find(SHADER_PLACEHOLD); pos.is_some()) {
        pre.insert_str(*pos, combo_defines.as_str());
    } else {
        pre.push_str(combo_defines.as_str());
    }
    pre.push_str(user_src);
    return pre;
}

namespace
{

// Serialize one CompileToSpv invocation as a JSON object. Captures the
// raw post-PreShaderSrc state (includes resolved, prologue not yet
// applied, regex extraction not yet run) so a replay through the full
// pipeline exercises every transform downstream.
Json BuildShaderRecord(ref<str> scene_id, slice<ShaderUnit> units, const ShaderInfo* shader_info,
                       slice<ShaderTexInfo> texs) {
    auto stage_name = [](ShaderType s) -> ref<str> {
        switch (s) {
        case ShaderType::VERTEX: return "VERTEX"_str;
        case ShaderType::FRAGMENT: return "FRAGMENT"_str;
        case ShaderType::GEOMETRY: return "GEOMETRY"_str;
        }
        return "UNKNOWN"_str;
    };

    auto rec = rstd::json::Map::make();
    rec.insert("scene_id"_Str, Json::String(rstd::into(scene_id)));

    auto js_stages = rstd::json::Array::make();
    for (const auto& u : units) {
        auto stage = rstd::json::Map::make();
        stage.insert("stage"_Str, rstd::into<owe::Json>(String::make(stage_name(u.stage))));
        stage.insert("src"_Str, Json::String(u.src.clone()));
        js_stages.push(Json::Object(rstd::move(stage)));
    }
    rec.insert("stages"_Str, Json::Array(rstd::move(js_stages)));

    auto js_combos = rstd::json::Map::make();
    if (shader_info) {
        for (const auto& [k, v] : shader_info->combos.iter())
            js_combos.insert(k->clone(), Json::String(v->clone()));
    }
    rec.insert("combos"_Str, Json::Object(rstd::move(js_combos)));

    auto js_texs = rstd::json::Array::make();
    for (const auto& t : texs) {
        auto compos = rstd::json::Array::make();
        for (bool enabled : t.composEnabled) compos.push(rstd::into<Json>(enabled));
        auto tex = rstd::json::Map::make();
        tex.insert("enabled"_Str, rstd::into<Json>(bool { t.enabled }));
        tex.insert("compos"_Str, Json::Array(rstd::move(compos)));
        js_texs.push(Json::Object(rstd::move(tex)));
    }
    rec.insert("tex_infos"_Str, Json::Array(rstd::move(js_texs)));

    return Json::Object(rstd::move(rec));
}

void MaybeRecordCompile(ref<str> scene_id, slice<ShaderUnit> units, const ShaderInfo* shader_info,
                        slice<ShaderTexInfo> texs) {
    auto path = rstd::env::var_os("WP_SHADER_RECORD"_str);
    if (! path || path->is_empty()) return;
    Json rec  = BuildShaderRecord(scene_id, units, shader_info, texs);
    auto line = DumpString(rec);
    line.push_ascii('\n');
    auto file = OpenOptions::make().append(true).create(true).open(ref<Path>(path->as_os_str()));
    if (file.is_ok()) {
        (void)file->write_all(line.as_str().as_bytes());
    } else {
        rstd_warn("WP_SHADER_RECORD: cannot open '{}' for append", path->as_os_str().display());
    }
}

} // namespace

bool ShaderParser::CompileToSpv(ref<str> scene_id, mut_ref<ShaderUnit[]> units,
                                Vec<ShaderCode>& codes, ShaderInfo* shader_info,
                                slice<ShaderTexInfo> texs, ShaderCache* cache) {
    MaybeRecordCompile(scene_id, units.as_ref(), shader_info, texs);

    auto make_compile_entry = [](slice<ShaderUnit> source_units, slice<ShaderCode> source_codes) {
        ShaderCache::CompileEntry entry;
        entry.stages.reserve(usize(source_units.len().to_primitive()));
        entry.codes.reserve(source_codes.len());
        entry.bytes = usize(sizeof(ShaderCache::CompileEntry));
        for (const auto& unit : source_units) {
            ShaderCache::CompiledStage stage { .stage = unit.stage };
            stage.uniforms = unit.preprocess_info.uniforms.clone();
            stage.active_tex_slots.reserve(
                usize(unit.preprocess_info.active_tex_slots.len().to_primitive()));
            for (const auto slot : unit.preprocess_info.active_tex_slots.iter()) {
                stage.active_tex_slots.push(u32(*slot));
            }
            entry.stages.push(rstd::move(stage));
            entry.bytes += usize(sizeof(ShaderCache::CompiledStage));
            for (const auto& [name, value] : unit.preprocess_info.uniforms.iter()) {
                entry.bytes += name->len() + value->len() + usize(128);
            }
            entry.bytes += usize(unit.preprocess_info.active_tex_slots.len().to_primitive() * 64);
        }
        for (const auto& code : source_codes) {
            entry.codes.push(code.clone());
            entry.bytes += usize(sizeof(ShaderCode)) + code.len() * usize(sizeof(rstd::uint32_t));
        }
        return entry;
    };

    auto apply_compile_entry = [&](const ShaderCache::CompileEntry& entry) {
        if (entry.stages.len() != usize(units.len().to_primitive()) ||
            entry.codes.len() != usize(units.len().to_primitive())) {
            return false;
        }
        for (usize index {}; index < entry.stages.len(); ++index) {
            const auto& stage = entry.stages[index];
            auto&       unit  = units[usize(index.to_primitive())];
            if (stage.stage != unit.stage) return false;
            unit.preprocess_info.uniforms = stage.uniforms.clone();
            unit.preprocess_info.active_tex_slots.clear();
            for (const auto slot : stage.active_tex_slots) {
                (void)unit.preprocess_info.active_tex_slots.insert(slot);
            }
        }
        codes.clone_from(entry.codes);
        ShapeShaderDefaults(units.as_ref(), *shader_info);
        return true;
    };

    auto store_compile_entry = [&](ref<str> key, ShaderCache::CompileEntry entry) {
        if (cache == nullptr) return;
        auto owned_key = String::make(key);
        entry.bytes += owned_key.len() * usize(2) + usize(128);
        if (! cache->ReserveCompile(entry.bytes)) return;
        const auto bytes     = entry.bytes;
        auto       order_key = owned_key.clone();
        cache->m_compile_entries.insert(rstd::move(owned_key), rstd::move(entry));
        cache->m_compile_order.push(rstd::move(order_key));
        cache->m_compile_bytes += bytes;
    };

    Option<rstd::path::PathBuf> cache_file_path;
    Option<ShaderCacheIdentity> cache_identity;
    if (cache != nullptr) {
        cache_identity = MakeShaderCacheIdentity(units.as_ref(), shader_info->combos);
        if (cache_identity) {
            auto key    = cache_identity->cache_key_hex.as_str();
            auto memory = cache->m_compile_entries.get(key);
            if (memory.is_some()) {
                return apply_compile_entry(**memory);
            }

            auto cache_dir = cache->directory();
            if (cache_dir.is_some()) {
                cache_file_path = Some(
                    GetCachePath(*cache_dir, scene_id, cache_identity->cache_key_hex.as_str()));
                auto                  cached = rstd::fs::read(cache_file_path->as_path());
                ShaderCacheReadResult decoded;
                if (cached.is_ok()) {
                    auto cached_bytes = rstd::move(cached).unwrap_unchecked();
                    decoded           = ShaderCacheArtifactCodec::Decode(
                        *cache_identity,
                        units.as_ref(),
                        slice<rstd::uint8_t>::from_raw_parts(
                            reinterpret_cast<const rstd::uint8_t*>(cached_bytes.data()),
                            usize(cached_bytes.len().to_primitive())));
                } else {
                    auto error = rstd::move(cached).unwrap_err_unchecked();
                    if (error.kind().code == rstd::io::error::ErrorKind::NotFound) {
                        decoded.status = ShaderCacheReadStatus::Miss;
                    } else {
                        rstd_warn(
                            "cannot read shader cache '{}': {}", cache_file_path->as_path(), error);
                    }
                }
                if (decoded.status == ShaderCacheReadStatus::Hit) {
                    auto entry = make_compile_entry(decoded.artifact.units.as_slice(),
                                                    decoded.artifact.codes.as_slice());
                    if (! apply_compile_entry(entry)) return false;
                    store_compile_entry(cache_identity->cache_key_hex.as_str(), rstd::move(entry));
                    return true;
                }
                if (decoded.status == ShaderCacheReadStatus::Invalid &&
                    ! decoded.reason.is_empty()) {
                    rstd_warn("shader cache '{}' is invalid ({}); recompiling",
                              cache_file_path->as_path(),
                              decoded.reason);
                }
            }
        } else {
            rstd_warn("shader cache identity exceeds format limits; compiling without cache");
        }
    }

    for (auto& unit : units) {
        unit.src =
            Preprocessor(unit.src.as_str(), unit.stage, shader_info->combos, unit.preprocess_info);
    }
    ShapeShaderDefaults(units.as_ref(), *shader_info);

    auto compile = [](mut_ref<ShaderUnit[]> units, Vec<ShaderCode>& codes) {
        // Build the cross-stage interface before rewriting each source. Using
        // only adjacent sources misses uniforms that
        // lives on a non-adjacent stage (e.g. FS-only `g_Brightness` not seen
        // by VS in a 3-stage VS→GS→FS chain), which results in different UBO
        // sizes per stage and the runtime allocating a buffer too small for
        // the longest stage.
        auto uniform_interface = BuildUniformInterface(units);

        auto vunits = Vec<vulkan::ShaderCompUnit>::with_capacity(usize(units.len().to_primitive()));
        for (rstd::size_t i = 0; i < units.len().to_primitive(); i++) {
            auto&             unit     = units[usize(i)];
            PreprocessorInfo* pre_info = i >= 1 ? &units[usize(i - 1)].preprocess_info : nullptr;
            PreprocessorInfo* post_info =
                i + 1 < units.len().to_primitive() ? &units[usize(i + 1)].preprocess_info : nullptr;

            const bool pack_varying_arrays =
                (unit.stage == ShaderType::VERTEX && i + 1 < units.len().to_primitive() &&
                 units[usize(i + 1)].stage == ShaderType::FRAGMENT) ||
                (unit.stage == ShaderType::FRAGMENT && i >= 1 &&
                 units[usize(i - 1)].stage == ShaderType::VERTEX);
            unit.src =
                Finalprocessor(unit, pre_info, post_info, &uniform_interface, pack_varying_arrays);

            vunits.push(vulkan::ShaderCompUnit {
                .stage = unit.stage,
                .src   = unit.src.clone(),
                .lang  = vulkan::SourceLang::Hlsl,
            });
        }

        vulkan::ShaderCompOpt opt;
        opt.target   = vulkan::VulkanTarget::Vulkan_1_1;
        opt.optimize = false;

        Vec<vulkan::Uni_ShaderSpv> spvs;
        spvs.reserve(usize(units.len().to_primitive()));

        if (! vulkan::CompileAndLinkShaderUnits(vunits.as_slice(), opt, spvs)) {
            return false;
        }

        codes.clear();
        for (auto& spv : spvs) {
            codes.push(rstd::move(spv->spirv));
        }
        return true;
    };

    if (! compile(units, codes)) return false;
    if (cache != nullptr && cache_identity) {
        store_compile_entry(cache_identity->cache_key_hex.as_str(),
                            make_compile_entry(units.as_ref(), codes.as_slice()));
    }
    if (cache_file_path.is_some() && cache_identity) {
        auto artifact =
            ShaderCacheArtifactCodec::Encode(*cache_identity, units.as_ref(), codes.as_slice());
        if (! artifact) {
            rstd_warn("cannot encode shader cache artifact '{}'; continuing without cache",
                      cache_file_path->as_path());
        } else {
            PublishShaderCacheArtifact(
                cache_file_path->as_path(),
                cache_identity->cache_key_hex.as_str(),
                slice<rstd::uint8_t>::from_raw_parts(artifact->data(),
                                                     usize(artifact->len().to_primitive())));
        }
    }
    return true;
}

namespace
{

ShaderTexInfo ToShaderTexInfo(const SceneShaderTextureCompileInfo& info) {
    return ShaderTexInfo {
        .enabled       = info.enabled,
        .composEnabled = info.components,
    };
}

SceneShaderTextureCompileInfo ToSceneShaderTextureCompileInfo(const ShaderTexInfo& info) {
    return SceneShaderTextureCompileInfo {
        .enabled    = info.enabled,
        .components = info.composEnabled,
    };
}

void MergeVariantFallbackMetadata(ShaderInfo& info, const SceneShaderVariantDesc& desc) {
    for (const auto& [key, value] : desc.uniform_aliases.iter()) {
        if (! info.alias.contains_key(key->as_str()))
            (void)info.alias.insert(key->clone(), value->clone());
    }
    for (const auto& [key, value] : desc.default_uniforms.iter()) {
        if (! info.svs.contains_key(key->as_str()))
            (void)info.svs.insert(key->clone(), value->clone());
    }
    for (const auto& texture : desc.default_textures) {
        const bool found = info.defTexs.iter().any([&](auto parsed) {
            return parsed->slot == texture.slot;
        });
        if (! found) info.defTexs.push(texture.clone());
    }
}

} // namespace

void ShaderParser::UpdateSceneShaderVariantDescFromCompiledUnits(SceneShaderVariantDesc& desc,
                                                                 slice<ShaderUnit>       units,
                                                                 slice<ShaderCode>       codes) {
    for (rstd::size_t i = 0; i < desc.stages.len().to_primitive() && i < units.len().to_primitive();
         ++i) {
        desc.stages[usize(i)].active_texture_slots =
            units[usize(i)].preprocess_info.active_tex_slots.clone();
        desc.stages[usize(i)].uniforms = units[usize(i)].preprocess_info.uniforms.clone();
        if (i < codes.len().to_primitive())
            desc.stages[usize(i)].code_hash = SceneShaderStageCodeHash(codes[usize(i)]);
    }

    Vec<vulkan::Uni_ShaderSpv> spvs;
    vulkan::ShaderReflected    reflected;
    if (! vulkan::GenReflect(codes, spvs, reflected)) return;

    desc.sampler_bindings.clear();
    constexpr ref<str> texture_prefix { "g_Texture"_str };
    for (const auto& [name_ref, binding_ref] : reflected.binding_map.iter()) {
        const auto  name    = name_ref->as_str();
        const auto& binding = *binding_ref;
        if (binding.layout.descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
            ! name.starts_with(texture_prefix)) {
            continue;
        }
        const auto suffix = *name.get(texture_prefix.len(), name.len());
        if (suffix.is_empty() || (suffix.len() > usize(1) && suffix.starts_with("0"_str))) continue;
        if (! suffix.bytes().all([](u8 value) {
                return value >= u8('0') && value <= u8('9');
            }))
            continue;
        auto parsed_slot = rstd::from_str<usize>(suffix);
        if (parsed_slot.is_err()) continue;
        const auto slot = parsed_slot.unwrap().to_primitive();
        desc.sampler_bindings.push(SceneSamplerBinding {
            .texture_slot  = slot,
            .shader_member = name_ref->clone(),
        });
    }
    sort_unstable_by(desc.sampler_bindings.as_mut_slice().as_mut_ref(),
                     [](const auto& lhs, const auto& rhs) {
                         return lhs.texture_slot < rhs.texture_slot;
                     });

    struct BindingRecord {
        ref<str>       name;
        rstd::uint32_t set { 0 };
        rstd::uint32_t binding { 0 };
        rstd::uint32_t descriptor_type { 0 };
        rstd::uint32_t descriptor_count { 0 };
        rstd::uint32_t stage_flags { 0 };
    };
    struct UniformMemberRecord {
        ref<str>     name;
        unsigned     offset { 0 };
        rstd::size_t size { 0 };
        rstd::size_t num { 0 };
    };
    struct UniformBlockRecord {
        ref<str>                 name;
        unsigned                 size { 0 };
        Vec<UniformMemberRecord> members;
    };

    auto binding_less = [](const BindingRecord& lhs, const BindingRecord& rhs) {
        if (lhs.set != rhs.set) return lhs.set < rhs.set;
        if (lhs.binding != rhs.binding) return lhs.binding < rhs.binding;
        return lhs.name.bytes().cmp(rhs.name.bytes()) < 0;
    };
    auto member_less = [](const UniformMemberRecord& lhs, const UniformMemberRecord& rhs) {
        if (lhs.offset != rhs.offset) return lhs.offset < rhs.offset;
        return lhs.name.bytes().cmp(rhs.name.bytes()) < 0;
    };
    auto block_less = [](const UniformBlockRecord& lhs, const UniformBlockRecord& rhs) {
        return lhs.name.bytes().cmp(rhs.name.bytes()) < 0;
    };

    Vec<BindingRecord> bindings;
    bindings.reserve(reflected.binding_map.len());
    for (const auto& [name_ref, binding_ref] : reflected.binding_map.iter()) {
        const auto& binding = *binding_ref;
        bindings.push(BindingRecord {
            .name             = name_ref->as_str(),
            .set              = binding.set,
            .binding          = binding.layout.binding,
            .descriptor_type  = static_cast<rstd::uint32_t>(binding.layout.descriptorType),
            .descriptor_count = binding.layout.descriptorCount,
            .stage_flags      = binding.layout.stageFlags,
        });
    }
    sort_unstable_by(bindings.as_mut_slice().as_mut_ref(), binding_less);

    Vec<UniformBlockRecord> blocks;
    blocks.reserve(reflected.blocks.len());
    for (const auto& block : reflected.blocks) {
        UniformBlockRecord record {
            .name = block.name.as_str(),
            .size = block.size,
        };
        record.members.reserve(block.member_map.len());
        for (const auto& [name_ref, member_ref] : block.member_map.iter()) {
            const auto& member = *member_ref;
            record.members.push(UniformMemberRecord {
                .name   = name_ref->as_str(),
                .offset = member.offset,
                .size   = member.size.to_primitive(),
                .num    = member.num.to_primitive(),
            });
        }
        sort_unstable_by(record.members.as_mut_slice().as_mut_ref(), member_less);
        blocks.push(rstd::move(record));
    }
    sort_unstable_by(blocks.as_mut_slice().as_mut_ref(), block_less);

    DefaultHasher seed;
    hash_into(bindings.len().to_primitive(), seed);
    for (const auto& binding : bindings) {
        hash_into(binding.name, seed);
        hash_into(binding.set, seed);
        hash_into(binding.binding, seed);
        hash_into(binding.descriptor_type, seed);
        hash_into(binding.descriptor_count, seed);
        hash_into(binding.stage_flags, seed);
    }
    hash_into(blocks.len().to_primitive(), seed);
    for (const auto& block : blocks) {
        hash_into(block.name, seed);
        hash_into(block.size, seed);
        hash_into(block.members.len().to_primitive(), seed);
        for (const auto& member : block.members) {
            hash_into(member.name, seed);
            hash_into(member.offset, seed);
            hash_into(member.size, seed);
            hash_into(member.num, seed);
        }
    }
    desc.descriptor_layout_hash = rstd::as_cast<usize>(seed.finish());

    desc.uniform_blocks.clear();
    bool canonical_abi = false;
    for (const auto& block : reflected.blocks) {
        DefaultHasher block_seed;
        hash_into(block.set, block_seed);
        hash_into(block.binding, block_seed);
        hash_into(block.name.as_str(), block_seed);
        hash_into(block.size, block_seed);
        for (const auto& [name_ref, member_ref] : block.member_map.iter()) {
            const auto  name   = name_ref->as_str();
            const auto& member = *member_ref;
            hash_into(name, block_seed);
            hash_into(member.offset, block_seed);
            hash_into(member.size.to_primitive(), block_seed);
        }
        auto       shared_block = FindGlobalUniformBlock(block.name.as_str());
        const bool shared       = shared_block.is_some();
        canonical_abi           = canonical_abi || shared;
        desc.uniform_blocks.push(SceneShaderUniformBlockInterface {
            .name    = block.name.clone(),
            .set     = u32(block.set),
            .binding = u32(block.binding),
            .scope =
                shared ? SceneShaderUniformBlockScope::Shared : SceneShaderUniformBlockScope::Local,
            .identity = shared ? (**shared_block).identity : block_seed.finish(),
        });
    }

    desc.descriptor_sets.clear();
    for (const auto& binding : bindings) {
        SceneShaderDescriptorSetInterface* target = nullptr;
        for (auto& set : desc.descriptor_sets) {
            if (set.set == u32(binding.set)) target = rstd::addressof(set);
        }
        if (target == nullptr) {
            desc.descriptor_sets.push(SceneShaderDescriptorSetInterface {
                .set = u32(binding.set),
                .push_descriptor =
                    ! canonical_abi || binding.set != kGlobalUniformSet.to_primitive(),
                .identity = canonical_abi && binding.set == kGlobalUniformSet.to_primitive()
                                ? kGlobalUniformSetIdentity
                                : u64(),
            });
            target = rstd::addressof(desc.descriptor_sets.last_mut().unwrap().get_mut());
        }
        target->bindings.push(SceneShaderDescriptorBindingInterface {
            .name             = rstd::into(binding.name),
            .binding          = u32(binding.binding),
            .descriptor_type  = u32(binding.descriptor_type),
            .descriptor_count = u32(binding.descriptor_count),
            .stage_flags      = u32(canonical_abi && binding.set == kGlobalUniformSet.to_primitive()
                                        ? VK_SHADER_STAGE_ALL_GRAPHICS
                                        : binding.stage_flags),
        });
    }
    if (canonical_abi) {
        bool has_draw_set = false;
        for (const auto& set : desc.descriptor_sets) has_draw_set |= set.set == kDrawUniformSet;
        if (! has_draw_set) {
            desc.descriptor_sets.push(SceneShaderDescriptorSetInterface {
                .set             = kDrawUniformSet,
                .push_descriptor = true,
            });
        }
    }
    for (auto& set : desc.descriptor_sets) {
        if (set.identity != u64()) continue;
        DefaultHasher set_seed;
        hash_into(set.set.to_primitive(), set_seed);
        for (const auto& binding : set.bindings) {
            hash_into(binding.name.as_str(), set_seed);
            hash_into(binding.binding.to_primitive(), set_seed);
            hash_into(binding.descriptor_type.to_primitive(), set_seed);
            hash_into(binding.descriptor_count.to_primitive(), set_seed);
            hash_into(binding.stage_flags.to_primitive(), set_seed);
        }
        set.identity = set_seed.finish();
    }
    sort_unstable_by(desc.descriptor_sets.as_mut_slice().as_mut_ref(),
                     [](const auto& lhs, const auto& rhs) {
                         return lhs.set < rhs.set;
                     });
}

CompileSceneShaderVariantResult
ShaderParser::CompileSceneShaderVariant(const SceneShaderVariantDesc& desc, fs::VFS& vfs,
                                        const Combos& combos_override, ShaderCache* cache) {
    CompileSceneShaderVariantResult result;
    result.variant = desc.clone();

    if (! desc.Valid()) {
        result.error = "invalid shader variant descriptor"_Str;
        return result;
    }

    result.tex_info.reserve(desc.texture_infos.len());
    for (const auto& texinfo : desc.texture_infos) {
        result.tex_info.push(ToShaderTexInfo(texinfo));
    }

    Vec<ShaderUnit> units;
    units.reserve(desc.stages.len());
    bool has_geometry_stage = false;
    for (const auto& stage : desc.stages) {
        if (stage.source.is_empty()) {
            result.error = "shader variant stage source is empty"_Str;
            return result;
        }
        has_geometry_stage = has_geometry_stage || stage.stage == ShaderType::GEOMETRY;
        units.push(ShaderUnit {
            .stage           = stage.stage,
            .src             = stage.source.clone(),
            .preprocess_info = {},
        });
    }

    for (auto& unit : units) {
        unit.src = ShaderParser::PreShaderSrc(
            vfs, unit.src.as_str(), &result.info, result.tex_info.as_slice(), cache);
    }

    Combos input_combos = result.info.combos.clone();
    for (const auto& [key, value] : desc.resolved_combos.iter())
        (void)input_combos.insert(key->clone(), value->clone());
    for (const auto& [key, value] : desc.input_combos.iter())
        (void)input_combos.insert(key->clone(), value->clone());
    for (const auto& [key, value] : combos_override.iter()) {
        (void)input_combos.insert(key->clone(), value->clone());
    }
    if (has_geometry_stage && ! input_combos.contains_key(WE_CB_GS_ENABLED)) {
        (void)input_combos.insert(rstd::into(WE_CB_GS_ENABLED), "1"_Str);
    }
    result.info.combos          = ResolveShaderCombos(result.info, input_combos);
    result.variant.input_combos = rstd::move(input_combos);
    MergeVariantFallbackMetadata(result.info, desc);

    result.variant.resolved_combos         = result.info.combos.clone();
    result.variant.uniform_aliases         = result.info.alias.clone();
    result.variant.default_uniforms        = result.info.svs.clone();
    result.variant.default_textures        = result.info.defTexs.clone();
    result.variant.geometry_shader_enabled = has_geometry_stage;
    result.variant.texture_infos.clear();
    result.variant.texture_infos.reserve(usize(result.tex_info.len().to_primitive()));
    for (auto entry : result.tex_info.iter()) {
        const auto& texinfo = *entry;
        result.variant.texture_infos.push(ToSceneShaderTextureCompileInfo(texinfo));
    }

    Vec<ShaderCode> spvs;
    const bool      ok = CompileToSpv(desc.scene_id.as_str(),
                                      units.as_mut_slice().as_mut_ref(),
                                      spvs,
                                      &result.info,
                                      result.tex_info.as_slice(),
                                      cache);
    if (! ok) {
        result.error = "CompileToSpv failed"_Str;
        return result;
    }
    result.variant.default_uniforms = result.info.svs.clone();
    ShaderParser::UpdateSceneShaderVariantDescFromCompiledUnits(
        result.variant, units.as_slice(), spvs.as_slice());

    auto shader               = Arc<SceneShader>::make();
    shader->name              = desc.shader_name.clone();
    shader->matrix_convention = ShaderMatrixConvention::RowVector;
    shader->matrix_abi        = ShaderMatrixAbi::Hlsl;
    shader->codes             = rstd::move(spvs);
    shader->sampler_bindings  = result.variant.sampler_bindings.clone();
    shader->uniform_blocks    = result.variant.uniform_blocks.clone();
    shader->descriptor_sets   = result.variant.descriptor_sets.clone();
    shader->default_uniforms  = result.info.svs.clone();
    result.shader             = Some(rstd::move(shader));
    result.ok                 = true;
    return result;
}

CompileMaterialShaderResult ShaderParser::CompileMaterialShader(const Json& material_json,
                                                                fs::VFS& vfs, ref<str> scene_id,
                                                                const Combos& combos_override,
                                                                ShaderCache*  cache) {
    CompileMaterialShaderResult r;

    wpscene::Material mat;
    if (! mat.FromJson(material_json)) {
        r.error = "Material::FromJson failed"_Str;
        return r;
    }
    r.shader_name = mat.shader.clone();

    if (mat.shader.is_empty()) {
        r.error = "material has no shader name"_Str;
        return r;
    }

    const auto shader_path = rstd::format("/assets/shaders/{}", mat.shader);
    auto       vert_source =
        fs::ReadFileContent(vfs, fs::Path(rstd::format("{}.vert", shader_path).as_str()));
    auto frag_source =
        fs::ReadFileContent(vfs, fs::Path(rstd::format("{}.frag", shader_path).as_str()));
    if (vert_source.is_err() || frag_source.is_err()) {
        r.error = rstd::format("shader source missing: {}.{{vert,frag}}", shader_path);
        return r;
    }
    String vert_src = rstd::move(vert_source).unwrap_unchecked();
    String frag_src = rstd::move(frag_source).unwrap_unchecked();
    String geom_src;
    if (mat.shader == "genericparticle"_str || mat.shader == "genericropeparticle"_str) {
        auto geom_source =
            fs::ReadFileContent(vfs, fs::Path(rstd::format("{}.geom", shader_path).as_str()));
        if (geom_source.is_err()) {
            r.error = rstd::format("shader source missing: {}.geom", shader_path);
            return r;
        }
        geom_src = rstd::move(geom_source).unwrap_unchecked();
    }
    if (vert_src.is_empty() || frag_src.is_empty()) {
        r.error = rstd::format("shader source missing: {}.{{vert,frag}}", shader_path);
        return r;
    }

    // Texture info: enabled flag from non-empty material.textures.
    // Component flags normally come from each .tex header. Skipping the
    // header parse keeps this entry path lightweight; sprite-sheet /
    // packed-channel materials may accordingly compile a different variant
    // than the production path.
    r.tex_info.reserve(mat.textures.len());
    for (const auto& t : mat.textures) {
        r.tex_info.push({ ! t.is_empty() });
    }

    Vec<ShaderUnit> units;
    units.push({ ShaderType::VERTEX, rstd::move(vert_src), {} });
    if (! geom_src.is_empty()) {
        units.push({ ShaderType::GEOMETRY, rstd::move(geom_src), {} });
        (void)r.info.combos.insert(rstd::into(WE_CB_GS_ENABLED), "1"_Str);
    }
    units.push({ ShaderType::FRAGMENT, rstd::move(frag_src), {} });

    for (auto& u : units) {
        u.src =
            ShaderParser::PreShaderSrc(vfs, u.src.as_str(), &r.info, r.tex_info.as_slice(), cache);
    }

    Combos input_combos = r.info.combos.clone();
    for (auto [key, value] : mat.combos.iter()) {
        (void)input_combos.insert(key->clone(), rstd::format("{}", *value));
    }
    for (const auto& [key, value] : combos_override.iter())
        (void)input_combos.insert(key->clone(), value->clone());
    if (! input_combos.contains_key(WE_CB_BLENDMODE))
        (void)input_combos.insert(rstd::into(WE_CB_BLENDMODE), "0"_Str);
    if (! input_combos.contains_key(WE_CB_BONECOUNT))
        (void)input_combos.insert(rstd::into(WE_CB_BONECOUNT), "1"_Str);
    r.info.combos = ResolveShaderCombos(r.info, input_combos);

    const bool ok = ShaderParser::CompileToSpv(
        scene_id, units.as_mut_slice().as_mut_ref(), r.spvs, &r.info, r.tex_info.as_slice(), cache);
    r.ok = ok;
    if (! ok) {
        r.error = "CompileToSpv failed"_Str;
        return r;
    }
    SceneShaderVariantDesc variant;
    ShaderParser::UpdateSceneShaderVariantDescFromCompiledUnits(
        variant, units.as_slice(), r.spvs.as_slice());
    r.uniform_blocks  = rstd::move(variant.uniform_blocks);
    r.descriptor_sets = rstd::move(variant.descriptor_sets);
    return r;
}
