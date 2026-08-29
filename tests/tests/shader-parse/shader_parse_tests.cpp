// Unit coverage for the WE shader annotation collector
// (ShaderParser_Pegtl.cpp). The collector is `#if`-agnostic by design —
// dead-branch GLSL stripping is glslang's job downstream. These tests assert
// the collector grabs annotations unconditionally and the comment/keyword
// handling resists obvious false positives.

#include <rstd/test/gtest.hpp>
#include <spirv_reflect.h>

#include <array>
#include <filesystem>
#include <fstream>

import rstd.cppstd;
import rstd;
import wescene.fs;
import wescene.pkg.parse;
import wescene.scene;
import wescene.shader_compile;
import wescene.types;

using owe::ParseShader;
using owe::ShaderInfo;
using owe::ShaderTexInfo;
using namespace rstd::literals;
using namespace rstd::prelude;

namespace
{

ShaderInfo Parse(const std::string& src, std::size_t n_tex_slots = 8) {
    ShaderInfo                 info {};
    std::vector<ShaderTexInfo> texs(n_tex_slots);
    for (auto& t : texs) t.enabled = true;
    ParseShader(src, &info, texs);
    return info;
}

} // namespace

// --- annotation collection: unconditional ----------------------------------

TEST(ShaderParser, TextureDefaultCollectedRegardlessOfIfdef) {
    // Mirrors `pulse.frag` / `genericimage2.frag` shape: the texture uniform
    // sits inside `#if SOME_COMBO`, and SOME_COMBO is itself derived from
    // the uniform's own `combo:` annotation. Collector must not gate on #if.
    const std::string src  = R"(
#if LIGHTS_SHADOW_MAPPING
uniform sampler2D g_Texture6; // {"hidden":true,"default":"_rt_shadowAtlas"}
#endif
void main(){}
)";
    auto              info = Parse(src);
    ASSERT_EQ(info.defTexs.size(), 1u);
    EXPECT_EQ(info.defTexs[0].first, i32(6));
    EXPECT_EQ(info.defTexs[0].second, "_rt_shadowAtlas");
}

TEST(ShaderParser, ShadowPassAnnotationSelectsCasterShader) {
    auto info = Parse(R"(
// [PASS] shadow shadowcasterfoliage4
void main(){}
)");

    EXPECT_EQ(info.shadow_pass, "shadowcasterfoliage4"_str);
}

TEST(ShaderParser, TextureComboFlagSetRegardlessOfIfdef) {
    // The combo flag on a texture binding is the chicken in the
    // chicken-and-egg. With 8 slots all enabled, slot 2 is bound, so MASK=1.
    const std::string src  = R"(
#if MASK == 1
uniform sampler2D g_Texture2; // {"material":"mask","combo":"MASK","default":"util/white"}
#endif
void main(){}
)";
    auto              info = Parse(src);
    EXPECT_EQ(info.combos.at("MASK"), "1");
}

TEST(ShaderParser, FourthPackedTextureComponentSetsItsCombo) {
    const std::string          src = R"(
uniform sampler2D g_Texture2; // {"components":[{"combo":"METALLIC_MAP"},{"combo":"ROUGHNESS_MAP"},{"combo":"REFLECTION_MAP"},{"combo":"EMISSIVE_MAP"}]}
void main(){}
)";
    ShaderInfo                 info {};
    std::vector<ShaderTexInfo> texs(3);
    texs[2] = ShaderTexInfo {
        .enabled       = true,
        .composEnabled = { false, false, false, true },
    };

    ParseShader(src, &info, texs);

    EXPECT_FALSE(info.combos.contains("METALLIC_MAP"));
    EXPECT_FALSE(info.combos.contains("ROUGHNESS_MAP"));
    EXPECT_FALSE(info.combos.contains("REFLECTION_MAP"));
    EXPECT_EQ(info.combos.at("EMISSIVE_MAP"), "1");
}

TEST(ShaderParser, ComboLineCollectedRegardlessOfIfdef) {
    const std::string src  = R"(
#if 0
// [COMBO] {"combo":"NEVER","default":1}
#endif
void main(){}
)";
    auto              info = Parse(src);
    EXPECT_EQ(info.combos.at("NEVER"), "1");
}

// --- comment / false-positive handling -------------------------------------

TEST(ShaderParser, LineCommentUniformNotCollected) {
    // A line that begins with `//` is a comment, even if it contains the
    // word `uniform`. Legacy substring scanner false-positived this.
    const std::string src  = R"(
// uniform vec4 g_Foo; // {"default":42}
void main(){}
)";
    auto              info = Parse(src);
    EXPECT_EQ(info.svs.count("g_Foo"), 0u);
}

TEST(ShaderParser, BlockCommentUniformNotCollected) {
    const std::string src  = R"(
/*
uniform vec4 g_Y; // {"default":1}
*/
void main(){}
)";
    auto              info = Parse(src);
    EXPECT_EQ(info.svs.count("g_Y"), 0u);
}

TEST(ShaderParser, BlockCommentDoesNotEatLaterUniforms) {
    // Make sure the block-comment pre-strip terminates at the closing `*/`
    // and lets real declarations through.
    const std::string src  = R"(
/* unused */
uniform float g_Real; // {"default":2.5}
void main(){}
)";
    auto              info = Parse(src);
    EXPECT_EQ(info.svs.count("g_Real"), 1u);
}

// --- combo + uniform schema -------------------------------------------------

TEST(ShaderParser, ComboDefaultIsRecorded) {
    const std::string src  = R"(
// [COMBO] {"combo":"BLENDMODE","default":9}
void main(){}
)";
    auto              info = Parse(src);
    EXPECT_EQ(info.combos.at("BLENDMODE"), "9");
}

TEST(ShaderParser, ComboMaterialKeyIsRecorded) {
    const std::string src  = R"(
// [COMBO] {"material":"toggle","combo":"USE_FEATURE","default":1}
void main(){}
)";
    auto              info = Parse(src);
    ASSERT_EQ(info.combo_defs.len(), usize(1));
    EXPECT_EQ(info.combo_defs[usize()].material, "toggle"_str);
    EXPECT_EQ(info.combo_defs[usize()].combo, "USE_FEATURE"_str);
}

TEST(ShaderParser, ComboOptionsUseOwnedAnnotationKeys) {
    const std::string src  = R"(
// [COMBO] {"material":"quality","combo":"QUALITY","type":"options","options":{"Low":0,"High":2}}
void main(){}
)";
    auto              info = Parse(src);
    ASSERT_EQ(info.combo_defs.len(), usize(1));
    auto high = info.combo_defs[usize()].options.get("High"_str);
    ASSERT_TRUE(high.is_some());
    EXPECT_EQ(**high, i32(2));
}

