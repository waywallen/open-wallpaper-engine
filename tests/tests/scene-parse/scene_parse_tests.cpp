// scene.json parse regression net.
//
// For each observed PKGV version, every matching workshop is re-opened,
// scene.json is read via VFS, and parsed via the
// canonical version-bearing SceneMetadata::FromJson(json, pkg_version) path.
// Establishes the baseline for the upcoming SceneMetadata refactor that splits
// FromJson by version: a regression here means the refactor changed
// what was previously parseable.
//
// Deliberately does not depend on Corpus / DumpWorkshop — those exercise
// MdlParser / TexImageParser too, which can hit unrelated assertions
// on rare .mdl inputs and would mask scene.json regressions.

#include <rstd/test/gtest.hpp>

#include <cmath>

import rstd.cppstd;
import rstd;
import wavsen.audio;
import wescene.fs;
import wescene.json;
import wescene.pkg.parse;
import wescene.pkg.scene_obj;
import wescene.scene;
import wescene.testing.scene_parse_probe;
import wescene.types;

using namespace rstd::literals;

TEST(FieldBindingJson, CompatibilityReaderPopulatesAnimationMetadata) {
    auto parsed = owe::ParseJson(R"({"enabled":true,"x":{"value":1.5},"y":-2.0,"magic":7})");
    ASSERT_TRUE(parsed.is_ok());

    owe::wpscene::AnimKeyframeTangent tangent;
    ASSERT_TRUE(owe::wpscene::ParseAnimKeyframeTangent(parsed.unwrap(), tangent));
    EXPECT_TRUE(tangent.enabled);
    EXPECT_FLOAT_EQ(tangent.x, 1.5f);
    EXPECT_FLOAT_EQ(tangent.y, -2.0f);
    EXPECT_EQ(tangent.magic, rstd::i32(7));
}

TEST(FieldBindingJson, TangentObjectDefaultsToEnabledAndParsesStep) {
    auto parsed = owe::ParseJson(R"({
        "frame": 10,
        "value": 2.0,
        "step": true,
        "front": {"x": 0.5, "y": 1.0}
    })");
    ASSERT_TRUE(parsed.is_ok());

    owe::wpscene::AnimKeyframe key;
    ASSERT_TRUE(owe::wpscene::ParseAnimKeyframe(parsed.unwrap(), key));
    EXPECT_TRUE(key.step);
    EXPECT_TRUE(key.front.enabled);
    EXPECT_FLOAT_EQ(key.front.x, 0.5f);
    EXPECT_FLOAT_EQ(key.front.y, 1.0f);
    EXPECT_FALSE(key.back.enabled);
}

TEST(SceneObjectClone, MembersProvideCloneTraitImplementation) {
    owe::wpscene::AnimCurve curve;
    curve.relative = true;
    curve.c0.push_back({ .frame = rstd::i32(3), .value = 1.5f });

    auto direct_curve = curve.clone();
    auto trait_curve  = rstd::as<rstd::clone::Clone>(curve).clone();
    ASSERT_EQ(direct_curve.c0.size(), 1u);
    EXPECT_FLOAT_EQ(trait_curve.c0[0].value, 1.5f);

    owe::wpscene::Material material;
    material.shader      = "generic";
    auto direct_material = material.clone();
    auto trait_material  = rstd::as<rstd::clone::Clone>(material).clone();
    EXPECT_EQ(direct_material.shader, "generic");
    EXPECT_EQ(trait_material.shader, "generic");
}

TEST(ObjectInstanceJson, AppliesMaterialBindingOverridesBySlot) {
    auto parsed = owe::ParseJson(R"({
        "textures": [null, "linked"],
        "usertextures": [null, "replacement"],
        "combos": {"version": 2}
    })");
    ASSERT_TRUE(parsed.is_ok());

    owe::wpscene::ObjectInstance instance;
    ASSERT_TRUE(instance.FromJson(parsed.unwrap()));
    owe::wpscene::Material material;
    material.textures = { "base-0", "base-1" };
    instance.ApplyTo(material);

    ASSERT_EQ(material.textures.size(), 2u);
    EXPECT_EQ(material.textures[0], "base-0");
    EXPECT_EQ(material.textures[1], "linked");
    ASSERT_EQ(material.usertextures.len(), rstd::usize(2));
    EXPECT_TRUE(material.usertextures[rstd::usize()].is_null());
    ASSERT_TRUE(material.usertextures[rstd::usize(1)].is_string());
    EXPECT_EQ(rstd::cppstd::to_string(*material.usertextures[rstd::usize(1)].as_str()),
              "replacement");
    EXPECT_EQ(material.combos.at("version"), rstd::i32(2));
}

