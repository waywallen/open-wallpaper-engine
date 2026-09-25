#include <rstd/test/gtest.hpp>

import rstd.cppstd;
import rstd;
import wescene.json;
import wescene.pkg.parse;
import wescene.scene;
import wescene.fs;
import wavsen.audio;

using namespace rstd::literals;
using namespace rstd::prelude;

namespace
{

void ParseBinding(owe::wpscene::FieldBindings& bindings, ref<str> field, ref<str> json) {
    auto value = rstd::json::from_str(json).unwrap();
    ASSERT_GT(owe::wpscene::AbsorbFieldBinding(field, value, bindings), rstd::usize());
}

bool HasIssue(const owe::SceneAnimationBindingScope& scope, owe::SceneAnimationBindingIssue issue) {
    for (const auto& diagnostic : scope.Diagnostics()) {
        if (diagnostic.issue == issue) return true;
    }
    return false;
}

} // namespace

TEST(MaterialParser, ParsesLegacyUserShaderValues) {
    auto j = rstd::json::from_str(R"({
        "passes": [
            {
                "shader": "flag",
                "textures": ["eagle", "flag_normal", "cloth"],
                "usershadervalues": {
                    "flagcolor1": "color2",
                    "flagcolor2": "color3",
                    "schemecolor": "color1"
                }
            }
        ]
    })"_str)
                 .unwrap();

    owe::wpscene::Material material;
    ASSERT_TRUE(material.FromJson(j));

    ASSERT_EQ(material.user_shader_values.len(), usize(3));
    EXPECT_EQ(*material.user_shader_values.get("flagcolor1"_str).unwrap(), "color2"_str);
    EXPECT_EQ(*material.user_shader_values.get("flagcolor2"_str).unwrap(), "color3"_str);
    EXPECT_EQ(*material.user_shader_values.get("schemecolor"_str).unwrap(), "color1"_str);
}

TEST(MaterialParser, ShaderKeysAreCaseSensitiveAndPrecedeUniformShorthand) {
    auto document = owe::wpscene::ParseSceneDocumentJson(R"({
        "camera": {},
        "general": {"orthogonalprojection": {"width": 128, "height": 128}},
        "objects": [
            {"id": 1, "name": "exact", "visible": true, "image": "models/util/solidlayer.json",
             "effects": [{"file": "effects/tint/effect.json", "visible": true, "passes": [{
                 "constantshadervalues": {"Color": "1 1 1", "color": "0 0 0"}
             }]}]},
            {"id": 2, "name": "legacy", "visible": true, "image": "models/util/solidlayer.json",
             "effects": [{"file": "effects/tint/effect.json", "visible": true, "passes": [{
                 "constantshadervalues": {"Color": "0.25 0.25 0.25"}
             }]}]},
            {"id": 3, "name": "uniform", "visible": true, "image": "models/util/solidlayer.json",
             "effects": [{"file": "effects/tint/effect.json", "visible": true, "passes": [{
                 "constantshadervalues": {"TintColor": "0.5 0.5 0.5"}
             }]}]}
        ]
    })"_str,
                                                         owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());
    auto assets = owe::fs::make_physical_fs(
        owe::fs::Path(rstd::cppstd::as_str(WAYWALLEN_ASSETS_DIR).unwrap()));
    ASSERT_TRUE(assets.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, rstd::move(assets).unwrap()).is_ok());
    auto effect_assets = owe::fs::make_physical_fs(owe::fs::Path(
        rstd::cppstd::as_str(std::string(WAYWALLEN_ASSETS_DIR) + "/effects/tint").unwrap()));
    ASSERT_TRUE(effect_assets.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, rstd::move(effect_assets).unwrap()).is_ok());
    wavsen::audio::SoundManager sound_manager;
    owe::SceneParser            parser;
    auto                        parsed = parser.Parse(
        "material-key-precedence"_str,
        ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
        mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound_manager)));
    ASSERT_TRUE(parsed.is_ok());
    auto                            scene = rstd::move(parsed).unwrap();
    const array<array<float, 3>, 3> expected { array<float, 3> { 0.0f, 0.0f, 0.0f },
                                               array<float, 3> { 1.0f, 0.0f, 0.0f },
                                               array<float, 3> { 0.5f, 0.5f, 0.5f } };
    usize                           index {};
    for (const auto* name : { "exact", "legacy", "uniform" }) {
        auto node = scene.scene->RootMut()->FindByName(rstd::cppstd::as_str(name).unwrap());
        ASSERT_NE(node, nullptr);
        ASSERT_TRUE(node->HasLayer());
        auto& layer = node->Layer();
        layer->ResolveEffect(*scene.scene->DefaultEffectMesh(), "effect"_str);
        ASSERT_FALSE(layer->ResolvedEffects().is_empty());
        auto* effect = layer->ResolvedEffects()[usize()];
        ASSERT_FALSE(effect->Nodes().is_empty());
        auto* material = effect->Nodes()[rstd::usize()]->sceneNode->Mesh()->Material();
        ASSERT_NE(material, nullptr);
        auto value = material->customShader.constValues.get("g_TintColor"_str);
        if (value.is_none()) {
            ASSERT_TRUE(material->customShader.shader.is_some());
            value = (*material->customShader.shader)->default_uniforms.get("g_TintColor"_str);
        }
        ASSERT_TRUE(value.is_some());
        const auto& color = **value;
        ASSERT_EQ(color.size(), usize(3));
        for (usize i {}; i < color.size(); ++i)
            EXPECT_FLOAT_EQ(color.data()[i.to_primitive()], expected[index][i]);
        ++index;
    }
}

