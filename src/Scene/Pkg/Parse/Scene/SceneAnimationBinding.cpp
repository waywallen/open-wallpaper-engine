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
    for (const auto& event : events)
        result.push(SceneAnimationEvent {
            .frame = event.frame,
            .name  = String::make(rstd::cppstd::as_str(event.name).unwrap()),
        });
    sort_unstable_by(result.as_mut_slice().as_mut_ref(),
                     [](const SceneAnimationEvent& left, const SceneAnimationEvent& right) {
                         return left.frame < right.frame;
                     });
    return result;
}

SceneAnimationCurve BuildSceneAnimationCurve(const owe::wpscene::AnimCurve& curve) {
    return SceneAnimationCurve {
        .c0          = ToSceneAnimationAxis(curve.c0),
        .c1          = ToSceneAnimationAxis(curve.c1),
        .c2          = ToSceneAnimationAxis(curve.c2),
        .events      = ToSceneAnimationEvents(curve.options.events),
        .fps         = curve.options.fps,
        .length      = curve.options.length,
        .mode        = String::make(rstd::cppstd::as_str(curve.options.mode).unwrap()),
        .wraploop    = curve.options.wraploop,
        .relative    = curve.relative,
        .startpaused = curve.options.startpaused,
    };
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

} // namespace

void owe::AssignNodeFieldAnimations(SceneNode& node, const wpscene::FieldBindings& bindings) {
    if (auto origin = bindings.animations.find("origin"); origin != bindings.animations.end())
        node.SetOriginAnimation(ToSceneAnimationCurve(origin->second));
    if (auto scale = bindings.animations.find("scale"); scale != bindings.animations.end())
        node.SetScaleAnimation(ToSceneAnimationCurve(scale->second));
    if (auto angles = bindings.animations.find("angles"); angles != bindings.animations.end())
        node.SetRotationAnimation(ToSceneAnimationCurve(angles->second));
    if (auto alpha = bindings.animations.find("alpha"); alpha != bindings.animations.end())
        node.SetAlphaAnimation(ToSceneAnimationCurve(alpha->second));
}

auto owe::ToSceneAnimationCurve(const wpscene::AnimCurve& curve) -> SceneAnimationCurve {
    return BuildSceneAnimationCurve(curve);
}

void owe::AssignAnimationCurve(SceneAnimationCurve&          destination,
                               const wpscene::FieldBindings& bindings, ref<str> field) {
    auto found = bindings.animations.find(rstd::cppstd::to_string(field));
    if (found != bindings.animations.end()) destination = ToSceneAnimationCurve(found->second);
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