TEST(TextObjectJson, ReadsDirectUserValueBinding) {
    auto parsed = owe::ParseJson(R"({"text":{"user":"title","value":"default"}})");
    ASSERT_TRUE(parsed.is_ok());

    owe::fs::VFS             vfs;
    owe::wpscene::TextObject text;
    ASSERT_TRUE(text.FromJson(parsed.unwrap(), vfs));
    EXPECT_EQ(text.text_user.name, "title");
    EXPECT_TRUE(text.reflected);
}

TEST(TextObjectJson, ReadsReflectionParticipation) {
    auto parsed = owe::ParseJson(R"({"reflected":false})");
    ASSERT_TRUE(parsed.is_ok());

    owe::fs::VFS             vfs;
    owe::wpscene::TextObject text;
    ASSERT_TRUE(text.FromJson(parsed.unwrap(), vfs));
    EXPECT_FALSE(text.reflected);
}

TEST(TextRenderMode, UsesDirectRenderingOnlyWithoutIndependentSurfaceRequirements) {
    EXPECT_EQ(owe::ResolveTextRenderMode({}), owe::TextRenderMode::Direct);
    EXPECT_EQ(owe::ResolveTextRenderMode({ .has_effect = true }), owe::TextRenderMode::Offscreen);
    EXPECT_EQ(owe::ResolveTextRenderMode({ .copy_background = true }),
              owe::TextRenderMode::Offscreen);
    EXPECT_EQ(owe::ResolveTextRenderMode({ .opaque_background = true }),
              owe::TextRenderMode::Offscreen);
    EXPECT_EQ(owe::ResolveTextRenderMode({ .linked_source = true }),
              owe::TextRenderMode::Offscreen);
}

TEST(SceneObjectExpansion, PreservesHiddenTextLayers) {
    auto parsed = owe::ParseJson(R"({
        "objects": [{
            "id": 7,
            "name": "Style1",
            "text": "progress",
            "visible": false
        }]
    })");
    ASSERT_TRUE(parsed.is_ok());

    owe::fs::VFS vfs;
    auto objects = owe::ExpandObjects(parsed.unwrap(), vfs, owe::wpscene::kSceneVersionUnknown);

    ASSERT_EQ(objects.len(), rstd::usize(1));
    ASSERT_TRUE(objects[rstd::usize()].is_Text());
    EXPECT_FALSE(objects[rstd::usize()].as_Text().value.visible);
}

TEST(SceneDocumentObjects, PreservesDeclarationOrderAndObjectKinds) {
    auto document = owe::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {},
            "objects": [
                {"id": 10, "name": "Group", "parent": 0},
                {"id": 11, "name": "Shape", "shape": "quad", "parent": 10},
                {"id": 12, "name": "Text", "text": "hello", "parent": 10}
            ]
        })JSON",
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());
    ASSERT_EQ(document->objects.len(), rstd::usize(3));
    EXPECT_EQ(document->objects[rstd::usize()].metadata.id, rstd::i32(10));
    EXPECT_EQ(document->objects[rstd::usize()].metadata.kind,
              owe::wpscene::SceneObjectKind::Container);
    EXPECT_EQ(document->objects[rstd::usize(1)].metadata.id, rstd::i32(11));
    EXPECT_EQ(document->objects[rstd::usize(1)].metadata.kind,
              owe::wpscene::SceneObjectKind::Shape);
    EXPECT_EQ(document->objects[rstd::usize(2)].metadata.id, rstd::i32(12));
    EXPECT_EQ(document->objects[rstd::usize(2)].metadata.kind, owe::wpscene::SceneObjectKind::Text);
}

