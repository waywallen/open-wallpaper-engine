module;

#include <rstd/macro.hpp>

module wescene.pkg.parse;
import :scene_context;
import eigen;
import rstd;
import rstd.log;
import rstd.cppstd;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::slice_::sort_unstable_by;
using rstd::sync::Arc;
using namespace owe;
using namespace Eigen;

namespace
{

SceneAnimationKey ToSceneAnimationKey(const owe::wpscene::AnimKeyframe& key) {
    return {
        .frame         = key.frame,
        .value         = key.value,
        .step          = key.step,
        .front_enabled = key.front.enabled,
        .front_x       = key.front.x,
        .front_y       = key.front.y,
        .back_enabled  = key.back.enabled,
        .back_x        = key.back.x,
        .back_y        = key.back.y,
    };
}

Vec<SceneAnimationKey> ToSceneAnimationAxis(const std::vector<owe::wpscene::AnimKeyframe>& keys) {
    Vec<SceneAnimationKey> result;
    result.reserve(usize(keys.size()));
    for (const auto& key : keys) result.push(ToSceneAnimationKey(key));
    sort_unstable_by(result.as_mut_slice().as_mut_ref(),
                     [](const SceneAnimationKey& left, const SceneAnimationKey& right) {
                         return left.frame < right.frame;
                     });
    return result;
}

Vec<SceneAnimationEvent>
ToSceneAnimationEvents(const std::vector<owe::wpscene::AnimEvent>& events) {
    Vec<SceneAnimationEvent> result;
    result.reserve(usize(events.size()));
    for (usize index {}; index < usize(events.size()); ++index) {
        const auto& event = events[index.to_primitive()];
        result.push(SceneAnimationEvent {
            .frame = event.frame,
            .order = index,
            .name  = String::make(rstd::cppstd::as_str(event.name).unwrap()),
        });
    }
    sort_unstable_by(result.as_mut_slice().as_mut_ref(),
                     [](const SceneAnimationEvent& left, const SceneAnimationEvent& right) {
                         if (left.frame != right.frame) return left.frame < right.frame;
                         return left.order < right.order;
                     });
    return result;
}

SceneAnimationCurve BuildSceneAnimationCurve(const owe::wpscene::AnimCurve& curve) {
    return SceneAnimationCurve {
        .c0       = ToSceneAnimationAxis(curve.c0),
        .c1       = ToSceneAnimationAxis(curve.c1),
        .c2       = ToSceneAnimationAxis(curve.c2),
        .relative = curve.relative,
    };
}

auto BuildSceneAnimationClip(const owe::wpscene::AnimCurve& curve, i32 end)
    -> Arc<SceneAnimationClip> {
    return Arc<SceneAnimationClip>::make(SceneAnimationClipSpec {
        .events   = ToSceneAnimationEvents(curve.options.events),
        .name     = String::make(rstd::cppstd::as_str(curve.options.name).unwrap()),
        .mode     = String::make(rstd::cppstd::as_str(curve.options.mode).unwrap()),
        .fps      = curve.options.fps > 0.0f ? curve.options.fps : 30.0f,
        .end      = std::max(curve.options.length, end),
        .wraploop = curve.options.wraploop,
    });
}

Option<SceneCameraLookAtKey> ParseLookAtKey(const owe::Json& json) {
    if (! json.is_object()) return None();
    SceneCameraLookAtKey key;
    std::array<float, 3> eye {};
    std::array<float, 3> center {};
    std::array<float, 3> up {};
    if (! owe::GetJsonValue(json, "eye", eye, false) ||
        ! owe::GetJsonValue(json, "center", center, false) ||
        ! owe::GetJsonValue(json, "up", up, false))
        return None();
    owe::GetJsonValue(json, "timestamp", key.frame, false);
    key.eye    = Vector3f(eye.data());
    key.center = Vector3f(center.data());
    key.up     = Vector3f(up.data());
    return Some(rstd::move(key));
}

Option<SceneCameraLookAtTrack> ParseLookAtTrack(const owe::Json& json) {
    auto transforms = json.get("transforms"_str);
    if (transforms.is_none()) return None();
    auto values = (*transforms)->as_array();
    if (values.is_none()) return None();

    SceneCameraLookAtTrack track;
    owe::GetJsonValue(json, "duration", track.duration, false);
    for (const auto& raw_key : **values) {
        auto key = ParseLookAtKey(raw_key);
        if (key.is_some()) track.keys.push(rstd::move(*key));
    }
    if (track.keys.is_empty()) return None();
    sort_unstable_by(track.keys.as_mut_slice().as_mut_ref(),
                     [](const SceneCameraLookAtKey& left, const SceneCameraLookAtKey& right) {
                         return left.frame < right.frame;
                     });
    if (track.duration <= 0.0f) track.duration = track.keys[track.keys.len() - usize(1)].frame;
    if (track.duration <= 0.0f) track.duration = 1.0f;
    return Some(rstd::move(track));
}

struct ResolvedFieldAnimations {
    HashMap<String, SceneAnimationTrack> tracks;

