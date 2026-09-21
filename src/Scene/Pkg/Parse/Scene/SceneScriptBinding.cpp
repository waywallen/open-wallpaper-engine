module;

#include <rstd/enum.hpp>

module wescene.pkg.parse;
import :scene_context;
import eigen;
import wescene.pkg.spec_names;
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

auto LoadJsonFile(fs::VFS& vfs, ref<str> path) -> Option<Json> {
    auto parsed = owe::ReadJsonFile(vfs, fs::Path(path));
    if (parsed.is_err()) {
        auto error = rstd::move(parsed).unwrap_err_unchecked();
        rstd_error("Can't load json {}: {}", path, error.message.as_str());
        return None();
    }
    return Some(rstd::move(parsed).unwrap_unchecked());
}

bool SourceWritesLayerText(ref<str> src) {
    const bool writes_text =
        src.contains(".text"_str) || src.contains("[\"text\"]"_str) || src.contains("['text']"_str);
    if (! writes_text) return false;
    return src.contains("getLayer"_str);
}

bool FieldBindingsWriteLayerText(const wpscene::FieldBindings& fb) {
    for (const auto& binding : fb.Entries()) {
        if (binding.script.is_some() && SourceWritesLayerText(binding.script->source.as_str()))
            return true;
    }
    return false;
}

const wpscene::FieldBindings& SceneObjectFieldBindings(const SceneObjectVar& object) {
    RSTD_MATCH(object) {
        RSTD_CASE(Container, value) { return value.field_bindings; }
        RSTD_CASE(Image, value) { return value.field_bindings; }
        RSTD_CASE(Shape, value) { return value.field_bindings; }
        RSTD_CASE(Particle, value) { return value.field_bindings; }
        RSTD_CASE(Sound, value) { return value.field_bindings; }
        RSTD_CASE(Light, value) { return value.field_bindings; }
        RSTD_CASE(Text, value) { return value.field_bindings; }
        RSTD_CASE(Model, value) { return value.field_bindings; }
        RSTD_CASE(Camera, value) { return value.field_bindings; }
    }
    rstd::unreachable();
}

bool SceneWritesLayerText(slice<SceneObjectVar> scene_objs) {
    for (usize index {}; index < scene_objs.len(); ++index) {
        if (FieldBindingsWriteLayerText(SceneObjectFieldBindings(scene_objs[index]))) return true;
    }
    return false;
}

bool SceneHasScripts(slice<SceneObjectVar> scene_objs) {
    for (usize index {}; index < scene_objs.len(); ++index) {
        for (const auto& binding : SceneObjectFieldBindings(scene_objs[index]).Entries()) {
            if (binding.script.is_some()) return true;
        }
        const auto&                          object = scene_objs[index];
        slice<wpscene::PuppetAnimationLayer> layers;
        if (object.is_Image()) layers = object.as_Image().value.puppet_layers.as_slice();
        if (object.is_Model()) layers = object.as_Model().value.puppet_layers.as_slice();
        for (const auto& layer : layers) {
            for (const auto& binding : layer.field_bindings.Entries()) {
                if (binding.script.is_some()) return true;
            }
        }
    }
    return false;
}

bool AppendLayerCompositePassthroughEffect(fs::VFS& vfs, wpscene::ImageObject& image) {
    wpscene::Material material;
    auto              json = LoadJsonFile(vfs, "/assets/materials/util/effectpassthrough.json"_str);
    if (! json || ! material.FromJson(*json)) {
        rstd_error("parse effectpassthrough.json failed for '{}'", image.name);
        return false;
    }

    wpscene::ImageEffect effect;
    effect.name    = "linked layer composite"_Str;
    effect.visible = true;
    effect.materials.push(rstd::move(material));
    image.effects.push(rstd::move(effect));
    return true;
}

