#include <rstd/test/gtest.hpp>

import rstd.cppstd;
import rstd;
import wescene.json;
import wescene.pkg.parse;

using namespace rstd::literals;
using namespace rstd::prelude;

namespace
{

void ParseBinding(owe::wpscene::FieldBindings& bindings, ref<str> field, ref<str> json) {
    auto value = rstd::json::from_str(json).unwrap();
    ASSERT_GT(
        owe::wpscene::AbsorbFieldBinding(rstd::cppstd::as_string_view(field), value, bindings), 0u);
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

    ASSERT_EQ(material.user_shader_values.size(), 3u);
    EXPECT_EQ(material.user_shader_values.at("flagcolor1"), "color2");
    EXPECT_EQ(material.user_shader_values.at("flagcolor2"), "color3");
    EXPECT_EQ(material.user_shader_values.at("schemecolor"), "color1");
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
                "value": "0.1 0.2 0.3"
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
    EXPECT_EQ((**binding).script->source, "export function update(value) { return value; }");
    ASSERT_TRUE((**binding).script_properties.is_some());
    EXPECT_TRUE((**binding).script_properties->is_object());
    EXPECT_TRUE((**binding).script->initial_value.is_string());
    ASSERT_TRUE((**binding).animation.is_some());
    ASSERT_EQ((**binding).animation->c0.size(), 1u);
    EXPECT_FLOAT_EQ((**binding).animation->c0[0].value, 0.1f);

    auto clone          = material.clone();
    auto cloned_binding = clone.constantshadervalues_bindings.Get("color"_str);
    ASSERT_TRUE(cloned_binding.is_some());
    ASSERT_TRUE((**cloned_binding).script.is_some());
    EXPECT_EQ((**cloned_binding).script->source, (**binding).script->source);
    ASSERT_TRUE((**cloned_binding).script_properties.is_some());
    EXPECT_TRUE((**cloned_binding).script_properties->is_object());
    EXPECT_TRUE((**cloned_binding).script->initial_value.is_string());
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
    image.effects.back().materials.emplace_back();
    ParseBinding(image.effects.back().materials.back().constantshadervalues_bindings,
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
