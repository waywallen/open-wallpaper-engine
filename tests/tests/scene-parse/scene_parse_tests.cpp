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
import wescene.pkg_fs;
import wescene.pkg.scene_obj;
import wescene.scene;
import wescene.spec_names;
import wescene.testing.scene_parse_probe;
import wescene.types;

using namespace rstd::literals;

TEST(FieldBindingJson, CompatibilityReaderPopulatesAnimationMetadata) {
    auto parsed = owe::ParseJson(R"({"enabled":true,"x":{"value":1.5},"y":-2.0,"magic":7})"_str);
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
    })"_str);
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
    curve.c0.push({ .frame = rstd::i32(3), .value = 1.5f });

    auto direct_curve = curve.clone();
    auto trait_curve  = rstd::as<rstd::clone::Clone>(curve).clone();
    ASSERT_EQ(direct_curve.c0.len(), rstd::usize(1));
    EXPECT_FLOAT_EQ(trait_curve.c0[rstd::usize()].value, 1.5f);

    owe::wpscene::Material material;
    material.shader      = "generic"_Str;
    auto direct_material = material.clone();
    auto trait_material  = rstd::as<rstd::clone::Clone>(material).clone();
    EXPECT_EQ(direct_material.shader, "generic"_str);
    EXPECT_EQ(trait_material.shader, "generic"_str);
}

TEST(UserBindingJson, OwnsNamesConditionsAndResetsOnReparse) {
    auto                             json = owe::ParseJson(R"({
        "visible": {"value": 0, "user": {"name": "mode", "condition": {"value": 2}}},
        "text": {"user": {"name": "title", "condition": ["first", "second"]}}
    })"_str)
                                                .unwrap();
    owe::wpscene::VisibleUserBinding visible;
    owe::wpscene::UserValueBinding   text;
    bool                             enabled = true;
    owe::wpscene::ReadVisibleProperty(json, enabled, visible);
    owe::wpscene::ReadUserValueBinding(json, "text"_str, text);
    EXPECT_FALSE(enabled);
    EXPECT_EQ(visible.name, "mode"_str);
    EXPECT_EQ(text.name, "title"_str);
    EXPECT_TRUE(visible.has_condition);
    EXPECT_TRUE(text.has_condition);
    auto visible_copy = visible.clone();
    auto text_copy    = text.clone();
    json              = owe::Json {};
    visible.name      = "changed"_Str;
    text.name         = "changed"_Str;
    visible.condition = owe::Json {};
    text.condition    = owe::Json {};
    EXPECT_EQ(visible_copy.name, "mode"_str);
    EXPECT_EQ(text_copy.name, "title"_str);
    EXPECT_TRUE(visible_copy.condition.is_object());
    EXPECT_TRUE(text_copy.condition.is_array());
    auto direct =
        owe::ParseJson(R"({"visible":{"value":1,"user":"enabled"},"text":{"user":"caption"}})"_str)
            .unwrap();
    owe::wpscene::ReadVisibleProperty(direct, enabled, visible_copy);
    owe::wpscene::ReadUserValueBinding(direct, "text"_str, text_copy);
    EXPECT_TRUE(enabled);
    EXPECT_EQ(visible_copy.name, "enabled"_str);
    EXPECT_EQ(text_copy.name, "caption"_str);
    EXPECT_FALSE(visible_copy.has_condition);
    EXPECT_TRUE(visible_copy.condition.is_null());
    EXPECT_FALSE(text_copy.has_condition);
    auto absent = owe::ParseJson("{}"_str).unwrap();
    owe::wpscene::ReadVisibleProperty(absent, enabled, visible_copy);
    owe::wpscene::ReadUserValueBinding(absent, "text"_str, text_copy);
    EXPECT_TRUE(enabled);
    EXPECT_TRUE(visible_copy.empty());
    EXPECT_TRUE(text_copy.empty());
}

TEST(SceneDocumentStorage, PreservesPathOrderAndBindingReplacement) {
    owe::wpscene::SceneCamera camera;
    auto first = owe::ParseJson(R"({"paths":["first",7,"second","first"]})"_str).unwrap();
    ASSERT_TRUE(camera.FromJson(first));
    auto second = owe::ParseJson(R"({"paths":["last"]})"_str).unwrap();
    ASSERT_TRUE(camera.FromJson(second));
    ASSERT_EQ(camera.paths.len(), rstd::usize(4));
    EXPECT_EQ(camera.paths[rstd::usize()], "first"_str);
    EXPECT_EQ(camera.paths[rstd::usize(1)], "second"_str);
    EXPECT_EQ(camera.paths[rstd::usize(2)], "first"_str);
    EXPECT_EQ(camera.paths[rstd::usize(3)], "last"_str);

    owe::wpscene::SceneGeneral general;
    auto                       bindings = owe::ParseJson(R"({
        "clearcolor":{"value":"1 1 1","user":"background"},
        "cameraparallaxamount":{"value":1,"user":"amount"},
        "camerashake":{"value":false,"user":{"name":"ignored"}}
    })"_str)
                                              .unwrap();
    ASSERT_TRUE(general.FromJson(bindings));
    bindings =
        owe::ParseJson(R"({"clearcolor":{"value":"0 0 0","user":"replacement"}})"_str).unwrap();
    ASSERT_TRUE(general.FromJson(bindings));
    ASSERT_EQ(general.user_bindings.len(), rstd::usize(2));
    ASSERT_TRUE(general.user_bindings.get("clearcolor"_str).is_some());
    EXPECT_EQ(**general.user_bindings.get("clearcolor"_str), "replacement"_str);
    EXPECT_EQ(**general.user_bindings.get("cameraparallaxamount"_str), "amount"_str);
}

