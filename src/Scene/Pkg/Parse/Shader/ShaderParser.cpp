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

static constexpr std::string_view SHADER_PLACEHOLD { "__SHADER_PLACEHOLD__" };

#define SHADER_DIR    "spvs03"
#define SHADER_SUFFIX "spvs"

using namespace owe;
using namespace rstd::prelude;
using namespace rstd::literals;

namespace
{
// Decl scanners over GLSL declaration lines. Each WE shader decl is
// line-scoped; the Cursor primitives in :shader_lex do all char-level work.

struct DeclMatch {
    std::size_t start;       // offset of leading newline (or 0 at file start)
    std::size_t end;         // one past trailing `;`
    std::size_t keep_prefix; // number of bytes from start to preserve when stripping
    ref<str>    storage;     // attribute/varying/in/out/uniform
    ref<str>    type;
    ref<str>    name;
    ref<str>    array; // "[N]" or empty
};

// Try to match `[ws]<storage_kw> <type> <name>[opt-array][ws];` on the line
// starting at `line_start`. Anchored — leading non-whitespace fails it.
inline Option<DeclMatch> TryParseDeclLine(ref<str> src, usize line_start,
                                          std::initializer_list<ref<str>> storage_kws) {
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
inline void ForEachDeclLine(ref<str> src, std::initializer_list<ref<str>> storage_kws, Fn&& fn) {
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
inline std::string ReplaceAll(std::string body, std::string_view needle, std::string_view repl) {
    if (needle.empty()) return body;
    std::string out;
    out.reserve(body.size());
    std::size_t pos = 0;
    while (true) {
        auto next = body.find(needle, pos);
        if (next == std::string::npos) {
            out.append(body, pos, std::string::npos);
            break;
        }
        out.append(body, pos, next - pos);
        out.append(repl);
        pos = next + needle.size();
    }
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
static constexpr const char* pre_shader_code = R"(// auto-generated WE→HLSL prologue
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
// SampleCmpLevelZero handles the depth-compare semantics that `sampler2DComparison`
// implies in GLSL; the paired sampler is a SamplerComparisonState (see
// HLSLSamplerStateType in ShaderParser.cpp). uv.xy is the atlas coord,
// uv.z is the depth to compare against.
#define texSample2DCompare(t, uv, ref) ((t).SampleCmpLevelZero(t##_ww_sampler, (uv), (ref)))
#define texture(t, uv)             texSample2D((t), (uv))
#define textureLod(t, uv, lod)     texSample2DLod((t), (uv), (lod))

__SHADER_TAIL__
__SHADER_PLACEHOLD__

)";

static constexpr const char* lighting_v1_source = R"(
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
)";

// VS/FS tail: stage I/O is plumbed by the Finalprocessor synthesizer. It
// strips every `attribute|varying TYPE NAME;` line and re-emits canonical
// `static TYPE NAME;` decls; combo-gated `#if` branches drop their decls
// at preprocess time, so vert/frag stages get a matching live name set.
// The keywords MUST NOT be #define'd here; if they were, the regex would
// see unsubstituted text but the HLSL parser would see the substituted
// text, drifting the two views apart.
static constexpr const char* pre_shader_tail_vert = R"(
static float4 gl_Position;
// Rename the user's main() so a synthesized HLSL entry point can wrap it.
// The wrapper (main_vs) is appended in Finalprocessor.
#define main shader_main
)";

static constexpr const char* pre_shader_tail_frag = R"(
static float4 gl_FragCoord;
static float4 glOutColor;
#define gl_FragColor glOutColor
#define main shader_main
)";

static constexpr const char* pre_shader_tail_geom = R"()";

// HLSL prologue used when type==GEOMETRY. WE's .geom source is a hybrid:
// GLSL-flavoured top-level `in vec4 X;` / `out vec4 X;` decls + HLSL-style
// `[maxvertexcount] void main() { ... IN[0].X ... v.Y = ...; OUT.Append(v); }`
// body. We feed it to glslang's HLSL frontend (EShSourceHlsl); this prologue
// bridges GLSL types/builtins to HLSL and Finalprocessor strips the `in`/`out`
// lines + emits `struct WW_VSOut/WW_PSIn` + `cbuffer ww_Uniforms` + replaces
// `void main()` with the GS entry signature.
static constexpr const char* pre_shader_code_gs_hlsl = R"(// auto-generated WE→HLSL prologue (GS)
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

)";

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

    int         depth      = 1;
    std::size_t expr_start = (open.offset + open.text.size()).to_primitive();
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
        auto out = String::make("(int)("_str);
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

inline std::string LoadGlslInclude(fs::VFS& vfs, ref<str> input) {
    auto        input_view = rstd::cppstd::as_string_view(input);
    std::string output;
    output.reserve(input.size().to_primitive());
    std::size_t            pos = 0;
    shader_lex::LineWalker w(input);
    for (; ! w.Done(); w.Step()) {
        shader_lex::Cursor c(input);
        c.SeekTo(w.LineStart());
        if (! c.MatchHashDirective("include"_str)) continue;

        // Emit everything up to the directive line, then resolve the include
        // and append the recursively-expanded body. Bytes after the directive
        // on the same line (rare in practice) are skipped — matching the
        // original behavior.
        output.append(input_view, pos, w.LineStart().to_primitive() - pos);
        auto        line_view = *input.get(w.LineStart(), w.LineEnd());
        std::string line      = rstd::cppstd::to_string(line_view);
        auto        in_p      = line.find_first_of('\"');
        auto        in_e      = line.find_last_of('\"');
        if (in_p == std::string::npos || in_e == std::string::npos || in_e <= in_p) {
            // Malformed include — preserve verbatim.
            output.append(line);
            pos = w.LineEnd().to_primitive();
            continue;
        }
        std::string includeName = line.substr(in_p + 1, in_e - in_p - 1);
        auto        include     = fs::ReadFileContent(vfs, "/assets/shaders/" + includeName);
        std::string includeSrc;
        if (include.is_ok()) {
            includeSrc = rstd::move(include).unwrap_unchecked();
        } else {
            rstd_error("Can't read shader include {}", includeName);
        }
        output.append("\n//-----include ");
        output.append(includeName);
        output.append("\n");
        output.append(LoadGlslInclude(vfs, rstd::cppstd::as_str(includeSrc).unwrap()));
        // WE shaders routinely pass a vector opacity (opacity * mask) to the
        // scalar ApplyBlending, relying on fxc's implicit vector->scalar
        // truncation. glslang's HLSL frontend won't truncate at the call, so
        // emit forwarding overloads right after the definition. Gate on the
        // directly-loaded file (not the recursively-expanded body) so a parent
        // header that nests common_blending.h doesn't re-inject the overloads.
        if (includeSrc.find("ApplyBlending(const int") != std::string::npos) {
            output.append("\nvec3 ApplyBlending(const int bm, in vec3 A, in vec3 B, in vec2 o) { "
                          "return ApplyBlending(bm, A, B, o.x); }"
                          "\nvec3 ApplyBlending(const int bm, in vec3 A, in vec3 B, in vec3 o) { "
                          "return ApplyBlending(bm, A, B, o.x); }"
                          "\nvec3 ApplyBlending(const int bm, in vec3 A, in vec3 B, in vec4 o) { "
                          "return ApplyBlending(bm, A, B, o.x); }\n");
        }
        output.append("\n//-----include end\n");
        pos = w.LineEnd().to_primitive();
    }
    output.append(input_view, pos, std::string::npos);
    return output;
}

// ParseShader implementation moved to ShaderParser_Pegtl.cpp.
// Declaration is reachable through wescene.pkg.parse via the same module.

// Find a safe spot in `src` to splice an `#include` line into. The chosen
// position lies after every top-level `attribute/varying/uniform/struct`
// declaration, before `void main(`, and outside any `#if/#endif` block.
// Returns 0 when no preceding decls are found or the source has multiple
// entry points (post-include shaders we can't reason about).
inline std::size_t FindIncludeInsertPos(const std::string& src, std::size_t startPos) {
    using shader_lex::PpKind;
    (void)startPos;
    auto source = rstd::cppstd::as_str(src).unwrap();

    const std::size_t main_pos = src.find("void main(");
    if (main_pos == std::string::npos) return 0;
    if (src.find("void main(", main_pos + 2) != std::string::npos) return 0;

    std::size_t                                      after_pos = std::string::npos;
    std::vector<std::pair<std::size_t, std::size_t>> if_ranges;
    std::vector<std::size_t>                         if_stack;
    const array<ref<str>, 4> kKws { "attribute"_str, "varying"_str, "uniform"_str, "struct"_str };

    shader_lex::LineWalker w(source);
    for (; ! w.Done(); w.Step()) {
        if (w.LineStart().to_primitive() >= main_pos) break;
        std::size_t line_end = std::min(w.LineEnd().to_primitive(), main_pos);

        shader_lex::Cursor c(source);
        c.SeekTo(w.LineStart());
        c.SkipHSpace();
        if (c.Eof() || c.Pos().to_primitive() >= line_end) continue;

        if (c.Peek() == '#') {
            shader_lex::Cursor cc(source);
            cc.SeekTo(w.LineStart());
            auto kind = shader_lex::ClassifyPreproc(cc);
            if (kind == PpKind::If || kind == PpKind::Ifdef || kind == PpKind::Ifndef) {
                if_stack.push_back(w.LineStart().to_primitive());
            } else if (kind == PpKind::Endif) {
                if (! if_stack.empty()) {
                    std::size_t start = if_stack.back();
                    if_stack.pop_back();
                    std::size_t end = w.LineEnd().to_primitive() < src.size()
                                          ? w.LineEnd().to_primitive() + 1
                                          : w.LineEnd().to_primitive();
                    if_ranges.emplace_back(start, end);
                }
            }
        } else {
            for (usize keyword_index {}; keyword_index < kKws.len(); ++keyword_index) {
                shader_lex::Cursor probe(source);
                probe.SeekTo(c.Pos());
                if (probe.MatchKeyword(kKws[keyword_index]) &&
                    probe.Pos().to_primitive() < line_end &&
                    shader_lex::IsHSpace(src[probe.Pos().to_primitive()])) {
                    after_pos = w.LineEnd().to_primitive() < src.size()
                                    ? w.LineEnd().to_primitive() + 1
                                    : w.LineEnd().to_primitive();
                    break;
                }
            }
        }
    }

    std::size_t pos = (after_pos == std::string::npos) ? 0 : std::min(after_pos, main_pos);
    for (const auto& [s, e] : if_ranges) {
        if (pos > s && pos <= e) pos = e;
    }
    return std::min(pos, main_pos);
}

// Comment out stray `#endif` directives with no matching `#if`. A class of
// WE-shipped community shader templates (audio_bars / dot_matrix / sine_wave
// variants — 244 of the corpus failures pre-fix) has one extra `#endif`
// past the file's last `#if`. WE's HLSL toolchain tolerates this; glslang
// rejects it as a preprocess error. Stack-walk the source, and when
// `#endif` would pop an empty stack, comment the line instead.
inline std::string BalanceConditionals(std::string src) {
    using shader_lex::PpKind;
    auto        source = rstd::cppstd::as_str(src).unwrap();
    int         depth  = 0;
    std::string out;
    out.reserve(src.size() + 32);
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
        if (stray_endif) out.append("// (ww stray-endif) ");
        out.append(src, w.LineStart().to_primitive(), (w.LineEnd() - w.LineStart()).to_primitive());
        if (w.LineEnd().to_primitive() < src.size()) out.push_back('\n');
    }
    return out;
}

inline std::string Preprocessor(const std::string& in_src, ShaderType type, const Combos& combos,
                                PreprocessorInfo& process_info) {
    std::string with_prologue = owe::ShaderParser::PreShaderHeader(in_src, combos, type);

    // `#require` is a WE-specific marker, not a real preprocessor directive.
    // Prefix `//` to neutralize it. Allowed leading horizontal whitespace.
    {
        std::string out;
        out.reserve(with_prologue.size());
        auto                   source = rstd::cppstd::as_str(with_prologue).unwrap();
        shader_lex::LineWalker w(source);
        for (; ! w.Done(); w.Step()) {
            shader_lex::Cursor c(source);
            c.SeekTo(w.LineStart());
            if (c.MatchHashDirective("require"_str)) {
                c.SkipHSpace();
                auto requirement = c.ReadIdent();
                if (requirement.is_some() && *requirement == "LightingV1"_str) {
                    out.append(lighting_v1_source);
                    if (w.LineEnd().to_primitive() < with_prologue.size()) out.push_back('\n');
                    continue;
                }
                out.append("//");
            }
            out.append(with_prologue,
                       w.LineStart().to_primitive(),
                       (w.LineEnd() - w.LineStart()).to_primitive());
            if (w.LineEnd().to_primitive() < with_prologue.size()) out.push_back('\n');
        }
        with_prologue = std::move(out);
    }

    with_prologue = BalanceConditionals(std::move(with_prologue));

    // Run glslang's own preprocessor: every `#if SKINNING` / `#if FOG_COMPUTED
    // && (...)` / `#if BLENDMODE == 0` block resolves, combo names (BONECOUNT,
    // …) expand, and `#include`s (already inlined in PreShaderSrc, but
    // harmless to re-run) get handled. The regex extraction below then sees
    // only live declarations.
    // All stages route through glslang's HLSL frontend. Bridging macros in
    // the prologue turn GLSL types/intrinsics into HLSL equivalents.
    vulkan::SourceLang lang = vulkan::SourceLang::Hlsl;
    std::string        src;
    if (! vulkan::Preprocess(with_prologue, type, lang, src)) {
        // Fall through: subsequent compile will fail loudly with the same
        // diagnostics. Keep with_prologue so the failing path matches what
        // a developer would see if they bypassed the preprocess step.
        src = std::move(with_prologue);
    }
    auto source = rstd::cppstd::as_str(src).unwrap();

    // GS source uses `in`/`out` storage classes; VS/FS use `attribute`/`varying`.
    ForEachDeclLine(
        source, { "attribute"_str, "varying"_str, "in"_str, "out"_str }, [&](const DeclMatch& m) {
            // `in`/`out` keep their GLSL storage direction in every stage.
            // Legacy `attribute` is a VS input; `varying` is produced by VS and
            // consumed by FS.
            bool        is_input = (m.storage == "attribute"_str) || (m.storage == "in"_str) ||
                                   (m.storage == "varying"_str && type == ShaderType::FRAGMENT);
            std::string line(src.substr(m.start, m.end - m.start));
            auto        name = rstd::cppstd::to_string(m.name);
            if (is_input)
                process_info.input[name] = std::move(line);
            else
                process_info.output[name] = std::move(line);
        });

    // Non-sampler uniform decls feed Finalprocessor's shared cbuffer.
    // Sampler-typed uniforms are emitted as Texture/SamplerState pairs and
    // captured in active_tex_slots instead.
    ForEachDeclLine(source, { "uniform"_str }, [&](const DeclMatch& m) {
        if (IsSamplerType(m.type)) {
            // Track active sampler slot if it's a `g_TextureN`.
            const ref<str> kTex { "g_Texture"_str };
            if (m.name.size() > kTex.size() && m.name.starts_with(kTex)) {
                auto     num   = *m.name.get(kTex.size(), m.name.size());
                unsigned slot  = 0;
                auto*    begin = reinterpret_cast<const char*>(num.data());
                auto [ptr, ec] = std::from_chars(begin, begin + num.size().to_primitive(), slot);
                if (ec == std::errc()) process_info.active_tex_slots.insert(slot);
            }
            return;
        }
        process_info.uniforms[rstd::cppstd::to_string(m.name)] =
            rstd::cppstd::to_string(m.type) + rstd::cppstd::to_string(m.array);
    });
    return src;
}

