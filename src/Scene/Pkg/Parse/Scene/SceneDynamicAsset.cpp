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
import wescene.scene_user_property;
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

bool AssetEndsWith(ref<str> asset, ref<str> suffix) { return asset.ends_with(suffix); }

Option<String> WorkshopAssetPath(const script::LayerAssetReference& reference) {
    if (reference.workshop_id.is_none() || reference.path.starts_with("/"_str)) return None();
    auto parts = reference.path->split_once("/"_str);
    if (parts.is_none()) return None();
    auto [kind, relative] = *parts;
    if (relative->is_empty() || relative->starts_with("workshop/"_str)) return None();
    return Some(rstd::format("{}/workshop/{}/{}", kind, **reference.workshop_id, relative));
}

bool AssetPathExists(const SceneParseContext& context, ref<str> asset) {
    auto resolved = fs::ResolveAssetPath(asset);
    return resolved.is_ok() && context.vfs->metadata(resolved->as_path()).is_ok();
}

Option<String> ResolveLayerAssetPath(const SceneParseContext&           context,
                                     const script::LayerAssetReference& reference) {
    if (AssetPathExists(context, reference.path)) return Some(rstd::into<String>(reference.path));
    auto workshop_path = WorkshopAssetPath(reference);
    if (workshop_path.is_none()) return None();
    if (AssetPathExists(context, workshop_path->as_str())) return workshop_path;
    return None();
}

Option<wpscene::ImageObject> LoadImageAsset(SceneLayerBuildContext& context, ref<str> asset) {
    wpscene::ImageObject image;
    if (! image.FromAsset(asset, *context.vfs, context.layer_resources->pkg_version)) return None();
    if (image.size_from_texture) {
        if (image.material.textures.is_empty()) return None();
        auto size = ResolveTextureSize(*context.scene, image.material.textures[usize()].as_str());
        if (size.is_none()) return None();
        image.size              = *size;
        image.size_from_texture = false;
    }
    return Some(rstd::move(image));
}

void ResolveRegisteredAsset(SceneParseContext& context, ref<str> asset) {
    if (AssetEndsWith(asset, ".json"_str) && asset.starts_with("particles/"_str)) {
        if (context.dynamic_particle_prototypes.contains_key(asset)) return;
        wpscene::ParticleObject particle;
        if (! particle.FromAsset(asset, *context.vfs)) return;
        (void)context.dynamic_particle_prototypes.insert(String::make(asset), rstd::move(particle));
    }
}

void ResolveRegisteredAssets(SceneParseContext& context) {
    for (auto* field_script : context.registered_asset_scripts) {
        if (! field_script) continue;
        for (const auto& asset : field_script->RegisteredAssets()) {
            script::LayerAssetReference reference { .path        = asset.as_str(),
                                                    .workshop_id = field_script->WorkshopId() };
            auto                        resolved = ResolveLayerAssetPath(context, reference);
            if (resolved.is_some()) ResolveRegisteredAsset(context, resolved->as_str());
        }
    }
}

Option<Arc<SceneNode>> InstantiateResolvedAsset(SceneParseContext& context, SceneNode* owner,
                                                ref<str> asset) {
    auto attach = [&](i32 id) -> Option<Arc<SceneNode>> {
        auto draft = context.layer_build.node_id_map.remove(id);
        if (draft.is_none() || draft->node.is_none()) return None();
        SceneNode* parent =
            owner && owner->Parent() ? owner->Parent() : context.scene->RootMut().as_raw_ptr();
        for (const auto& before : draft->ordered_before_nodes) parent->AppendChild(before.clone());
        auto node  = (*draft->node).clone();
        node->ID() = i32(-1);
        parent->AppendChild(node.clone());
        context.scene->RegisterLayerCompanions(*node, draft->ordered_before_nodes.as_slice());
        return Some(rstd::move(node));
    };

    ResolveRegisteredAsset(context, asset);
    if (AssetEndsWith(asset, ".json"_str) && asset.starts_with("models/"_str)) {
        auto image = LoadImageAsset(context, asset);
        if (image.is_none()) return None();
        image->id   = context.NextSyntheticObjectId();
        image->name = String::make(asset);
        ParseImageObj(context, *image);
        return attach(image->id);
    } else if (AssetEndsWith(asset, ".mdl"_str)) {
        wpscene::ModelObject model;
        model.id    = context.NextSyntheticObjectId();
        model.name  = String::make(asset);
        model.model = String::make(asset);
        ParseModelObj(context, model);
        return attach(model.id);
    } else if (AssetEndsWith(asset, ".json"_str)) {
        auto prototype = context.dynamic_particle_prototypes.get(asset);
        if (prototype.is_none()) return None();
        auto particle    = (**prototype).Clone();
        particle.id      = context.NextSyntheticObjectId();
        particle.name    = rstd::into(asset);
        particle.origin  = { 0.0f, 0.0f, 0.0f };
        particle.scale   = { 1.0f, 1.0f, 1.0f };
        particle.angles  = { 0.0f, 0.0f, 0.0f };
        particle.parent  = u32();
        particle.visible = true;
        ParseParticleObj(context, particle);
        return attach(particle.id);
    } else if (AssetEndsWith(asset, ".ogg"_str)) {
        if (! context.sound_manager) return None();
        wpscene::SoundObject sound;
        sound.id          = context.NextSyntheticObjectId();
        sound.name        = rstd::into(asset);
        sound.startsilent = false;
        sound.sound.push(sound.name.clone());
        ParseSoundObj(context, sound, *context.sound_manager);
        return attach(sound.id);
    } else {
        return None();
    }
}