TEST(SceneDocumentObjects, RejectsNonArrayObjectsAtTheCanonicalParseEntry) {
    auto document = owe::wpscene::ParseSceneDocumentJson(
        R"JSON({"camera": {}, "general": {}, "objects": {}})JSON",
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());
    ASSERT_FALSE(document->objects_are_array);

    owe::fs::VFS                vfs;
    wavsen::audio::SoundManager sound_manager;
    owe::SceneParser            parser;
    auto                        parsed = parser.Parse(
        "invalid-objects"_str,
        rstd::ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        rstd::mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
        rstd::mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound_manager)));
    ASSERT_TRUE(parsed.is_err());
    EXPECT_EQ(parsed.unwrap_err().kind, owe::SceneParseErrorKind::ObjectExpansion);
}

TEST(SceneDocumentObjects, CapturesAuthoredAutoOrthoCanvas) {
    auto document = owe::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {"orthogonalprojection": {"auto": true}},
            "objects": [
                {"id": 1, "image": "small", "size": "640 480"},
                {"id": 2, "image": "canvas", "size": "1920 1080"}
            ]
        })JSON",
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());
    ASSERT_TRUE(document->metadata.canvas_extent.is_some());
    EXPECT_EQ((*document->metadata.canvas_extent)[0], rstd::u32(1920));
    EXPECT_EQ((*document->metadata.canvas_extent)[1], rstd::u32(1080));
}

TEST(SceneObjectExpansion, AutoOrthoExtentUsesDecodedImageSize) {
    owe::wpscene::SceneMetadata metadata;
    metadata.general.orthogonalprojection.auto_ = true;
    metadata.canvas_extent =
        rstd::Some(std::array<rstd::u32, 2> { rstd::u32(640), rstd::u32(480) });

    owe::wpscene::ImageObject image;
    image.size = { 1920.0f, 1080.0f };
    rstd::vec::Vec<owe::SceneObjectVar> objects;
    objects.push(owe::SceneObjectVar::Image(rstd::move(image)));

    auto extent = owe::ResolveOrthoProjectionExtent(metadata, objects.as_slice());
    EXPECT_EQ(extent[rstd::usize()], rstd::i32(1920));
    EXPECT_EQ(extent[rstd::usize(1)], rstd::i32(1080));
}

TEST(SceneObjectExpansion, IgnoresContainerWithoutAuthoredId) {
    auto document = owe::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {},
            "objects": [
                {"name": "Missing Id"},
                {"id": 7, "name": "Valid Container"}
            ]
        })JSON",
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());

    owe::fs::VFS vfs;
    auto         objects = owe::wpscene::DecodeSceneObjects(
        rstd::ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        rstd::mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)));

    ASSERT_EQ(objects.len(), rstd::usize(1));
    ASSERT_TRUE(objects[rstd::usize()].is_Container());
    EXPECT_EQ(objects[rstd::usize()].as_Container().value.id, rstd::i32(7));
}

TEST(SceneObjectExpansion, PreservesHiddenSourceReferencedByContainer) {
    auto parsed = owe::ParseJson(R"JSON({
        "objects": [
            {"id": 7, "name": "Hidden Source", "sound": [], "visible": false},
            {"id": 8, "name": "Container", "dependencies": [7]}
        ]
    })JSON");
    ASSERT_TRUE(parsed.is_ok());

    owe::fs::VFS vfs;
    auto objects = owe::ExpandObjects(parsed.unwrap(), vfs, owe::wpscene::kSceneVersionUnknown);

    ASSERT_EQ(objects.len(), rstd::usize(2));
    ASSERT_TRUE(objects[rstd::usize()].is_Sound());
    EXPECT_TRUE(objects[rstd::usize()].as_Sound().value.visible);
    EXPECT_TRUE(objects[rstd::usize(1)].is_Container());
}