// Pass GLSL type names through unchanged; aliases like `float`/`float2` get
// re-emitted as is for HLSL-flavoured leftovers.
inline std::string ToGLSLType(std::string_view t) {
    if (t == "float2") return "vec2";
    if (t == "float3") return "vec3";
    if (t == "float4") return "vec4";
    if (t == "int2") return "ivec2";
    if (t == "int3") return "ivec3";
    if (t == "int4") return "ivec4";
    if (t == "uint2") return "uvec2";
    if (t == "uint3") return "uvec3";
    if (t == "uint4") return "uvec4";
    if (t == "float2x2") return "mat2";
    if (t == "float3x3") return "mat3";
    if (t == "float4x4") return "mat4";
    return std::string(t);
}

// Inverse of ToGLSLType: bridge GLSL aliases back to HLSL canonical names
// (used by the GS synth which feeds HLSL to glslang's HLSL frontend).
inline std::string ToHLSLType(std::string_view t) {
    if (t == "vec2") return "float2";
    if (t == "vec3") return "float3";
    if (t == "vec4") return "float4";
    if (t == "ivec2") return "int2";
    if (t == "ivec3") return "int3";
    if (t == "ivec4") return "int4";
    if (t == "uvec2") return "uint2";
    if (t == "uvec3") return "uint3";
    if (t == "uvec4") return "uint4";
    if (t == "mat2" || t == "mat2x2") return "float2x2";
    if (t == "mat3" || t == "mat3x3") return "float3x3";
    if (t == "mat4" || t == "mat4x4") return "float4x4";
    if (t == "mat2x3") return "float3x2";
    if (t == "mat2x4") return "float4x2";
    if (t == "mat3x2") return "float2x3";
    if (t == "mat3x4") return "float4x3";
    if (t == "mat4x2") return "float2x4";
    if (t == "mat4x3") return "float3x4";
    return std::string(t);
}

struct IODecl {
    char        storage; // 'a' for attribute, 'v' for varying, 'i' for GS `in`, 'o' for GS `out'
    std::string type;    // GLSL type as captured (vec2/vec4/mat3/...)
    std::string name;
    std::string array; // "[N]" or empty
};

inline char StorageCharFor(const std::string& storage_word) {
    if (storage_word == "attribute") return 'a';
    if (storage_word == "in") return 'i';
    if (storage_word == "out") return 'o';
    return 'v'; // varying
}

struct SamplerDecl {
    std::string sampler_type; // "sampler2D" / "samplerCube" / ...
    std::string name;
};

inline std::pair<std::vector<SamplerDecl>, std::string>
ScanAndStripSamplers(const std::string& src) {
    std::vector<SamplerDecl> decls;
    std::string              out;
    out.reserve(src.size());
    std::size_t cursor = 0;
    ForEachDeclLine(rstd::cppstd::as_str(src).unwrap(), { "uniform"_str }, [&](const DeclMatch& m) {
        if (! IsSamplerType(m.type)) return;
        out.append(src, cursor, m.start - cursor);
        out.append(src, m.start, m.keep_prefix);
        cursor = m.end;
        decls.push_back({ rstd::cppstd::to_string(m.type), rstd::cppstd::to_string(m.name) });
    });
    out.append(src, cursor, std::string::npos);
    return { std::move(decls), std::move(out) };
}

inline const char* HLSLSamplerType(std::string_view glsl) {
    if (glsl == "sampler2D") return "Texture2D<float4>";
    if (glsl == "sampler3D") return "Texture3D<float4>";
    if (glsl == "samplerCube") return "TextureCube<float4>";
    // GLSL shadow / comparison samplers: scalar-result texture with a
    // SamplerComparisonState. We bind a Texture2D<float> and a paired
    // SamplerComparisonState (the latter chosen via HLSLSamplerStateType).
    if (glsl == "sampler2DComparison" || glsl == "sampler2DShadow") return "Texture2D<float>";
    return "Texture2D<float4>";
}

inline const char* HLSLSamplerStateType(std::string_view glsl) {
    if (glsl == "sampler2DComparison" || glsl == "sampler2DShadow") return "SamplerComparisonState";
    return "SamplerState";
}

inline bool IsSamplerCombinedImage(std::string_view glsl) {
    // All sampler types in WE are combined image samplers from the
    // descriptor-set side. The HLSL sampling intrinsic differs (Sample
    // vs SampleCmp) but binding semantics are identical.
    (void)glsl;
    return true;
}

// Strip every `uniform TYPE NAME;` declaration (including samplers — already
// stripped by ScanAndStripSamplers when called in sequence, idempotent). The
// caller re-emits them as members of a shared cbuffer.
inline std::string StripUniforms(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    std::size_t cursor = 0;
    ForEachDeclLine(rstd::cppstd::as_str(src).unwrap(), { "uniform"_str }, [&](const DeclMatch& m) {
        out.append(src, cursor, m.start - cursor);
        out.append(src, m.start, m.keep_prefix);
        cursor = m.end;
    });
    out.append(src, cursor, std::string::npos);
    return out;
}

inline bool IsGlobalVariableQualifier(ref<str> token) {
    return token == "const"_str || token == "precise"_str || token == "row_major"_str ||
           token == "column_major"_str || token == "static"_str;
}

// GLSL file-scope variables are private shader state. HLSL instead places an
// unqualified file-scope variable in an implicit $Global uniform block.
inline std::string QualifyGlobalVariablesForHlsl(const std::string& src) {
    auto               source = rstd::cppstd::as_str(src).unwrap();
    shader_lex::Lexer  lexer(source);
    std::vector<usize> insertions;
    int                brace_depth { 0 };
    bool               statement_start { true };
    bool               directive { false };

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
        if (token.text == rstd::cppstd::as_str(SHADER_PLACEHOLD).unwrap()) continue;
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
        if (variable && ! has_static) insertions.push_back(declaration_start);
    }

    if (insertions.empty()) return src;
    std::string out;
    out.reserve(src.size() + insertions.size() * 7);
    std::size_t copied {};
    for (auto offset : insertions) {
        auto pos = offset.to_primitive();
        out.append(src, copied, pos - copied);
        out.append("static ");
        copied = pos;
    }
    out.append(src, copied, std::string::npos);
    return out;
}

inline Option<IODecl> ParseIODecl(const std::string& line) {
    // Skip leading newline / CR that the capture loops preserved as an anchor.
    std::size_t start = 0;
    while (start < line.size() && shader_lex::IsVSpace(line[start])) ++start;
    auto m = TryParseDeclLine(rstd::cppstd::as_str(line).unwrap(),
                              usize(start),
                              { "attribute"_str, "varying"_str, "in"_str, "out"_str });
    if (m.is_none()) return None();
    return Some(IODecl { StorageCharFor(rstd::cppstd::to_string(m->storage)),
                         rstd::cppstd::to_string(m->type),
                         rstd::cppstd::to_string(m->name),
                         rstd::cppstd::to_string(m->array) });
}

enum class IODeclPrecedence
{
    KeepExisting,
    PreferIncoming,
};

inline void AddIODecl(std::vector<IODecl>& decls, const IODecl& decl, IODeclPrecedence precedence) {
    for (auto& existing : decls) {
        if (existing.name != decl.name) continue;
        if (precedence == IODeclPrecedence::PreferIncoming) {
            existing.type  = decl.type;
            existing.array = decl.array;
        }
        return;
    }
    decls.push_back(decl);
}

// Pull all `attribute|varying|in|out TYPE NAME;` decls out, return them
// structured + a copy of the source with the lines removed. Stripping is
// essential: `attribute`/`varying` are not HLSL keywords; the entry-point
// synthesizer re-emits canonical `static TYPE NAME;` decls so it never
// drifts from what DXC's preprocessor actually compiled.
inline std::pair<std::vector<IODecl>, std::string> ScanAndStripIO(const std::string& src) {
    std::vector<IODecl> decls;
    std::string         out;
    out.reserve(src.size());
    std::size_t cursor = 0;
    ForEachDeclLine(rstd::cppstd::as_str(src).unwrap(),
                    { "attribute"_str, "varying"_str, "in"_str, "out"_str },
                    [&](const DeclMatch& m) {
                        out.append(src, cursor, m.start - cursor);
                        out.append(src, m.start, m.keep_prefix);
                        cursor = m.end;
                        decls.push_back({ StorageCharFor(rstd::cppstd::to_string(m.storage)),
                                          rstd::cppstd::to_string(m.type),
                                          rstd::cppstd::to_string(m.name),
                                          rstd::cppstd::to_string(m.array) });
                    });
    out.append(src, cursor, std::string::npos);
    return { std::move(decls), std::move(out) };
}

// Synthesizer output split in two: `pre` is `static TYPE NAME;` decls
// that must precede the user `void main()` (HLSL needs identifiers
// declared before use). `post` is the HLSL entry-point wrapper that
// must follow `void main()` so it can call the renamed `shader_main()`.
struct SynthOutput {
    std::string pre;
    std::string post;
};

inline std::size_t ArraySlots(const std::string& arr) {
    if (arr.size() < 3 || arr.front() != '[' || arr.back() != ']') return 1;
    std::size_t n = 0;
    for (std::size_t i = 1; i + 1 < arr.size(); ++i) {
        const char c = arr[i];
        if (c < '0' || c > '9') return 1;
        n = n * 10 + static_cast<std::size_t>(c - '0');
    }
    return n > 0 ? n : 1;
}

struct PackedIOArray {
    std::string_view vector_type;
    std::size_t      elements;
    std::size_t      components;
    std::size_t      slots;
};

inline Option<PackedIOArray> BuildPackedIOArray(const IODecl& decl) {
    if (decl.array.empty()) return None();

    const auto elements = ArraySlots(decl.array);
    if (elements <= 1) return None();

    const auto       type = ToHLSLType(decl.type);
    std::string_view vector_type;
    std::size_t      components {};
    if (type == "float") {
        vector_type = "float4";
        components  = 1;
    } else if (type == "float2") {
        vector_type = "float4";
        components  = 2;
    } else if (type == "float3") {
        vector_type = "float4";
        components  = 3;
    } else if (type == "int") {
        vector_type = "int4";
        components  = 1;
    } else if (type == "int2") {
        vector_type = "int4";
        components  = 2;
    } else if (type == "int3") {
        vector_type = "int4";
        components  = 3;
    } else if (type == "uint") {
        vector_type = "uint4";
        components  = 1;
    } else if (type == "uint2") {
        vector_type = "uint4";
        components  = 2;
    } else if (type == "uint3") {
        vector_type = "uint4";
        components  = 3;
    } else {
        return None();
    }

    if (elements > std::numeric_limits<std::size_t>::max() / components) return None();
    const auto scalar_count = elements * components;
    return Some(PackedIOArray {
        .vector_type = vector_type,
        .elements    = elements,
        .components  = components,
        .slots       = (scalar_count + 3) / 4,
    });
}

inline std::string PackedIOField(const IODecl& decl) { return "_ww_packed_" + decl.name; }

inline void EmitPackedIOCopy(std::string& out, const IODecl& decl, std::string_view packed_owner,
                             bool pack) {
    auto layout = BuildPackedIOArray(decl);
    if (layout.is_none()) return;

    static constexpr std::string_view components { "xyzw" };
    const auto                        field = PackedIOField(decl);
    for (std::size_t element = 0; element < layout->elements; ++element) {
        for (std::size_t component = 0; component < layout->components; ++component) {
            const auto  scalar = element * layout->components + component;
            std::string source = decl.name + "[" + std::to_string(element) + "]";
            if (layout->components > 1) source += "." + std::string(1, components[component]);
            std::string packed = std::string(packed_owner) + "." + field + "[" +
                                 std::to_string(scalar / 4) + "]." +
                                 std::string(1, components[scalar % 4]);
            out += "    " + (pack ? packed : source) + " = " + (pack ? source : packed) + ";\n";
        }
    }
}