TEST(SceneObjectDependencies, PreserveDuplicatesOrderAndCloneStorage) {
    owe::fs::VFS vfs;
    auto         json =
        owe::ParseJson(
            R"({"id":1,"name":"shared-name","attachment":"joint","dependencies":[3,-2,3],"sound":[]})"_str)
            .unwrap();
    owe::wpscene::ContainerObject container;
    owe::wpscene::SoundObject     sound;
    owe::wpscene::LightObject     light;
    owe::wpscene::TextObject      text;
    owe::wpscene::ModelObject     model;
    owe::wpscene::CameraObject    camera;
    ASSERT_TRUE(container.FromJson(json));
    ASSERT_TRUE(sound.FromJson(json, vfs));
    ASSERT_TRUE(light.FromJson(json, vfs));
    ASSERT_TRUE(text.FromJson(json, vfs));
    ASSERT_TRUE(model.FromJson(json, vfs));
    ASSERT_TRUE(camera.FromJson(json, vfs));
    for (const auto* values : { &container.dependencies,
                                &sound.dependencies,
                                &light.dependencies,
                                &text.dependencies,
                                &model.dependencies,
                                &camera.dependencies }) {
        ASSERT_EQ(values->len(), rstd::usize(3));
        EXPECT_EQ((*values)[rstd::usize()], rstd::i32(3));
        EXPECT_EQ((*values)[rstd::usize(1)], rstd::i32(-2));
        EXPECT_EQ((*values)[rstd::usize(2)], rstd::i32(3));
    }
    for (const auto* name :
         { &container.name, &sound.name, &light.name, &text.name, &model.name, &camera.name })
        EXPECT_EQ(*name, "shared-name"_str);
    for (const auto* attachment : { &container.attachment, &text.attachment, &model.attachment })
        EXPECT_EQ(*attachment, "joint"_str);
    owe::wpscene::ParticleObject particle;
    particle.name                        = "particle"_Str;
    particle.attachment                  = "bone"_Str;
    particle.dependencies                = container.dependencies.clone();
    auto copy                            = particle.Clone();
    particle.dependencies[rstd::usize()] = rstd::i32(99);
    particle.name                        = "changed"_Str;
    particle.attachment                  = "changed"_Str;
    EXPECT_EQ(copy.name, "particle"_str);
    EXPECT_EQ(copy.attachment, "bone"_str);
    EXPECT_EQ(copy.dependencies[rstd::usize()], rstd::i32(3));
}

TEST(SoundObjectJson, OwnsPathsAndPreservesReparseSemantics) {
    owe::fs::VFS              vfs;
    owe::wpscene::SoundObject sound;
    auto                      json = owe::ParseJson(R"({
        "sound":["first.ogg","","second.ogg","first.ogg"],
        "volume":{"value":0.25,"user":"level"},
        "playbackmode":"random",
        "queuemode":"queue",
        "visible":{"value":false,"user":"enabled"}
    })"_str)
                                         .unwrap();
    ASSERT_TRUE(sound.FromJson(json, vfs));
    json = owe::Json {};
    ASSERT_EQ(sound.sound.len(), rstd::usize(3));
    EXPECT_EQ(sound.sound[rstd::usize()], "first.ogg"_str);
    EXPECT_EQ(sound.sound[rstd::usize(1)], "second.ogg"_str);
    EXPECT_EQ(sound.sound[rstd::usize(2)], "first.ogg"_str);
    EXPECT_EQ(sound.playbackmode, "random"_str);
    EXPECT_EQ(sound.queuemode, "queue"_str);
    EXPECT_EQ(sound.volume_user_key, "level"_str);
    EXPECT_EQ(sound.visible_user_key, "enabled"_str);
    EXPECT_FALSE(sound.visible);
    auto more = owe::ParseJson(R"({"sound":["last.ogg"],"volume":0.5})"_str).unwrap();
    ASSERT_TRUE(sound.FromJson(more, vfs));
    ASSERT_EQ(sound.sound.len(), rstd::usize(4));
    EXPECT_EQ(sound.sound[rstd::usize(3)], "last.ogg"_str);
    EXPECT_EQ(sound.volume_user_key, "level"_str);
    EXPECT_TRUE(sound.visible_user_key.is_empty());
    EXPECT_FALSE(sound.visible);
}

TEST(ObjectInstanceJson, AppliesMaterialBindingOverridesBySlot) {
    auto parsed = owe::ParseJson(R"({
        "textures": [null, "linked"],
        "usertextures": [null, "replacement"],
        "combos": {"version": 2}
    })"_str);
    ASSERT_TRUE(parsed.is_ok());

    owe::wpscene::ObjectInstance instance;
    ASSERT_TRUE(instance.FromJson(parsed.unwrap()));
    owe::wpscene::Material material;
    material.textures.push("base-0"_Str);
    material.textures.push("base-1"_Str);
    instance.ApplyTo(material);

    ASSERT_EQ(material.textures.len(), rstd::usize(2));
    EXPECT_EQ(material.textures[rstd::usize(0)], "base-0"_str);
    EXPECT_EQ(material.textures[rstd::usize(1)], "linked"_str);
    ASSERT_EQ(material.usertextures.len(), rstd::usize(2));
    EXPECT_TRUE(material.usertextures[rstd::usize()].is_null());
    ASSERT_TRUE(material.usertextures[rstd::usize(1)].is_string());
    EXPECT_EQ(rstd::cppstd::to_string(*material.usertextures[rstd::usize(1)].as_str()),
              "replacement");
    EXPECT_EQ(*material.combos.get("version"_str).unwrap(), rstd::i32(2));
}

