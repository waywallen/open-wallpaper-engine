#include <rstd/test/gtest.hpp>

import rstd;
import rstd.json;
import wescene.json;
import owe.user_property;
import wescene.scene;
import wescene.fs;
import wescene.types;
import wescene.scene_user_property;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

namespace
{

struct FakeParticleOverrideControl {
    float* observed;

    void Apply(slice<float> values) {
        if (! values.is_empty()) *observed = values[usize()];
    }
};

} // namespace

TEST(SceneUserProperty, CanonicalizesHostSchemeColor) {
    EXPECT_EQ(owe::CanonicalSceneUserPropertyKey("waywallen.scheme_color"_str), "schemecolor"_str);
    EXPECT_EQ(owe::CanonicalSceneUserPropertyKey("custom"_str), "custom"_str);
}

TEST(SceneUserProperty, ResolvesClampedColorDescriptor) {
    auto property = rstd::json::from_str(R"({"type":"color","value":"-0.2 0.4 1.4"})"_str).unwrap();
    auto color    = owe::ResolveSceneUserPropertyColor(property);

    ASSERT_TRUE(color.is_some());
    EXPECT_FLOAT_EQ((*color)[usize()], 0.0f);
    EXPECT_FLOAT_EQ((*color)[usize(1)], 0.4f);
    EXPECT_FLOAT_EQ((*color)[usize(2)], 1.0f);
}

TEST(SceneUserProperty, ColorClampPreservesNegativeZero) {
    auto property =
        rstd::json::from_str(R"({"type":"color","value":"-0.0 -1.0 2.0"})"_str).unwrap();
    auto color = owe::ResolveSceneUserPropertyColor(property);
    ASSERT_TRUE(color.is_some());
    EXPECT_EQ(f32((*color)[usize()]).to_bits(), f32(-0.0f).to_bits());
    EXPECT_EQ((*color)[usize(1)], 0.0f);
    EXPECT_EQ((*color)[usize(2)], 1.0f);
}

TEST(SceneUserProperty, ResolvesResetWireValueFromExistingDescriptor) {
    auto schema = rstd::json::from_str(R"({"type":"color","value":"0.2 0.4 0.6"})"_str).unwrap();
    auto overridden =
        owe::MergeUserPropertyDescriptor(schema, owe::MakeUserPropertyWirePatch("0.8 0.7 0.6"_str));
    auto reset = owe::MergeUserPropertyDescriptor(
        overridden, owe::MakeUserPropertyWirePatch("0.2 0.4 0.6"_str));
    auto color = owe::ResolveSceneUserPropertyColor(reset);

    ASSERT_TRUE(color.is_some());
    EXPECT_FLOAT_EQ((*color)[usize()], 0.2f);
    EXPECT_FLOAT_EQ((*color)[usize(1)], 0.4f);
    EXPECT_FLOAT_EQ((*color)[usize(2)], 0.6f);
}

TEST(SceneUserProperty, BatchUsesSinglePropertySemantics) {
    owe::Scene single;
    owe::Scene batch;
    single.SetClearColorUserKey(String::make("schemecolor"_str));
    batch.SetClearColorUserKey(String::make("schemecolor"_str));

    auto property = rstd::json::from_str(R"({"type":"color","value":"0.2 0.4 0.6"})"_str).unwrap();
    auto properties =
        rstd::json::from_str(
            R"({"waywallen.scheme_color":{"type":"color","value":"0.2 0.4 0.6"}})"_str)
            .unwrap();
    auto values = properties.as_object();
    ASSERT_TRUE(values.is_some());

    auto single_mutation =
        owe::SceneUserPropertyApplier::Apply(single, "waywallen.scheme_color"_str, property);
    auto batch_mutation = owe::SceneUserPropertyApplier::ApplyAll(batch, **values);

    EXPECT_FALSE(single_mutation.graph_changed);
    EXPECT_FALSE(batch_mutation.graph_changed);
    ASSERT_TRUE(single_mutation.clear_color.is_some());
    ASSERT_TRUE(batch_mutation.clear_color.is_some());
    EXPECT_TRUE(single_mutation.texture_materials.is_empty());
    EXPECT_TRUE(batch_mutation.texture_materials.is_empty());
    auto single_color = single.ClearColor();
    auto batch_color  = batch.ClearColor();
    for (usize index {}; index < usize(3); ++index) {
        EXPECT_FLOAT_EQ(single_color[index], batch_color[index]);
    }
    EXPECT_FLOAT_EQ(batch_color[usize()], 0.2f);
    EXPECT_FLOAT_EQ(batch_color[usize(1)], 0.4f);
    EXPECT_FLOAT_EQ(batch_color[usize(2)], 0.6f);
}