// Build `layout(location=N) in/out TYPE NAME[arr];` declarations from a
// list of IO decls, with locations assigned alphabetically so neighbouring
// stages agree without explicit coordination. `is_input` picks the storage
// qualifier (in vs out). Returns the joined block.
inline std::string EmitStageIOLayout(std::vector<IODecl> decls, bool is_input) {
    // gl_Position is a GLSL builtin; never re-declare it. _ww_sv_position is
    // the GS-side macro alias for the same slot.
    decls.erase(std::remove_if(decls.begin(),
                               decls.end(),
                               [](const IODecl& d) {
                                   return d.name == "gl_Position" || d.name == "_ww_sv_position";
                               }),
                decls.end());
    std::sort(decls.begin(), decls.end(), [](const IODecl& a, const IODecl& b) {
        return a.name < b.name;
    });
    const char* qual = is_input ? "in" : "out";
    std::string out;
    std::size_t loc = 0;
    for (const auto& d : decls) {
        out += "layout(location = " + std::to_string(loc) + ") " + qual + " " + ToGLSLType(d.type) +
               " " + d.name + d.array + ";\n";
        loc += ArraySlots(d.array);
    }
    return out;
}

// HLSL-side struct emission for the GS synth. Drops `[[vk::location(N)]]`
// for the same reason as EmitVSFSStruct — glslang's HLSL frontend collapses
// every element of an explicitly-located array onto the same Location.
inline std::string EmitGSHLSLStruct(std::string_view name, std::vector<IODecl> decls) {
    decls.erase(std::remove_if(decls.begin(),
                               decls.end(),
                               [](const IODecl& d) {
                                   return d.name == "gl_Position" || d.name == "_ww_sv_position";
                               }),
                decls.end());
    std::sort(decls.begin(), decls.end(), [](const IODecl& a, const IODecl& b) {
        return a.name < b.name;
    });
    std::string out;
    out += "struct ";
    out += name;
    out += " {\n";
    out += "    float4 _ww_sv_position : SV_Position;\n";
    for (const auto& d : decls) {
        out += "    " + ToHLSLType(d.type) + " " + d.name + d.array + " : " + d.name + ";\n";
    }
    out += "};\n";
    return out;
}

