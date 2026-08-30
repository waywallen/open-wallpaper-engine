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

auto LoadJsonFile(fs::VFS& vfs, const std::string& path) -> Option<Json> {
    auto parsed = owe::ReadJsonFile(vfs, path);
    if (parsed.is_err()) {
        auto error = rstd::move(parsed).unwrap_err_unchecked();
        rstd_error("Can't load json {}: {}", path, error.message.as_str());
        return None();
    }
    return Some(rstd::move(parsed).unwrap_unchecked());
}

template<typename T>
struct CopyableArcHold {
    Arc<T> value;

    explicit CopyableArcHold(Arc<T> owner): value(rstd::move(owner)) {}
    CopyableArcHold(const CopyableArcHold& other): value(other.value.clone()) {}
    CopyableArcHold(CopyableArcHold&&) noexcept            = default;
    CopyableArcHold& operator=(CopyableArcHold&&) noexcept = default;
    CopyableArcHold& operator=(const CopyableArcHold&)     = delete;
};

bool SourceWritesLayerText(std::string_view src) {
    const bool writes_text = src.find(".text") != std::string_view::npos ||
                             src.find("[\"text\"]") != std::string_view::npos ||
                             src.find("['text']") != std::string_view::npos;
    if (! writes_text) return false;
    return src.find("getLayer") != std::string_view::npos;
}

bool FieldBindingsWriteLayerText(const wpscene::FieldBindings& fb) {
    for (const auto& [_, sb] : fb.scripts) {
        if (SourceWritesLayerText(sb.source)) return true;
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
        if (! SceneObjectFieldBindings(scene_objs[index]).scripts.empty()) return true;
    }
    return false;
}

bool AppendLayerCompositePassthroughEffect(fs::VFS& vfs, wpscene::ImageObject& image) {
    wpscene::Material material;
    auto              json = LoadJsonFile(vfs, "/assets/materials/util/effectpassthrough.json");
    if (! json || ! material.FromJson(*json)) {
        rstd_error("parse effectpassthrough.json failed for '{}'", image.name);
        return false;
    }

    wpscene::ImageEffect effect;
    effect.name    = "linked layer composite";
    effect.visible = true;
    effect.materials.push_back(std::move(material));
    image.effects.push_back(std::move(effect));
    return true;
}

Arc<PuppetLayer> MakePuppetLayer(Arc<Puppet>                            puppet,
                                 std::span<PuppetLayer::AnimationLayer> layers) {
    auto out = Arc<PuppetLayer>::make(rstd::move(puppet));
    out->prepared(
        slice<PuppetLayer::AnimationLayer>::from_raw_parts(layers.data(), usize(layers.size())));
    return out;
}

void RegisterPuppetLayer(SceneParseContext& context, SceneNode* node, Arc<PuppetLayer> layer) {
    if (! node) return;
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
    out.key           = String::make(rstd::cppstd::as_str(binding.name).unwrap());
    out.condition     = binding.condition.clone();
    out.has_condition = binding.has_condition;
    return out;
}