    void Bind(SceneNode& node) const {
        tracks.iter().for_each([&](auto entry) {
            auto [field, track] = entry;
            node.BindFieldAnimation(field->clone(), track->playback.clone());
        });
    }

    auto Take(ref<str> field) -> Option<SceneAnimationTrack> { return tracks.remove(field); }
};

struct AuthoredFieldAnimation {
    const wpscene::FieldBindingSpec* binding { nullptr };
    Arc<SceneAnimationCurve>         curve;
};

void CollectBindings(const wpscene::FieldBindings&          bindings,
                     Vec<const wpscene::FieldBindingSpec*>& out) {
    for (const auto& binding : bindings.Entries()) {
        if (binding.animation.is_some()) out.push(rstd::addressof(binding));
    }
}

void CollectMaterialBindings(const wpscene::Material&               material,
                             Vec<const wpscene::FieldBindingSpec*>& out) {
    CollectBindings(material.constantshadervalues_bindings, out);
}

void CollectEffectBindings(const wpscene::ImageEffect&            effect,
                           Vec<const wpscene::FieldBindingSpec*>& out) {
    CollectBindings(effect.field_bindings, out);
    for (const auto& material : effect.materials) CollectMaterialBindings(material, out);
    for (const auto& pass : effect.passes) CollectBindings(pass.constantshadervalues_bindings, out);
}

void CollectParticleBindings(const wpscene::Particle&               particle,
                             Vec<const wpscene::FieldBindingSpec*>& out) {
    CollectMaterialBindings(particle.material, out);
    for (const auto& child : particle.children) CollectParticleBindings(child.obj, out);
}

bool CompatibleTimeline(const wpscene::AnimCurve& left, const wpscene::AnimCurve& right) {
    return left.options.fps == right.options.fps && left.options.length == right.options.length &&
           left.options.mode == right.options.mode &&
           left.options.wraploop == right.options.wraploop;
}

bool HasChild(const wpscene::AnimCurve& curve, ref<str> field) {
    for (const auto& child : curve.options.children) {
        if (child == field) return true;
    }
    return false;
}

void ResolveAnimationScope(SceneAnimationBindingScope&             scope,
                           slice<const wpscene::FieldBindingSpec*> bindings) {
    scope.Clear();
    Vec<AuthoredFieldAnimation> authored;
    authored.reserve(bindings.len());
    for (auto* binding : bindings) {
        if (binding == nullptr || binding->animation.is_none()) continue;
        authored.push(AuthoredFieldAnimation {
            .binding = binding,
            .curve = Arc<SceneAnimationCurve>::make(BuildSceneAnimationCurve(*binding->animation)),
        });
    }

    Vec<usize> parents;
    parents.reserve(authored.len());
    for (usize index {}; index < authored.len(); ++index) parents.push(usize(index.to_primitive()));
    auto root = [&](usize value) {
        while (parents[value] != value) value = parents[value];
        return value;
    };
    auto unite = [&](usize left, usize right) {
        left  = root(left);
        right = root(right);
        if (left != right) parents[right] = left;
    };

    auto report = [&](SceneAnimationBindingIssue       issue,
                      const wpscene::FieldBindingSpec& binding,
                      ref<str>
                          relation) {
        scope.Report(SceneAnimationBindingDiagnostic {
            .issue    = issue,
            .binding  = binding.identity,
            .field    = binding.field.clone(),
            .relation = String::make(relation),
        });
    };

    Vec<Option<usize>> selected_parents;
    selected_parents.resize(authored.len(), None<usize>());
    for (usize index {}; index < authored.len(); ++index) {
        const auto& binding = *authored[index].binding;
        const auto& curve   = *binding.animation;
        if (curve.options.parent.is_some()) {
            Option<usize> parent;
            usize         named_matches {};
            usize         reciprocal_matches {};
            usize         compatible_matches {};
            for (usize candidate {}; candidate < authored.len(); ++candidate) {
                const auto& parent_binding = *authored[candidate].binding;
                const auto& parent_curve   = *parent_binding.animation;
                if (parent_binding.field != curve.options.parent->as_str()) continue;
                ++named_matches;
                if (! HasChild(parent_curve, binding.field.as_str())) continue;
                ++reciprocal_matches;
                if (! CompatibleTimeline(curve, parent_curve)) continue;
                parent = Some(candidate);
                ++compatible_matches;
            }
            if (compatible_matches == usize(1)) {
                selected_parents[index] = parent;
            } else if (named_matches == usize()) {
                report(SceneAnimationBindingIssue::MissingRelation,
                       binding,
                       curve.options.parent->as_str());
                rstd_warn("animation parent '{}' for '{}' is missing or incompatible in its scope",
                          *curve.options.parent,
                          binding.field);
            } else if (reciprocal_matches == usize()) {
                report(SceneAnimationBindingIssue::NonReciprocalRelation,
                       binding,
                       curve.options.parent->as_str());
                rstd_warn("animation parent '{}' for '{}' is non-reciprocal in its scope",
                          *curve.options.parent,
                          binding.field);
            } else if (compatible_matches == usize()) {
                report(SceneAnimationBindingIssue::MetadataMismatch,
                       binding,
                       curve.options.parent->as_str());
                rstd_warn("animation parent '{}' for '{}' has incompatible timeline metadata",
                          *curve.options.parent,
                          binding.field);
            } else {
                report(SceneAnimationBindingIssue::AmbiguousTarget,
                       binding,
                       curve.options.parent->as_str());
                rstd_warn("animation parent '{}' for '{}' is ambiguous in its scope",
                          *curve.options.parent,
                          binding.field);
            }
        }
        for (const auto& child_name : curve.options.children) {
            usize named_matches {};
            usize reciprocal_matches {};
            usize compatible_matches {};
            for (usize candidate {}; candidate < authored.len(); ++candidate) {
                const auto& child_binding = *authored[candidate].binding;
                const auto& child_curve   = *child_binding.animation;
                if (child_binding.field != child_name.as_str()) continue;
                ++named_matches;
                if (child_curve.options.parent.is_none() ||
                    *child_curve.options.parent != binding.field.as_str())
                    continue;
                ++reciprocal_matches;
                if (CompatibleTimeline(curve, child_curve)) ++compatible_matches;
            }
            if (named_matches == usize()) {
                report(SceneAnimationBindingIssue::MissingRelation, binding, child_name.as_str());
                rstd_warn("animation child '{}' for '{}' is missing or non-reciprocal in its scope",
                          child_name,
                          binding.field);
            } else if (reciprocal_matches == usize()) {
                report(SceneAnimationBindingIssue::NonReciprocalRelation,
                       binding,
                       child_name.as_str());
                rstd_warn("animation child '{}' for '{}' is non-reciprocal in its scope",
                          child_name,
                          binding.field);
            } else if (compatible_matches == usize()) {
                report(SceneAnimationBindingIssue::MetadataMismatch, binding, child_name.as_str());
                rstd_warn("animation child '{}' for '{}' has incompatible timeline metadata",
                          child_name,
                          binding.field);
            } else if (compatible_matches > usize(1)) {
                report(SceneAnimationBindingIssue::AmbiguousTarget, binding, child_name.as_str());
                rstd_warn("animation child '{}' for '{}' is ambiguous in its scope",
                          child_name,
                          binding.field);
            }
        }
    }

    Vec<u8> states;
    states.resize(authored.len(), u8());
    HashSet<usize> cycle_nodes;
    for (usize start {}; start < authored.len(); ++start) {
        if (states[start] != u8()) continue;
        Vec<usize> path;
        usize      current = start;
        bool       reached_root { false };
        while (states[current] == u8()) {
            states[current] = u8(1);
            path.push(usize(current.to_primitive()));
            if (selected_parents[current].is_none()) {
                reached_root = true;
                break;
            }
            current = *selected_parents[current];
        }
        if (! reached_root && states[current] == u8(1)) {
            bool in_cycle = false;
            for (usize member : path) {
                if (member == current) in_cycle = true;
                if (! in_cycle) continue;
                cycle_nodes.insert(usize(member.to_primitive()));
                const auto& binding = *authored[member].binding;
                report(SceneAnimationBindingIssue::Cycle,
                       binding,
                       binding.animation->options.parent.is_some()
                           ? binding.animation->options.parent->as_str()
                           : binding.field.as_str());
            }
        }
        for (usize member : path) states[member] = u8(2);
    }

    for (usize index {}; index < selected_parents.len(); ++index) {
        if (selected_parents[index].is_none() || cycle_nodes.contains(index) ||
            cycle_nodes.contains(*selected_parents[index]))
            continue;
        unite(index, *selected_parents[index]);
    }

    HashMap<usize, Arc<SceneAnimationPlayback>> playbacks;
    HashMap<String, usize>                      named_components;
    for (usize index {}; index < authored.len(); ++index) {
        usize component = root(index);
        if (playbacks.contains_key(component)) continue;

        usize timeline = component;
        i32   timeline_score { -1 };
        i32   end {};
        for (usize candidate {}; candidate < authored.len(); ++candidate) {
            if (root(candidate) != component) continue;
            const auto& curve = *authored[candidate].binding->animation;
            end = std::max(end,
                           std::max(curve.options.length, authored[candidate].curve->EndFrame()));
            i32 score {};
            if (curve.options.parent.is_none()) score += i32(4);
            if (! curve.options.name.empty() || ! curve.options.events.empty()) score += i32(2);
            if (! curve.options.children.is_empty()) score += i32(1);
            if (score > timeline_score) {
                timeline       = candidate;
                timeline_score = score;
            }
        }
        const auto& authored_timeline = *authored[timeline].binding->animation;
        auto        clip              = BuildSceneAnimationClip(authored_timeline, end);
        if (! clip->Name().is_empty()) {
            auto existing = named_components.get(clip->Name());
            if (existing.is_some() && **existing != component) {
                report(SceneAnimationBindingIssue::DuplicateName,
                       *authored[timeline].binding,
                       clip->Name());
            } else if (existing.is_none()) {
                (void)named_components.insert(String::make(clip->Name()), component);
            }
        }
        (void)playbacks.insert(component,
                               Arc<SceneAnimationPlayback>::make(
                                   rstd::move(clip), authored_timeline.options.startpaused));
    }

    for (usize index {}; index < authored.len(); ++index) {
        auto playback = playbacks.get(root(index));
        if (playback.is_none()) continue;
        scope.Insert(authored[index].binding->identity,
                     SceneAnimationTrack { .curve    = authored[index].curve.clone(),
                                           .playback = (**playback).clone() });
    }
}

auto ResolveFieldAnimations(const SceneParseContext&      context,
                            const wpscene::FieldBindings& bindings) -> ResolvedFieldAnimations {
    ResolvedFieldAnimations parsed;
    for (const auto& binding : bindings.Entries()) {
        if (binding.animation.is_none()) continue;
        (void)parsed.tracks.insert(binding.field.clone(), ResolveAnimationTrack(context, binding));
    }
    return parsed;
}

} // namespace