TEST(ShaderParser, ScalarDefaultPushedToSvs) {
    const std::string src  = R"(
uniform float g_Brightness; // {"material":"brightness","default":1.5,"range":[0,10]}
void main(){}
)";
    auto              info = Parse(src);
    ASSERT_EQ(info.svs.count("g_Brightness"), 1u);
    EXPECT_EQ(info.alias.at("brightness"), "g_Brightness");
}

TEST(ShaderParser, ScalarAnnotationAcceptsLeadingZeroRangeNumber) {
    const std::string src  = R"(
uniform float u_userSpeed; // {"material":"Speed","default":1,"range":[0,01]}
void main(){}
)";
    auto              info = Parse(src);
    ASSERT_EQ(info.svs.count("u_userSpeed"), 1u);
    EXPECT_EQ(info.alias.at("Speed"), "u_userSpeed");
}

TEST(ShaderParser, TextureAliasRecorded) {
    const std::string src  = R"(
uniform sampler2D g_Texture0; // {"material":"albedo","label":"Albedo","default":"util/white"}
void main(){}
)";
    auto              info = Parse(src);
    ASSERT_EQ(info.defTexs.size(), 1u);
    EXPECT_EQ(info.defTexs[0].first, i32());
    EXPECT_EQ(info.defTexs[0].second, "util/white");
    EXPECT_EQ(info.alias.at("albedo"), "g_Texture0");
}

TEST(ShaderParser, TextureBoundIfSlotEnabled) {
    // Slot 0 in texinfos is enabled by default in Parse(); the texture-side
    // combo flag therefore reads "1".
    const std::string src  = R"(
uniform sampler2D g_Texture0; // {"combo":"HASTEX","default":"util/white"}
void main(){}
)";
    auto              info = Parse(src);
    EXPECT_EQ(info.combos.at("HASTEX"), "1");
}

TEST(ShaderParser, UndefsBuiltinMacroBeforeUserRedefine) {
    const std::string out = owe::ShaderParser::PreShaderHeader(
        "#define M_PI_2 1.57079632679\nfloat f() { return M_PI_2; }\n",
        {},
        owe::ShaderType::FRAGMENT);

    const auto undef_pos  = out.find("#undef M_PI_2");
    const auto define_pos = out.find("#define M_PI_2 1.57079632679");
    ASSERT_NE(undef_pos, std::string::npos);
    ASSERT_NE(define_pos, std::string::npos);
    EXPECT_LT(undef_pos, define_pos);
}

TEST(ShaderParser, UndefsDerivativeAliasesBeforeUserRedefine) {
    const std::string out = owe::ShaderParser::PreShaderHeader(
        R"(
#if HLSL
#define dFdx ddx
#define dFdy ddy
#define textureGrad(s, uv, dx, dy) texSample2DGrad(s, uv, dx, dy)
#endif
void main() {}
)",
        {},
        owe::ShaderType::FRAGMENT);

    const auto dfdx_undef  = out.find("#undef dFdx");
    const auto dfdx_define = out.rfind("#define dFdx ddx");
    const auto dfdy_undef  = out.find("#undef dFdy");
    const auto dfdy_define = out.rfind("#define dFdy ddy");
    ASSERT_NE(dfdx_undef, std::string::npos);
    ASSERT_NE(dfdx_define, std::string::npos);
    ASSERT_NE(dfdy_undef, std::string::npos);
    ASSERT_NE(dfdy_define, std::string::npos);
    EXPECT_LT(dfdx_undef, dfdx_define);
    EXPECT_LT(dfdy_undef, dfdy_define);
    EXPECT_EQ(out.find("#undef textureGrad"), std::string::npos);
}

TEST(ShaderParser, CompileSceneShaderVariantAcceptsBothAtanForms) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "atan-overloads-test";
    desc.shader_name = "atan-overloads-test";
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/atan-overloads-test.vert",
        .source     = R"(
attribute vec3 a_Position;
varying vec2 v_TexCoord;
void main() {
    v_TexCoord = a_Position.xy;
    gl_Position = vec4(a_Position, 1.0);
}
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/atan-overloads-test.frag",
        .source     = R"(
varying vec2 v_TexCoord;
void main() {
    float one = atan(v_TexCoord.y / v_TexCoord.x);
    float two = atan(v_TexCoord.y, v_TexCoord.x);
    gl_FragColor = vec4(one, two, 0.0, 1.0);
}
)",
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.shader);
}

TEST(ShaderParser, PreShaderHeaderFlattensPackedAudioSpectrumAccess) {
    const std::string out = owe::ShaderParser::PreShaderHeader(
        R"(
uniform float g_AudioSpectrum64Left[64];
float sample(float barID) {
    return g_AudioSpectrum64Left[barID / 4][barID % 4];
}
)",
        {},
        owe::ShaderType::FRAGMENT);

    EXPECT_EQ(out.find("g_AudioSpectrum64Left[barID / 4][barID % 4]"), std::string::npos);
    EXPECT_NE(out.find("g_AudioSpectrum64Left[(int)(barID)]"), std::string::npos);
}

TEST(ShaderParser, PreShaderHeaderNormalizesFullwidthSemicolon) {
    const std::string out = owe::ShaderParser::PreShaderHeader(
        "void main() { gl_FragColor = vec4(1.0)； }\n", {}, owe::ShaderType::FRAGMENT);

    EXPECT_EQ(out.find("；"), std::string::npos);
    EXPECT_NE(out.find("gl_FragColor = vec4(1.0);"), std::string::npos);
}

TEST(ShaderParser, PreShaderHeaderPreservesLocalMatrixConstructorMul) {
    const std::string out = owe::ShaderParser::PreShaderHeader(
        R"(
vec2 rotate(vec2 uv, float th) {
    return mul(uv, mat2(cos(th), sin(th), -sin(th), cos(th)));
}
)",
        {},
        owe::ShaderType::FRAGMENT);

    EXPECT_NE(out.find("mul(uv, mat2(cos(th), sin(th), -sin(th), cos(th)))"), std::string::npos);
    EXPECT_EQ(out.find("transpose(float2x2"), std::string::npos);
    EXPECT_EQ(out.find("_ww_mul"), std::string::npos);
}