TEST(TextObjectJson, ReadsDirectUserValueBinding) {
    auto parsed = owe::ParseJson(R"({"text":{"user":"title","value":"default"}})"_str);
    ASSERT_TRUE(parsed.is_ok());

    owe::fs::VFS             vfs;
    owe::wpscene::TextObject text;
    ASSERT_TRUE(text.FromJson(parsed.unwrap(), vfs));
    EXPECT_EQ(text.text_user.name, "title"_str);
    EXPECT_TRUE(text.reflected);
}

TEST(TextObjectJson, ReadsReflectionParticipation) {
    auto parsed = owe::ParseJson(R"({"reflected":false})"_str);
    ASSERT_TRUE(parsed.is_ok());

    owe::fs::VFS             vfs;
    owe::wpscene::TextObject text;
    ASSERT_TRUE(text.FromJson(parsed.unwrap(), vfs));
    EXPECT_FALSE(text.reflected);
}

TEST(TextRenderMode, UsesDirectRenderingOnlyWithoutIndependentSurfaceRequirements) {
    EXPECT_EQ(owe::ResolveTextRenderMode({}), owe::TextRenderMode::Direct);
    EXPECT_EQ(owe::ResolveTextRenderMode({ .color_blend = true }), owe::TextRenderMode::Offscreen);
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
    })"_str);
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
        })JSON"_str,
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
        R"JSON({"camera": {}, "general": {}, "objects": {}})JSON"_str,
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
        })JSON"_str,
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());
    ASSERT_TRUE(document->metadata.canvas_extent.is_some());
    EXPECT_EQ((*document->metadata.canvas_extent)[rstd::usize(0)], rstd::u32(1920));
    EXPECT_EQ((*document->metadata.canvas_extent)[rstd::usize(1)], rstd::u32(1080));
}

TEST(SceneObjectExpansion, AutoOrthoExtentUsesDecodedImageSize) {
    owe::wpscene::SceneMetadata metadata;
    metadata.general.orthogonalprojection.auto_ = true;
    metadata.canvas_extent =
        rstd::Some(rstd::array<rstd::u32, 2> { rstd::u32(640), rstd::u32(480) });

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
        })JSON"_str,
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
    })JSON"_str);
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
        })JSON"_str,
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());

    auto assets = owe::fs::make_physical_fs(
        owe::fs::Path(rstd::cppstd::as_str(WAYWALLEN_ASSETS_DIR).unwrap()));
    ASSERT_TRUE(assets.is_ok());
    auto effect_assets = owe::fs::make_physical_fs(owe::fs::Path(
        rstd::cppstd::as_str(std::string(WAYWALLEN_ASSETS_DIR) + "/effects/lightshafts").unwrap()));
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
    auto shape = scene.scene->RootMut()->FindByName("Direct Draw Shape"_str);
    ASSERT_NE(shape, nullptr);
    ASSERT_TRUE(shape->WallpaperIdentity().is_some());
    EXPECT_EQ(shape->WallpaperIdentity()->value, rstd::i32(42));
}

TEST(PuppetScriptParsing, VisibilityScriptsInitializeTheirOwnPlayback) {
    const auto pkg_path =
        std::filesystem::path(WAYWALLEN_WORKSHOP_DIR) / "3462491575" / "scene.pkg";
    if (! std::filesystem::exists(pkg_path)) GTEST_SKIP() << "workshop 3462491575 is not available";
    auto document = owe::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {"orthogonalprojection": {"width": 1920, "height": 1080}},
            "objects": [
                {"id": 1, "name": "First", "image": "models/身体.json",
                 "animationlayers": [{"id": 101, "animation": 3835, "name": "Offset",
                    "visible": {"value": true, "scriptproperties": {"offset": 0.8},
                    "script": "export var scriptProperties = createScriptProperties().addSlider({name:'offset', value:0}).finish(); export function init(value) { if (thisObject === thisLayer) throw new Error('wrong owner'); const animation = thisObject.getAnimation(); animation.play(); animation.setFrame(animation.frameCount * scriptProperties.offset); return value; }"}}]},
                {"id": 2, "name": "Second", "image": "models/身体.json",
                 "animationlayers": [{"id": 201, "animation": 3835, "name": "Offset",
                    "visible": {"value": false, "scriptproperties": {"offset": 0.36},
                    "script": "export var scriptProperties = createScriptProperties().addSlider({name:'offset', value:0}).finish(); export function init(value) { const animation = thisObject.getAnimation(); animation.play(); animation.setFrame(animation.frameCount * scriptProperties.offset); return true; }"}}]}
            ]
        })JSON"_str,
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());
    auto assets = owe::fs::make_physical_fs(
        owe::fs::Path(rstd::cppstd::as_str(WAYWALLEN_ASSETS_DIR).unwrap()));
    ASSERT_TRUE(assets.is_ok());
    auto pkg =
        owe::fs::WPPkgFs::open(owe::fs::Path(rstd::cppstd::as_str(pkg_path.string()).unwrap()));
    ASSERT_TRUE(pkg.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(assets).unwrap_unchecked()).is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, pkg->mount_handle()).is_ok());
    wavsen::audio::SoundManager sound;
    owe::SceneParser            parser;
    auto                        parsed = parser.Parse(
        "puppet-script"_str,
        rstd::ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        rstd::mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
        rstd::mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound)));
    ASSERT_TRUE(parsed.is_ok());
    auto scene  = rstd::move(parsed).unwrap();
    auto first  = scene.scene->RootMut()->FindByName("First"_str);
    auto second = scene.scene->RootMut()->FindByName("Second"_str);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    auto first_playback  = first->NamedAnimation("Offset"_str);
    auto second_playback = second->NamedAnimation("Offset"_str);
    ASSERT_TRUE(first_playback.is_some());
    ASSERT_TRUE(second_playback.is_some());
    EXPECT_EQ((*first_playback)->Frame(), rstd::i32(96));
    EXPECT_EQ((*second_playback)->Frame(), rstd::i32(43));
    EXPECT_NEAR((*second_playback)->Sample().current, 43.2f, 0.0001f);
    EXPECT_NE((*first_playback).as_ptr(), (*second_playback).as_ptr());
}