TEST(MaterialParser, ModelMaterialsIgnoreObsoleteAlphaAndDisableBlendedDepthWrites) {
    auto temporary = rstd::fs::TempDir::make("owe-material-state"_str).unwrap();
    auto root      = rstd::path::PathBuf::from(temporary.path().as_os_str().to_os_string());
    const array<ref<str>, 5> modes {
        "normal"_str, "disabled"_str, "alphatocoverage"_str, "translucent"_str, "additive"_str
    };
    for (auto mode : modes) {
        auto model =
            rstd::format(R"({{"material":"{}-material.json","width":64,"height":64}})", mode);
        auto material = rstd::format(R"({{"passes":[{{
            "shader":"generic3","blending":"{}","depthtest":"enabled","depthwrite":"enabled",
            "combos":{{"LIGHTING":0}},"constantshadervalues":{{"Alpha":0,"color":"0 0 0"}}
        }}]}})",
                                     mode);
        ASSERT_TRUE(rstd::fs::write(root.join(rstd::format("{}.json", mode).as_str()).as_path(),
                                    model->as_bytes())
                        .is_ok());
        ASSERT_TRUE(
            rstd::fs::write(root.join(rstd::format("{}-material.json", mode).as_str()).as_path(),
                            material->as_bytes())
                .is_ok());
    }
    auto document = owe::wpscene::ParseSceneDocumentJson(R"({
        "camera": {}, "general": {"orthogonalprojection": {"width":128,"height":128}},
        "objects": [
            {"id":1,"name":"normal","image":"normal.json"},
            {"id":2,"name":"disabled","image":"disabled.json"},
            {"id":3,"name":"alphatocoverage","image":"alphatocoverage.json"},
            {"id":4,"name":"translucent","image":"translucent.json"},
            {"id":5,"name":"additive","image":"additive.json"}
        ]
    })"_str,
                                                         owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());
    auto assets = owe::fs::make_physical_fs(
        owe::fs::Path(rstd::cppstd::as_str(WAYWALLEN_ASSETS_DIR).unwrap()));
    auto generated = owe::fs::make_physical_fs(temporary.path());
    ASSERT_TRUE(assets.is_ok());
    ASSERT_TRUE(generated.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, rstd::move(assets).unwrap()).is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, rstd::move(generated).unwrap()).is_ok());
    wavsen::audio::SoundManager sound;
    owe::SceneParser            parser;
    auto                        parsed =
        parser.Parse("model-material-state"_str,
                     ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
                     mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
                     mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound)));
    ASSERT_TRUE(parsed.is_ok());
    auto scene = rstd::move(parsed).unwrap();
    for (auto mode : modes) {
        auto* node = scene.scene->RootMut()->FindByName(mode);
        ASSERT_NE(node, nullptr);
        ASSERT_NE(node->Mesh(), nullptr);
        auto* material = node->Mesh()->Material();
        ASSERT_NE(material, nullptr);
        EXPECT_TRUE(material->Pipeline().depth_test);
        EXPECT_EQ(material->Pipeline().depth_write,
                  mode != "translucent"_str && mode != "additive"_str);
        EXPECT_TRUE(material->customShader.constValues.get("g_TintAlpha"_str).is_none());
        ASSERT_TRUE(material->customShader.shader.is_some());
        auto alpha = (*material->customShader.shader)->default_uniforms.get("g_TintAlpha"_str);
        ASSERT_TRUE(alpha.is_some());
        ASSERT_EQ((**alpha).size(), usize(1));
        EXPECT_FLOAT_EQ((**alpha).data()[0], 1.0f);
    }
}

