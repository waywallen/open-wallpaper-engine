#include <rstd/test/gtest.hpp>

import rstd;
import rstd.cppstd;
import eigen;
import wescene.types;
import wescene.json;
import wescene.scene;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::cppstd::as_str;
using rstd::sync::Arc;

namespace
{

Arc<owe::SceneMesh> MakeSingleSubmesh(std::string name) {
    auto mesh = Arc<owe::SceneMesh>::make();

    owe::SceneMaterial material;
    material.name = rstd::into(rstd::cppstd::as_str(name).unwrap());
    mesh->AddMaterial(std::move(material));

    owe::SceneMesh::Submesh submesh;
    submesh.material_slot = u32();
    mesh->Submeshes().push(std::move(submesh));
    return mesh;
}

class FakeImageParser {
public:
    auto Parse(ref<str>) const -> Result<Arc<owe::Image>, owe::ImageParseError> {
        return Err(owe::ImageParseError {
            .kind    = owe::ImageParseErrorKind::MissingContent,
            .message = String::make("missing fake image"_str),
        });
    }
    auto ParseMany(slice<String> names) const
        -> Vec<Result<Arc<owe::Image>, owe::ImageParseError>> {
        auto images =
            Vec<Result<Arc<owe::Image>, owe::ImageParseError>>::with_capacity(names.len());
        for (usize index {}; index < names.len(); ++index) {
            images.push(Parse(names[index].as_str()));
        }
        return images;
    }
    auto ParseHeader(ref<str>) const -> Result<owe::ImageHeader, owe::ImageParseError> {
        owe::ImageHeader header;
        header.width  = 64;
        header.height = 32;
        return Ok(rstd::move(header));
    }
};

struct FakeSceneExtension {
    int value { 0 };
};

} // namespace

TEST(SceneExtensions, StoresAndReplacesTypedOwners) {
    owe::Scene scene;
    scene.InstallExtension(Box<FakeSceneExtension>::make(FakeSceneExtension { .value = 7 }));

    auto extension = scene.Extension<FakeSceneExtension>();
    ASSERT_TRUE(extension.is_some());
    EXPECT_EQ((**extension).value, 7);

    auto mutable_extension = scene.ExtensionMut<FakeSceneExtension>();
    ASSERT_TRUE(mutable_extension.is_some());
    (**mutable_extension).value = 11;
    EXPECT_EQ((**scene.Extension<FakeSceneExtension>()).value, 11);

    scene.InstallExtension(Box<FakeSceneExtension>::make(FakeSceneExtension { .value = 13 }));
    EXPECT_EQ((**scene.Extension<FakeSceneExtension>()).value, 13);
}

TEST(SceneLights, OwnsRegisteredLightsBehindBorrowedViews) {
    owe::Scene scene;
    auto       light =
        scene.RegisterLight(Box<owe::SceneLight>::make(owe::SceneLight::Desc { .radius = 4.0f }));
    light->setRuntimeVisible(false);

    auto lights = scene.Lights();
    ASSERT_EQ(lights.len(), usize(1));
    EXPECT_FLOAT_EQ(lights[usize()]->radius(), 4.0f);
    EXPECT_FALSE(lights[usize()]->runtimeVisible());
}

TEST(SceneMesh, CloneInstanceSharesGeometryAndOwnsMaterials) {
    auto source = MakeSingleSubmesh("source");
    auto clone  = source->CloneInstance();

    ASSERT_EQ(source->Submeshes().len().to_primitive(), 1u);
    ASSERT_EQ(clone->Submeshes().len().to_primitive(), 1u);
    ASSERT_EQ(source->MaterialSlots().len().to_primitive(), 1u);
    ASSERT_EQ(clone->MaterialSlots().len().to_primitive(), 1u);
    EXPECT_EQ(&source->Submeshes()[usize(0)], &clone->Submeshes()[usize(0)]);
    EXPECT_NE(source->MaterialSlots()[usize()].as_ptr().as_raw_ptr(),
              clone->MaterialSlots()[usize()].as_ptr().as_raw_ptr());

    clone->MaterialSlots()[usize()]->name = "clone"_Str;
    EXPECT_EQ(source->MaterialSlots()[usize()]->name, "source"_str);
    EXPECT_EQ(clone->MaterialSlots()[usize()]->name, "clone"_str);
}

TEST(SceneMesh, CloneKeepsGeometryAfterSourceDestructionAndCanReplaceIt) {
    auto clone = [] {
        auto source = MakeSingleSubmesh("source");
        return source->CloneInstance();
    }();
    ASSERT_EQ(clone->Submeshes().len(), usize(1));
    EXPECT_EQ(clone->Material()->name, "source"_str);

    {
        auto replacement = MakeSingleSubmesh("replacement");
        replacement->Submeshes().emplace_back();
        clone->ChangeMeshDataFrom(*replacement);
        EXPECT_EQ(&clone->Submeshes()[usize()], &replacement->Submeshes()[usize()]);
    }
    EXPECT_EQ(clone->Submeshes().len(), usize(2));
    EXPECT_EQ(clone->Material()->name, "source"_str);
}

TEST(SceneMesh, NodeReleasesReplacedMeshAndOwnsCurrentMesh) {
    auto previous = rstd::sync::Weak<owe::SceneMesh>::make();
    auto current  = rstd::sync::Weak<owe::SceneMesh>::make();
    {
        owe::SceneNode node;
        EXPECT_EQ(node.Mesh(), nullptr);
        auto mesh = MakeSingleSubmesh("previous");
        previous  = mesh.downgrade();
        node.AddMesh(rstd::move(mesh));
        EXPECT_TRUE(previous.upgrade());
        auto replacement = MakeSingleSubmesh("current");
        current          = replacement.downgrade();
        node.AddMesh(rstd::move(replacement));
        EXPECT_TRUE(! previous.upgrade());
        ASSERT_NE(node.Mesh(), nullptr);
        EXPECT_EQ(node.Mesh()->Material()->name, "current"_str);
    }
    EXPECT_TRUE(! current.upgrade());
}

TEST(SceneMesh, ConsumesOnlyRequestedDirtyFlags) {
    owe::SceneMesh mesh;
    mesh.SetDirty(owe::SceneMeshDirtyData | owe::SceneMeshDirtyLayout);
    EXPECT_EQ(mesh.ConsumeDirtyFlags(owe::SceneMeshDirtyData), owe::SceneMeshDirtyData);
    EXPECT_EQ(mesh.DirtyFlags(), owe::SceneMeshDirtyLayout);
    EXPECT_TRUE(mesh.Dirty().load());
    EXPECT_EQ(mesh.ConsumeDirtyFlags(), owe::SceneMeshDirtyLayout);
    EXPECT_EQ(mesh.DirtyFlags(), owe::SceneMeshDirtyNone);
    EXPECT_FALSE(mesh.Dirty().load());
}

TEST(SceneMaterial, CopyKeepsShaderAliveUntilLastOwnerIsDestroyed) {
    auto weak = rstd::sync::Weak<owe::SceneShader>::make();
    {
        owe::SceneMaterial copy;
        {
            owe::SceneMaterial source;
            auto               shader  = Arc<owe::SceneShader>::make();
            shader->name               = "shared"_Str;
            weak                       = shader.downgrade();
            source.customShader.shader = Some(rstd::move(shader));
            copy                       = source;
            ASSERT_TRUE(copy.customShader.shader);
            EXPECT_EQ((*copy.customShader.shader).as_ptr().as_raw_ptr(),
                      (*source.customShader.shader).as_ptr().as_raw_ptr());
        }
        EXPECT_TRUE(weak.upgrade());
        EXPECT_EQ((*copy.customShader.shader)->name.as_str(), "shared"_str);
    }
    EXPECT_TRUE(! weak.upgrade());
}

TEST(SceneResourceIndex, KeepsNamedIdentitiesWhenEarlierNamesAreAdded) {
    owe::Scene scene;
    scene.RegisterTexture(String::make("z/texture"_str),
                          owe::SceneTexture { .url = "z/texture"_Str });
    scene.RegisterRenderTarget(String::make("z/target"_str),
                               owe::SceneRenderTarget { .width = i32(37), .height = i32(19) });
    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(37, 19, -1.0, 1.0));
    scene.RegisterCamera(String::make("z/camera"_str), camera.clone());
    scene.RebuildResourceIndex();
    auto texture_id = scene.ResourceIndex().textureId("z/texture"_str).unwrap();
    auto target_id  = scene.ResourceIndex().renderTargetId("z/target"_str).unwrap();
    auto camera_id  = scene.ResourceIndex().cameraId("z/camera"_str).unwrap();
    scene.RegisterTexture(String::make("a/texture"_str),
                          owe::SceneTexture { .url = "a/texture"_Str });
    scene.RegisterRenderTarget(String::make("a/target"_str),
                               owe::SceneRenderTarget { .width = i32(1), .height = i32(1) });
    scene.RegisterCamera(
        String::make("a/camera"_str),
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(1, 1, -1.0, 1.0)));
    scene.RebuildResourceIndex();
    EXPECT_EQ(scene.ResourceIndex().textureId("z/texture"_str).unwrap(), texture_id);
    EXPECT_EQ(scene.ResourceIndex().renderTargetId("z/target"_str).unwrap(), target_id);
    EXPECT_EQ(scene.ResourceIndex().cameraId("z/camera"_str).unwrap(), camera_id);
    ASSERT_NE(scene.ResourceIndex().texture(texture_id), nullptr);
    EXPECT_EQ(scene.ResourceIndex().texture(texture_id)->url, "z/texture"_str);
    ASSERT_NE(scene.ResourceIndex().renderTarget(target_id), nullptr);
    EXPECT_EQ(scene.ResourceIndex().renderTarget(target_id)->width, i32(37));
    EXPECT_EQ(scene.ResourceIndex().camera(camera_id), camera.as_ptr());
}

TEST(SceneResourceIndex, ResolvesDrawItemsAndNamedResources) {
    owe::Scene scene;
    scene.RootMut()->ID() = rstd::i32(1);
    EXPECT_EQ(scene.Root()->ID(), rstd::i32(1));
    scene.RegisterTexture(String::make("tex/main"_str),
                          owe::SceneTexture { .url = "tex/main"_Str });
    scene.RegisterRenderTarget(String::make("_rt_default"_str),
                               owe::SceneRenderTarget { .width = i32(1920), .height = i32(1080) });
    auto default_camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(1920, 1080, -1.0, 1.0));
    scene.RegisterCamera(String::make("default"_str), default_camera.clone());

    auto child      = rstd::sync::Arc<owe::SceneNode>::make();
    child->ID()     = rstd::i32(2);
    auto child_mesh = MakeSingleSubmesh("child-material");
    child->AddMesh(child_mesh.clone());
    scene.RootMut()->AppendChild(child.clone());

    auto post_node  = rstd::sync::Arc<owe::SceneNode>::make();
    post_node->ID() = rstd::i32(3);
    auto post_mesh  = MakeSingleSubmesh("post-material");
    post_node->AddMesh(post_mesh.clone());

    auto post = rstd::boxed::Box<owe::ScenePostProcess>::make();
    post->steps.push(owe::ScenePostProcessStep::Pass(
        owe::ScenePostProcessPass { .node = post_node.clone(), .output = "_rt_post"_Str }));
    (void)scene.RegisterPostProcess(rstd::move(post));
    ASSERT_EQ(scene.PostProcesses().len(), usize(1));

    scene.RebuildResourceIndex();
    const auto& index = scene.ResourceIndex();

    auto child_node_id = index.nodeId(*child.as_ptr());
    ASSERT_TRUE(child_node_id.is_some());
    EXPECT_EQ(index.node(*child_node_id), child.as_ptr());

    auto child_draw_id = index.drawItemFor(*child_node_id, rstd::u32());
    ASSERT_TRUE(child_draw_id.is_some());

    auto child_draw = index.resolve(*child_draw_id);
    ASSERT_TRUE(child_draw.is_some());
    EXPECT_EQ(child_draw->node, child.as_ptr());
    EXPECT_EQ(child_draw->mesh, child_mesh.as_ptr().as_raw_ptr());
    EXPECT_EQ(child_draw->material, child_mesh->MaterialSlots()[usize()].as_ptr().as_raw_ptr());
    EXPECT_EQ(child_draw->submesh, &child_mesh->Submeshes()[usize(0)]);

    auto post_node_id = index.nodeId(*post_node.as_ptr());
    ASSERT_TRUE(post_node_id.is_some());
    auto post_draw_id = index.drawItemFor(*post_node_id, rstd::u32());
    ASSERT_TRUE(post_draw_id.is_some());
    EXPECT_EQ(index.resolve(*post_draw_id)->material,
              post_mesh->MaterialSlots()[usize()].as_ptr().as_raw_ptr());

    auto texture_id = index.textureId("tex/main"_str);
    ASSERT_TRUE(texture_id.is_some());
    EXPECT_EQ(index.texture(*texture_id)->url, "tex/main"_str);

    auto rt_id = index.renderTargetId("_rt_default"_str);
    ASSERT_TRUE(rt_id.is_some());
    EXPECT_EQ(index.renderTarget(*rt_id)->width, i32(1920));
    EXPECT_EQ(index.mutableRenderTarget(*rt_id)->height, i32(1080));

    auto camera_id = index.cameraId("default"_str);
    ASSERT_TRUE(camera_id.is_some());
    EXPECT_EQ(index.camera(*camera_id), default_camera.as_ptr());

    owe::Scene other_scene;
    other_scene.RebuildResourceIndex();
    EXPECT_EQ(other_scene.ResourceIndex().node(*child_node_id), nullptr);
    EXPECT_TRUE(other_scene.ResourceIndex().resolve(*child_draw_id).is_none());
}

TEST(SceneResourceIndex, RebuildPicksUpNewRenderTargets) {
    owe::Scene scene;
    scene.RebuildResourceIndex();
    EXPECT_TRUE(scene.ResourceIndex().renderTargetId("_rt_link_7"_str).is_none());

    scene.RegisterRenderTarget(String::make("_rt_link_7"_str),
                               owe::SceneRenderTarget { .width = i32(64), .height = i32(32) });
    scene.RebuildResourceIndex();

    auto id = scene.ResourceIndex().renderTargetId("_rt_link_7"_str);
    ASSERT_TRUE(id.is_some());
    ASSERT_NE(scene.ResourceIndex().mutableRenderTarget(*id), nullptr);
    EXPECT_EQ(scene.ResourceIndex().mutableRenderTarget(*id)->width, i32(64));
    EXPECT_EQ(scene.ResourceIndex().renderTarget(*id)->height, i32(32));
}

