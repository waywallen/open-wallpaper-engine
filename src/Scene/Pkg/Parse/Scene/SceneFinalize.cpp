module;

#include <rstd/macro.hpp>

module wescene.pkg.parse;
import :scene_context;
import eigen;
import wescene.load_bench;
import wescene.core;
import wescene.types;
import rstd;
import rstd.log;
import wescene.utils;
import wescene.scene;
import wescene.text;
import wescene.script;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::collections::HashMap;
using rstd::collections::HashSet;
using rstd::slice_::sort_unstable_by;
using rstd::sync::Arc;
using namespace owe;
using namespace Eigen;

namespace owe
{

template<typename T>
struct CopyableArcHold {
    Arc<T> value;

    explicit CopyableArcHold(Arc<T> owner): value(rstd::move(owner)) {}
    CopyableArcHold(const CopyableArcHold& other): value(other.value.clone()) {}
    CopyableArcHold(CopyableArcHold&&) noexcept            = default;
    CopyableArcHold& operator=(CopyableArcHold&&) noexcept = default;
    CopyableArcHold& operator=(const CopyableArcHold&)     = delete;
};

bool RegisterUniformNodeSources(Scene& scene, const Arc<UniformSceneState>& uniform_state,
                                const Arc<UniformCameraResolver>& camera_resolver,
                                const Arc<SceneNode>& node, const UniformNodeConfigDraft& config) {
    auto node_id = scene.ResourceIndex().nodeId(*node);
    if (node_id.is_none()) return false;

    auto state       = Arc<UniformNodeState>::make(node.clone(), camera_resolver.clone());
    state->object_id = config.object_id;
    state->parallax  = config.parallax;
    state->propagate_parallax_to_children = config.propagate_parallax_to_children;
    state->ride_parent_parallax           = config.ride_parent_parallax;
    state->use_camera_eye_position        = config.use_camera_eye_position;
    state->eye_position_override          = config.eye_position_override;
    state->vertices_in_world_space        = config.vertices_in_world_space;
    state->effect_projection_size         = config.effect_projection_size;
    if (config.effect_projection_node.is_some())
        state->effect_projection_node = Some((*config.effect_projection_node).clone());
    uniform_state->SetNodeState(*node_id, state.clone());

    auto       registrar = dyn<UniformSourceRegistrar>::from_ref(scene);
    auto       writer    = dyn<UniformAttachmentWriter>::from_ref(scene);
    const auto transform = registrar->Register(Box<dyn<UniformSource>>::make(
        TransformUniformSource { uniform_state.clone(), rstd::move(state) }));
    const auto color =
        registrar->Register(Box<dyn<UniformSource>>::make(ColorUniformSource { node.clone() }));
    const auto texture =
        registrar->Register(Box<dyn<UniformSource>>::make(TextureUniformSource {}));
    (void)writer->AttachNode(*node_id, transform, i32());
    (void)writer->AttachNode(*node_id, color, i32());
    (void)writer->AttachNode(*node_id, texture, i32());
    return true;
}

bool RegisterParticleTrailUniformSource(Scene& scene, const Arc<SceneNode>& node,
                                        const Arc<ParticleTrailUniformState>& state) {
    auto node_id = scene.ResourceIndex().nodeId(*node);
    if (node_id.is_none()) return false;
    auto       registrar = dyn<UniformSourceRegistrar>::from_ref(scene);
    auto       writer    = dyn<UniformAttachmentWriter>::from_ref(scene);
    const auto source    = registrar->Register(
        Box<dyn<UniformSource>>::make(ParticleTrailUniformSource { state.clone() }));
    (void)writer->AttachNode(*node_id, source, i32(10));
    return true;
}

void RegisterLayerUniformSources(Scene& scene, const Arc<UniformSceneState>& uniform_state,
                                 const Arc<UniformCameraResolver>& camera_resolver,
                                 const SceneLayerBuildResult&      result) {
    auto active_camera = scene.ActiveCameraHandle();

    auto camera_for = [&](const SceneNode& node) -> Option<Arc<SceneCamera>> {
        if (! node.Camera().is_empty()) return scene.CameraHandle(node.Camera());
        if (node.Perspective()) return scene.CameraHandle("global_perspective"_str);
        return active_camera.is_some() ? Some((*active_camera).clone()) : None<Arc<SceneCamera>>();
    };
    auto registrar = dyn<UniformSourceRegistrar>::from_ref(scene);
    auto writer    = dyn<UniformAttachmentWriter>::from_ref(scene);
    for (const auto& draft : result.text_uniform_configs) {
        if (active_camera.is_none()) continue;
        auto node_id = scene.ResourceIndex().nodeId(*draft.node);
        if (node_id.is_none()) continue;
        auto state               = Arc<text::TextUniformState>::make(draft.node.clone());
        state->camera            = camera_for(*draft.node);
        state->active_camera     = Some((*active_camera).clone());
        state->effect_projection = draft.effect_projection.clone();
        const auto source        = registrar->Register(
            Box<dyn<UniformSource>>::make(text::TextUniformSource { rstd::move(state) }));
        (void)writer->AttachNode(*node_id, source, i32());
    }
    for (const auto& entry : result.uniform_configs) {
        (void)RegisterUniformNodeSources(
            scene, uniform_state, camera_resolver, entry.node, entry.config);
    }
    for (const auto& draft : result.particle_trail_uniform_configs) {
        (void)RegisterParticleTrailUniformSource(scene, draft.node, draft.uniform_state);
    }
}

bool RegisterRuntimePuppetUniformSources(Scene& scene, SceneLayerResources& resources,
                                         slice<Arc<SceneNode>> nodes) {
    HashMap<PuppetLayer*, UniformSourceId> sources;
    for (const auto& node : nodes) {
        auto layer = resources.puppet_layers->by_node.get(node.as_ptr().as_raw_ptr());
        if (layer.is_none()) continue;
        auto node_id = scene.ResourceIndex().nodeId(*node);
        if (node_id.is_none()) return false;
        auto*           key    = (**layer).as_ptr().as_raw_ptr();
        auto            source = sources.get(key);
        UniformSourceId id;
        if (source.is_none()) {
            id = scene.Register(
                Box<dyn<UniformSource>>::make(PuppetUniformSource { (**layer).clone() }));
            (void)sources.insert(key, id);
        } else {
            id = **source;
        }
        if (! scene.AttachNode(*node_id, id, i32(10))) return false;
    }
    return true;
}

void FinalizeUniformSources(SceneParseContext& context) {
    auto& scene = *context.scene;
    scene.RebuildResourceIndex();

    auto active_camera = scene.ActiveCameraHandle();
    if (active_camera.is_none()) return;
    auto camera_resolver =
        Arc<UniformCameraResolver>::make((*active_camera).clone(), scene.CameraRegistry());

    auto ortho = scene.Ortho();
    context.layer_resources->uniform_state->SetOrtho(
        static_cast<float>(ortho[usize()].to_primitive()),
        static_cast<float>(ortho[usize(1)].to_primitive()));
    scene.Runtime().RegisterSystem(
        UniformRuntimeSystem { context.layer_resources->uniform_state.clone(),
                               scene,
                               context.layer_resources->orthographic_scene },
        SceneRuntimeSchedule::BeforeRender);

    auto registrar = dyn<UniformSourceRegistrar>::from_ref(scene);
    auto writer    = dyn<UniformAttachmentWriter>::from_ref(scene);

    const auto frame_source = registrar->Register(Box<dyn<UniformSource>>::make(
        FrameUniformSource { context.layer_resources->uniform_state.clone() }));
    const auto audio_source = registrar->Register(Box<dyn<UniformSource>>::make(
        AudioUniformSource { context.layer_resources->uniform_state.clone() }));
    (void)writer->AttachGlobal(frame_source, i32());

    auto frame_sources = Vec<UniformSourceAttachment>::make();
    frame_sources.push(UniformSourceAttachment { .source = frame_source });
    (void)scene.RegisterUniformBlock(UniformBlockDefinition {
        .identity = kFrameUniformSchemaIdentity,
        .name     = String::make(kGlobalUniformBlockName),
        .scope    = UniformBlockScope::Shared,
        .sources  = rstd::move(frame_sources),
    });

    auto audio_sources = Vec<UniformSourceAttachment>::make();
    audio_sources.push(UniformSourceAttachment { .source = audio_source });
    (void)scene.RegisterUniformBlock(UniformBlockDefinition {
        .identity = kAudioUniformSchemaIdentity,
        .name     = String::make(kAudioUniformBlockName),
        .scope    = UniformBlockScope::Shared,
        .sources  = rstd::move(audio_sources),
    });

    bool shadow_light = false;
    auto owned_lights = scene.Lights();
    for (usize index {}; index < owned_lights.len(); ++index) {
        const auto& light = *owned_lights[index];
        if (! shadow_light && light.type() == SceneLightType::Directional &&
            light.desc().cast_shadow) {
            shadow_light = true;
        }
    }
    const auto light_source = registrar->Register(
        Box<dyn<UniformSource>>::make(LightUniformSource { ref<Scene>::from_raw_parts(&scene) }));
    auto lighting_sources = Vec<UniformSourceAttachment>::make();
    lighting_sources.push(UniformSourceAttachment { .source = light_source });
    if (context.layer_resources->shader_environment.directional_shadow && shadow_light) {
        const auto shadow_source = registrar->Register(Box<dyn<UniformSource>>::make(
            ShadowUniformSource { (*active_camera).clone(), ref<Scene>::from_raw_parts(&scene) }));
        lighting_sources.push(UniformSourceAttachment { .source = shadow_source });
    }
    (void)scene.RegisterUniformBlock(UniformBlockDefinition {
        .identity = kLightingUniformSchemaIdentity,
        .name     = String::make(kLightingUniformBlockName),
        .scope    = UniformBlockScope::Shared,
        .sources  = rstd::move(lighting_sources),
    });

    for (auto& entry : context.layer_build.uniform_configs) {
        if (entry.config.object_id != i32() &&
            context.ride_parent_parallax_ids.contains(entry.config.object_id)) {
            entry.config.ride_parent_parallax = true;
        }
    }
    RegisterLayerUniformSources(
        scene, context.layer_resources->uniform_state, camera_resolver, context.layer_build);

    HashMap<PuppetLayer*, UniformSourceId> puppet_sources;
    context.layer_resources->puppet_layers->by_node.iter().for_each([&](auto entry) {
        auto [node_ref, layer_ref] = entry;
        auto*       node           = *node_ref;
        const auto& layer          = *layer_ref;
        if (node == nullptr) return;
        auto node_id = scene.ResourceIndex().nodeId(*node);
        if (node_id.is_none()) return;
        auto* key    = layer.as_ptr().as_raw_ptr();
        auto  source = puppet_sources.get(key);
        if (source.is_none()) {
            auto registered = registrar->Register(
                Box<dyn<UniformSource>>::make(PuppetUniformSource { layer.clone() }));
            (void)puppet_sources.insert(key, registered);
            (void)writer->AttachNode(*node_id, registered, i32(10));
        } else {
            (void)writer->AttachNode(*node_id, **source, i32(10));
        }
    });

    InstallRuntimeLayerFactory(context, camera_resolver);
}

void FinalizeScene(SceneParseContext& context) {
    // Attach once after every registered node has been created in JSON
    // declaration order (node_id_order) but not yet inserted into the scene
    // graph. Walk that order and AppendChild to parent (or root). Result:
    // child lists at every depth match scene.json declaration order, which
    // is what WE treats as z-order.
    int attached = 0, missing_parent = 0;
    for (auto id : context.layer_build.node_id_order) {
        auto found = context.layer_build.node_id_map.get_mut(id);
        if (found.is_none() || (**found).node.is_none()) continue;
        auto&                      ref         = **found;
        SceneNode*                 parent_node = context.scene->RootMut().as_raw_ptr();
        const SceneLayerNodeDraft* parent_ref  = nullptr;
        if (ref.parent_id != u32()) {
            auto parent = context.layer_build.node_id_map.get(rstd::as_cast<i32>(ref.parent_id));
            if (parent.is_none() || (**parent).node.is_none()) {
                missing_parent++;
                continue;
            }
            parent_node = (*(**parent).node).as_ptr();
            parent_ref  = &**parent;
        }
        // Named MDAT anchors provide the child's local frame in the parent
        // puppet's bind space.
        if (! ref.attachment.is_empty() && parent_ref && parent_ref->puppet.is_some()) {
            const auto& puppet           = **parent_ref->puppet;
            auto        attachment_index = puppet.attachmentIndex(ref.attachment.as_str());
            if (attachment_index.is_some()) {
                context.ride_parent_parallax_ids.insert(id);
                auto apply_bind_offset = [&]() {
                    auto anchor = puppet.attachmentBindTransform(*attachment_index);
                    if (anchor.is_none()) return;
                    if (ref.apply_attachment_offset.is_some()) {
                        (*ref.apply_attachment_offset)->operator()(anchor->translation());
                    } else {
                        (*ref.node)->SetLocalFrame(anchor->matrix().cast<double>() *
                                                   (*ref.node)->LocalFrame());
                    }
                };
                if (ref.apply_attachment_offset.is_none() && parent_ref->puppet_layer.is_some()) {
                    SceneNode* node       = (*ref.node).as_ptr();
                    auto       layer      = CopyableArcHold((*parent_ref->puppet_layer).clone());
                    auto       local_base = node->LocalFrame();
                    auto       update     = [node,
                                             layer,
                                             attachment_index = *attachment_index,
                                             local_base       = rstd::move(local_base)](f64 time) {
                        auto anchor =
                            layer.value->attachmentTransform(attachment_index, time.to_primitive());
                        if (anchor.is_none()) return;
                        node->SetLocalFrame(anchor->matrix().cast<double>() * local_base);
                    };
                    update(context.scene->Runtime().Frame().elapsed);
                    context.scene->RegisterTransformUpdater(
                        Box<dyn<FnMut<void(f64)>>>::make(rstd::move(update)),
                        context.scene->RegisterNode(*node));
                } else {
                    apply_bind_offset();
                }
            }
        }
        for (auto& before_node : ref.ordered_before_nodes) {
            parent_node->AppendChild(before_node.clone());
        }
        parent_node->AppendChild((*ref.node).clone());
        context.scene->RegisterLayerCompanions(**ref.node, ref.ordered_before_nodes.as_slice());
        attached++;
    }
    rstd_info("attach: {}/{} nodes ({} missing parents)",
              attached,
              context.layer_build.node_id_map.len(),
              missing_parent);

    // If any object during the visit installed a script binding, hand the
    // ScriptScene off to the Scene now. The renderer ticks it once per
    // frame via owe::script::TickSceneScripts. Empty ScriptScenes are
    // skipped so image-only pkgs don't pay any runtime cost.
    if (context.script_scene.is_some() && ! (*context.script_scene)->empty()) {
        // Hand the scene root to the JS runtime so `thisScene.getLayer(name)`
        // can resolve against the live graph. The renderer also ticks the
        // ScriptScene once per frame via owe::script::TickSceneScripts.
        auto& runtime = (*context.script_scene)->runtime();
        if (context.general_settings.is_some()) {
            auto settings = rstd::json::Map::make();
            for (const auto& [key, value] : (*context.general_settings)->iter())
                settings.insert(key->clone(), value->clone());
            runtime.SetGeneralSettings(Json::Object(rstd::move(settings)));
        }
        for (auto id : context.layer_build.node_id_order) {
            auto node   = context.layer_build.node_id_map.get(id);
            auto config = context.initial_layer_configs.get(id);
            if (node.is_none() || (**node).node.is_none() || config.is_none()) continue;
            runtime.RegisterInitialLayerConfig((*(**node).node).as_ptr(), (**config).clone());
        }
        runtime.SetScene(context.scene);
        auto parallax_state = CopyableArcHold(context.layer_resources->uniform_state.clone());
        runtime.SetNodeParallaxDepthAccessors(
            script::JsRuntime::NodeParallaxDepthGetter::make(
                [parallax_state](SceneNode* node) mutable -> Option<script::Vec2Value> {
                    if (node == nullptr) return None();
                    auto depth = parallax_state.value->NodeParallaxDepth(*node);
                    if (depth.is_none()) return None();
                    return Some(
                        script::Vec2Value { .x = (*depth)[usize()], .y = (*depth)[usize(1)] });
                }),
            script::JsRuntime::NodeParallaxDepthSetter::make(
                [parallax_state](SceneNode* node, script::Vec2Value depth) mutable {
                    if (node == nullptr) return;
                    (void)parallax_state.value->SetNodeParallaxDepth(
                        *node, { static_cast<float>(depth.x), static_cast<float>(depth.y) });
                }));
        runtime.SetLayerFactory(script::JsRuntime::LayerFactory::make(
            [&context](SceneNode*                  owner,
                       script::LayerAssetReference request) -> Option<Arc<SceneNode>> {
                auto node = InstantiateRegisteredAsset(context, owner, request);
                if (node.is_none())
                    rstd_error("layer asset '{}' is unsupported or unavailable", request.path);
                return node;
            }));
        runtime.SetLayerConfigFactory(script::JsRuntime::LayerConfigFactory::make(
            [&context](SceneNode* owner, Json config) -> Option<Arc<SceneNode>> {
                auto node = InstantiateLayerConfiguration(context, owner, config);
                if (node.is_none()) rstd_error("layer configuration is unsupported or unavailable");
                return node;
            }));
        runtime.SetSceneRoot(context.scene->RootMut().as_raw_ptr());
        runtime.ClearLayerFactory();
        runtime.ClearLayerConfigFactory();
        owe::script::InstallScriptScene(*context.scene,
                                        context.script_scene.take().unwrap_unchecked());
    }
    if (context.layer_resources->particle_runtime.is_some()) {
        context.scene->Runtime().RegisterSystem(
            ParticleRuntimeSystem { (*context.layer_resources->particle_runtime).clone() },
            SceneRuntimeSchedule::BeforeRender);
    }
    context.layer_resources->shader_cache->ReleaseTransientEntries();
    context.scene->InstallExtension(
        Box<Arc<ShaderCache>>::make(context.layer_resources->shader_cache.clone()));
    FinalizeUniformSources(context);
}

} // namespace owe