TEST(MaterialParser, PreservesConstantShaderValueScriptBindingsAcrossPassMerge) {
    auto material_json = rstd::json::from_str(R"({
        "passes": [{
            "shader": "effect",
            "constantshadervalues": {
                "color": [1.0, 1.0, 1.0]
            }
        }]
    })"_str)
                             .unwrap();
    auto pass_json     = rstd::json::from_str(R"({
        "constantshadervalues": {
            "color": {
                "script": "export function update(value) { return value; }",
                "scriptproperties": {"speed": 2.0},
                "animation": {"c0": [{"frame": 0, "value": 0.1}]},
                "value": "0.1, 0.2, 0.3"
            }
        }
    })"_str)
                             .unwrap();

    owe::wpscene::Material material;
    ASSERT_TRUE(material.FromJson(material_json));
    owe::wpscene::MaterialPass pass;
    ASSERT_TRUE(pass.FromJson(pass_json));
    material.MergePass(pass);

    auto binding = material.constantshadervalues_bindings.Get("color"_str);
    ASSERT_TRUE(binding.is_some());
    ASSERT_TRUE((**binding).script.is_some());
    EXPECT_EQ((**binding).script->source, "export function update(value) { return value; }"_str);
    ASSERT_TRUE((**binding).script_properties.is_some());
    EXPECT_TRUE((**binding).script_properties->is_object());
    EXPECT_TRUE((**binding).script->initial_value.is_string());
    ASSERT_TRUE((**binding).animation.is_some());
    ASSERT_EQ((**binding).animation->c0.len(), rstd::usize(1));
    EXPECT_FLOAT_EQ((**binding).animation->c0[rstd::usize()].value, 0.1f);

    auto clone          = material.clone();
    auto cloned_binding = clone.constantshadervalues_bindings.Get("color"_str);
    ASSERT_TRUE(cloned_binding.is_some());
    ASSERT_TRUE((**cloned_binding).script.is_some());
    EXPECT_EQ((**cloned_binding).script->source, (**binding).script->source);
    ASSERT_TRUE((**cloned_binding).script_properties.is_some());
    EXPECT_TRUE((**cloned_binding).script_properties->is_object());
    EXPECT_TRUE((**cloned_binding).script->initial_value.is_string());
}

TEST(MaterialParser, CloneOwnsMaterialDataAndPassOverrides) {
    auto                   json = rstd::json::from_str(R"({"passes":[{
        "shader":"original", "textures":["first",null,"last"],
        "usertextures":["property",null,{"name":"$mediaThumbnail","type":"system"}],
        "combos":{"MODE":1}, "constantshadervalues":{"color":"0.1 0.2 0.3"},
        "usershadervalues":{"property":"color"}
    }]})"_str)
                                      .unwrap();
    owe::wpscene::Material material;
    ASSERT_TRUE(material.FromJson(json));
    auto copy                      = material.clone();
    material.shader                = "changed"_Str;
    material.textures[usize()]     = "changed"_Str;
    material.usertextures[usize()] = owe::Json::Null();
    (void)material.combos.insert("MODE"_Str, i32(2));
    (*material.constantshadervalues.get_mut("color"_str).unwrap())[usize()] = 9.0f;
    (void)material.user_shader_values.insert("property"_Str, "other"_Str);

    EXPECT_EQ(copy.shader, "original"_str);
    ASSERT_EQ(copy.textures.len(), usize(3));
    EXPECT_EQ(copy.textures[usize()], "first"_str);
    EXPECT_TRUE(copy.textures[usize(1)].is_empty());
    EXPECT_EQ(*copy.usertextures[usize()].as_str(), "property"_str);
    EXPECT_EQ(*copy.combos.get("MODE"_str).unwrap(), i32(1));
    EXPECT_FLOAT_EQ((*copy.constantshadervalues.get("color"_str).unwrap())[usize()], 0.1f);
    EXPECT_EQ(*copy.user_shader_values.get("property"_str).unwrap(), "color"_str);

    auto                       overrides = rstd::json::from_str(R"({
        "textures":[null,"replacement","",null,"new"],
        "usertextures":[null,"new-property"], "combos":{"MODE":3},
        "constantshadervalues":{"color":[0.4,0.5]},
        "usershadervalues":{"property":"tint"}
    })"_str)
                                               .unwrap();
    owe::wpscene::MaterialPass pass;
    ASSERT_TRUE(pass.FromJson(overrides));
    copy.MergePass(pass);
    pass.textures[usize(1)]                                             = "mutated"_Str;
    (*pass.constantshadervalues.get_mut("color"_str).unwrap())[usize()] = 8.0f;
    ASSERT_EQ(copy.textures.len(), usize(5));
    EXPECT_EQ(copy.textures[usize()], "first"_str);
    EXPECT_EQ(copy.textures[usize(1)], "replacement"_str);
    EXPECT_EQ(copy.textures[usize(2)], "last"_str);
    EXPECT_TRUE(copy.textures[usize(3)].is_empty());
    EXPECT_EQ(copy.textures[usize(4)], "new"_str);
    ASSERT_EQ(copy.usertextures.len(), usize(3));
    EXPECT_EQ(*copy.usertextures[usize()].as_str(), "property"_str);
    EXPECT_EQ(*copy.usertextures[usize(1)].as_str(), "new-property"_str);
    EXPECT_TRUE(copy.usertextures[usize(2)].is_object());
    EXPECT_EQ(*copy.combos.get("MODE"_str).unwrap(), i32(3));
    EXPECT_EQ(copy.constantshadervalues.get("color"_str).unwrap()->len(), usize(2));
    EXPECT_FLOAT_EQ((*copy.constantshadervalues.get("color"_str).unwrap())[usize()], 0.4f);
    EXPECT_EQ(*copy.user_shader_values.get("property"_str).unwrap(), "tint"_str);
}