Arc<PuppetLayer> MakePuppetLayer(Arc<Puppet> puppet, slice<wpscene::PuppetAnimationLayer> layers) {
    auto                             out = Arc<PuppetLayer>::make(rstd::move(puppet));
    Vec<PuppetLayer::AnimationLayer> playback_layers;
    for (const auto& layer : layers) playback_layers.push(layer.playback.Clone());
    out->prepared(playback_layers.as_slice());
    return out;
}

void RegisterPuppetLayer(SceneParseContext& context, SceneNode* node, Arc<PuppetLayer> layer) {
    if (! node) return;
    for (const auto& playback : layer->AnimationPlaybacks())
        node->RegisterAnimation(playback.clone());
    (void)context.puppet_layers->by_node.insert(node, rstd::move(layer));
}

Option<Arc<PuppetLayer>> LookupPuppetLayer(const Arc<PuppetLayerRegistry>& layers,
                                           SceneNode*                      node) {
    if (! node) return None();
    if (auto layer = layers->by_node.get(node); layer.is_some()) return Some((**layer).clone());
    if (auto fallback = layers->fallback_by_node.get(node); fallback.is_some()) {
        return Some((**fallback).clone());
    }
    return None();
}

SceneNode* RootOf(SceneNode* node) {
    if (! node) return nullptr;
    while (node->Parent()) node = node->Parent();
    return node;
}

void MarkHiddenLinkSource(SceneParseContext& context, i32 id) {
    if (context.hidden_link_source_ids.contains(i32(id)))
        context.scene->MarkLayerVisibilityElidable(WallpaperLayerId { .value = i32(id) });
}

SceneUserVisibilityBinding
ToSceneUserVisibilityBinding(const wpscene::VisibleUserBinding& binding) {
    SceneUserVisibilityBinding out;
    out.key           = binding.name.clone();
    out.condition     = binding.condition.clone();
    out.has_condition = binding.has_condition;
    return out;
}

array<float, 2> Texture0UvScale(const SceneMaterial& material, bool nopadding) {
    if (nopadding) return { 1.0f, 1.0f };
    auto it = material.customShader.constValues.get(WE_GLTEX_RESOLUTION_NAMES[usize()]);
    if (it.is_none()) return { 1.0f, 1.0f };
    const auto& r = **it;
    if (r.size() < usize(4) || r[usize(0)] == 0.0f || r[usize(1)] == 0.0f) {
        return { 1.0f, 1.0f };
    }
    return { r[usize(2)] / r[usize(0)], r[usize(3)] / r[usize(1)] };
}

void InstallImageAlignmentBinding(script::JsRuntime& runtime, SceneNode* node, ref<str> alignment,
                                  const SceneParseContext::ImageAlignmentSetter& setter) {
    runtime.RegisterImageAlignmentSetter(node, alignment, setter.clone());
}

void RegisterImageAlignmentBinding(SceneParseContext& context, SceneNode* node, ref<str> alignment,
                                   SceneParseContext::ImageAlignmentSetter setter) {
    if (context.script_scene.is_some()) {
        InstallImageAlignmentBinding((*context.script_scene)->runtime(), node, alignment, setter);
    }
    context.image_alignment_bindings.push(SceneParseContext::ImageAlignmentBinding {
        .node      = node,
        .alignment = String::make(alignment),
        .setter    = rstd::move(setter),
    });
}

Option<Arc<PuppetLayer>> FindPuppetLayerWithBone(const Arc<PuppetLayerRegistry>& layers,
                                                 SceneNode* node, ref<str> name,
                                                 rstd::uint32_t& index) {
    if (! node) return None();
    if (auto layer = layers->by_node.get(node); layer.is_some()) {
        index = (**layer)->boneIndex(name);
        if (index != 0) return Some((**layer).clone());
    }
    for (auto& child : node->GetChildren()) {
        auto hit = FindPuppetLayerWithBone(layers, child.as_ptr(), name, index);
        if (hit.is_some()) return hit;
    }
    return None();
}

