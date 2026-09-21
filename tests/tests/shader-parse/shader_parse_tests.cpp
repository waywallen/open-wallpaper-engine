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

TEST(SceneShaderHash, PreservesStageOrderAndTracksRuntimeInterfaces) {
    auto make_shader = [] {
        owe::SceneShader shader;
        shader.codes.push(owe::ShaderCode::from(array<rstd::uint32_t, 2> { 1u, 2u }.as_slice()));
        shader.codes.push(owe::ShaderCode::from(array<rstd::uint32_t, 2> { 3u, 4u }.as_slice()));
        shader.descriptor_sets.push(owe::SceneShaderDescriptorSetInterface {
            .set             = u32(1),
            .push_descriptor = true,
            .identity        = u64(41),
        });
        shader.uniform_blocks.push(owe::SceneShaderUniformBlockInterface {
            .name     = "block"_Str,
            .set      = u32(1),
            .binding  = u32(2),
            .identity = u64(42),
        });
        return shader;
    };
    auto       original = make_shader();
    const auto expected = owe::SceneShaderCodeHash(original);
    EXPECT_EQ(expected, owe::SceneShaderCodeHash(make_shader()));
    auto changed = make_shader();
    rstd::mem::swap(changed.codes[usize()], changed.codes[usize(1)]);
    EXPECT_NE(expected, owe::SceneShaderCodeHash(changed));
    changed                         = make_shader();
    changed.codes[usize()][usize()] = 5;
    EXPECT_NE(expected, owe::SceneShaderCodeHash(changed));
    changed = make_shader();
    changed.codes.push(owe::ShaderCode {});
    EXPECT_NE(expected, owe::SceneShaderCodeHash(changed));
    changed            = make_shader();
    changed.matrix_abi = owe::ShaderMatrixAbi::Hlsl;
    EXPECT_NE(expected, owe::SceneShaderCodeHash(changed));
    changed                   = make_shader();
    changed.matrix_convention = owe::ShaderMatrixConvention::RowVector;
    EXPECT_NE(expected, owe::SceneShaderCodeHash(changed));
    changed                                   = make_shader();
    changed.descriptor_sets[usize()].identity = u64(43);
    EXPECT_NE(expected, owe::SceneShaderCodeHash(changed));
    changed                                          = make_shader();
    changed.descriptor_sets[usize()].push_descriptor = false;
    EXPECT_NE(expected, owe::SceneShaderCodeHash(changed));
    changed                                  = make_shader();
    changed.uniform_blocks[usize()].identity = u64(43);
    EXPECT_NE(expected, owe::SceneShaderCodeHash(changed));
    changed                               = make_shader();
    changed.uniform_blocks[usize()].scope = owe::SceneShaderUniformBlockScope::Shared;
    EXPECT_NE(expected, owe::SceneShaderCodeHash(changed));
}

TEST(ShaderBackend, NativeSourcePreprocessesAndCompiles) {
    String source =
        "#version 450\n#define POSITION vec4(0.0)\nvoid main(){gl_Position=POSITION;}\n"_Str;
    String preprocessed;
    ASSERT_TRUE(owe::vulkan::Preprocess(
        source.as_str(), owe::ShaderType::VERTEX, owe::vulkan::SourceLang::Glsl, preprocessed));
    EXPECT_TRUE(preprocessed.as_str().contains("gl_Position"_str));
    Vec<owe::vulkan::ShaderCompUnit> units;
    units.push(owe::vulkan::ShaderCompUnit {
        .stage       = owe::ShaderType::VERTEX,
        .src         = rstd::move(preprocessed),
        .entry_point = {},
    });
    Vec<owe::vulkan::Uni_ShaderSpv> output;
    ASSERT_TRUE(owe::vulkan::CompileAndLinkShaderUnits(units.as_slice(), {}, output));
    ASSERT_EQ(output.len(), usize(1));
    EXPECT_EQ(output[usize()]->entry_point, "main"_str);
    EXPECT_FALSE(output[usize()]->spirv.is_empty());
}

TEST(ShaderBackend, RejectsEmbeddedNulEntryPoint) {
    Vec<owe::vulkan::ShaderCompUnit> units;
    units.push(owe::vulkan::ShaderCompUnit {
        .stage       = owe::ShaderType::VERTEX,
        .src         = "#version 450\nvoid main(){gl_Position=vec4(0.0);}\n"_Str,
        .entry_point = "main\0ignored"_Str,
    });
    Vec<owe::vulkan::Uni_ShaderSpv> output;
    EXPECT_FALSE(owe::vulkan::CompileAndLinkShaderUnits(units.as_slice(), {}, output));
}

TEST(ShaderBackend, InvalidSourcesReturnFailure) {
    String preprocessed;
    EXPECT_FALSE(owe::vulkan::Preprocess("#version 450\n#error owe shader diagnostic test\n"_str,
                                         owe::ShaderType::VERTEX,
                                         owe::vulkan::SourceLang::Glsl,
                                         preprocessed));
    Vec<owe::vulkan::ShaderCompUnit> units;
    units.push(owe::vulkan::ShaderCompUnit {
        .stage       = owe::ShaderType::VERTEX,
        .src         = "#version 450\nvoid main(){gl_Position=owe_invalid_test_symbol;}\n"_Str,
        .entry_point = {},
    });
    Vec<owe::vulkan::Uni_ShaderSpv> output;
    EXPECT_FALSE(owe::vulkan::CompileAndLinkShaderUnits(units.as_slice(), {}, output));
}

namespace
{

ShaderInfo Parse(const std::string& src, std::size_t n_tex_slots = 8) {
    ShaderInfo         info {};
    Vec<ShaderTexInfo> texs;
    texs.resize(usize(n_tex_slots), ShaderTexInfo {});
    for (auto t : texs.iter_mut()) t->enabled = true;
    ParseShader(rstd::cppstd::as_str(src).unwrap(), &info, texs.as_slice());
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
    ASSERT_EQ(info.defTexs.len().to_primitive(), 1u);
    EXPECT_EQ(info.defTexs[usize()].slot, i32(6));
    EXPECT_EQ(info.defTexs[usize()].texture.as_str(), "_rt_shadowAtlas"_str);
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
    ASSERT_TRUE(info.combos.contains_key("MASK"_str));
    EXPECT_EQ((**info.combos.get("MASK"_str)).as_str(), "1"_str);
}

TEST(ShaderParser, FourthPackedTextureComponentSetsItsCombo) {
    const std::string  src = R"(
uniform sampler2D g_Texture2; // {"components":[{"combo":"METALLIC_MAP"},{"combo":"ROUGHNESS_MAP"},{"combo":"REFLECTION_MAP"},{"combo":"EMISSIVE_MAP"}]}
void main(){}
)";
    ShaderInfo         info {};
    Vec<ShaderTexInfo> texs;
    texs.resize(usize(3), ShaderTexInfo {});
    texs[usize(2)] = ShaderTexInfo {
        .enabled       = true,
        .composEnabled = { false, false, false, true },
    };

    ParseShader(rstd::cppstd::as_str(src).unwrap(), &info, texs.as_slice());

    EXPECT_FALSE(info.combos.contains_key("METALLIC_MAP"_str));
    EXPECT_FALSE(info.combos.contains_key("ROUGHNESS_MAP"_str));
    EXPECT_FALSE(info.combos.contains_key("REFLECTION_MAP"_str));
    ASSERT_TRUE(info.combos.contains_key("EMISSIVE_MAP"_str));
    EXPECT_EQ((**info.combos.get("EMISSIVE_MAP"_str)).as_str(), "1"_str);
}

TEST(ShaderParser, ComboLineCollectedRegardlessOfIfdef) {
    const std::string src  = R"(
#if 0
// [COMBO] {"combo":"NEVER","default":1}
#endif
void main(){}
)";
    auto              info = Parse(src);
    ASSERT_TRUE(info.combos.contains_key("NEVER"_str));
    EXPECT_EQ((**info.combos.get("NEVER"_str)).as_str(), "1"_str);
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
    EXPECT_FALSE(info.svs.contains_key("g_Foo"_str));
}

TEST(ShaderParser, BlockCommentUniformNotCollected) {
    const std::string src  = R"(
/*
uniform vec4 g_Y; // {"default":1}
*/
void main(){}
)";
    auto              info = Parse(src);
    EXPECT_FALSE(info.svs.contains_key("g_Y"_str));
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
    EXPECT_TRUE(info.svs.contains_key("g_Real"_str));
}