TEST(MaterialParser, PassUpdateKeepsRoutingAndOwnsOverrides) {
    auto                       original      = rstd::json::from_str(R"({
        "id":7,"target":"output","bind":[{"name":"previous","index":0}],
        "textures":["base"],"combos":{"MODE":1}
    })"_str)
                                                   .unwrap();
    auto                       override_json = rstd::json::from_str(R"({
        "id":8,"target":"ignored","bind":[{"name":"ignored","index":2}],
        "textures":[null,"added"],"combos":{"MODE":2}
    })"_str)
                                                   .unwrap();
    owe::wpscene::MaterialPass pass, overrides;
    ASSERT_TRUE(pass.FromJson(original));
    ASSERT_TRUE(overrides.FromJson(override_json));
    pass.Update(overrides);
    overrides.textures[usize(1)].clear();
    EXPECT_EQ(pass.id, u32(7));
    EXPECT_EQ(pass.target, "output"_str);
    ASSERT_EQ(pass.bind.len(), usize(1));
    EXPECT_EQ(pass.bind[usize()].name, "previous"_str);
    EXPECT_EQ(pass.bind[usize()].index, i32());
    ASSERT_EQ(pass.textures.len(), usize(2));
    EXPECT_EQ(pass.textures[usize()], "base"_str);
    EXPECT_EQ(pass.textures[usize(1)], "added"_str);
    EXPECT_EQ(*pass.combos.get("MODE"_str).unwrap(), i32(2));
}

TEST(AnimationBinding, ResolvesReciprocalTracksAcrossPropertyObjects) {
    owe::wpscene::ImageObject image;
    ParseBinding(image.field_bindings,
                 "origin"_str,
                 R"({
                    "animation": {
                        "c0": [{"frame": 0, "value": 0.0}],
                        "options": {
                            "parent": {"key": "amount"},
                            "fps": 30, "length": 60, "mode": "loop"
                        }
                    }
                 })"_str);
    ParseBinding(image.material.constantshadervalues_bindings,
                 "amount"_str,
                 R"({
                    "animation": {
                        "c0": [{"frame": 0, "value": 1.0}],
                        "options": {
                            "children": [{"key": "origin"}],
                            "name": "shared", "fps": 30, "length": 60, "mode": "loop"
                        }
                    }
                 })"_str);

    auto scope          = owe::BuildAnimationBindingScope(image);
    auto origin_binding = image.field_bindings.Get("origin"_str);
    auto amount_binding = image.material.constantshadervalues_bindings.Get("amount"_str);
    ASSERT_TRUE(origin_binding.is_some());
    ASSERT_TRUE(amount_binding.is_some());

    auto origin_track = scope.Resolve(**origin_binding);
    auto amount_track = scope.Resolve(**amount_binding);
    ASSERT_TRUE(origin_track.is_some());
    ASSERT_TRUE(amount_track.is_some());
    EXPECT_EQ(origin_track->playback.as_ptr().as_raw_ptr(),
              amount_track->playback.as_ptr().as_raw_ptr());
    EXPECT_NE(origin_track->curve.as_ptr().as_raw_ptr(), amount_track->curve.as_ptr().as_raw_ptr());

    auto material_clone = image.material.clone();
    auto cloned_binding = material_clone.constantshadervalues_bindings.Get("amount"_str);
    ASSERT_TRUE(cloned_binding.is_some());
    auto cloned_track = scope.Resolve(**cloned_binding);
    ASSERT_TRUE(cloned_track.is_some());
    EXPECT_EQ(cloned_track->playback.as_ptr().as_raw_ptr(),
              amount_track->playback.as_ptr().as_raw_ptr());
}