TEST(ShaderParser, PreShaderHeaderPreservesNestedMatrixConstructors) {
    const std::string out = owe::ShaderParser::PreShaderHeader(
        R"(
mat3 rotate(vec3 c, vec3 s) {
    return mul(mul(mat3(c.z, -s.z, 0, s.z, c.z, 0, 0, 0, 1),
                   mat3(1, 0, 0, 0, c.x, -s.x, 0, s.x, c.x)),
               mat3(c.y, 0, s.y, 0, 1, 0, -s.y, 0, c.y));
}
)",
        {},
        owe::ShaderType::GEOMETRY);

    EXPECT_NE(out.find("mul(mul(mat3(c.z, -s.z"), std::string::npos);
    EXPECT_EQ(out.find("transpose(float3x3"), std::string::npos);
}

TEST(ShaderParser, CompileSceneShaderVariantAcceptsLeadingIntegerScalarMul) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "integer-scalar-mul-test";
    desc.shader_name = "integer-scalar-mul-test";
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/integer-scalar-mul-test.vert",
        .source     = R"(
attribute vec3 a_Position;
void main() {
    float value = 2.0;
    float scaled = mul(1, value / 50.0);
    gl_Position = vec4(a_Position + vec3(scaled * 0.0), 1.0);
}
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/integer-scalar-mul-test.frag",
        .source     = R"(
void main() {
    gl_FragColor = vec4(1.0);
}
)",
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.shader);
    ASSERT_EQ(result.shader->codes.size(), 2u);
}

TEST(ShaderParser, CompileSceneShaderVariantKeepsGlobalVariablesPrivate) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "private-global-test";
    desc.shader_name = "private-global-test";
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/private-global-test.vert",
        .source     = R"(
attribute vec3 a_Position;
void main() {
    gl_Position = vec4(a_Position, 1.0);
}
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/private-global-test.frag",
        .source     = R"(
float tint;
void main() {
    tint = 0.25;
    gl_FragColor = vec4(tint, tint, tint, 1.0);
}
)",
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.shader);
    for (const auto& block : result.shader->uniform_blocks) {
        EXPECT_NE(block.name, "$Global");
    }
}

TEST(ShaderParser, PreShaderHeaderUsesHlslRectangularMatrixDimensions) {
    const std::string out = owe::ShaderParser::PreShaderHeader(
        R"(
mat2x3 glslMatrix() {
    return mat2x3(1, 2, 3, 4, 5, 6);
}
float3x2 hlslMatrix() {
    return float3x2(1, 2, 3, 4, 5, 6);
}
)",
        {},
        owe::ShaderType::VERTEX);

    EXPECT_NE(out.find("#define mat2x3 float2x3"), std::string::npos);
    EXPECT_NE(out.find("#define mat4x3 float4x3"), std::string::npos);
    EXPECT_NE(out.find("return mat2x3(1, 2, 3, 4, 5, 6);"), std::string::npos);
    EXPECT_NE(out.find("return float3x2(1, 2, 3, 4, 5, 6);"), std::string::npos);
}

TEST(ShaderParser, CommonPerspectiveIncludePreservesMatrixSource) {
    auto root = std::filesystem::temp_directory_path() /
                ("owe-wpshader-" + std::to_string(rstd::process::id().to_primitive()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "shaders");

    {
        std::ofstream out(root / "shaders" / "common_perspective.h");
        out << R"(
mat3 squareToQuad(vec2 p0, vec2 p1, vec2 p2, vec2 p3) {
	mat3 m = mat3(1.0);
	if (p0.x == p1.x) {
		return m;
	}
	return m;
}
)";
    }

    owe::fs::VFS vfs;
    auto         physical = owe::fs::make_physical_fs(owe::fs::ToPath(root.string()));
    ASSERT_TRUE(physical.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(physical).unwrap_unchecked()).is_ok());

    const std::string out = owe::ShaderParser::PreShaderSrc(
        vfs, "#include \"common_perspective.h\"\nvoid main(){}\n", nullptr, {});

    EXPECT_EQ(out.find("_ww_perspective_mat"), std::string::npos);
    EXPECT_NE(out.find("return m;"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(ShaderParser, CompileSceneShaderVariantRejectsInvalidDescriptor) {
    owe::fs::VFS vfs;

    const auto result = owe::ShaderParser::CompileSceneShaderVariant({}, vfs);

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.shader);
    EXPECT_FALSE(result.error.empty());
}

TEST(ShaderParser, LightingRequirementInjectsSceneLightInterface) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id                        = "lighting-v1-interface-test";
    desc.shader_name                     = "lighting-v1-interface-test";
    desc.input_combos["SCENE_ORTHO"]     = "1";
    desc.input_combos["OWE_IMAGE_LAYER"] = "1";
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/lighting-v1-interface-test.vert",
        .source     = R"(
attribute vec3 a_Position;
varying vec3 v_WorldPos;
void main() {
    v_WorldPos = a_Position;
    gl_Position = vec4(a_Position, 1.0);
}
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/lighting-v1-interface-test.frag",
        .source     = R"(
varying vec3 v_WorldPos;
vec3 ComputePBRLightShadow(vec3 N, vec3 L, vec3 V, vec3 albedo, vec3 lightColor,
    float radius, float exponent, vec3 specularTint, vec3 baseReflectance,
    float roughness, float metallic, float shadowFactor) {
    return lightColor;
}
vec3 ComputePBRLightShadowInfinite(vec3 N, vec3 L, vec3 V, vec3 albedo,
    vec3 lightColor, vec3 specularTint, vec3 baseReflectance, float roughness,
    float metallic, float shadowFactor) {
    return lightColor;
}
#require LightingV1
void main() {
    vec3 light = PerformLighting_V1(v_WorldPos, vec3(1.0), vec3(0.0, 1.0, 0.0),
        vec3(0.0, 0.0, 1.0), vec3(1.0), vec3(0.04), 0.5, 0.0);
    gl_FragColor = vec4(light, 1.0);
}
)",
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.shader);
    std::vector<owe::vulkan::Uni_ShaderSpv> spvs;
    owe::vulkan::ShaderReflected            reflected;
    ASSERT_TRUE(owe::vulkan::GenReflect(result.shader->codes, spvs, reflected));
    ASSERT_EQ(reflected.blocks.size(), 1u);
    EXPECT_EQ(reflected.blocks.front().name, "ww_LightingUniforms");
    EXPECT_EQ(reflected.blocks.front().set, 0u);
    EXPECT_EQ(reflected.blocks.front().binding, 2u);
    EXPECT_EQ(reflected.blocks.front().size, owe::kLightingUniformBlockSize.to_primitive());
    const auto& members = reflected.blocks.front().member_map;
    EXPECT_TRUE(members.contains("g_LightsPosition"));
    EXPECT_TRUE(members.contains("g_LightsColorRadius"));
    EXPECT_TRUE(members.contains("g_LightsDirectionType"));
    EXPECT_TRUE(members.contains("g_LightsConeExponent"));
    EXPECT_TRUE(members.contains("g_LightsCastShadow"));
}