// --- combo + uniform schema -------------------------------------------------

TEST(ShaderParser, ComboDefaultIsRecorded) {
    const std::string src  = R"(
// [COMBO] {"combo":"BLENDMODE","default":9}
void main(){}
)";
    auto              info = Parse(src);
    ASSERT_TRUE(info.combos.contains_key("BLENDMODE"_str));
    EXPECT_EQ((**info.combos.get("BLENDMODE"_str)).as_str(), "9"_str);
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
    ASSERT_TRUE(info.svs.contains_key("g_Brightness"_str));
    ASSERT_TRUE(info.alias.get("brightness"_str).is_some());
    EXPECT_EQ((**info.alias.get("brightness"_str)).as_str(), "g_Brightness"_str);
}

TEST(ShaderParser, ScalarAnnotationAcceptsLeadingZeroRangeNumber) {
    const std::string src  = R"(
uniform float u_userSpeed; // {"material":"Speed","default":1,"range":[0,01]}
void main(){}
)";
    auto              info = Parse(src);
    ASSERT_TRUE(info.svs.contains_key("u_userSpeed"_str));
    ASSERT_TRUE(info.alias.get("Speed"_str).is_some());
    EXPECT_EQ((**info.alias.get("Speed"_str)).as_str(), "u_userSpeed"_str);
}

TEST(ShaderParser, TextureAliasRecorded) {
    const std::string src  = R"(
uniform sampler2D g_Texture0; // {"material":"albedo","label":"Albedo","default":"util/white"}
void main(){}
)";
    auto              info = Parse(src);
    ASSERT_EQ(info.defTexs.len().to_primitive(), 1u);
    EXPECT_EQ(info.defTexs[usize()].slot, i32());
    EXPECT_EQ(info.defTexs[usize()].texture.as_str(), "util/white"_str);
    ASSERT_TRUE(info.alias.get("albedo"_str).is_some());
    EXPECT_EQ((**info.alias.get("albedo"_str)).as_str(), "g_Texture0"_str);
}

TEST(ShaderParser, NativeAnnotationsPreserveQuotedBytesAndPartialDefaults) {
    auto source = "// [COMBO] {\"combo\":\"PADDED\",\"default\":0002}\n"_Str;
    source.push_str("uniform vec3 u_Color; // {\"material\":\""_str);
    source.push_str("\u5b57\u6bb5-001\\\"x"_str);
    source.push_str(R"(","default":"1 bad 3","range":[-0002,0003]}
uniform float u_Signed; // {"default":-0002}
uniform float u_Invalid; // {"default":00oops}
void main(){}
uniform float u_AfterMain; // {"default":5}
)"_str);
    ShaderInfo info {};
    ParseShader(source.as_str(), &info, {});
    EXPECT_EQ(**info.combos.get("PADDED"_str), "2"_str);
    auto alias = info.alias.get("\u5b57\u6bb5-001\"x"_str);
    ASSERT_TRUE(alias.is_some());
    EXPECT_EQ(**alias, "u_Color"_str);
    auto color = info.svs.get("u_Color"_str);
    ASSERT_TRUE(color.is_some());
    EXPECT_EQ((*color)->size(), usize(3));
    EXPECT_FLOAT_EQ((**color)[usize()], 1.0f);
    EXPECT_FLOAT_EQ((**color)[usize(1)], 0.0f);
    EXPECT_FLOAT_EQ((**color)[usize(2)], 0.0f);
    auto signed_value = info.svs.get("u_Signed"_str);
    ASSERT_TRUE(signed_value.is_some());
    EXPECT_FLOAT_EQ((**signed_value)[usize()], -2.0f);
    EXPECT_FALSE(info.svs.contains_key("u_Invalid"_str));
    EXPECT_FALSE(info.svs.contains_key("u_AfterMain"_str));
}

TEST(ShaderParser, NativeShadowPassKeepsExactWhitespaceRules) {
    auto info = Parse("// [PASS] shadow \t caster \t\r\n// [PASS] shadow \t\r\nvoid main(){}\n");
    EXPECT_EQ(info.shadow_pass, "caster"_str);
    auto vertical = Parse("// [PASS] shadow \v caster \f\r\nvoid main(){}\n");
    EXPECT_EQ(vertical.shadow_pass, "\v caster \f"_str);
}

TEST(ShaderParser, TextureBoundIfSlotEnabled) {
    // Slot 0 in texinfos is enabled by default in Parse(); the texture-side
    // combo flag therefore reads "1".
    const std::string src  = R"(
uniform sampler2D g_Texture0; // {"combo":"HASTEX","default":"util/white"}
void main(){}
)";
    auto              info = Parse(src);
    ASSERT_TRUE(info.combos.contains_key("HASTEX"_str));
    EXPECT_EQ((**info.combos.get("HASTEX"_str)).as_str(), "1"_str);
}

TEST(ShaderParser, UndefsBuiltinMacroBeforeUserRedefine) {
    const std::string out = rstd::cppstd::to_string(
        owe::ShaderParser::PreShaderHeader(
            rstd::cppstd::as_str("#define M_PI_2 1.57079632679\nfloat f() { return M_PI_2; }\n")
                .unwrap(),
            {},
            owe::ShaderType::FRAGMENT)
            .as_str());

    const auto undef_pos  = out.find("#undef M_PI_2");
    const auto define_pos = out.find("#define M_PI_2 1.57079632679");
    ASSERT_NE(undef_pos, std::string::npos);
    ASSERT_NE(define_pos, std::string::npos);
    EXPECT_LT(undef_pos, define_pos);
}

TEST(ShaderParser, UndefsDerivativeAliasesBeforeUserRedefine) {
    const std::string out =
        rstd::cppstd::to_string(owe::ShaderParser::PreShaderHeader(rstd::cppstd::as_str(
                                                                       R"(
#if HLSL
#define dFdx ddx
#define dFdy ddy
#define textureGrad(s, uv, dx, dy) texSample2DGrad(s, uv, dx, dy)
#endif
void main() {}
)")
                                                                       .unwrap(),
                                                                   {},
                                                                   owe::ShaderType::FRAGMENT)
                                    .as_str());

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
    desc.scene_id    = "atan-overloads-test"_Str;
    desc.shader_name = "atan-overloads-test"_Str;
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/atan-overloads-test.vert"_Str,
        .source     = R"(
attribute vec3 a_Position;
varying vec2 v_TexCoord;
void main() {
    v_TexCoord = a_Position.xy;
    gl_Position = vec4(a_Position, 1.0);
}
)"_Str,
    });
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/atan-overloads-test.frag"_Str,
        .source     = R"(
varying vec2 v_TexCoord;
void main() {
    float one = atan(v_TexCoord.y / v_TexCoord.x);
    float two = atan(v_TexCoord.y, v_TexCoord.x);
    gl_FragColor = vec4(one, two, 0.0, 1.0);
}
)"_Str,
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << rstd::cppstd::as_string_view(result.error.as_str());
    ASSERT_TRUE(result.shader);
}

TEST(ShaderParser, PreShaderHeaderFlattensPackedAudioSpectrumAccess) {
    const std::string out =
        rstd::cppstd::to_string(owe::ShaderParser::PreShaderHeader(rstd::cppstd::as_str(
                                                                       R"(
uniform float g_AudioSpectrum64Left[64];
float sample(float barID) {
    return g_AudioSpectrum64Left[barID / 4][barID % 4];
}
)")
                                                                       .unwrap(),
                                                                   {},
                                                                   owe::ShaderType::FRAGMENT)
                                    .as_str());

    EXPECT_EQ(out.find("g_AudioSpectrum64Left[barID / 4][barID % 4]"), std::string::npos);
    EXPECT_NE(out.find("g_AudioSpectrum64Left[(int)(barID)]"), std::string::npos);
}

TEST(ShaderParser, PreShaderHeaderNormalizesFullwidthSemicolon) {
    const std::string out = rstd::cppstd::to_string(
        owe::ShaderParser::PreShaderHeader(
            rstd::cppstd::as_str("void main() { gl_FragColor = vec4(1.0)； }\n").unwrap(),
            {},
            owe::ShaderType::FRAGMENT)
            .as_str());

    EXPECT_EQ(out.find("；"), std::string::npos);
    EXPECT_NE(out.find("gl_FragColor = vec4(1.0);"), std::string::npos);
}