script::ScriptScene& EnsureScriptScene(SceneParseContext& context) {
    if (context.script_scene.is_none()) {
        context.script_scene =
            Some(Box<script::ScriptScene>::make(Some(context.audio_response_demand.clone())));
        auto layers = context.puppet_layers.clone();
        (*context.script_scene)
            ->runtime()
            .SetBoneResolvers(
                script::JsRuntime::BoneIndexResolver::make(
                    [layers = layers.clone()](SceneNode* node, ref<str> name) -> rstd::uint32_t {
                        auto           layer = LookupPuppetLayer(layers, node);
                        rstd::uint32_t index = layer.is_some() ? (*layer)->boneIndex(name) : 0;
                        if (index != 0) return index;

                        if (auto fallback =
                                FindPuppetLayerWithBone(layers, RootOf(node), name, index);
                            fallback.is_some()) {
                            (void)layers->fallback_by_node.insert(node, rstd::move(*fallback));
                            return index;
                        }
                        return 0;
                    }),
                script::JsRuntime::BoneTransformResolver::make(
                    [layers = layers.clone()](SceneNode*     node,
                                              rstd::uint32_t index,
                                              double time) -> Option<script::BoneTranslation> {
                        auto layer = LookupPuppetLayer(layers, node);
                        if (layer.is_none()) return None();
                        auto bone = (*layer)->boneTransform(index, time);
                        if (bone.is_none()) return None();

                        node->UpdateTrans();
                        Eigen::Affine3f world = Eigen::Affine3f::Identity();
                        world.matrix()        = node->ModelTrans().cast<float>();
                        Eigen::Vector3f t     = (world * *bone).translation();
                        return Some(script::BoneTranslation { t.x(), t.y(), t.z() });
                    }));
        if (context.user_properties.is_some())
            (*context.user_properties)->iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                auto key                      = entry_key->as_str();
                (*context.script_scene)->runtime().SetUserProperty(key, *entry_value);
            });
        for (const auto& binding : context.image_alignment_bindings) {
            InstallImageAlignmentBinding((*context.script_scene)->runtime(),
                                         binding.node,
                                         binding.alignment.as_str(),
                                         binding.setter);
        }
    }
    return **context.script_scene;
}

void SetScriptInitializationOrder(SceneParseContext& context, script::FieldScript& script,
                                  const SceneNode* node) {
    if (node == nullptr) return;
    auto order = context.script_initialization_orders.get(node->ID());
    if (order.is_none()) return;
    EnsureScriptScene(context).runtime().SetInitializationOrder(script, **order);
}

void TrackRegisteredAssets(SceneParseContext& context, script::FieldScript* script) {
    if (script && ! script->RegisteredAssets().is_empty())
        context.registered_asset_scripts.push(rstd::move(script));
}

Option<float> ScriptValueAsFloat(const script::ScriptValue& value) {
    if (auto* p = (value.is_Scalar() ? &value.as_Scalar().value : nullptr))
        return Some(static_cast<float>(p->v));
    if (auto* p = (value.is_Bool() ? &value.as_Bool().value : nullptr))
        return Some(p->v ? 1.0f : 0.0f);
    if (auto* p = (value.is_Vec2() ? &value.as_Vec2().value : nullptr))
        return Some(static_cast<float>(p->x));
    if (auto* p = (value.is_Vec3() ? &value.as_Vec3().value : nullptr))
        return Some(static_cast<float>(p->x));
    return None();
}