auto owe::SceneAnimationBindingScope::Resolve(const wpscene::FieldBindingSpec& binding) const
    -> Option<SceneAnimationTrack> {
    auto track = m_tracks.get(binding.identity);
    return track.is_some() ? Some((**track).Share()) : None();
}

auto owe::BuildAnimationBindingScope(const wpscene::FieldBindings& bindings)
    -> SceneAnimationBindingScope {
    SceneAnimationBindingScope            scope;
    Vec<const wpscene::FieldBindingSpec*> authored;
    CollectBindings(bindings, authored);
    ResolveAnimationScope(scope, authored.as_slice());
    return scope;
}

auto owe::BuildAnimationBindingScope(const wpscene::ImageObject& object)
    -> SceneAnimationBindingScope {
    SceneAnimationBindingScope            scope;
    Vec<const wpscene::FieldBindingSpec*> authored;
    CollectBindings(object.field_bindings, authored);
    CollectMaterialBindings(object.material, authored);
    for (const auto& effect : object.effects) CollectEffectBindings(effect, authored);
    ResolveAnimationScope(scope, authored.as_slice());
    return scope;
}

void owe::PrepareAnimationBindings(SceneParseContext&            context,
                                   const wpscene::FieldBindings& bindings) {
    context.animation_bindings = BuildAnimationBindingScope(bindings);
}