TEST(AnimationBinding, KeepsAmbiguousRelationsIndependent) {
    owe::wpscene::ImageObject image;
    ParseBinding(image.field_bindings,
                 "origin"_str,
                 R"({
                    "animation": {
                        "c0": [{"frame": 0, "value": 0.0}],
                        "options": {
                            "parent": {"key": "amount"},
                            "fps": 30, "length": 60, "mode": "loop"
                        }
                    }
                 })"_str);
    ParseBinding(image.material.constantshadervalues_bindings,
                 "amount"_str,
                 R"({
                    "animation": {
                        "c0": [{"frame": 0, "value": 1.0}],
                        "options": {
                            "children": [{"key": "origin"}],
                            "fps": 30, "length": 60, "mode": "loop"
                        }
                    }
                 })"_str);
    image.effects.emplace_back();
    image.effects.last_mut().unwrap()->materials.push(owe::wpscene::Material {});
    ParseBinding(image.effects.last_mut()
                     .unwrap()
                     .get_mut()
                     .materials[usize()]
                     .constantshadervalues_bindings,
                 "amount"_str,
                 R"({
                    "animation": {
                        "c0": [{"frame": 0, "value": 2.0}],
                        "options": {
                            "children": [{"key": "origin"}],
                            "fps": 30, "length": 60, "mode": "loop"
                        }
                    }
                 })"_str);

    auto scope          = owe::BuildAnimationBindingScope(image);
    auto origin_binding = image.field_bindings.Get("origin"_str);
    auto amount_binding = image.material.constantshadervalues_bindings.Get("amount"_str);
    ASSERT_TRUE(origin_binding.is_some());
    ASSERT_TRUE(amount_binding.is_some());
    auto origin_track = scope.Resolve(**origin_binding);
    auto amount_track = scope.Resolve(**amount_binding);
    ASSERT_TRUE(origin_track.is_some());
    ASSERT_TRUE(amount_track.is_some());

    EXPECT_NE(origin_track->playback.as_ptr().as_raw_ptr(),
              amount_track->playback.as_ptr().as_raw_ptr());
    EXPECT_TRUE(HasIssue(scope, owe::SceneAnimationBindingIssue::AmbiguousTarget));
}

TEST(AnimationBinding, ReportsMalformedRelationsWithoutMergingTracks) {
    owe::wpscene::FieldBindings missing;
    ParseBinding(missing,
                 "origin"_str,
                 R"({"animation":{"c0":[{"frame":0,"value":0}],"options":{
                     "parent":{"key":"missing"},"fps":30,"length":60,"mode":"loop"
                 }}})"_str);
    auto missing_scope = owe::BuildAnimationBindingScope(missing);
    EXPECT_TRUE(HasIssue(missing_scope, owe::SceneAnimationBindingIssue::MissingRelation));

    owe::wpscene::FieldBindings nonreciprocal;
    ParseBinding(nonreciprocal,
                 "origin"_str,
                 R"({"animation":{"c0":[{"frame":0,"value":0}],"options":{
                     "parent":{"key":"amount"},"fps":30,"length":60,"mode":"loop"
                 }}})"_str);
    ParseBinding(nonreciprocal,
                 "amount"_str,
                 R"({"animation":{"c0":[{"frame":0,"value":1}],"options":{
                     "fps":30,"length":60,"mode":"loop"
                 }}})"_str);
    auto nonreciprocal_scope = owe::BuildAnimationBindingScope(nonreciprocal);
    EXPECT_TRUE(
        HasIssue(nonreciprocal_scope, owe::SceneAnimationBindingIssue::NonReciprocalRelation));

    owe::wpscene::FieldBindings mismatch;
    ParseBinding(mismatch,
                 "origin"_str,
                 R"({"animation":{"c0":[{"frame":0,"value":0}],"options":{
                     "parent":{"key":"amount"},"fps":30,"length":60,"mode":"loop"
                 }}})"_str);
    ParseBinding(mismatch,
                 "amount"_str,
                 R"({"animation":{"c0":[{"frame":0,"value":1}],"options":{
                     "children":[{"key":"origin"}],"fps":60,"length":60,"mode":"loop"
                 }}})"_str);
    auto mismatch_scope = owe::BuildAnimationBindingScope(mismatch);
    EXPECT_TRUE(HasIssue(mismatch_scope, owe::SceneAnimationBindingIssue::MetadataMismatch));
    auto mismatch_origin = mismatch.Get("origin"_str);
    auto mismatch_amount = mismatch.Get("amount"_str);
    ASSERT_TRUE(mismatch_origin.is_some());
    ASSERT_TRUE(mismatch_amount.is_some());
    auto mismatch_origin_track = mismatch_scope.Resolve(**mismatch_origin);
    auto mismatch_amount_track = mismatch_scope.Resolve(**mismatch_amount);
    ASSERT_TRUE(mismatch_origin_track.is_some());
    ASSERT_TRUE(mismatch_amount_track.is_some());
    EXPECT_NE(mismatch_origin_track->playback.as_ptr().as_raw_ptr(),
              mismatch_amount_track->playback.as_ptr().as_raw_ptr());
}