TEST(PuppetUvParsing, ImageAndEffectMasksUseTheirBoundTextureSpace) {
    using namespace rstd::prelude;
    auto path = rstd::format("{}/3493636545/scene.pkg",
                             rstd::cppstd::as_str(WAYWALLEN_WORKSHOP_DIR).unwrap());
    auto pkg  = owe::fs::WPPkgFs::open(owe::fs::Path(path.as_str()));
    if (pkg.is_err()) GTEST_SKIP() << "workshop 3493636545 is not available";
    auto assets = owe::fs::make_physical_fs(
        owe::fs::Path(rstd::cppstd::as_str(WAYWALLEN_ASSETS_DIR).unwrap()));
    ASSERT_TRUE(assets.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, rstd::move(assets).unwrap()).is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, pkg->mount_handle()).is_ok());
    auto padded_path = rstd::format("{}/3757177441/scene.pkg",
                                    rstd::cppstd::as_str(WAYWALLEN_WORKSHOP_DIR).unwrap());
    auto padded_pkg  = owe::fs::WPPkgFs::open(owe::fs::Path(padded_path.as_str()));
    if (padded_pkg.is_err()) GTEST_SKIP() << "workshop 3757177441 is not available";
    ASSERT_TRUE(vfs.mount("/assets"_str, padded_pkg->mount_handle()).is_ok());
    auto effect_assets = owe::fs::make_physical_fs(owe::fs::Path(
        rstd::format("{}/effects/scroll", rstd::cppstd::as_str(WAYWALLEN_ASSETS_DIR).unwrap())
            .as_str()));
    ASSERT_TRUE(effect_assets.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, rstd::move(effect_assets).unwrap()).is_ok());
    auto temporary = rstd::fs::TempDir::make("owe-puppet-uv"_str).unwrap();
    auto root      = rstd::path::PathBuf::from(temporary.path().as_os_str().to_os_string());
    ASSERT_TRUE(
        rstd::fs::write(
            root.join("direct.json"_str).as_path(),
            R"({"autosize":true,"material":"materials/睫毛.json","puppet":"models/图层 1_puppet.mdl"})"_bytes)
            .is_ok());
    ASSERT_TRUE(
        rstd::fs::write(
            root.join("unpadded.json"_str).as_path(),
            R"({"autosize":true,"nopadding":true,"material":"materials/睫毛.json","puppet":"models/图层 1_puppet.mdl"})"_bytes)
            .is_ok());
    auto mount = owe::fs::make_physical_fs(temporary.path());
    ASSERT_TRUE(mount.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, rstd::move(mount).unwrap()).is_ok());
    owe::Mdl mdl;
    ASSERT_TRUE(owe::MdlParser::Parse("models/图层 1_puppet.mdl"_str, vfs, mdl));
    ASSERT_FALSE(mdl.meshes.is_empty());
    const auto& source = mdl.meshes[usize()];
    ASSERT_FALSE(source.masks.is_empty());

    auto document = owe::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {"orthogonalprojection": {"width": 1920, "height": 1080}},
            "objects": [
                {"id": 1, "name": "Direct", "image": "direct.json"},
                {"id": 2, "name": "Unpadded", "image": "unpadded.json"},
                {"id": 3, "name": "Effect", "image": "direct.json",
                 "effects": [{"file": "effects/scroll/effect.json", "visible": true}]}
            ]
        })JSON"_str,
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());
    wavsen::audio::SoundManager sound;
    owe::SceneParser            parser;
    auto                        parsed =
        parser.Parse("puppet-uv"_str,
                     ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
                     mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
                     mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound)));
    ASSERT_TRUE(parsed.is_ok());
    auto scene = rstd::move(parsed).unwrap();
    for (ref<str> name : { "Direct"_str, "Unpadded"_str, "Effect"_str }) {
        auto* node = scene.scene->RootMut()->FindByName(name);
        ASSERT_NE(node, nullptr);
        owe::SceneMesh* mesh = node->Mesh();
        if (name == "Effect"_str) {
            ASSERT_TRUE(node->HasLayer());
            auto& layer = node->Layer();
            layer->ResolveEffect(*scene.scene->DefaultEffectMesh(), "effect"_str);
            ASSERT_FALSE(layer->ResolvedEffects().is_empty());
            auto* effect = layer->ResolvedEffects().last().unwrap().get();
            ASSERT_FALSE(effect->Nodes().is_empty());
            mesh = effect->Nodes().last().unwrap().get()->sceneNode->Mesh();
        }
        ASSERT_NE(mesh, nullptr);
        ASSERT_EQ(mesh->Submeshes().len(), usize(1) + source.masks.len() * usize(2));
        array<float, 2> scale { 1.0f, 1.0f };
        if (name == "Direct"_str) {
            auto resolution =
                mesh->Material()->customShader.constValues.get("g_Texture0Resolution"_str);
            ASSERT_TRUE(resolution.is_some());
            const auto& value = **resolution;
            scale = { value[usize(2)] / value[usize()], value[usize(3)] / value[usize(1)] };
            EXPECT_TRUE(scale[usize()] != 1.0f || scale[usize(1)] != 1.0f);
        }
        for (const auto& submesh : mesh->Submeshes()) {
            ASSERT_EQ(submesh.vertex_arrays.len(), usize(1));
            const auto& vertices = submesh.vertex_arrays[usize()];
            const auto  uv =
                vertices.AttributeOffset(owe::VAttr::TexCoord.name).unwrap() / usize(sizeof(float));
            EXPECT_FLOAT_EQ(vertices.Data()[uv.to_primitive()],
                            source.texcoords[usize()][usize()] * scale[usize()]);
            EXPECT_FLOAT_EQ(vertices.Data()[uv.to_primitive() + 1],
                            source.texcoords[usize()][usize(1)] * scale[usize(1)]);
        }
    }
}