array<float, 2> Texture0UvScale(const SceneMaterial& material, bool nopadding) {
    if (nopadding) return { 1.0f, 1.0f };
    auto it = material.customShader.constValues.find(WE_GLTEX_RESOLUTION_NAMES[usize()]);
    if (it == material.customShader.constValues.end()) return { 1.0f, 1.0f };
    const auto& r = it->second;
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
                                                 SceneNode* node, std::string_view name,
                                                 std::uint32_t& index) {
    if (! node) return None();
    if (auto layer = layers->by_node.get(node); layer.is_some()) {
        index = (**layer)->boneIndex(rstd::cppstd::as_str(name).unwrap());
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
        auto layers = CopyableArcHold(context.puppet_layers.clone());
        (*context.script_scene)
            ->runtime()
            .SetBoneResolvers(
                [layers](SceneNode* node, std::string_view name) -> std::uint32_t {
                    auto          layer = LookupPuppetLayer(layers.value, node);
                    std::uint32_t index =
                        layer.is_some() ? (*layer)->boneIndex(rstd::cppstd::as_str(name).unwrap())
                                        : 0;
                    if (index != 0) return index;

                    if (auto fallback =
                            FindPuppetLayerWithBone(layers.value, RootOf(node), name, index);
                        fallback.is_some()) {
                        (void)layers.value->fallback_by_node.insert(node, rstd::move(*fallback));
                        return index;
                    }
                    return 0;
                },
                [layers](SceneNode*    node,
                         std::uint32_t index,
                         double        time) -> Option<script::BoneTranslation> {
                    auto layer = LookupPuppetLayer(layers.value, node);
                    if (layer.is_none()) return None();
                    auto bone = (*layer)->boneTransform(index, time);
                    if (bone.is_none()) return None();

                    node->UpdateTrans();
                    Eigen::Affine3f world = Eigen::Affine3f::Identity();
                    world.matrix()        = node->ModelTrans().cast<float>();
                    Eigen::Vector3f t     = (world * *bone).translation();
                    return Some(script::BoneTranslation { t.x(), t.y(), t.z() });
                });
        if (context.user_properties.is_some())
            (*context.user_properties)->iter().for_each([&](auto entry) {
                auto [entry_key, entry_value] = entry;
                auto key                      = rstd::cppstd::as_string_view(entry_key->as_str());
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
    if (auto* p = std::get_if<script::ScalarValue>(&value)) return Some(static_cast<float>(p->v));
    if (auto* p = std::get_if<script::BoolValue>(&value)) return Some(p->v ? 1.0f : 0.0f);
    if (auto* p = std::get_if<script::Vec2Value>(&value)) return Some(static_cast<float>(p->x));
    if (auto* p = std::get_if<script::Vec3Value>(&value)) return Some(static_cast<float>(p->x));
    return None();
}

Option<array<float, 2>> ScriptValueAsVec2(const script::ScriptValue& value) {
    auto* vector = std::get_if<script::Vec2Value>(&value);
    if (vector == nullptr) return None();
    return Some(array<float, 2> {
        static_cast<float>(vector->x),
        static_cast<float>(vector->y),
    });
}

Option<Vector3f> ScriptValueAsVec3(const script::ScriptValue& value, const Vector3f& current) {
    Vector3f next = current;
    if (auto* p = std::get_if<script::Vec3Value>(&value)) {
        next = Vector3f { static_cast<float>(p->x),
                          static_cast<float>(p->y),
                          static_cast<float>(p->z) };
    } else if (auto* p = std::get_if<script::Vec2Value>(&value)) {
        next = Vector3f { static_cast<float>(p->x), static_cast<float>(p->y), current.z() };
    } else if (auto* p = std::get_if<script::ScalarValue>(&value)) {
        next.x() = static_cast<float>(p->v);
    } else
        return None();
    return Some(next);
}

bool IsFractionSliderProperty(const SceneParseContext& context, const Json& binding) {
    if (context.user_properties.is_none() || ! binding.is_object()) return false;
    auto user = binding.get("user"_str);
    if (user.is_none()) return false;
    auto key = (*user)->as_str();
    if (key.is_none()) return false;
    auto prop = (*context.user_properties)->get(*key);
    if (prop.is_none() || ! (*prop)->is_object()) return false;
    auto type = (*prop)->get("type"_str);
    if (type.is_none()) return false;
    auto type_string = (*type)->as_str();
    if (type_string.is_none() || rstd::cppstd::as_string_view(*type_string) != "slider")
        return false;
    auto fraction = (*prop)->get("fraction"_str);
    return fraction.is_some() && (*fraction)->as_bool().unwrap_or(false);
}

Json ScriptPropertiesForField(const SceneParseContext& context, std::string_view field,
                              const wpscene::ScriptBinding& binding) {
    Json props = binding.properties.clone();
    if (field != "scale" || binding.source.find("/10000") == std::string::npos ||
        ! props.is_object())
        return props;

    auto object = props.as_object_mut();
    (*object)->iter_mut().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        auto& item                    = *entry_value;
        if (IsFractionSliderProperty(context, item)) {
            auto item_object = item.as_object_mut();
            (*item_object)
                ->insert(::alloc::string::String::make("__scriptValueScale"_str),
                         rstd::into<Json>(f64(50.0)));
        }
    });
    return props;
}

Json ScriptInitialValueForField(std::string_view field, const Json& value) {
    if (field != "angles") return value.clone();

    constexpr float kRadToDeg = 180.0f / rstd::f32::consts::PI.to_primitive();
    if (value.is_null()) return Json::Null();
    if (value.is_number()) {
        auto number = value.as_f64();
        return number.is_some() && number->to_primitive() >= std::numeric_limits<float>::lowest() &&
                       number->to_primitive() <= std::numeric_limits<float>::max()
                   ? rstd::into<Json>(f32(static_cast<float>(number->to_primitive()) * kRadToDeg))
                   : Json::Null();
    }

    if (value.is_object()) {
        auto out = value.clone();
        for (auto axis : rstd::array<ref<str>, 3> { "x"_str, "y"_str, "z"_str }) {
            auto member = out.get_mut(axis);
            if (member.is_none()) continue;
            auto number = (*member)->as_f64();
            if (number.is_some() &&
                number->to_primitive() >= std::numeric_limits<float>::lowest() &&
                number->to_primitive() <= std::numeric_limits<float>::max()) {
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

// Markers live on the animation, the callback lives on the script, and the
// two are joined by the field name they hang off. One timeline can also
// drive sibling fields (`options.children`) — those scripts hear it too.
void WireAnimationEventSources(script::JsRuntime& runtime, script::FieldScript& script,
                               const wpscene::FieldBindings& fb, std::string_view field) {
    for (const auto& [animated_field, curve] : fb.animations) {
        if (curve.options.events.empty()) continue;
        bool drives_field = animated_field == field;
        for (const auto& child : curve.options.children) {
            if (child == field) drives_field = true;
        }
        if (drives_field) runtime.AddAnimationEventSource(script, ToSceneAnimationCurve(curve));
    }
}

void WireFieldScripts(SceneParseContext& context, const Arc<SceneNode>& node_sp,
                      const wpscene::FieldBindings&                   fb,
                      std::function<void(const script::ScriptValue&)> origin_apply,
                      std::function<void(const script::ScriptValue&)> scale_apply) {
    SceneNode* node = node_sp.as_ptr();

    auto parallax_user = fb.users.find("parallaxDepth");
    if (parallax_user != fb.users.end() && node->ID() != i32() &&
        ! context.parallax_depth_user_binding_ids.contains(node->ID())) {
        context.parallax_depth_user_binding_ids.insert(node->ID());
        auto state = CopyableArcHold(context.uniform_state.clone());
        context.scene->RegisterUserPropertyBinding(
            String::make(as_str(parallax_user->second).unwrap()),
            Box<dyn<FnMut<void(ref<Json>)>>>::make(
                [state, object_id = node->ID()](ref<Json> property) mutable {
                    (void)state.value->ApplyObjectParallaxDepth(object_id, *property);
                }));
    }
    if (fb.scripts.empty()) return;
    auto& ss = EnsureScriptScene(context);
    auto& rt = ss.runtime();

    for (const auto& [field, sb] : fb.scripts) {
        script::NodeTransformTarget tgt = script::NodeTransformTarget::Translate;
        script::FieldKind           kind;
        bool                        has_actuator = true;
        bool                        is_alpha     = false;
        bool                        is_color     = false;
        bool                        is_volume    = false;
        bool                        is_parallax  = false;
        if (field == "origin") {
            tgt  = script::NodeTransformTarget::Translate;
            kind = script::FieldKind::Vec3;
        } else if (field == "scale") {
            tgt  = script::NodeTransformTarget::Scale;
            kind = script::FieldKind::Vec3;
        } else if (field == "angles") {
            tgt  = script::NodeTransformTarget::Rotation;
            kind = script::FieldKind::Vec3;
        } else if (field == "visible") {
            // Side-effect-only script bound to visibility. update() may
            // drive other layers via createLayer + property writes; we
            // don't write a return value back to the node.
            kind         = script::FieldKind::Bool;
            has_actuator = false;
        } else if (field == "alpha") {
            kind     = script::FieldKind::Scalar;
            is_alpha = true;
        } else if (field == "color") {
            kind     = script::FieldKind::Vec3;
            is_color = true;
        } else if (field == "volume") {
            kind      = script::FieldKind::Scalar;
            is_volume = true;
        } else if (field == "parallaxDepth") {
            kind        = script::FieldKind::Vec2;
            is_parallax = true;
        } else {
            // text/rate/intensity/... are wired elsewhere or not yet supported.
            continue;
        }
        std::string sha           = utils::genSha1(std::span<const char>(sb.source));
        auto        props         = ScriptPropertiesForField(context, field, sb);
        auto        initial_value = ScriptInitialValueForField(field, sb.initial_value);
        auto*       fs = rt.MakeFieldScript(sb.source, sha, kind, props, initial_value, node);
        if (! fs) continue;
        WireAnimationEventSources(rt, *fs, fb, field);
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
            auto state = CopyableArcHold(context.uniform_state.clone());
            ss.AddActuator(
                { fs, [state, object_id = node->ID()](const script::ScriptValue& value) mutable {
                     auto depth = ScriptValueAsVec2(value);
                     if (depth.is_some())
                         (void)state.value->SetObjectParallaxDepth(object_id, *depth);
                 } });
        } else if (field == "origin" && origin_apply)
            ss.AddActuator({ fs, origin_apply });
        else if (field == "scale" && scale_apply)
            ss.AddActuator({ fs, scale_apply });
        else
            ss.AddActuator({ fs, script::MakeNodeTransformApply(node_sp.clone(), tgt) });
    }
}

void WireCameraShakeScripts(SceneParseContext& context, const wpscene::FieldBindings& fb) {
    if (fb.scripts.empty()) return;

    auto& ss = EnsureScriptScene(context);
    auto& rt = ss.runtime();

    for (const auto& [field, sb] : fb.scripts) {
        script::FieldKind kind = script::FieldKind::Scalar;
        if (field == "camerashake") {
            kind = script::FieldKind::Bool;
        } else if (field != "camerashakeamplitude" && field != "camerashakespeed" &&
                   field != "camerashakeroughness") {
            continue;
        }

        std::string sha = utils::genSha1(std::span<const char>(sb.source));
        auto*       fs  = rt.MakeFieldScript(sb.source, sha, kind, sb.properties, sb.initial_value);
        if (! fs) continue;
        TrackRegisteredAssets(context, fs);

        auto state = mut_ref<UniformSceneState>::from_raw_parts(context.uniform_state.as_ptr());
        ss.AddActuator({ fs, [state, field](const script::ScriptValue& value) mutable {
                            auto scalar = ScriptValueAsFloat(value);
                            if (! scalar) return;
                            auto& shake = state->CameraShake();
                            if (field == "camerashake")
                                shake.enable = *scalar >= 0.5f;
                            else if (field == "camerashakeamplitude")
                                shake.amplitude = *scalar;
                            else if (field == "camerashakespeed")
                                shake.speed = *scalar;
                            else if (field == "camerashakeroughness")
                                shake.roughness = *scalar;
                        } });
    }
}

void WireCameraFieldScripts(SceneParseContext& context, const Arc<SceneNode>& node_sp,
                            const Arc<SceneCamera>& camera, const Arc<SceneCameraPath>& camera_path,
                            const wpscene::FieldBindings& fb, const Vector3f& translate_bias,
                            const Vector3f& rotation_bias) {
    SceneNode* node = node_sp.as_ptr();
    if (fb.scripts.empty()) return;
    auto& ss = EnsureScriptScene(context);
    auto& rt = ss.runtime();

    for (const auto& [field, sb] : fb.scripts) {
        script::FieldKind kind = script::FieldKind::Vec3;
        if (field == "visible") {
            kind = script::FieldKind::Bool;
        } else if (field != "origin" && field != "angles") {
            continue;
        }

        std::string sha           = utils::genSha1(std::span<const char>(sb.source));
        auto        initial_value = ScriptInitialValueForField(field, sb.initial_value);
        auto* fs = rt.MakeFieldScript(sb.source, sha, kind, sb.properties, initial_value, node);
        if (! fs) continue;
        SetScriptInitializationOrder(context, *fs, node);
        TrackRegisteredAssets(context, fs);

        if (field == "origin") {
            auto path         = CopyableArcHold(camera_path.clone());
            auto camera_owner = CopyableArcHold(camera.clone());
            ss.AddActuator(
                { fs, [node, camera_owner, path, translate_bias](const script::ScriptValue& value) {
                     Vector3f current = path.value->origin_base;
                     auto     next    = ScriptValueAsVec3(value, current);
                     if (next) {
                         path.value->origin_base = *next;
                         node->SetTranslate(translate_bias + *next);
                         camera_owner.value->Update();
                     }
                 } });
        } else if (field == "angles") {
            auto path         = CopyableArcHold(camera_path.clone());
            auto camera_owner = CopyableArcHold(camera.clone());
            ss.AddActuator(
                { fs, [node, camera_owner, path, rotation_bias](const script::ScriptValue& value) {
                     constexpr float kRadToDeg = 180.0f / rstd::f32::consts::PI.to_primitive();
                     constexpr float kDegToRad = rstd::f32::consts::PI.to_primitive() / 180.0f;
                     Vector3f        current   = path.value->rotation_base;
                     current *= kRadToDeg;
                     auto next = ScriptValueAsVec3(value, current);
                     if (next) {
                         path.value->rotation_base = *next * kDegToRad;
                         node->SetRotation(rotation_bias + *next * kDegToRad);
                         camera_owner.value->Update();
                     }
                 } });
        }
    }
}

} // namespace owe