Option<Arc<SceneNode>> InstantiateRegisteredAsset(SceneParseContext& context, SceneNode* owner,
                                                  const script::LayerAssetReference& reference) {
    auto resolved = ResolveLayerAssetPath(context, reference);
    if (resolved.is_none()) return None();
    return InstantiateResolvedAsset(context, owner, resolved->as_str());
}

Option<SceneLayerNodeDraft> TakeCreatedLayer(SceneLayerBuildContext& context, i32 id) {
    auto parsed = context.layer_build.node_id_map.remove(id);
    if (parsed.is_none() || parsed->node.is_none()) return None();
    (*parsed->node)->ID() = i32(-1);
    return parsed;
}

Option<SceneLayerNodeDraft> BuildLayerConfiguration(SceneLayerBuildContext& context,
                                                    const Json& config, i32 id) {
    auto kind = wpscene::ClassifySceneObject(config);
    if (kind == wpscene::SceneObjectKind::Text || kind == wpscene::SceneObjectKind::Image) {
        auto decoded = wpscene::DecodeSceneObject(config,
                                                  mut_ref<fs::VFS>::from_raw_parts(context.vfs),
                                                  context.layer_resources->pkg_version);
        if (decoded.is_none()) return None();
        if (decoded->is_Text()) {
            auto text   = rstd::move(decoded->as_Text().value);
            text.id     = id;
            text.parent = u32();
            ResolveVisibleUserBinding(text.visible, text.visible_user, context.user_properties);
            for (auto& effect : text.effects)
                ResolveVisibleUserBinding(
                    effect.visible, effect.visible_user, context.user_properties);
            ParseTextObj(context, text);
            return TakeCreatedLayer(context, id);
        }
        auto image   = rstd::move(decoded->as_Image().value);
        image.id     = id;
        image.parent = u32();
        ResolveVisibleUserBinding(image.visible, image.visible_user, context.user_properties);
        for (auto& effect : image.effects)
            ResolveVisibleUserBinding(effect.visible, effect.visible_user, context.user_properties);
        const bool visible = image.visible;
        ParseImageObj(context, image);
        auto created = TakeCreatedLayer(context, id);
        if (created.is_some()) {
            auto& node = **created->node;
            if (! visible) (void)context.scene->SetNodeVisible(node, false);
            if (context.user_properties.is_some()) {
                auto apply = [&](ref<str> key) {
                    if (key.is_empty()) return;
                    auto property = (*context.user_properties)->get(key);
                    if (property.is_some())
                        SceneUserPropertyApplier::ApplyImageBinding(
                            *context.scene, node, key, **property);
                };
                apply(image.color_user_key.as_str());
                if (image.alpha_user_key != image.color_user_key)
                    apply(image.alpha_user_key.as_str());
            }
        }
        return created;
    }

    Vec<float> requested_size;
    owe::GetJsonValue(config, "size"_str, requested_size, false);
    array<float, 2> size { 2.0f, 2.0f };
    if (requested_size.len() >= usize(2)) {
        size[usize()]  = requested_size[usize()];
        size[usize(1)] = requested_size[usize(1)];
    }

    wpscene::ImageObject image;
    if (! image.FromAsset("models/util/solidlayer.json"_str,
                          size,
                          *context.vfs,
                          context.layer_resources->pkg_version)) {
        return None();
    }
    image.id      = id;
    image.name    = "__createLayer"_Str;
    image.size    = { size[usize()], size[usize(1)] };
    image.solid   = true;
    image.parent  = u32();
    image.visible = true;
    owe::GetJsonValue(config, "origin"_str, image.origin, false);
    owe::GetJsonValue(config, "angles"_str, image.angles, false);
    owe::GetJsonValue(config, "scale"_str, image.scale, false);
    owe::GetJsonValue(config, "color"_str, image.color, false);
    owe::GetJsonValue(config, "alpha"_str, image.alpha, false);
    owe::GetJsonValue(config, "brightness"_str, image.brightness, false);
    owe::GetJsonValue(config, "alignment"_str, image.alignment, false);
    owe::GetJsonValue(config, "perspective"_str, image.perspective, false);
    image.alpha = wpscene::NormalizeLayerAlpha(image.alpha);
    context.layer_resources->solid_layer_ids.insert(i32(id));
    ParseImageObj(context, image);
    return TakeCreatedLayer(context, id);
}