TEST(SceneIdentity, RegistersStableNodeAndEffectOwners) {
    owe::Scene scene;
    auto       first  = Arc<owe::SceneNode>::make();
    auto       second = Arc<owe::SceneNode>::make();
    auto       first_id =
        scene.RegisterNode(*first, Some(owe::WallpaperLayerId { .value = rstd::i32(7) }));
    auto second_id = scene.RegisterNode(*second);

    EXPECT_TRUE(first_id.Valid());
    EXPECT_TRUE(second_id.Valid());
    EXPECT_NE(first_id.index, second_id.index);
    EXPECT_EQ(scene.RegisterNode(*first), first_id);
    EXPECT_EQ(first->WallpaperIdentity()->value, rstd::i32(7));
    auto duplicate = Arc<owe::SceneNode>::make();
    auto duplicate_id =
        scene.RegisterNode(*duplicate, Some(owe::WallpaperLayerId { .value = rstd::i32(7) }));
    EXPECT_NE(duplicate_id, first_id);
    EXPECT_TRUE(duplicate->WallpaperIdentity().is_none());

    auto layer = Arc<owe::SceneNodeLayer>::make(first.as_ptr(), 64.0f, 32.0f, "_rt_composite"_str);
    auto effect_a    = Arc<owe::SceneImageEffect>::make();
    auto effect_b    = Arc<owe::SceneImageEffect>::make();
    effect_a->name   = "duplicate"_Str;
    effect_b->name   = "duplicate"_Str;
    auto effect_a_id = scene.RegisterEffect(first_id, *layer, effect_a.clone());
    auto effect_b_id = scene.RegisterEffect(first_id, *layer, effect_b.clone());
    EXPECT_TRUE(effect_a_id.Valid());
    EXPECT_TRUE(effect_b_id.Valid());
    EXPECT_NE(effect_a_id.index, effect_b_id.index);
    layer->SetPublishedEffect(Arc<owe::SceneImageEffect>::make());
    layer->SetVisibleResolveEffect(Arc<owe::SceneImageEffect>::make());
    first->AttachLayer(layer.clone());
    first->SetBaseColor({ 1.0f, 1.0f, 1.0f }, 0.5f);
    EXPECT_TRUE(layer->VisibleOutputEnabled());
    EXPECT_TRUE(scene.SetNodeVisible(*first, false));
    EXPECT_FALSE(layer->VisibleOutputEnabled());
    EXPECT_FLOAT_EQ(first->EffectiveAlpha(), 0.5f);
    EXPECT_TRUE(scene.SetNodeVisible(*first, true));
    EXPECT_TRUE(layer->VisibleOutputEnabled());

    auto node_key = scene.NodeResourceKey(first_id, "layer_pingpong_a"_str);
    auto fbo_a    = scene.EffectResourceKey(effect_a_id, "half"_str);
    auto fbo_b    = scene.EffectResourceKey(effect_b_id, "half"_str);
    EXPECT_EQ(rstd::cppstd::as_string_view(node_key), "_rt_node_1_layer_pingpong_a");
    EXPECT_NE(fbo_a, fbo_b);

    scene.RegisterLayerLinkSource(owe::WallpaperLayerId { .value = rstd::i32(7) }, *first);
    scene.RegisterRenderTarget(String::make("_rt_default"_str), owe::SceneRenderTarget {});
    scene.RegisterRenderTarget(node_key.clone(), owe::SceneRenderTarget {});
    owe::SceneMaterial material;
    material.textures.push("_rt_link_7"_Str);
    material.textures.push(fbo_a.clone());
    material.textures.push("_rt_default"_Str);
    material.textures.push("asset/image"_Str);
    material.textures.push("_rt_imageLayerComposite_7_b"_Str);
    material.textures.push(node_key.clone());
    material.textures.push("_rt_unknown"_Str);
    scene.ResolveMaterialTextureSources(material);
    ASSERT_EQ(material.texture_sources.len(), usize(7));
    EXPECT_EQ(material.texture_sources[usize()].kind,
              owe::SceneMaterialTextureSourceKind::LayerOutput);
    ASSERT_TRUE(material.texture_sources[usize()].layer.is_some());
    EXPECT_EQ(*material.texture_sources[usize()].layer, first_id);
    EXPECT_EQ(material.texture_sources[usize(1)].kind,
              owe::SceneMaterialTextureSourceKind::EffectLocal);
    ASSERT_TRUE(material.texture_sources[usize(1)].effect.is_some());
    EXPECT_EQ(*material.texture_sources[usize(1)].effect, effect_a_id);
    EXPECT_EQ(material.texture_sources[usize(2)].kind,
              owe::SceneMaterialTextureSourceKind::SceneSurface);
    EXPECT_EQ(material.texture_sources[usize(3)].kind,
              owe::SceneMaterialTextureSourceKind::Imported);
    EXPECT_EQ(material.texture_sources[usize(4)].kind,
              owe::SceneMaterialTextureSourceKind::LayerOutput);
    EXPECT_EQ(material.texture_sources[usize(4)].key, "_rt_link_7"_str);
    ASSERT_TRUE(material.texture_sources[usize(4)].layer.is_some());
    EXPECT_EQ(*material.texture_sources[usize(4)].layer, first_id);
    EXPECT_EQ(material.texture_sources[usize(5)].kind,
              owe::SceneMaterialTextureSourceKind::LayerStage);
    ASSERT_TRUE(material.texture_sources[usize(5)].layer.is_some());
    EXPECT_EQ(*material.texture_sources[usize(5)].layer, first_id);
    EXPECT_EQ(material.texture_sources[usize(6)].kind,
              owe::SceneMaterialTextureSourceKind::UnsupportedSpecial);

    scene.RootMut()->AppendChild(first.clone());
    scene.RootMut()->AppendChild(second.clone());
    scene.RebuildResourceIndex();
    auto rebuilt_id = scene.ResourceIndex().nodeId(*scene.ResourceIndex().node(first_id));
    ASSERT_TRUE(rebuilt_id.is_some());
    EXPECT_EQ(*rebuilt_id, first_id);
    scene.RebuildResourceIndex();
    rebuilt_id = scene.ResourceIndex().nodeId(*scene.ResourceIndex().node(first_id));
    ASSERT_TRUE(rebuilt_id.is_some());
    EXPECT_EQ(*rebuilt_id, first_id);
}

TEST(SceneResourceIndex, IncludesAllNodeLayerEffectDrawItems) {
    owe::Scene scene;
    auto       camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(1920, 1080, -1.0, 1.0));
    auto layer = Arc<owe::SceneNodeLayer>::make(
        scene.RootMut().as_raw_ptr(), 1920.0f, 1080.0f, "_rt_composite"_str);

    auto prefill = rstd::sync::Arc<owe::SceneNode>::make();
    prefill->AddMesh(MakeSingleSubmesh("prefill"));
    layer->AddPrefillNode(owe::SceneImageEffectNode { .sceneNode = prefill.clone() });

    auto effect_node = rstd::sync::Arc<owe::SceneNode>::make();
    effect_node->AddMesh(MakeSingleSubmesh("effect"));
    auto effect = Arc<owe::SceneImageEffect>::make();
    effect->AddNode(owe::SceneImageEffectNode { .sceneNode = effect_node.clone() });
    layer->AddEffect(effect);

    auto final_node = rstd::sync::Arc<owe::SceneNode>::make();
    final_node->AddMesh(MakeSingleSubmesh("final"));
    auto final_effect = Arc<owe::SceneImageEffect>::make();
    final_effect->AddNode(owe::SceneImageEffectNode { .sceneNode = final_node.clone() });
    layer->SetFinalResolveEffect(final_effect.clone());

    scene.RootMut()->AttachLayer(layer.clone());
    scene.RegisterCamera(String::make("effect"_str), rstd::move(camera));
    scene.RebuildResourceIndex();

    for (auto node : { prefill.as_ptr(), effect_node.as_ptr(), final_node.as_ptr() }) {
        auto node_id = scene.ResourceIndex().nodeId(*node);
        ASSERT_TRUE(node_id.is_some());
        EXPECT_TRUE(scene.ResourceIndex().drawItemFor(*node_id, rstd::u32()).is_some());
    }
}

TEST(SceneNodeLayer, FinalResolveTargetsFinalOutputBeforePublish) {
    owe::Scene scene;
    auto       layer = Arc<owe::SceneNodeLayer>::make(
        scene.RootMut().as_raw_ptr(), 1920.0f, 1080.0f, "_rt_composite"_str);
    layer->SetFinalTarget("_rt_final"_str);

    auto final_node = Arc<owe::SceneNode>::make();
    final_node->AddMesh(MakeSingleSubmesh("final"));
    auto final_effect = Arc<owe::SceneImageEffect>::make();
    final_effect->AddNode(owe::SceneImageEffectNode {
        .output    = owe::SceneEffectTarget::LayerNext(),
        .sceneNode = final_node.clone(),
    });
    layer->SetFinalResolveEffect(final_effect.clone());

    auto publish_node = Arc<owe::SceneNode>::make();
    publish_node->AddMesh(MakeSingleSubmesh("publish"));
    auto publish_effect = Arc<owe::SceneImageEffect>::make();
    publish_effect->AddNode(owe::SceneImageEffectNode {
        .output    = owe::SceneEffectTarget::Named("_rt_link"_str),
        .sceneNode = publish_node.clone(),
    });
    layer->SetPublishedEffect(publish_effect.clone());

    layer->ResolveEffect(*scene.DefaultEffectMesh(), "effect"_str);

    auto final_target = layer->ResolvedTarget(*final_effect->Nodes().last().unwrap().get());
    EXPECT_EQ(final_target.kind, owe::SceneEffectTargetKind::Named);
    EXPECT_EQ(final_target.key, "_rt_final"_str);

    auto publish_target = layer->ResolvedTarget(*publish_effect->Nodes().last().unwrap().get());
    EXPECT_EQ(publish_target.kind, owe::SceneEffectTargetKind::Named);
    EXPECT_EQ(publish_target.key, "_rt_link"_str);
}

TEST(SceneResourceIndex, PreservesTypedLayerPreviousSourceAcrossResolution) {
    owe::Scene         scene;
    owe::SceneMaterial material;
    material.textures.push("_rt_composite"_Str);
    auto layer = owe::SceneNodeId { .index = rstd::u32(4), .generation = rstd::u32(2) };

    ASSERT_TRUE(
        scene.SetMaterialLayerPreviousSource(material, rstd::u32(), layer, "_rt_composite"_str));
    scene.ResolveMaterialTextureSources(material);

    ASSERT_EQ(material.texture_sources.len(), rstd::usize(1));
    EXPECT_EQ(material.texture_sources[rstd::usize()].kind,
              owe::SceneMaterialTextureSourceKind::LayerPrevious);
    EXPECT_EQ(material.texture_sources[rstd::usize()].key, "_rt_composite"_str);
    ASSERT_TRUE(material.texture_sources[rstd::usize()].layer.is_some());
    EXPECT_EQ(*material.texture_sources[rstd::usize()].layer, layer);
}

TEST(SceneResourceIndex, RebuildPreservesNodeAndDrawIdsAfterLayerBindingChanges) {
    owe::Scene scene;
    auto       camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(1920, 1080, -1.0, 1.0));
    auto layer = Arc<owe::SceneNodeLayer>::make(
        scene.RootMut().as_raw_ptr(), 1920.0f, 1080.0f, "_rt_composite"_str);

    auto effect_node = rstd::sync::Arc<owe::SceneNode>::make();
    effect_node->AddMesh(MakeSingleSubmesh("effect"));
    auto effect = Arc<owe::SceneImageEffect>::make();
    effect->AddNode(owe::SceneImageEffectNode { .sceneNode = effect_node.clone() });
    layer->AddEffect(effect);
    scene.RootMut()->AttachLayer(layer.clone());
    scene.RegisterCamera(String::make("effect"_str), rstd::move(camera));

    auto source_node = rstd::sync::Arc<owe::SceneNode>::make();
    source_node->AddMesh(MakeSingleSubmesh("source"));
    scene.RootMut()->AppendChild(source_node.clone());

    auto sibling_node = rstd::sync::Arc<owe::SceneNode>::make();
    sibling_node->AddMesh(MakeSingleSubmesh("sibling"));
    scene.RootMut()->AppendChild(sibling_node.clone());

    scene.RebuildResourceIndex();
    auto sibling_id = scene.ResourceIndex().nodeId(*sibling_node.as_ptr());
    auto effect_id  = scene.ResourceIndex().nodeId(*effect_node.as_ptr());
    ASSERT_TRUE(sibling_id.is_some());
    ASSERT_TRUE(effect_id.is_some());
    auto sibling_draw = scene.ResourceIndex().drawItemFor(*sibling_id, rstd::u32());
    auto effect_draw  = scene.ResourceIndex().drawItemFor(*effect_id, rstd::u32());
    ASSERT_TRUE(sibling_draw.is_some());
    ASSERT_TRUE(effect_draw.is_some());

    source_node->SetCamera("effect"_str);
    scene.RebuildResourceIndex();

    auto rebuilt_sibling_id = scene.ResourceIndex().nodeId(*sibling_node.as_ptr());
    auto rebuilt_effect_id  = scene.ResourceIndex().nodeId(*effect_node.as_ptr());
    ASSERT_TRUE(rebuilt_sibling_id.is_some());
    ASSERT_TRUE(rebuilt_effect_id.is_some());
    EXPECT_EQ(rebuilt_sibling_id->index, sibling_id->index);
    EXPECT_EQ(rebuilt_effect_id->index, effect_id->index);

    auto rebuilt_sibling_draw = scene.ResourceIndex().drawItemFor(*rebuilt_sibling_id, rstd::u32());
    auto rebuilt_effect_draw  = scene.ResourceIndex().drawItemFor(*rebuilt_effect_id, rstd::u32());
    ASSERT_TRUE(rebuilt_sibling_draw.is_some());
    ASSERT_TRUE(rebuilt_effect_draw.is_some());
    EXPECT_EQ(rebuilt_sibling_draw->index, sibling_draw->index);
    EXPECT_EQ(rebuilt_effect_draw->index, effect_draw->index);
    EXPECT_EQ(scene.ResourceIndex().resolve(*sibling_draw)->node, sibling_node.as_ptr());
    EXPECT_EQ(scene.ResourceIndex().resolve(*effect_draw)->node, effect_node.as_ptr());
}