void WirePuppetAnimationScripts(SceneParseContext& context, SceneNode* node,
                                Arc<PuppetLayer>                     puppet,
                                slice<wpscene::PuppetAnimationLayer> layers) {
    for (const auto& layer : layers) {
        auto binding = layer.field_bindings.Get("visible"_str);
        if (binding.is_none() || (**binding).script.is_none()) continue;
        auto playback = puppet->AnimationPlayback(i32(layer.playback.layer_id));
        if (playback.is_none()) continue;
        const auto& spec    = **binding;
        const auto& source  = *spec.script;
        auto&       scripts = EnsureScriptScene(context);
        auto*       field   = scripts.runtime().MakeFieldScript(
            source.source.as_str(),
            utils::genSha1(rstd::as_bytes(source.source.as_str().as_bytes())).as_str(),
            script::FieldKind::Bool,
            spec.ScriptProperties(),
            source.initial_value,
            script::ScriptBindingContext::ForAnimationLayer(
                node, spec.field.as_str(), rstd::move(*playback)));
        if (! field) continue;
        SetScriptInitializationOrder(context, *field, node);
        TrackRegisteredAssets(context, field);
        auto owner = puppet.clone();
        scripts.AddActuator({ field,
                              [owner = owner.clone(), id = i32(layer.playback.layer_id)](
                                  const script::ScriptValue& value) {
                                  if (auto visible = ScriptValueAsFloat(value); visible.is_some())
                                      owner->SetAnimationVisible(id, *visible >= 0.5f);
                              } });
    }
}

Option<array<float, 2>> ScriptValueAsVec2(const script::ScriptValue& value) {
    auto* vector = (value.is_Vec2() ? &value.as_Vec2().value : nullptr);
    if (vector == nullptr) return None();
    return Some(array<float, 2> {
        static_cast<float>(vector->x),
        static_cast<float>(vector->y),
    });
}

Option<Vector3f> ScriptValueAsVec3(const script::ScriptValue& value, const Vector3f& current) {
    Vector3f next = current;
    if (auto* p = (value.is_Vec3() ? &value.as_Vec3().value : nullptr)) {
        next = Vector3f { static_cast<float>(p->x),
                          static_cast<float>(p->y),
                          static_cast<float>(p->z) };
    } else if (auto* p = (value.is_Vec2() ? &value.as_Vec2().value : nullptr)) {
        next = Vector3f { static_cast<float>(p->x), static_cast<float>(p->y), current.z() };
    } else if (auto* p = (value.is_Scalar() ? &value.as_Scalar().value : nullptr)) {
        next.x() = static_cast<float>(p->v);
    } else
        return None();
    return Some(next);
}

Json ScriptInitialValueForField(ref<str> field, const Json& value) {
    if (field != "angles"_str) return value.clone();

    constexpr float kRadToDeg = 180.0f / f32::consts::PI.to_primitive();
    if (value.is_null()) return Json::Null();
    if (value.is_number()) {
        auto number = value.as_f64();
        return number.is_some() && number->to_primitive() >= f32::MIN.to_primitive() &&
                       number->to_primitive() <= f32::MAX.to_primitive()
                   ? rstd::into<Json>(f32(static_cast<float>(number->to_primitive()) * kRadToDeg))
                   : Json::Null();
    }

    if (value.is_object()) {
        auto out = value.clone();
        for (auto axis : rstd::array<ref<str>, 3> { "x"_str, "y"_str, "z"_str }) {
            auto member = out.get_mut(axis);
            if (member.is_none()) continue;
            auto number = (*member)->as_f64();
            if (number.is_some() && number->to_primitive() >= f32::MIN.to_primitive() &&
                number->to_primitive() <= f32::MAX.to_primitive()) {
                **member =
                    rstd::into<Json>(f32(static_cast<float>(number->to_primitive()) * kRadToDeg));
            }
        }
        return out;
    }

    Vec<float> values;
    if (owe::GetJsonValue(value, values) && ! values.is_empty()) {
        for (auto& axis : values) axis *= kRadToDeg;
        auto out = rstd::json::Array::make();
        for (float axis : values) out.push(rstd::into<Json>(f32(axis)));
        return Json::Array(rstd::move(out));
    }

    return value.clone();
}

} // namespace owe