Option<Arc<SceneNode>> InstantiateLayerConfiguration(SceneParseContext& context, SceneNode* owner,
                                                     const Json& config) {
    auto built = BuildLayerConfiguration(context, config, context.NextSyntheticObjectId());
    if (built.is_none()) return None();
    SceneNode* parent =
        owner && owner->Parent() ? owner->Parent() : context.scene->RootMut().as_raw_ptr();
    for (auto& before : built->ordered_before_nodes) parent->AppendChild(before.clone());
    auto node = (*built->node).clone();
    parent->AppendChild(node.clone());
    return Some(rstd::move(node));
}

struct RuntimeLayerFactory {
    Scene*                                   scene;
    script::ScriptScene*                     scripts;
    HashMap<String, wpscene::ParticleObject> particles;
    Arc<SceneLayerResources>                 resources;
    i32                                      next_object_id;
    Arc<UniformCameraResolver>               camera_resolver;

    auto NextObjectId() -> i32 {
        auto id        = next_object_id;
        next_object_id = next_object_id.checked_sub(i32(1)).unwrap();
        return id;
    }

    void Rollback(SceneLayerBuildContext& context, SceneNode* parent) {
        for (auto& node : context.created_nodes) {
            if (parent && node->Parent() == parent) (void)parent->RemoveChild(*node);
            RemoveNode(*node);
        }
        for (auto* field : context.deferred_scripts)
            if (field) (void)scripts->DiscardDeferredFieldScript(*field);
        if (! scene->RemoveDetachedNodeRegistrations(context.created_nodes.as_slice()))
            rstd_error("runtime layer registrations could not be rolled back");
    }

    void RemoveNode(SceneNode& node) {
        scripts->runtime().InvalidateLayer(node);
        if (resources->particle_runtime.is_some())
            (*resources->particle_runtime)->RemoveOwner(node);
        (void)resources->puppet_layers->by_node.remove(rstd::addressof(node));
        (void)resources->puppet_layers->fallback_by_node.remove(rstd::addressof(node));
        resources->uniform_state->RemoveNodeParallaxContract(node);
        (void)resources->uniform_state->RemoveNodeState(node.Identity());
    }

    void PrepareContext(SceneLayerBuildContext& context) {
        context.user_properties       = Some(EnsureSceneUserPropertyState(*scene).Values());
        context.script_initialization = script::FieldScriptInitialization::Deferred;
        context.runtime_created       = true;
        context.track_created_nodes   = true;
    }