TEST(SceneObjectExpansion, ShapeOwnsItsWallpaperLayerIdentity) {
    auto document = owe::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {"orthogonalprojection": {"width": 1920, "height": 1080}},
            "objects": [{
                "id": 42,
                "name": "Direct Draw Shape",
                "shape": "quad",
                "origin": [0.0, 0.0, 0.0],
                "angles": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
                "effects": [{
                    "file": "effects/lightshafts/effect.json",
                    "visible": true,
                    "passes": [{
                        "combos": {"DIRECTDRAW": 1, "RAYMODE": 1},
                        "constantshadervalues": {
                            "point0": "-1.0 -1.0",
                            "point1": "-1.0 1.0",
                            "point2": "1.0 1.0",
                            "point3": "1.0 -1.0"
                        }
                    }]
                }]
            }]
        })JSON",
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());

    auto assets = owe::fs::make_physical_fs(owe::fs::ToPath(WAYWALLEN_ASSETS_DIR));
    ASSERT_TRUE(assets.is_ok());
    auto effect_assets = owe::fs::make_physical_fs(
        owe::fs::ToPath(std::string(WAYWALLEN_ASSETS_DIR) + "/effects/lightshafts"));
    ASSERT_TRUE(effect_assets.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(assets).unwrap_unchecked()).is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(effect_assets).unwrap_unchecked()).is_ok());

    wavsen::audio::SoundManager sound_manager;
    owe::SceneParser            parser;
    auto                        parsed = parser.Parse(
        "shape-layer-identity"_str,
        rstd::ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        rstd::mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
        rstd::mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound_manager)));
    ASSERT_TRUE(parsed.is_ok());

    auto scene = rstd::move(parsed).unwrap();
    auto shape = scene.scene->RootMut()->FindByName("Direct Draw Shape");
    ASSERT_NE(shape, nullptr);
    ASSERT_TRUE(shape->WallpaperIdentity().is_some());
    EXPECT_EQ(shape->WallpaperIdentity()->value, rstd::i32(42));
}

TEST(ImageColorBlendParsing, LinearDodgeUsesAdditiveAttachmentOwner) {
    auto document = owe::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {"orthogonalprojection": {"width": 1920, "height": 1080}},
            "objects": [{
                "id": 31,
                "name": "Linear Dodge",
                "image": "models/util/fullscreenlayer.json",
                "colorBlendMode": 31,
                "visible": true
            }]
        })JSON",
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());

    auto assets = owe::fs::make_physical_fs(owe::fs::ToPath(WAYWALLEN_ASSETS_DIR));
    ASSERT_TRUE(assets.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(assets).unwrap_unchecked()).is_ok());

    wavsen::audio::SoundManager sound_manager;
    owe::SceneParser            parser;
    auto                        parsed = parser.Parse(
        "linear-dodge-owner"_str,
        rstd::ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        rstd::mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
        rstd::mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound_manager)));
    ASSERT_TRUE(parsed.is_ok());

    auto scene = rstd::move(parsed).unwrap();
    auto node  = scene.scene->RootMut()->FindByName("Linear Dodge");
    ASSERT_NE(node, nullptr);
    ASSERT_NE(node->Mesh(), nullptr);
    ASSERT_NE(node->Mesh()->Material(), nullptr);
    EXPECT_EQ(node->Mesh()->Material()->blenmode, owe::BlendMode::Additive);
    ASSERT_TRUE(node->Mesh()->Material()->customShader.variant.is_some());
    EXPECT_EQ(node->Mesh()->Material()->customShader.variant->input_combos.at("SCENE_ORTHO"), "1");
    EXPECT_EQ(node->Mesh()->Material()->customShader.variant->input_combos.at("OWE_IMAGE_LAYER"),
              "1");
}

TEST(ImageColorBlendParsing, EffectLayerPreservesLinearDodgeAttachmentOwner) {
    auto document = owe::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {"orthogonalprojection": {"width": 1920, "height": 1080}},
            "objects": [{
                "id": 31,
                "name": "Linear Dodge Effect",
                "image": "models/util/solidlayer.json",
                "colorBlendMode": 31,
                "effects": [{
                    "file": "effects/scroll/effect.json",
                    "visible": true
                }],
                "visible": true
            }]
        })JSON",
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());

    auto assets = owe::fs::make_physical_fs(owe::fs::ToPath(WAYWALLEN_ASSETS_DIR));
    ASSERT_TRUE(assets.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(assets).unwrap_unchecked()).is_ok());

    wavsen::audio::SoundManager sound_manager;
    owe::SceneParser            parser;
    auto                        parsed = parser.Parse(
        "linear-dodge-effect-owner"_str,
        rstd::ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        rstd::mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
        rstd::mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound_manager)));
    ASSERT_TRUE(parsed.is_ok());

    auto scene = rstd::move(parsed).unwrap();
    auto node  = scene.scene->RootMut()->FindByName("Linear Dodge Effect");
    ASSERT_NE(node, nullptr);
    ASSERT_TRUE(node->HasLayer());

    auto& layer = node->Layer();
    layer->ResolveEffect(*scene.scene->DefaultEffectMesh(), "effect");
    ASSERT_FALSE(layer->ResolvedEffects().empty());
    auto* final_effect = layer->ResolvedEffects().back();
    ASSERT_NE(final_effect, nullptr);
    ASSERT_FALSE(final_effect->nodes.empty());
    auto* final_material = final_effect->nodes.back().sceneNode->Mesh()->Material();
    ASSERT_NE(final_material, nullptr);
    EXPECT_EQ(final_material->blenmode, owe::BlendMode::Additive);
}