namespace owe
{

void WireFieldScripts(SceneParseContext& context, const Arc<SceneNode>& node_sp,
                      const wpscene::FieldBindings&                             fb,
                      Option<Arc<dyn<FnMut<void(const script::ScriptValue&)>>>> origin_apply,
                      Option<Arc<dyn<FnMut<void(const script::ScriptValue&)>>>> scale_apply) {
    SceneNode* node = node_sp.as_ptr();

    auto parallax_binding = fb.Get("parallaxDepth"_str);
    if (parallax_binding.is_some() && (**parallax_binding).user.is_some() && node->ID() != i32() &&
        ! context.parallax_depth_user_binding_ids.contains(node->ID())) {
        context.parallax_depth_user_binding_ids.insert(node->ID());
        auto state = context.uniform_state.clone();
        context.scene->RegisterUserPropertyBinding(
            (**parallax_binding).user->clone(),
            Box<dyn<FnMut<void(ref<Json>)>>>::make(
                [state = state.clone(), object_id = node->ID()](ref<Json> property) mutable {
                    (void)state->ApplyObjectParallaxDepth(object_id, *property);
                }));
    }
    auto& ss = EnsureScriptScene(context);
    auto& rt = ss.runtime();

    for (const auto& binding : fb.Entries()) {
        if (binding.script.is_none()) continue;
        const auto&                 sb    = *binding.script;
        auto                        field = binding.field.as_str();
        script::NodeTransformTarget tgt   = script::NodeTransformTarget::Translate;
        script::FieldKind           kind;
        bool                        has_actuator = true;
        bool                        is_alpha     = false;
        bool                        is_color     = false;
        bool                        is_volume    = false;
        bool                        is_parallax  = false;
        if (field == "origin"_str) {
            tgt  = script::NodeTransformTarget::Translate;
            kind = script::FieldKind::Vec3;
        } else if (field == "scale"_str) {
            tgt  = script::NodeTransformTarget::Scale;
            kind = script::FieldKind::Vec3;
        } else if (field == "angles"_str) {
            tgt  = script::NodeTransformTarget::Rotation;
            kind = script::FieldKind::Vec3;
        } else if (field == "visible"_str) {
            // Side-effect-only script bound to visibility. update() may
            // drive other layers via createLayer + property writes; we
            // don't write a return value back to the node.
            kind         = script::FieldKind::Bool;
            has_actuator = false;
        } else if (field == "alpha"_str) {
            kind     = script::FieldKind::Scalar;
            is_alpha = true;
        } else if (field == "color"_str) {
            kind     = script::FieldKind::Vec3;
            is_color = true;
        } else if (field == "volume"_str) {
            kind      = script::FieldKind::Scalar;
            is_volume = true;
        } else if (field == "parallaxDepth"_str) {
            kind        = script::FieldKind::Vec2;
            is_parallax = true;
        } else {
            // text/rate/intensity/... are wired elsewhere or not yet supported.
            continue;
        }
        auto sha           = utils::genSha1(rstd::as_bytes(sb.source.as_str().as_bytes()));
        auto initial_value = ScriptInitialValueForField(field, sb.initial_value);
        Option<Arc<SceneAnimationPlayback>> animation;
        if (binding.animation.is_some())
            animation = Some(ResolveAnimationTrack(context, binding).playback.clone());
        auto* fs = rt.MakeFieldScript(sb.source.as_str(),
                                      sha.as_str(),
                                      kind,
                                      binding.ScriptProperties(),
                                      initial_value,
                                      script::ScriptBindingContext::ForLayer(
                                          node, binding.field.as_str(), rstd::move(animation)));
        if (! fs) continue;
        SetScriptInitializationOrder(context, *fs, node);
        TrackRegisteredAssets(context, fs);
        if (! has_actuator) continue;
        if (is_alpha)
            ss.AddActuator({ fs, script::MakeNodeAlphaApply(node_sp.clone()) });
        else if (is_color)
            ss.AddActuator({ fs, script::MakeNodeColorApply(node_sp.clone()) });
        else if (is_volume)
            ss.AddActuator({ fs, script::MakeNodeVolumeApply(node_sp.clone()) });
        else if (is_parallax) {
            auto state = context.uniform_state.clone();
            ss.AddActuator({ fs,
                             [state     = state.clone(),
                              object_id = node->ID()](const script::ScriptValue& value) mutable {
                                 auto depth = ScriptValueAsVec2(value);
                                 if (depth.is_some())
                                     (void)state->SetObjectParallaxDepth(object_id, *depth);
                             } });
        } else if (field == "origin"_str && origin_apply)
            ss.AddActuator({ fs, [apply = origin_apply->clone()](const script::ScriptValue& value) {
                                apply->operator()(value);
                            } });
        else if (field == "scale"_str && scale_apply)
            ss.AddActuator({ fs, [apply = scale_apply->clone()](const script::ScriptValue& value) {
                                apply->operator()(value);
                            } });
        else
            ss.AddActuator({ fs, script::MakeNodeTransformApply(node_sp.clone(), tgt) });
    }
}

// An effect's `visible` can be driven by a script, the same way a layer's
// can. The effect is already registered with the scene, so the actuator
// only has to flip its runtime visibility; the render graph rebuild is
// handled by Scene::SetImageEffectRuntimeVisible.
void WireImageEffectVisibilityScript(SceneParseContext& context, SceneNode* node,
                                     const wpscene::ImageEffect& effect, SceneEffectId effect_id) {
    const auto* binding = effect.visible_binding();
    if (binding == nullptr || binding->script.is_none() || ! effect_id.Valid()) return;
    const auto& sb = *binding->script;

    auto& ss  = EnsureScriptScene(context);
    auto& rt  = ss.runtime();
    auto  sha = utils::genSha1(rstd::as_bytes(sb.source.as_str().as_bytes()));
    Option<Arc<SceneAnimationPlayback>> animation;
    if (binding->animation.is_some()) {
        auto track = ResolveAnimationTrack(context, *binding);
        node->RegisterAnimation(track.playback.clone());
        animation = Some(rstd::move(track.playback));
    }
    auto* fs =
        rt.MakeFieldScript(sb.source.as_str(),
                           sha.as_str(),
                           script::FieldKind::Bool,
                           binding->ScriptProperties(),
                           sb.initial_value,
                           script::ScriptBindingContext::ForEffect(
                               node, { .id = effect_id }, "visible"_str, rstd::move(animation)));
    if (! fs) return;
    SetScriptInitializationOrder(context, *fs, node);
    TrackRegisteredAssets(context, fs);

    auto* scene = context.scene.get();
    ss.AddActuator({ fs, [scene, effect_id](const script::ScriptValue& value) {
                        auto flag = ScriptValueAsFloat(value);
                        if (! flag) return;
                        (void)scene->SetImageEffectRuntimeVisible({ .id = effect_id },
                                                                  *flag >= 0.5f);
                    } });
}

void WireCameraShakeScripts(SceneParseContext& context, const wpscene::FieldBindings& fb) {
    auto& ss = EnsureScriptScene(context);
    auto& rt = ss.runtime();

    for (const auto& binding : fb.Entries()) {
        if (binding.script.is_none()) continue;
        const auto&       sb    = *binding.script;
        auto              field = binding.field.as_str();
        script::FieldKind kind  = script::FieldKind::Scalar;
        if (field == "camerashake"_str) {
            kind = script::FieldKind::Bool;
        } else if (field != "camerashakeamplitude"_str && field != "camerashakespeed"_str &&
                   field != "camerashakeroughness"_str) {
            continue;
        }

        auto sha = utils::genSha1(rstd::as_bytes(sb.source.as_str().as_bytes()));
        Option<Arc<SceneAnimationPlayback>> animation;
        if (binding.animation.is_some()) {
            auto track = ResolveAnimationTrack(context, binding);
            if (context.global_camera_node.is_some())
                (**context.global_camera_node).RegisterAnimation(track.playback.clone());
            animation = Some(rstd::move(track.playback));
        }
        auto* fs = rt.MakeFieldScript(sb.source.as_str(),
                                      sha.as_str(),
                                      kind,
                                      binding.ScriptProperties(),
                                      sb.initial_value,
                                      script::ScriptBindingContext::ForLayer(
                                          nullptr, binding.field.as_str(), rstd::move(animation)));
        if (! fs) continue;
        TrackRegisteredAssets(context, fs);

        auto state      = context.uniform_state.clone();
        auto field_name = binding.field.clone();
        ss.AddActuator({ fs,
                         [state = state.clone(), field_name = rstd::move(field_name)](
                             const script::ScriptValue& value) mutable {
                             auto scalar = ScriptValueAsFloat(value);
                             if (! scalar) return;
                             auto& shake = state->CameraShake();
                             if (field_name == "camerashake"_str)
                                 shake.enable = *scalar >= 0.5f;
                             else if (field_name == "camerashakeamplitude"_str)
                                 shake.amplitude = *scalar;
                             else if (field_name == "camerashakespeed"_str)
                                 shake.speed = *scalar;
                             else if (field_name == "camerashakeroughness"_str)
                                 shake.roughness = *scalar;
                         } });
    }
}

void WireCameraFieldScripts(SceneParseContext& context, const Arc<SceneNode>& node_sp,
                            const Arc<SceneCamera>& camera, const Arc<SceneCameraPath>& camera_path,
                            const wpscene::FieldBindings& fb, const Vector3f& translate_bias,
                            const Vector3f& rotation_bias) {
    SceneNode* node = node_sp.as_ptr();
    auto&      ss   = EnsureScriptScene(context);
    auto&      rt   = ss.runtime();

    for (const auto& binding : fb.Entries()) {
        if (binding.script.is_none()) continue;
        const auto&       sb    = *binding.script;
        auto              field = binding.field.as_str();
        script::FieldKind kind  = script::FieldKind::Vec3;
        if (field == "visible"_str) {
            kind = script::FieldKind::Bool;
        } else if (field != "origin"_str && field != "angles"_str) {
            continue;
        }

        auto  sha           = utils::genSha1(rstd::as_bytes(sb.source.as_str().as_bytes()));
        auto  initial_value = ScriptInitialValueForField(field, sb.initial_value);
        auto* fs            = rt.MakeFieldScript(
            sb.source.as_str(),
            sha.as_str(),
            kind,
            binding.ScriptProperties(),
            initial_value,
            script::ScriptBindingContext::ForLayer(
                node, binding.field.as_str(), node->FieldAnimation(binding.field.as_str())));
        if (! fs) continue;
        SetScriptInitializationOrder(context, *fs, node);
        TrackRegisteredAssets(context, fs);

        if (field == "origin"_str) {
            auto path         = camera_path.clone();
            auto camera_owner = camera.clone();
            ss.AddActuator(
                { fs,
                  [node, camera_owner = camera_owner.clone(), path = path.clone(), translate_bias](
                      const script::ScriptValue& value) {
                      Vector3f current = path->origin_base;
                      auto     next    = ScriptValueAsVec3(value, current);
                      if (next) {
                          path->origin_base = *next;
                          node->SetTranslate(translate_bias + *next);
                          camera_owner->Update();
                      }
                  } });
        } else if (field == "angles"_str) {
            auto path         = camera_path.clone();
            auto camera_owner = camera.clone();
            ss.AddActuator(
                { fs,
                  [node, camera_owner = camera_owner.clone(), path = path.clone(), rotation_bias](
                      const script::ScriptValue& value) {
                      constexpr float kRadToDeg = 180.0f / f32::consts::PI.to_primitive();
                      constexpr float kDegToRad = f32::consts::PI.to_primitive() / 180.0f;
                      Vector3f        current   = path->rotation_base;
                      current *= kRadToDeg;
                      auto next = ScriptValueAsVec3(value, current);
                      if (next) {
                          path->rotation_base = *next * kDegToRad;
                          node->SetRotation(rotation_bias + *next * kDegToRad);
                          camera_owner->Update();
                      }
                  } });
        }
    }
}

} // namespace owe