TEST(SceneUserProperty, AppliesRegisteredShaderUniformBinding) {
    owe::Scene scene;
    auto       material = Arc<owe::SceneMaterial>::make();
    scene.RegisterShaderUserBinding(
        String::make("brightness"_str), material.clone(), String::make("u_Brightness"_str));

    auto property = rstd::json::from_str(R"({"type":"slider","value":0.5})"_str).unwrap();
    owe::SceneUserPropertyApplier::Apply(scene, "brightness"_str, property);

    auto bindings = scene.ShaderUserBindings("brightness"_str);
    ASSERT_EQ(bindings.len(), usize(1));
    EXPECT_EQ(bindings[usize()].material.as_ptr().as_raw_ptr(), material.as_ptr().as_raw_ptr());
    EXPECT_EQ(bindings[usize()].uniform, "u_Brightness"_str);
    EXPECT_TRUE(material->customShader.constValues.contains_key("u_Brightness"_str));
}

TEST(SceneUserProperty, RecompilesComboAndSkipsUnchangedValue) {
    owe::Scene scene;
    scene.InstallExtension(Box<owe::fs::VFS>::make());
    auto                        material = Arc<owe::SceneMaterial>::make();
    owe::SceneShaderVariantDesc variant;
    variant.scene_id    = "combo-property"_Str;
    variant.shader_name = "combo-property"_Str;
    (void)variant.input_combos.insert("QUALITY"_Str, "0"_Str);
    (void)variant.resolved_combos.insert("QUALITY"_Str, "0"_Str);
    variant.stages.push({
        .stage  = owe::ShaderType::VERTEX,
        .source = "attribute vec3 a_Position;\nvoid main(){gl_Position=vec4(a_Position,1.0);}"_Str,
    });
    variant.stages.push({
        .stage  = owe::ShaderType::FRAGMENT,
        .source = "void main(){gl_FragColor=vec4(float(QUALITY));}"_Str,
    });
    material->customShader.variant = Some(rstd::move(variant));
    owe::Scene::ShaderComboUserBinding binding {
        .material = material.clone(),
        .combo    = "QUALITY"_Str,
        .fallback = "0"_Str,
    };
    (void)binding.options.insert("high"_Str, "2"_Str);
    scene.RegisterShaderComboUserBinding("quality"_Str, rstd::move(binding));
    auto property = rstd::json::from_str(R"({"type":"combo","value":"high"})"_str).unwrap();
    owe::SceneUserPropertyApplier::Apply(scene, "quality"_str, property);
    ASSERT_TRUE(material->customShader.shader);
    ASSERT_TRUE(material->customShader.variant.is_some());
    const auto& current = *material->customShader.variant;
    ASSERT_TRUE(current.resolved_combos.contains_key("QUALITY"_str));
    EXPECT_EQ((**current.resolved_combos.get("QUALITY"_str)).as_str(), "2"_str);
    EXPECT_EQ((**current.input_combos.get("QUALITY"_str)).as_str(), "2"_str);
    auto shader = material->customShader.shader.clone();
    owe::SceneUserPropertyApplier::Apply(scene, "quality"_str, property);
    ASSERT_TRUE(material->customShader.shader);
    EXPECT_EQ((*material->customShader.shader).as_ptr().as_raw_ptr(),
              (*shader).as_ptr().as_raw_ptr());
}

TEST(SceneUserProperty, ReportsRegisteredShaderComboWithoutVfs) {
    owe::Scene                         scene;
    auto                               material = Arc<owe::SceneMaterial>::make();
    owe::Scene::ShaderComboUserBinding binding {
        .material = material.clone(),
        .combo    = String::make("QUALITY"_str),
        .fallback = String::make("0"_str),
    };
    (void)binding.options.insert(String::make("high"_str), String::make("2"_str));
    scene.RegisterShaderComboUserBinding(String::make("quality"_str), rstd::move(binding));

    auto property = rstd::json::from_str(R"({"type":"combo","value":"high"})"_str).unwrap();
    auto mutation = owe::SceneUserPropertyApplier::Apply(scene, "quality"_str, property);

    EXPECT_TRUE(mutation.diagnostics_changed);
    auto bindings = scene.ShaderComboUserBindings("quality"_str);
    ASSERT_EQ(bindings.len(), usize(1));
    EXPECT_EQ(bindings[usize()].combo, "QUALITY"_str);
    EXPECT_EQ(**bindings[usize()].options.get("high"_str), "2"_str);
    auto diagnostics = owe::CollectSceneUserPropertyDiagnostics(scene, "quality"_str);
    ASSERT_EQ(diagnostics.len(), usize(1));
    EXPECT_EQ(diagnostics[usize()].code, owe::SceneUserPropertyDiagnosticCode::SceneVfsUnavailable);
}