TEST(TextColorBlendParsing, PreservesOverlayAdditiveAndDirectModes) {
    auto document = owe::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {"orthogonalprojection": {"width": 1920, "height": 1080}},
            "objects": [
                {"id": 11, "name": "Overlay", "text": "12:34",
                 "font": "systemfont_DejaVu Sans", "pointsize": 96, "colorBlendMode": 11},
                {"id": 31, "name": "Additive", "text": "12:34",
                 "font": "systemfont_DejaVu Sans", "pointsize": 96, "colorBlendMode": 31},
                {"id": 1, "name": "Direct", "text": "12:34",
                 "font": "systemfont_DejaVu Sans", "pointsize": 96}
            ]
        })JSON"_str,
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());
    auto assets = owe::fs::make_physical_fs(
        owe::fs::Path(rstd::cppstd::as_str(WAYWALLEN_ASSETS_DIR).unwrap()));
    ASSERT_TRUE(assets.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(assets).unwrap_unchecked()).is_ok());
    wavsen::audio::SoundManager sound_manager;
    owe::SceneParser            parser;
    auto                        parsed = parser.Parse(
        "text-color-blend"_str,
        rstd::ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        rstd::mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
        rstd::mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound_manager)));
    ASSERT_TRUE(parsed.is_ok());
    auto scene   = rstd::move(parsed).unwrap();
    auto overlay = scene.scene->RootMut()->FindByName("Overlay"_str);
    ASSERT_NE(overlay, nullptr);
    ASSERT_NE(overlay->Mesh(), nullptr);
    auto* material = overlay->Mesh()->Material();
    ASSERT_NE(material, nullptr);
    ASSERT_TRUE(material->customShader.variant.is_some());
    EXPECT_EQ(*material->customShader.variant->input_combos.get("BLENDMODE"_str).unwrap(),
              "11"_str);
    EXPECT_EQ(material->Pipeline().blend_mode, owe::BlendMode::Translucent);
    auto additive = scene.scene->RootMut()->FindByName("Additive"_str);
    ASSERT_NE(additive, nullptr);
    ASSERT_NE(additive->Mesh(), nullptr);
    ASSERT_NE(additive->Mesh()->Material(), nullptr);
    EXPECT_EQ(additive->Mesh()->Material()->Pipeline().blend_mode, owe::BlendMode::Additive);
    auto direct = scene.scene->RootMut()->FindByName("Direct"_str);
    ASSERT_NE(direct, nullptr);
    ASSERT_NE(direct->Mesh(), nullptr);
    ASSERT_NE(direct->Mesh()->Material(), nullptr);
    EXPECT_EQ(direct->Mesh()->Material()->name, "text"_str);
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
        })JSON"_str,
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());

    auto assets = owe::fs::make_physical_fs(
        owe::fs::Path(rstd::cppstd::as_str(WAYWALLEN_ASSETS_DIR).unwrap()));
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
    auto node  = scene.scene->RootMut()->FindByName("Linear Dodge"_str);
    ASSERT_NE(node, nullptr);
    ASSERT_NE(node->Mesh(), nullptr);
    ASSERT_NE(node->Mesh()->Material(), nullptr);
    EXPECT_EQ(node->Mesh()->Material()->Pipeline().blend_mode, owe::BlendMode::Additive);
    ASSERT_TRUE(node->Mesh()->Material()->customShader.variant.is_some());
    EXPECT_EQ(*node->Mesh()
                   ->Material()
                   ->customShader.variant->input_combos.get("SCENE_ORTHO"_str)
                   .unwrap(),
              "1"_str);
    EXPECT_EQ(*node->Mesh()
                   ->Material()
                   ->customShader.variant->input_combos.get("OWE_IMAGE_LAYER"_str)
                   .unwrap(),
              "1"_str);
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
        })JSON"_str,
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());

    auto assets = owe::fs::make_physical_fs(
        owe::fs::Path(rstd::cppstd::as_str(WAYWALLEN_ASSETS_DIR).unwrap()));
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
    auto node  = scene.scene->RootMut()->FindByName("Linear Dodge Effect"_str);
    ASSERT_NE(node, nullptr);
    ASSERT_TRUE(node->HasLayer());

    auto& layer = node->Layer();
    layer->ResolveEffect(*scene.scene->DefaultEffectMesh(), "effect"_str);
    ASSERT_FALSE(layer->ResolvedEffects().is_empty());
    auto* final_effect = layer->ResolvedEffects().last().unwrap().get();
    ASSERT_NE(final_effect, nullptr);
    ASSERT_FALSE(final_effect->Nodes().is_empty());
    auto* final_material =
        final_effect->Nodes().last().unwrap().get()->sceneNode->Mesh()->Material();
    ASSERT_NE(final_material, nullptr);
    EXPECT_EQ(final_material->Pipeline().blend_mode, owe::BlendMode::Additive);
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
        })JSON"_str,
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());

    auto assets = owe::fs::make_physical_fs(
        owe::fs::Path(rstd::cppstd::as_str(WAYWALLEN_ASSETS_DIR).unwrap()));
    ASSERT_TRUE(assets.is_ok());
    auto effect_assets = owe::fs::make_physical_fs(owe::fs::Path(
        rstd::cppstd::as_str(std::string(WAYWALLEN_ASSETS_DIR) + "/effects/godrays").unwrap()));
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
    auto node  = scene.scene->RootMut()->FindByName("Self Composite"_str);
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
        })JSON"_str,
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());

    auto assets = owe::fs::make_physical_fs(
        owe::fs::Path(rstd::cppstd::as_str(WAYWALLEN_ASSETS_DIR).unwrap()));
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
        })JSON"_str,
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());

    auto assets = owe::fs::make_physical_fs(
        owe::fs::Path(rstd::cppstd::as_str(WAYWALLEN_ASSETS_DIR).unwrap()));
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
    auto parsed = owe::ParseJson(R"({"model":"models/prism.mdl","skin":2,"perspective":true})"_str);
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
        })JSON"_str,
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());

    auto assets = owe::fs::make_physical_fs(
        owe::fs::Path(rstd::cppstd::as_str(WAYWALLEN_ASSETS_DIR).unwrap()));
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