    auto Commit(SceneLayerBuildContext& context, SceneNode& parent,
                Option<SceneLayerNodeDraft> built) -> Option<Arc<SceneNode>> {
        if (built.is_none()) {
            Rollback(context, nullptr);
            return None();
        }
        for (auto& before : built->ordered_before_nodes)
            scene->AttachRuntimeNode(parent, before.clone());
        auto node = (*built->node).clone();
        scene->AttachRuntimeNode(parent, node.clone());
        scene->RegisterLayerCompanions(*node, built->ordered_before_nodes.as_slice());
        for (const auto& entry : context.layer_build.uniform_configs) {
            if (scene->ResourceIndex().nodeId(*entry.node).is_none()) {
                Rollback(context, rstd::addressof(parent));
                return None();
            }
        }
        for (const auto& entry : context.layer_build.text_uniform_configs) {
            if (scene->ResourceIndex().nodeId(*entry.node).is_none()) {
                Rollback(context, rstd::addressof(parent));
                return None();
            }
        }
        for (const auto& entry : context.layer_build.particle_trail_uniform_configs) {
            if (scene->ResourceIndex().nodeId(*entry.node).is_none()) {
                Rollback(context, rstd::addressof(parent));
                return None();
            }
        }
        RegisterLayerUniformSources(
            *scene, resources->uniform_state, camera_resolver, context.layer_build);
        if (! RegisterRuntimePuppetUniformSources(
                *scene, *resources, context.created_nodes.as_slice())) {
            Rollback(context, rstd::addressof(parent));
            return None();
        }
        for (auto* field : context.deferred_scripts) {
            if (! field || ! scripts->runtime().InitializeFieldScript(*field)) {
                Rollback(context, rstd::addressof(parent));
                return None();
            }
        }
        for (auto& sound : context.pending_sounds) {
            if (resources->sound_mount.is_none() ||
                ! (**resources->sound_mount)(rstd::move(sound.stream), sound.mount.clone())) {
                Rollback(context, rstd::addressof(parent));
                return None();
            }
        }
        return Some(rstd::move(node));
    }

    auto InstantiateConfig(SceneNode* owner, const Json& config) -> Option<Arc<SceneNode>> {
        auto kind = wpscene::ClassifySceneObject(config);
        if (kind != wpscene::SceneObjectKind::Image && kind != wpscene::SceneObjectKind::Text &&
            kind != wpscene::SceneObjectKind::Particle &&
            kind != wpscene::SceneObjectKind::Container &&
            kind != wpscene::SceneObjectKind::Model && kind != wpscene::SceneObjectKind::Shape &&
            kind != wpscene::SceneObjectKind::Sound)
            return None();
        auto vfs = scene->ExtensionMut<fs::VFS>();
        if (vfs.is_none()) return None();
        SceneLayerBuildContext context(*scene, **vfs, resources.clone());
        PrepareContext(context);
        auto* parent = owner && owner->Parent() ? owner->Parent() : scene->RootMut().as_raw_ptr();
        if (kind == wpscene::SceneObjectKind::Sound) {
            auto decoded = wpscene::DecodeSceneObject(
                config, mut_ref<fs::VFS>::from_raw_parts(context.vfs), resources->pkg_version);
            if (decoded.is_none() || ! decoded->is_Sound()) return None();
            auto sound   = rstd::move(decoded->as_Sound().value);
            sound.id     = NextObjectId();
            sound.parent = u32();
            ResolveVisibleUserBinding(sound.visible, sound.visible_user, context.user_properties);
            if (context.user_properties.is_some() && ! sound.volume_user_key.is_empty()) {
                auto property = (*context.user_properties)->get(sound.volume_user_key.as_str());
                if (property.is_some()) {
                    auto volume = ResolveSceneUserPropertyScalar(**property);
                    if (volume.is_some())
                        sound.volume = rstd::cmp::min(1.0f, rstd::cmp::max(0.0f, *volume));
                }
            }
            if (! ParseSoundObj(context, sound)) return Commit(context, *parent, None());
            return Commit(context, *parent, TakeCreatedLayer(context, sound.id));
        }
        if (kind == wpscene::SceneObjectKind::Shape) {
            auto decoded = wpscene::DecodeSceneObject(
                config, mut_ref<fs::VFS>::from_raw_parts(context.vfs), resources->pkg_version);
            if (decoded.is_none() || ! decoded->is_Shape()) return None();
            auto shape   = rstd::move(decoded->as_Shape().value);
            shape.id     = NextObjectId();
            shape.parent = u32();
            ResolveVisibleUserBinding(shape.visible, shape.visible_user, context.user_properties);
            for (auto& effect : shape.effects)
                ResolveVisibleUserBinding(
                    effect.visible, effect.visible_user, context.user_properties);
            ParseShapeObj(context, shape);
            return Commit(context, *parent, TakeCreatedLayer(context, shape.id));
        }
        if (kind == wpscene::SceneObjectKind::Model) {
            auto decoded = wpscene::DecodeSceneObject(
                config, mut_ref<fs::VFS>::from_raw_parts(context.vfs), resources->pkg_version);
            if (decoded.is_none() || ! decoded->is_Model()) return None();
            auto model   = rstd::move(decoded->as_Model().value);
            model.id     = NextObjectId();
            model.parent = u32();
            ResolveVisibleUserBinding(model.visible, model.visible_user, context.user_properties);
            ParseModelObj(context, model);
            return Commit(context, *parent, TakeCreatedLayer(context, model.id));
        }
        if (kind == wpscene::SceneObjectKind::Particle) {
            auto decoded = wpscene::DecodeSceneObject(
                config, mut_ref<fs::VFS>::from_raw_parts(context.vfs), resources->pkg_version);
            if (decoded.is_none() || ! decoded->is_Particle() ||
                resources->particle_runtime.is_none())
                return None();
            auto particle   = rstd::move(decoded->as_Particle().value);
            particle.id     = NextObjectId();
            particle.parent = u32();
            ResolveVisibleUserBinding(
                particle.visible, particle.visible_user, context.user_properties);
            PrepareAnimationBindings(context, particle);
            ParticleObjectParseServices services {
                .scene                     = scene,
                .vfs                       = (*vfs).as_raw_ptr(),
                .shader_cache              = resources->shader_cache.clone(),
                .shader_environment        = resources->shader_environment,
                .geometry_shader_limits    = resources->geometry_shader_limits,
                .geometry_shader_supported = resources->geometry_shader_supported,
                .global_base_uniforms      = resources->global_base_uniforms.clone(),
                .particle_runtime          = (*resources->particle_runtime).clone(),
                .ortho_w                   = resources->ortho_w,
                .ortho_h                   = resources->ortho_h,
                .construction_context      = context,
            };
            auto parsed                         = BuildParticleObject(services, particle);
            context.layer_build.uniform_configs = rstd::move(parsed.uniform_configs);
            context.layer_build.particle_trail_uniform_configs =
                rstd::move(parsed.trail_uniform_configs);
            if (parsed.root.is_none()) return Commit(context, *parent, None());
            return Commit(context,
                          *parent,
                          Some(SceneLayerNodeDraft { .node = Some(rstd::move(*parsed.root)) }));
        }
        auto built = BuildLayerConfiguration(context, config, NextObjectId());
        return Commit(context, *parent, rstd::move(built));
    }