TEST(ShaderParser, PreShaderHeaderPreservesLocalMatrixConstructorMul) {
    const std::string out =
        rstd::cppstd::to_string(owe::ShaderParser::PreShaderHeader(rstd::cppstd::as_str(
                                                                       R"(
vec2 rotate(vec2 uv, float th) {
    return mul(uv, mat2(cos(th), sin(th), -sin(th), cos(th)));
}
)")
                                                                       .unwrap(),
                                                                   {},
                                                                   owe::ShaderType::FRAGMENT)
                                    .as_str());

    EXPECT_NE(out.find("mul(uv, mat2(cos(th), sin(th), -sin(th), cos(th)))"), std::string::npos);
    EXPECT_EQ(out.find("transpose(float2x2"), std::string::npos);
    EXPECT_EQ(out.find("_ww_mul"), std::string::npos);
}

TEST(ShaderParser, PreShaderHeaderPreservesNestedMatrixConstructors) {
    const std::string out =
        rstd::cppstd::to_string(owe::ShaderParser::PreShaderHeader(rstd::cppstd::as_str(
                                                                       R"(
mat3 rotate(vec3 c, vec3 s) {
    return mul(mul(mat3(c.z, -s.z, 0, s.z, c.z, 0, 0, 0, 1),
                   mat3(1, 0, 0, 0, c.x, -s.x, 0, s.x, c.x)),
               mat3(c.y, 0, s.y, 0, 1, 0, -s.y, 0, c.y));
}
)")
                                                                       .unwrap(),
                                                                   {},
                                                                   owe::ShaderType::GEOMETRY)
                                    .as_str());

    EXPECT_NE(out.find("mul(mul(mat3(c.z, -s.z"), std::string::npos);
    EXPECT_EQ(out.find("transpose(float3x3"), std::string::npos);
}

TEST(ShaderParser, CompileSceneShaderVariantAcceptsLeadingIntegerScalarMul) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "integer-scalar-mul-test"_Str;
    desc.shader_name = "integer-scalar-mul-test"_Str;
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/integer-scalar-mul-test.vert"_Str,
        .source     = R"(
attribute vec3 a_Position;
void main() {
    float value = 2.0;
    float scaled = mul(1, value / 50.0);
    gl_Position = vec4(a_Position + vec3(scaled * 0.0), 1.0);
}
)"_Str,
    });
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/integer-scalar-mul-test.frag"_Str,
        .source     = R"(
void main() {
    gl_FragColor = vec4(1.0);
}
)"_Str,
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << rstd::cppstd::as_string_view(result.error.as_str());
    ASSERT_TRUE(result.shader);
    ASSERT_EQ((*result.shader)->codes.len().to_primitive(), 2u);
}

TEST(ShaderParser, CompileSceneShaderVariantKeepsGlobalVariablesPrivate) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "private-global-test"_Str;
    desc.shader_name = "private-global-test"_Str;
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/private-global-test.vert"_Str,
        .source     = R"(
attribute vec3 a_Position;
void main() {
    gl_Position = vec4(a_Position, 1.0);
}
)"_Str,
    });
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/private-global-test.frag"_Str,
        .source     = R"(
float tint;
void main() {
    tint = 0.25;
    gl_FragColor = vec4(tint, tint, tint, 1.0);
}
)"_Str,
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << rstd::cppstd::as_string_view(result.error.as_str());
    ASSERT_TRUE(result.shader);
    for (const auto& block : (*result.shader)->uniform_blocks) {
        EXPECT_NE(block.name.as_str(), "$Global"_str);
    }
}

TEST(ShaderParser, PreShaderHeaderUsesHlslRectangularMatrixDimensions) {
    const std::string out =
        rstd::cppstd::to_string(owe::ShaderParser::PreShaderHeader(rstd::cppstd::as_str(
                                                                       R"(
mat2x3 glslMatrix() {
    return mat2x3(1, 2, 3, 4, 5, 6);
}
float3x2 hlslMatrix() {
    return float3x2(1, 2, 3, 4, 5, 6);
}
)")
                                                                       .unwrap(),
                                                                   {},
                                                                   owe::ShaderType::VERTEX)
                                    .as_str());

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
    auto         physical =
        owe::fs::make_physical_fs(owe::fs::Path(rstd::cppstd::as_str(root.string()).unwrap()));
    ASSERT_TRUE(physical.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(physical).unwrap_unchecked()).is_ok());

    const auto source = owe::ShaderParser::PreShaderSrc(
        vfs, "#include \"common_perspective.h\"\nvoid main(){}\n"_str, nullptr, {});
    const auto out = rstd::cppstd::as_string_view(source.as_str());

    EXPECT_EQ(out.find("_ww_perspective_mat"), std::string::npos);
    EXPECT_NE(out.find("return m;"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(ShaderParser, CompileSceneShaderVariantRejectsInvalidDescriptor) {
    owe::fs::VFS vfs;

    const auto result = owe::ShaderParser::CompileSceneShaderVariant({}, vfs);

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.shader);
    EXPECT_FALSE(result.error.is_empty());
}

TEST(ShaderParser, ComboDependenciesPropagateWithoutMutatingInputs) {
    auto        info = Parse(R"(
// [COMBO] {"combo":"A","default":0,"require":{"B":7}}
// [COMBO] {"combo":"B","default":2,"require":{"Z":1}}
// [COMBO] {"combo":"Z","default":0}
// [COMBO] {"combo":"NEGATIVE","default":-3}
// [COMBO] {"combo":"MISSING","default":0,"require":{"ABSENT":1}}
)");
    owe::Combos inputs;
    (void)inputs.insert("A"_Str, "9"_Str);
    (void)inputs.insert("B"_Str, "7"_Str);
    (void)inputs.insert("Z"_Str, "0"_Str);
    auto resolved = owe::ShaderParser::ResolveShaderCombos(info, inputs);
    EXPECT_FALSE(resolved.contains_key("A"_str));
    EXPECT_FALSE(resolved.contains_key("MISSING"_str));
    ASSERT_TRUE(resolved.contains_key("B"_str));
    EXPECT_EQ((**resolved.get("B"_str)).as_str(), "2"_str);
    ASSERT_TRUE(resolved.contains_key("NEGATIVE"_str));
    EXPECT_EQ((**resolved.get("NEGATIVE"_str)).as_str(), "-3"_str);
    EXPECT_EQ((**inputs.get("A"_str)).as_str(), "9"_str);
    EXPECT_EQ((**inputs.get("B"_str)).as_str(), "7"_str);
    (void)resolved.insert("Z"_Str, "changed"_Str);
    EXPECT_EQ((**inputs.get("Z"_str)).as_str(), "0"_str);
}

TEST(ShaderParser, ComboHeadersUseSortedKeysAndLatestValues) {
    owe::Combos combos;
    (void)combos.insert("zebra"_Str, "2"_Str);
    (void)combos.insert("alpha"_Str, "1"_Str);
    (void)combos.insert("alpha"_Str, "3"_Str);
    const auto header = rstd::cppstd::to_string(
        owe::ShaderParser::PreShaderHeader(
            rstd::cppstd::as_str("void main(){}").unwrap(), combos, owe::ShaderType::VERTEX)
            .as_str());
    EXPECT_NE(header.find("#define ALPHA 3\n#define ZEBRA 2\n"), std::string::npos);
    EXPECT_EQ((**combos.get("alpha"_str)).as_str(), "3"_str);
    EXPECT_FALSE(combos.contains_key("ALPHA"_str));
}

TEST(ShaderParser, CachedCombosOwnValuesAndOverwriteExistingKeys) {
    owe::fs::VFS      vfs;
    owe::ShaderCache  cache;
    const std::string source = R"(
// [COMBO] {"combo":"QUALITY","default":1}
)";
    ShaderInfo        first;
    owe::ShaderParser::PreShaderSrc(vfs, rstd::cppstd::as_str(source).unwrap(), &first, {}, &cache);
    (void)first.combos.insert("QUALITY"_Str, "changed"_Str);
    ShaderInfo second;
    (void)second.combos.insert("QUALITY"_Str, "stale"_Str);
    (void)second.combos.insert("KEEP"_Str, "7"_Str);
    owe::ShaderParser::PreShaderSrc(
        vfs, rstd::cppstd::as_str(source).unwrap(), &second, {}, &cache);
    ASSERT_TRUE(second.combos.contains_key("QUALITY"_str));
    EXPECT_EQ((**second.combos.get("QUALITY"_str)).as_str(), "1"_str);
    EXPECT_EQ((**second.combos.get("KEEP"_str)).as_str(), "7"_str);
    EXPECT_EQ((**first.combos.get("QUALITY"_str)).as_str(), "changed"_str);
}