TEST(BloomParsing, PreservesPassBindingsAndAuthoredConstants) {
    auto document = owe::wpscene::ParseSceneDocumentJson(R"({
        "camera":{}, "general":{
            "orthogonalprojection":{"width":128,"height":128},
            "bloom":true,"bloomstrength":1.5,"bloomthreshold":0.25,
            "bloomtint":[0.2,0.4,0.6]
        }, "objects":[]
    })"_str,
                                                         owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());
    auto assets = owe::fs::make_physical_fs(
        owe::fs::Path(rstd::cppstd::as_str(WAYWALLEN_ASSETS_DIR).unwrap()));
    ASSERT_TRUE(assets.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, rstd::move(assets).unwrap()).is_ok());
    wavsen::audio::SoundManager sound;
    owe::SceneParser            parser;
    auto                        parsed = parser.Parse(
        "bloom-material"_str,
        rstd::ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        rstd::mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
        rstd::mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound)));
    ASSERT_TRUE(parsed.is_ok());
    auto chains = parsed->scene->PostProcesses();
    ASSERT_EQ(chains.len(), rstd::usize(1));
    const auto& steps = chains[rstd::usize()]->steps;
    ASSERT_EQ(steps.len(), rstd::usize(5));
    for (rstd::usize i {}; i < rstd::usize(4); ++i) ASSERT_TRUE(steps[i].is_Pass());
    const auto& first = steps[rstd::usize()].as_Pass().value;
    EXPECT_EQ(first.output, "_rt_bloom_mip1"_str);
    const auto* material = first.node->Mesh()->Material();
    ASSERT_NE(material, nullptr);
    ASSERT_EQ(material->textures.len(), rstd::usize(1));
    EXPECT_EQ(material->textures[rstd::usize()], "_rt_default"_str);
    const auto& values = material->customShader.constValues;
    ASSERT_TRUE(values.contains_key("g_BloomStrength"_str));
    ASSERT_TRUE(values.contains_key("g_BloomThreshold"_str));
    ASSERT_TRUE(values.contains_key("g_BloomTint"_str));
    EXPECT_FLOAT_EQ(values.get("g_BloomStrength"_str).unwrap()->data()[0], 1.5f);
    EXPECT_FLOAT_EQ(values.get("g_BloomThreshold"_str).unwrap()->data()[0], 0.25f);
    auto tint = values.get("g_BloomTint"_str).unwrap();
    ASSERT_EQ(tint->size(), rstd::usize(3));
    EXPECT_FLOAT_EQ(tint->data()[0], 0.2f);
    EXPECT_FLOAT_EQ(tint->data()[1], 0.4f);
    EXPECT_FLOAT_EQ(tint->data()[2], 0.6f);
    const auto* combine = steps[rstd::usize(3)].as_Pass().value.node->Mesh()->Material();
    ASSERT_NE(combine, nullptr);
    ASSERT_EQ(combine->textures.len(), rstd::usize(2));
    EXPECT_EQ(combine->textures[rstd::usize()], "_rt_default"_str);
    EXPECT_EQ(combine->textures[rstd::usize(1)], "_rt_bloom_mip3"_str);
    ASSERT_TRUE(steps[rstd::usize(4)].is_Copy());
    EXPECT_EQ(steps[rstd::usize(4)].as_Copy().value.src, "_rt_bloom_combine"_str);
    EXPECT_EQ(steps[rstd::usize(4)].as_Copy().value.dst, "_rt_default"_str);
}