TEST(SceneResourceIndex, RebuildAppendsNewDrawsWithoutRenumberingExistingDraws) {
    owe::Scene scene;

    auto existing_node = rstd::sync::Arc<owe::SceneNode>::make();
    existing_node->AddMesh(MakeSingleSubmesh("existing"));
    scene.RootMut()->AppendChild(existing_node.clone());

    auto pending_node = rstd::sync::Arc<owe::SceneNode>::make();
    auto pending_mesh = MakeSingleSubmesh("pending");
    pending_mesh->Submeshes().clear();
    pending_node->AddMesh(pending_mesh.clone());
    scene.RootMut()->AppendChild(pending_node.clone());

    scene.RebuildResourceIndex();
    auto existing_node_id = scene.ResourceIndex().nodeId(*existing_node.as_ptr());
    auto pending_node_id  = scene.ResourceIndex().nodeId(*pending_node.as_ptr());
    ASSERT_TRUE(existing_node_id.is_some());
    ASSERT_TRUE(pending_node_id.is_some());
    auto existing_draw = scene.ResourceIndex().drawItemFor(*existing_node_id, rstd::u32());
    ASSERT_TRUE(existing_draw.is_some());
    EXPECT_TRUE(scene.ResourceIndex().drawItemFor(*pending_node_id, rstd::u32()).is_none());
    auto existing_mesh_id = scene.ResourceIndex().meshId(*existing_node->Mesh());
    auto existing_material_id =
        scene.ResourceIndex().materialId(*existing_node->Mesh()->MaterialSlots()[usize()]);
    ASSERT_TRUE(existing_mesh_id.is_some());
    ASSERT_TRUE(existing_material_id.is_some());

    auto resolved_mesh = MakeSingleSubmesh("resolved");
    pending_mesh->ChangeMeshDataFrom(*resolved_mesh);
    scene.RebuildResourceIndex();

    auto rebuilt_existing_draw = scene.ResourceIndex().drawItemFor(*existing_node_id, rstd::u32());
    auto appended_draw         = scene.ResourceIndex().drawItemFor(*pending_node_id, rstd::u32());
    ASSERT_TRUE(rebuilt_existing_draw.is_some());
    ASSERT_TRUE(appended_draw.is_some());
    EXPECT_EQ(rebuilt_existing_draw->index, existing_draw->index);
    EXPECT_GT(appended_draw->index, existing_draw->index);
    auto rebuilt_existing_mesh_id = scene.ResourceIndex().meshId(*existing_node->Mesh());
    auto rebuilt_existing_material_id =
        scene.ResourceIndex().materialId(*existing_node->Mesh()->MaterialSlots()[usize()]);
    ASSERT_TRUE(rebuilt_existing_mesh_id.is_some());
    ASSERT_TRUE(rebuilt_existing_material_id.is_some());
    EXPECT_EQ(rebuilt_existing_mesh_id->index, existing_mesh_id->index);
    EXPECT_EQ(rebuilt_existing_material_id->index, existing_material_id->index);
    EXPECT_EQ(scene.ResourceIndex().resolve(*existing_draw)->node, existing_node.as_ptr());
    EXPECT_EQ(scene.ResourceIndex().resolve(*appended_draw)->node, pending_node.as_ptr());
}

TEST(SceneResourceIndex, RebuildInvalidatesRemovedNodesWithoutRenumberingRemainingDraws) {
    owe::Scene scene;

    auto removed_node = rstd::sync::Arc<owe::SceneNode>::make();
    removed_node->AddMesh(MakeSingleSubmesh("removed"));
    scene.RootMut()->AppendChild(removed_node.clone());

    auto remaining_node = rstd::sync::Arc<owe::SceneNode>::make();
    remaining_node->AddMesh(MakeSingleSubmesh("remaining"));
    scene.RootMut()->AppendChild(remaining_node.clone());

    scene.RebuildResourceIndex();
    auto removed_node_id   = scene.ResourceIndex().nodeId(*removed_node.as_ptr());
    auto remaining_node_id = scene.ResourceIndex().nodeId(*remaining_node.as_ptr());
    ASSERT_TRUE(removed_node_id.is_some());
    ASSERT_TRUE(remaining_node_id.is_some());
    auto removed_draw   = scene.ResourceIndex().drawItemFor(*removed_node_id, rstd::u32());
    auto remaining_draw = scene.ResourceIndex().drawItemFor(*remaining_node_id, rstd::u32());
    ASSERT_TRUE(removed_draw.is_some());
    ASSERT_TRUE(remaining_draw.is_some());
    auto removed_mesh_id = scene.ResourceIndex().meshId(*removed_node->Mesh());
    auto removed_material_id =
        scene.ResourceIndex().materialId(*removed_node->Mesh()->MaterialSlots()[usize()]);
    auto remaining_mesh_id = scene.ResourceIndex().meshId(*remaining_node->Mesh());
    auto remaining_material_id =
        scene.ResourceIndex().materialId(*remaining_node->Mesh()->MaterialSlots()[usize()]);
    ASSERT_TRUE(removed_mesh_id.is_some());
    ASSERT_TRUE(removed_material_id.is_some());
    ASSERT_TRUE(remaining_mesh_id.is_some());
    ASSERT_TRUE(remaining_material_id.is_some());

    scene.RootMut()->ClearChildren();
    scene.RootMut()->AppendChild(remaining_node.clone());
    scene.RebuildResourceIndex();

    EXPECT_TRUE(scene.ResourceIndex().nodeId(*removed_node.as_ptr()).is_none());
    EXPECT_EQ(scene.ResourceIndex().node(*removed_node_id), nullptr);
    EXPECT_TRUE(scene.ResourceIndex().resolve(*removed_draw).is_none());
    EXPECT_EQ(scene.ResourceIndex().mesh(*removed_mesh_id), nullptr);
    EXPECT_EQ(scene.ResourceIndex().material(*removed_material_id), nullptr);

    auto rebuilt_remaining_node_id = scene.ResourceIndex().nodeId(*remaining_node.as_ptr());
    ASSERT_TRUE(rebuilt_remaining_node_id.is_some());
    EXPECT_EQ(rebuilt_remaining_node_id->index, remaining_node_id->index);
    auto rebuilt_remaining_draw =
        scene.ResourceIndex().drawItemFor(*rebuilt_remaining_node_id, rstd::u32());
    ASSERT_TRUE(rebuilt_remaining_draw.is_some());
    EXPECT_EQ(rebuilt_remaining_draw->index, remaining_draw->index);
    auto rebuilt_remaining_mesh_id = scene.ResourceIndex().meshId(*remaining_node->Mesh());
    auto rebuilt_remaining_material_id =
        scene.ResourceIndex().materialId(*remaining_node->Mesh()->MaterialSlots()[usize()]);
    ASSERT_TRUE(rebuilt_remaining_mesh_id.is_some());
    ASSERT_TRUE(rebuilt_remaining_material_id.is_some());
    EXPECT_EQ(rebuilt_remaining_mesh_id->index, remaining_mesh_id->index);
    EXPECT_EQ(rebuilt_remaining_material_id->index, remaining_material_id->index);
    EXPECT_EQ(scene.ResourceIndex().resolve(*remaining_draw)->node, remaining_node.as_ptr());
}

TEST(SceneTextureAnimation, AdvancesOncePerRuntimeFrame) {
    owe::Scene scene;
    auto       node = rstd::sync::Arc<owe::SceneNode>::make();
    auto       mesh = MakeSingleSubmesh("sprite-a");
    mesh->MaterialSlots()[usize()]->textures.push("tex/sprite"_Str);
    node->AddMesh(mesh.clone());
    scene.RootMut()->AppendChild(node.clone());
    auto second_node = rstd::sync::Arc<owe::SceneNode>::make();
    auto second_mesh = MakeSingleSubmesh("sprite-b");
    second_mesh->MaterialSlots()[usize()]->textures.push("tex/sprite"_Str);
    second_node->AddMesh(second_mesh.clone());
    scene.RootMut()->AppendChild(second_node.clone());

    owe::SceneTexture texture { .url = "tex/sprite"_Str, .isSprite = true };
    texture.spriteAnim.AppendFrame(owe::SpriteFrame { .imageId = 0, .frametime = 0.1f, .x = 0.0f });
    texture.spriteAnim.AppendFrame(owe::SpriteFrame { .imageId = 1, .frametime = 0.1f, .x = 0.5f });
    scene.RegisterTexture(String::make("tex/sprite"_str), rstd::move(texture));
    scene.RebuildResourceIndex();

    auto node_id = scene.ResourceIndex().nodeId(*node.as_ptr());
    ASSERT_TRUE(node_id.is_some());
    auto draw_id = scene.ResourceIndex().drawItemFor(*node_id, rstd::u32());
    ASSERT_TRUE(draw_id.is_some());
    auto second_node_id = scene.ResourceIndex().nodeId(*second_node.as_ptr());
    ASSERT_TRUE(second_node_id.is_some());
    auto second_draw_id = scene.ResourceIndex().drawItemFor(*second_node_id, rstd::u32());
    ASSERT_TRUE(second_draw_id.is_some());

    auto initial = scene.TextureFrame(*draw_id, rstd::usize());
    ASSERT_TRUE(initial.is_some());
    EXPECT_FLOAT_EQ(initial->translation[rstd::usize()], 0.0f);
    EXPECT_EQ(initial->image_slot, rstd::usize());

    scene.Runtime().Advance(rstd::f64(0.01));
    auto first_query  = scene.TextureFrame(*draw_id, rstd::usize());
    auto second_query = scene.TextureFrame(*draw_id, rstd::usize());
    auto shared_query = scene.TextureFrame(*second_draw_id, rstd::usize());
    ASSERT_TRUE(first_query.is_some());
    ASSERT_TRUE(second_query.is_some());
    ASSERT_TRUE(shared_query.is_some());
    EXPECT_FLOAT_EQ(first_query->translation[rstd::usize()], 0.5f);
    EXPECT_EQ(first_query->image_slot, rstd::usize(1));
    EXPECT_EQ(first_query->translation, second_query->translation);
    EXPECT_EQ(first_query->translation, shared_query->translation);
    EXPECT_EQ(first_query->revision, shared_query->revision);

    scene.RebuildResourceIndex();
    auto rebuilt_node_id = scene.ResourceIndex().nodeId(*node.as_ptr());
    ASSERT_TRUE(rebuilt_node_id.is_some());
    auto rebuilt_draw_id = scene.ResourceIndex().drawItemFor(*rebuilt_node_id, rstd::u32());
    ASSERT_TRUE(rebuilt_draw_id.is_some());
    auto rebuilt_second_node_id = scene.ResourceIndex().nodeId(*second_node.as_ptr());
    ASSERT_TRUE(rebuilt_second_node_id.is_some());
    auto rebuilt_second_draw_id =
        scene.ResourceIndex().drawItemFor(*rebuilt_second_node_id, rstd::u32());
    ASSERT_TRUE(rebuilt_second_draw_id.is_some());
    auto rebuilt = scene.TextureFrame(*rebuilt_draw_id, rstd::usize());
    ASSERT_TRUE(rebuilt.is_some());
    EXPECT_FLOAT_EQ(rebuilt->translation[rstd::usize()], 0.5f);
    EXPECT_EQ(rebuilt->revision, first_query->revision);

    node->TexAnim().playing = false;
    scene.Runtime().Advance(rstd::f64(0.11));
    auto paused  = scene.TextureFrame(*rebuilt_draw_id, rstd::usize());
    auto playing = scene.TextureFrame(*rebuilt_second_draw_id, rstd::usize());
    ASSERT_TRUE(paused.is_some());
    ASSERT_TRUE(playing.is_some());
    EXPECT_FLOAT_EQ(paused->translation[rstd::usize()], 0.5f);
    EXPECT_FLOAT_EQ(playing->translation[rstd::usize()], 0.0f);
}

TEST(SceneTextures, EnsureTextureDescriptorRegistersImportedTexture) {
    owe::Scene scene;
    EXPECT_FALSE(scene.EnsureTextureDescriptor("tex/runtime"_str));

    scene.SetImageParser(rstd::sync::Arc<dyn<owe::IImageParser>>::make(FakeImageParser {}));
    EXPECT_TRUE(scene.EnsureTextureDescriptor("tex/runtime"_str));
    auto texture = scene.Texture("tex/runtime"_str);
    ASSERT_TRUE(texture.is_some());
    EXPECT_EQ((**texture).url, "tex/runtime"_str);
    EXPECT_TRUE(scene.EnsureTextureDescriptor("_rt_default"_str));
    EXPECT_TRUE(scene.Texture("_rt_default"_str).is_none());
}

TEST(SceneTextures, RegisteredReplacementKeepsOneName) {
    owe::Scene scene;
    scene.RegisterTexture(String::make("slot"_str), owe::SceneTexture { .url = "first"_Str });
    scene.RegisterTexture(String::make("slot"_str), owe::SceneTexture { .url = "second"_Str });

    auto texture = scene.Texture("slot"_str);
    ASSERT_TRUE(texture.is_some());
    EXPECT_EQ((**texture).url, "second"_str);
    EXPECT_EQ(scene.TextureNames().len(), usize(1));
}

TEST(SceneTextures, SnapshotSeparatesLocatorGenerationFromContentRevision) {
    owe::Scene scene;
    scene.RegisterTexture(String::make("video"_str),
                          owe::SceneTexture {
                              .url     = "video-a"_Str,
                              .isVideo = true,
                          });
    auto first_control = scene.VideoControl("video"_str);
    ASSERT_TRUE(first_control.is_some());

    auto first  = owe::ExtractRenderSceneSnapshot(scene);
    auto second = owe::ExtractRenderSceneSnapshot(scene);
    ASSERT_EQ(first.TextureDescs().len(), usize(1));
    ASSERT_EQ(second.TextureDescs().len(), usize(1));
    EXPECT_NE(first.TextureDescs()[usize()].id.generation,
              second.TextureDescs()[usize()].id.generation);
    EXPECT_EQ(first.TextureDescs()[usize()].content_revision, u64(1));
    EXPECT_EQ(second.TextureDescs()[usize()].content_revision, u64(1));
    ASSERT_TRUE(second.TextureDescs()[usize()].video_control.is_some());
    EXPECT_TRUE(Arc<owe::VideoPlaybackState>::ptr_eq(
        *first_control, *second.TextureDescs()[usize()].video_control));

    scene.RegisterTexture(String::make("video"_str),
                          owe::SceneTexture {
                              .url     = "video-b"_Str,
                              .isVideo = true,
                          });
    auto replaced = owe::ExtractRenderSceneSnapshot(scene);
    ASSERT_EQ(replaced.TextureDescs().len(), usize(1));
    EXPECT_EQ(replaced.TextureDescs()[usize()].content_revision, u64(2));
    ASSERT_TRUE(replaced.TextureDescs()[usize()].video_control.is_some());
    EXPECT_FALSE(Arc<owe::VideoPlaybackState>::ptr_eq(
        *first_control, *replaced.TextureDescs()[usize()].video_control));
}