TEST(ShaderParser, CachedUniformValuesRemainIndependent) {
    owe::fs::VFS     vfs;
    owe::ShaderCache cache;
    const auto       source = R"(
uniform float g_Brightness; // {"material":"brightness","default":1.0}
)"_str;
    ShaderInfo       first;
    auto             expanded = owe::ShaderParser::PreShaderSrc(vfs, source, &first, {}, &cache);
    ASSERT_TRUE(first.svs.contains_key("g_Brightness"_str));
    (void)first.svs.insert("g_Brightness"_Str, owe::ShaderValue(9.0f));
    expanded.clear();
    ShaderInfo second;
    (void)second.svs.insert("g_Brightness"_Str, owe::ShaderValue(3.0f));
    (void)second.svs.insert("u_Keep"_Str, owe::ShaderValue(7.0f));
    auto cached = owe::ShaderParser::PreShaderSrc(vfs, source, &second, {}, &cache);
    EXPECT_EQ(cached.as_str(), source);
    EXPECT_FLOAT_EQ((**second.svs.get("g_Brightness"_str))[usize()], 1.0f);
    EXPECT_FLOAT_EQ((**second.svs.get("u_Keep"_str))[usize()], 7.0f);
    EXPECT_FLOAT_EQ((**first.svs.get("g_Brightness"_str))[usize()], 9.0f);
}

TEST(ShaderParser, CachedAliasesOwnValuesAndOverwriteExistingKeys) {
    owe::fs::VFS      vfs;
    owe::ShaderCache  cache;
    const std::string source = R"(
uniform float g_Brightness; // {"material":"brightness","default":1.0}
)";
    ShaderInfo        first;
    const auto        initial = owe::ShaderParser::PreShaderSrc(
        vfs, rstd::cppstd::as_str(source).unwrap(), &first, {}, &cache);
    ASSERT_TRUE(first.alias.contains_key("brightness"_str));
    (void)first.alias.insert("brightness"_Str, "changed"_Str);

    ShaderInfo second;
    (void)second.alias.insert("brightness"_Str, "stale"_Str);
    (void)second.alias.insert("keep"_Str, "u_Keep"_Str);
    EXPECT_EQ(owe::ShaderParser::PreShaderSrc(
                  vfs, rstd::cppstd::as_str(source).unwrap(), &second, {}, &cache),
              initial);
    ASSERT_TRUE(second.alias.contains_key("brightness"_str));
    EXPECT_EQ((**second.alias.get("brightness"_str)).as_str(), "g_Brightness"_str);
    ASSERT_TRUE(second.alias.contains_key("keep"_str));
    EXPECT_EQ((**second.alias.get("keep"_str)).as_str(), "u_Keep"_str);
    EXPECT_EQ((**first.alias.get("brightness"_str)).as_str(), "changed"_str);
}

TEST(ShaderParser, CachedDefaultTexturesPreserveOrderAndOwnership) {
    owe::fs::VFS      vfs;
    owe::ShaderCache  cache;
    const std::string source = R"(
uniform sampler2D g_Texture3; // {"default":"util/first"}
uniform sampler2D g_Texture0; // {"default":"util/zero"}
uniform sampler2D g_Texture3; // {"default":"util/second"}
)";
    ShaderInfo        first;
    const auto        initial = owe::ShaderParser::PreShaderSrc(
        vfs, rstd::cppstd::as_str(source).unwrap(), &first, {}, &cache);
    ASSERT_EQ(first.defTexs.len(), usize(3));
    first.defTexs[usize()].texture.clear();

    ShaderInfo second;
    second.defTexs.push({ .slot = i32(7), .texture = "existing"_Str });
    EXPECT_EQ(owe::ShaderParser::PreShaderSrc(
                  vfs, rstd::cppstd::as_str(source).unwrap(), &second, {}, &cache),
              initial);
    ASSERT_EQ(second.defTexs.len(), usize(4));
    EXPECT_EQ(second.defTexs[usize()].texture.as_str(), "existing"_str);
    EXPECT_EQ(second.defTexs[usize(1)].slot, i32(3));
    EXPECT_EQ(second.defTexs[usize(1)].texture.as_str(), "util/first"_str);
    EXPECT_EQ(second.defTexs[usize(2)].slot, i32());
    EXPECT_EQ(second.defTexs[usize(2)].texture.as_str(), "util/zero"_str);
    EXPECT_EQ(second.defTexs[usize(3)].slot, i32(3));
    EXPECT_EQ(second.defTexs[usize(3)].texture.as_str(), "util/second"_str);
    cache.ReleaseTransientEntries();
    EXPECT_EQ(second.defTexs[usize(1)].texture.as_str(), "util/first"_str);
}

TEST(ShaderParser, LightingRequirementInjectsSceneLightInterface) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "lighting-v1-interface-test"_Str;
    desc.shader_name = "lighting-v1-interface-test"_Str;
    (void)desc.input_combos.insert("SCENE_ORTHO"_Str, "1"_Str);
    (void)desc.input_combos.insert("OWE_IMAGE_LAYER"_Str, "1"_Str);
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/lighting-v1-interface-test.vert"_Str,
        .source     = R"(
attribute vec3 a_Position;
varying vec3 v_WorldPos;
void main() {
    v_WorldPos = a_Position;
    gl_Position = vec4(a_Position, 1.0);
}
)"_Str,
    });
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/lighting-v1-interface-test.frag"_Str,
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
)"_Str,
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << rstd::cppstd::as_string_view(result.error.as_str());
    ASSERT_TRUE(result.shader);
    Vec<owe::vulkan::Uni_ShaderSpv> spvs;
    owe::vulkan::ShaderReflected    reflected;
    ASSERT_TRUE(owe::vulkan::GenReflect((*result.shader)->codes.as_slice(), spvs, reflected));
    ASSERT_EQ(reflected.blocks.len(), rstd::usize(1));
    EXPECT_EQ(reflected.blocks[rstd::usize()].name.as_str(), "ww_LightingUniforms"_str);
    EXPECT_EQ(reflected.blocks[rstd::usize()].set, 0u);
    EXPECT_EQ(reflected.blocks[rstd::usize()].binding, 2u);
    EXPECT_EQ(reflected.blocks[rstd::usize()].size, owe::kLightingUniformBlockSize.to_primitive());
    const auto& members = reflected.blocks[rstd::usize()].member_map;
    EXPECT_TRUE(members.contains_key("g_LightsPosition"_str));
    EXPECT_TRUE(members.contains_key("g_LightsColorRadius"_str));
    EXPECT_TRUE(members.contains_key("g_LightsDirectionType"_str));
    EXPECT_TRUE(members.contains_key("g_LightsConeExponent"_str));
    EXPECT_TRUE(members.contains_key("g_LightsCastShadow"_str));
}

TEST(ShaderParser, CompileSceneShaderVariantAcceptsPackedAudioSpectrumAccess) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "packed-audio-spectrum-test"_Str;
    desc.shader_name = "packed-audio-spectrum-test"_Str;
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/packed-audio-spectrum-test.vert"_Str,
        .source     = R"(
attribute vec3 a_Position;
varying vec2 v_TexCoord;
void main() {
    v_TexCoord = a_Position.xy;
    gl_Position = vec4(a_Position, 1.0);
}
)"_Str,
    });
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/packed-audio-spectrum-test.frag"_Str,
        .source     = R"(