TEST(ImageEffectJson, PreservesCopyPositionsAndComposeBindings) {
    auto json = owe::ParseJson(R"({
        "name":"compose-test", "version":1,
        "fbos":[{"name":"scratch","format":"rgba8","scale":2}],
        "passes":[
            {"command":"copy","source":"previous","target":"scratch"},
            {"material":"materials/util/passthrough.json"},
            {"command":"copy","source":"scratch","target":"previous"},
            {"material":"materials/util/passthrough.json","compose":true}
        ]
    })"_str);
    ASSERT_TRUE(json.is_ok());
    auto assets = owe::fs::make_physical_fs(
        owe::fs::Path(rstd::cppstd::as_str(WAYWALLEN_ASSETS_DIR).unwrap()));
    ASSERT_TRUE(assets.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, rstd::move(assets).unwrap()).is_ok());
    owe::wpscene::ImageEffect effect;
    ASSERT_TRUE(effect.FromFileJson(*json, vfs));
    EXPECT_EQ(effect.name, "compose-test"_str);
    ASSERT_EQ(effect.commands.len(), rstd::usize(2));
    EXPECT_EQ(effect.commands[rstd::usize()].afterpos, rstd::i32());
    EXPECT_EQ(effect.commands[rstd::usize(1)].afterpos, rstd::i32(1));
    EXPECT_EQ(effect.commands[rstd::usize()].source, "previous"_str);
    ASSERT_EQ(effect.materials.len(), rstd::usize(2));
    ASSERT_EQ(effect.passes.len(), rstd::usize(2));
    ASSERT_EQ(effect.fbos.len(), rstd::usize(2));
    EXPECT_EQ(effect.fbos[rstd::usize()].name, "scratch"_str);
    EXPECT_EQ(effect.fbos[rstd::usize()].scale, rstd::u32(2));
    EXPECT_EQ(effect.fbos[rstd::usize(1)].name, "_rt_FullCompoBuffer1"_str);
    EXPECT_EQ(effect.passes[rstd::usize()].target, "_rt_FullCompoBuffer1"_str);
    ASSERT_EQ(effect.passes[rstd::usize()].bind.len(), rstd::usize(1));
    ASSERT_EQ(effect.passes[rstd::usize(1)].bind.len(), rstd::usize(1));
    EXPECT_EQ(effect.passes[rstd::usize()].bind[rstd::usize()].name, "previous"_str);
    EXPECT_EQ(effect.passes[rstd::usize(1)].bind[rstd::usize()].name, "_rt_FullCompoBuffer1"_str);
}

TEST(ImageEffectJson, FailedEffectsAreAbsentFromParsedObjects) {
    auto image_json = owe::ParseJson(R"({
        "image": "models/util/fullscreenlayer.json",
        "effects": [
            {"file": "effects/_empty/effect.json", "visible": true},
            {"file": "effects/missing/effect.json", "visible": true}
        ]
    })"_str);
    auto shape_json = owe::ParseJson(R"({
        "shape": "rectangle",
        "origin": [0.0, 0.0, 0.0],
        "angles": [0.0, 0.0, 0.0],
        "scale": [1.0, 1.0, 1.0],
        "effects": [
            {"file": "effects/_empty/effect.json", "visible": true},
            {"file": "effects/missing/effect.json", "visible": true}
        ]
    })"_str);
    ASSERT_TRUE(image_json.is_ok());
    ASSERT_TRUE(shape_json.is_ok());

    auto assets = owe::fs::make_physical_fs(
        owe::fs::Path(rstd::cppstd::as_str(WAYWALLEN_ASSETS_DIR).unwrap()));
    ASSERT_TRUE(assets.is_ok());
    auto effect_assets = owe::fs::make_physical_fs(owe::fs::Path(
        rstd::cppstd::as_str(std::string(WAYWALLEN_ASSETS_DIR) + "/effects/_empty").unwrap()));
    ASSERT_TRUE(effect_assets.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(assets).unwrap_unchecked()).is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, std::move(effect_assets).unwrap_unchecked()).is_ok());

    owe::wpscene::ImageObject image;
    ASSERT_TRUE(image.FromJson(image_json.unwrap(), vfs));
    ASSERT_EQ(image.effects.len(), rstd::usize(1));
    EXPECT_FALSE(image.effects[rstd::usize()].materials.is_empty());

    owe::wpscene::ShapeObject shape;
    ASSERT_TRUE(shape.FromJson(shape_json.unwrap(), vfs, owe::wpscene::kSceneVersionUnknown));
    ASSERT_EQ(shape.effects.len(), rstd::usize(1));
    EXPECT_FALSE(shape.effects[rstd::usize()].materials.is_empty());
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

TEST(SceneDocumentLoading, VersionStampPreservesStrictUnsignedGrammar) {
    using owe::wpscene::ParsePkgVersionStamp;
    EXPECT_EQ(ParsePkgVersionStamp("PKGV0001"_str), 1);
    EXPECT_EQ(ParsePkgVersionStamp("PKGV65535"_str), 65535);
    EXPECT_EQ(ParsePkgVersionStamp("PKGV00000023"_str), 23);
    for (auto invalid : rstd::array<rstd::ref<rstd::str>, 11> { ""_str,
                                                                "PKGV"_str,
                                                                "pkgv23"_str,
                                                                " PKGV23"_str,
                                                                "PKGV+23"_str,
                                                                "PKGV-1"_str,
                                                                "PKGV23 "_str,
                                                                "PKGV23x"_str,
                                                                "PKGV65536"_str,
                                                                "PKGV999999999999999999999"_str,
                                                                "PKGV23\0tail"_str }) {
        EXPECT_EQ(ParsePkgVersionStamp(invalid), owe::wpscene::kSceneVersionUnknown);
    }
}