TEST(SceneTextures, RuntimeImageOverridesParserContent) {
    owe::Scene scene;
    scene.SetImageParser(rstd::sync::Arc<dyn<owe::IImageParser>>::make(FakeImageParser {}));
    auto image           = Arc<owe::Image>::make();
    image->header.width  = 128;
    image->header.height = 64;
    scene.RegisterRuntimeImage(String::make("runtime/atlas"_str), image.clone());

    auto parsed = scene.ParseImage("runtime/atlas"_str);
    ASSERT_TRUE(parsed.is_ok());
    EXPECT_TRUE(Arc<owe::Image>::ptr_eq(*parsed, image));

    auto header = scene.ParseImageHeader("runtime/atlas"_str);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header->width, 128);
    EXPECT_EQ(header->height, 64);
}

TEST(SceneUserPropertyDiagnostics, StoresAndClearsByKey) {
    owe::Scene scene;
    scene.AddUserPropertyDiagnostic(owe::SceneUserPropertyDiagnostic {
        .key      = String::make("combo_a"_str),
        .code     = owe::SceneUserPropertyDiagnosticCode::UnsupportedShaderComboValue,
        .material = String::make("mat_a"_str),
        .combo    = String::make("USE_A"_str),
        .message  = String::make("bad value"_str),
    });
    scene.AddUserPropertyDiagnostic(owe::SceneUserPropertyDiagnostic {
        .key      = String::make("combo_b"_str),
        .code     = owe::SceneUserPropertyDiagnosticCode::ShaderComboCompileFailed,
        .material = String::make("mat_b"_str),
        .combo    = String::make("USE_B"_str),
        .message  = String::make("compile failed"_str),
    });

    auto diagnostics = scene.UserPropertyDiagnostics();
    ASSERT_EQ(diagnostics.len(), usize(2));
    EXPECT_EQ(diagnostics[usize()].key, "combo_a"_str);
    EXPECT_EQ(diagnostics[usize()].code,
              owe::SceneUserPropertyDiagnosticCode::UnsupportedShaderComboValue);

    scene.ClearUserPropertyDiagnostics("combo_a"_str);
    diagnostics = scene.UserPropertyDiagnostics();
    ASSERT_EQ(diagnostics.len(), usize(1));
    EXPECT_EQ(diagnostics[usize()].key, "combo_b"_str);

    scene.ClearUserPropertyDiagnostics(""_str);
    EXPECT_TRUE(scene.UserPropertyDiagnostics().is_empty());
}

TEST(SceneMaterialRuntimeMutation, UpdatesShaderValuesAndTextureSlotsThroughSceneOwner) {
    owe::Scene scene;
    scene.RootMut()->ID() = rstd::i32(1);
    scene.SetImageParser(rstd::sync::Arc<dyn<owe::IImageParser>>::make(FakeImageParser {}));

    auto node  = rstd::sync::Arc<owe::SceneNode>::make();
    node->ID() = rstd::i32(2);
    auto mesh  = MakeSingleSubmesh("material");
    node->AddMesh(mesh.clone());
    scene.RootMut()->AppendChild(node.clone());

    auto* material = mesh->MaterialSlots()[usize()].as_ptr().as_raw_ptr();
    ASSERT_NE(material, nullptr);

    auto shader = Arc<owe::SceneShader>::make();
    (void)shader->default_uniforms.insert(
        "u_Color"_Str,
        owe::ShaderValue(owe::ShaderValue(std::array<float, 4> { 0.0f, 0.0f, 0.0f, 0.0f })));
    material->customShader.shader = Some(rstd::move(shader));

    const auto value_version = material->customShader.value_version;
    EXPECT_TRUE(scene.SetMaterialShaderValue(*material, "u_Color"_str, owe::ShaderValue(0.5f)));
    auto color_it = material->customShader.constValues.get("u_Color"_str);
    ASSERT_TRUE(color_it.is_some());
    ASSERT_EQ((**color_it).size(), rstd::usize(4));
    EXPECT_FLOAT_EQ((**color_it)[rstd::usize()], 0.5f);
    EXPECT_FLOAT_EQ((**color_it)[rstd::usize(3)], 0.5f);
    EXPECT_GT(material->customShader.value_version, value_version);

    const auto updated_version = material->customShader.value_version;
    EXPECT_FALSE(scene.SetMaterialShaderValue(*material, ""_str, owe::ShaderValue(1.0f)));
    EXPECT_EQ(material->customShader.value_version, updated_version);

    auto mutation = scene.SetMaterialTextureSlot(*material, rstd::u32(), "tex/runtime"_str);
    EXPECT_TRUE(mutation.changed);
    ASSERT_TRUE(mutation.material.is_some());
    ASSERT_TRUE(scene.Texture("tex/runtime"_str).is_some());
    EXPECT_EQ(material->textures[rstd::usize(0)].as_str(), "tex/runtime"_str);

    auto unchanged = scene.SetMaterialTextureSlot(*material, rstd::u32(), "tex/runtime"_str);
    EXPECT_FALSE(unchanged.changed);
    EXPECT_TRUE(unchanged.material.is_none());

    auto spec = scene.SetMaterialTextureSlot(*material, rstd::u32(1), "_rt_default"_str);
    EXPECT_TRUE(spec.changed);
    EXPECT_TRUE(scene.Texture("_rt_default"_str).is_none());
    ASSERT_GE(material->textures.len().to_primitive(), 2u);
    EXPECT_EQ(material->textures[rstd::usize(1)].as_str(), "_rt_default"_str);
    EXPECT_EQ(material->DirtyFlags(), owe::SceneMaterialDirtyTextureBindings);
    auto texture_events = scene.ConsumePreparedMaterialDirtyEvents();
    ASSERT_EQ(texture_events.len(), usize(1));
    EXPECT_EQ(texture_events[usize()].flags, owe::SceneMaterialDirtyTextureBindings);
}

TEST(SceneMaterialRuntimeMutation, ZeroFillsShortShaderVectorsToDeclaredShape) {
    owe::SceneMaterial material;
    auto               shader = Arc<owe::SceneShader>::make();
    (void)shader->default_uniforms.insert(
        "g_CloudSpeeds"_Str,
        owe::ShaderValue(owe::ShaderValue(rstd::array<float, 4> { 0.01f, 0.01f, -0.02f, -0.02f })));
    material.customShader.shader = Some(rstd::move(shader));

    ASSERT_TRUE(material.SetShaderValue("g_CloudSpeeds"_str,
                                        owe::ShaderValue(rstd::array<float, 2> { 0.01f, -0.02f })));

    const auto& value = (**material.customShader.constValues.get("g_CloudSpeeds"_str));
    ASSERT_EQ(value.size(), rstd::usize(4));
    EXPECT_FLOAT_EQ(value[rstd::usize(0)], 0.01f);
    EXPECT_FLOAT_EQ(value[rstd::usize(1)], -0.02f);
    EXPECT_FLOAT_EQ(value[rstd::usize(2)], 0.0f);
    EXPECT_FLOAT_EQ(value[rstd::usize(3)], 0.0f);
}

TEST(SceneMaterialShaderVariant, CarriesCompileDescriptorThroughMaterialMove) {
    owe::SceneMaterial material;
    material.name = "variant"_Str;

    owe::SceneShaderVariantDesc variant;
    variant.scene_id    = "scene"_Str;
    variant.shader_name = "genericimage"_Str;
    (void)variant.input_combos.insert("BLENDMODE"_Str, "1"_Str);
    (void)variant.resolved_combos.insert("BLENDMODE"_Str, "1"_Str);
    (void)variant.resolved_combos.insert("TEX0FORMAT"_Str, "FORMAT_R8"_Str);
    (void)variant.uniform_aliases.insert("brightness"_Str, "u_Brightness"_Str);
    variant.default_textures.push(
        owe::SceneShaderDefaultTexture { .slot = i32(), .texture = "tex/default"_Str });
    variant.sampler_bindings.push(
        owe::SceneSamplerBinding { .texture_slot = 0, .shader_member = "u_Albedo"_Str });
    variant.texture_infos.push(owe::SceneShaderTextureCompileInfo {
        .enabled    = true,
        .components = { true, false, true, false },
    });
    variant.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/genericimage.vert"_Str,
        .source     = "vertex source"_Str,
    });
    material.customShader.variant = Some(rstd::move(variant));

    auto mesh = Arc<owe::SceneMesh>::make();
    mesh->AddMaterial(std::move(material));
    auto* moved = mesh->MaterialSlots()[usize()].as_ptr().as_raw_ptr();
    ASSERT_NE(moved, nullptr);
    ASSERT_TRUE(moved->customShader.variant.is_some());

    const auto& stored = *moved->customShader.variant;
    EXPECT_TRUE(stored.Valid());
    EXPECT_EQ(stored.scene_id, "scene"_str);
    EXPECT_EQ(stored.shader_name, "genericimage"_str);
    ASSERT_TRUE(stored.resolved_combos.contains_key("TEX0FORMAT"_str));
    EXPECT_EQ((**stored.resolved_combos.get("TEX0FORMAT"_str)).as_str(), "FORMAT_R8"_str);
    ASSERT_EQ(stored.default_textures.len().to_primitive(), 1u);
    EXPECT_EQ(stored.default_textures[usize(0)].texture, "tex/default"_str);
    ASSERT_EQ(stored.sampler_bindings.len().to_primitive(), 1u);
    EXPECT_EQ(stored.sampler_bindings[usize()].shader_member, "u_Albedo"_str);
    ASSERT_EQ(stored.texture_infos.len().to_primitive(), 1u);
    EXPECT_TRUE(stored.texture_infos[usize(0)].components[rstd::usize(2)]);
    ASSERT_EQ(stored.stages.len().to_primitive(), 1u);
    EXPECT_EQ(stored.stages[usize(0)].source_key, "/assets/shaders/genericimage.vert"_str);
}

TEST(SceneMaterial, PreservesOwnedStateAcrossCopyAndMove) {
    owe::SceneMaterial material;
    material.textures.clear();
    material.textures.push("masks/padded"_Str);
    material.texture_metadata.push(owe::SceneMaterialTextureMetadata {
        .has_extent    = true,
        .source_extent = { 1024.0f, 1024.0f },
        .sample_extent = { 960.0f, 540.0f },
    });
    auto curve = Arc<owe::SceneAnimationCurve>::make();
    curve->c0.push({ .frame = i32(), .value = 0.25f });
    auto clip = Arc<owe::SceneAnimationClip>::make(
        owe::SceneAnimationClipSpec { .name = String::make("shared"_str) });
    auto playback = Arc<owe::SceneAnimationPlayback>::make(rstd::move(clip));
    auto track    = Arc<owe::SceneAnimationTrack>::make(
        owe::SceneAnimationTrack { .curve = curve.clone(), .playback = playback.clone() });
    (void)material.customShader.valueAnimations.insert(String::make("u_Alpha"_str),
                                                       owe::SceneShaderValueAnimation {
                                                           .base  = owe::ShaderValue(1.0f),
                                                           .track = Some(track.clone()),
                                                       });
    (void)material.customShader.valueAnimations.insert(
        String::make("u_Color"_str),
        owe::SceneShaderValueAnimation {
            .base  = owe::ShaderValue(0.5f),
            .track = Some(Arc<owe::SceneAnimationTrack>::make(track->Share())),
        });

    owe::SceneMaterial copied = material;
    ASSERT_EQ(copied.texture_metadata.len().to_primitive(), 1u);
    EXPECT_TRUE(copied.texture_metadata[rstd::usize(0)].has_extent);
    EXPECT_EQ(copied.texture_metadata[rstd::usize(0)].source_extent,
              (rstd::array<float, 2> { 1024.0f, 1024.0f }));
    EXPECT_EQ(copied.texture_metadata[rstd::usize(0)].sample_extent,
              (rstd::array<float, 2> { 960.0f, 540.0f }));
    auto original_animation = material.customShader.valueAnimations.get("u_Alpha"_str);
    auto copied_animation   = copied.customShader.valueAnimations.get("u_Alpha"_str);
    auto copied_peer        = copied.customShader.valueAnimations.get("u_Color"_str);
    ASSERT_TRUE(original_animation.is_some());
    ASSERT_TRUE(copied_animation.is_some());
    ASSERT_TRUE(copied_peer.is_some());
    ASSERT_TRUE((**original_animation).track.is_some());
    ASSERT_TRUE((**copied_animation).track.is_some());
    ASSERT_TRUE((**copied_peer).track.is_some());
    EXPECT_EQ((**(**original_animation).track).curve.as_ptr().as_raw_ptr(),
              (**(**copied_animation).track).curve.as_ptr().as_raw_ptr());
    EXPECT_NE((**(**original_animation).track).playback.as_ptr().as_raw_ptr(),
              (**(**copied_animation).track).playback.as_ptr().as_raw_ptr());
    EXPECT_EQ((**(**copied_animation).track).playback.as_ptr().as_raw_ptr(),
              (**(**copied_peer).track).playback.as_ptr().as_raw_ptr());
    owe::SceneNode copied_owner;
    copied.RegisterAnimations(copied_owner);
    auto copied_named = copied_owner.NamedAnimation("shared"_str);
    ASSERT_TRUE(copied_named.is_some());
    EXPECT_EQ((*copied_named).as_ptr().as_raw_ptr(),
              (**(**copied_animation).track).playback.as_ptr().as_raw_ptr());

    auto mesh = Arc<owe::SceneMesh>::make();
    mesh->AddMaterial(std::move(material));
    const auto* moved = mesh->MaterialSlots()[usize()].as_ptr().as_raw_ptr();
    ASSERT_NE(moved, nullptr);
    ASSERT_EQ(moved->texture_metadata.len().to_primitive(), 1u);
    EXPECT_EQ(moved->texture_metadata[rstd::usize(0)].sample_extent,
              (rstd::array<float, 2> { 960.0f, 540.0f }));
    auto moved_animation = moved->customShader.valueAnimations.get("u_Alpha"_str);
    ASSERT_TRUE(moved_animation.is_some());
    ASSERT_TRUE((**moved_animation).track.is_some());
    EXPECT_EQ((*(**moved_animation).track).as_ptr().as_raw_ptr(), track.as_ptr().as_raw_ptr());
}