    auto InstantiateImageAsset(SceneNode& parent, ref<str> asset) -> Option<Arc<SceneNode>> {
        auto vfs = scene->ExtensionMut<fs::VFS>();
        if (vfs.is_none()) return None();
        auto path = fs::ResolveAssetPath(asset);
        if (path.is_err() || (*vfs)->metadata(path->as_path()).is_err()) return None();
        SceneLayerBuildContext context(*scene, **vfs, resources.clone());
        PrepareContext(context);
        auto image = LoadImageAsset(context, asset);
        if (image.is_none()) return None();
        image->id   = NextObjectId();
        image->name = String::make(asset);
        ParseImageObj(context, *image);
        return Commit(context, parent, TakeCreatedLayer(context, image->id));
    }

    auto Instantiate(SceneNode& parent, ref<str> asset) -> Option<Arc<SceneNode>> {
        if (AssetEndsWith(asset, ".ogg"_str)) {
            auto vfs = scene->ExtensionMut<fs::VFS>();
            if (vfs.is_none()) return None();
            auto path = fs::ResolveAssetPath(asset);
            if (path.is_err() || (*vfs)->metadata(path->as_path()).is_err()) return None();
            SceneLayerBuildContext context(*scene, **vfs, resources.clone());
            PrepareContext(context);
            wpscene::SoundObject sound;
            sound.id   = NextObjectId();
            sound.name = String::make(asset);
            sound.sound.push(String::make(asset));
            if (! ParseSoundObj(context, sound)) return Commit(context, parent, None());
            return Commit(context, parent, TakeCreatedLayer(context, sound.id));
        }
        if (AssetEndsWith(asset, ".mdl"_str)) {
            auto vfs = scene->ExtensionMut<fs::VFS>();
            if (vfs.is_none()) return None();
            auto path = fs::ResolveAssetPath(asset);
            if (path.is_err() || (*vfs)->metadata(path->as_path()).is_err()) return None();
            SceneLayerBuildContext context(*scene, **vfs, resources.clone());
            PrepareContext(context);
            wpscene::ModelObject model;
            model.id    = NextObjectId();
            model.name  = String::make(asset);
            model.model = String::make(asset);
            ParseModelObj(context, model);
            return Commit(context, parent, TakeCreatedLayer(context, model.id));
        }
        if (AssetEndsWith(asset, ".json"_str) && asset.starts_with("models/"_str))
            return InstantiateImageAsset(parent, asset);

        if (resources->particle_runtime.is_none()) return None();
        auto vfs = scene->ExtensionMut<fs::VFS>();
        if (vfs.is_none()) {
            rstd_error("registered particle asset '{}' has no runtime VFS", asset);
            return None();
        }

        if (! particles.contains_key(asset) && AssetEndsWith(asset, ".json"_str) &&
            asset.starts_with("particles/"_str)) {
            auto path = fs::ResolveAssetPath(asset);
            if (path.is_err() || (*vfs)->metadata(path->as_path()).is_err()) return None();
            wpscene::ParticleObject loaded;
            if (! loaded.FromAsset(asset, **vfs)) return None();
            (void)particles.insert(String::make(asset), rstd::move(loaded));
        }
        auto prototype = particles.get(asset);
        if (prototype.is_none()) return None();

        auto particle    = (**prototype).Clone();
        particle.id      = NextObjectId();
        particle.name    = rstd::into(asset);
        particle.origin  = { 0.0f, 0.0f, 0.0f };
        particle.scale   = { 1.0f, 1.0f, 1.0f };
        particle.angles  = { 0.0f, 0.0f, 0.0f };
        particle.parent  = u32();
        particle.visible = true;
        SceneLayerBuildContext build_context(*scene, **vfs, resources.clone());
        PrepareContext(build_context);
        PrepareAnimationBindings(build_context, particle);
        ParticleObjectParseServices services {
            .scene                     = scene,
            .vfs                       = (*vfs).as_raw_ptr(),
            .shader_cache              = resources->shader_cache.clone(),
            .shader_environment        = resources->shader_environment,
            .geometry_shader_limits    = resources->geometry_shader_limits,
            .geometry_shader_supported = resources->geometry_shader_supported,
            .global_base_uniforms      = resources->global_base_uniforms.clone(),
            .particle_runtime          = (*resources->particle_runtime).clone(),
            .ortho_w                   = resources->ortho_w,
            .ortho_h                   = resources->ortho_h,
            .construction_context      = build_context,
        };
        auto parsed                               = BuildParticleObject(services, particle);
        build_context.layer_build.uniform_configs = rstd::move(parsed.uniform_configs);
        build_context.layer_build.particle_trail_uniform_configs =
            rstd::move(parsed.trail_uniform_configs);
        if (parsed.root.is_none()) return Commit(build_context, parent, None());
        return Commit(build_context,
                      parent,
                      Some(SceneLayerNodeDraft { .node = Some(rstd::move(*parsed.root)) }));
    }