TEST(SceneUserProperty, ValidatesCompleteShaderComboIntegerTokens) {
    struct Case {
        ref<str> value;
        bool     valid;
    };
    for (const auto& item : array<Case, 12> {
             Case { "2147483647"_str, true },
             Case { "-2147483648"_str, true },
             Case { "+2"_str, true },
             Case { "true"_str, true },
             Case { "high"_str, true },
             Case { ""_str, true },
             Case { "2147483648"_str, false },
             Case { "-2147483649"_str, false },
             Case { "2tail"_str, false },
             Case { "2\0tail"_str, false },
             Case { " 2"_str, false },
             Case { "2 "_str, false },
         }) {
        SCOPED_TRACE(item.value);
        owe::Scene scene;
        scene.InstallExtension(Box<owe::fs::VFS>::make());
        auto                               material = Arc<owe::SceneMaterial>::make();
        owe::Scene::ShaderComboUserBinding binding {
            .material = material.clone(),
            .combo    = "QUALITY"_Str,
            .fallback = "0"_Str,
        };
        binding.options.insert("high"_Str, "2"_Str);
        scene.RegisterShaderComboUserBinding("quality"_Str, rstd::move(binding));
        owe::SceneUserPropertyApplier::Apply(
            scene, "quality"_str, owe::Json::String(String::make(item.value)));
        auto diagnostics = owe::CollectSceneUserPropertyDiagnostics(scene, "quality"_str);
        ASSERT_EQ(diagnostics.len(), usize(1));
        EXPECT_EQ(diagnostics[usize()].code,
                  item.valid ? owe::SceneUserPropertyDiagnosticCode::MissingShaderVariantDescriptor
                             : owe::SceneUserPropertyDiagnosticCode::UnsupportedShaderComboValue);
    }
}

TEST(SceneUserProperty, AppliesRegisteredMaterialTextureBinding) {
    owe::Scene scene;
    auto       material = Arc<owe::SceneMaterial>::make();
    scene.RegisterTexture(String::make("tex/new"_str), owe::SceneTexture { .url = "tex/new"_Str });
    material->textures.push("tex/old"_Str);
    scene.RegisterMaterialTextureUserBinding(String::make("cover"_str),
                                             owe::Scene::MaterialTextureUserBinding {
                                                 .material = material.clone(),
                                                 .slot     = rstd::u32(),
                                                 .fallback = String::make("tex/fallback"_str),
                                             });

    auto property = rstd::json::from_str(R"({"type":"texture","value":"tex/new"})"_str).unwrap();
    (void)owe::SceneUserPropertyApplier::ApplyTexture(scene, "cover"_str, property);

    auto bindings = scene.MaterialTextureUserBindings("cover"_str);
    ASSERT_EQ(bindings.len(), usize(1));
    EXPECT_EQ(bindings[usize()].fallback, "tex/fallback"_str);
    EXPECT_EQ(material->textures[rstd::usize(0)].as_str(), "tex/new"_str);
}

TEST(SceneUserProperty, AppliesRegisteredImageColorBinding) {
    owe::Scene                              scene;
    auto                                    node     = Arc<owe::SceneNode>::make();
    auto                                    material = Arc<owe::SceneMaterial>::make();
    rstd::array<Arc<owe::SceneMaterial>, 1> materials { material.clone() };
    scene.RegisterImageColorUserBinding(String::make("tint"_str), node, materials.as_slice());

    auto property = rstd::json::from_str(R"({"type":"color","value":"0.2 0.4 0.6"})"_str).unwrap();
    (void)owe::SceneUserPropertyApplier::Apply(scene, "tint"_str, property);

    auto bindings = scene.ImageColorUserBindings("tint"_str);
    ASSERT_EQ(bindings.len(), usize(1));
    ASSERT_EQ(bindings[usize()].materials.len(), usize(1));
    EXPECT_EQ(bindings[usize()].materials[usize()].as_ptr().as_raw_ptr(),
              material.as_ptr().as_raw_ptr());
    EXPECT_FLOAT_EQ(node->Color().x(), 0.2f);
    EXPECT_FLOAT_EQ(node->Color().y(), 0.4f);
    EXPECT_FLOAT_EQ(node->Color().z(), 0.6f);
}