void owe::PrepareAnimationBindings(SceneParseContext&              context,
                                   const wpscene::ContainerObject& object) {
    PrepareAnimationBindings(context, object.field_bindings);
}

void owe::PrepareAnimationBindings(SceneParseContext& context, const wpscene::ImageObject& object) {
    context.animation_bindings = BuildAnimationBindingScope(object);
}

void owe::PrepareAnimationBindings(SceneParseContext& context, const wpscene::ShapeObject& object) {
    Vec<const wpscene::FieldBindingSpec*> authored;
    CollectBindings(object.field_bindings, authored);
    for (const auto& effect : object.effects) CollectEffectBindings(effect, authored);
    ResolveAnimationScope(context.animation_bindings, authored.as_slice());
}

void owe::PrepareAnimationBindings(SceneParseContext&             context,
                                   const wpscene::ParticleObject& object) {
    Vec<const wpscene::FieldBindingSpec*> authored;
    CollectBindings(object.field_bindings, authored);
    CollectParticleBindings(object.particleObj, authored);
    if (object.instanceoverride.field_bindings)
        CollectBindings(*object.instanceoverride.field_bindings, authored);
    ResolveAnimationScope(context.animation_bindings, authored.as_slice());
}

void owe::PrepareAnimationBindings(SceneParseContext& context, const wpscene::SoundObject& object) {
    PrepareAnimationBindings(context, object.field_bindings);
}