varying vec2 v_TexCoord;
uniform float g_AudioSpectrum64Left[64];
void main() {
    float barID = v_TexCoord.x * 8.0;
    float value = g_AudioSpectrum64Left[barID / 4][barID % 4];
    gl_FragColor = vec4(value, value, value, 1.0);
}
)"_Str,
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << rstd::cppstd::as_string_view(result.error.as_str());
    ASSERT_TRUE(result.shader);
    ASSERT_EQ((*result.shader)->codes.len().to_primitive(), 2u);
    EXPECT_FALSE((*result.shader)->codes[usize(1)].is_empty());

    Vec<owe::vulkan::Uni_ShaderSpv> spvs;
    owe::vulkan::ShaderReflected    reflected;
    ASSERT_TRUE(owe::vulkan::GenReflect((*result.shader)->codes.as_slice(), spvs, reflected));
    ASSERT_EQ(reflected.blocks.len(), rstd::usize(1));
    EXPECT_EQ(reflected.blocks[rstd::usize()].name.as_str(), "ww_AudioUniforms"_str);
    EXPECT_EQ(reflected.blocks[rstd::usize()].set, 0u);
    EXPECT_EQ(reflected.blocks[rstd::usize()].binding, 1u);
    EXPECT_EQ(reflected.blocks[rstd::usize()].size, owe::kAudioUniformBlockSize.to_primitive());
    const auto& members = reflected.blocks[rstd::usize()].member_map;
    ASSERT_TRUE(members.contains_key("g_AudioSpectrum64Left"_str));
    const auto& spectrum = *members.get("g_AudioSpectrum64Left"_str).unwrap();
    EXPECT_EQ(spectrum.scalar_kind, owe::ShaderScalarKind::Float);
    EXPECT_EQ(spectrum.scalar_width, 32u);
    EXPECT_EQ(spectrum.vector_components, 1u);
    EXPECT_EQ(spectrum.num, rstd::usize(64));
    EXPECT_EQ(spectrum.array_stride, 16u);
}

TEST(ShaderParser, CompileSceneShaderVariantPacksLargeVaryingArrays) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "large-varying-array-test"_Str;
    desc.shader_name = "large-varying-array-test"_Str;
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/large-varying-array-test.vert"_Str,
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
)"_Str,
    });
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/large-varying-array-test.frag"_Str,
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
)"_Str,
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << rstd::cppstd::as_string_view(result.error.as_str());
    ASSERT_TRUE(result.shader);
    ASSERT_EQ((*result.shader)->codes.len().to_primitive(), 2u);
    for (std::size_t stage = 0; stage < (*result.shader)->codes.len().to_primitive(); ++stage) {
        const auto&            code = (*result.shader)->codes[usize(stage)];
        SpvReflectShaderModule module {};
        ASSERT_EQ(spvReflectCreateShaderModule(
                      code.len().to_primitive() * sizeof(uint32_t), code.data(), &module),
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
    desc.scene_id    = "narrow-producer-varying-test"_Str;
    desc.shader_name = "narrow-producer-varying-test"_Str;
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/narrow-producer-varying-test.vert"_Str,
        .source     = R"(
attribute vec3 a_Position;
varying vec2 v_TexCoord;
void main() {
    v_TexCoord = a_Position.xy;
    gl_Position = vec4(a_Position, 1.0);
}
)"_Str,
    });
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/narrow-producer-varying-test.frag"_Str,
        .source     = R"(
varying vec4 v_TexCoord;
void main() {
    vec2 uv = v_TexCoord * vec2(1.0, 1.0);
    gl_FragColor = vec4(uv, 0.0, 1.0);
}
)"_Str,
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << rstd::cppstd::as_string_view(result.error.as_str());
    ASSERT_TRUE(result.shader);
    ASSERT_EQ((*result.shader)->codes.len().to_primitive(), 2u);
}

TEST(ShaderParser, CompileSceneShaderVariantUsesWideProducerVaryingType) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "wide-producer-varying-test"_Str;
    desc.shader_name = "wide-producer-varying-test"_Str;
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/wide-producer-varying-test.vert"_Str,
        .source     = R"(
attribute vec3 a_Position;
varying vec4 v_TexCoord;
void main() {
    v_TexCoord = vec4(a_Position.xy, 0.25, 0.75);
    gl_Position = vec4(a_Position, 1.0);
}
)"_Str,
    });
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/wide-producer-varying-test.frag"_Str,
        .source     = R"(
varying vec2 v_TexCoord;
void main() {
    gl_FragColor = vec4(v_TexCoord.zw, 0.0, 1.0);
}
)"_Str,
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << rstd::cppstd::as_string_view(result.error.as_str());
    ASSERT_TRUE(result.shader);
    ASSERT_EQ((*result.shader)->codes.len().to_primitive(), 2u);
}

TEST(ShaderParser, CompileSceneShaderVariantShapesCrossStageUniformDefaults) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "cross-stage-uniform-default-test"_Str;
    desc.shader_name = "cross-stage-uniform-default-test"_Str;
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/cross-stage-uniform-default-test.vert"_Str,
        .source     = R"(
attribute vec3 a_Position;
uniform vec2 u_refResolution; // {"material":"resolution","default":"512 512"}
void main() {
    gl_Position = vec4(a_Position.xy / u_refResolution, a_Position.z, 1.0);
}
)"_Str,
    });
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/cross-stage-uniform-default-test.frag"_Str,
        .source     = R"(
uniform float u_refResolution; // {"material":"resolution","default":512}
void main() {
    gl_FragColor = vec4(1.0);
}
)"_Str,
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << rstd::cppstd::as_string_view(result.error.as_str());
    ASSERT_TRUE(result.shader);
    const auto& shader_default = (**(*result.shader)->default_uniforms.get("u_refResolution"_str));
    ASSERT_EQ(shader_default.size(), rstd::usize(2));
    EXPECT_FLOAT_EQ(shader_default[rstd::usize()], 512.0f);
    EXPECT_FLOAT_EQ(shader_default[rstd::usize(1)], 512.0f);
    const auto& variant_default = (**result.variant.default_uniforms.get("u_refResolution"_str));
    EXPECT_EQ(variant_default.size(), rstd::usize(2));
}

TEST(ShaderParser, CompileSceneShaderVariantAcceptsNativeMatrixConstructors) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "matrix-constructor-test"_Str;
    desc.shader_name = "matrix-constructor-test"_Str;
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/matrix-constructor-test.vert"_Str,
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
)"_Str,
    });
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/matrix-constructor-test.frag"_Str,
        .source     = R"(
void main() {
    gl_FragColor = vec4(1.0);
}
)"_Str,
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << rstd::cppstd::as_string_view(result.error.as_str());
    ASSERT_TRUE(result.shader);
    ASSERT_EQ((*result.shader)->codes.len().to_primitive(), 2u);
    EXPECT_FALSE((*result.shader)->codes[usize(0)].is_empty());
    EXPECT_FALSE((*result.shader)->codes[usize(1)].is_empty());
}

TEST(ShaderParser, ReflectsNativeHlslMatrixLayout) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "matrix-reflection-test"_Str;
    desc.shader_name = "matrix-reflection-test"_Str;
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/matrix-reflection-test.vert"_Str,
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
)"_Str,
    });
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/matrix-reflection-test.frag"_Str,
        .source     = R"(
void main() {
    gl_FragColor = vec4(1.0);
}
)"_Str,
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << rstd::cppstd::as_string_view(result.error.as_str());
    ASSERT_TRUE(result.shader);
    EXPECT_EQ((*result.shader)->matrix_convention, owe::ShaderMatrixConvention::RowVector);
    EXPECT_EQ((*result.shader)->matrix_abi, owe::ShaderMatrixAbi::Hlsl);
    Vec<owe::vulkan::Uni_ShaderSpv> spvs;
    owe::vulkan::ShaderReflected    reflected;
    ASSERT_TRUE(owe::vulkan::GenReflect((*result.shader)->codes.as_slice(), spvs, reflected));
    ASSERT_EQ(reflected.blocks.len(), rstd::usize(1));
    const auto local =
        std::find_if(reflected.blocks.begin(), reflected.blocks.end(), [](const auto& block) {
            return block.name.as_str() == "ww_DrawUniforms"_str;
        });
    ASSERT_NE(local, reflected.blocks.end());
    const auto& members = local->member_map;
    ASSERT_TRUE(members.contains_key("g_Mat2"_str));
    ASSERT_TRUE(members.contains_key("g_Mat3"_str));
    ASSERT_TRUE(members.contains_key("g_Mat4"_str));
    ASSERT_TRUE(members.contains_key("g_Bones"_str));

    const auto& mat3 = *members.get("g_Mat3"_str).unwrap();
    EXPECT_EQ(mat3.scalar_kind, owe::ShaderScalarKind::Float);
    EXPECT_EQ(mat3.scalar_width, 32u);
    EXPECT_EQ(mat3.matrix_rows, 3u);
    EXPECT_EQ(mat3.matrix_columns, 3u);
    EXPECT_EQ(mat3.matrix_major, owe::ShaderMatrixMajor::Row);
    EXPECT_EQ(mat3.matrix_stride, 16u);

    const auto& bones = *members.get("g_Bones"_str).unwrap();
    EXPECT_EQ(bones.matrix_rows, 3u);
    EXPECT_EQ(bones.matrix_columns, 4u);
    EXPECT_EQ(bones.matrix_major, owe::ShaderMatrixMajor::Row);
    EXPECT_EQ(bones.matrix_stride, 16u);
    EXPECT_EQ(bones.array_stride, 48u);
    EXPECT_EQ(bones.num, rstd::usize(2));
    ASSERT_EQ(bones.array_dimensions.len(), rstd::usize(1));
    EXPECT_EQ(bones.array_dimensions[rstd::usize()], rstd::u32(2));
}