TEST(AnimationBinding, ReportsCyclesAndDuplicateNames) {
    owe::wpscene::FieldBindings cyclic;
    ParseBinding(cyclic,
                 "left"_str,
                 R"({"animation":{"c0":[{"frame":0,"value":0}],"options":{
                     "parent":{"key":"right"},"children":[{"key":"right"}],
                     "fps":30,"length":60,"mode":"loop"
                 }}})"_str);
    ParseBinding(cyclic,
                 "right"_str,
                 R"({"animation":{"c0":[{"frame":0,"value":1}],"options":{
                     "parent":{"key":"left"},"children":[{"key":"left"}],
                     "fps":30,"length":60,"mode":"loop"
                 }}})"_str);
    auto cyclic_scope = owe::BuildAnimationBindingScope(cyclic);
    EXPECT_TRUE(HasIssue(cyclic_scope, owe::SceneAnimationBindingIssue::Cycle));
    auto left  = cyclic.Get("left"_str);
    auto right = cyclic.Get("right"_str);
    ASSERT_TRUE(left.is_some());
    ASSERT_TRUE(right.is_some());
    auto left_track  = cyclic_scope.Resolve(**left);
    auto right_track = cyclic_scope.Resolve(**right);
    ASSERT_TRUE(left_track.is_some());
    ASSERT_TRUE(right_track.is_some());
    EXPECT_NE(left_track->playback.as_ptr().as_raw_ptr(),
              right_track->playback.as_ptr().as_raw_ptr());

    owe::wpscene::FieldBindings duplicate;
    ParseBinding(duplicate,
                 "left"_str,
                 R"({"animation":{"c0":[{"frame":0,"value":0}],"options":{
                     "name":"shared","fps":30,"length":60,"mode":"loop"
                 }}})"_str);
    ParseBinding(duplicate,
                 "right"_str,
                 R"({"animation":{"c0":[{"frame":0,"value":1}],"options":{
                     "name":"shared","fps":30,"length":60,"mode":"loop"
                 }}})"_str);
    auto duplicate_scope = owe::BuildAnimationBindingScope(duplicate);
    EXPECT_TRUE(HasIssue(duplicate_scope, owe::SceneAnimationBindingIssue::DuplicateName));
}

TEST(FieldBindingStorage, CloneOwnsCurvesEventsAndSourceWithoutChangingIdentity) {
    owe::wpscene::FieldBindings bindings;
    ParseBinding(bindings, "origin"_str, R"({
        "animation": {
            "c0":[{"frame":3,"value":2.5}],
            "options":{"mode":"loop","name":"motion","events":[{"frame":3,"name":"marker"}]}
        },
        "script":"export function update(value) { return value; }",
        "value":[1,2,3]
    })"_str);
    auto clone    = bindings.clone();
    auto original = bindings.GetMut("origin"_str).unwrap();
    auto copied   = clone.Get("origin"_str).unwrap();
    EXPECT_EQ(original->identity, copied->identity);
    original->animation->c0[usize()].value            = 8.0f;
    original->animation->options.events[usize()].name = "changed"_Str;
    original->script->source                          = "changed"_Str;
    EXPECT_FLOAT_EQ(copied->animation->c0[usize()].value, 2.5f);
    EXPECT_EQ(copied->animation->options.events[usize()].name, "marker"_str);
    EXPECT_EQ(copied->script->source, "export function update(value) { return value; }"_str);
    EXPECT_TRUE(bindings.Get("missing"_str).is_none());
    EXPECT_TRUE(bindings.GetMut("missing"_str).is_none());
    auto identity    = original->identity;
    auto replacement = rstd::json::from_str(R"({"script":"replacement","value":0})"_str).unwrap();
    EXPECT_EQ(owe::wpscene::AbsorbFieldBinding("origin"_str, replacement, bindings), usize(1));
    EXPECT_EQ(bindings.Get("origin"_str).unwrap()->identity, identity);
    EXPECT_EQ(bindings.Entries().len(), usize(1));
}

TEST(AnimationBinding, PreservesDeclarationOrderOfSameFrameEvents) {
    owe::wpscene::FieldBindings bindings;
    ParseBinding(bindings, "origin"_str, R"({
        "animation": {
            "c0":[{"frame":20,"value":2},{"frame":0,"value":0}],
            "options":{"events":[
                {"frame":10,"name":"first"},
                {"frame":2,"name":"early"},
                {"frame":10,"name":"second"},
                {"frame":10,"name":"third"}
            ]}
        }
    })"_str);
    auto scope   = owe::BuildAnimationBindingScope(bindings);
    auto binding = bindings.Get("origin"_str).unwrap();
    auto track   = scope.Resolve(*binding).unwrap();
    auto clip    = track.playback->Clip();
    auto events  = clip->Events();
    ASSERT_EQ(events.len(), usize(4));
    EXPECT_EQ(events[usize()].name, "early"_str);
    EXPECT_EQ(events[usize(1)].name, "first"_str);
    EXPECT_EQ(events[usize(2)].name, "second"_str);
    EXPECT_EQ(events[usize(3)].name, "third"_str);
    EXPECT_EQ(events[usize()].order, usize(1));
    EXPECT_EQ(events[usize(1)].order, usize());
    EXPECT_EQ(track.curve->c0[usize()].frame, i32());
}