void owe::PrepareAnimationBindings(SceneParseContext& context, const wpscene::LightObject& object) {
    PrepareAnimationBindings(context, object.field_bindings);
}

void owe::PrepareAnimationBindings(SceneParseContext& context, const wpscene::TextObject& object) {
    Vec<const wpscene::FieldBindingSpec*> authored;
    CollectBindings(object.field_bindings, authored);
    for (const auto& effect : object.effects) CollectEffectBindings(effect, authored);
    ResolveAnimationScope(context.animation_bindings, authored.as_slice());
}

void owe::PrepareAnimationBindings(SceneParseContext& context, const wpscene::ModelObject& object) {
    PrepareAnimationBindings(context, object.field_bindings);
}

void owe::PrepareAnimationBindings(SceneParseContext&           context,
                                   const wpscene::CameraObject& object) {
    PrepareAnimationBindings(context, object.field_bindings);
}

auto owe::ResolveAnimationTrack(const SceneParseContext&         context,
                                const wpscene::FieldBindingSpec& binding) -> SceneAnimationTrack {
    auto track = context.animation_bindings.Resolve(binding);
    if (track.is_some()) return rstd::move(*track);
    return ToSceneAnimationTrack(*binding.animation);
}

void owe::AssignNodeFieldAnimations(SceneParseContext& context, SceneNode& node,
                                    const wpscene::FieldBindings& bindings) {
    auto parsed = ResolveFieldAnimations(context, bindings);
    parsed.Bind(node);
    auto assign = [&](ref<str> field, auto setter) {
        auto curve = parsed.Take(field);
        if (curve.is_some()) setter(rstd::move(*curve));
    };
    assign("origin"_str, [&](SceneAnimationTrack track) {
        node.SetOriginAnimation(rstd::move(track));
    });
    assign("scale"_str, [&](SceneAnimationTrack track) {
        node.SetScaleAnimation(rstd::move(track));
    });
    assign("angles"_str, [&](SceneAnimationTrack track) {
        node.SetRotationAnimation(rstd::move(track));
    });
    assign("alpha"_str, [&](SceneAnimationTrack track) {
        node.SetAlphaAnimation(rstd::move(track));
    });
}

void owe::AssignCameraFieldAnimations(SceneParseContext& context, SceneNode& node,
                                      SceneCameraPath&              path,
                                      const wpscene::FieldBindings& bindings) {
    auto parsed = ResolveFieldAnimations(context, bindings);
    parsed.Bind(node);
    auto assign = [&](Option<SceneAnimationTrack>& destination, ref<str> field) {
        auto track = parsed.Take(field);
        if (track.is_some()) destination = Some(rstd::move(*track));
    };
    assign(path.origin_track, "origin"_str);
    assign(path.rotation_track, "angles"_str);
    assign(path.zoom_track, "zoom"_str);
    assign(path.fov_track, "fov"_str);
}