inline std::string_view HLSLSystemSemantic(std::string_view name) {
    if (name == "gl_VertexID") return "SV_VertexID";
    if (name == "gl_InstanceID") return "SV_InstanceID";
    if (name == "gl_ViewportIndex") return "SV_ViewportArrayIndex";
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
inline std::string EmitVSFSStruct(std::string_view name, std::vector<IODecl> decls,
                                  bool include_sv_position, bool pack_arrays = false) {
    decls.erase(std::remove_if(decls.begin(),
                               decls.end(),
                               [](const IODecl& d) {
                                   return d.name == "gl_Position" || d.name == "_ww_sv_position";
                               }),
                decls.end());
    std::sort(decls.begin(), decls.end(), [](const IODecl& a, const IODecl& b) {
        return a.name < b.name;
    });
    std::string out;
    out += "struct ";
    out += name;
    out += " {\n";
    if (include_sv_position) {
        out += "    float4 _ww_sv_position : SV_Position;\n";
    }
    for (const auto& d : decls) {
        const auto semantic = HLSLSystemSemantic(d.name);
        auto       packed   = pack_arrays ? BuildPackedIOArray(d) : None();
        if (packed.is_some()) {
            out += "    " + std::string(packed->vector_type) + " " + PackedIOField(d) + "[" +
                   std::to_string(packed->slots) + "] : " + d.name + ";\n";
        } else {
            out += "    " + ToHLSLType(d.type) + " " + d.name + d.array + " : " +
                   (semantic.empty() ? d.name : std::string(semantic)) + ";\n";
        }
    }
    out += "};\n";
    return out;
}

// Emit the HLSL synth block (pre = decls / structs / cbuffer / samplers,
// post = entry-point wrapper) for VS or FS. Locations are alphabetical so
// vert/frag stages agree without explicit coordination — both are called
// with the same cross-stage varying union.
inline SynthOutput SynthesizeHLSLEntry(ShaderType stage, std::vector<IODecl> attrs,
                                       std::vector<IODecl> varyings, bool pack_varying_arrays) {
    SynthOutput so;
    if (stage == ShaderType::GEOMETRY) return so;

    // gl_Position propagates via the SV_Position field, not a regular slot.
    // Filter both names (the GS prologue rewrites `gl_Position` to
    // `_ww_sv_position`, so its post-preprocess form needs filtering too).
    auto drop_position = [](std::vector<IODecl>& v) {
        v.erase(std::remove_if(v.begin(),
                               v.end(),
                               [](const IODecl& d) {
                                   return d.name == "gl_Position" || d.name == "_ww_sv_position";
                               }),
                v.end());
    };
    drop_position(attrs);
    drop_position(varyings);

    auto by_name = [](const IODecl& a, const IODecl& b) {
        return a.name < b.name;
    };
    std::sort(attrs.begin(), attrs.end(), by_name);
    std::sort(varyings.begin(), varyings.end(), by_name);

    // Static globals so the user shader body resolves `a_Position`,
    // `v_TexCoord`, etc. regardless of #if-branch visibility — the wrapper
    // copies from/to the entry struct.
    so.pre += "\n// === auto-generated stage I/O statics ===\n";
    for (const auto& d : attrs) {
        so.pre += "static " + ToHLSLType(d.type) + " " + d.name + d.array + ";\n";
    }
    for (const auto& d : varyings) {
        so.pre += "static " + ToHLSLType(d.type) + " " + d.name + d.array + ";\n";
    }

    std::string& out = so.post;
    out += "\n// === auto-generated entry point ===\n";
    if (stage == ShaderType::VERTEX) {
        out += EmitVSFSStruct("WW_VSIn", attrs, /*sv_pos=*/false);
        out += EmitVSFSStruct("WW_VSOut", varyings, /*sv_pos=*/true, pack_varying_arrays);
        out += "WW_VSOut main_vs(WW_VSIn _ww_in) {\n";
        for (const auto& a : attrs) {
            out += "    " + a.name + " = _ww_in." + a.name + ";\n";
        }
        out += "    shader_main();\n";
        out += "    WW_VSOut _ww_out;\n";
        out += "    _ww_out._ww_sv_position = gl_Position;\n";
        for (const auto& v : varyings) {
            if (v.name == "gl_Position" || v.name == "_ww_sv_position") continue;
            if (pack_varying_arrays && BuildPackedIOArray(v).is_some()) {
                EmitPackedIOCopy(out, v, "_ww_out", true);
            } else {
                out += "    _ww_out." + v.name + " = " + v.name + ";\n";
            }
        }
        out += "    return _ww_out;\n";
        out += "}\n";
    } else { // FRAGMENT
        out += EmitVSFSStruct("WW_PSIn", varyings, /*sv_pos=*/true, pack_varying_arrays);
        out += "float4 main_ps(WW_PSIn _ww_in) : SV_Target0 {\n";
        out += "    gl_FragCoord = _ww_in._ww_sv_position;\n";
        for (const auto& v : varyings) {
            if (v.name == "gl_Position" || v.name == "_ww_sv_position") continue;
            if (pack_varying_arrays && BuildPackedIOArray(v).is_some()) {
                EmitPackedIOCopy(out, v, "_ww_in", false);
            } else {
                out += "    " + v.name + " = _ww_in." + v.name + ";\n";
            }
        }
        out += "    shader_main();\n";
        out += "    return glOutColor;\n";
        out += "}\n";
    }
    return so;
}

// Find a literal `void main()` call in `src` (no regex). Replace with the GS
// entry-point signature. Returns the modified source unchanged if no match.
inline std::string RewriteGSMain(std::string src) {
    static constexpr std::string_view marker { "void main()" };
    static constexpr std::string_view repl {
        "void main_gs(point WW_VSOut IN[1], inout TriangleStream<WW_PSIn> OUT)"
    };
    if (auto pos = src.find(marker); pos != std::string::npos) {
        src.replace(pos, marker.size(), repl);
    }
    return src;
}

// std140 base alignment + size for one element (not the array — caller
// scales). HLSL form (`floatRxC`, `floatN`, scalars). Unknown types fall back
// to vec4-equivalent which is always safely-aligned, never under-padded.
struct Std140Layout {
    std::size_t align;
    std::size_t size;
};
inline Std140Layout Std140Base(std::string_view hlsl_base) {
    if (hlsl_base == "float" || hlsl_base == "int" || hlsl_base == "uint" || hlsl_base == "bool")
        return { 4, 4 };
    if (hlsl_base == "float2" || hlsl_base == "int2" || hlsl_base == "uint2") return { 8, 8 };
    if (hlsl_base == "float3" || hlsl_base == "int3" || hlsl_base == "uint3") return { 16, 12 };
    if (hlsl_base == "float4" || hlsl_base == "int4" || hlsl_base == "uint4") return { 16, 16 };
    // column_major float<R>x<C> = C columns of vec<R>, each padded to 16
    // bytes by std140 → 16*C bytes total.
    if (hlsl_base.size() == 8 && hlsl_base.substr(0, 5) == "float" && hlsl_base[6] == 'x' &&
        hlsl_base[5] >= '2' && hlsl_base[5] <= '4' && hlsl_base[7] >= '2' && hlsl_base[7] <= '4') {
        std::size_t cols = (std::size_t)(hlsl_base[7] - '0');
        return { 16, cols * 16 };
    }
    return { 16, 16 };
}

inline std::pair<std::string_view, std::string_view> SplitUniformType(std::string_view ty) {
    if (auto pos = ty.find('['); pos != std::string_view::npos) {
        return { ty.substr(0, pos), ty.substr(pos) };
    }
    return { ty, {} };
}

struct UniformLayout {
    std::string_view array;
    std::string      hlsl_ty;
    std::size_t      align;
    std::size_t      size;
};

inline std::size_t ParseArrayCount(std::string_view arr) {
    if (arr.size() < 3 || arr.front() != '[' || arr.back() != ']') return 1;
    std::string_view inner = arr.substr(1, arr.size() - 2);
    std::size_t      n     = 0;
    for (char c : inner) {
        if (c == ' ' || c == '\t') continue;
        if (c < '0' || c > '9') return 1;
        n = n * 10 + (std::size_t)(c - '0');
    }
    return n == 0 ? 1 : n;
}

inline UniformLayout LayoutUniform(std::string_view ty) {
    const auto [base_ty, array] = SplitUniformType(ty);
    auto       hlsl_ty          = ToHLSLType(base_ty);
    const auto n                = ParseArrayCount(array);
    const auto L                = Std140Base(hlsl_ty);
    return {
        .array   = array,
        .hlsl_ty = std::move(hlsl_ty),
        .align   = (n > 1) ? std::size_t(16) : L.align,
        .size    = (n > 1) ? ((L.size + 15) & ~std::size_t(15)) * n : L.size,
    };
}

inline void MergeUniform(Map<std::string, std::string>& uniforms_union, std::string_view name,
                         std::string_view ty) {
    auto [it, inserted] = uniforms_union.try_emplace(std::string(name), std::string(ty));
    if (inserted) return;

    const auto old_layout = LayoutUniform(it->second);
    const auto new_layout = LayoutUniform(ty);
    if (new_layout.size > old_layout.size ||
        (new_layout.size == old_layout.size && new_layout.align > old_layout.align)) {
        it->second = std::string(ty);
    }
}

Map<std::string, std::string> BuildUniformUnion(std::span<const ShaderUnit> units) {
    Map<std::string, std::string> uniforms;
    for (const auto& unit : units) {
        for (const auto& [name, ty] : unit.preprocess_info.uniforms) {
            MergeUniform(uniforms, name, ty);
        }
    }
    return uniforms;
}

std::string CanonicalizeGlobalUniformAliases(std::string source) {
    auto              view = rstd::cppstd::as_str(source).unwrap();
    shader_lex::Lexer lexer(view);
    std::string       out;
    std::size_t       copied {};
    for (auto token = lexer.Next(); token.kind != shader_lex::TokenKind::Eof;
         token      = lexer.Next()) {
        if (token.kind != shader_lex::TokenKind::Ident) continue;
        auto field = FindGlobalUniform(token.text);
        if (field.is_none() || (**field).alias.is_empty() || token.text != (**field).alias)
            continue;
        const auto begin = token.offset.to_primitive();
        out.append(source, copied, begin - copied);
        out.append(rstd::cppstd::as_string_view((**field).name));
        copied = begin + token.text.size().to_primitive();
    }
    if (copied == 0) return source;
    out.append(source, copied, std::string::npos);
    return out;
}

Set<std::string> ReferencedUniformNames(std::span<const ShaderUnit> units) {
    Set<std::string> names;
    for (const auto& unit : units) {
        auto              body = StripUniforms(unit.src);
        shader_lex::Lexer lexer(rstd::cppstd::as_str(body).unwrap());
        for (auto token = lexer.Next(); token.kind != shader_lex::TokenKind::Eof;
             token      = lexer.Next()) {
            if (token.kind == shader_lex::TokenKind::Ident) {
                names.insert(rstd::cppstd::to_string(token.text));
            }
        }
    }
    return names;
}

struct UniformCompileInterface {
    bool                          legacy { false };
    Vec<GlobalUniformBlockKind>   global_blocks;
    Map<std::string, std::string> local;
};

UniformCompileInterface BuildUniformInterface(std::span<ShaderUnit> units) {
    UniformCompileInterface result;
    for (const auto& unit : units) {
        for (const auto& [name, type] : unit.preprocess_info.uniforms) {
            auto field = FindGlobalUniform(rstd::cppstd::as_str(name).unwrap());
            if (field.is_none()) continue;
            const auto expected = LayoutUniform(rstd::cppstd::as_string_view((**field).type));
            const auto authored = LayoutUniform(type);
            if (expected.hlsl_ty != authored.hlsl_ty || expected.array != authored.array) {
                rstd_warn("uniform {} type {} does not match canonical type {}; using legacy ABI",
                          name,
                          type,
                          (**field).type);
                result.legacy = true;
            }
        }
    }
    if (result.legacy) {
        result.local = BuildUniformUnion(units);
        return result;
    }

    for (auto& unit : units) {
        unit.src = CanonicalizeGlobalUniformAliases(rstd::move(unit.src));
        Map<std::string, std::string> canonical;
        for (const auto& [name, type] : unit.preprocess_info.uniforms) {
            auto field = FindGlobalUniform(rstd::cppstd::as_str(name).unwrap());
            if (field.is_none()) {
                MergeUniform(canonical, name, type);
                continue;
            }
            MergeUniform(canonical,
                         rstd::cppstd::to_string((**field).name),
                         rstd::cppstd::as_string_view((**field).type));
        }
        unit.preprocess_info.uniforms = rstd::move(canonical);
    }

    const auto referenced = ReferencedUniformNames(units);
    for (const auto& block : GlobalUniformBlocks()) {
        bool active = false;
        for (const auto& field : GlobalUniformFields()) {
            if (GlobalUniformBlockFor(field.producer) != block.kind) continue;
            if (referenced.contains(rstd::cppstd::to_string(field.name))) {
                active = true;
                break;
            }
        }
        if (active) result.global_blocks.push(GlobalUniformBlockKind(block.kind));
    }
    for (const auto& unit : units) {
        for (const auto& [name, type] : unit.preprocess_info.uniforms) {
            if (FindGlobalUniform(rstd::cppstd::as_str(name).unwrap()).is_some() ||
                ! referenced.contains(name)) {
                continue;
            }
            MergeUniform(result.local, name, type);
        }
    }
    return result;
}

usize LinearUniformElementCount(std::string_view ty) {
    const auto [base_ty, array] = SplitUniformType(ty);
    if (! array.empty()) return usize();

    const auto hlsl_ty = ToHLSLType(base_ty);
    if (hlsl_ty == "float" || hlsl_ty == "int" || hlsl_ty == "uint" || hlsl_ty == "bool") {
        return usize(1);
    }
    for (std::string_view prefix : { "float", "int", "uint", "bool" }) {
        if (! hlsl_ty.starts_with(prefix) || hlsl_ty.size() != prefix.size() + 1) continue;
        const char width = hlsl_ty.back();
        if (width >= '2' && width <= '4') return usize(static_cast<std::size_t>(width - '0'));
    }
    return usize();
}

void ShapeShaderValues(ShaderValues& values, const Map<std::string, std::string>& uniforms) {
    for (auto& [name, value] : values) {
        if (value.size() != usize(1)) continue;
        auto uniform = uniforms.find(name);
        if (uniform == uniforms.end()) continue;
        const auto elements = LinearUniformElementCount(uniform->second);
        if (elements <= usize(1) || elements > usize(4)) continue;

        std::array<float, 4> shaped;
        shaped.fill(value[usize()]);
        value = ShaderValue(shaped.data(), elements);
    }
}

void ShapeShaderDefaults(std::span<const ShaderUnit> units, ShaderInfo& info) {
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
inline std::string EmitCBufferStd140(const Map<std::string, std::string>& uniforms_union,
                                     std::string_view block_name, u32 set, u32 binding) {
    std::string out;
    out += "[[vk::binding(" + std::to_string(binding.to_primitive()) + ", " +
           std::to_string(set.to_primitive()) + ")]] cbuffer " + std::string(block_name) + " {\n";
    std::size_t offset = 0;
    for (const auto& [name, ty] : uniforms_union) {
        const auto  layout    = LayoutUniform(ty);
        std::string array     = std::string(layout.array);
        offset                = (offset + layout.align - 1) & ~(layout.align - 1);
        std::size_t reg       = offset / 16;
        std::size_t comp      = (offset % 16) / 4;
        const char  letter    = "xyzw"[comp];
        const bool  is_matrix = layout.hlsl_ty == "float2x2" || layout.hlsl_ty == "float3x3" ||
                                layout.hlsl_ty == "float4x4" || layout.hlsl_ty == "float2x3" ||
                                layout.hlsl_ty == "float2x4" || layout.hlsl_ty == "float3x2" ||
                                layout.hlsl_ty == "float3x4" || layout.hlsl_ty == "float4x2" ||
                                layout.hlsl_ty == "float4x3";
        out += "    ";
        if (is_matrix) out += "column_major ";
        out += layout.hlsl_ty + " " + name + array;
        out += " : packoffset(c" + std::to_string(reg);
        if (comp != 0) {
            out += ".";
            out += letter;
        }
        out += ");\n";
        offset += layout.size;
    }
    out += "};\n";
    return out;
}

inline std::string EmitGlobalCBufferStd140(const GlobalUniformBlockSchema& block) {
    std::string out;
    out += "[[vk::binding(" + std::to_string(block.binding.to_primitive()) + ", " +
           std::to_string(kGlobalUniformSet.to_primitive()) + ")]] cbuffer " +
           rstd::cppstd::to_string(block.name) + " {\n";
    for (const auto& field : GlobalUniformFields()) {
        if (GlobalUniformBlockFor(field.producer) != block.kind) continue;
        const auto layout = LayoutUniform(rstd::cppstd::as_string_view(field.type));
        const auto offset = field.offset.to_primitive();
        const auto reg    = offset / 16;
        const auto comp   = (offset % 16) / 4;
        out += "    " + layout.hlsl_ty + " " + rstd::cppstd::to_string(field.name) +
               std::string(layout.array) + " : packoffset(c" + std::to_string(reg);
        if (comp != 0) out += "." + std::string(1, "xyzw"[comp]);
        out += ");\n";
    }
    out += "};\n";
    return out;
}

inline std::string EmitGlobalCBuffersStd140(const UniformCompileInterface& interface) {
    std::string out;
    for (const auto kind : interface.global_blocks) {
        auto block = FindGlobalUniformBlock(kind);
        if (block.is_some()) out += EmitGlobalCBufferStd140(**block);
    }
    return out;
}

inline std::string Finalprocessor(const ShaderUnit& unit, const PreprocessorInfo* pre,
                                  const PreprocessorInfo*        next,
                                  const UniformCompileInterface* interface,
                                  bool                           pack_varying_arrays) {
    // GS: feed glslang's HLSL frontend. Strip GLSL-style top-level `in`/`out`
    // decls, emit HLSL structs (WW_VSOut/WW_PSIn) + ww_Uniforms cbuffer, and
    // rewrite `void main()` to the entry signature `point WW_VSOut IN[1],
    // inout TriangleStream<WW_PSIn> OUT`.
    if (unit.stage == ShaderType::GEOMETRY) {
        auto [io_decls, stripped] = ScanAndStripIO(unit.src);
        std::string body          = QualifyGlobalVariablesForHlsl(StripUniforms(stripped));

        std::vector<IODecl> in_decls, out_decls;
        auto add_to = [](std::vector<IODecl>& v,
                         const IODecl&        d,
                         IODeclPrecedence     precedence = IODeclPrecedence::KeepExisting) {
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
            for (auto& [k, v] : pre->output) {
                if (auto d = ParseIODecl(v); d) {
                    add_to(in_decls, *d, IODeclPrecedence::PreferIncoming);
                }
            }
        if (next)
            for (auto& [k, v] : next->input) {
                if (auto d = ParseIODecl(v); d) add_out(*d);
            }

        std::string synth;
        synth += "\n// === auto-generated GS stage I/O (HLSL) ===\n";
        synth += EmitGSHLSLStruct("WW_VSOut", std::move(in_decls));
        synth += EmitGSHLSLStruct("WW_PSIn", std::move(out_decls));

        // Legacy callers still synthesize one cross-stage uniform block.
        Map<std::string, std::string> uniforms_union_local;
        if (! interface) {
            auto absorb = [&](const Map<std::string, std::string>& m) {
                for (const auto& [k, v] : m) MergeUniform(uniforms_union_local, k, v);
            };
            absorb(unit.preprocess_info.uniforms);
            if (pre) absorb(pre->uniforms);
            if (next) absorb(next->uniforms);
        }
        const Map<std::string, std::string>& uniforms_union =
            interface ? interface->local : uniforms_union_local;
        if (interface && ! interface->legacy) synth += EmitGlobalCBuffersStd140(*interface);
        if (! uniforms_union.empty()) {
            synth += "\n// === auto-generated draw uniforms (HLSL, std140 via packoffset) ===\n";
            synth += EmitCBufferStd140(uniforms_union,
                                       interface && ! interface->legacy
                                           ? rstd::cppstd::as_string_view(kDrawUniformBlockName)
                                           : "ww_Uniforms",
                                       kDrawUniformSet,
                                       u32(0));
        }

        body = RewriteGSMain(std::move(body));
        return ReplaceAll(std::move(body), SHADER_PLACEHOLD, synth);
    }

    // Strip `attribute/varying` lines and collect them as structured decls.
    auto [io_decls, stage1] = ScanAndStripIO(unit.src);

    // Strip sampler declarations; they are re-emitted with explicit bindings.
    auto [sampler_decls, stage2] = ScanAndStripSamplers(stage1);

    // Strip non-sampler declarations; they are re-emitted through the selected ABI.
    std::string stage3 = QualifyGlobalVariablesForHlsl(StripUniforms(stage2));

    // Partition IO decls into VS inputs and varyings
    // (everything else). The producing stage owns each cross-stage interface
    // type; consumers only contribute names missing from that interface.
    std::vector<IODecl> attrs, varyings;
    auto add = [&](const IODecl& d, IODeclPrecedence precedence = IODeclPrecedence::KeepExisting) {
        const bool vertex_input =
            unit.stage == ShaderType::VERTEX && (d.storage == 'a' || d.storage == 'i');
        std::vector<IODecl>& v = vertex_input ? attrs : varyings;
        AddIODecl(v, d, precedence);
    };
    for (const auto& d : io_decls) add(d);
    auto add_varying_from_line = [&](const std::string& line, IODeclPrecedence precedence) {
        if (auto d = ParseIODecl(line); d) AddIODecl(varyings, *d, precedence);
    };
    if (unit.stage == ShaderType::VERTEX && next) {
        for (auto& [k, v] : next->input) {
            add_varying_from_line(v, IODeclPrecedence::KeepExisting);
        }
    } else if (unit.stage == ShaderType::FRAGMENT && pre) {
        for (auto& [k, v] : pre->output) {
            add_varying_from_line(v, IODeclPrecedence::PreferIncoming);
        }
    }

    // Synthesize the HLSL entry point: static globals for every attr /
    // varying, WW_VSIn/WW_VSOut/WW_PSIn structs, and a main_vs / main_ps
    // wrapper that copies between the struct and the statics.
    SynthOutput synth = SynthesizeHLSLEntry(unit.stage, attrs, varyings, pack_varying_arrays);

    // Legacy callers still synthesize one cross-stage uniform block.
    Map<std::string, std::string> uniforms_union_local;
    if (! interface) {
        auto absorb = [&](const Map<std::string, std::string>& m) {
            for (const auto& [k, v] : m) MergeUniform(uniforms_union_local, k, v);
        };
        absorb(unit.preprocess_info.uniforms);
        if (pre) absorb(pre->uniforms);
        if (next) absorb(next->uniforms);
    }
    const Map<std::string, std::string>& uniforms_union =
        interface ? interface->local : uniforms_union_local;

    std::string uniform_block;
    if (interface && ! interface->legacy) uniform_block += EmitGlobalCBuffersStd140(*interface);
    if (! uniforms_union.empty()) {
        uniform_block +=
            "\n// === auto-generated draw uniforms (HLSL, std140 via packoffset) ===\n";
        uniform_block += EmitCBufferStd140(uniforms_union,
                                           interface && ! interface->legacy
                                               ? rstd::cppstd::as_string_view(kDrawUniformBlockName)
                                               : "ww_Uniforms",
                                           kDrawUniformSet,
                                           u32(0));
    }

    // Binding 0 stays reserved for the draw uniform block. `vk::combinedImageSampler`
    // joins each texture and sampler pair into one Vulkan descriptor.
    Set<std::string> sampler_seen;
    std::string      sampler_block;
    if (! sampler_decls.empty()) sampler_block += "\n// === auto-generated samplers (HLSL) ===\n";
    std::size_t sampler_idx = 1;
    for (const auto& s : sampler_decls) {
        if (! sampler_seen.insert(s.name).second) continue;
        const char* tex_ty   = HLSLSamplerType(s.sampler_type);
        const char* state_ty = HLSLSamplerStateType(s.sampler_type);
        sampler_block +=
            "[[vk::combinedImageSampler]][[vk::binding(" + std::to_string(sampler_idx) + ", " +
            std::to_string(kDrawUniformSet.to_primitive()) + ")]] " + tex_ty + " " + s.name + ";\n";
        sampler_block += "[[vk::combinedImageSampler]][[vk::binding(" +
                         std::to_string(sampler_idx) + ", " +
                         std::to_string(kDrawUniformSet.to_primitive()) + ")]] " + state_ty + " " +
                         s.name + "_ww_sampler;\n";
        ++sampler_idx;
    }

    // Splice synth.pre into the placeholder slot, then append synth.post
    // (which contains the entry-point wrapper that has to follow the user's
    // shader_main()).
    std::string with_decls =
        ReplaceAll(stage3, SHADER_PLACEHOLD, synth.pre + uniform_block + sampler_block);
    return with_decls + synth.post;
}

using ShaderCacheDigest = std::array<std::uint8_t, 20>;

constexpr std::array<std::uint8_t, 8> kShaderCacheMagic { 'O', 'W', 'E', 'S', 'P', 'V', '3', 0 };
constexpr std::uint32_t               kShaderCacheFormatVersion = 3;
constexpr std::uint32_t               kShaderCacheAbiVersion    = 19;
// 8-byte magic, six u32 fields, and four SHA-1 digests total 112 bytes.
constexpr std::uint32_t kShaderCacheHeaderSize = static_cast<std::uint32_t>(
    kShaderCacheMagic.size() + 6 * sizeof(std::uint32_t) + 4 * ShaderCacheDigest {}.size());
constexpr std::uint32_t kMaxShaderCacheStages      = 16;
constexpr std::uint32_t kMaxShaderCacheMapEntries  = 4096;
constexpr std::uint32_t kMaxShaderCacheSlots       = 1024;
constexpr std::uint32_t kMaxShaderCacheStringSize  = 32 * 1024 * 1024;
constexpr std::uint32_t kMaxShaderCachePayloadSize = 256 * 1024 * 1024;

class ShaderCacheByteWriter {
public:
    void U32(std::uint32_t value) {
        m_bytes.push_back(static_cast<std::uint8_t>(value));
        m_bytes.push_back(static_cast<std::uint8_t>(value >> 8));
        m_bytes.push_back(static_cast<std::uint8_t>(value >> 16));
        m_bytes.push_back(static_cast<std::uint8_t>(value >> 24));
    }

    bool Bytes(std::span<const std::uint8_t> value) {
        if (m_bytes.size() > std::numeric_limits<std::uint32_t>::max() ||
            value.size() > std::numeric_limits<std::uint32_t>::max() - m_bytes.size()) {
            return false;
        }
        m_bytes.insert(m_bytes.end(), value.begin(), value.end());
        return true;
    }

    bool String(std::string_view value) {
        if (value.size() > kMaxShaderCacheStringSize) return false;
        U32(static_cast<std::uint32_t>(value.size()));
        return Bytes(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
    }

    const std::vector<std::uint8_t>& bytes() const noexcept { return m_bytes; }
    std::vector<std::uint8_t>        Take() noexcept { return std::move(m_bytes); }

private:
    std::vector<std::uint8_t> m_bytes;
};

class ShaderCacheByteReader {
public:
    explicit ShaderCacheByteReader(std::span<const std::uint8_t> bytes): m_bytes(bytes) {}

    bool U32(std::uint32_t& value) {
        std::span<const std::uint8_t> bytes;
        if (! Bytes(4, bytes)) return false;
        value = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
                (static_cast<std::uint32_t>(bytes[2]) << 16) |
                (static_cast<std::uint32_t>(bytes[3]) << 24);
        return true;
    }

    bool Bytes(std::size_t size, std::span<const std::uint8_t>& value) {
        if (size > remaining()) return false;
        value = m_bytes.subspan(m_position, size);
        m_position += size;
        return true;
    }

    bool String(std::string& value) {
        std::uint32_t size = 0;
        if (! U32(size) || size > kMaxShaderCacheStringSize) return false;
        std::span<const std::uint8_t> bytes;
        if (! Bytes(size, bytes)) return false;
        value.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    }

    std::size_t remaining() const noexcept { return m_bytes.size() - m_position; }
    bool        done() const noexcept { return m_position == m_bytes.size(); }

private:
    std::span<const std::uint8_t> m_bytes;
    std::size_t                   m_position { 0 };
};

int HexDigit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

Option<ShaderCacheDigest> DecodeShaderCacheDigest(std::string_view value) {
    if (value.size() != 40) return None();
    ShaderCacheDigest digest {};
    for (std::size_t i = 0; i < digest.size(); ++i) {
        const int hi = HexDigit(value[i * 2]);
        const int lo = HexDigit(value[i * 2 + 1]);
        if (hi < 0 || lo < 0) return None();
        digest[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return Some(digest);
}

Option<ShaderCacheDigest> HashShaderCacheBytes(std::span<const std::uint8_t> bytes) {
    return DecodeShaderCacheDigest(utils::genSha1(
        std::span<const char>(reinterpret_cast<const char*>(bytes.data()), bytes.size())));
}

struct ShaderCacheIdentity {
    ShaderCacheDigest cache_key;
    ShaderCacheDigest source;
    ShaderCacheDigest combos;
    std::string       cache_key_hex;
};

Option<ShaderCacheIdentity> MakeShaderCacheIdentity(std::span<const ShaderUnit> units,
                                                    const Combos&               combos) {
    if (units.size() > kMaxShaderCacheStages || combos.size() > kMaxShaderCacheMapEntries) {
        return None();
    }

    ShaderCacheByteWriter source;
    source.U32(static_cast<std::uint32_t>(units.size()));
    for (const auto& unit : units) {
        source.U32(static_cast<std::uint32_t>(unit.stage));
        if (! source.String(unit.src)) return None();
    }

    ShaderCacheByteWriter combo;
    combo.U32(static_cast<std::uint32_t>(combos.size()));
    for (const auto& [name, value] : combos) {
        if (! combo.String(name) || ! combo.String(value)) return None();
    }

    auto source_digest = HashShaderCacheBytes(
        std::span<const std::uint8_t>(source.bytes().data(), source.bytes().size()));
    auto combo_digest = HashShaderCacheBytes(
        std::span<const std::uint8_t>(combo.bytes().data(), combo.bytes().size()));
    if (source_digest.is_none() || combo_digest.is_none()) return None();

    ShaderCacheByteWriter key;
    key.String("owe.shader-cache.v3");
    key.U32(kShaderCacheAbiVersion);
    key.Bytes(std::span<const std::uint8_t>(source_digest->data(), source_digest->size()));
    key.Bytes(std::span<const std::uint8_t>(combo_digest->data(), combo_digest->size()));
    key.String("vulkan-1.1");
    key.String("hlsl");
    key.U32(0);

    const std::string key_hex    = utils::genSha1(std::span<const char>(
        reinterpret_cast<const char*>(key.bytes().data()), key.bytes().size()));
    auto              key_digest = DecodeShaderCacheDigest(key_hex);
    if (key_digest.is_none()) return None();
    return Some(ShaderCacheIdentity {
        .cache_key     = *key_digest,
        .source        = *source_digest,
        .combos        = *combo_digest,
        .cache_key_hex = key_hex,
    });
}

inline rstd::path::PathBuf GetCachePath(ref<rstd::path::Path> cache_dir, std::string_view scene_id,
                                        std::string_view filename) {
    auto path = rstd::path::PathBuf::from(cache_dir);
    path.push(ref<rstd::path::Path>(rstd::cppstd::as_str(scene_id).unwrap()));
    path.push(ref<rstd::path::Path>(rstd::cppstd::as_str(SHADER_DIR).unwrap()));
    const std::string cache_filename = std::string(filename) + "." SHADER_SUFFIX;
    path.push(ref<rstd::path::Path>(rstd::cppstd::as_str(cache_filename).unwrap()));
    return path;
}

bool WriteCacheMap(ShaderCacheByteWriter& writer, const Map<std::string, std::string>& values) {
    if (values.size() > kMaxShaderCacheMapEntries) return false;
    writer.U32(static_cast<std::uint32_t>(values.size()));
    for (const auto& [name, value] : values) {
        if (! writer.String(name) || ! writer.String(value)) return false;
    }
    return true;
}

bool ReadCacheMap(ShaderCacheByteReader& reader, Map<std::string, std::string>& values) {
    std::uint32_t count = 0;
    if (! reader.U32(count) || count > kMaxShaderCacheMapEntries) return false;
    values.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        std::string name;
        std::string value;
        if (! reader.String(name) || ! reader.String(value) ||
            ! values.emplace(std::move(name), std::move(value)).second) {
            return false;
        }
    }
    return true;
}

bool WriteCacheSlots(ShaderCacheByteWriter& writer, const Set<unsigned>& slots) {
    if (slots.size() > kMaxShaderCacheSlots) return false;
    writer.U32(static_cast<std::uint32_t>(slots.size()));
    for (const auto slot : slots) writer.U32(static_cast<std::uint32_t>(slot));
    return true;
}

bool ReadCacheSlots(ShaderCacheByteReader& reader, Set<unsigned>& slots) {
    std::uint32_t count = 0;
    if (! reader.U32(count) || count > kMaxShaderCacheSlots) return false;
    slots.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t slot = 0;
        if (! reader.U32(slot) || ! slots.insert(static_cast<unsigned>(slot)).second) return false;
    }
    return true;
}

struct ShaderCacheArtifact {
    std::vector<ShaderUnit> units;
    std::vector<ShaderCode> codes;
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
    std::string           reason;
};

class ShaderCacheArtifactCodec {
public:
    static Option<std::vector<std::uint8_t>> Encode(const ShaderCacheIdentity&  identity,
                                                    std::span<const ShaderUnit> units,
                                                    std::span<const ShaderCode> codes) {
        if (units.empty() || units.size() != codes.size() || units.size() > kMaxShaderCacheStages) {
            return None();
        }

        ShaderCacheByteWriter payload;
        for (std::size_t i = 0; i < units.size(); ++i) {
            if (static_cast<std::uint32_t>(units[i].stage) >
                static_cast<std::uint32_t>(ShaderType::FRAGMENT)) {
                return None();
            }
            ShaderCacheByteWriter record;
            record.U32(static_cast<std::uint32_t>(units[i].stage));
            if (! record.String(units[i].src) ||
                ! WriteCacheMap(record, units[i].preprocess_info.input) ||
                ! WriteCacheMap(record, units[i].preprocess_info.output) ||
                ! WriteCacheMap(record, units[i].preprocess_info.uniforms) ||
                ! WriteCacheSlots(record, units[i].preprocess_info.active_tex_slots) ||
                codes[i].size() > std::numeric_limits<std::uint32_t>::max() / 4) {
                return None();
            }

            record.U32(static_cast<std::uint32_t>(codes[i].size() * 4));
            for (const auto word : codes[i]) record.U32(word);
            if (record.bytes().size() > kMaxShaderCachePayloadSize - sizeof(std::uint32_t) ||
                payload.bytes().size() >
                    kMaxShaderCachePayloadSize - sizeof(std::uint32_t) - record.bytes().size()) {
                return None();
            }
            payload.U32(static_cast<std::uint32_t>(record.bytes().size()));
            if (! payload.Bytes(
                    std::span<const std::uint8_t>(record.bytes().data(), record.bytes().size()))) {
                return None();
            }
        }
        if (payload.bytes().size() > kMaxShaderCachePayloadSize) return None();

        auto payload_digest = HashShaderCacheBytes(
            std::span<const std::uint8_t>(payload.bytes().data(), payload.bytes().size()));
        if (payload_digest.is_none()) return None();

        ShaderCacheByteWriter artifact;
        artifact.Bytes(
            std::span<const std::uint8_t>(kShaderCacheMagic.data(), kShaderCacheMagic.size()));
        artifact.U32(kShaderCacheFormatVersion);
        artifact.U32(kShaderCacheAbiVersion);
        artifact.U32(kShaderCacheHeaderSize);
        artifact.U32(static_cast<std::uint32_t>(payload.bytes().size()));
        artifact.U32(static_cast<std::uint32_t>(units.size()));
        artifact.U32(0);
        artifact.Bytes(
            std::span<const std::uint8_t>(identity.cache_key.data(), identity.cache_key.size()));
        artifact.Bytes(
            std::span<const std::uint8_t>(identity.source.data(), identity.source.size()));
        artifact.Bytes(
            std::span<const std::uint8_t>(identity.combos.data(), identity.combos.size()));
        artifact.Bytes(
            std::span<const std::uint8_t>(payload_digest->data(), payload_digest->size()));
        if (artifact.bytes().size() != kShaderCacheHeaderSize ||
            ! artifact.Bytes(
                std::span<const std::uint8_t>(payload.bytes().data(), payload.bytes().size()))) {
            return None();
        }
        return Some(artifact.Take());
    }

    static ShaderCacheReadResult Decode(const ShaderCacheIdentity&    identity,
                                        std::span<const ShaderUnit>   expected_units,
                                        std::span<const std::uint8_t> bytes) {
        if (bytes.size() < kShaderCacheHeaderSize) return Invalid("truncated header");
        if (bytes.size() > kShaderCacheHeaderSize + kMaxShaderCachePayloadSize) {
            return Invalid("artifact exceeds size limit");
        }

        ShaderCacheByteReader         reader(bytes);
        std::span<const std::uint8_t> magic;
        std::uint32_t                 format_version = 0;
        std::uint32_t                 shader_abi     = 0;
        std::uint32_t                 header_size    = 0;
        std::uint32_t                 payload_size   = 0;
        std::uint32_t                 stage_count    = 0;
        std::uint32_t                 flags          = 0;
        if (! reader.Bytes(kShaderCacheMagic.size(), magic) ||
            ! std::equal(magic.begin(), magic.end(), kShaderCacheMagic.begin()) ||
            ! reader.U32(format_version) || ! reader.U32(shader_abi) || ! reader.U32(header_size) ||
            ! reader.U32(payload_size) || ! reader.U32(stage_count) || ! reader.U32(flags)) {
            return Invalid("invalid header");
        }
        if (format_version != kShaderCacheFormatVersion || shader_abi != kShaderCacheAbiVersion ||
            header_size != kShaderCacheHeaderSize || flags != 0) {
            return Invalid("unsupported format or ABI");
        }
        if (stage_count == 0 || stage_count > kMaxShaderCacheStages ||
            stage_count != expected_units.size()) {
            return Invalid("stage count mismatch");
        }

        ShaderCacheDigest cache_key {};
        ShaderCacheDigest source {};
        ShaderCacheDigest combos {};
        ShaderCacheDigest payload_digest {};
        if (! ReadDigest(reader, cache_key) || ! ReadDigest(reader, source) ||
            ! ReadDigest(reader, combos) || ! ReadDigest(reader, payload_digest)) {
            return Invalid("truncated identity");
        }
        if (cache_key != identity.cache_key || source != identity.source ||
            combos != identity.combos) {
            return Invalid("identity mismatch");
        }
        if (payload_size > kMaxShaderCachePayloadSize || payload_size != reader.remaining()) {
            return Invalid("payload size mismatch");
        }

        std::span<const std::uint8_t> payload;
        if (! reader.Bytes(payload_size, payload) || ! reader.done()) {
            return Invalid("truncated payload");
        }
        auto actual_payload_digest = HashShaderCacheBytes(payload);
        if (! actual_payload_digest || *actual_payload_digest != payload_digest) {
            return Invalid("payload digest mismatch");
        }

        ShaderCacheArtifact artifact;
        artifact.units.reserve(stage_count);
        artifact.codes.reserve(stage_count);
        ShaderCacheByteReader payload_reader(payload);
        for (std::uint32_t i = 0; i < stage_count; ++i) {
            std::uint32_t                 record_size = 0;
            std::span<const std::uint8_t> record_bytes;
            if (! payload_reader.U32(record_size) || record_size > payload_reader.remaining() ||
                ! payload_reader.Bytes(record_size, record_bytes)) {
                return Invalid("invalid stage record size");
            }

            ShaderCacheByteReader record(record_bytes);
            std::uint32_t         stage_value = 0;
            if (! record.U32(stage_value) ||
                stage_value > static_cast<std::uint32_t>(ShaderType::FRAGMENT)) {
                return Invalid("invalid shader stage");
            }
            const auto stage = static_cast<ShaderType>(stage_value);
            if (stage != expected_units[i].stage) return Invalid("shader stage mismatch");

            ShaderUnit unit { .stage = stage };
            if (! record.String(unit.src) || ! ReadCacheMap(record, unit.preprocess_info.input) ||
                ! ReadCacheMap(record, unit.preprocess_info.output) ||
                ! ReadCacheMap(record, unit.preprocess_info.uniforms) ||
                ! ReadCacheSlots(record, unit.preprocess_info.active_tex_slots)) {
                return Invalid("invalid shader metadata");
            }

            std::uint32_t spirv_size = 0;
            if (! record.U32(spirv_size) || spirv_size % 4 != 0 ||
                spirv_size > record.remaining()) {
                return Invalid("invalid SPIR-V size");
            }
            ShaderCode code;
            code.reserve(spirv_size / 4);
            for (std::uint32_t word = 0; word < spirv_size / 4; ++word) {
                std::uint32_t value = 0;
                if (! record.U32(value)) return Invalid("truncated SPIR-V");
                code.push_back(value);
            }
            if (! record.done()) return Invalid("unexpected stage data");
            artifact.units.push_back(std::move(unit));
            artifact.codes.push_back(std::move(code));
        }
        if (! payload_reader.done()) return Invalid("unexpected payload data");
        return ShaderCacheReadResult {
            .status   = ShaderCacheReadStatus::Hit,
            .artifact = std::move(artifact),
        };
    }

private:
    static bool ReadDigest(ShaderCacheByteReader& reader, ShaderCacheDigest& digest) {
        std::span<const std::uint8_t> bytes;
        if (! reader.Bytes(digest.size(), bytes)) return false;
        std::copy(bytes.begin(), bytes.end(), digest.begin());
        return true;
    }

    static ShaderCacheReadResult Invalid(std::string reason) {
        return ShaderCacheReadResult {
            .status = ShaderCacheReadStatus::Invalid,
            .reason = std::move(reason),
        };
    }
};

bool PublishShaderCacheArtifact(ref<rstd::path::Path> path, std::string_view cache_key,
                                std::span<const std::uint8_t> bytes) {
    auto parent = path.parent();
    if (parent.is_none()) return false;

    auto created = rstd::fs::create_dir_all(*parent);
    if (created.is_err()) {
        rstd_warn("cannot create shader cache directory '{}': {}",
                  *parent,
                  rstd::move(created).unwrap_err_unchecked());
        return false;
    }

    static std::atomic<std::uint64_t> temporary_sequence { 0 };
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        const std::string temporary_name =
            std::string(cache_key) + "." + std::to_string(rstd::process::id().to_primitive()) +
            "." + std::to_string(temporary_sequence.fetch_add(1, std::memory_order_relaxed)) +
            ".tmp";
        auto temporary_path = rstd::path::PathBuf::from(*parent);
        temporary_path.push(ref<rstd::path::Path>(rstd::cppstd::as_str(temporary_name).unwrap()));

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
            auto data = rstd::slice<u8>::from_raw_parts(reinterpret_cast<const byte*>(bytes.data()),
                                                        rstd::usize(bytes.size()));
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

Option<String> MakeShaderSourceCacheKey(const std::string&                source,
                                        const std::vector<ShaderTexInfo>& texinfos) {
    ShaderCacheByteWriter key;
    if (! key.String(source) || texinfos.size() > kMaxShaderCacheMapEntries) return None();
    key.U32(u32(texinfos.size()).to_primitive());
    for (const auto& texinfo : texinfos) {
        u32 bits { texinfo.enabled ? 1u : 0u };
        for (usize index {}; index < texinfo.composEnabled.len(); ++index) {
            if (texinfo.composEnabled[index]) bits |= u32(1u << (index.to_primitive() + 1));
        }
        key.U32(bits.to_primitive());
    }
    auto digest = utils::genSha1(std::span<const char>(
        reinterpret_cast<const char*>(key.bytes().data()), key.bytes().size()));
    return Some(String::make(rstd::cppstd::as_str(digest).unwrap()));
}

usize EstimateShaderAnnotations(const ShaderInfo& info) {
    usize bytes { sizeof(ShaderInfo) };
    auto  add_map = [&](const auto& values) {
        bytes += usize(values.size() * 128);
        for (const auto& [name, value] : values) bytes += usize(name.size() + value.size());
    };
    add_map(info.combos);
    add_map(info.alias);
    for (const auto& [name, value] : info.svs) {
        bytes += usize(name.size() + sizeof(value) + 96);
    }
    for (const auto& [slot, texture] : info.defTexs) {
        static_cast<void>(slot);
        bytes += usize(texture.size() + 64);
    }
    bytes += (info.combo_defs.len() + info.texture_uniforms.len() + info.scalar_uniforms.len()) *
             usize(512);
    bytes += info.shadow_pass.len();
    return bytes;
}

void MergeShaderAnnotations(ShaderInfo& target, const ShaderInfo& source) {
    for (const auto& [name, value] : source.combos) target.combos[name] = value;
    for (const auto& [name, value] : source.svs) target.svs[name] = value;
    for (const auto& [name, value] : source.alias) target.alias[name] = value;
    target.defTexs.insert(target.defTexs.end(), source.defTexs.begin(), source.defTexs.end());
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
    Combos                                  resolved = input_combos;
    Map<std::string, const wpscene::Combo*> definitions;
    Map<std::string, bool>                  active;

    for (const auto& combo : info.combo_defs) {
        auto name         = rstd::cppstd::to_string(combo.combo.as_str());
        definitions[name] = &combo;
        active[name]      = true;
        if (! resolved.contains(name)) {
            resolved[name] = std::to_string(combo.default_.to_primitive());
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [name, combo] : definitions) {
            if (! active[name]) continue;
            auto requirements = combo->require.iter();
            for (auto item = requirements.next(); item.is_some(); item = requirements.next()) {
                auto require_name_value = item->template get<0>();
                auto require_value      = item->template get<1>();
                auto require_name       = rstd::cppstd::to_string(require_name_value->as_str());
                auto value              = resolved.find(require_name);
                auto dependency         = active.find(require_name);
                if (value == resolved.end() ||
                    value->second != std::to_string(require_value->to_primitive()) ||
                    (dependency != active.end() && ! dependency->second)) {
                    active[name] = false;
                    changed      = true;
                    break;
                }
            }
        }
    }

    // Saved materials retain values for hidden editor controls. Compile hidden combos at their
    // declared defaults; leave a zero default undefined so shader-side aliases can define it.
    for (const auto& [name, combo] : definitions) {
        if (active[name]) continue;
        if (combo->default_ == i32()) {
            resolved.erase(name);
        } else {
            resolved[name] = std::to_string(combo->default_.to_primitive());
        }
    }
    return resolved;
}

std::string ShaderParser::PreShaderSrc(fs::VFS& vfs, const std::string& src,
                                       ShaderInfo*                       pShaderInfo,
                                       const std::vector<ShaderTexInfo>& texinfos,
                                       ShaderCache*                      cache) {
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
                return rstd::cppstd::to_string((*cached)->source.as_str());
            }
        }
    }

    std::string newsrc;
    newsrc.reserve(src.size());
    std::string all_includes;

    std::size_t            cursor = 0;
    auto                   source = rstd::cppstd::as_str(src).unwrap();
    shader_lex::LineWalker w(source);
    for (; ! w.Done(); w.Step()) {
        shader_lex::Cursor c(source);
        c.SeekTo(w.LineStart());
        if (! c.MatchHashDirective("include"_str)) continue;

        // Copy bytes up to this line, then splice in the recursively-expanded
        // include body. The newline after the directive stays as part of the
        // splice (we step the outer cursor to LineEnd).
        newsrc.append(src, cursor, w.LineStart().to_primitive() - cursor);
        std::string line =
            src.substr(w.LineStart().to_primitive(), (w.LineEnd() - w.LineStart()).to_primitive());
        auto include_line = String::make(rstd::cppstd::as_str(line).unwrap());
        include_line.push_ascii(u8('\n'));
        std::string expanded = LoadGlslInclude(vfs, include_line.as_str());
        newsrc.append(expanded);
        all_includes.append(expanded);
        cursor = w.LineEnd().to_primitive();
    }
    newsrc.append(src, cursor, std::string::npos);
    if (cache == nullptr) {
        ParseShader(all_includes, pShaderInfo, texinfos);
        ParseShader(newsrc, pShaderInfo, texinfos);
        return newsrc;
    }

    ShaderInfo annotations;
    ParseShader(all_includes, &annotations, texinfos);
    ParseShader(newsrc, &annotations, texinfos);
    MergeShaderAnnotations(*pShaderInfo, annotations);
    if (source_cache_key.is_some()) {
        auto bytes = usize(newsrc.size()) * usize(4) + EstimateShaderAnnotations(annotations) +
                     source_cache_key->len() * usize(2) + usize(128);
        if (cache->ReserveSource(bytes)) {
            auto order_key = source_cache_key->clone();
            cache->m_source_entries.insert(
                source_cache_key.take().unwrap_unchecked(),
                ShaderCache::SourceEntry {
                    .source      = String::make(rstd::cppstd::as_str(newsrc).unwrap()),
                    .annotations = rstd::move(annotations),
                    .bytes       = bytes,
                });
            cache->m_source_order.push(rstd::move(order_key));
            cache->m_source_bytes += bytes;
        }
    }
    return newsrc;
}

std::string ShaderParser::PreShaderHeader(const std::string& src, const Combos& combos,
                                          ShaderType type) {
    // Some workshop shaders contain full-width semicolons, which glslang rejects
    // while compiling the Vulkan shader source.
    auto compatible = ReplaceAll(src, "\xEF\xBC\x9B", ";");
    auto undefined  = UndefBeforeConflictingMacroDefines(rstd::cppstd::as_str(compatible).unwrap());
    auto normalized_audio = NormalizePackedAudioSpectrumAccess(undefined.as_str());
    auto normalized_mul   = NormalizeLeadingIntegerMulLiteral(normalized_audio.as_str());
    auto user_src         = rstd::cppstd::to_string(normalized_mul.as_str());

    // All stages route through glslang's HLSL frontend.
    std::string pre;
    if (type == ShaderType::GEOMETRY) {
        pre = pre_shader_code_gs_hlsl;
    } else {
        pre = pre_shader_code;
        const char* tail =
            (type == ShaderType::FRAGMENT) ? pre_shader_tail_frag : pre_shader_tail_vert;
        if (auto pos = pre.find("__SHADER_TAIL__"); pos != std::string::npos) {
            pre.replace(pos, std::string_view("__SHADER_TAIL__").size(), tail);
        }
    }

    // If user shader defines its own `mod(...)` at file scope, gate out the
    // prologue's mod overloads to avoid redefinition errors. Substring scan
    // is good enough — function decls always start with one of these tokens
    // followed by a space and `mod(`.
    static constexpr std::string_view kModSentinels[] = {
        "\nfloat mod(", "\nfloat2 mod(", "\nfloat3 mod(", "\nfloat4 mod(",
        "\nvec2 mod(",  "\nvec3 mod(",   "\nvec4 mod(",
    };
    bool user_mod = false;
    for (auto needle : kModSentinels) {
        if (user_src.find(needle) != std::string::npos ||
            (user_src.size() >= needle.size() - 1 &&
             std::string_view(user_src).substr(0, needle.size() - 1) == needle.substr(1))) {
            user_mod = true;
            break;
        }
    }
    if (user_mod) {
        // Inject #define ahead of the prologue text so the #ifndef guard
        // around our `mod` overloads sees it during glslang preprocess.
        pre = "#define WW_USER_MOD 1\n" + pre;
    }

    std::string combo_defines;
    for (const auto& c : combos) {
        std::string cup(c.first);
        std::transform(c.first.begin(), c.first.end(), cup.begin(), [](unsigned char value) {
            return static_cast<char>(std::toupper(value));
        });
        if (c.second.empty()) {
            rstd_error("combo '{}' can't be empty", cup);
            continue;
        }
        combo_defines += "#define " + cup + " " + c.second + "\n";
    }

    // Combo `#define`s land before __SHADER_PLACEHOLD__ so they're visible
    // throughout the user source during the DXC -P pass. The placeholder
    // slot itself is filled by Finalprocessor *after* preprocessing, so
    // the synthesized cbuffer always sees combo references already
    // expanded to literal numbers (e.g. `g_Bones[BONECOUNT]` → `[4]`).
    if (auto pos = pre.find(SHADER_PLACEHOLD); pos != std::string::npos) {
        pre.insert(pos, combo_defines);
    } else {
        pre += combo_defines;
    }
    return pre + user_src;
}

namespace
{

// Serialize one CompileToSpv invocation as a JSON object. Captures the
// raw post-PreShaderSrc state (includes resolved, prologue not yet
// applied, regex extraction not yet run) so a replay through the full
// pipeline exercises every transform downstream.
Json BuildShaderRecord(std::string_view scene_id, std::span<const ShaderUnit> units,
                       const ShaderInfo* shader_info, std::span<const ShaderTexInfo> texs) {
    auto stage_name = [](ShaderType s) -> const char* {
        switch (s) {
        case ShaderType::VERTEX: return "VERTEX";
        case ShaderType::FRAGMENT: return "FRAGMENT";
        case ShaderType::GEOMETRY: return "GEOMETRY";
        }
        return "UNKNOWN";
    };

    auto rec = rstd::json::Map::make();
    rec.insert(::alloc::string::String::make("scene_id"_str), JsonFromStd(scene_id));

    auto js_stages = rstd::json::Array::make();
    for (const auto& u : units) {
        auto stage = rstd::json::Map::make();
        stage.insert(::alloc::string::String::make("stage"_str), JsonFromStd(stage_name(u.stage)));
        stage.insert(::alloc::string::String::make("src"_str), JsonFromStd(u.src));
        js_stages.push(Json::Object(rstd::move(stage)));
    }
    rec.insert(::alloc::string::String::make("stages"_str), Json::Array(rstd::move(js_stages)));

    auto js_combos = rstd::json::Map::make();
    if (shader_info) {
        for (const auto& [k, v] : shader_info->combos)
            js_combos.insert(::alloc::string::String::make(rstd::cppstd::as_str(k).unwrap()),
                             JsonFromStd(v));
    }
    rec.insert(::alloc::string::String::make("combos"_str), Json::Object(rstd::move(js_combos)));

    auto js_texs = rstd::json::Array::make();
    for (const auto& t : texs) {
        auto compos = rstd::json::Array::make();
        for (bool enabled : t.composEnabled) compos.push(rstd::into<Json>(enabled));
        auto tex = rstd::json::Map::make();
        tex.insert(::alloc::string::String::make("enabled"_str),
                   rstd::into<Json>(bool { t.enabled }));
        tex.insert(::alloc::string::String::make("compos"_str), Json::Array(rstd::move(compos)));
        js_texs.push(Json::Object(rstd::move(tex)));
    }
    rec.insert(::alloc::string::String::make("tex_infos"_str), Json::Array(rstd::move(js_texs)));

    return Json::Object(rstd::move(rec));
}

// Appends one JSONL line to WP_SHADER_RECORD's path. O_APPEND is atomic
// for writes ≤ PIPE_BUF on Linux, which is more than enough for a single
// JSON line; concurrent recorders won't interleave.
void MaybeRecordCompile(std::string_view scene_id, std::span<const ShaderUnit> units,
                        const ShaderInfo* shader_info, std::span<const ShaderTexInfo> texs) {
    const char* path = std::getenv("WP_SHADER_RECORD");
    if (! path || path[0] == '\0') return;
    Json        rec  = BuildShaderRecord(scene_id, units, shader_info, texs);
    std::string line = Dump(rec);
    line.push_back('\n');
    if (FILE* f = std::fopen(path, "a")) {
        std::fwrite(line.data(), 1, line.size(), f);
        std::fclose(f);
    } else {
        rstd_warn("WP_SHADER_RECORD: cannot open '{}' for append", path);
    }
}

} // namespace

bool ShaderParser::CompileToSpv(std::string_view scene_id, std::span<ShaderUnit> units,
                                std::vector<ShaderCode>& codes, ShaderInfo* shader_info,
                                std::span<const ShaderTexInfo> texs, ShaderCache* cache) {
    MaybeRecordCompile(scene_id, units, shader_info, texs);

    auto make_compile_entry =
        [](std::span<const ShaderUnit> source_units, std::span<const ShaderCode> source_codes) {
            ShaderCache::CompileEntry entry;
            entry.stages.reserve(usize(source_units.size()));
            entry.codes.reserve(usize(source_codes.size()));
            entry.bytes = usize(sizeof(ShaderCache::CompileEntry));
            for (const auto& unit : source_units) {
                ShaderCache::CompiledStage stage { .stage = unit.stage };
                stage.uniforms.reserve(usize(unit.preprocess_info.uniforms.size()));
                for (const auto& [name, value] : unit.preprocess_info.uniforms) {
                    stage.uniforms.insert(String::make(rstd::cppstd::as_str(name).unwrap()),
                                          String::make(rstd::cppstd::as_str(value).unwrap()));
                }
                stage.active_tex_slots.reserve(usize(unit.preprocess_info.active_tex_slots.size()));
                for (const auto slot : unit.preprocess_info.active_tex_slots) {
                    stage.active_tex_slots.push(u32(slot));
                }
                entry.stages.push(rstd::move(stage));
                entry.bytes += usize(sizeof(ShaderCache::CompiledStage));
                for (const auto& [name, value] : unit.preprocess_info.uniforms) {
                    entry.bytes += usize(name.size() + value.size() + 128);
                }
                entry.bytes += usize(unit.preprocess_info.active_tex_slots.size() * 64);
            }
            for (const auto& code : source_codes) {
                auto cached_code = Vec<u32>::with_capacity(usize(code.size()));
                for (const auto word : code) cached_code.push(u32(word));
                entry.codes.push(rstd::move(cached_code));
                entry.bytes += usize(sizeof(Vec<u32>) + code.size() * sizeof(u32));
            }
            return entry;
        };

    auto apply_compile_entry = [&](const ShaderCache::CompileEntry& entry) {
        if (entry.stages.len() != usize(units.size()) || entry.codes.len() != usize(units.size())) {
            return false;
        }
        for (usize index {}; index < entry.stages.len(); ++index) {
            const auto& stage = entry.stages[index];
            auto&       unit  = units[index.to_primitive()];
            if (stage.stage != unit.stage) return false;
            unit.preprocess_info.uniforms.clear();
            auto uniform = stage.uniforms.iter();
            for (auto item = uniform.next(); item.is_some(); item = uniform.next()) {
                auto name  = item->template get<0>();
                auto value = item->template get<1>();
                unit.preprocess_info.uniforms.emplace(rstd::cppstd::to_string(name->as_str()),
                                                      rstd::cppstd::to_string(value->as_str()));
            }
            unit.preprocess_info.active_tex_slots.clear();
            for (const auto slot : stage.active_tex_slots) {
                unit.preprocess_info.active_tex_slots.insert(slot.to_primitive());
            }
        }
        codes.clear();
        codes.reserve(entry.codes.len().to_primitive());
        for (const auto& cached_code : entry.codes) {
            ShaderCode code;
            code.reserve(cached_code.len().to_primitive());
            for (const auto word : cached_code) code.push_back(word.to_primitive());
            codes.push_back(rstd::move(code));
        }
        ShapeShaderDefaults(units, *shader_info);
        return true;
    };

    auto store_compile_entry = [&](std::string_view key, ShaderCache::CompileEntry entry) {
        if (cache == nullptr) return;
        auto owned_key = String::make(rstd::cppstd::as_str(key).unwrap());
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
        cache_identity = MakeShaderCacheIdentity(units, shader_info->combos);
        if (cache_identity) {
            auto key    = rstd::cppstd::as_str(cache_identity->cache_key_hex).unwrap();
            auto memory = cache->m_compile_entries.get(key);
            if (memory.is_some()) {
                return apply_compile_entry(**memory);
            }

            auto cache_dir = cache->directory();
            if (cache_dir.is_some()) {
                cache_file_path =
                    Some(GetCachePath(*cache_dir, scene_id, cache_identity->cache_key_hex));
                auto                  cached = rstd::fs::read(cache_file_path->as_path());
                ShaderCacheReadResult decoded;
                if (cached.is_ok()) {
                    auto cached_bytes = rstd::move(cached).unwrap_unchecked();
                    decoded           = ShaderCacheArtifactCodec::Decode(
                        *cache_identity,
                        std::span<const ShaderUnit>(units.data(), units.size()),
                        std::span<const std::uint8_t>(
                            reinterpret_cast<const std::uint8_t*>(cached_bytes.data()),
                            cached_bytes.len().to_primitive()));
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
                    auto entry = make_compile_entry(
                        std::span<const ShaderUnit>(decoded.artifact.units.data(),
                                                    decoded.artifact.units.size()),
                        std::span<const ShaderCode>(decoded.artifact.codes.data(),
                                                    decoded.artifact.codes.size()));
                    if (! apply_compile_entry(entry)) return false;
                    store_compile_entry(cache_identity->cache_key_hex, rstd::move(entry));
                    return true;
                }
                if (decoded.status == ShaderCacheReadStatus::Invalid && ! decoded.reason.empty()) {
                    rstd_warn("shader cache '{}' is invalid ({}); recompiling",
                              cache_file_path->as_path(),
                              decoded.reason);
                }
            }
        } else {
            rstd_warn("shader cache identity exceeds format limits; compiling without cache");
        }
    }

    std::for_each(units.begin(), units.end(), [shader_info](auto& unit) {
        unit.src = Preprocessor(unit.src, unit.stage, shader_info->combos, unit.preprocess_info);
    });
    ShapeShaderDefaults(units, *shader_info);

    auto compile = [](std::span<ShaderUnit> units, std::vector<ShaderCode>& codes) {
        // Build the cross-stage interface before rewriting each source. Using
        // only adjacent sources misses uniforms that
        // lives on a non-adjacent stage (e.g. FS-only `g_Brightness` not seen
        // by VS in a 3-stage VS→GS→FS chain), which results in different UBO
        // sizes per stage and the runtime allocating a buffer too small for
        // the longest stage.
        auto uniform_interface = BuildUniformInterface(units);

        std::vector<vulkan::ShaderCompUnit> vunits(units.size());
        for (std::size_t i = 0; i < units.size(); i++) {
            auto&             unit     = units[i];
            auto&             vunit    = vunits[i];
            PreprocessorInfo* pre_info = i >= 1 ? &units[i - 1].preprocess_info : nullptr;
            PreprocessorInfo* post_info =
                i + 1 < units.size() ? &units[i + 1].preprocess_info : nullptr;

            const bool pack_varying_arrays =
                (unit.stage == ShaderType::VERTEX && i + 1 < units.size() &&
                 units[i + 1].stage == ShaderType::FRAGMENT) ||
                (unit.stage == ShaderType::FRAGMENT && i >= 1 &&
                 units[i - 1].stage == ShaderType::VERTEX);
            unit.src =
                Finalprocessor(unit, pre_info, post_info, &uniform_interface, pack_varying_arrays);

            vunit.src   = unit.src;
            vunit.stage = unit.stage;
            vunit.lang  = vulkan::SourceLang::Hlsl;
        }

        vulkan::ShaderCompOpt opt;
        opt.target   = vulkan::VulkanTarget::Vulkan_1_1;
        opt.optimize = false;

        std::vector<vulkan::Uni_ShaderSpv> spvs;
        spvs.reserve(units.size());

        if (! vulkan::CompileAndLinkShaderUnits(vunits, opt, spvs)) {
            return false;
        }

        codes.clear();
        for (auto& spv : spvs) {
            codes.emplace_back(std::move(spv->spirv));
        }
        return true;
    };

    if (! compile(units, codes)) return false;
    if (cache != nullptr && cache_identity) {
        store_compile_entry(
            cache_identity->cache_key_hex,
            make_compile_entry(units, std::span<const ShaderCode>(codes.data(), codes.size())));
    }
    if (cache_file_path.is_some() && cache_identity) {
        auto artifact = ShaderCacheArtifactCodec::Encode(
            *cache_identity,
            std::span<const ShaderUnit>(units.data(), units.size()),
            std::span<const ShaderCode>(codes.data(), codes.size()));
        if (! artifact) {
            rstd_warn("cannot encode shader cache artifact '{}'; continuing without cache",
                      cache_file_path->as_path());
        } else {
            PublishShaderCacheArtifact(
                cache_file_path->as_path(),
                cache_identity->cache_key_hex,
                std::span<const std::uint8_t>(artifact->data(), artifact->size()));
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

std::vector<SceneShaderDefaultTexture> ToSceneShaderDefaultTextures(const ShaderInfo& info) {
    std::vector<SceneShaderDefaultTexture> out;
    out.reserve(info.defTexs.size());
    for (const auto& [slot, texture] : info.defTexs) {
        out.push_back(SceneShaderDefaultTexture { .slot = slot, .texture = texture });
    }
    return out;
}

void MergeVariantFallbackMetadata(ShaderInfo& info, const SceneShaderVariantDesc& desc) {
    for (const auto& [key, value] : desc.uniform_aliases) {
        if (! info.alias.contains(key)) info.alias[key] = value;
    }
    for (const auto& [key, value] : desc.default_uniforms) {
        if (! info.svs.contains(key)) info.svs[key] = value;
    }
    for (const auto& texture : desc.default_textures) {
        auto found = std::find_if(info.defTexs.begin(), info.defTexs.end(), [&](const auto& item) {
            return item.first == texture.slot;
        });
        if (found == info.defTexs.end()) info.defTexs.push_back({ texture.slot, texture.texture });
    }
}

} // namespace

void ShaderParser::UpdateSceneShaderVariantDescFromCompiledUnits(
    SceneShaderVariantDesc& desc, std::span<const ShaderUnit> units,
    std::span<const ShaderCode> codes) {
    for (std::size_t i = 0; i < desc.stages.size() && i < units.size(); ++i) {
        desc.stages[i].active_texture_slots = units[i].preprocess_info.active_tex_slots;
        desc.stages[i].uniforms             = units[i].preprocess_info.uniforms;
        if (i < codes.size()) desc.stages[i].code_hash = SceneShaderStageCodeHash(codes[i]);
    }

    std::vector<vulkan::Uni_ShaderSpv> spvs;
    vulkan::ShaderReflected            reflected;
    if (! vulkan::GenReflect(codes, spvs, reflected)) return;

    desc.sampler_bindings.clear();
    constexpr std::string_view texture_prefix { "g_Texture" };
    for (const auto& [name, binding] : reflected.binding_map) {
        if (binding.layout.descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
            ! std::string_view(name).starts_with(texture_prefix)) {
            continue;
        }
        const auto suffix = std::string_view(name).substr(texture_prefix.size());
        if (suffix.empty() || (suffix.size() > 1 && suffix.front() == '0')) continue;
        std::size_t slot { 0 };
        const auto [end, error] =
            std::from_chars(suffix.data(), suffix.data() + suffix.size(), slot);
        if (error != std::errc() || end != suffix.data() + suffix.size()) continue;
        desc.sampler_bindings.push_back(SceneSamplerBinding {
            .texture_slot  = slot,
            .shader_member = name,
        });
    }
    std::sort(desc.sampler_bindings.begin(),
              desc.sampler_bindings.end(),
              [](const auto& lhs, const auto& rhs) {
                  return lhs.texture_slot < rhs.texture_slot;
              });

    struct BindingRecord {
        std::string name;
        uint32_t    set { 0 };
        uint32_t    binding { 0 };
        uint32_t    descriptor_type { 0 };
        uint32_t    descriptor_count { 0 };
        uint32_t    stage_flags { 0 };
    };
    struct UniformMemberRecord {
        std::string name;
        unsigned    offset { 0 };
        std::size_t size { 0 };
        std::size_t num { 0 };
    };
    struct UniformBlockRecord {
        std::string                      name;
        unsigned                         size { 0 };
        std::vector<UniformMemberRecord> members;
    };

    auto binding_less = [](const BindingRecord& lhs, const BindingRecord& rhs) {
        if (lhs.set != rhs.set) return lhs.set < rhs.set;
        if (lhs.binding != rhs.binding) return lhs.binding < rhs.binding;
        return lhs.name < rhs.name;
    };
    auto member_less = [](const UniformMemberRecord& lhs, const UniformMemberRecord& rhs) {
        if (lhs.offset != rhs.offset) return lhs.offset < rhs.offset;
        return lhs.name < rhs.name;
    };
    auto block_less = [](const UniformBlockRecord& lhs, const UniformBlockRecord& rhs) {
        return lhs.name < rhs.name;
    };

    std::vector<BindingRecord> bindings;
    bindings.reserve(reflected.binding_map.size());
    for (const auto& [name, binding] : reflected.binding_map) {
        bindings.push_back(BindingRecord {
            .name             = name,
            .set              = binding.set,
            .binding          = binding.layout.binding,
            .descriptor_type  = static_cast<uint32_t>(binding.layout.descriptorType),
            .descriptor_count = binding.layout.descriptorCount,
            .stage_flags      = binding.layout.stageFlags,
        });
    }
    std::sort(bindings.begin(), bindings.end(), binding_less);

    std::vector<UniformBlockRecord> blocks;
    blocks.reserve(reflected.blocks.size());
    for (const auto& block : reflected.blocks) {
        UniformBlockRecord record {
            .name = block.name,
            .size = block.size,
        };
        record.members.reserve(block.member_map.size());
        for (const auto& [name, member] : block.member_map) {
            record.members.push_back(UniformMemberRecord {
                .name   = name,
                .offset = member.offset,
                .size   = member.size.to_primitive(),
                .num    = member.num.to_primitive(),
            });
        }
        std::sort(record.members.begin(), record.members.end(), member_less);
        blocks.push_back(std::move(record));
    }
    std::sort(blocks.begin(), blocks.end(), block_less);

    std::size_t seed { 0 };
    utils::hash_combine(seed, bindings.size());
    for (const auto& binding : bindings) {
        utils::hash_combine(seed, binding.name);
        utils::hash_combine(seed, binding.set);
        utils::hash_combine(seed, binding.binding);
        utils::hash_combine(seed, binding.descriptor_type);
        utils::hash_combine(seed, binding.descriptor_count);
        utils::hash_combine(seed, binding.stage_flags);
    }
    utils::hash_combine(seed, blocks.size());
    for (const auto& block : blocks) {
        utils::hash_combine(seed, block.name);
        utils::hash_combine(seed, block.size);
        utils::hash_combine(seed, block.members.size());
        for (const auto& member : block.members) {
            utils::hash_combine(seed, member.name);
            utils::hash_combine(seed, member.offset);
            utils::hash_combine(seed, member.size);
            utils::hash_combine(seed, member.num);
        }
    }
    desc.descriptor_layout_hash = seed;

    desc.uniform_blocks.clear();
    bool canonical_abi = false;
    for (const auto& block : reflected.blocks) {
        std::size_t block_seed {};
        utils::hash_combine(block_seed, block.set);
        utils::hash_combine(block_seed, block.binding);
        utils::hash_combine(block_seed, block.name);
        utils::hash_combine(block_seed, block.size);
        for (const auto& [name, member] : block.member_map) {
            utils::hash_combine(block_seed, name);
            utils::hash_combine(block_seed, member.offset);
            utils::hash_combine(block_seed, member.size.to_primitive());
        }
        auto       shared_block = FindGlobalUniformBlock(rstd::cppstd::as_str(block.name).unwrap());
        const bool shared       = shared_block.is_some();
        canonical_abi           = canonical_abi || shared;
        desc.uniform_blocks.push_back(SceneShaderUniformBlockInterface {
            .name    = block.name,
            .set     = u32(block.set),
            .binding = u32(block.binding),
            .scope =
                shared ? SceneShaderUniformBlockScope::Shared : SceneShaderUniformBlockScope::Local,
            .identity = shared ? (**shared_block).identity : u64(block_seed),
        });
    }

    desc.descriptor_sets.clear();
    for (const auto& binding : bindings) {
        SceneShaderDescriptorSetInterface* target = nullptr;
        for (auto& set : desc.descriptor_sets) {
            if (set.set == u32(binding.set)) target = rstd::addressof(set);
        }
        if (target == nullptr) {
            desc.descriptor_sets.push_back(SceneShaderDescriptorSetInterface {
                .set = u32(binding.set),
                .push_descriptor =
                    ! canonical_abi || binding.set != kGlobalUniformSet.to_primitive(),
                .identity = canonical_abi && binding.set == kGlobalUniformSet.to_primitive()
                                ? kGlobalUniformSetIdentity
                                : u64(),
            });
            target = rstd::addressof(desc.descriptor_sets.back());
        }
        target->bindings.push_back(SceneShaderDescriptorBindingInterface {
            .name             = binding.name,
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
            desc.descriptor_sets.push_back(SceneShaderDescriptorSetInterface {
                .set             = kDrawUniformSet,
                .push_descriptor = true,
            });
        }
    }
    for (auto& set : desc.descriptor_sets) {
        if (set.identity != u64()) continue;
        std::size_t set_seed {};
        utils::hash_combine(set_seed, set.set.to_primitive());
        for (const auto& binding : set.bindings) {
            utils::hash_combine(set_seed, binding.name);
            utils::hash_combine(set_seed, binding.binding.to_primitive());
            utils::hash_combine(set_seed, binding.descriptor_type.to_primitive());
            utils::hash_combine(set_seed, binding.descriptor_count.to_primitive());
            utils::hash_combine(set_seed, binding.stage_flags.to_primitive());
        }
        set.identity = u64(set_seed);
    }
    std::sort(desc.descriptor_sets.begin(),
              desc.descriptor_sets.end(),
              [](const auto& lhs, const auto& rhs) {
                  return lhs.set < rhs.set;
              });
}

CompileSceneShaderVariantResult
ShaderParser::CompileSceneShaderVariant(const SceneShaderVariantDesc& desc, fs::VFS& vfs,
                                        const Combos& combos_override, ShaderCache* cache) {
    CompileSceneShaderVariantResult result;
    result.variant = desc;

    if (! desc.Valid()) {
        result.error = "invalid shader variant descriptor";
        return result;
    }

    result.tex_info.reserve(desc.texture_infos.size());
    for (const auto& texinfo : desc.texture_infos) {
        result.tex_info.push_back(ToShaderTexInfo(texinfo));
    }

    std::vector<ShaderUnit> units;
    units.reserve(desc.stages.size());
    bool has_geometry_stage = false;
    for (const auto& stage : desc.stages) {
        if (stage.source.empty()) {
            result.error = "shader variant stage source is empty";
            return result;
        }
        has_geometry_stage = has_geometry_stage || stage.stage == ShaderType::GEOMETRY;
        units.push_back(ShaderUnit {
            .stage           = stage.stage,
            .src             = stage.source,
            .preprocess_info = {},
        });
    }

    for (auto& unit : units) {
        unit.src = ShaderParser::PreShaderSrc(vfs, unit.src, &result.info, result.tex_info, cache);
    }

    Combos input_combos = result.info.combos;
    for (const auto& [key, value] : desc.resolved_combos) input_combos[key] = value;
    for (const auto& [key, value] : desc.input_combos) input_combos[key] = value;
    for (const auto& [key, value] : combos_override) {
        input_combos[key] = value;
    }
    if (has_geometry_stage && ! input_combos.contains(rstd::cppstd::to_string(WE_CB_GS_ENABLED))) {
        input_combos[rstd::cppstd::to_string(WE_CB_GS_ENABLED)] = "1";
    }
    result.info.combos          = ResolveShaderCombos(result.info, input_combos);
    result.variant.input_combos = rstd::move(input_combos);
    MergeVariantFallbackMetadata(result.info, desc);

    result.variant.resolved_combos         = result.info.combos;
    result.variant.uniform_aliases         = result.info.alias;
    result.variant.default_uniforms        = result.info.svs;
    result.variant.default_textures        = ToSceneShaderDefaultTextures(result.info);
    result.variant.geometry_shader_enabled = has_geometry_stage;
    result.variant.texture_infos.clear();
    result.variant.texture_infos.reserve(result.tex_info.size());
    for (const auto& texinfo : result.tex_info) {
        result.variant.texture_infos.push_back(ToSceneShaderTextureCompileInfo(texinfo));
    }

    std::vector<ShaderCode> spvs;
    const bool              ok = CompileToSpv(desc.scene_id,
                                              std::span<ShaderUnit>(units.data(), units.size()),
                                              spvs,
                                              &result.info,
                                              result.tex_info,
                                              cache);
    if (! ok) {
        result.error = "CompileToSpv failed";
        return result;
    }
    result.variant.default_uniforms = result.info.svs;
    ShaderParser::UpdateSceneShaderVariantDescFromCompiledUnits(result.variant, units, spvs);

    auto shader               = std::make_shared<SceneShader>();
    shader->name              = desc.shader_name;
    shader->matrix_convention = ShaderMatrixConvention::RowVector;
    shader->matrix_abi        = ShaderMatrixAbi::Hlsl;
    shader->codes             = std::move(spvs);
    shader->sampler_bindings  = result.variant.sampler_bindings;
    shader->uniform_blocks    = result.variant.uniform_blocks;
    shader->descriptor_sets   = result.variant.descriptor_sets;
    shader->default_uniforms  = result.info.svs;
    result.shader             = std::move(shader);
    result.ok                 = true;
    return result;
}

CompileMaterialShaderResult ShaderParser::CompileMaterialShader(const Json&      material_json,
                                                                fs::VFS&         vfs,
                                                                std::string_view scene_id,
                                                                const Combos&    combos_override,
                                                                ShaderCache*     cache) {
    CompileMaterialShaderResult r;

    wpscene::Material mat;
    if (! mat.FromJson(material_json)) {
        r.error = "Material::FromJson failed";
        return r;
    }
    r.shader_name = mat.shader;

    if (mat.shader.empty()) {
        r.error = "material has no shader name";
        return r;
    }

    const std::string shader_path = "/assets/shaders/" + mat.shader;
    auto              vert_source = fs::ReadFileContent(vfs, shader_path + ".vert");
    auto              frag_source = fs::ReadFileContent(vfs, shader_path + ".frag");
    if (vert_source.is_err() || frag_source.is_err()) {
        r.error = "shader source missing: " + shader_path + ".{vert,frag}";
        return r;
    }
    std::string vert_src = rstd::move(vert_source).unwrap_unchecked();
    std::string frag_src = rstd::move(frag_source).unwrap_unchecked();
    std::string geom_src;
    if (mat.shader == "genericparticle" || mat.shader == "genericropeparticle") {
        auto geom_source = fs::ReadFileContent(vfs, shader_path + ".geom");
        if (geom_source.is_err()) {
            r.error = "shader source missing: " + shader_path + ".geom";
            return r;
        }
        geom_src = rstd::move(geom_source).unwrap_unchecked();
    }
    if (vert_src.empty() || frag_src.empty()) {
        r.error = "shader source missing: " + shader_path + ".{vert,frag}";
        return r;
    }

    // Texture info: enabled flag from non-empty material.textures.
    // Component flags normally come from each .tex header. Skipping the
    // header parse keeps this entry path lightweight; sprite-sheet /
    // packed-channel materials may accordingly compile a different variant
    // than the production path.
    r.tex_info.reserve(mat.textures.size());
    for (const auto& t : mat.textures) {
        r.tex_info.push_back({ ! t.empty() });
    }

    std::vector<ShaderUnit> units;
    units.push_back({ ShaderType::VERTEX, std::move(vert_src), {} });
    if (! geom_src.empty()) {
        units.push_back({ ShaderType::GEOMETRY, std::move(geom_src), {} });
        r.info.combos[rstd::cppstd::to_string(WE_CB_GS_ENABLED)] = "1";
    }
    units.push_back({ ShaderType::FRAGMENT, std::move(frag_src), {} });

    for (auto& u : units) {
        u.src = ShaderParser::PreShaderSrc(vfs, u.src, &r.info, r.tex_info, cache);
    }

    Combos input_combos = r.info.combos;
    for (const auto& kv : mat.combos) {
        input_combos[kv.first] = std::to_string(kv.second.to_primitive());
    }
    for (const auto& kv : combos_override) input_combos[kv.first] = kv.second;
    if (! input_combos.contains(rstd::cppstd::to_string(WE_CB_BLENDMODE)))
        input_combos[rstd::cppstd::to_string(WE_CB_BLENDMODE)] = "0";
    if (! input_combos.contains(rstd::cppstd::to_string(WE_CB_BONECOUNT)))
        input_combos[rstd::cppstd::to_string(WE_CB_BONECOUNT)] = "1";
    r.info.combos = ResolveShaderCombos(r.info, input_combos);

    const bool ok = ShaderParser::CompileToSpv(scene_id,
                                               std::span<ShaderUnit>(units.data(), units.size()),
                                               r.spvs,
                                               &r.info,
                                               r.tex_info,
                                               cache);
    r.ok          = ok;
    if (! ok) {
        r.error = "CompileToSpv failed";
        return r;
    }
    SceneShaderVariantDesc variant;
    ShaderParser::UpdateSceneShaderVariantDescFromCompiledUnits(variant, units, r.spvs);
    r.uniform_blocks  = rstd::move(variant.uniform_blocks);
    r.descriptor_sets = rstd::move(variant.descriptor_sets);
    return r;
}