TEST(ParticleDocument, PreservesRendererDefaultsAndDeclarationOrder) {
    owe::fs::VFS vfs;
    auto         assets = owe::fs::make_physical_fs(
        owe::fs::Path(rstd::cppstd::as_str(WAYWALLEN_ASSETS_DIR).unwrap()));
    ASSERT_TRUE(assets.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, rstd::move(assets).unwrap()).is_ok());
    auto                   json = owe::ParseJson(R"({
        "emitter":[{"name":"boxrandom","id":2,"sign":"-5 0 8"},{"name":"sphererandom","id":1}],
        "material":"materials/util/effectpassthrough.json",
        "maxcount":42,"starttime":1.5,"animationmode":"sequence",
        "controlpoint":[{"id":7,"offset":"1 2 3"},{"id":2,"offset":"4 5 6"}]
    })"_str)
                                      .unwrap();
    owe::wpscene::Particle particle;
    ASSERT_TRUE(particle.FromJson(json, vfs));
    ASSERT_EQ(particle.emitters.len(), usize(2));
    EXPECT_EQ(particle.emitters[usize()].name, "boxrandom"_str);
    EXPECT_EQ(particle.emitters[usize(1)].id, i32(1));
    EXPECT_EQ(particle.emitters[usize()].sign[usize(0)], i32(-1));
    EXPECT_EQ(particle.emitters[usize()].sign[usize(2)], i32(1));
    ASSERT_EQ(particle.renderers.len(), usize(1));
    EXPECT_EQ(particle.renderers[usize()].name, "sprite"_str);
    ASSERT_EQ(particle.controlpoints.len(), usize(2));
    EXPECT_EQ(particle.controlpoints[usize()].id, i32(7));
    EXPECT_EQ(particle.controlpoints[usize(1)].id, i32(2));
    auto more = owe::ParseJson(R"({
        "emitter":[], "renderer":[{"name":"ropetrail"},{"name":"spritetrail"}],
        "material":"materials/util/effectpassthrough.json"
    })"_str)
                    .unwrap();
    ASSERT_TRUE(particle.FromJson(more, vfs));
    ASSERT_EQ(particle.emitters.len(), usize(2));
    ASSERT_EQ(particle.renderers.len(), usize(3));
    EXPECT_EQ(particle.renderers[usize(1)].name, "ropetrail"_str);
    EXPECT_FLOAT_EQ(particle.renderers[usize(1)].subdivision, 1.0f);
    EXPECT_EQ(particle.renderers[usize(2)].name, "spritetrail"_str);
    EXPECT_FLOAT_EQ(particle.renderers[usize(2)].subdivision, 3.0f);
}