void owe::LoadCameraObjectPath(SceneParseContext& context, const wpscene::CameraObject& object,
                               SceneCameraPath& path) {
    if (object.path.empty() || context.vfs == nullptr) return;

    auto relative_path = "/assets/" + object.path;
    auto file          = fs::OpenBinary(*context.vfs, relative_path);
    if (file.is_err()) {
        rstd_warn("Can't open camera path {}", object.path);
        return;
    }
    auto parsed = ParseJson(file->ReadAllStr());
    if (parsed.is_err()) {
        rstd_warn("Can't parse camera path json {}: {}", object.path, parsed.unwrap_err());
        return;
    }

    wpscene::CameraPathDocument document;
    if (! document.FromJson(parsed.unwrap())) {
        rstd_warn("Invalid camera path document {}", object.path);
        return;
    }

    path.queue_mode = object.queuemode == "random" ? SceneCameraPathQueueMode::Random
                                                   : SceneCameraPathQueueMode::Sequential;
    for (const auto& authored : document.paths) {
        if (! authored.visible) continue;
        SceneCameraPathClip clip {
            .id     = authored.id,
            .fps    = authored.options.fps > 0.0f ? authored.options.fps : 30.0f,
            .length = authored.options.length,
        };
        auto assign = [&](Option<Arc<SceneAnimationCurve>>& destination,
                          const Option<wpscene::AnimCurve>& source) {
            if (source.is_none()) return;
            auto curve  = Arc<SceneAnimationCurve>::make(ToSceneAnimationCurve(*source));
            clip.length = std::max(clip.length, curve->EndFrame());
            destination = Some(rstd::move(curve));
        };
        assign(clip.eye, authored.eye);
        assign(clip.center, authored.center);
        assign(clip.up, authored.up);
        assign(clip.fov, authored.fov);
        assign(clip.zoom, authored.zoom);
        if (clip.length > i32()) path.queue.push(rstd::move(clip));
    }
}

auto owe::ToSceneAnimationCurve(const wpscene::AnimCurve& curve) -> SceneAnimationCurve {
    return BuildSceneAnimationCurve(curve);
}

auto owe::ToSceneAnimationTrack(const wpscene::AnimCurve& authored) -> SceneAnimationTrack {
    auto curve = Arc<SceneAnimationCurve>::make(BuildSceneAnimationCurve(authored));
    auto clip =
        BuildSceneAnimationClip(authored, std::max(authored.options.length, curve->EndFrame()));
    auto playback =
        Arc<SceneAnimationPlayback>::make(rstd::move(clip), authored.options.startpaused);
    return SceneAnimationTrack { .curve = rstd::move(curve), .playback = rstd::move(playback) };
}

void owe::LoadRootCameraPaths(SceneParseContext& context, const wpscene::SceneMetadata& metadata) {
    if (metadata.general.isOrtho || metadata.camera.paths.empty() || context.vfs == nullptr) return;

    auto camera = context.scene->CameraHandle("global_perspective"_str);
    if (camera.is_none()) return;
    auto path               = Arc<SceneCameraPath>::make();
    path->camera_name       = String::make("global_perspective"_str);
    path->camera            = Some(rstd::move(*camera));
    path->node              = context.global_perspective_camera_node.is_some()
                                  ? (*context.global_perspective_camera_node).as_ptr()
                                  : nullptr;
    path->default_translate = path->node ? path->node->Translate() : Vector3f::Zero();
    path->default_rotation  = path->node ? path->node->Rotation() : Vector3f::Zero();
    path->default_width     = (**path->camera).Width();
    path->default_height    = (**path->camera).Height();
    path->default_fov       = (**path->camera).Fov();
    path->fov_base          = static_cast<float>((**path->camera).Fov());
    path->perspective       = true;
    path->enabled           = true;
    path->default_lookat    = true;
    path->default_eye       = Vector3f(metadata.camera.eye.data());
    path->default_center    = Vector3f(metadata.camera.center.data());
    path->default_up        = Vector3f(metadata.camera.up.data());

    for (const auto& relative_path : metadata.camera.paths) {
        auto file = fs::OpenBinary(*context.vfs, "/assets/" + relative_path);
        if (file.is_err()) continue;
        auto parsed = ParseJson(file->ReadAllStr());
        if (parsed.is_err()) {
            rstd_warn("Can't parse camera path json {}: {}", relative_path, parsed.unwrap_err());
            continue;
        }
        auto json   = parsed.unwrap();
        auto tracks = json.get("paths"_str);
        if (tracks.is_none()) continue;
        auto values = (*tracks)->as_array();
        if (values.is_none()) continue;
        for (const auto& raw_track : **values) {
            auto track = ParseLookAtTrack(raw_track);
            if (track.is_some()) path->lookat_tracks.push(rstd::move(*track));
        }
    }
    if (! path->lookat_tracks.is_empty()) context.scene->RegisterCameraPath(rstd::move(path));
}