TEST(ShaderParser, UsesOneCanonicalGlobalAbiForLegacyDaytimeAlias) {
    auto compile = [](std::string_view declaration, std::string_view expression) {
        owe::SceneShaderVariantDesc desc;
        desc.scene_id    = "global-uniform-abi-test"_Str;
        desc.shader_name = "global-uniform-abi-test"_Str;
        desc.stages.push(owe::SceneShaderVariantStage {
            .stage      = owe::ShaderType::VERTEX,
            .source_key = "/assets/shaders/global-uniform-abi-test.vert"_Str,
            .source     = rstd::format(
                "attribute vec3 a_Position;\nuniform float {};\nvoid main() {{ gl_Position = "
                "vec4(a_Position.x + {} * 0.0, a_Position.yz, 1.0); }}\n",
                rstd::cppstd::as_str(declaration).unwrap(),
                rstd::cppstd::as_str(expression).unwrap()),
        });
        desc.stages.push(owe::SceneShaderVariantStage {
            .stage      = owe::ShaderType::FRAGMENT,
            .source_key = "/assets/shaders/global-uniform-abi-test.frag"_Str,
            .source     = "void main() { gl_FragColor = vec4(1.0); }"_Str,
        });
        owe::fs::VFS vfs;
        return owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);
    };

    const auto canonical = compile("g_Daytime", "g_Daytime");
    const auto legacy    = compile("g_DayTime", "g_DayTime");
    ASSERT_TRUE(canonical.ok) << canonical.error;
    ASSERT_TRUE(legacy.ok) << legacy.error;
    ASSERT_EQ((*canonical.shader)->descriptor_sets.len().to_primitive(), 2u);
    ASSERT_EQ((*legacy.shader)->descriptor_sets.len().to_primitive(), 2u);
    EXPECT_EQ((*canonical.shader)->descriptor_sets[usize()].identity,
              (*legacy.shader)->descriptor_sets[usize()].identity);

    Vec<owe::vulkan::Uni_ShaderSpv> canonical_spvs;
    Vec<owe::vulkan::Uni_ShaderSpv> legacy_spvs;
    owe::vulkan::ShaderReflected    canonical_reflection;
    owe::vulkan::ShaderReflected    legacy_reflection;
    ASSERT_TRUE(owe::vulkan::GenReflect(
        (*canonical.shader)->codes.as_slice(), canonical_spvs, canonical_reflection));
    ASSERT_TRUE(owe::vulkan::GenReflect(
        (*legacy.shader)->codes.as_slice(), legacy_spvs, legacy_reflection));
    const auto& canonical_members = canonical_reflection.blocks[rstd::usize()].member_map;
    const auto& legacy_members    = legacy_reflection.blocks[rstd::usize()].member_map;
    EXPECT_EQ(canonical_members.len(), legacy_members.len());
    EXPECT_TRUE(legacy_members.contains_key("g_Daytime"_str));
    EXPECT_FALSE(legacy_members.contains_key("g_DayTime"_str));
    const auto global = std::find_if(canonical_reflection.blocks.begin(),
                                     canonical_reflection.blocks.end(),
                                     [](const auto& block) {
                                         return block.name.as_str() == "ww_GlobalUniforms"_str;
                                     });
    ASSERT_NE(global, canonical_reflection.blocks.end());
    EXPECT_EQ(global->size, owe::kFrameUniformBlockSize.to_primitive());
    for (const auto& field : owe::GlobalUniformFields()) {
        if (owe::GlobalUniformBlockFor(field.producer) != owe::GlobalUniformBlockKind::Frame)
            continue;
        auto member = global->member_map.get(field.name);
        ASSERT_TRUE(member.is_some());
        EXPECT_EQ((**member).offset, field.offset.to_primitive());
    }
    EXPECT_EQ(canonical_reflection.blocks.len(), rstd::usize(1));
}

TEST(ShaderParser, KeepsShadowMatricesInCanonicalGlobalSet) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "shadow-global-uniform-abi-test"_Str;
    desc.shader_name = "shadow-global-uniform-abi-test"_Str;
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/shadow-global-uniform-abi-test.vert"_Str,
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
)"_Str,
    });
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/shadow-global-uniform-abi-test.frag"_Str,
        .source     = "void main() { gl_FragColor = vec4(1.0); }"_Str,
    });

    owe::fs::VFS vfs;
    auto         compile = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);
    ASSERT_TRUE(compile.ok) << compile.error;

    Vec<owe::vulkan::Uni_ShaderSpv> spvs;
    owe::vulkan::ShaderReflected    reflection;
    ASSERT_TRUE(owe::vulkan::GenReflect((*compile.shader)->codes.as_slice(), spvs, reflection));
    const auto global =
        std::find_if(reflection.blocks.begin(), reflection.blocks.end(), [](const auto& block) {
            return block.name.as_str() == "ww_LightingUniforms"_str;
        });
    ASSERT_NE(global, reflection.blocks.end());
    const auto matrices = global->member_map.get("g_ViewportViewProjectionMatrices"_str);
    ASSERT_TRUE(matrices.is_some());
    EXPECT_EQ((**matrices).num, rstd::usize(6));
    EXPECT_EQ(reflection.input_location_map.len(), rstd::usize(1));
    EXPECT_TRUE(reflection.input_location_map.contains_key("a_Position"_str));
}

TEST(ShaderParser, FallsBackAsOneLegacyInterfaceOnGlobalTypeConflict) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "global-uniform-conflict-test"_Str;
    desc.shader_name = "global-uniform-conflict-test"_Str;
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/global-uniform-conflict-test.vert"_Str,
        .source     = R"(
attribute vec3 a_Position;
uniform vec2 g_Time;
void main() { gl_Position = vec4(a_Position.xy + g_Time * 0.0, a_Position.z, 1.0); }
)"_Str,
    });
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/global-uniform-conflict-test.frag"_Str,
        .source     = R"(
uniform sampler2D g_Texture0;
void main() { gl_FragColor = texSample2D(g_Texture0, vec2(0.5)); }
)"_Str,
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);
    ASSERT_TRUE(result.ok) << rstd::cppstd::as_string_view(result.error.as_str());
    ASSERT_EQ((*result.shader)->descriptor_sets.len().to_primitive(), 1u);
    EXPECT_EQ((*result.shader)->descriptor_sets[usize()].set, rstd::u32(1));
    EXPECT_TRUE((*result.shader)->descriptor_sets[usize()].push_descriptor);
    ASSERT_EQ((*result.shader)->uniform_blocks.len().to_primitive(), 1u);
    EXPECT_EQ((*result.shader)->uniform_blocks[usize()].name.as_str(), "ww_Uniforms"_str);
    EXPECT_EQ((*result.shader)->uniform_blocks[usize()].set, rstd::u32(1));
    EXPECT_EQ((*result.shader)->uniform_blocks[usize()].scope,
              owe::SceneShaderUniformBlockScope::Local);
}

TEST(ShaderParser, PrunesUnusedDrawUniformsAndInactiveGlobalBlocks) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "draw-uniform-prune-test"_Str;
    desc.shader_name = "draw-uniform-prune-test"_Str;
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/draw-uniform-prune-test.vert"_Str,
        .source     = R"(