TEST(ShaderParser, CompileSceneShaderVariantAcceptsPackedAudioSpectrumAccess) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "packed-audio-spectrum-test";
    desc.shader_name = "packed-audio-spectrum-test";
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/packed-audio-spectrum-test.vert",
        .source     = R"(
attribute vec3 a_Position;
varying vec2 v_TexCoord;
void main() {
    v_TexCoord = a_Position.xy;
    gl_Position = vec4(a_Position, 1.0);
}
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/packed-audio-spectrum-test.frag",
        .source     = R"(
varying vec2 v_TexCoord;
uniform float g_AudioSpectrum64Left[64];
void main() {
    float barID = v_TexCoord.x * 8.0;
    float value = g_AudioSpectrum64Left[barID / 4][barID % 4];
    gl_FragColor = vec4(value, value, value, 1.0);
}
)",
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.shader);
    ASSERT_EQ(result.shader->codes.size(), 2u);
    EXPECT_FALSE(result.shader->codes[1].empty());

    std::vector<owe::vulkan::Uni_ShaderSpv> spvs;
    owe::vulkan::ShaderReflected            reflected;
    ASSERT_TRUE(owe::vulkan::GenReflect(result.shader->codes, spvs, reflected));
    ASSERT_EQ(reflected.blocks.size(), 1u);
    EXPECT_EQ(reflected.blocks.front().name, "ww_AudioUniforms");
    EXPECT_EQ(reflected.blocks.front().set, 0u);
    EXPECT_EQ(reflected.blocks.front().binding, 1u);
    EXPECT_EQ(reflected.blocks.front().size, owe::kAudioUniformBlockSize.to_primitive());
    const auto& members = reflected.blocks.front().member_map;
    ASSERT_TRUE(members.contains("g_AudioSpectrum64Left"));
    const auto& spectrum = members.at("g_AudioSpectrum64Left");
    EXPECT_EQ(spectrum.scalar_kind, owe::ShaderScalarKind::Float);
    EXPECT_EQ(spectrum.scalar_width, 32u);
    EXPECT_EQ(spectrum.vector_components, 1u);
    EXPECT_EQ(spectrum.num, rstd::usize(64));
    EXPECT_EQ(spectrum.array_stride, 16u);
}

TEST(ShaderParser, CompileSceneShaderVariantPacksLargeVaryingArrays) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "large-varying-array-test";
    desc.shader_name = "large-varying-array-test";
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/large-varying-array-test.vert",
        .source     = R"(
attribute vec3 a_Position;
varying float audioValue[32];
varying vec2 v_TexCoord;
void main() {
    for (int i = 0; i < 32; ++i) {
        audioValue[i] = a_Position.x + float(i);
    }
    v_TexCoord = a_Position.xy;
    gl_Position = vec4(a_Position, 1.0);
}
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/large-varying-array-test.frag",
        .source     = R"(
varying float audioValue[32];
varying vec2 v_TexCoord;
void main() {
    float value = 0.0;
    for (int i = 0; i < 32; ++i) {
        value += audioValue[i];
    }
    gl_FragColor = vec4(v_TexCoord, value, 1.0);
}
)",
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.shader);
    ASSERT_EQ(result.shader->codes.size(), 2u);
    for (std::size_t stage = 0; stage < result.shader->codes.size(); ++stage) {
        const auto&            code = result.shader->codes[stage];
        SpvReflectShaderModule module {};
        ASSERT_EQ(spvReflectCreateShaderModule(code.size() * sizeof(code[0]), code.data(), &module),
                  SPV_REFLECT_RESULT_SUCCESS);

        std::uint32_t count = 0;
        const auto    enumerate =
            stage == 0 ? spvReflectEnumerateOutputVariables : spvReflectEnumerateInputVariables;
        ASSERT_EQ(enumerate(&module, &count, nullptr), SPV_REFLECT_RESULT_SUCCESS);
        std::vector<SpvReflectInterfaceVariable*> variables(count);
        ASSERT_EQ(enumerate(&module, &count, variables.data()), SPV_REFLECT_RESULT_SUCCESS);
        for (const auto* variable : variables) {
            if ((variable->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) == 0) {
                EXPECT_LT(variable->location, 32u);
            }
        }
        spvReflectDestroyShaderModule(&module);
    }
}

TEST(ShaderParser, CompileSceneShaderVariantUsesNarrowProducerVaryingType) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "narrow-producer-varying-test";
    desc.shader_name = "narrow-producer-varying-test";
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/narrow-producer-varying-test.vert",
        .source     = R"(
attribute vec3 a_Position;
varying vec2 v_TexCoord;
void main() {
    v_TexCoord = a_Position.xy;
    gl_Position = vec4(a_Position, 1.0);
}
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/narrow-producer-varying-test.frag",
        .source     = R"(
varying vec4 v_TexCoord;
void main() {
    vec2 uv = v_TexCoord * vec2(1.0, 1.0);
    gl_FragColor = vec4(uv, 0.0, 1.0);
}
)",
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.shader);
    ASSERT_EQ(result.shader->codes.size(), 2u);
}

TEST(ShaderParser, CompileSceneShaderVariantUsesWideProducerVaryingType) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "wide-producer-varying-test";
    desc.shader_name = "wide-producer-varying-test";
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/wide-producer-varying-test.vert",
        .source     = R"(
attribute vec3 a_Position;
varying vec4 v_TexCoord;
void main() {
    v_TexCoord = vec4(a_Position.xy, 0.25, 0.75);
    gl_Position = vec4(a_Position, 1.0);
}
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/wide-producer-varying-test.frag",
        .source     = R"(
varying vec2 v_TexCoord;
void main() {
    gl_FragColor = vec4(v_TexCoord.zw, 0.0, 1.0);
}
)",
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.shader);
    ASSERT_EQ(result.shader->codes.size(), 2u);
}

TEST(ShaderParser, CompileSceneShaderVariantShapesCrossStageUniformDefaults) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "cross-stage-uniform-default-test";
    desc.shader_name = "cross-stage-uniform-default-test";
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/cross-stage-uniform-default-test.vert",
        .source     = R"(
attribute vec3 a_Position;
uniform vec2 u_refResolution; // {"material":"resolution","default":"512 512"}
void main() {
    gl_Position = vec4(a_Position.xy / u_refResolution, a_Position.z, 1.0);
}
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/cross-stage-uniform-default-test.frag",
        .source     = R"(