TEST(SceneLinkedSources, EffectSelfCompositeStaysInOwningLayer) {
    auto document = owe::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {"orthogonalprojection": {"width": 1920, "height": 1080}},
            "objects": [{
                "id": 568,
                "name": "Self Composite",
                "image": "models/util/fullscreenlayer.json",
                "copybackground": true,
                "dependencies": [568],
                "effects": [{
                    "file": "effects/godrays/effect.json",
                    "visible": true,
                    "passes": [{}, {}, {}, {}, {
                        "textures": [null, "_rt_imageLayerComposite_568_a"]
                    }]
                }],
                "visible": true
            }]
        })JSON",
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());

    auto assets = owe::fs::make_physical_fs(owe::fs::ToPath(WAYWALLEN_ASSETS_DIR));
    ASSERT_TRUE(assets.is_ok());
    auto effect_assets = owe::fs::make_physical_fs(
        owe::fs::ToPath(std::string(WAYWALLEN_ASSETS_DIR) + "/effects/godrays"));
    ASSERT_TRUE(effect_assets.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(assets).unwrap_unchecked()).is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(effect_assets).unwrap_unchecked()).is_ok());

    wavsen::audio::SoundManager sound_manager;
    owe::SceneParser            parser;
    auto                        parsed = parser.Parse(
        "self-composite"_str,
        rstd::ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        rstd::mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
        rstd::mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound_manager)));
    ASSERT_TRUE(parsed.is_ok());

    auto scene = rstd::move(parsed).unwrap();
    auto node  = scene.scene->RootMut()->FindByName("Self Composite");
    ASSERT_NE(node, nullptr);
    ASSERT_TRUE(node->HasLayer());
    EXPECT_EQ(
        scene.scene->RegisteredLayerLinkSource(owe::WallpaperLayerId { .value = rstd::i32(568) }),
        nullptr);
    EXPECT_TRUE(scene.scene->RenderTarget("_rt_link_568"_str).is_none());
}

TEST(SceneLightParsing, RecognizesPrefixedKindsAndFullConeAngles) {
    auto document = owe::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {"orthogonalprojection": {"width": 1920, "height": 1080}},
            "objects": [{
                "id": 1,
                "name": "Spot",
                "light": "lspot",
                "origin": [0.0, 0.0, 0.0],
                "angles": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
                "innercone": 72.29,
                "outercone": 77.94
            }, {
                "id": 2,
                "name": "Directional",
                "light": "ldirectional",
                "origin": [0.0, 0.0, 0.0],
                "angles": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0]
            }]
        })JSON",
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());

    auto assets = owe::fs::make_physical_fs(owe::fs::ToPath(WAYWALLEN_ASSETS_DIR));
    ASSERT_TRUE(assets.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(assets).unwrap_unchecked()).is_ok());

    wavsen::audio::SoundManager sound_manager;
    owe::SceneParser            parser;
    auto                        parsed = parser.Parse(
        "prefixed-light-kinds"_str,
        rstd::ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        rstd::mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
        rstd::mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound_manager)));
    ASSERT_TRUE(parsed.is_ok());

    auto scene  = rstd::move(parsed).unwrap();
    auto lights = scene.scene->Lights();
    ASSERT_EQ(lights.len(), rstd::usize(2));
    EXPECT_EQ(lights[rstd::usize()]->type(), owe::SceneLightType::Spot);
    EXPECT_EQ(lights[rstd::usize(1)]->type(), owe::SceneLightType::Directional);
    const float deg_to_rad = rstd::f32::consts::PI.to_primitive() / 180.0f;
    EXPECT_NEAR(lights[rstd::usize()]->desc().inner_cone_cos, std::cos(72.29f * deg_to_rad), 1e-5f);
    EXPECT_NEAR(lights[rstd::usize()]->desc().outer_cone_cos, std::cos(77.94f * deg_to_rad), 1e-5f);
}