TEST(SceneMaterialShaderVariant, ClonesInterfaceDescriptionsIndependently) {
    owe::SceneMaterial          original;
    owe::SceneShaderVariantDesc variant;
    variant.shader_name = "clone-test"_Str;
    variant.scene_id    = "owned-scene"_Str;
    (void)variant.uniform_aliases.insert("brightness"_Str, "u_Brightness"_Str);
    variant.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/owned/shader.frag"_Str,
        .source     = "source"_Str,
    });
    (void)variant.stages[usize()].uniforms.insert("u_Color"_Str, "float4"_Str);
    variant.default_textures.push({ .slot = i32(3), .texture = "owned/default-texture"_Str });
    variant.texture_infos.push({ .enabled = true });
    variant.sampler_bindings.push({ .texture_slot = 3, .shader_member = "u_SourceImage"_Str });
    variant.uniform_blocks.push(
        { .name = "uniform_block_with_owned_name"_Str, .identity = u64(41) });
    owe::SceneShaderDescriptorSetInterface set { .set = u32(2), .identity = u64(42) };
    set.bindings.push({ .name = "descriptor_binding_with_owned_name"_Str, .binding = u32(3) });
    variant.descriptor_sets.push(rstd::move(set));
    original.customShader.variant = Some(rstd::move(variant));
    auto  copied                  = original;
    auto& source                  = *original.customShader.variant;
    auto& target                  = *copied.customShader.variant;
    EXPECT_EQ(owe::ClassifySceneShaderVariantMutation(source, target), owe::SceneMaterialDirtyNone);
    source.stages[usize()].source.push_str(" changed"_str);
    EXPECT_EQ(owe::ClassifySceneShaderVariantMutation(source, target),
              owe::SceneMaterialDirtyPipeline);
    (void)source.stages[usize()].uniforms.insert("u_Color"_Str, "float2"_Str);
    ASSERT_TRUE(target.stages[usize()].uniforms.contains_key("u_Color"_str));
    EXPECT_EQ((**target.stages[usize()].uniforms.get("u_Color"_str)).as_str(), "float4"_str);
    source.stages[usize()].source_key.clear();
    source.default_textures[usize()].texture.clear();
    source.texture_infos[usize()].enabled = false;
    source.scene_id.clear();
    source.shader_name.clear();
    (void)source.uniform_aliases.insert("brightness"_Str, "changed"_Str);
    ASSERT_TRUE(target.uniform_aliases.contains_key("brightness"_str));
    EXPECT_EQ((**target.uniform_aliases.get("brightness"_str)).as_str(), "u_Brightness"_str);
    EXPECT_EQ(target.scene_id.as_str(), "owned-scene"_str);
    EXPECT_EQ(target.shader_name.as_str(), "clone-test"_str);
    EXPECT_EQ(target.stages[usize()].source.as_str(), "source"_str);
    EXPECT_EQ(target.stages[usize()].source_key.as_str(), "/owned/shader.frag"_str);
    EXPECT_EQ(target.default_textures[usize()].texture.as_str(), "owned/default-texture"_str);
    EXPECT_TRUE(target.texture_infos[usize()].enabled);
    source.sampler_bindings[usize()].shader_member = "changed"_Str;
    source.uniform_blocks[usize()].name.clear();
    source.descriptor_sets[usize()].bindings[usize()].name.clear();
    EXPECT_EQ(target.sampler_bindings[usize()].shader_member.as_str(), "u_SourceImage"_str);
    EXPECT_EQ(target.uniform_blocks[usize()].name.as_str(), "uniform_block_with_owned_name"_str);
    EXPECT_EQ(target.descriptor_sets[usize()].bindings[usize()].name.as_str(),
              "descriptor_binding_with_owned_name"_str);
    EXPECT_EQ(target.uniform_blocks[usize()].identity, u64(41));
    EXPECT_EQ(target.descriptor_sets[usize()].identity, u64(42));
    auto cloned = target.clone();
    cloned.clone_from(cloned);
    EXPECT_EQ(cloned.descriptor_sets, target.descriptor_sets);
}

TEST(SceneShader, ResolvesLoaderDefinedSamplerMember) {
    owe::SceneShader shader;
    shader.sampler_bindings.push(
        owe::SceneSamplerBinding { .texture_slot = 3, .shader_member = "u_SourceImage"_Str });

    EXPECT_EQ(shader.SamplerMember(3), "u_SourceImage"_str);
    EXPECT_TRUE(shader.SamplerMember(0).is_empty());
}

TEST(SceneMaterialShaderVariant, AppliesCompiledVariantThroughSceneOwner) {
    owe::Scene scene;
    scene.RootMut()->ID() = rstd::i32(1);

    auto node  = rstd::sync::Arc<owe::SceneNode>::make();
    node->ID() = rstd::i32(2);
    auto mesh  = MakeSingleSubmesh("variant");
    node->AddMesh(mesh.clone());
    scene.RootMut()->AppendChild(node.clone());

    auto* material = mesh->MaterialSlots()[usize()].as_ptr().as_raw_ptr();
    ASSERT_NE(material, nullptr);

    EXPECT_FALSE(scene.SetMaterialShaderVariant(*material, {}).changed);
    EXPECT_EQ(material->DirtyFlags(), owe::SceneMaterialDirtyNone);

    auto shader  = Arc<owe::SceneShader>::make();
    shader->name = "variant-next"_Str;
    shader->codes.push(
        owe::ShaderCode::from(rstd::array<rstd::uint32_t, 3> { 1u, 2u, 3u }.as_slice()));

    owe::SceneShaderVariantDesc variant;
    variant.scene_id    = "scene"_Str;
    variant.shader_name = "variant-next"_Str;
    (void)variant.resolved_combos.insert("USE_COLOR"_Str, "1"_Str);
    variant.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::VERTEX,
        .source_key = "/assets/shaders/variant-next.vert"_Str,
        .source     = "source"_Str,
    });

    auto mutation = scene.SetMaterialShaderVariant(*material,
                                                   owe::SceneShaderVariantMutation {
                                                       .shader  = Some(shader.clone()),
                                                       .variant = variant.clone(),
                                                   });

    EXPECT_TRUE(mutation.changed);
    ASSERT_TRUE(mutation.material.is_some());
    ASSERT_TRUE(material->customShader.shader);
    EXPECT_EQ((*material->customShader.shader).as_ptr().as_raw_ptr(), shader.as_ptr().as_raw_ptr());
    ASSERT_TRUE(material->customShader.variant.is_some());
    ASSERT_TRUE(material->customShader.variant->resolved_combos.contains_key("USE_COLOR"_str));
    EXPECT_EQ((**material->customShader.variant->resolved_combos.get("USE_COLOR"_str)).as_str(),
              "1"_str);
    EXPECT_EQ(material->DirtyFlags(), owe::SceneMaterialDirtyGraph);

    auto events = scene.ConsumePreparedMaterialDirtyEvents();
    ASSERT_EQ(events.len(), usize(1));
    EXPECT_EQ(events[usize()].material.index, mutation.material->index);
    EXPECT_EQ(events[usize()].flags, owe::SceneMaterialDirtyGraph);
    EXPECT_EQ(material->DirtyFlags(), owe::SceneMaterialDirtyNone);
}

TEST(SceneMaterialShaderVariant, ClassifiesVariantImpactAndAppliesActiveTextureSlots) {
    owe::Scene scene;
    scene.RootMut()->ID() = rstd::i32(1);

    auto node  = rstd::sync::Arc<owe::SceneNode>::make();
    node->ID() = rstd::i32(2);
    auto mesh  = MakeSingleSubmesh("variant");
    node->AddMesh(mesh.clone());
    scene.RootMut()->AppendChild(node.clone());

    auto* material = mesh->MaterialSlots()[usize()].as_ptr().as_raw_ptr();
    ASSERT_NE(material, nullptr);
    material->textures.clear();
    material->textures.push("tex/a"_Str);
    material->textures.push(""_Str);
    material->customShader.shader = Some(Arc<owe::SceneShader>::make());

    owe::SceneShaderVariantDesc current;
    current.scene_id    = "scene"_Str;
    current.shader_name = "variant"_Str;
    current.texture_slots.clear();
    current.texture_slots.push("tex/a"_Str);
    current.texture_slots.push("tex/b"_Str);
    (void)current.resolved_combos.insert("USE_B"_Str, "0"_Str);
    current.descriptor_layout_hash = usize(1000);
    current.stages.push(owe::SceneShaderVariantStage {
        .stage      = owe::ShaderType::FRAGMENT,
        .source_key = "/assets/shaders/variant.frag"_Str,
        .source     = "source"_Str,
        .code_hash  = rstd::usize(100),
    });
    (void)current.stages[usize()].active_texture_slots.insert(u32());
    (void)current.stages[usize()].uniforms.insert("u_Color"_Str, "float4"_Str);
    material->customShader.variant = Some<owe::SceneShaderVariantDesc>(current.clone());

    auto hash_only                       = current.clone();
    hash_only.stages[usize(0)].code_hash = rstd::usize(101);
    auto hash_shader                     = Arc<owe::SceneShader>::make();
    hash_shader->name                    = "variant"_Str;
    hash_shader->codes.push(
        owe::ShaderCode::from(rstd::array<rstd::uint32_t, 1> { 101u }.as_slice()));
    auto hash_rt = scene.SetMaterialShaderVariant(*material,
                                                  owe::SceneShaderVariantMutation {
                                                      .shader  = Some(hash_shader.clone()),
                                                      .variant = hash_only.clone(),
                                                  });

    EXPECT_TRUE(hash_rt.changed);
    EXPECT_EQ(material->DirtyFlags(), owe::SceneMaterialDirtyPipeline);

    auto hash_events = scene.ConsumePreparedMaterialDirtyEvents();
    ASSERT_EQ(hash_events.len(), usize(1));
    EXPECT_EQ(hash_events[usize()].flags, owe::SceneMaterialDirtyPipeline);

    auto layout_only                   = hash_only.clone();
    layout_only.descriptor_layout_hash = usize(2000);
    auto layout_shader                 = Arc<owe::SceneShader>::make();
    layout_shader->name                = "variant"_Str;
    layout_shader->codes.push(
        owe::ShaderCode::from(rstd::array<rstd::uint32_t, 1> { 102u }.as_slice()));
    auto layout_rt = scene.SetMaterialShaderVariant(*material,
                                                    owe::SceneShaderVariantMutation {
                                                        .shader  = Some(layout_shader.clone()),
                                                        .variant = layout_only.clone(),
                                                    });

    EXPECT_TRUE(layout_rt.changed);
    EXPECT_EQ(material->DirtyFlags(),
              owe::SceneMaterialDirtyResources | owe::SceneMaterialDirtyPipeline);

    auto layout_events = scene.ConsumePreparedMaterialDirtyEvents();
    ASSERT_EQ(layout_events.len(), usize(1));
    EXPECT_EQ(layout_events[usize()].flags,
              owe::SceneMaterialDirtyResources | owe::SceneMaterialDirtyPipeline);

    auto same_slots = layout_only.clone();
    (void)same_slots.resolved_combos.insert("USE_B"_Str, "2"_Str);
    same_slots.stages[usize(0)].code_hash = rstd::usize(103);
    auto shader                           = Arc<owe::SceneShader>::make();
    shader->name                          = "variant"_Str;
    shader->codes.push(owe::ShaderCode::from(rstd::array<rstd::uint32_t, 1> { 1u }.as_slice()));
    auto pipeline_rt = scene.SetMaterialShaderVariant(*material,
                                                      owe::SceneShaderVariantMutation {
                                                          .shader  = Some(shader.clone()),
                                                          .variant = same_slots.clone(),
                                                      });

    EXPECT_TRUE(pipeline_rt.changed);
    EXPECT_EQ(material->DirtyFlags(),
              owe::SceneMaterialDirtyResources | owe::SceneMaterialDirtyPipeline);
    ASSERT_EQ(material->textures.len().to_primitive(), 2u);
    EXPECT_EQ(material->textures[rstd::usize(0)].as_str(), "tex/a"_str);
    EXPECT_TRUE(material->textures[rstd::usize(1)].is_empty());

    auto events = scene.ConsumePreparedMaterialDirtyEvents();
    ASSERT_EQ(events.len(), usize(1));
    EXPECT_EQ(events[usize()].flags,
              owe::SceneMaterialDirtyResources | owe::SceneMaterialDirtyPipeline);

    auto graph_slots = same_slots.clone();
    (void)graph_slots.resolved_combos.insert("USE_B"_Str, "1"_Str);
    graph_slots.stages[usize(0)].active_texture_slots.clear();
    (void)graph_slots.stages[usize(0)].active_texture_slots.insert(u32(1));
    auto graph_shader  = Arc<owe::SceneShader>::make();
    graph_shader->name = "variant"_Str;
    graph_shader->codes.push(
        owe::ShaderCode::from(rstd::array<rstd::uint32_t, 1> { 2u }.as_slice()));
    auto graph_rt = scene.SetMaterialShaderVariant(*material,
                                                   owe::SceneShaderVariantMutation {
                                                       .shader  = Some(graph_shader.clone()),
                                                       .variant = graph_slots.clone(),
                                                   });

    EXPECT_TRUE(graph_rt.changed);
    EXPECT_EQ(material->DirtyFlags(), owe::SceneMaterialDirtyGraph);
    ASSERT_EQ(material->textures.len().to_primitive(), 2u);
    EXPECT_TRUE(material->textures[rstd::usize(0)].is_empty());
    EXPECT_EQ(material->textures[rstd::usize(1)].as_str(), "tex/b"_str);
}

TEST(SceneVisibility, VisibleRuntimeChangeClearsOnlyVisibilityElideReason) {
    owe::Scene scene;
    auto       node = rstd::sync::Arc<owe::SceneNode>::make();
    node->ID()      = rstd::i32(7);
    node->SetVisible(false);
    scene.RegisterNode(*node, Some(owe::WallpaperLayerId { .value = rstd::i32(7) }));
    scene.RootMut()->AppendChild(node.clone());

    scene.MarkLayerVisibilityElidable(owe::WallpaperLayerId { .value = rstd::i32(7) });
    ASSERT_TRUE(scene.IsLayerElidable(owe::WallpaperLayerId { .value = i32(7) }));

    EXPECT_TRUE(scene.SetNodeVisible(*node.as_ptr(), true));
    EXPECT_TRUE(node->Visible());
    EXPECT_FALSE(scene.IsLayerVisibilityElidable(owe::WallpaperLayerId { .value = i32(7) }));
    EXPECT_FALSE(scene.IsLayerElidable(owe::WallpaperLayerId { .value = i32(7) }));

    scene.MarkLayerStaticElidable(owe::WallpaperLayerId { .value = rstd::i32(7) });
    scene.MarkLayerVisibilityElidable(owe::WallpaperLayerId { .value = rstd::i32(7) });
    EXPECT_FALSE(scene.SetNodeVisible(*node.as_ptr(), true));
    EXPECT_FALSE(scene.IsLayerVisibilityElidable(owe::WallpaperLayerId { .value = i32(7) }));
    EXPECT_TRUE(scene.IsLayerStaticElidable(owe::WallpaperLayerId { .value = i32(7) }));
    EXPECT_TRUE(scene.IsLayerElidable(owe::WallpaperLayerId { .value = i32(7) }));
}