uniform float u_refResolution; // {"material":"resolution","default":512}
void main() {
    gl_FragColor = vec4(1.0);
}
)",
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.shader);
    const auto& shader_default = result.shader->default_uniforms.at("u_refResolution");
    ASSERT_EQ(shader_default.size(), rstd::usize(2));
    EXPECT_FLOAT_EQ(shader_default[rstd::usize()], 512.0f);
    EXPECT_FLOAT_EQ(shader_default[rstd::usize(1)], 512.0f);
    const auto& variant_default = result.variant.default_uniforms.at("u_refResolution");
    EXPECT_EQ(variant_default.size(), rstd::usize(2));
}

TEST(ShaderParser, CompileSceneShaderVariantAcceptsNativeMatrixConstructors) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "matrix-constructor-test";
    desc.shader_name = "matrix-constructor-test";
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/matrix-constructor-test.vert",
        .source     = R"(
attribute vec3 a_Position;
mat3 rotate(vec3 c, vec3 s) {
    return mul(mul(mat3(c.z, -s.z, 0, s.z, c.z, 0, 0, 0, 1),
                   mat3(1, 0, 0, 0, c.x, -s.x, 0, s.x, c.x)),
               mat3(c.y, 0, s.y, 0, 1, 0, -s.y, 0, c.y));
}
mat2x3 rectangular() {
    return mat2x3(1, 2, 3, 4, 5, 6);
}
void main() {
    mat2x3 rectangularValue = rectangular();
    vec3 p = mul(a_Position, rotate(vec3(1), vec3(0)));
    gl_Position = vec4(p + vec3(rectangularValue[0][0] * 0), 1.0);
}
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/matrix-constructor-test.frag",
        .source     = R"(
void main() {
    gl_FragColor = vec4(1.0);
}
)",
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.shader);
    ASSERT_EQ(result.shader->codes.size(), 2u);
    EXPECT_FALSE(result.shader->codes[0].empty());
    EXPECT_FALSE(result.shader->codes[1].empty());
}

TEST(ShaderParser, ReflectsNativeHlslMatrixLayout) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "matrix-reflection-test";
    desc.shader_name = "matrix-reflection-test";
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/matrix-reflection-test.vert",
        .source     = R"(
attribute vec4 a_Position;
uniform mat2 g_Mat2;
uniform mat3 g_Mat3;
uniform mat4 g_Mat4;
uniform mat4x3 g_Bones[2];
void main() {
    vec2 p2 = mul(a_Position.xy, g_Mat2);
    vec3 p3 = mul(a_Position.xyz, g_Mat3);
    vec3 bone = mul(a_Position, g_Bones[1]);
    gl_Position = mul(vec4(bone + p3 + vec3(p2, 0.0), 1.0), g_Mat4);
}
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/matrix-reflection-test.frag",
        .source     = R"(
void main() {
    gl_FragColor = vec4(1.0);
}
)",
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.shader);
    EXPECT_EQ(result.shader->matrix_convention, owe::ShaderMatrixConvention::RowVector);
    EXPECT_EQ(result.shader->matrix_abi, owe::ShaderMatrixAbi::Hlsl);
    std::vector<owe::vulkan::Uni_ShaderSpv> spvs;
    owe::vulkan::ShaderReflected            reflected;
    ASSERT_TRUE(owe::vulkan::GenReflect(result.shader->codes, spvs, reflected));
    ASSERT_EQ(reflected.blocks.size(), 1u);
    const auto local =
        std::find_if(reflected.blocks.begin(), reflected.blocks.end(), [](const auto& block) {
            return block.name == "ww_DrawUniforms";
        });
    ASSERT_NE(local, reflected.blocks.end());
    const auto& members = local->member_map;
    ASSERT_TRUE(members.contains("g_Mat2"));
    ASSERT_TRUE(members.contains("g_Mat3"));
    ASSERT_TRUE(members.contains("g_Mat4"));
    ASSERT_TRUE(members.contains("g_Bones"));

    const auto& mat3 = members.at("g_Mat3");
    EXPECT_EQ(mat3.scalar_kind, owe::ShaderScalarKind::Float);
    EXPECT_EQ(mat3.scalar_width, 32u);
    EXPECT_EQ(mat3.matrix_rows, 3u);
    EXPECT_EQ(mat3.matrix_columns, 3u);
    EXPECT_EQ(mat3.matrix_major, owe::ShaderMatrixMajor::Row);
    EXPECT_EQ(mat3.matrix_stride, 16u);

    const auto& bones = members.at("g_Bones");
    EXPECT_EQ(bones.matrix_rows, 3u);
    EXPECT_EQ(bones.matrix_columns, 4u);
    EXPECT_EQ(bones.matrix_major, owe::ShaderMatrixMajor::Row);
    EXPECT_EQ(bones.matrix_stride, 16u);
    EXPECT_EQ(bones.array_stride, 48u);
    EXPECT_EQ(bones.num, rstd::usize(2));
    ASSERT_EQ(bones.array_dimensions.size(), 1u);
    EXPECT_EQ(bones.array_dimensions.front(), 2u);
}