TEST(SceneShadowParsing, RequiresRendererCapabilityBeforeRegisteringDerivedResources) {
    auto document = owe::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {
                "lightconfig": {"directionalshadow": 1},
                "orthogonalprojection": {"width": 1920, "height": 1080}
            },
            "objects": [{
                "id": 1,
                "name": "Directional",
                "light": "ldirectional",
                "castshadow": true,
                "origin": [0.0, 0.0, 0.0],
                "angles": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0]
            }, {
                "id": 2,
                "name": "Caster",
                "model": "models/editor/camera/camera.mdl",
                "castshadow": true,
                "origin": [0.0, 0.0, 0.0],
                "angles": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0]
            }]
        })JSON",
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());

    auto assets = owe::fs::make_physical_fs(owe::fs::ToPath(WAYWALLEN_ASSETS_DIR));
    ASSERT_TRUE(assets.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(assets).unwrap_unchecked()).is_ok());

    wavsen::audio::SoundManager sound_manager;
    owe::SceneParser            parser;
    auto                        unsupported = parser.Parse(
        "shadow-capability-disabled"_str,
        rstd::ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        rstd::mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
        rstd::mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound_manager)));
    ASSERT_TRUE(unsupported.is_ok());
    auto unsupported_scene = rstd::move(unsupported).unwrap();
    EXPECT_TRUE(unsupported_scene.scene->ShadowDefinitions().is_empty());
    EXPECT_TRUE(unsupported_scene.scene->RenderTarget("_rt_shadowAtlas"_str).is_none());

    auto supported = parser.Parse(
        "shadow-capability-enabled"_str,
        rstd::ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        rstd::mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
        rstd::mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound_manager)),
        owe::SceneParseOptions {
            .capabilities = { .directional_shadow = true },
        });
    ASSERT_TRUE(supported.is_ok());
    auto supported_scene = rstd::move(supported).unwrap();
    ASSERT_EQ(supported_scene.scene->ShadowDefinitions().len(), rstd::usize(1));
    auto atlas = supported_scene.scene->RenderTarget("_rt_shadowAtlas"_str);
    ASSERT_TRUE(atlas.is_some());
    EXPECT_EQ((**atlas).kind, owe::SceneRenderTargetKind::DepthSampled);
    EXPECT_FLOAT_EQ((**atlas).depth_clear_value, 0.0f);
    EXPECT_TRUE((**atlas).sample.compare_enable);
    EXPECT_EQ((**atlas).sample.compare_op, owe::CompareOp::Greater);
}

TEST(ModelObjectJson, ReadsMaterialSkin) {
    auto parsed = owe::ParseJson(R"({"model":"models/prism.mdl","skin":2,"perspective":true})");
    ASSERT_TRUE(parsed.is_ok());

    owe::fs::VFS              vfs;
    owe::wpscene::ModelObject model;
    ASSERT_TRUE(model.FromJson(parsed.unwrap(), vfs));
    EXPECT_EQ(model.skin, rstd::u32(2));
    EXPECT_TRUE(model.perspective);
}

TEST(SceneCameraParsing, PerspectiveOverridePreservesTheOrthographicReferencePlane) {
    auto document = owe::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {
                "nearz": 0.01,
                "farz": 10000.0,
                "perspectiveoverridefov": 21.0,
                "orthogonalprojection": {"width": 7680, "height": 4320}
            },
            "objects": []
        })JSON",
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());

    auto assets = owe::fs::make_physical_fs(owe::fs::ToPath(WAYWALLEN_ASSETS_DIR));
    ASSERT_TRUE(assets.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(assets).unwrap_unchecked()).is_ok());

    wavsen::audio::SoundManager sound_manager;
    owe::SceneParser            parser;
    auto                        parsed = parser.Parse(
        "perspective-override"_str,
        rstd::ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        rstd::mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
        rstd::mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound_manager)));
    ASSERT_TRUE(parsed.is_ok());

    auto scene  = rstd::move(parsed).unwrap();
    auto camera = scene.scene->CameraMut("global_perspective"_str);
    ASSERT_TRUE(camera.is_some());
    EXPECT_DOUBLE_EQ((**camera).Fov(), 21.0);
    const double expected_distance =
        4320.0 / (2.0 * std::tan(21.0 * rstd::f64::consts::PI.to_primitive() / 360.0));
    EXPECT_NEAR((**camera).GetPosition().z(), expected_distance, 1e-3);

    const auto view_projection = (**camera).GetViewProjectionMatrix();
    EXPECT_NEAR(view_projection(0, 0), 3.03497815, 1e-6);
    EXPECT_NEAR(view_projection(1, 1), 5.39551687, 1e-6);
    EXPECT_NEAR(view_projection(2, 2), 0.000333444477, 1e-9);
    EXPECT_NEAR(view_projection(2, 3), 1.11559916, 1e-6);
    EXPECT_NEAR(view_projection(3, 3), expected_distance, 1e-3);
}