TEST(SceneDocumentLoading, NativePathsShareDocumentParsingAndRejectInvalidInputs) {
    auto temporary = rstd::fs::TempDir::make("owe-scene-document"_str).unwrap();
    auto root      = rstd::path::PathBuf::from(temporary.path().as_os_str().to_os_string());
    auto source    = root.join("scene.JsOn"_str);
    auto content   = R"({"camera":{},"general":{},"objects":[{"id":7,"name":"kept"}]})"_str;
    ASSERT_TRUE(rstd::fs::write(source.as_path(), content->as_bytes()).is_ok());

    auto direct = owe::wpscene::LoadSceneDocumentFromSource(source.as_path());
    ASSERT_TRUE(direct.is_some());
    ASSERT_EQ(direct->objects.len(), rstd::usize(1));
    EXPECT_EQ(direct->objects[rstd::usize()].metadata.name, "kept"_str);
    EXPECT_EQ(direct->metadata.pkg_version, owe::wpscene::kSceneVersionUnknown);

    owe::fs::VFS vfs;
    auto         mount = owe::fs::make_physical_fs(temporary.path());
    ASSERT_TRUE(mount.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, rstd::move(mount).unwrap()).is_ok());
    auto mounted =
        owe::wpscene::LoadSceneDocumentFromVfs(vfs, owe::fs::Path("/assets/scene.JsOn"_str), 23);
    ASSERT_TRUE(mounted.is_some());
    EXPECT_EQ(mounted->metadata.pkg_version, 23);
    ASSERT_EQ(mounted->objects.len(), rstd::usize(1));
    EXPECT_EQ(mounted->objects[rstd::usize()].metadata.id, rstd::i32(7));

    auto unsupported = root.join("scene.txt"_str);
    ASSERT_TRUE(rstd::fs::write(unsupported.as_path(), content->as_bytes()).is_ok());
    EXPECT_TRUE(owe::wpscene::LoadSceneDocumentFromSource(unsupported.as_path()).is_none());
    EXPECT_TRUE(owe::wpscene::LoadSceneDocumentFromSource(root.join("missing.json"_str).as_path())
                    .is_none());
    ASSERT_TRUE(rstd::fs::write(source.as_path(), "{"_bytes).is_ok());
    EXPECT_TRUE(owe::wpscene::LoadSceneDocumentFromSource(source.as_path()).is_none());
    EXPECT_TRUE(
        owe::wpscene::LoadSceneDocumentFromVfs(vfs, owe::fs::Path("/assets/scene.JsOn"_str), 23)
            .is_none());
}

TEST(SceneAssetPaths, ImageMetadataAndObjectOwnNativeResourceNames) {
    auto temporary     = rstd::fs::TempDir::make("owe-scene-assets"_str).unwrap();
    auto root          = rstd::path::PathBuf::from(temporary.path().as_os_str().to_os_string());
    auto image_path    = root.join("image.json"_str);
    auto material_path = root.join("material.json"_str);
    ASSERT_TRUE(rstd::fs::write(
                    image_path.as_path(),
                    R"({"material":"material.json","puppet":"model.mdl","solidlayer":true})"_bytes)
                    .is_ok());
    ASSERT_TRUE(rstd::fs::write(
                    material_path.as_path(),
                    R"({"passes":[{"shader":"genericimage","textures":["texture-name"]}]})"_bytes)
                    .is_ok());
    owe::fs::VFS vfs;
    auto         mount = owe::fs::make_physical_fs(temporary.path());
    ASSERT_TRUE(mount.is_ok());
    ASSERT_TRUE(vfs.mount("/assets"_str, rstd::move(mount).unwrap()).is_ok());

    auto info = owe::wpscene::LoadImageAssetInfo(vfs, "image.json"_str);
    ASSERT_TRUE(info.is_some());
    EXPECT_TRUE(info->solid_layer);
    EXPECT_TRUE(info->size.is_none());
    EXPECT_EQ(info->first_texture, "texture-name"_str);

    owe::wpscene::ImageObject image;
    ASSERT_TRUE(image.FromAsset("image.json"_str, { 4.0f, 8.0f }, vfs, 23));
    EXPECT_EQ(image.image, "image.json"_str);
    EXPECT_EQ(image.material_path, "material.json"_str);
    EXPECT_EQ(image.puppet, "model.mdl"_str);

    ASSERT_TRUE(rstd::fs::write(image_path.as_path(),
                                R"({"width":16,"height":32,"material":"missing.json"})"_bytes)
                    .is_ok());
    info = owe::wpscene::LoadImageAssetInfo(vfs, "image.json"_str);
    ASSERT_TRUE(info.is_some());
    ASSERT_TRUE(info->size.is_some());
    EXPECT_FLOAT_EQ((*info->size)[rstd::usize(0)], 16.0f);
    EXPECT_FLOAT_EQ((*info->size)[rstd::usize(1)], 32.0f);
    EXPECT_TRUE(info->first_texture.is_empty());
    EXPECT_EQ(image.material_path, "material.json"_str);
}

TEST(SceneAssetPaths, ModelCameraAndClipRetainTheirReparseSemantics) {
    owe::fs::VFS vfs;
    auto         source =
        rstd::json::from_str(
            R"({"model":"mesh.mdl","camera":"main","path":"motion.json","name":"clip"})"_str)
            .unwrap();
    owe::wpscene::ModelObject    model;
    owe::wpscene::CameraObject   camera;
    owe::wpscene::CameraPathClip clip;
    ASSERT_TRUE(model.FromJson(source, vfs));
    ASSERT_TRUE(camera.FromJson(source, vfs));
    ASSERT_TRUE(clip.FromJson(source));
    source = owe::Json {};
    EXPECT_EQ(model.model, "mesh.mdl"_str);
    EXPECT_EQ(camera.camera, "main"_str);
    EXPECT_EQ(camera.path, "motion.json"_str);
    EXPECT_EQ(clip.name, "clip"_str);

    auto empty = rstd::json::from_str("{}"_str).unwrap();
    ASSERT_TRUE(model.FromJson(empty, vfs));
    ASSERT_TRUE(camera.FromJson(empty, vfs));
    ASSERT_TRUE(clip.FromJson(empty));
    EXPECT_EQ(model.model, "mesh.mdl"_str);
    EXPECT_EQ(camera.path, "motion.json"_str);
    EXPECT_TRUE(clip.name.is_empty());
}