TEST(ShaderParser, UsesOneCanonicalGlobalAbiForLegacyDaytimeAlias) {
    auto compile = [](std::string_view declaration, std::string_view expression) {
        owe::SceneShaderVariantDesc desc;
        desc.scene_id    = "global-uniform-abi-test";
        desc.shader_name = "global-uniform-abi-test";
        desc.stages.push_back(owe::SceneShaderVariantStage {
            .stage      = owe::ShaderType::VERTEX,
            .source_key = "/assets/shaders/global-uniform-abi-test.vert",
            .source     = std::string("attribute vec3 a_Position;\nuniform float ") +
                          std::string(declaration) +
                          ";\nvoid main() { gl_Position = vec4(a_Position.x + " +
                          std::string(expression) + " * 0.0, a_Position.yz, 1.0); }\n",
        });
        desc.stages.push_back(owe::SceneShaderVariantStage {
            .stage      = owe::ShaderType::FRAGMENT,
            .source_key = "/assets/shaders/global-uniform-abi-test.frag",
            .source     = "void main() { gl_FragColor = vec4(1.0); }",
        });
        owe::fs::VFS vfs;
        return owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);
    };

    const auto canonical = compile("g_Daytime", "g_Daytime");
    const auto legacy    = compile("g_DayTime", "g_DayTime");
    ASSERT_TRUE(canonical.ok) << canonical.error;
    ASSERT_TRUE(legacy.ok) << legacy.error;
    ASSERT_EQ(canonical.shader->descriptor_sets.size(), 2u);
    ASSERT_EQ(legacy.shader->descriptor_sets.size(), 2u);
    EXPECT_EQ(canonical.shader->descriptor_sets[0].identity,
              legacy.shader->descriptor_sets[0].identity);

    std::vector<owe::vulkan::Uni_ShaderSpv> canonical_spvs;
    std::vector<owe::vulkan::Uni_ShaderSpv> legacy_spvs;
    owe::vulkan::ShaderReflected            canonical_reflection;
    owe::vulkan::ShaderReflected            legacy_reflection;
    ASSERT_TRUE(
        owe::vulkan::GenReflect(canonical.shader->codes, canonical_spvs, canonical_reflection));
    ASSERT_TRUE(owe::vulkan::GenReflect(legacy.shader->codes, legacy_spvs, legacy_reflection));
    const auto& canonical_members = canonical_reflection.blocks.front().member_map;
    const auto& legacy_members    = legacy_reflection.blocks.front().member_map;
    EXPECT_EQ(canonical_members.size(), legacy_members.size());
    EXPECT_TRUE(legacy_members.contains("g_Daytime"));
    EXPECT_FALSE(legacy_members.contains("g_DayTime"));
    const auto global = std::find_if(canonical_reflection.blocks.begin(),
                                     canonical_reflection.blocks.end(),
                                     [](const auto& block) {
                                         return block.name == "ww_GlobalUniforms";
                                     });
    ASSERT_NE(global, canonical_reflection.blocks.end());
    EXPECT_EQ(global->size, owe::kFrameUniformBlockSize.to_primitive());
    for (const auto& field : owe::GlobalUniformFields()) {
        if (owe::GlobalUniformBlockFor(field.producer) != owe::GlobalUniformBlockKind::Frame)
            continue;
        auto member = global->member_map.find(rstd::cppstd::to_string(field.name));
        ASSERT_NE(member, global->member_map.end());
        EXPECT_EQ(member->second.offset, field.offset.to_primitive());
    }
    EXPECT_EQ(canonical_reflection.blocks.size(), 1u);
}

TEST(ShaderParser, KeepsShadowMatricesInCanonicalGlobalSet) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "shadow-global-uniform-abi-test";
    desc.shader_name = "shadow-global-uniform-abi-test";
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/shadow-global-uniform-abi-test.vert",
        .source     = R"(
attribute vec3 a_Position;
in uint gl_InstanceID;
in uint gl_VertexID;
varying uint gl_ViewportIndex;
uniform mat4 g_ViewportViewProjectionMatrices[6];
void main() {
    gl_Position = mul(vec4(a_Position, 1.0),
                      g_ViewportViewProjectionMatrices[gl_InstanceID]);
    gl_Position.x += float(gl_VertexID) * 0.0;
    gl_ViewportIndex = gl_InstanceID;
}
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/shadow-global-uniform-abi-test.frag",
        .source     = "void main() { gl_FragColor = vec4(1.0); }",
    });

    owe::fs::VFS vfs;
    auto         compile = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);
    ASSERT_TRUE(compile.ok) << compile.error;

    std::vector<owe::vulkan::Uni_ShaderSpv> spvs;
    owe::vulkan::ShaderReflected            reflection;
    ASSERT_TRUE(owe::vulkan::GenReflect(compile.shader->codes, spvs, reflection));
    const auto global =
        std::find_if(reflection.blocks.begin(), reflection.blocks.end(), [](const auto& block) {
            return block.name == "ww_LightingUniforms";
        });
    ASSERT_NE(global, reflection.blocks.end());
    const auto matrices = global->member_map.find("g_ViewportViewProjectionMatrices");
    ASSERT_NE(matrices, global->member_map.end());
    EXPECT_EQ(matrices->second.num, rstd::usize(6));
    EXPECT_EQ(reflection.input_location_map.size(), 1u);
    EXPECT_TRUE(reflection.input_location_map.contains("a_Position"));
}

TEST(ShaderParser, FallsBackAsOneLegacyInterfaceOnGlobalTypeConflict) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "global-uniform-conflict-test";
    desc.shader_name = "global-uniform-conflict-test";
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/global-uniform-conflict-test.vert",
        .source     = R"(
attribute vec3 a_Position;
uniform vec2 g_Time;
void main() { gl_Position = vec4(a_Position.xy + g_Time * 0.0, a_Position.z, 1.0); }
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/global-uniform-conflict-test.frag",
        .source     = R"(
uniform sampler2D g_Texture0;
void main() { gl_FragColor = texSample2D(g_Texture0, vec2(0.5)); }
)",
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.shader->descriptor_sets.size(), 1u);
    EXPECT_EQ(result.shader->descriptor_sets.front().set, rstd::u32(1));
    EXPECT_TRUE(result.shader->descriptor_sets.front().push_descriptor);
    ASSERT_EQ(result.shader->uniform_blocks.size(), 1u);
    EXPECT_EQ(result.shader->uniform_blocks.front().name, "ww_Uniforms");
    EXPECT_EQ(result.shader->uniform_blocks.front().set, rstd::u32(1));
    EXPECT_EQ(result.shader->uniform_blocks.front().scope,
              owe::SceneShaderUniformBlockScope::Local);
}

TEST(ShaderParser, PrunesUnusedDrawUniformsAndInactiveGlobalBlocks) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "draw-uniform-prune-test";
    desc.shader_name = "draw-uniform-prune-test";
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/draw-uniform-prune-test.vert",
        .source     = R"(
attribute vec3 a_Position;
uniform float g_Used;
uniform float g_Unused;
void main() { gl_Position = vec4(a_Position.x + g_Used, a_Position.yz, 1.0); }
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/draw-uniform-prune-test.frag",
        .source     = "void main() { gl_FragColor = vec4(1.0); }",
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);
    ASSERT_TRUE(result.ok) << result.error;
    std::vector<owe::vulkan::Uni_ShaderSpv> spvs;
    owe::vulkan::ShaderReflected            reflected;
    ASSERT_TRUE(owe::vulkan::GenReflect(result.shader->codes, spvs, reflected));
    const auto local =
        std::find_if(reflected.blocks.begin(), reflected.blocks.end(), [](const auto& block) {
            return block.name == "ww_DrawUniforms";
        });
    ASSERT_NE(local, reflected.blocks.end());
    EXPECT_TRUE(local->member_map.contains("g_Used"));
    EXPECT_FALSE(local->member_map.contains("g_Unused"));
    const auto global =
        std::find_if(reflected.blocks.begin(), reflected.blocks.end(), [](const auto& block) {
            return block.name == "ww_GlobalUniforms" || block.name == "ww_AudioUniforms" ||
                   block.name == "ww_LightingUniforms";
        });
    EXPECT_EQ(global, reflected.blocks.end());
}