attribute vec3 a_Position;
uniform float g_Used;
uniform float g_Unused;
void main() { gl_Position = vec4(a_Position.x + g_Used, a_Position.yz, 1.0); }
)"_Str,
    });
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/draw-uniform-prune-test.frag"_Str,
        .source     = "void main() { gl_FragColor = vec4(1.0); }"_Str,
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);
    ASSERT_TRUE(result.ok) << rstd::cppstd::as_string_view(result.error.as_str());
    Vec<owe::vulkan::Uni_ShaderSpv> spvs;
    owe::vulkan::ShaderReflected    reflected;
    ASSERT_TRUE(owe::vulkan::GenReflect((*result.shader)->codes.as_slice(), spvs, reflected));
    const auto local =
        std::find_if(reflected.blocks.begin(), reflected.blocks.end(), [](const auto& block) {
            return block.name.as_str() == "ww_DrawUniforms"_str;
        });
    ASSERT_NE(local, reflected.blocks.end());
    EXPECT_TRUE(local->member_map.contains_key("g_Used"_str));
    EXPECT_FALSE(local->member_map.contains_key("g_Unused"_str));
    const auto global =
        std::find_if(reflected.blocks.begin(), reflected.blocks.end(), [](const auto& block) {
            return block.name.as_str() == "ww_GlobalUniforms"_str ||
                   block.name.as_str() == "ww_AudioUniforms"_str ||
                   block.name.as_str() == "ww_LightingUniforms"_str;
        });
    EXPECT_EQ(global, reflected.blocks.end());
}

TEST(ShaderParser, SplitsActiveGlobalUniformsAcrossSetZeroBindings) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "split-global-uniform-abi-test"_Str;
    desc.shader_name = "split-global-uniform-abi-test"_Str;
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/split-global-uniform-abi-test.vert"_Str,
        .source     = R"(
attribute vec3 a_Position;
uniform float g_Time;
uniform float g_AudioSpectrum16Left[16];
uniform vec3 g_LightsPosition[4];
void main() {
    float inputValue = g_Time + g_AudioSpectrum16Left[0] + g_LightsPosition[0].x;
    gl_Position = vec4(a_Position.x + inputValue * 0.0, a_Position.yz, 1.0);
}
)"_Str,
    });
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/split-global-uniform-abi-test.frag"_Str,
        .source     = "void main() { gl_FragColor = vec4(1.0); }"_Str,
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);
    ASSERT_TRUE(result.ok) << rstd::cppstd::as_string_view(result.error.as_str());
    ASSERT_EQ((*result.shader)->uniform_blocks.len().to_primitive(), 3u);
    for (const auto& block : owe::GlobalUniformBlocks()) {
        const auto reflected = std::find_if((*result.shader)->uniform_blocks.begin(),
                                            (*result.shader)->uniform_blocks.end(),
                                            [&](const auto& candidate) {
                                                return candidate.name.as_str() == block.name;
                                            });
        ASSERT_NE(reflected, (*result.shader)->uniform_blocks.end());
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
    desc.scene_id    = "physical-cache-test"_Str;
    desc.shader_name = "physical-cache-test"_Str;
    desc.texture_infos.resize(usize(1), owe::SceneShaderTextureCompileInfo {});
    desc.texture_infos[usize(0)].enabled = true;
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/physical-cache-test.vert"_Str,
        .source     = R"(
attribute vec3 a_Position;
varying vec2 v_TexCoord;
void main() {
    v_TexCoord = a_Position.xy;
    gl_Position = vec4(a_Position, 1.0);
}
)"_Str,
    });
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/physical-cache-test.frag"_Str,
        .source     = R"(
varying vec2 v_TexCoord;
uniform float g_Brightness;
uniform sampler2D g_Texture0;
void main() {
    gl_FragColor = texSample2D(g_Texture0, v_TexCoord) * g_Brightness;
}
)"_Str,
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

    owe::ShaderCache memory_cache;
    auto memory_first = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs, {}, &memory_cache);
    ASSERT_TRUE(memory_first.ok) << memory_first.error;
    ASSERT_TRUE(memory_first.shader);
    auto original_codes = (*memory_first.shader)->codes.clone();
    ASSERT_FALSE(original_codes.is_empty());
    ASSERT_FALSE(original_codes[usize()].is_empty());
    (*memory_first.shader)->codes[usize()][usize()] = 0u;
    (void)memory_first.variant.stages[usize(1)].uniforms.insert("g_Brightness"_Str, "vec4"_Str);
    auto memory_second = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs, {}, &memory_cache);
    ASSERT_TRUE(memory_second.ok) << memory_second.error;
    ASSERT_TRUE(memory_second.shader);
    EXPECT_EQ((*memory_second.shader)->codes, original_codes);
    ASSERT_TRUE(memory_second.variant.stages[usize(1)].uniforms.contains_key("g_Brightness"_str));
    EXPECT_EQ((**memory_second.variant.stages[usize(1)].uniforms.get("g_Brightness"_str)).as_str(),
              "float"_str);
    EXPECT_EQ((*memory_second.shader)->codes, (*first.shader)->codes);
    (*memory_second.shader)->codes.clear();
    auto memory_third = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs, {}, &memory_cache);
    ASSERT_TRUE(memory_third.ok) << memory_third.error;
    ASSERT_TRUE(memory_third.shader);
    EXPECT_EQ((*memory_third.shader)->codes, original_codes);

    const auto shader_cache = root / rstd::cppstd::to_string(desc.scene_id.as_str()) / "spvs03";
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
    EXPECT_EQ((*second.shader)->codes, (*first.shader)->codes);
    EXPECT_EQ(second.variant.descriptor_layout_hash, first.variant.descriptor_layout_hash);
    EXPECT_EQ(second.variant.descriptor_sets, first.variant.descriptor_sets);
    EXPECT_EQ(second.variant.uniform_blocks, first.variant.uniform_blocks);
    EXPECT_EQ(owe::SceneShaderCodeHash(**second.shader), owe::SceneShaderCodeHash(**first.shader));
    EXPECT_TRUE(second.variant.stages[usize(1)].uniforms.contains_key("g_Brightness"_str));
    EXPECT_TRUE(second.variant.stages[usize(1)].active_texture_slots.contains(u32()));
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(shader_cache),
                            std::filesystem::directory_iterator {}),
              1);
    EXPECT_EQ(std::filesystem::last_write_time(artifact_path), initial_write_time);

    std::filesystem::resize_file(artifact_path, 16);
    const auto after_truncation = compile_cached(desc);
    ASSERT_TRUE(after_truncation.ok) << after_truncation.error;
    ASSERT_TRUE(after_truncation.shader);
    EXPECT_EQ((*after_truncation.shader)->codes, (*first.shader)->codes);
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
    EXPECT_EQ((*after_identity_corruption.shader)->codes, (*first.shader)->codes);

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
    EXPECT_EQ((*after_payload_corruption.shader)->codes, (*first.shader)->codes);

    owe::Combos cache_override;
    (void)cache_override.insert("CACHE_VARIANT"_Str, "1"_Str);
    const auto different_combo = compile_cached(desc, cache_override);
    ASSERT_TRUE(different_combo.ok) << different_combo.error;
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(shader_cache),
                            std::filesystem::directory_iterator {}),
              2);

    desc.stages[usize(1)].source.push_str("\n// source identity variant"_str);
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
    desc.scene_id    = "sampler-binding-test"_Str;
    desc.shader_name = "sampler-binding-test"_Str;
    desc.default_textures.push({ .slot = i32(3), .texture = "stale"_Str });
    desc.default_textures.push({ .slot = i32(5), .texture = "util/fallback"_Str });
    desc.default_textures.push({ .slot = i32(5), .texture = "ignored"_Str });
    desc.texture_infos.resize(usize(4), owe::SceneShaderTextureCompileInfo {});
    desc.texture_infos[usize(3)].enabled = true;
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/sampler-binding-test.vert"_Str,
        .source     = R"(
attribute vec3 a_Position;
varying vec2 v_TexCoord;
void main() {
    v_TexCoord = a_Position.xy;
    gl_Position = vec4(a_Position, 1.0);
}
)"_Str,
    });
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/sampler-binding-test.frag"_Str,
        .source     = R"(