// A parsed object that never reaches the finalized scene (a spawn-template
// prototype, say) is released with the parse context, but its image bindings
// stay in the scene-wide index and are applied right after the parse. The
// index has to keep those targets alive on its own.
TEST(SceneUserProperty, KeepsImageColorBindingTargetsAliveAfterParse) {
    owe::Scene scene;
    {
        auto                                    node     = Arc<owe::SceneNode>::make();
        auto                                    material = Arc<owe::SceneMaterial>::make();
        rstd::array<Arc<owe::SceneMaterial>, 1> materials { material.clone() };
        scene.RegisterImageColorUserBinding(String::make("tint"_str), node, materials.as_slice());
    }

    auto property = rstd::json::from_str(R"({"type":"color","value":"0.2 0.4 0.6"})"_str).unwrap();
    (void)owe::SceneUserPropertyApplier::Apply(scene, "tint"_str, property);

    auto bindings = scene.ImageColorUserBindings("tint"_str);
    ASSERT_EQ(bindings.len(), usize(1));
    ASSERT_EQ(bindings[usize()].materials.len(), usize(1));
    EXPECT_FLOAT_EQ(bindings[usize()].node->Color().x(), 0.2f);
    EXPECT_FLOAT_EQ(bindings[usize()].node->Color().y(), 0.4f);
    EXPECT_FLOAT_EQ(bindings[usize()].node->Color().z(), 0.6f);
}

TEST(SceneUserProperty, KeepsImageAlphaBindingTargetsAliveAfterParse) {
    owe::Scene scene;
    {
        auto                                    node     = Arc<owe::SceneNode>::make();
        auto                                    material = Arc<owe::SceneMaterial>::make();
        rstd::array<Arc<owe::SceneMaterial>, 1> materials { material.clone() };
        scene.RegisterImageAlphaUserBinding(String::make("fade"_str), node, materials.as_slice());
    }

    auto property = rstd::json::from_str(R"({"type":"slider","value":0.25})"_str).unwrap();
    (void)owe::SceneUserPropertyApplier::Apply(scene, "fade"_str, property);

    auto bindings = scene.ImageAlphaUserBindings("fade"_str);
    ASSERT_EQ(bindings.len(), usize(1));
    EXPECT_FLOAT_EQ(bindings[usize()].node->UserAlpha(), 0.25f);
}

TEST(SceneUserProperty, AppliesRegisteredParticleOverrideBinding) {
    owe::Scene scene;
    float      observed = 0.0f;
    scene.RegisterParticleOverrideBinding(
        String::make("rate"_str),
        Arc<dyn<owe::SceneParticleOverrideControl>>::make(
            FakeParticleOverrideControl { .observed = &observed }));

    auto property = rstd::json::from_str(R"({"type":"slider","value":2.5})"_str).unwrap();
    (void)owe::SceneUserPropertyApplier::Apply(scene, "rate"_str, property);

    EXPECT_FLOAT_EQ(observed, 2.5f);
    EXPECT_EQ(scene.ParticleOverrideBindings("rate"_str).len(), usize(1));
}

TEST(SceneUserProperty, UsesStrictFloatTokensAndPreservesListWhitespace) {
    auto prefixed =
        rstd::json::from_str(R"({"type":"color","value":"0.2tail 0.4\u0000tail 0.6"})"_str)
            .unwrap();
    EXPECT_TRUE(owe::ResolveSceneUserPropertyColor(prefixed).is_none());
    auto valid =
        rstd::json::from_str(R"({"type":"color","value":" 0.2\t 0.4  0.6 "})"_str).unwrap();
    auto color = owe::ResolveSceneUserPropertyColor(valid);
    ASSERT_TRUE(color.is_some());
    EXPECT_FLOAT_EQ((*color)[usize()], 0.2f);
    EXPECT_FLOAT_EQ((*color)[usize(1)], 0.4f);
    EXPECT_FLOAT_EQ((*color)[usize(2)], 0.6f);
    auto overflow =
        rstd::json::from_str(R"({"type":"color","value":"1e999 0.4 0.6"})"_str).unwrap();
    EXPECT_TRUE(owe::ResolveSceneUserPropertyColor(overflow).is_none());
}