TEST(ShaderParser, SplitsActiveGlobalUniformsAcrossSetZeroBindings) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "split-global-uniform-abi-test";
    desc.shader_name = "split-global-uniform-abi-test";
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/split-global-uniform-abi-test.vert",
        .source     = R"(
attribute vec3 a_Position;
uniform float g_Time;
uniform float g_AudioSpectrum16Left[16];
uniform vec3 g_LightsPosition[4];
void main() {
    float inputValue = g_Time + g_AudioSpectrum16Left[0] + g_LightsPosition[0].x;
    gl_Position = vec4(a_Position.x + inputValue * 0.0, a_Position.yz, 1.0);
}
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/split-global-uniform-abi-test.frag",
        .source     = "void main() { gl_FragColor = vec4(1.0); }",
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.shader->uniform_blocks.size(), 3u);
    for (const auto& block : owe::GlobalUniformBlocks()) {
        const auto reflected =
            std::find_if(result.shader->uniform_blocks.begin(),
                         result.shader->uniform_blocks.end(),
                         [&](const auto& candidate) {
                             return candidate.name == rstd::cppstd::to_string(block.name);
                         });
        ASSERT_NE(reflected, result.shader->uniform_blocks.end());
        EXPECT_EQ(reflected->set, owe::kGlobalUniformSet);
        EXPECT_EQ(reflected->binding, block.binding);
        EXPECT_EQ(reflected->identity, block.identity);
        EXPECT_EQ(reflected->scope, owe::SceneShaderUniformBlockScope::Shared);
    }
}

TEST(ShaderParser, CompileSceneShaderVariantUsesPhysicalFileCache) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("owe-shader-cache-" + std::to_string(rstd::process::id().to_primitive()));
    std::filesystem::remove_all(root);

    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "physical-cache-test";
    desc.shader_name = "physical-cache-test";
    desc.texture_infos.resize(1);
    desc.texture_infos[0].enabled = true;
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/physical-cache-test.vert",
        .source     = R"(
attribute vec3 a_Position;
void main() {
    gl_Position = vec4(a_Position, 1.0);
}
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/physical-cache-test.frag",
        .source     = R"(
uniform float g_Brightness;
uniform sampler2D g_Texture0;
void main() {
    gl_FragColor = texSample2D(g_Texture0, vec2(0.5)) * g_Brightness;
}
)",
    });

    const auto   cache_text = root.string();
    const auto   cache_path = rstd::path::PathBuf::from(rstd::cppstd::as_str(cache_text).unwrap());
    owe::fs::VFS vfs;
    const auto   compile_cached = [&](const owe::SceneShaderVariantDesc& value,
                                      const owe::Combos&                 combos = {}) {
        owe::ShaderCache cache(Some(rstd::path::PathBuf::from(cache_path.as_path())));
        return owe::ShaderParser::CompileSceneShaderVariant(value, vfs, combos, &cache);
    };
    const auto first = compile_cached(desc);
    ASSERT_TRUE(first.ok) << first.error;
    ASSERT_TRUE(first.shader);

    const auto shader_cache = root / desc.scene_id / "spvs03";
    ASSERT_TRUE(std::filesystem::is_directory(shader_cache));
    const auto files = std::filesystem::directory_iterator(shader_cache);
    ASSERT_NE(files, std::filesystem::directory_iterator {});
    const auto artifact_path = files->path();
    EXPECT_EQ(artifact_path.extension(), ".spvs");
    EXPECT_GT(files->file_size(), 112u);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(shader_cache),
                            std::filesystem::directory_iterator {}),
              1);

    std::array<unsigned char, 28> header {};
    {
        std::ifstream artifact_file(artifact_path, std::ios::binary);
        ASSERT_TRUE(artifact_file.read(reinterpret_cast<char*>(header.data()), header.size()));
    }
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(header.data()), 8),
              std::string("OWESPV3\0", 8));
    const auto read_u32 = [&header](std::size_t offset) {
        return static_cast<std::uint32_t>(header[offset]) |
               (static_cast<std::uint32_t>(header[offset + 1]) << 8) |
               (static_cast<std::uint32_t>(header[offset + 2]) << 16) |
               (static_cast<std::uint32_t>(header[offset + 3]) << 24);
    };
    EXPECT_EQ(read_u32(8), 3u);
    EXPECT_EQ(read_u32(12), 19u);
    EXPECT_EQ(read_u32(16), 112u);
    EXPECT_EQ(read_u32(24), 2u);
    const auto initial_write_time = std::filesystem::last_write_time(artifact_path);

    const auto second = compile_cached(desc);
    ASSERT_TRUE(second.ok) << second.error;
    ASSERT_TRUE(second.shader);
    EXPECT_EQ(second.shader->codes, first.shader->codes);
    EXPECT_TRUE(second.variant.stages[1].uniforms.contains("g_Brightness"));
    EXPECT_TRUE(second.variant.stages[1].active_texture_slots.contains(0));
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(shader_cache),
                            std::filesystem::directory_iterator {}),
              1);
    EXPECT_EQ(std::filesystem::last_write_time(artifact_path), initial_write_time);

    std::filesystem::resize_file(artifact_path, 16);
    const auto after_truncation = compile_cached(desc);
    ASSERT_TRUE(after_truncation.ok) << after_truncation.error;
    ASSERT_TRUE(after_truncation.shader);
    EXPECT_EQ(after_truncation.shader->codes, first.shader->codes);
    EXPECT_GT(std::filesystem::file_size(artifact_path), 112u);

    {
        std::fstream artifact(artifact_path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(artifact.seekg(52));
        char byte = 0;
        ASSERT_TRUE(artifact.read(&byte, 1));
        byte ^= 1;
        ASSERT_TRUE(artifact.seekp(52));
        ASSERT_TRUE(artifact.write(&byte, 1));
    }
    const auto after_identity_corruption = compile_cached(desc);
    ASSERT_TRUE(after_identity_corruption.ok) << after_identity_corruption.error;
    ASSERT_TRUE(after_identity_corruption.shader);
    EXPECT_EQ(after_identity_corruption.shader->codes, first.shader->codes);

    {
        std::fstream artifact(artifact_path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(artifact.seekg(-1, std::ios::end));
        char byte = 0;
        ASSERT_TRUE(artifact.read(&byte, 1));
        byte ^= 1;
        ASSERT_TRUE(artifact.seekp(-1, std::ios::end));
        ASSERT_TRUE(artifact.write(&byte, 1));
    }
    const auto after_payload_corruption = compile_cached(desc);
    ASSERT_TRUE(after_payload_corruption.ok) << after_payload_corruption.error;
    ASSERT_TRUE(after_payload_corruption.shader);
    EXPECT_EQ(after_payload_corruption.shader->codes, first.shader->codes);

    const auto different_combo = compile_cached(desc, { { "CACHE_VARIANT", "1" } });
    ASSERT_TRUE(different_combo.ok) << different_combo.error;
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(shader_cache),
                            std::filesystem::directory_iterator {}),
              2);

    desc.stages[1].source += "\n// source identity variant";
    const auto different_source = compile_cached(desc);
    ASSERT_TRUE(different_source.ok) << different_source.error;
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(shader_cache),
                            std::filesystem::directory_iterator {}),
              3);
    for (const auto& entry : std::filesystem::directory_iterator(shader_cache)) {
        EXPECT_EQ(entry.path().extension(), ".spvs");
    }

    std::filesystem::remove_all(root);
}