    auto operator()(SceneNode* owner, script::LayerAssetReference request)
        -> Option<Arc<SceneNode>> {
        SceneNode* parent =
            owner && owner->Parent() ? owner->Parent() : scene->RootMut().as_raw_ptr();
        auto node = Instantiate(*parent, request.path);
        if (node.is_some()) return node;
        auto workshop_path = WorkshopAssetPath(request);
        if (workshop_path.is_none()) return None();
        return Instantiate(*parent, workshop_path->as_str());
    }
};

void InstallRuntimeLayerFactory(SceneParseContext&                context,
                                const Arc<UniformCameraResolver>& camera_resolver) {
    auto& scene   = *context.scene;
    auto  scripts = scene.ExtensionMut<script::ScriptScene>();
    if (scripts.is_none()) return;
    auto factory = Arc<RuntimeLayerFactory>::make(RuntimeLayerFactory {
        .scene           = rstd::addressof(scene),
        .scripts         = rstd::addressof(**scripts),
        .particles       = rstd::move(context.dynamic_particle_prototypes),
        .resources       = context.layer_resources.clone(),
        .next_object_id  = context.next_synthetic_object_id,
        .camera_resolver = camera_resolver.clone(),
    });
    scene.SetLayerRemovalObserver(
        Box<dyn<FnMut<void(SceneNode&)>>>::make([factory = factory.clone()](SceneNode& node) {
            factory->RemoveNode(node);
        }));
    (**scripts).runtime().SetLayerFactory(script::JsRuntime::LayerFactory::make(
        [factory = factory.clone()](SceneNode* owner, script::LayerAssetReference request) mutable {
            return factory->operator()(owner, request);
        }));
    (**scripts).runtime().SetLayerConfigFactory(script::JsRuntime::LayerConfigFactory::make(
        [factory = rstd::move(factory)](SceneNode* owner, Json config) {
            return factory->InstantiateConfig(owner, config);
        }));
}

} // namespace owe