TEST(SceneVisibility, UserBindingVisibilityChangesRequireGraphRebuild) {
    owe::Scene scene;
    auto       node = rstd::sync::Arc<owe::SceneNode>::make();
    node->ID()      = rstd::i32(7);
    node->SetVisible(false);
    scene.RegisterNode(*node, Some(owe::WallpaperLayerId { .value = rstd::i32(7) }));
    scene.RootMut()->AppendChild(node.clone());

    scene.MarkLayerVisibilityElidable(owe::WallpaperLayerId { .value = rstd::i32(7) });
    node->SetVisibleUserBinding(owe::SceneUserVisibilityBinding {
        .key = rstd::string::String::make("variant"_str),
    });
    EXPECT_TRUE(scene.IsLayerVisibilityElidable(owe::WallpaperLayerId { .value = i32(7) }));
    EXPECT_TRUE(scene.IsLayerElidable(owe::WallpaperLayerId { .value = i32(7) }));
    EXPECT_TRUE(scene.ApplyUserNodeVisibilityBindings("variant"_str, rstd::into<owe::Json>(true)));
    EXPECT_TRUE(node->Visible());
    EXPECT_FALSE(scene.IsLayerElidable(owe::WallpaperLayerId { .value = i32(7) }));
    EXPECT_FALSE(scene.ApplyUserNodeVisibilityBindings("variant"_str, rstd::into<owe::Json>(true)));
    EXPECT_TRUE(scene.ApplyUserNodeVisibilityBindings("variant"_str, rstd::into<owe::Json>(false)));
    EXPECT_FALSE(node->Visible());
    EXPECT_TRUE(scene.IsLayerElidable(owe::WallpaperLayerId { .value = i32(7) }));
}

TEST(SceneVisibility, TransientWritesDoNotDirtyTheFinalGraphState) {
    owe::Scene scene;
    auto       node = rstd::sync::Arc<owe::SceneNode>::make();
    node->ID()      = rstd::i32(7);
    scene.RegisterNode(*node, Some(owe::WallpaperLayerId { .value = rstd::i32(7) }));
    scene.RootMut()->AppendChild(node.clone());

    EXPECT_FALSE(scene.ConsumeRenderGraphDirty());
    EXPECT_TRUE(scene.SetNodeVisible(*node.as_ptr(), false));
    EXPECT_TRUE(scene.SetNodeVisible(*node.as_ptr(), true));
    EXPECT_FALSE(scene.ConsumeRenderGraphDirty());

    EXPECT_TRUE(scene.SetNodeVisible(*node.as_ptr(), false));
    EXPECT_TRUE(scene.ConsumeRenderGraphDirty());
    EXPECT_FALSE(scene.ConsumeRenderGraphDirty());
}

TEST(SceneRenderTargets, EnsureLinkRenderTargetCreatesOwnedDescriptor) {
    owe::Scene scene;
    scene.SetOrtho({ i32(1920), i32(1080) });

    owe::SceneNode sized;
    sized.SetSize({ 64.0f, 32.0f });
    auto key = scene.EnsureLinkRenderTarget(owe::WallpaperLayerId { .value = rstd::i32(7) }, sized);
    EXPECT_EQ(key, "_rt_link_7"_str);
    auto target = scene.RenderTarget(key.as_str());
    ASSERT_TRUE(target.is_some());
    EXPECT_EQ((**target).width, i32(64));
    EXPECT_EQ((**target).height, i32(32));
    EXPECT_TRUE((**target).initialize_transparent);

    owe::SceneNode fallback;
    auto           fallback_key =
        scene.EnsureLinkRenderTarget(owe::WallpaperLayerId { .value = rstd::i32(8) }, fallback);
    auto fallback_target = scene.RenderTarget(fallback_key.as_str());
    ASSERT_TRUE(fallback_target.is_some());
    EXPECT_EQ((**fallback_target).width, i32(1920));
    EXPECT_EQ((**fallback_target).height, i32(1080));
}

TEST(SceneRenderTargets, ReplacesDescriptorBehindStableName) {
    owe::Scene scene;
    scene.RegisterRenderTarget(String::make("_rt_clock"_str),
                               owe::SceneRenderTarget { .width = i32(64), .height = i32(32) });
    scene.RegisterRenderTarget(String::make("_rt_clock"_str),
                               owe::SceneRenderTarget { .width = i32(128), .height = i32(64) });

    auto target = scene.RenderTarget("_rt_clock"_str);
    ASSERT_TRUE(target.is_some());
    EXPECT_EQ((**target).width, i32(128));
    EXPECT_EQ((**target).height, i32(64));
    ASSERT_EQ(scene.RenderTargetNames().len(), usize(1));
    EXPECT_EQ(scene.RenderTargetNames()[usize()], "_rt_clock"_str);
}

TEST(SceneRenderTargets, CoalescesRuntimeExtentChanges) {
    owe::Scene scene;
    scene.RegisterRenderTarget(String::make("_rt_clock"_str),
                               owe::SceneRenderTarget { .width = i32(64), .height = i32(32) });

    EXPECT_TRUE(scene.ResizeRenderTarget("_rt_clock"_str, i32(96), i32(48)));
    EXPECT_TRUE(scene.ResizeRenderTarget("_rt_clock"_str, i32(128), i32(64)));
    EXPECT_FALSE(scene.ResizeRenderTarget("_rt_clock"_str, i32(128), i32(64)));

    auto events = scene.ConsumePreparedRenderTargetDirtyEvents();
    ASSERT_EQ(events.len(), usize(1));
    EXPECT_EQ(events[usize()].name, "_rt_clock"_str);
    EXPECT_EQ(events[usize()].old_width, i32(64));
    EXPECT_EQ(events[usize()].old_height, i32(32));
    EXPECT_EQ(events[usize()].width, i32(128));
    EXPECT_EQ(events[usize()].height, i32(64));
    EXPECT_TRUE(scene.ConsumePreparedRenderTargetDirtyEvents().is_empty());
    EXPECT_TRUE(scene.ConsumePreparedMeshDirtyEvents().is_empty());
}

TEST(SceneMaterialTextureDependency, ClassifiesPreparedRefreshCompatibility) {
    EXPECT_EQ(owe::ClassifySceneMaterialTexture(""_str),
              owe::SceneMaterialTextureDependency::Empty);
    EXPECT_EQ(owe::ClassifySceneMaterialTexture("tex/main"_str),
              owe::SceneMaterialTextureDependency::Imported);
    EXPECT_EQ(owe::ClassifySceneMaterialTexture("_rt_default"_str),
              owe::SceneMaterialTextureDependency::RenderTarget);
    EXPECT_EQ(owe::ClassifySceneMaterialTexture("_rt_link_7"_str),
              owe::SceneMaterialTextureDependency::LinkRenderTarget);
    EXPECT_EQ(owe::ClassifySceneMaterialTexture("_rt_MipMappedFrameBuffer"_str),
              owe::SceneMaterialTextureDependency::MipMappedFramebuffer);

    EXPECT_TRUE(owe::CanRefreshSceneMaterialTextureBinding(""_str, "tex/main"_str));
    EXPECT_TRUE(owe::CanRefreshSceneMaterialTextureBinding("tex/a"_str, "tex/b"_str));
    EXPECT_TRUE(owe::CanRefreshSceneMaterialTextureBinding("tex/a"_str, ""_str));
    EXPECT_TRUE(owe::CanRefreshSceneMaterialTextureBinding("_rt_link_7"_str, "_rt_link_7"_str));
    EXPECT_TRUE(owe::CanRefreshSceneMaterialTextureBinding("_rt_default"_str, "_rt_default"_str));
    EXPECT_FALSE(owe::CanRefreshSceneMaterialTextureBinding("tex/a"_str, "_rt_default"_str));
    EXPECT_FALSE(owe::CanRefreshSceneMaterialTextureBinding("_rt_link_7"_str, "tex/a"_str));
    EXPECT_FALSE(owe::CanRefreshSceneMaterialTextureBinding("tex/a"_str, "_rt_link_7"_str));
    EXPECT_FALSE(
        owe::CanRefreshSceneMaterialTextureBinding("tex/a"_str, "_rt_MipMappedFrameBuffer"_str));
    EXPECT_FALSE(owe::CanRefreshSceneMaterialTextureBinding("_rt_default"_str, "tex/a"_str));
    EXPECT_FALSE(owe::CanRefreshSceneMaterialTextureBinding(
        "tex/a"_str, "_rt_default"_str, "_rt_default"_str));
}

TEST(RenderSceneSnapshot, ExtractsDescriptorsAndRenderItems) {
    owe::Scene scene;
    scene.RootMut()->ID() = rstd::i32(1);
    scene.RegisterTexture(String::make("tex/main"_str),
                          owe::SceneTexture { .url = "tex/main"_Str });
    scene.RegisterRenderTarget(String::make("_rt_default"_str),
                               owe::SceneRenderTarget { .width = i32(1920), .height = i32(1080) });
    scene.RegisterRenderTarget(String::make("_rt_mask"_str),
                               owe::SceneRenderTarget { .width = i32(256), .height = i32(256) });

    auto child                                  = rstd::sync::Arc<owe::SceneNode>::make();
    child->ID()                                 = rstd::i32(42);
    auto mesh                                   = MakeSingleSubmesh("child-material");
    mesh->Submeshes()[usize(0)].output_override = "_rt_mask"_Str;
    mesh->MaterialSlots()[usize()]->textures.push("_rt_link_7"_Str);
    child->AddMesh(mesh.clone());
    scene.RegisterNode(*child, Some(owe::WallpaperLayerId { .value = rstd::i32(42) }));
    scene.RootMut()->AppendChild(child.clone());

    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
    EXPECT_GT(snapshot.Version().value, rstd::u64());
    ASSERT_EQ(snapshot.RenderItems().len(), rstd::usize(1));
    ASSERT_EQ(snapshot.TextureDescs().len(), rstd::usize(1));
    ASSERT_EQ(snapshot.RenderTargetDescs().len(), rstd::usize(2));

    auto node_id = scene.ResourceIndex().nodeId(*child.as_ptr());
    ASSERT_TRUE(node_id.is_some());
    auto draw_id = scene.ResourceIndex().drawItemFor(*node_id, rstd::u32());
    ASSERT_TRUE(draw_id.is_some());

    auto render_item_id = snapshot.renderItemFor(*draw_id);
    ASSERT_TRUE(render_item_id.is_some());
    const auto* render_item = snapshot.renderItem(*render_item_id);
    ASSERT_NE(render_item, nullptr);
    EXPECT_EQ(render_item->scene_draw_item.index, draw_id->index);
    EXPECT_EQ(render_item->scene_node.index, node_id->index);
    EXPECT_EQ(render_item->source_layer.value, rstd::i32(42));
    ASSERT_TRUE(render_item->output_override.is_some());

    const auto* mask_desc = snapshot.renderTargetDesc(*render_item->output_override);
    ASSERT_NE(mask_desc, nullptr);
    EXPECT_EQ(mask_desc->key, "_rt_mask"_str);
    EXPECT_EQ(mask_desc->desc.width, i32(256));
    EXPECT_EQ(mask_desc->desc.height, i32(256));

    auto tex_desc_id = snapshot.textureDescId("tex/main"_str);
    ASSERT_TRUE(tex_desc_id.is_some());
    const auto* tex_desc = snapshot.textureDesc(*tex_desc_id);
    ASSERT_NE(tex_desc, nullptr);
    EXPECT_EQ(tex_desc->key, "tex/main"_str);
    EXPECT_EQ(tex_desc->desc.url, "tex/main"_str);

    auto layer_items = snapshot.renderItemsFor(owe::WallpaperLayerId { .value = rstd::i32(42) });
    ASSERT_EQ(layer_items.len(), rstd::usize(1));
    EXPECT_EQ(layer_items[rstd::usize()].index, render_item_id->index);
    EXPECT_EQ(layer_items[rstd::usize()].generation, render_item_id->generation);

    auto material_items = snapshot.renderItemsFor(render_item->scene_material);
    ASSERT_EQ(material_items.len(), rstd::usize(1));
    EXPECT_EQ(material_items[rstd::usize()].index, render_item_id->index);
    EXPECT_EQ(material_items[rstd::usize()].generation, render_item_id->generation);

    auto mesh_items = snapshot.renderItemsFor(render_item->scene_mesh);
    ASSERT_EQ(mesh_items.len(), rstd::usize(1));
    EXPECT_EQ(mesh_items[rstd::usize()].index, render_item_id->index);
    EXPECT_EQ(mesh_items[rstd::usize()].generation, render_item_id->generation);

    EXPECT_TRUE(snapshot.HasLinkConsumer(owe::WallpaperLayerId { .value = rstd::i32(7) }));
    EXPECT_FALSE(snapshot.HasLinkConsumer(owe::WallpaperLayerId { .value = rstd::i32(42) }));
    EXPECT_TRUE(snapshot.LinkedLayerIds().contains(rstd::i32(7)));

    auto rebuilt = owe::ExtractRenderSceneSnapshot(scene);
    EXPECT_GT(rebuilt.Version().value, snapshot.Version().value);
    EXPECT_EQ(rebuilt.renderItem(*render_item_id), nullptr);
}