varying vec2 v_TexCoord;
uniform sampler2D g_Texture3; // {"default":"util/parsed"}
void main() {
    gl_FragColor = texSample2D(g_Texture3, v_TexCoord);
}
)"_Str,
    });

    owe::fs::VFS vfs;
    auto         result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << rstd::cppstd::as_string_view(result.error.as_str());
    ASSERT_TRUE(result.shader);
    ASSERT_EQ(result.variant.sampler_bindings.len().to_primitive(), 1u);
    EXPECT_EQ(result.variant.sampler_bindings[usize()].texture_slot, 3u);
    EXPECT_EQ(result.variant.sampler_bindings[usize()].shader_member, "g_Texture3"_str);
    EXPECT_EQ((*result.shader)->SamplerMember(3), "g_Texture3"_str);
    ASSERT_EQ(result.info.defTexs.len(), usize(2));
    ASSERT_EQ(result.variant.default_textures.len(), usize(2));
    EXPECT_EQ(result.variant.default_textures[usize()].slot, i32(3));
    EXPECT_EQ(result.variant.default_textures[usize()].texture.as_str(), "util/parsed"_str);
    EXPECT_EQ(result.variant.default_textures[usize(1)].slot, i32(5));
    EXPECT_EQ(result.variant.default_textures[usize(1)].texture.as_str(), "util/fallback"_str);
    desc.default_textures[usize(1)].texture.clear();
    result.info.defTexs[usize()].texture.clear();
    result.info.defTexs[usize(1)].texture.clear();
    EXPECT_EQ(result.variant.default_textures[usize()].texture.as_str(), "util/parsed"_str);
    EXPECT_EQ(result.variant.default_textures[usize(1)].texture.as_str(), "util/fallback"_str);
}

TEST(ShaderParser, CompileSceneShaderVariantUsesDescriptorAndComboOverride) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "variant-test"_Str;
    desc.shader_name = "variant-test"_Str;
    (void)desc.resolved_combos.insert("USE_COLOR"_Str, "0"_Str);
    (void)desc.uniform_aliases.insert("brightness"_Str, "stale"_Str);
    (void)desc.uniform_aliases.insert("fallback"_Str, "u_Fallback"_Str);
    desc.texture_infos.push(owe::SceneShaderTextureCompileInfo { .enabled = false });
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/variant-test.vert"_Str,
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
)"_Str,
    });
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/variant-test.frag"_Str,
        .source     = R"(
varying vec4 v_Color;
uniform float g_Brightness; // {"material":"brightness","default":1.0,"range":[0,2]}
void main() {
    gl_FragColor = v_Color * g_Brightness;
}
)"_Str,
    });

    owe::fs::VFS vfs;
    owe::Combos  overrides;
    (void)overrides.insert("USE_COLOR"_Str, "1"_Str);
    const auto result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs, overrides);

    ASSERT_TRUE(result.ok) << rstd::cppstd::as_string_view(result.error.as_str());
    ASSERT_TRUE(result.shader);
    EXPECT_EQ((*result.shader)->name.as_str(), "variant-test"_str);
    ASSERT_EQ((*result.shader)->codes.len().to_primitive(), 2u);
    EXPECT_FALSE((*result.shader)->codes[usize(0)].is_empty());
    ASSERT_TRUE(result.variant.resolved_combos.contains_key("USE_COLOR"_str));
    EXPECT_EQ((**result.variant.resolved_combos.get("USE_COLOR"_str)).as_str(), "1"_str);
    ASSERT_TRUE(result.variant.input_combos.contains_key("USE_COLOR"_str));
    EXPECT_EQ((**result.variant.input_combos.get("USE_COLOR"_str)).as_str(), "1"_str);
    ASSERT_TRUE(result.variant.uniform_aliases.get("brightness"_str).is_some());
    EXPECT_EQ((**result.variant.uniform_aliases.get("brightness"_str)).as_str(),
              "g_Brightness"_str);
    EXPECT_TRUE(result.variant.default_uniforms.contains_key("g_Brightness"_str));
    ASSERT_TRUE(result.variant.uniform_aliases.contains_key("fallback"_str));
    EXPECT_EQ((**result.variant.uniform_aliases.get("fallback"_str)).as_str(), "u_Fallback"_str);
    EXPECT_NE(result.variant.descriptor_layout_hash, usize());
    ASSERT_EQ(result.variant.stages.len().to_primitive(), 2u);
    EXPECT_EQ(result.variant.stages[usize(0)].source_key, "/assets/shaders/variant-test.vert"_str);
    EXPECT_NE(result.variant.stages[usize(0)].code_hash, rstd::usize());
    EXPECT_TRUE(result.variant.stages[usize(1)].uniforms.contains_key("g_Brightness"_str));
    EXPECT_NE(result.variant.stages[usize(1)].code_hash, rstd::usize());
}

TEST(ShaderParser, CompileSceneShaderVariantResolvesRequiredComboDefaults) {
    owe::SceneShaderVariantDesc desc;
    desc.scene_id    = "required-combo-test"_Str;
    desc.shader_name = "required-combo-test"_Str;
    (void)desc.input_combos.insert("HATCH"_Str, "0"_Str);
    (void)desc.input_combos.insert("LINE_COUNT"_Str, "3"_Str);
    (void)desc.input_combos.insert("LINE_STYLE2"_Str, "8"_Str);
    (void)desc.input_combos.insert("LINE_STYLE3"_Str, "3"_Str);
    (void)desc.input_combos.insert("SHAPE_VARIATION"_Str, "0"_Str);
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/required-combo-test.vert"_Str,
        .source     = R"(
attribute vec3 a_Position;
void main() {
    gl_Position = vec4(a_Position, 1.0);
}
)"_Str,
    });
    desc.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/required-combo-test.frag"_Str,
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
)"_Str,
    });

    owe::fs::VFS vfs;
    const auto   result = owe::ShaderParser::CompileSceneShaderVariant(desc, vfs);

    ASSERT_TRUE(result.ok) << rstd::cppstd::as_string_view(result.error.as_str());
    EXPECT_FALSE(result.variant.resolved_combos.contains_key("LINE_STYLE2"_str));
    ASSERT_TRUE(result.variant.resolved_combos.contains_key("LINE_STYLE3"_str));
    EXPECT_EQ((**result.variant.resolved_combos.get("LINE_STYLE3"_str)).as_str(), "3"_str);
    ASSERT_TRUE(result.variant.resolved_combos.contains_key("SHAPE_VARIATION"_str));
    EXPECT_EQ((**result.variant.resolved_combos.get("SHAPE_VARIATION"_str)).as_str(), "1"_str);
    ASSERT_TRUE(result.variant.input_combos.contains_key("LINE_STYLE2"_str));
    EXPECT_EQ((**result.variant.input_combos.get("LINE_STYLE2"_str)).as_str(), "8"_str);
    ASSERT_TRUE(result.variant.input_combos.contains_key("SHAPE_VARIATION"_str));
    EXPECT_EQ((**result.variant.input_combos.get("SHAPE_VARIATION"_str)).as_str(), "0"_str);
}

TEST(ShaderParser, NativeIncludesPreserveNestedOrderMalformedLinesAndUtf8) {
    auto temporary = rstd::fs::TempDir::make("owe-native-includes"_str).unwrap();
    auto root      = rstd::path::PathBuf::from(temporary.path().as_os_str().to_os_string());
    ASSERT_TRUE(
        rstd::fs::write(
            root.join("outer.h"_str).as_path(),
            "// outer-before\n#include \"inner.h\"\n#include invalid\n// outer-after\n"_bytes)
            .is_ok());
    ASSERT_TRUE(rstd::fs::write(root.join("inner.h"_str).as_path(),
                                "// inner-\xc3\xa9\nfloat nestedValue = 1.0;\n"_bytes)
                    .is_ok());
    owe::fs::VFS vfs;
    auto         physical = owe::fs::make_physical_fs(temporary.path());
    ASSERT_TRUE(physical.is_ok());
    ASSERT_TRUE(vfs.mount("/assets/shaders"_str, rstd::move(physical).unwrap()).is_ok());

    auto source = owe::ShaderParser::PreShaderSrc(
        vfs, "// head\n#include \"outer.h\"\n// tail\n"_str, nullptr, {});
    auto text   = source.as_str();
    auto before = text->find("// outer-before"_str);
    auto inner  = text->find("// inner-\xc3\xa9"_str);
    auto after  = text->find("// outer-after"_str);
    ASSERT_TRUE(before.is_some());
    ASSERT_TRUE(inner.is_some());
    ASSERT_TRUE(after.is_some());
    EXPECT_LT(*before, *inner);
    EXPECT_LT(*inner, *after);
    EXPECT_TRUE(text->contains("#include invalid"_str));
    EXPECT_TRUE(text->contains("// head"_str));
    EXPECT_TRUE(text->contains("// tail"_str));
}