TEST(ShaderParser, CompileSceneShaderVariantExportsSamplerBindings) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "sampler-binding-test";
    desc.shader_name = "sampler-binding-test";
    desc.texture_infos.resize(4);
    desc.texture_infos[3].enabled = true;
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/sampler-binding-test.vert",
        .source     = R"(
attribute vec3 a_Position;
varying vec2 v_TexCoord;
void main() {
    v_TexCoord = a_Position.xy;
    gl_Position = vec4(a_Position, 1.0);
}
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/sampler-binding-test.frag",
        .source     = R"(
varying vec2 v_TexCoord;
uniform sampler2D g_Texture3;
void main() {
    gl_FragColor = texSample2D(g_Texture3, v_TexCoord);
}
)",
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.shader);
    ASSERT_EQ(result.variant.sampler_bindings.size(), 1u);
    EXPECT_EQ(result.variant.sampler_bindings[0].texture_slot, 3u);
    EXPECT_EQ(result.variant.sampler_bindings[0].shader_member, "g_Texture3");
    EXPECT_EQ(result.shader->SamplerMember(3), "g_Texture3");
}

TEST(ShaderParser, CompileSceneShaderVariantUsesDescriptorAndComboOverride) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id        = "variant-test";
    desc.shader_name     = "variant-test";
    desc.resolved_combos = { { "USE_COLOR", "0" } };
    desc.texture_infos.push_back(owe::SceneShaderTextureCompileInfo { .enabled = false });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/variant-test.vert",
        .source     = R"(
// [COMBO] {"combo":"USE_COLOR","default":0}
attribute vec3 a_Position;
varying vec4 v_Color;
void main() {
    gl_Position = vec4(a_Position, 1.0);
#if USE_COLOR == 1
    v_Color = vec4(1.0, 0.0, 0.0, 1.0);
#else
    v_Color = vec4(0.0, 1.0, 0.0, 1.0);
#endif
}
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/variant-test.frag",
        .source     = R"(
varying vec4 v_Color;
uniform float g_Brightness; // {"material":"brightness","default":1.0,"range":[0,2]}
void main() {
    gl_FragColor = v_Color * g_Brightness;
}
)",
    });

    owe::fs::VFS vfs;
    const auto   result =
        owe::ShaderParser::CompileSceneShaderVariant(desc, vfs, { { "USE_COLOR", "1" } });

    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.shader);
    EXPECT_EQ(result.shader->name, "variant-test");
    ASSERT_EQ(result.shader->codes.size(), 2u);
    EXPECT_FALSE(result.shader->codes[0].empty());
    EXPECT_EQ(result.variant.resolved_combos.at("USE_COLOR"), "1");
    EXPECT_EQ(result.variant.input_combos.at("USE_COLOR"), "1");
    EXPECT_EQ(result.variant.uniform_aliases.at("brightness"), "g_Brightness");
    EXPECT_TRUE(result.variant.default_uniforms.contains("g_Brightness"));
    EXPECT_NE(result.variant.descriptor_layout_hash, 0u);
    ASSERT_EQ(result.variant.stages.size(), 2u);
    EXPECT_EQ(result.variant.stages[0].source_key, "/assets/shaders/variant-test.vert");
    EXPECT_NE(result.variant.stages[0].code_hash, rstd::usize());
    EXPECT_TRUE(result.variant.stages[1].uniforms.contains("g_Brightness"));
    EXPECT_NE(result.variant.stages[1].code_hash, rstd::usize());
}

TEST(ShaderParser, CompileSceneShaderVariantResolvesRequiredComboDefaults) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id     = "required-combo-test";
    desc.shader_name  = "required-combo-test";
    desc.input_combos = {
        { "HATCH", "0" },       { "LINE_COUNT", "3" },      { "LINE_STYLE2", "8" },
        { "LINE_STYLE3", "3" }, { "SHAPE_VARIATION", "0" },
    };
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/required-combo-test.vert",
        .source     = R"(
attribute vec3 a_Position;
void main() {
    gl_Position = vec4(a_Position, 1.0);
}
)",
    });
    desc.stages.push_back(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/required-combo-test.frag",
        .source     = R"(
// [COMBO] {"combo":"HATCH","default":0}
// [COMBO] {"combo":"LINE_COUNT","default":1,"require":{"HATCH":0}}
// [COMBO] {"combo":"LINE_STYLE2","default":0,"require":{"HATCH":0,"LINE_COUNT":2}}
// [COMBO] {"combo":"LINE_STYLE3","default":0,"require":{"HATCH":0,"LINE_COUNT":3}}
// [COMBO] {"combo":"SHAPE_VARIATION","default":1,"require":{"HATCH":1}}
#if LINE_COUNT == 3
#define LINE_STYLE2 LINE_STYLE3
#endif
float selection() {
#if SHAPE_VARIATION == 1
    return 1.0;
#endif
}
void main() {
#if LINE_STYLE2 == 3
    gl_FragColor = vec4(selection());
#else
    gl_FragColor = vec4(0.0);
#endif
}
)",
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(result.variant.resolved_combos.contains("LINE_STYLE2"));
    EXPECT_EQ(result.variant.resolved_combos.at("LINE_STYLE3"), "3");
    EXPECT_EQ(result.variant.resolved_combos.at("SHAPE_VARIATION"), "1");
    EXPECT_EQ(result.variant.input_combos.at("LINE_STYLE2"), "8");
    EXPECT_EQ(result.variant.input_combos.at("SHAPE_VARIATION"), "0");
}