TEST(ParticleDocument, CloneOwnsRecursiveDataAndPreservesEmitterFields) {
    owe::wpscene::Particle particle;
    owe::wpscene::Emitter  emitter;
    auto                   json = owe::ParseJson(R"({
        "name":"boxrandom","id":7,"directions":"1 2 3",
        "distancemax":"4 5 6","distancemin":"-1 -2 -3","origin":"9 8 7",
        "sign":"-2 0 3","instantaneous":3,"maxtoemitperperiod":4,
        "speedmin":2,"speedmax":9,"audioprocessingmode":1,"audioamount":0.4,
        "audioexponent":2,"audiofrequency":"2 8","audiobounds":"0.2 0.7",
        "controlpoint":3,"flags":2,"rate":11,"duration":12
    })"_str)
                                      .unwrap();
    ASSERT_TRUE(emitter.FromJson(json));
    particle.emitters.push(rstd::move(emitter));
    owe::wpscene::ParticleRender renderer;
    ASSERT_TRUE(renderer.FromJson(owe::ParseJson(R"({
        "name":"ropetrail","length":0.2,"maxlength":8,"subdivision":6,"segments":7
    })"_str)
                                      .unwrap()));
    particle.renderers.push(rstd::move(renderer));
    particle.animationmode = "randomframe"_Str;
    particle.initializers.push(owe::ParseJson(R"({"name":"sizerandom","min":3})"_str).unwrap());
    particle.operators.push(owe::ParseJson(R"({"name":"movement","drag":2})"_str).unwrap());
    owe::wpscene::ParticleChild child;
    child.name                   = "child"_Str;
    child.type                   = "eventfollow"_Str;
    child.controlpointstartindex = Some(i32(5));
    child.probability            = 0.25f;
    child.obj.animationmode      = "sequence"_Str;
    child.obj.material.shader    = "nested"_Str;
    owe::wpscene::ParticleChild grandchild;
    grandchild.name = "grandchild"_Str;
    child.obj.children.push(rstd::move(grandchild));
    particle.children.push(rstd::move(child));
    auto  copy = particle.Clone();
    auto& e    = copy.emitters[usize()];
    EXPECT_EQ(e.name, "boxrandom"_str);
    EXPECT_EQ(e.id, i32(7));
    EXPECT_FLOAT_EQ(e.directions[usize(1)], 2.0f);
    EXPECT_FLOAT_EQ(e.distancemax[usize(2)], 6.0f);
    EXPECT_FLOAT_EQ(e.distancemin[usize(1)], -2.0f);
    EXPECT_FLOAT_EQ(e.origin[usize(0)], 9.0f);
    EXPECT_EQ(e.sign[usize(0)], i32(-1));
    EXPECT_EQ(e.instantaneous, u32(3));
    EXPECT_EQ(e.max_emit_per_period, u32(4));
    EXPECT_FLOAT_EQ(e.speedmin, 2.0f);
    EXPECT_FLOAT_EQ(e.speedmax, 9.0f);
    EXPECT_EQ(e.audioprocessingmode, u32(1));
    EXPECT_FLOAT_EQ(e.audioamount, 0.4f);
    EXPECT_FLOAT_EQ(e.audioexponent, 2.0f);
    EXPECT_FLOAT_EQ(e.audiofrequency[usize(1)], 8.0f);
    EXPECT_FLOAT_EQ(e.audiobounds[usize(0)], 0.2f);
    EXPECT_EQ(e.controlpoint, i32(3));
    EXPECT_TRUE(e.flags[owe::wpscene::Emitter::FlagEnum::one_per_frame]);
    EXPECT_FLOAT_EQ(e.rate, 11.0f);
    EXPECT_FLOAT_EQ(e.duration, 12.0f);
    particle.emitters[usize()].name  = "changed"_Str;
    particle.renderers[usize()].name = "changed"_Str;
    particle.initializers.clear();
    particle.operators.clear();
    particle.children.clear();
    particle.animationmode.clear();
    EXPECT_EQ(copy.animationmode, "randomframe"_str);
    EXPECT_EQ(e.name, "boxrandom"_str);
    EXPECT_EQ(copy.renderers[usize()].name, "ropetrail"_str);
    EXPECT_FLOAT_EQ(copy.renderers[usize()].length, 0.2f);
    EXPECT_FLOAT_EQ(copy.renderers[usize()].maxlength, 8.0f);
    EXPECT_FLOAT_EQ(copy.renderers[usize()].subdivision, 6.0f);
    EXPECT_EQ(copy.renderers[usize()].segments, i32(7));
    EXPECT_EQ(copy.initializers.len(), usize(1));
    EXPECT_EQ(copy.operators.len(), usize(1));
    ASSERT_EQ(copy.children.len(), usize(1));
    EXPECT_EQ(copy.children[usize()].type, "eventfollow"_str);
    EXPECT_EQ(*copy.children[usize()].controlpointstartindex, i32(5));
    EXPECT_FLOAT_EQ(copy.children[usize()].probability, 0.25f);
    EXPECT_EQ(copy.children[usize()].obj.material.shader, "nested"_str);
    EXPECT_EQ(copy.children[usize()].obj.children[usize()].name, "grandchild"_str);
}

TEST(ParticleDocument, OverrideCloneSeparatesKeysAndSharesReadOnlyBindings) {
    owe::wpscene::ParticleInstanceoverride source;
    ASSERT_TRUE(source.FromJosn(owe::ParseJson(R"({
        "alpha":{"value":0.25,"user":"opacity"},
        "size":{"value":4,"user":"scale"},
        "color":{"value":"1 0 0","user":"tint"},
        "controlpoint0":{"value":"1 2 3","user":"origin"}
    })"_str)
                                    .unwrap()));
    auto copy = source.clone();
    EXPECT_EQ(copy.FieldBindingsView(), source.FieldBindingsView());
    auto update = owe::ParseJson(R"({"alpha":{"value":0.5,"user":"replacement"}})"_str).unwrap();
    ASSERT_TRUE(source.FromJosn(update));
    EXPECT_EQ(**source.bindings.get("alpha"_str), "replacement"_str);
    EXPECT_EQ(**source.bindings.get("size"_str), "scale"_str);
    EXPECT_EQ(**copy.bindings.get("alpha"_str), "opacity"_str);
    EXPECT_EQ(**copy.bindings.get("controlpoint0"_str), "origin"_str);
    EXPECT_NE(copy.FieldBindingsView(), source.FieldBindingsView());
    EXPECT_NE(copy.FieldBindingsView(), nullptr);
    EXPECT_FLOAT_EQ(copy.alpha, 0.25f);
    EXPECT_FLOAT_EQ(copy.size, 4.0f);
}