TEST(RenderSceneSnapshot, PlansLinkRenderTargetForElidableLinkedSource) {
    owe::Scene scene;
    scene.SetOrtho({ i32(1920), i32(1080) });
    scene.RegisterRenderTarget(String::make("_rt_default"_str),
                               owe::SceneRenderTarget { .width = i32(1920), .height = i32(1080) });
    scene.RootMut()->ID() = rstd::i32(1);
    scene.MarkLayerStaticElidable(owe::WallpaperLayerId { .value = i32(7) });

    auto source  = rstd::sync::Arc<owe::SceneNode>::make();
    source->ID() = rstd::i32(7);
    source->SetSize({ 64.0f, 32.0f });
    source->AddMesh(MakeSingleSubmesh("source-material"));
    scene.RootMut()->AppendChild(source.clone());
    scene.RegisterLayerLinkSource(owe::WallpaperLayerId { .value = rstd::i32(7) }, *source);

    auto consumer      = rstd::sync::Arc<owe::SceneNode>::make();
    consumer->ID()     = rstd::i32(42);
    auto consumer_mesh = MakeSingleSubmesh("consumer-material");
    consumer_mesh->MaterialSlots()[usize()]->textures.push("_rt_link_7"_Str);
    consumer->AddMesh(consumer_mesh.clone());
    scene.RootMut()->AppendChild(consumer.clone());

    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);

    auto link_target = scene.RenderTarget("_rt_link_7"_str);
    ASSERT_TRUE(link_target.is_some());
    EXPECT_EQ((**link_target).width, i32(64));
    EXPECT_EQ((**link_target).height, i32(32));

    auto* link_source = snapshot.linkSource(owe::WallpaperLayerId { .value = rstd::i32(7) });
    ASSERT_NE(link_source, nullptr);
    EXPECT_EQ(link_source->render_target_key, "_rt_link_7"_str);

    const auto* link_desc = snapshot.renderTargetDesc(link_source->render_target);
    ASSERT_NE(link_desc, nullptr);
    EXPECT_EQ(link_desc->key, "_rt_link_7"_str);
    EXPECT_EQ(link_desc->desc.width, i32(64));
    EXPECT_EQ(link_desc->desc.height, i32(32));
}

TEST(RenderSceneSnapshot, UsesRegisteredLayerLinkSource) {
    owe::Scene scene;
    scene.SetOrtho({ i32(1920), i32(1080) });
    scene.RegisterRenderTarget(String::make("_rt_default"_str),
                               owe::SceneRenderTarget { .width = i32(1920), .height = i32(1080) });
    scene.RootMut()->ID() = rstd::i32(1);
    scene.MarkLayerVisibilityElidable(owe::WallpaperLayerId { .value = rstd::i32(7) });

    auto producer = rstd::sync::Arc<owe::SceneNode>::make();
    producer->SetSize({ 64.0f, 32.0f });
    producer->AddMesh(MakeSingleSubmesh("producer-material"));
    scene.RootMut()->AppendChild(producer.clone());

    auto public_node  = rstd::sync::Arc<owe::SceneNode>::make();
    public_node->ID() = rstd::i32(7);
    public_node->SetSize({ 320.0f, 180.0f });
    public_node->AddMesh(MakeSingleSubmesh("public-material"));
    scene.RegisterNode(*public_node, Some(owe::WallpaperLayerId { .value = rstd::i32(7) }));
    scene.RootMut()->AppendChild(public_node.clone());
    scene.RegisterLayerLinkSource(owe::WallpaperLayerId { .value = rstd::i32(7) },
                                  *producer.as_ptr());

    auto consumer      = rstd::sync::Arc<owe::SceneNode>::make();
    consumer->ID()     = rstd::i32(42);
    auto consumer_mesh = MakeSingleSubmesh("consumer-material");
    consumer_mesh->MaterialSlots()[usize()]->textures.push("_rt_link_7"_Str);
    consumer->AddMesh(consumer_mesh.clone());
    scene.RootMut()->AppendChild(consumer.clone());

    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);

    EXPECT_EQ(scene.RegisteredLayerLinkSource(owe::WallpaperLayerId { .value = rstd::i32(7) }),
              producer.as_ptr());
    ASSERT_TRUE(scene.ResolveLayerLinkSource(*producer.as_ptr()).is_some());
    EXPECT_EQ(scene.ResolveLayerLinkSource(*producer.as_ptr())->value, rstd::i32(7));
    EXPECT_TRUE(scene.ResolveLayerLinkSource(*public_node.as_ptr()).is_none());
    auto link_target = scene.RenderTarget("_rt_link_7"_str);
    ASSERT_TRUE(link_target.is_some());
    EXPECT_EQ((**link_target).width, i32(64));
    EXPECT_EQ((**link_target).height, i32(32));
    const auto* link_source = snapshot.linkSource(owe::WallpaperLayerId { .value = rstd::i32(7) });
    ASSERT_NE(link_source, nullptr);
    EXPECT_EQ(link_source->scene_node, producer->Identity());
}

TEST(SceneRenderGroups, ReplacesCameraByLayerId) {
    owe::Scene scene;
    auto       layer = owe::WallpaperLayerId { .value = i32(7) };

    scene.RegisterRenderGroup(layer, String::make("first"_str));
    scene.RegisterRenderGroup(layer, String::make("second"_str));

    auto camera = scene.RenderGroupCamera(layer);
    ASSERT_TRUE(camera.is_some());
    EXPECT_EQ(*camera, "second"_str);
}

TEST(SceneLinkedCameras, UpdatesRegisteredTargets) {
    owe::Scene scene;
    scene.RegisterCamera(
        String::make("source"_str),
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(100.0, 50.0, -1.0, 1.0)));
    scene.RegisterCamera(
        String::make("linked"_str),
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(1.0, 1.0, -1.0, 1.0)));
    scene.RegisterLinkedCamera(String::make("source"_str), String::make("linked"_str));

    scene.UpdateLinkedCamera("source"_str);

    auto linked = scene.Camera("linked"_str);
    ASSERT_TRUE(linked.is_some());
    EXPECT_DOUBLE_EQ((**linked).Width(), 100.0);
    EXPECT_DOUBLE_EQ((**linked).Height(), 50.0);
}

TEST(SceneCameras, ActiveCameraTracksRegisteredReplacement) {
    owe::Scene scene;
    scene.RegisterCamera(
        String::make("main"_str),
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(100.0, 50.0, -1.0, 1.0)));
    ASSERT_TRUE(scene.SetActiveCamera("main"_str));
    scene.RegisterCamera(
        String::make("main"_str),
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(200.0, 80.0, -1.0, 1.0)));

    auto active = scene.ActiveCamera();
    ASSERT_TRUE(active.is_some());
    EXPECT_DOUBLE_EQ((**active).Width(), 200.0);
    EXPECT_EQ(scene.CameraNames().len(), usize(1));
}

TEST(SceneCameras, ActiveTransformsUpdateLinkedCamera) {
    owe::Scene scene;
    auto       source =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakePerspective(1.0, 0.01, 1000.0, 53.0));
    source->SetLookAt({ 0.0, 0.0, 10.0 }, Eigen::Vector3d::Zero(), Eigen::Vector3d::UnitY());
    auto linked =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakePerspective(1.0, 0.01, 1000.0, 53.0));
    scene.RegisterCamera(String::make("main"_str), rstd::move(source));
    scene.RegisterCamera(String::make("linked"_str), rstd::move(linked));
    scene.RegisterLinkedCamera(String::make("main"_str), String::make("linked"_str));
    ASSERT_TRUE(scene.SetActiveCamera("main"_str));

    const owe::SceneCameraTransforms transforms {
        .eye    = { 3.0, 4.0, 5.0 },
        .center = { 0.0, 1.0, 0.0 },
        .up     = { 0.0, 1.0, 0.0 },
    };
    ASSERT_TRUE(scene.SetActiveCameraTransforms(transforms));

    auto current = scene.ActiveCameraTransforms();
    auto copy    = scene.Camera("linked"_str);
    ASSERT_TRUE(current.is_some());
    ASSERT_TRUE(copy.is_some());
    EXPECT_TRUE(current->eye.isApprox(transforms.eye));
    EXPECT_TRUE((**copy).Transforms().eye.isApprox(transforms.eye));
    EXPECT_TRUE((**copy).Transforms().center.isApprox(transforms.center));
}

