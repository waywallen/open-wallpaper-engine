module wescene.pkg.parse;
import :scene_context;
import eigen;
import wescene.load_bench;
import wescene.core;
import wescene.types;
import rstd;
import rstd.log;
import rstd.cppstd;
import wescene.utils;
import wescene.scene;
import wescene.text;
import wescene.script;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::collections::HashMap;
using rstd::collections::HashSet;
using rstd::cppstd::as_str;
using rstd::cppstd::as_string_view;
using rstd::slice_::sort_unstable_by;
using rstd::sync::Arc;
using namespace owe;
using namespace Eigen;

namespace owe
{

bool AssetEndsWith(ref<str> asset, ref<str> suffix) { return asset.ends_with(suffix); }

Option<String> WorkshopAssetPath(const script::LayerAssetReference& reference) {
    if (reference.workshop_id.is_none() || reference.path.starts_with("/"_str)) return None();
    auto path  = as_string_view(reference.path);
    auto slash = path.find('/');
    if (slash == std::string_view::npos || slash + 1 >= path.size()) return None();
    auto relative = path.substr(slash + 1);
    if (relative.starts_with("workshop/")) return None();
    return Some(rstd::format(
        "{}/workshop/{}/{}", path.substr(0, slash), **reference.workshop_id, relative));
}

bool HasDynamicAsset(const SceneParseContext& context, ref<str> asset) {
    return context.dynamic_image_prototypes.contains_key(asset) ||
           context.dynamic_model_prototypes.contains_key(asset) ||
           context.dynamic_particle_prototypes.contains_key(asset);
}

bool AssetPathExists(const SceneParseContext& context, ref<str> asset) {
    auto resolved = fs::ResolveAssetPath(as_string_view(asset));
    return resolved.is_ok() && context.vfs->metadata(resolved->as_path()).is_ok();
}

Option<String> ResolveLayerAssetPath(const SceneParseContext&           context,
                                     const script::LayerAssetReference& reference) {
    if (HasDynamicAsset(context, reference.path) || AssetPathExists(context, reference.path))
        return Some(String::make(reference.path));
    auto workshop_path = WorkshopAssetPath(reference);
    if (workshop_path.is_none()) return None();
    if (HasDynamicAsset(context, workshop_path->as_str()) ||
        AssetPathExists(context, workshop_path->as_str()))
        return workshop_path;
    return None();
}

Option<array<float, 2>> ResolveImageAssetSize(SceneParseContext& context, ref<str> asset) {
    auto info = wpscene::LoadImageAssetInfo(*context.vfs, rstd::cppstd::as_string_view(asset));
    if (! info) return None();
    if (info->size) return Some(array<float, 2> { (*info->size)[0], (*info->size)[1] });
    if (info->first_texture.empty()) return None();

    auto parsed =
        context.scene->ParseImageHeader(rstd::cppstd::as_str(info->first_texture).unwrap());
    if (parsed.is_err()) return None();
    auto  header = rstd::move(parsed).unwrap_unchecked();
    float width {};
    float height {};
    if (header.isSprite && header.spriteAnim.numFrames() > usize()) {
        const auto& frame = header.spriteAnim.GetCurFrame();
        width             = frame.width;
        height            = frame.height;
    } else {
        width  = static_cast<float>(header.width > 0 ? header.width : header.mapWidth);
        height = static_cast<float>(header.height > 0 ? header.height : header.mapHeight);
    }
    if (width <= 0.0f || height <= 0.0f) return None();
    return Some(array<float, 2> { width, height });
}

Arc<SceneNode> CloneRegisteredNode(Scene& scene, ref<SceneNode> source, ref<str> asset) {
    auto node = Arc<SceneNode>::make(Eigen::Vector3f::Zero(),
                                     Eigen::Vector3f::Ones(),
                                     Eigen::Vector3f::Zero(),
                                     rstd::cppstd::to_string(asset));
    node->SetSize(source->Size());
    node->SetGeometryTransform(source->GeometryTransform());
    node->SetPerspective(source->Perspective());
    node->SetReflected(source->Reflected());
    node->SetBaseColor(source->BaseColor(), source->BaseAlpha());
    node->TexAnim() = source->TexAnim();
    if (! source->Camera().empty()) node->SetCamera(source->Camera());
    if (source->MeshShared()) {
        auto mesh = source->MeshShared()->CloneInstance();
        mesh->RegisterAnimations(*node);
        node->AddMesh(rstd::move(mesh));
    }
    scene.RegisterNode(*node);
    return node;
}

void ResolveRegisteredAsset(SceneParseContext& context, ref<str> asset) {
    if (AssetEndsWith(asset, ".json"_str) && asset.starts_with("models/"_str)) {
        if (context.dynamic_image_prototypes.contains_key(asset)) return;
        auto size = ResolveImageAssetSize(context, asset);
        if (size.is_none()) return;

        wpscene::ImageObject image;
        image.id = context.NextSyntheticObjectId();
        if (! image.FromAsset(asset, *size, *context.vfs, context.pkg_version)) return;
        ParseImageObj(context, image);
        auto parsed = context.node_id_map.get(image.id);
        if (parsed.is_none() || (**parsed).node.is_none()) return;
        auto node   = (*(**parsed).node).clone();
        node->ID()  = i32(-1);
        auto config = FindUniformConfig(context, *node);
        if (config == nullptr) return;
        node->SetVisible(false);
        (void)context.dynamic_image_prototypes.insert(
            String::make(asset),
            SceneParseContext::DynamicImagePrototype { node.clone(), config->Clone() });
        (void)context.node_id_map.remove(image.id);
    } else if (AssetEndsWith(asset, ".mdl"_str)) {
        if (context.dynamic_model_prototypes.contains_key(asset)) return;
        wpscene::ModelObject model;
        model.id    = context.NextSyntheticObjectId();
        model.name  = rstd::cppstd::to_string(asset);
        model.model = model.name;
        ParseModelObj(context, model);
        auto parsed = context.node_id_map.get(model.id);
        if (parsed.is_none() || (**parsed).node.is_none()) return;
        auto node  = (*(**parsed).node).clone();
        node->ID() = i32(-1);
        node->SetVisible(false);
        (void)context.dynamic_model_prototypes.insert(String::make(asset), rstd::move(node));
        (void)context.node_id_map.remove(model.id);
    } else if (AssetEndsWith(asset, ".json"_str) && asset.starts_with("particles/"_str)) {
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
    auto attach = [&](Arc<SceneNode> node) {
        SceneNode* parent =
            owner && owner->Parent() ? owner->Parent() : context.scene->RootMut().as_raw_ptr();
        parent->AppendChild(node.clone());
        return Some(rstd::move(node));
    };

    ResolveRegisteredAsset(context, asset);
    if (AssetEndsWith(asset, ".json"_str) && asset.starts_with("models/"_str)) {
        auto prototype = context.dynamic_image_prototypes.get(asset);
        if (prototype.is_none()) return None();
        auto node = CloneRegisteredNode(*context.scene, (**prototype).node.deref(), asset);
        if (context.script_scene.is_some())
            (*context.script_scene)
                ->runtime()
                .CloneImageAlignmentBinding((**prototype).node.as_ptr(), node.as_ptr());
        SetUniformConfig(
            context,
            node,
            (**prototype).uniform_config.CloneForRuntimeLayer(context.NextSyntheticObjectId()));
        return attach(rstd::move(node));
    } else if (AssetEndsWith(asset, ".mdl"_str)) {
        auto prototype = context.dynamic_model_prototypes.get(asset);
        if (prototype.is_none()) return None();
        auto source = (**prototype).deref();
        auto node   = CloneRegisteredNode(*context.scene, source, asset);
        if (auto config = FindUniformConfig(context, *source); config != nullptr)
            SetUniformConfig(
                context, node, config->CloneForRuntimeLayer(context.NextSyntheticObjectId()));
        return attach(rstd::move(node));
    } else if (AssetEndsWith(asset, ".json"_str)) {
        auto prototype = context.dynamic_particle_prototypes.get(asset);
        if (prototype.is_none()) return None();
        auto particle    = (**prototype).Clone();
        particle.id      = context.NextSyntheticObjectId();
        particle.name    = rstd::cppstd::to_string(asset);
        particle.origin  = { 0.0f, 0.0f, 0.0f };
        particle.scale   = { 1.0f, 1.0f, 1.0f };
        particle.angles  = { 0.0f, 0.0f, 0.0f };
        particle.parent  = u32();
        particle.visible = true;
        ParseParticleObj(context, particle);
        auto parsed = context.node_id_map.get(particle.id);
        if (parsed.is_none() || (**parsed).node.is_none()) return None();
        auto node  = (*(**parsed).node).clone();
        node->ID() = i32(-1);
        (void)context.node_id_map.remove(particle.id);
        return attach(rstd::move(node));
    } else if (AssetEndsWith(asset, ".ogg"_str)) {
        if (! context.sound_manager) return None();
        wpscene::SoundObject sound;
        sound.id          = context.NextSyntheticObjectId();
        sound.name        = rstd::cppstd::to_string(asset);
        sound.startsilent = false;
        sound.sound.push_back(sound.name);
        ParseSoundObj(context, sound, *context.sound_manager);
        auto parsed = context.node_id_map.get(sound.id);
        if (parsed.is_none() || (**parsed).node.is_none()) return None();
        auto node  = (*(**parsed).node).clone();
        node->ID() = i32(-1);
        (void)context.node_id_map.remove(sound.id);
        return attach(rstd::move(node));
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

Option<Arc<SceneNode>> AttachCreatedLayer(SceneParseContext& context, SceneNode* owner, i32 id) {
    auto parsed = context.node_id_map.get_mut(id);
    if (parsed.is_none() || (**parsed).node.is_none()) return None();

    SceneNode* parent =
        owner && owner->Parent() ? owner->Parent() : context.scene->RootMut().as_raw_ptr();
    for (auto& before : (**parsed).ordered_before_nodes) parent->AppendChild(before.clone());
    auto node  = (*(**parsed).node).clone();
    node->ID() = i32(-1);
    parent->AppendChild(node.clone());
    (void)context.node_id_map.remove(id);
    return Some(rstd::move(node));
}

Option<Arc<SceneNode>> InstantiateLayerConfiguration(SceneParseContext& context, SceneNode* owner,
                                                     const Json& config) {
    const i32 id = context.NextSyntheticObjectId();

    if (config.get("text"_str).is_some()) {
        wpscene::TextObject text;
        if (! text.FromJson(config, *context.vfs, context.pkg_version)) return None();
        text.id      = id;
        text.parent  = u32();
        text.visible = true;
        ParseTextObj(context, text);
        return AttachCreatedLayer(context, owner, id);
    }

    if (config.get("image"_str).is_some()) {
        wpscene::ImageObject image;
        if (! image.FromJson(config, *context.vfs, context.pkg_version)) return None();
        image.id      = id;
        image.parent  = u32();
        image.visible = true;
        ParseImageObj(context, image);
        return AttachCreatedLayer(context, owner, id);
    }

    Vec<float> requested_size;
    owe::GetJsonValue(config, "size", requested_size, false);
    array<float, 2> size { 2.0f, 2.0f };
    if (requested_size.len() >= usize(2)) {
        size[usize()]  = requested_size[usize()];
        size[usize(1)] = requested_size[usize(1)];
    }

    wpscene::ImageObject image;
    if (! image.FromAsset(
            "models/util/solidlayer.json"_str, size, *context.vfs, context.pkg_version)) {
        return None();
    }
    image.id      = id;
    image.name    = "__createLayer";
    image.size    = { size[usize()], size[usize(1)] };
    image.solid   = true;
    image.parent  = u32();
    image.visible = true;
    owe::GetJsonValue(config, "origin", image.origin, false);
    owe::GetJsonValue(config, "angles", image.angles, false);
    owe::GetJsonValue(config, "scale", image.scale, false);
    owe::GetJsonValue(config, "color", image.color, false);
    owe::GetJsonValue(config, "alpha", image.alpha, false);
    owe::GetJsonValue(config, "brightness", image.brightness, false);
    owe::GetJsonValue(config, "alignment", image.alignment, false);
    owe::GetJsonValue(config, "perspective", image.perspective, false);
    image.alpha = wpscene::NormalizeLayerAlpha(image.alpha);
    context.solid_layer_ids.insert(i32(id));
    ParseImageObj(context, image);
    return AttachCreatedLayer(context, owner, id);
}

} // namespace owe