TEST(ImageEffectJson, FailedEffectsAreAbsentFromParsedObjects) {
    auto image_json = owe::ParseJson(R"({
        "image": "models/util/fullscreenlayer.json",
        "effects": [
            {"file": "effects/_empty/effect.json", "visible": true},
            {"file": "effects/missing/effect.json", "visible": true}
        ]
    })");
    auto shape_json = owe::ParseJson(R"({
        "shape": "rectangle",
        "origin": [0.0, 0.0, 0.0],
        "angles": [0.0, 0.0, 0.0],
        "scale": [1.0, 1.0, 1.0],
        "effects": [
            {"file": "effects/_empty/effect.json", "visible": true},
            {"file": "effects/missing/effect.json", "visible": true}
        ]
    })");
    ASSERT_TRUE(image_json.is_ok());
    ASSERT_TRUE(shape_json.is_ok());

    auto assets = owe::fs::make_physical_fs(owe::fs::ToPath(WAYWALLEN_ASSETS_DIR));
    ASSERT_TRUE(assets.is_ok());
    auto effect_assets = owe::fs::make_physical_fs(
        owe::fs::ToPath(std::string(WAYWALLEN_ASSETS_DIR) + "/effects/_empty"));
    ASSERT_TRUE(effect_assets.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(assets).unwrap_unchecked()).is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(effect_assets).unwrap_unchecked()).is_ok());

    owe::wpscene::ImageObject image;
    ASSERT_TRUE(image.FromJson(image_json.unwrap(), vfs));
    ASSERT_EQ(image.effects.size(), 1u);
    EXPECT_FALSE(image.effects[0].materials.empty());

    owe::wpscene::ShapeObject shape;
    ASSERT_TRUE(shape.FromJson(shape_json.unwrap(), vfs, owe::wpscene::kSceneVersionUnknown));
    ASSERT_EQ(shape.effects.size(), 1u);
    EXPECT_FALSE(shape.effects[0].materials.empty());
}

namespace
{

const std::vector<owe::testing::WorkshopProbe>& AllWorkshopProbes() {
    static const auto v = owe::testing::EnumerateWorkshopProbes(WAYWALLEN_WORKSHOP_DIR);
    return v;
}

const std::vector<std::string>& ObservedPkgVersionStamps() {
    static const auto v = [] {
        std::set<std::string> uniq;
        for (const auto& p : AllWorkshopProbes())
            if (! p.pkg_stamp.empty()) uniq.insert(p.pkg_stamp);
        return std::vector<std::string>(uniq.begin(), uniq.end());
    }();
    return v;
}

} // namespace

void CheckPkgVersionParse(const std::string& stamp) {
    std::size_t hits = 0;
    for (const auto& p : AllWorkshopProbes()) {
        if (p.pkg_stamp != stamp) continue;
        ++hits;
        SCOPED_TRACE("workshop " + p.id + " " + p.pkg_stamp);

        auto r = owe::testing::ProbeSceneParse(p.dir);
        EXPECT_TRUE(r.ok) << "ProbeSceneParse failed: " << r.error;
        EXPECT_EQ(r.pkg_version, p.pkg_version) << "pkg_version mismatch for " << p.id;
    }
    EXPECT_GT(hits, 0u) << "no workshops for " << stamp;
}

TEST(ScenePkgVersionParseTest, AllWorkshopsParseAtExplicitVersion) {
    for (const auto& stamp : ObservedPkgVersionStamps()) CheckPkgVersionParse(stamp);
}

TEST(SceneParseSmoke, EnumeratesNonEmpty) {
    EXPECT_FALSE(AllWorkshopProbes().empty());
    EXPECT_FALSE(ObservedPkgVersionStamps().empty());
}