TEST(SceneGeometryDataGeneration, IncrementsWhenGeometryDataChanges) {
    rstd::initializer_list<owe::SceneVertexArray::SceneVertexAttribute> attrs {
        { .name = "a_Position"_Str, .type = owe::VertexType::FLOAT3 },
    };
    owe::SceneVertexArray vertices(attrs, rstd::usize(2));
    auto                  vertex_generation = vertices.DataGeneration();

    std::array<float, 6> positions { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
    ASSERT_TRUE(vertices.SetVertex(
        "a_Position"_str,
        rstd::slice<float>::from_raw_parts(positions.data(), rstd::usize(positions.size()))));
    EXPECT_GT(vertices.DataGeneration(), vertex_generation);

    vertex_generation = vertices.DataGeneration();
    vertices.ResetSize();
    EXPECT_GT(vertices.DataGeneration(), vertex_generation);

    owe::SceneIndexArray    indices(rstd::usize(6));
    auto                    index_generation = indices.DataGeneration();
    std::array<uint32_t, 3> tri { 0, 1, 2 };
    indices.Assign(
        rstd::usize(),
        rstd::slice<rstd::uint32_t>::from_raw_parts(tri.data(), rstd::usize(tri.size())));
    EXPECT_GT(indices.DataGeneration(), index_generation);
}

TEST(SceneVertexArray, RewritePublishesSizeAndGenerationOnce) {
    rstd::initializer_list<owe::SceneVertexArray::SceneVertexAttribute> attrs {
        { .name = "a_Position"_Str, .type = owe::VertexType::FLOAT3 },
    };
    owe::SceneVertexArray vertices(attrs, rstd::usize(3));
    auto                  generation = vertices.DataGeneration();

    auto first = vertices.RewriteVertices([&](owe::SceneVertexWriter& writer) {
        EXPECT_EQ(writer.Stride(), rstd::usize(4));
        EXPECT_EQ(writer.Capacity(), rstd::usize(3));
        for (rstd::usize index {}; index < rstd::usize(3); ++index) {
            auto vertex = writer.AppendZeroedVertex();
            ASSERT_TRUE(vertex.is_some());
            (*vertex)[rstd::usize()]  = static_cast<float>(index.to_primitive() + 1);
            (*vertex)[rstd::usize(1)] = static_cast<float>(index.to_primitive() + 2);
            (*vertex)[rstd::usize(2)] = static_cast<float>(index.to_primitive() + 3);
        }
        EXPECT_EQ(writer.Written(), rstd::usize(3));
        EXPECT_EQ(vertices.DataGeneration(), generation);
    });

    EXPECT_FALSE(first.overflowed);
    EXPECT_EQ(first.vertex_count, rstd::usize(3));
    EXPECT_EQ(first.capacity, rstd::usize(3));
    EXPECT_EQ(vertices.DataGeneration(), generation + rstd::u64(1));
    EXPECT_EQ(vertices.VertexCount(), rstd::usize(3));
    EXPECT_EQ(vertices.DataSize(), rstd::usize(12));
    EXPECT_FLOAT_EQ(vertices.Data()[3], 0.0f);
    EXPECT_FLOAT_EQ(vertices.Data()[7], 0.0f);
    EXPECT_FLOAT_EQ(vertices.Data()[11], 0.0f);

    generation  = vertices.DataGeneration();
    auto shrunk = vertices.RewriteVertices([&](owe::SceneVertexWriter& writer) {
        auto vertex = writer.AppendZeroedVertex();
        ASSERT_TRUE(vertex.is_some());
        (*vertex)[rstd::usize()] = 9.0f;
    });
    EXPECT_FALSE(shrunk.overflowed);
    EXPECT_EQ(shrunk.vertex_count, rstd::usize(1));
    EXPECT_EQ(vertices.DataGeneration(), generation + rstd::u64(1));
    EXPECT_EQ(vertices.VertexCount(), rstd::usize(1));
    EXPECT_FLOAT_EQ(vertices.Data()[1], 0.0f);
    EXPECT_FLOAT_EQ(vertices.Data()[2], 0.0f);
    EXPECT_FLOAT_EQ(vertices.Data()[3], 0.0f);

    generation = vertices.DataGeneration();
    auto empty = vertices.RewriteVertices([](owe::SceneVertexWriter&) {
    });
    EXPECT_FALSE(empty.overflowed);
    EXPECT_EQ(empty.vertex_count, rstd::usize());
    EXPECT_EQ(vertices.DataGeneration(), generation + rstd::u64(1));
    EXPECT_EQ(vertices.VertexCount(), rstd::usize());
    EXPECT_EQ(vertices.DataSizeOf(), rstd::usize());
}

TEST(SceneVertexArray, RewriteStopsAtCapacityAndCommitsPrefix) {
    rstd::initializer_list<owe::SceneVertexArray::SceneVertexAttribute> attrs {
        { .name = "a_Position"_Str, .type = owe::VertexType::FLOAT3 },
    };
    owe::SceneVertexArray vertices(attrs, rstd::usize(1));
    auto                  generation = vertices.DataGeneration();

    auto result = vertices.RewriteVertices([](owe::SceneVertexWriter& writer) {
        auto first = writer.AppendZeroedVertex();
        ASSERT_TRUE(first.is_some());
        (*first)[rstd::usize()] = 4.0f;
        EXPECT_TRUE(writer.AppendZeroedVertex().is_none());
        EXPECT_TRUE(writer.Overflowed());
    });

    EXPECT_TRUE(result.overflowed);
    EXPECT_EQ(result.vertex_count, rstd::usize(1));
    EXPECT_EQ(result.capacity, rstd::usize(1));
    EXPECT_EQ(vertices.DataGeneration(), generation + rstd::u64(1));
    EXPECT_EQ(vertices.VertexCount(), rstd::usize(1));
    EXPECT_FLOAT_EQ(vertices.Data()[0], 4.0f);
}

TEST(SceneVertexArray, RewriteRejectsZeroStride) {
    rstd::initializer_list<owe::SceneVertexArray::SceneVertexAttribute> attrs;
    owe::SceneVertexArray vertices(attrs, rstd::usize(1));
    auto                  generation = vertices.DataGeneration();

    auto result = vertices.RewriteVertices([](owe::SceneVertexWriter& writer) {
        EXPECT_EQ(writer.Stride(), rstd::usize());
        EXPECT_EQ(writer.Capacity(), rstd::usize());
        EXPECT_TRUE(writer.AppendZeroedVertex().is_none());
    });

    EXPECT_TRUE(result.overflowed);
    EXPECT_EQ(result.vertex_count, rstd::usize());
    EXPECT_EQ(result.capacity, rstd::usize());
    EXPECT_EQ(vertices.DataGeneration(), generation + rstd::u64(1));
    EXPECT_EQ(vertices.VertexCount(), rstd::usize());
}

TEST(SceneVertexArray, AddVertexAppendsAndMoveKeepsOwnedState) {
    rstd::initializer_list<owe::SceneVertexArray::SceneVertexAttribute> attrs {
        { .name = "a_Position"_Str, .type = owe::VertexType::FLOAT3 },
        { .name = "a_TexCoord"_Str, .type = owe::VertexType::FLOAT2 },
    };
    owe::SceneVertexArray vertices(attrs, rstd::usize(2));
    vertices.SetOption("dynamic"_str, true);

    std::array<float, 5> a { 1.0f, 2.0f, 3.0f, 0.25f, 0.5f };
    std::array<float, 5> b { 4.0f, 5.0f, 6.0f, 0.75f, 1.0f };
    ASSERT_TRUE(vertices.AddVertex(a.data()));
    ASSERT_TRUE(vertices.AddVertex(b.data()));

    owe::SceneVertexArray moved(std::move(vertices));
    EXPECT_TRUE(moved.GetOption("dynamic"_str));
    ASSERT_EQ(moved.VertexCount(), rstd::usize(2));

    const auto pos_offset =
        (moved.AttributeOffset("a_Position"_str).unwrap() / rstd::usize(sizeof(float)))
            .to_primitive();
    const auto uv_offset =
        (moved.AttributeOffset("a_TexCoord"_str).unwrap() / rstd::usize(sizeof(float)))
            .to_primitive();
    EXPECT_FLOAT_EQ(moved.Data()[pos_offset], 1.0f);
    EXPECT_FLOAT_EQ(moved.Data()[pos_offset + moved.OneSize().to_primitive()], 4.0f);
    EXPECT_FLOAT_EQ(moved.Data()[uv_offset], 0.25f);
    EXPECT_FLOAT_EQ(moved.Data()[uv_offset + moved.OneSize().to_primitive()], 0.75f);

    owe::SceneVertexArray assigned(attrs, rstd::usize(1));
    assigned = std::move(moved);
    EXPECT_TRUE(assigned.GetOption("dynamic"_str));
    ASSERT_EQ(assigned.VertexCount(), rstd::usize(2));
    EXPECT_FLOAT_EQ(assigned.Data()[pos_offset + assigned.OneSize().to_primitive() + 1], 5.0f);
}

TEST(SceneNodeFieldAnimation, AlphaAnimationTicksThroughScene) {
    owe::Scene scene;
    auto       node = rstd::sync::Arc<owe::SceneNode>::make();
    node->SetBaseColor({ 1.0f, 1.0f, 1.0f }, 0.3f);

    auto curve = Arc<owe::SceneAnimationCurve>::make();
    curve->c0.push({ .frame = i32(), .value = 0.0f });
    curve->c0.push({ .frame = i32(60), .value = 0.5f });
    curve->c0.push({ .frame = i32(100), .value = 0.0f });
    auto clip     = Arc<owe::SceneAnimationClip>::make(owe::SceneAnimationClipSpec {
        .mode = String::make("single"_str),
        .fps  = 30.0f,
        .end  = i32(180),
    });
    auto playback = Arc<owe::SceneAnimationPlayback>::make(rstd::move(clip));
    node->SetAlphaAnimation({ .curve = rstd::move(curve), .playback = rstd::move(playback) });
    scene.RootMut()->AppendChild(node.clone());

    scene.TickNodeFieldAnimations();
    EXPECT_TRUE(node->IsAlphaOverridden());
    EXPECT_FLOAT_EQ(node->EffectiveAlpha(), 0.0f);

    scene.Runtime().Advance(rstd::f64(2.0));
    scene.TickNodeFieldAnimations();
    EXPECT_FLOAT_EQ(node->EffectiveAlpha(), 0.5f);

    scene.Runtime().Advance(rstd::f64(6.8));
    scene.TickNodeFieldAnimations();
    EXPECT_FLOAT_EQ(node->EffectiveAlpha(), 0.0f);
}

TEST(SceneNodeFieldAnimation, WrapLoopReturnsSmoothlyToFirstKey) {
    owe::SceneAnimationCurve curve;
    curve.c0.push({ .frame = i32(), .value = 0.0f });
    curve.c0.push({ .frame = i32(2), .value = 10.0f });

    EXPECT_FLOAT_EQ(curve.EvaluateScalar(0.0f, { .current = 0.0f, .end = i32(4), .wraps = true }),
                    0.0f);
    EXPECT_FLOAT_EQ(curve.EvaluateScalar(0.0f, { .current = 2.0f, .end = i32(4), .wraps = true }),
                    10.0f);
    EXPECT_FLOAT_EQ(curve.EvaluateScalar(0.0f, { .current = 3.0f, .end = i32(4), .wraps = true }),
                    5.0f);
    EXPECT_FLOAT_EQ(curve.EvaluateScalar(0.0f, { .current = 4.0f, .end = i32(4), .wraps = true }),
                    0.0f);
}

TEST(SceneNodeFieldAnimation, TangentXScalesWithHalfSegmentDuration) {
    owe::SceneAnimationCurve curve;
    curve.c0.push({
        .frame         = i32(),
        .value         = 0.0f,
        .front_enabled = true,
        .front_x       = 1.0f,
        .front_y       = 10.0f,
    });
    curve.c0.push({ .frame = i32(100), .value = 0.0f });

    EXPECT_NEAR(
        curve.EvaluateScalar(0.0f, { .current = 50.0f, .end = i32(100) }), 4.438677f, 0.0001f);
}

TEST(SceneNodeFieldAnimation, StepKeyHoldsPreviousValueUntilItsFrame) {
    owe::SceneAnimationCurve curve;
    curve.c0.push({ .frame = i32(), .value = 2.0f });
    curve.c0.push({ .frame = i32(10), .value = 8.0f, .step = true });

    EXPECT_FLOAT_EQ(curve.EvaluateScalar(0.0f, { .current = 9.0f, .end = i32(10) }), 2.0f);
    EXPECT_FLOAT_EQ(curve.EvaluateScalar(0.0f, { .current = 10.0f, .end = i32(10) }), 8.0f);
}

TEST(SceneNodeFieldAnimation, MirrorTakesPrecedenceOverWrapLoop) {
    owe::SceneAnimationCurve curve;
    curve.c0.push({ .frame = i32(), .value = 0.0f });
    curve.c0.push({ .frame = i32(2), .value = 10.0f });

    EXPECT_FLOAT_EQ(curve.EvaluateScalar(0.0f, { .current = 2.0f, .end = i32(2) }), 10.0f);
    EXPECT_FLOAT_EQ(curve.EvaluateScalar(0.0f, { .current = 1.0f, .end = i32(2) }), 5.0f);
    EXPECT_FLOAT_EQ(curve.EvaluateScalar(0.0f, { .current = 0.0f, .end = i32(2) }), 0.0f);
}

TEST(SceneMeshDirtyEvents, RoutesDataAndLayoutDirtyByOwner) {
    owe::Scene scene;
    scene.RootMut()->ID() = rstd::i32(1);

    auto static_node  = rstd::sync::Arc<owe::SceneNode>::make();
    static_node->ID() = rstd::i32(2);
    auto static_mesh  = MakeSingleSubmesh("static");
    static_node->AddMesh(static_mesh.clone());
    scene.RootMut()->AppendChild(static_node.clone());

    auto dynamic_node  = rstd::sync::Arc<owe::SceneNode>::make();
    dynamic_node->ID() = rstd::i32(3);
    auto dynamic_mesh  = Arc<owe::SceneMesh>::make(true);
    dynamic_mesh->Submeshes().push(owe::SceneMesh::Submesh {});
    dynamic_mesh->AddMaterial(owe::SceneMaterial {});
    dynamic_node->AddMesh(dynamic_mesh.clone());
    scene.RootMut()->AppendChild(dynamic_node.clone());

    scene.RebuildResourceIndex();
    auto static_id = scene.ResourceIndex().meshId(*static_mesh);
    ASSERT_TRUE(static_id.is_some());

    static_mesh->SetDirty();
    dynamic_mesh->SetDirty();
    auto events = scene.ConsumePreparedMeshDirtyEvents();
    ASSERT_EQ(events.len(), usize(1));
    EXPECT_EQ(events[usize()].mesh.index, static_id->index);
    EXPECT_EQ(events[usize()].flags, owe::SceneMeshDirtyData);
    EXPECT_EQ(static_mesh->DirtyFlags(), owe::SceneMeshDirtyNone);
    EXPECT_EQ(dynamic_mesh->DirtyFlags(), owe::SceneMeshDirtyData);

    dynamic_mesh->SetLayoutDirty();
    events = scene.ConsumePreparedMeshDirtyEvents();
    ASSERT_EQ(events.len(), usize(1));
    EXPECT_EQ(events[usize()].flags, owe::SceneMeshDirtyLayout);
    EXPECT_EQ(dynamic_mesh->DirtyFlags(), owe::SceneMeshDirtyNone);
}

TEST(SceneMaterialPipeline, PublishesOwnedStateAndRoutesEveryPipelineField) {
    owe::SceneMaterial material;
    auto               original  = material.PipelineSnapshot();
    auto               pipeline  = material.Pipeline();
    pipeline.alpha_write         = Some(false);
    pipeline.depth_clamp         = true;
    pipeline.depth_bias          = true;
    pipeline.depth_bias_constant = 1.0f;
    pipeline.depth_bias_clamp    = 2.0f;
    pipeline.depth_bias_slope    = -4.0f;
    ASSERT_TRUE(material.SetPipeline(pipeline));
    EXPECT_EQ(material.PipelineRevision(), original.revision + u64(1));
    EXPECT_EQ(material.ConsumeDirtyFlags(), owe::SceneMaterialDirtyPipeline);
    EXPECT_FALSE(material.SetPipeline(pipeline));
    EXPECT_EQ(material.DirtyFlags(), owe::SceneMaterialDirtyNone);
    EXPECT_TRUE(original.value.alpha_write.is_none());
    EXPECT_FALSE(original.value.depth_bias);
    auto clone = material;
    clone.ConsumeDirtyFlags();
    ASSERT_TRUE(clone.SetDepthTest(true));
    EXPECT_FALSE(material.Pipeline().depth_test);
    EXPECT_TRUE(clone.Pipeline().depth_test);
    auto revision = clone.PipelineRevision();
    material.ConsumeDirtyFlags();
    clone = material;
    EXPECT_EQ(clone.PipelineRevision(), revision + u64(1));
    EXPECT_EQ(clone.DirtyFlags(), owe::SceneMaterialDirtyPipeline);
    EXPECT_FALSE(clone.Pipeline().depth_test);
}

TEST(SceneMaterialPipeline, MoveAssignmentInvalidatesExistingPipelineRevision) {
    owe::SceneMaterial source;
    source.SetCullMode(owe::CullMode::Back);
    source.ConsumeDirtyFlags();
    owe::SceneMaterial target;
    target.SetDepthTest(true);
    target.SetDepthWrite(true);
    auto revision = target.PipelineRevision();
    target        = rstd::move(source);
    EXPECT_EQ(target.PipelineRevision(), revision + u64(1));
    EXPECT_EQ(target.Pipeline().cull_mode, owe::CullMode::Back);
    EXPECT_FALSE(target.Pipeline().depth_test);
    EXPECT_EQ(target.DirtyFlags(), owe::SceneMaterialDirtyPipeline);
}

TEST(SceneMaterialPipeline, AssignmentDoesNotAcknowledgePendingChanges) {
    owe::SceneMaterial source;
    source.SetDepthTest(true);
    source.ConsumeDirtyFlags();
    owe::SceneMaterial target;
    target.SetDepthTest(true);
    target.SetResourceDirty();
    auto revision = target.PipelineRevision();
    target        = source;
    EXPECT_EQ(target.PipelineRevision(), revision);
    EXPECT_EQ(target.DirtyFlags(),
              owe::SceneMaterialDirtyPipeline | owe::SceneMaterialDirtyResources);
}

TEST(SceneMaterialDirtyEvents, RoutesMaterialDirtyByOwner) {
    owe::Scene scene;
    scene.RootMut()->ID() = rstd::i32(1);

    auto node  = rstd::sync::Arc<owe::SceneNode>::make();
    node->ID() = rstd::i32(2);
    auto mesh  = MakeSingleSubmesh("material");
    node->AddMesh(mesh.clone());
    scene.RootMut()->AppendChild(node.clone());

    scene.RebuildResourceIndex();
    auto* material = mesh->MaterialSlots()[usize()].as_ptr().as_raw_ptr();
    ASSERT_NE(material, nullptr);
    auto material_id = scene.ResourceIndex().materialId(*material);
    ASSERT_TRUE(material_id.is_some());

    EXPECT_TRUE(material->SetBlendMode(owe::BlendMode::Normal));
    EXPECT_FALSE(material->SetBlendMode(owe::BlendMode::Normal));
    auto events = scene.ConsumePreparedMaterialDirtyEvents();
    ASSERT_EQ(events.len(), usize(1));
    EXPECT_EQ(events[usize()].material.index, material_id->index);
    EXPECT_EQ(events[usize()].flags, owe::SceneMaterialDirtyPipeline);
    EXPECT_EQ(material->DirtyFlags(), owe::SceneMaterialDirtyNone);

    material->SetResourceDirty();
    material->SetCullMode(owe::CullMode::Back);
    events = scene.ConsumePreparedMaterialDirtyEvents();
    ASSERT_EQ(events.len(), usize(1));
    EXPECT_EQ(events[usize()].flags,
              owe::SceneMaterialDirtyResources | owe::SceneMaterialDirtyPipeline);
    EXPECT_EQ(material->DirtyFlags(), owe::SceneMaterialDirtyNone);

    material->SetResourceDirty();
    material->SetGraphDirty();
    events = scene.ConsumePreparedMaterialDirtyEvents();
    ASSERT_EQ(events.len(), usize(1));
    EXPECT_EQ(events[usize()].flags, owe::SceneMaterialDirtyGraph);
    EXPECT_EQ(material->DirtyFlags(), owe::SceneMaterialDirtyNone);

    material->SetTextureBindingsDirty();
    events = scene.ConsumePreparedMaterialDirtyEvents();
    ASSERT_EQ(events.len(), usize(1));
    EXPECT_EQ(events[usize()].flags, owe::SceneMaterialDirtyTextureBindings);
    EXPECT_EQ(material->DirtyFlags(), owe::SceneMaterialDirtyNone);
}

TEST(SceneImageEffect, RetainsNodeAddressesAcrossGrowth) {
    owe::SceneImageEffect effect;
    effect.AddNode({ .sceneNode = Arc<owe::SceneNode>::make() });
    auto* first = effect.Nodes()[usize()].get();
    for (int i = 0; i < 128; ++i) effect.AddNode({ .sceneNode = Arc<owe::SceneNode>::make() });
    EXPECT_EQ(first, effect.Nodes()[usize()].get());
    EXPECT_EQ(effect.Nodes().len(), usize(129));
}
