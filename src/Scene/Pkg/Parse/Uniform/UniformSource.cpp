module;

#include <algorithm>
#include <cmath>

module wescene.pkg.parse;
import eigen;
import owe.scene_audio_response;
import rstd;
import rstd.cppstd;
import wescene.scene;
import wescene.pkg.spec_names;
import wescene.utils;

using namespace Eigen;
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

namespace owe
{

namespace
{

template<typename T>
struct ArcUniformBindingLease {
    Arc<T> state;

    void KeepAlive() const {}
};

template<typename T>
auto MakeArcUniformBindingLease(const Arc<T>& state) -> Option<Box<dyn<UniformBindingLease>>> {
    return Some(
        Box<dyn<UniformBindingLease>>::make(ArcUniformBindingLease<T> { .state = state.clone() }));
}

float Smooth(float value) { return value * value * (3.0f - 2.0f * value); }

template<typename T>
constexpr T Clamp(T value, T minimum, T maximum) {
    return rstd::cmp::min(rstd::cmp::max(value, minimum), maximum);
}

auto UserScalar(const Json& property) -> Option<float> {
    const auto& value = SceneUserPropertyPayload(property);
    if (auto number = value.as_f64(); number.is_some()) {
        return Some(static_cast<float>(number->to_primitive()));
    }
    if (auto boolean = value.as_bool(); boolean.is_some()) return Some(*boolean ? 1.0f : 0.0f);
    if (auto string = value.as_str(); string.is_some()) {
        try {
            return Some(std::stof(rstd::cppstd::to_string(*string)));
        } catch (...) {
        }
    }
    return None();
}

Vector2f ShakeOffset(float x, float roughness) {
    const float r    = Clamp(roughness, 0.0f, 2.0f);
    const float over = Clamp(r - 1.0f, 0.0f, 1.0f);
    const float grow = over * over;

    constexpr float pi       = rstd::f32::consts::PI.to_primitive();
    const float     beat_pos = std::max(0.0f, x) / (pi * 0.5f);
    const auto      beat     = static_cast<std::int32_t>(std::floor(beat_pos));
    const float     local    = beat_pos - static_cast<float>(beat);
    const float     amount   = Smooth(local);

    static constexpr std::array<std::array<float, 2>, 8> directions {
        std::array<float, 2> { -1.0f, 1.0f }, std::array<float, 2> { 1.0f, -1.0f },
        std::array<float, 2> { -1.0f, 1.0f }, std::array<float, 2> { 1.0f, -1.0f },
        std::array<float, 2> { 1.0f, 1.0f },  std::array<float, 2> { -1.0f, -1.0f },
        std::array<float, 2> { 1.0f, 1.0f },  std::array<float, 2> { -1.0f, -1.0f },
    };
    static constexpr std::array<float, 8> base_factors {
        0.8f, 1.0f, 0.45f, 0.6f, 0.8f, 1.0f, 0.45f, 0.6f,
    };
    static constexpr std::array<float, 8> rough_factors {
        6.0f, 8.0f, 1.0f, 1.0f, 6.0f, 8.0f, 1.0f, 1.0f,
    };

    auto sample = [&](std::int32_t index) -> Vector2f {
        if ((index % 2) != 0) return Vector2f::Zero();
        const auto direction =
            static_cast<std::size_t>((index / 2) % static_cast<std::int32_t>(directions.size()));
        const float factor =
            base_factors[direction] * (1.0f + (rough_factors[direction] - 1.0f) * grow);
        return { directions[direction][0] * factor, directions[direction][1] * factor };
    };

    const Vector2f a     = sample(beat);
    const Vector2f b     = sample(beat + 1);
    const Vector2f delta = b - a;
    Vector2f       curve { -delta.y(), delta.x() };
    if (curve.squaredNorm() > 0.0f) curve.normalize();
    const float bend = std::sin(local * pi) * (0.09f + grow * 0.04f) * delta.norm();
    return a * (1.0f - amount) + b * amount + curve * bend;
}

class UniformWriter {
public:
    explicit UniformWriter(mut_ref<dyn<UniformValueSink>> sink): m_sink(sink) {}

    template<typename Output>
    bool Wants(Output output) {
        return m_sink->Wants(ToUniformOutput(output));
    }
    bool Wants(UniformOutputId output) { return m_sink->Wants(output); }

    template<typename Output, typename Value>
    void Write(Output output, const Value& value) {
        Write(ToUniformOutput(output), value);
    }

    template<typename Value>
    void Write(UniformOutputId output, const Value& value) {
        auto uniform = UniformValue(value);
        WriteView(output, uniform.View());
    }

    void Write(UniformOutputId output, const UniformValue& value) {
        WriteView(output, value.View());
    }

    void WriteView(UniformOutputId output, UniformValueView value) {
        if (m_failed || ! m_sink->Wants(output)) return;
        auto result = m_sink->Write(output, value);
        if (result.is_err()) {
            m_error  = rstd::move(result).unwrap_err_unchecked().message;
            m_failed = true;
        }
    }

    auto Finish() -> Result<empty, UniformError> {
        if (m_failed) return Err(UniformError { .message = rstd::move(m_error) });
        return Ok(empty {});
    }

private:
    mut_ref<dyn<UniformValueSink>> m_sink;
    String                         m_error;
    bool                           m_failed { false };
};

template<typename Output>
auto Bind(mut_ref<dyn<UniformBindingSink>> sink, Output output, ref<str> name,
          UniformValueShape shape) -> Result<empty, UniformError> {
    auto result = sink->Bind(ToUniformOutput(output), name, shape);
    if (result.is_err()) return Err(rstd::move(result).unwrap_err_unchecked());
    return Ok(empty {});
}

auto Bind(mut_ref<dyn<UniformBindingSink>> sink, UniformOutputId output, ref<str> name,
          UniformValueShape shape) -> Result<empty, UniformError> {
    auto result = sink->Bind(output, name, shape);
    if (result.is_err()) return Err(rstd::move(result).unwrap_err_unchecked());
    return Ok(empty {});
}

auto BindGlobalProducer(mut_ref<dyn<UniformBindingSink>> sink, GlobalUniformProducer producer)
    -> Result<empty, UniformError> {
    for (const auto& field : GlobalUniformFields()) {
        if (field.producer != producer) continue;
        auto result = Bind(sink, field.output, field.name, field.shape);
        if (result.is_err()) return result;
        if (! field.alias.is_empty()) {
            result = Bind(sink, field.output, field.alias, field.shape);
            if (result.is_err()) return result;
        }
    }
    return Ok(empty {});
}

template<typename Output>
struct BindingEntry {
    Output            output;
    ref<str>          name;
    UniformValueShape shape;
};

template<typename Output, std::size_t N>
auto BindEntries(mut_ref<dyn<UniformBindingSink>>            sink,
                 const rstd::array<BindingEntry<Output>, N>& entries)
    -> Result<empty, UniformError> {
    for (const auto& entry : entries) {
        auto result = Bind(sink, entry.output, entry.name, entry.shape);
        if (result.is_err()) return result;
    }
    return Ok(empty {});
}

} // namespace

void UniformNodeConfigDraft::SetParallaxContract(wpscene::ParallaxDepthBinding binding, i32 owner,
                                                 bool propagate_to_children) {
    object_id                      = owner;
    parallax                       = rstd::move(binding);
    propagate_parallax_to_children = propagate_to_children;
}

auto UniformNodeConfigDraft::Clone() const -> UniformNodeConfigDraft {
    UniformNodeConfigDraft result {
        .configured                     = configured,
        .object_id                      = object_id,
        .parallax                       = parallax,
        .propagate_parallax_to_children = propagate_parallax_to_children,
        .ride_parent_parallax           = ride_parent_parallax,
        .use_camera_eye_position        = use_camera_eye_position,
        .eye_position_override          = eye_position_override,
        .vertices_in_world_space        = vertices_in_world_space,
        .effect_projection_node =
            effect_projection_node.is_some() ? Some((*effect_projection_node).clone()) : None(),
        .effect_projection_size = effect_projection_size,
    };
    return result;
}

auto UniformNodeConfigDraft::CloneForRuntimeLayer(i32 owner) const -> UniformNodeConfigDraft {
    auto result      = Clone();
    result.object_id = owner;
    return result;
}

void UniformCameraResolver::Add(String name, Arc<SceneCamera> camera) {
    (void)m_cameras.insert(rstd::move(name), rstd::move(camera));
}

auto UniformCameraResolver::Resolve(const SceneNode& node) const -> Option<mut_ref<SceneCamera>> {
    auto name = rstd::cppstd::as_str(node.Camera()).unwrap();
    if (name.is_empty()) {
        if (! node.Perspective()) return Some(m_active_camera.deref_mut());
        name = "global_perspective"_str;
    }
    auto camera = m_cameras.get(name);
    return camera.is_some() ? Some((**camera).deref_mut()) : None();
}

void UniformSceneState::SetNodeState(SceneNodeId id, Arc<UniformNodeState> state) {
    RegisterNodeParallaxContract(*state->node, state->object_id, state->parallax);
    (void)m_nodes_by_address.insert(rstd::addressof(*state->node), state.clone());
    if (state->object_id != i32()) {
        if (auto current = m_object_parallax_depths.get(state->object_id); current.is_some()) {
            state->parallax.depth    = { (**current)[usize()], (**current)[usize(1)] };
            state->parallax.authored = true;
        }
        auto owners = m_nodes_by_object.get_mut(state->object_id);
        if (owners.is_none()) {
            (void)m_nodes_by_object.insert(state->object_id, Vec<Arc<UniformNodeState>>::make());
            owners = m_nodes_by_object.get_mut(state->object_id);
        }
        (*owners)->push(state.clone());
    } else if (auto current = m_node_parallax_depths.get(state->node.as_ptr()); current.is_some()) {
        state->parallax.depth    = { (**current)[usize()], (**current)[usize(1)] };
        state->parallax.authored = true;
    }
    (void)m_nodes.insert(Key(id), rstd::move(state));
}

void UniformSceneState::RegisterNodeParallaxContract(const SceneNode&           node,
                                                     i32                        object_id,
                                                     const wpscene::ParallaxDepthBinding& binding) {
    (void)m_parallax_owners.insert(rstd::addressof(node), object_id);
    if (! binding.authored) return;
    const array<float, 2> depth { binding.depth[0], binding.depth[1] };
    if (object_id != i32()) {
        if (! m_object_parallax_depths.contains_key(object_id))
            (void)m_object_parallax_depths.insert(object_id, depth);
        return;
    }
    if (! m_node_parallax_depths.contains_key(rstd::addressof(node)))
        (void)m_node_parallax_depths.insert(rstd::addressof(node), depth);
}

bool UniformSceneState::SetEffectProjectionSize(SceneNodeId id, rstd::array<float, 2> size) {
    auto found = m_nodes.get_mut(Key(id));
    if (found.is_none()) return false;
    (**found)->effect_projection_size = size;
    return true;
}

bool UniformSceneState::SetObjectParallaxDepth(i32 object_id, array<float, 2> depth) {
    if (auto current = m_object_parallax_depths.get_mut(object_id); current.is_some()) {
        **current = depth;
    } else {
        (void)m_object_parallax_depths.insert(object_id, depth);
    }
    auto states = m_nodes_by_object.get_mut(object_id);
    if (states.is_none()) return true;

    // A logical layer may own a world node, private effect writers, and a detached final writer.
    // Updating the owner group atomically keeps every pass on the same depth revision.
    for (usize index {}; index < (*states)->len(); ++index) {
        (**states)[index]->parallax.depth    = { depth[usize()], depth[usize(1)] };
        (**states)[index]->parallax.authored = true;
    }
    return true;
}

bool UniformSceneState::SetNodeParallaxDepth(const SceneNode& node, array<float, 2> depth) {
    auto owner = m_parallax_owners.get(rstd::addressof(node));
    if (owner.is_some() && **owner != i32()) return SetObjectParallaxDepth(**owner, depth);

    if (auto current = m_node_parallax_depths.get_mut(rstd::addressof(node)); current.is_some()) {
        **current = depth;
    } else {
        (void)m_node_parallax_depths.insert(rstd::addressof(node), depth);
    }
    auto state = m_nodes_by_address.get(rstd::addressof(node));
    if (state.is_some()) {
        (**state)->parallax.depth    = { depth[usize()], depth[usize(1)] };
        (**state)->parallax.authored = true;
    }
    return owner.is_some() || state.is_some();
}

bool UniformSceneState::ApplyObjectParallaxDepth(i32 object_id, const Json& property) {
    const auto&     value = SceneUserPropertyPayload(property);
    array<float, 2> depth {};
    if (owe::GetJsonValue(value, depth)) return SetObjectParallaxDepth(object_id, depth);

    auto scalar = UserScalar(property);
    if (scalar.is_none()) return false;
    return SetObjectParallaxDepth(object_id, { *scalar, *scalar });
}

auto UniformSceneState::NodeParallaxDepth(const SceneNode& node) const -> Option<array<float, 2>> {
    auto owner = m_parallax_owners.get(rstd::addressof(node));
    if (owner.is_some() && **owner != i32()) {
        auto depth = m_object_parallax_depths.get(**owner);
        if (depth.is_some())
            return Some(array<float, 2> { (**depth)[usize()], (**depth)[usize(1)] });
    }
    auto depth = m_node_parallax_depths.get(rstd::addressof(node));
    if (depth.is_some()) return Some(array<float, 2> { (**depth)[usize()], (**depth)[usize(1)] });
    auto state = m_nodes_by_address.get(rstd::addressof(node));
    return state.is_some() ? Some(array<float, 2> { (**state)->parallax.depth[0],
                                                    (**state)->parallax.depth[1] })
                           : None();
}

auto UniformSceneState::FindNodeState(const SceneNode* node) const -> const UniformNodeState* {
    if (node == nullptr) return nullptr;
    auto found = m_nodes_by_address.get(node);
    if (found.is_none()) return nullptr;
    return rstd::addressof(***found);
}

void UniformSceneState::SetPointerInput(double x, double y) {
    m_pointer_input = { static_cast<float>(x), static_cast<float>(y) };
}

void UniformSceneState::SetAudioSpectrum(const scene_audio::Buffers& buffers) {
    m_inputs.audio = buffers;
}

auto UniformSceneState::LogicalParallaxState(const UniformNodeState& state) const
    -> const UniformNodeState* {
    const UniformNodeState* current = rstd::addressof(state);
    if (state.effect_projection_node.is_some()) {
        if (auto* found = FindNodeState(rstd::addressof(**state.effect_projection_node));
            found != nullptr) {
            current = found;
        }
    } else if (state.object_id != i32()) {
        auto group = m_nodes_by_object.get(state.object_id);
        if (group.is_some()) {
            for (usize index {}; index < (*group)->len(); ++index) {
                const auto& candidate = (**group)[index];
                if (candidate->node->Camera().empty()) {
                    current = rstd::addressof(*candidate);
                    break;
                }
            }
        }
    }
    if (current == rstd::addressof(state) && ! state.node->Camera().empty()) {
        for (auto* parent = state.node->Parent(); parent != nullptr; parent = parent->Parent()) {
            if (auto* found = FindNodeState(parent);
                found != nullptr && found->node->Camera().empty()) {
                current = found;
                break;
            }
        }
    }

    if (current->parallax.authored) return current;

    // Unauthored layers inherit from the nearest authored ancestor.
    auto remaining = m_nodes.len();
    while (remaining != usize()) {
        auto* parent = ParentParallaxState(*current);
        if (parent == nullptr) break;
        --remaining;
        if (! parent->propagate_parallax_to_children) break;
        current = parent;
        if (current->parallax.authored) return current;
    }
    return current;
}

auto UniformSceneState::ParentParallaxState(const UniformNodeState& current) const
    -> const UniformNodeState* {
    for (auto* parent = current.node->Parent(); parent != nullptr; parent = parent->Parent()) {
        if (auto* found = FindNodeState(parent); found != nullptr) return found;
    }
    return nullptr;
}

auto UniformSceneState::ComputeParallaxOffset(const UniformNodeState& state,
                                              const SceneCamera&      camera,
                                              SceneRenderViewKind     view) const
    -> rstd::array<float, 2> {
    if (! m_layer_parallax_enabled) return { 0.0f, 0.0f };

    const auto* source = LogicalParallaxState(state);

    array<float, 2> depth_values;
    if (source->parallax.authored) {
        depth_values = { source->parallax.depth[0], source->parallax.depth[1] };
        if (wpscene::IsZeroParallaxDepth(depth_values)) return { 0.0f, 0.0f };
    } else if (m_orthographic_implicit_parallax) {
        depth_values = { wpscene::kImplicitOrthographicParallaxDepth[0],
                         wpscene::kImplicitOrthographicParallaxDepth[1] };
    } else {
        return { 0.0f, 0.0f };
    }

    source->node->UpdateTrans();
    const Vector3f node_position       = source->node->ModelTrans().block<3, 1>(0, 3).cast<float>();
    const Vector2f depth { depth_values[usize(0)], depth_values[usize(1)] };
    const auto     ortho_values = Ortho();
    const Vector2f ortho { ortho_values[usize(0)], ortho_values[usize(1)] };
    const Vector2f pointer(Inputs().pointer.data());
    const Vector2f pointer_offset = Scaling(1.0f, -1.0f) * (Vector2f { 0.5f, 0.5f } - pointer);
    const Vector2f mouse = pointer_offset.cwiseProduct(ortho) * CameraParallax().mouse_influence;
    const auto     camera_position = camera.GetPosition(view).cast<float>();
    const Vector2f offset =
        (node_position.head<2>() - camera_position.head<2>() + mouse).cwiseProduct(depth) *
        CameraParallax().amount;
    return { offset.x(), offset.y() };
}

void UniformSceneState::Advance(const SceneFrame& frame) {
    m_inputs.pointer_last = m_inputs.pointer;
    const double delay    = std::max(0.0, static_cast<double>(m_camera_parallax.delay));
    if (delay <= 0.0) {
        m_inputs.pointer = m_pointer_input;
        return;
    }

    // Delay 0 snaps. Small delay stays a follow; the rate reaches 0 at 3.
    // Delay 2 is still a follow, not a two-second time constant.
    constexpr double kDelayRange   = 3.0;
    constexpr double kResponseRate = 10.0;
    const double     rate          = kResponseRate * std::max(0.0, 1.0 - delay / kDelayRange);
    if (rate <= 0.0) return;

    const double t = std::min(1.0, rate * std::max(0.0, frame.delta.to_primitive()));
    for (usize index {}; index < m_inputs.pointer.len(); ++index) {
        const auto current = m_inputs.pointer[index];
        m_inputs.pointer[index] =
            static_cast<float>(current + (m_pointer_input[index] - current) * t);
    }
}

void UniformSceneState::ApplyUserProperty(std::string_view field, const Json& property) {
    auto value = UserScalar(property);
    if (value.is_none()) return;
    if (field == "cameraparallax") {
        m_camera_parallax.enable = *value >= 0.5f;
    } else if (field == "cameraparallaxamount") {
        m_camera_parallax.amount = *value;
    } else if (field == "cameraparallaxdelay") {
        m_camera_parallax.delay = *value;
    } else if (field == "cameraparallaxmouseinfluence") {
        m_camera_parallax.mouse_influence = *value;
    } else if (field == "camerashake") {
        m_camera_shake.enable = *value >= 0.5f;
    } else if (field == "camerashakeamplitude") {
        m_camera_shake.amplitude = *value;
    } else if (field == "camerashakespeed") {
        m_camera_shake.speed = *value;
    } else if (field == "camerashakeroughness") {
        m_camera_shake.roughness = *value;
    }
}

auto TransformUniformSource::Describe(mut_ref<dyn<UniformBindingSink>> sink) const
    -> Result<empty, UniformError> {
    using Output = TransformUniformOutput;
    auto model   = Bind(sink, Output::Model, G_M, UniformValueShape::Matrix(u32(4), u32(4)));
    if (model.is_err()) return model;

    const rstd::array<BindingEntry<Output>, 13> entries {
        BindingEntry<Output> {
            Output::ModelInverse, G_MI, UniformValueShape::Matrix(u32(4), u32(4)) },
        BindingEntry<Output> {
            Output::NormalModel, G_NORMALMODELMATRIX, UniformValueShape::Matrix(u32(3), u32(3)) },
        BindingEntry<Output> {
            Output::AlternateModel, G_AM, UniformValueShape::Matrix(u32(4), u32(4)) },
        BindingEntry<Output> {
            Output::ModelViewProjection, G_MVP, UniformValueShape::Matrix(u32(4), u32(4)) },
        BindingEntry<Output> {
            Output::ModelViewProjectionInverse, G_MVPI, UniformValueShape::Matrix(u32(4), u32(4)) },
        BindingEntry<Output> {
            Output::EyePosition, G_EYEPOSITION, UniformValueShape::Float(u32(3)) },
        BindingEntry<Output> {
            Output::EffectModel, G_EFFECTMODELMATRIX, UniformValueShape::Matrix(u32(4), u32(4)) },
        BindingEntry<Output> {
            Output::EffectModelViewProjection, G_EMVP, UniformValueShape::Matrix(u32(4), u32(4)) },
        BindingEntry<Output> { Output::EffectModelViewProjectionInverse,
                               G_EFFECTMODELVIEWPROJECTIONMATRIXINVERSE,
                               UniformValueShape::Matrix(u32(4), u32(4)) },
        BindingEntry<Output> {
            Output::LayerModel, G_LAYERMODELMATRIX, UniformValueShape::Matrix(u32(4), u32(4)) },
        BindingEntry<Output> { Output::EffectTextureViewProjection,
                               G_ETVP,
                               UniformValueShape::Matrix(u32(4), u32(4)) },
        BindingEntry<Output> { Output::EffectTextureViewProjectionInverse,
                               G_ETVPI,
                               UniformValueShape::Matrix(u32(4), u32(4)) },
        BindingEntry<Output> {
            Output::ViewProjection, G_VP, UniformValueShape::Matrix(u32(4), u32(4)) },
    };
    return BindEntries(sink, entries);
}

auto TransformUniformSource::Version(ref<dyn<UniformUpdateContext>> context) const -> u64 {
    return context->Frame()->revision;
}

auto TransformUniformSource::Evaluate(ref<dyn<UniformUpdateContext>> context,
                                      mut_ref<dyn<UniformValueSink>> sink) const
    -> Result<empty, UniformError> {
    auto camera_ref = m_node->camera_resolver->Resolve(*m_node->node);
    if (camera_ref.is_none()) return Ok(empty {});

    using Output = TransformUniformOutput;
    UniformWriter writer(sink);
    auto&         node        = *m_node->node;
    auto&         camera      = **camera_ref;
    const auto    render_view = context->RenderView();
    node.UpdateTrans();

    const auto frame            = context->Frame();
    const bool req_mi           = writer.Wants(Output::ModelInverse);
    const bool req_m            = writer.Wants(Output::Model);
    const bool req_normal_model = writer.Wants(Output::NormalModel);
    const bool req_am           = writer.Wants(Output::AlternateModel);
    const bool req_mvp          = writer.Wants(Output::ModelViewProjection);
    const bool req_mvpi         = writer.Wants(Output::ModelViewProjectionInverse);
    const bool req_emvp         = writer.Wants(Output::EffectModelViewProjection);
    const bool req_emvpi        = writer.Wants(Output::EffectModelViewProjectionInverse);
    const bool req_effect_model = writer.Wants(Output::EffectModel) || req_emvp || req_emvpi ||
                                  writer.Wants(Output::LayerModel);

    Matrix4d    view_projection   = camera.GetViewProjectionMatrix(render_view);
    const auto& shake             = m_state->CameraShake();
    auto        active_camera_ref = m_node->camera_resolver->Active();
    const bool  active_camera     = (*camera_ref).as_raw_ptr() == active_camera_ref.as_raw_ptr();
    if (shake.enable && active_camera && ! camera.IsPerspective() && shake.amplitude > 0.0f &&
        shake.speed > 0.0f) {
        const auto  ortho       = m_state->Ortho();
        const float base_extent = std::min(ortho[usize(0)], ortho[usize(1)]);
        const float scale       = shake.amplitude * base_extent * 0.01f;
        const float time   = static_cast<float>(frame->elapsed.to_primitive()) * shake.speed * 2.0f;
        const auto  offset = ShakeOffset(time, shake.roughness);
        view_projection =
            view_projection *
            Affine3d(Translation3d(Vector3d(offset.x() * scale, offset.y() * scale, 0.0))).matrix();
    }

    writer.Write(Output::ViewProjection, ShaderValue::fromMatrix(view_projection));
    if (m_node->eye_position_override.is_some()) {
        writer.Write(Output::EyePosition, *m_node->eye_position_override);
    } else if (m_node->use_camera_eye_position || camera.IsPerspective()) {
        const auto position = camera.GetPosition(render_view).cast<float>();
        writer.Write(Output::EyePosition,
                     rstd::array<float, 3> { position.x(), position.y(), position.z() });
    }

    if (req_m || req_normal_model || req_am || req_mvp || req_mi || req_mvpi || req_effect_model) {
        Matrix4d model = m_node->vertices_in_world_space ? Matrix4d::Identity() : node.ModelTrans();
        const auto& parallax = m_state->CameraParallax();
        auto        attached = camera.GetAttachedNode();
        const bool  own_image_effect =
            attached.is_some() && (*attached)->HasLayer() && *attached == m_node->node.as_ptr();
        const bool apply_model_parallax =
            node.Camera() != "effect" && parallax.enable && ! own_image_effect;
        array<float, 2> shift {};
        if (apply_model_parallax) {
            shift = m_state->ComputeParallaxOffset(*m_node, camera, render_view);
            if (shift[usize(0)] != 0.0f || shift[usize(1)] != 0.0f) {
                model = Affine3d(Translation3d(Vector3d(shift[usize(0)], shift[usize(1)], 0.0)))
                            .matrix() *
                        model;
            }
        }

        model *= node.GeometryTransform();
        if (auto* mesh = node.Mesh(); mesh != nullptr) model *= mesh->GeometryTransform();

        if (req_m) writer.Write(Output::Model, ShaderValue::fromMatrix(model));
        if (req_normal_model) {
            Matrix3d normal_model = model.block<3, 3>(0, 0);
            if (std::abs(normal_model.determinant()) > 1e-12) {
                normal_model = normal_model.inverse().transpose();
                for (Eigen::Index row {}; row < normal_model.rows(); ++row) {
                    normal_model.row(row).normalize();
                }
            } else {
                normal_model.setIdentity();
            }
            writer.Write(Output::NormalModel, ShaderValue::fromMatrix(normal_model));
        }
        if (req_am) writer.Write(Output::AlternateModel, ShaderValue::fromMatrix(model));
        if (req_mi) writer.Write(Output::ModelInverse, ShaderValue::fromMatrix(model.inverse()));
        if (req_mvp || req_mvpi) {
            const Matrix4d mvp = view_projection * model;
            if (req_mvp) writer.Write(Output::ModelViewProjection, ShaderValue::fromMatrix(mvp));
            if (req_mvpi)
                writer.Write(Output::ModelViewProjectionInverse,
                             ShaderValue::fromMatrix(mvp.inverse()));
        }
        if (req_effect_model) {
            Matrix4d layer_model  = model;
            Matrix4d effect_model = model;
            if (m_node->effect_projection_node.is_some()) {
                auto& source = **m_node->effect_projection_node;
                source.UpdateTrans();
                layer_model  = source.ModelTrans();
                effect_model = layer_model;
                if (m_node->effect_projection_size[usize(0)] > 0.0f &&
                    m_node->effect_projection_size[usize(1)] > 0.0f) {
                    effect_model =
                        effect_model *
                        Affine3d(
                            Scaling(
                                static_cast<double>(m_node->effect_projection_size[usize(0)]) * 0.5,
                                static_cast<double>(m_node->effect_projection_size[usize(1)]) * 0.5,
                                1.0))
                            .matrix();
                }
                // Screen compose can bind g_EffectModel / g_LayerModel instead of
                // g_Model. Keep those matrices on the same shift as the layer.
                if (apply_model_parallax && (shift[usize(0)] != 0.0f || shift[usize(1)] != 0.0f)) {
                    const auto parallax_model =
                        Affine3d(Translation3d(Vector3d(shift[usize(0)], shift[usize(1)], 0.0)))
                            .matrix();
                    layer_model  = parallax_model * layer_model;
                    effect_model = parallax_model * effect_model;
                }
            }
            writer.Write(Output::LayerModel, ShaderValue::fromMatrix(layer_model));
            writer.Write(Output::EffectModel, ShaderValue::fromMatrix(effect_model));
            if (req_emvp || req_emvpi) {
                const Matrix4d effect_view =
                    active_camera_ref->GetViewProjectionMatrix(render_view);
                const Matrix4d effect_mvp = effect_view * effect_model;
                if (req_emvp)
                    writer.Write(Output::EffectModelViewProjection,
                                 ShaderValue::fromMatrix(effect_mvp));
                if (req_emvpi)
                    writer.Write(Output::EffectModelViewProjectionInverse,
                                 ShaderValue::fromMatrix(effect_mvp.inverse()));
            }
        }
    }

    return writer.Finish();
}

auto FrameUniformSource::Describe(mut_ref<dyn<UniformBindingSink>> sink) const
    -> Result<empty, UniformError> {
    using Output = FrameUniformOutput;
    auto global  = BindGlobalProducer(sink, GlobalUniformProducer::Frame);
    if (global.is_err()) return global;
    const rstd::array<BindingEntry<Output>, 3> entries {
        BindingEntry<Output> { Output::TexelSize, G_TEXELSIZE, UniformValueShape::Float(u32(2)) },
        BindingEntry<Output> {
            Output::TexelSizeHalf, G_TEXELSIZEHALF, UniformValueShape::Float(u32(2)) },
        BindingEntry<Output> { Output::Screen, G_SCREEN, UniformValueShape::Float(u32(3)) },
    };
    return BindEntries(sink, entries);
}

auto FrameUniformSource::Version(ref<dyn<UniformUpdateContext>> context) const -> u64 {
    return context->Frame()->revision;
}

auto FrameUniformSource::Evaluate(ref<dyn<UniformUpdateContext>> context,
                                  mut_ref<dyn<UniformValueSink>> sink) const
    -> Result<empty, UniformError> {
    using Output = FrameUniformOutput;
    UniformWriter writer(sink);
    const auto    frame  = context->Frame();
    const auto&   inputs = m_state->Inputs();

    writer.Write(Output::Time, static_cast<float>(frame->elapsed.to_primitive()));
    writer.Write(Output::FrameTime, static_cast<float>(frame->delta.to_primitive()));
    writer.Write(Output::DayTime, 0.0f);
    writer.Write(Output::PointerPosition, inputs.pointer);
    writer.Write(Output::PointerPositionLast, inputs.pointer_last);
    if (writer.Wants(Output::TexelSize) || writer.Wants(Output::TexelSizeHalf) ||
        writer.Wants(Output::Screen)) {
        auto       resources = context->Resources();
        const auto texel     = resources->TexelSize();
        const auto viewport  = resources->Viewport();
        writer.Write(Output::TexelSize, texel);
        writer.Write(Output::TexelSizeHalf,
                     rstd::array<float, 2> { texel[usize(0)] * 0.5f, texel[usize(1)] * 0.5f });
        const float aspect =
            viewport[usize(1)] > 0.0f ? viewport[usize(0)] / viewport[usize(1)] : 1.0f;
        writer.Write(Output::Screen,
                     rstd::array<float, 3> { viewport[usize(0)], viewport[usize(1)], aspect });
    }

    Vector2f    parallax_position { 0.5f, 0.5f };
    const auto& parallax = m_state->CameraParallax();
    if (parallax.enable) {
        const Vector2f centered = Vector2f(inputs.pointer.data()) - Vector2f { 0.5f, 0.5f };
        parallax_position =
            Vector2f { 0.5f, 0.5f } + (Scaling(1.0f, -1.0f) * centered) * parallax.mouse_influence;
    }
    writer.Write(Output::ParallaxPosition,
                 rstd::array<float, 2> { parallax_position.x(), parallax_position.y() });
    return writer.Finish();
}

auto AudioUniformSource::Describe(mut_ref<dyn<UniformBindingSink>> sink) const
    -> Result<empty, UniformError> {
    return BindGlobalProducer(sink, GlobalUniformProducer::Audio);
}

auto AudioUniformSource::Version(ref<dyn<UniformUpdateContext>> context) const -> u64 {
    return context->Frame()->revision;
}

auto AudioUniformSource::AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>> {
    return Some(m_state->AcquireAudioResponse());
}

auto ParticleTrailUniformSource::AcquireBindingLease() const
    -> Option<Box<dyn<UniformBindingLease>>> {
    return MakeArcUniformBindingLease(m_state);
}

auto AudioUniformSource::Evaluate(ref<dyn<UniformUpdateContext>>,
                                  mut_ref<dyn<UniformValueSink>> sink) const
    -> Result<empty, UniformError> {
    using Output = AudioUniformOutput;
    UniformWriter writer(sink);
    const auto&   inputs      = m_state->Inputs();
    auto          write_audio = [&](Output output, slice<float> values) {
        if (writer.Wants(output))
            writer.Write(output, UniformValue(values.as_raw_ptr(), values.len()));
    };
    write_audio(Output::Spectrum16Left, inputs.audio.bands16.left.as_slice());
    write_audio(Output::Spectrum16Right, inputs.audio.bands16.right.as_slice());
    write_audio(Output::Spectrum32Left, inputs.audio.bands32.left.as_slice());
    write_audio(Output::Spectrum32Right, inputs.audio.bands32.right.as_slice());
    write_audio(Output::Spectrum64Left, inputs.audio.bands64.left.as_slice());
    write_audio(Output::Spectrum64Right, inputs.audio.bands64.right.as_slice());
    return writer.Finish();
}

auto ColorUniformSource::Describe(mut_ref<dyn<UniformBindingSink>> sink) const
    -> Result<empty, UniformError> {
    using Output = ColorUniformOutput;
    const rstd::array<BindingEntry<Output>, 5> entries {
        BindingEntry<Output> { Output::UserAlpha, G_USERALPHA, UniformValueShape::Float(u32(1)) },
        BindingEntry<Output> { Output::Color4, G_COLOR4, UniformValueShape::Float(u32(4)) },
        BindingEntry<Output> { Output::Color, G_COLOR, UniformValueShape::Float(u32(3)) },
        BindingEntry<Output> { Output::Alpha, G_ALPHA, UniformValueShape::Float(u32(1)) },
        BindingEntry<Output> { Output::Brightness, G_BRIGHTNESS, UniformValueShape::Float(u32(1)) },
    };
    return BindEntries(sink, entries);
}

auto ColorUniformSource::Version(ref<dyn<UniformUpdateContext>> context) const -> u64 {
    return context->Frame()->revision;
}

auto ColorUniformSource::Evaluate(ref<dyn<UniformUpdateContext>>,
                                  mut_ref<dyn<UniformValueSink>> sink) const
    -> Result<empty, UniformError> {
    using Output = ColorUniformOutput;
    UniformWriter writer(sink);
    const auto&   node           = *m_node;
    const bool    has_user_alpha = writer.Wants(Output::UserAlpha);
    const bool    has_alpha      = writer.Wants(Output::Alpha);
    const bool    has_color4     = writer.Wants(Output::Color4);
    const bool    has_color      = writer.Wants(Output::Color);
    auto          write_color4   = [&](const Vector3f& color, float alpha) {
        writer.Write(Output::Color4,
                     rstd::array<float, 4> { color.x(), color.y(), color.z(), alpha });
    };
    auto write_color = [&](const Vector3f& color) {
        writer.Write(Output::Color, rstd::array<float, 3> { color.x(), color.y(), color.z() });
    };
    if (node.IsAlphaOverridden()) {
        if (has_user_alpha) writer.Write(Output::UserAlpha, node.EffectiveAlpha());
        if (has_alpha) writer.Write(Output::Alpha, node.EffectiveAlpha());
        if (has_color4) {
            if (! has_user_alpha) {
                write_color4(node.IsColorOverridden() ? node.Color() : node.BaseColor(),
                             node.EffectiveAlpha());
            } else if (node.IsColorOverridden()) {
                write_color4(node.Color(), node.BaseAlpha());
            }
        }
        if (has_color && node.IsColorOverridden()) write_color(node.Color());
    } else if (node.IsColorOverridden()) {
        if (has_color4) write_color4(node.Color(), node.BaseAlpha());
        if (has_color) write_color(node.Color());
    }
    if (node.IsBrightnessOverridden()) writer.Write(Output::Brightness, node.Brightness());
    return writer.Finish();
}

auto LightUniformSource::Describe(mut_ref<dyn<UniformBindingSink>> sink) const
    -> Result<empty, UniformError> {
    return BindGlobalProducer(sink, GlobalUniformProducer::Light);
}

auto LightUniformSource::Version(ref<dyn<UniformUpdateContext>> context) const -> u64 {
    return context->Frame()->revision;
}

auto LightUniformSource::Evaluate(ref<dyn<UniformUpdateContext>>,
                                  mut_ref<dyn<UniformValueSink>> sink) const
    -> Result<empty, UniformError> {
    using Output = LightUniformOutput;
    UniformWriter          writer(sink);
    constexpr usize        max_lights { 4 };
    rstd::array<float, 12> positions {};
    rstd::array<float, 16> colors_radius {};
    rstd::array<float, 12> colors_legacy {};
    rstd::array<float, 16> directions_type {};
    rstd::array<float, 16> cones_exponent {};
    rstd::array<float, 4>  cast_shadow {};
    for (usize index {}; index < max_lights; ++index)
        directions_type[index * usize(4) + usize(3)] = -1.0f;
    for (usize index {}; index < rstd::cmp::min(max_lights, m_lights.len()); ++index) {
        const auto& light = *m_lights[index];
        if (light.node() == nullptr || ! light.runtimeVisible()) continue;
        auto& node = *light.node();
        node.UpdateTrans();
        const auto transform = node.ModelTrans();
        const auto position  = transform.block<3, 1>(0, 3).cast<float>();
        auto       color     = light.color();
        if (node.IsColorOverridden()) color = node.Color() * light.desc().intensity;
        positions[index * usize(3)]                = position.x();
        positions[index * usize(3) + usize(1)]     = position.y();
        positions[index * usize(3) + usize(2)]     = position.z();
        colors_radius[index * usize(4)]            = color.x();
        colors_radius[index * usize(4) + usize(1)] = color.y();
        colors_radius[index * usize(4) + usize(2)] = color.z();
        colors_radius[index * usize(4) + usize(3)] = light.radius();
        Eigen::Vector3d local_direction            = Eigen::Vector3d::UnitX();
        if (light.type() != SceneLightType::Spot) local_direction = -local_direction;
        const auto direction =
            (transform.block<3, 3>(0, 0) * local_direction).normalized().cast<float>();
        directions_type[index * usize(4)]            = direction.x();
        directions_type[index * usize(4) + usize(1)] = direction.y();
        directions_type[index * usize(4) + usize(2)] = direction.z();
        directions_type[index * usize(4) + usize(3)] = static_cast<float>(light.type());
        cones_exponent[index * usize(4)]             = light.desc().inner_cone_cos;
        cones_exponent[index * usize(4) + usize(1)]  = light.desc().outer_cone_cos;
        cones_exponent[index * usize(4) + usize(2)]  = light.desc().exponent;
        cast_shadow[index]                           = light.desc().cast_shadow ? 1.0f : 0.0f;
        if (index < usize(3)) {
            const auto premultiplied = color * light.radius() * light.radius();
            for (usize component {}; component < usize(3); ++component) {
                colors_legacy[index * usize(4) + component] =
                    premultiplied[component.to_primitive()];
            }
        }
    }
    writer.Write(Output::Position, positions);
    writer.Write(Output::ColorLegacy, colors_legacy);
    writer.Write(Output::ColorRadius, colors_radius);
    writer.Write(Output::DirectionType, directions_type);
    writer.Write(Output::ConeExponent, cones_exponent);
    writer.Write(Output::CastShadow, cast_shadow);
    return writer.Finish();
}

auto ShadowUniformSource::Describe(mut_ref<dyn<UniformBindingSink>> sink) const
    -> Result<empty, UniformError> {
    return BindGlobalProducer(sink, GlobalUniformProducer::Shadow);
}

auto ShadowUniformSource::Version(ref<dyn<UniformUpdateContext>> context) const -> u64 {
    return context->Frame()->revision;
}

auto ShadowUniformSource::Evaluate(ref<dyn<UniformUpdateContext>>,
                                   mut_ref<dyn<UniformValueSink>> sink) const
    -> Result<empty, UniformError> {
    using Output = ShadowUniformOutput;
    UniformWriter writer(sink);

    const auto      camera_transform = m_camera->Transforms();
    Eigen::Vector3d forward          = camera_transform.center - camera_transform.eye;
    if (! forward.allFinite() || forward.squaredNorm() <= 1e-12) {
        return writer.Finish();
    }
    forward.normalize();

    auto* light_node = m_light->node();
    if (light_node == nullptr) return writer.Finish();
    light_node->UpdateTrans();
    const auto      light_frame = light_node->ModelTrans().block<3, 3>(0, 0);
    Eigen::Vector3d view_x      = light_frame.col(2);
    Eigen::Vector3d view_y      = light_frame.col(1);
    Eigen::Vector3d view_z      = light_frame.col(0);
    if (! view_x.allFinite() || ! view_y.allFinite() || ! view_z.allFinite() ||
        view_x.squaredNorm() <= 1e-12 || view_y.squaredNorm() <= 1e-12 ||
        view_z.squaredNorm() <= 1e-12) {
        return writer.Finish();
    }
    view_x.normalize();
    view_y.normalize();
    view_z.normalize();

    rstd::array<float, 96> matrix_values {};
    const double           camera_far = rstd::cmp::max(0.001, std::abs(m_camera->FarClip()));
    for (usize cascade {}; cascade < usize(3); ++cascade) {
        const float     authored = m_light->desc().cascade_distances[cascade.to_primitive()];
        const double    distance = authored > 0.0f ? static_cast<double>(authored) : camera_far;
        const auto      center   = camera_transform.eye + forward * (distance * 0.5);
        Eigen::Matrix4d view     = Eigen::Matrix4d::Identity();
        view.block<1, 3>(0, 0)   = view_x.transpose();
        view.block<1, 3>(1, 0)   = view_y.transpose();
        view.block<1, 3>(2, 0)   = view_z.transpose();
        view(0, 3)               = -view_x.dot(center);
        view(1, 3)               = -view_y.dot(center);
        view(2, 3)               = -view_z.dot(center);

        const double          half_extent = distance * 0.5;
        const double          depth_span  = rstd::cmp::max(80.0, distance * 1.5);
        const Eigen::Matrix4f matrix      = (Eigen::Ortho(-half_extent,
                                                          half_extent,
                                                          -half_extent,
                                                          half_extent,
                                                          -depth_span * 0.5,
                                                          depth_span * 0.5) *
                                             view)
                                                .cast<float>();
        for (usize value {}; value < usize(16); ++value) {
            matrix_values[cascade * usize(16) + value] = matrix.data()[value.to_primitive()];
        }
    }

    auto matrices = UniformValue::fromMatrixArray(
        matrix_values.data(), u32(4), u32(4), usize(6), UniformMatrixStorage::ColumnMajor);
    writer.Write(ToUniformOutput(Output::ViewProjectionMatrices), matrices);
    writer.Write(Output::AtlasTransforms,
                 rstd::array<float, 12> { 0.0f,
                                          0.0f,
                                          1.0f / 3.0f,
                                          1.0f,
                                          1.0f / 3.0f,
                                          0.0f,
                                          1.0f / 3.0f,
                                          1.0f,
                                          2.0f / 3.0f,
                                          0.0f,
                                          1.0f / 3.0f,
                                          1.0f });
    return writer.Finish();
}

auto TextureUniformSource::Describe(mut_ref<dyn<UniformBindingSink>> sink) const
    -> Result<empty, UniformError> {
    for (usize index {}; index < WE_GLTEX_NAMES.len(); ++index) {
        auto resolution = Bind(sink,
                               TextureResolutionOutput(index.to_primitive()),
                               rstd::cppstd::as_str(WE_GLTEX_RESOLUTION_NAMES[index]).unwrap(),
                               UniformValueShape::Float(u32(4)));
        if (resolution.is_err()) return resolution;
        auto mipmap = Bind(sink,
                           TextureMipmapOutput(index.to_primitive()),
                           rstd::cppstd::as_str(WE_GLTEX_MIPMAPINFO_NAMES[index]).unwrap(),
                           UniformValueShape::Float(u32(1)));
        if (mipmap.is_err()) return mipmap;
        auto rotation = Bind(sink,
                             TextureRotationOutput(index.to_primitive()),
                             rstd::cppstd::as_str(WE_GLTEX_ROTATION_NAMES[index]).unwrap(),
                             UniformValueShape::Float(u32(4)));
        if (rotation.is_err()) return rotation;
        auto translation = Bind(sink,
                                TextureTranslationOutput(index.to_primitive()),
                                rstd::cppstd::as_str(WE_GLTEX_TRANSLATION_NAMES[index]).unwrap(),
                                UniformValueShape::Float(u32(2)));
        if (translation.is_err()) return translation;
        auto texel = Bind(sink,
                          TextureTexelOutput(index.to_primitive()),
                          rstd::cppstd::as_str(WE_GLTEX_TEXEL_NAMES[index]).unwrap(),
                          UniformValueShape::Float(u32(4)));
        if (texel.is_err()) return texel;
    }
    return Ok(empty {});
}

auto TextureUniformSource::Version(ref<dyn<UniformUpdateContext>> context) const -> u64 {
    return context->Frame()->revision;
}

auto TextureUniformSource::Evaluate(ref<dyn<UniformUpdateContext>> context,
                                    mut_ref<dyn<UniformValueSink>> sink) const
    -> Result<empty, UniformError> {
    UniformWriter writer(sink);
    auto          resources = context->Resources();
    for (usize index {}; index < WE_GLTEX_NAMES.len(); ++index) {
        auto texture = resources->Texture(index);
        if (texture.is_none()) continue;
        if (texture->has_extent) {
            writer.Write(TextureResolutionOutput(index.to_primitive()),
                         rstd::array<float, 4> { texture->source_extent[usize(0)],
                                                 texture->source_extent[usize(1)],
                                                 texture->sample_extent[usize(0)],
                                                 texture->sample_extent[usize(1)] });
            const auto width  = texture->sample_extent[usize(0)];
            const auto height = texture->sample_extent[usize(1)];
            writer.Write(TextureTexelOutput(index.to_primitive()),
                         rstd::array<float, 4> { width > 0.0f ? 1.0f / width : 0.0f,
                                                 height > 0.0f ? 1.0f / height : 0.0f,
                                                 width,
                                                 height });
        }
        if (texture->has_mipmap) {
            writer.Write(TextureMipmapOutput(index.to_primitive()), texture->mipmap_level);
        }
        writer.Write(TextureRotationOutput(index.to_primitive()), texture->rotation);
        writer.Write(TextureTranslationOutput(index.to_primitive()), texture->translation);
    }
    return writer.Finish();
}

auto ParticleTrailUniformSource::Describe(mut_ref<dyn<UniformBindingSink>> sink) const
    -> Result<empty, UniformError> {
    return Bind(sink,
                ParticleTrailUniformOutput::RenderVar0,
                G_RENDERVAR0,
                UniformValueShape::Float(u32(4)));
}

auto ParticleTrailUniformSource::Version(ref<dyn<UniformUpdateContext>> context) const -> u64 {
    return context->Frame()->revision;
}

auto ParticleTrailUniformSource::Evaluate(ref<dyn<UniformUpdateContext>>,
                                          mut_ref<dyn<UniformValueSink>> sink) const
    -> Result<empty, UniformError> {
    UniformWriter writer(sink);
    writer.Write(ParticleTrailUniformOutput::RenderVar0, m_state->render_var);
    return writer.Finish();
}

auto PuppetUniformSource::Describe(mut_ref<dyn<UniformBindingSink>> sink) const
    -> Result<empty, UniformError> {
    return Bind(sink,
                UniformOutputId { .value = u32() },
                G_BONES,
                UniformValueShape::MatrixArray(u32(4), u32(4), usize(1), usize(256)));
}

auto PuppetUniformSource::Version(ref<dyn<UniformUpdateContext>> context) const -> u64 {
    return context->Frame()->revision;
}

auto PuppetUniformSource::Evaluate(ref<dyn<UniformUpdateContext>> context,
                                   mut_ref<dyn<UniformValueSink>> sink) const
    -> Result<empty, UniformError> {
    const auto output = UniformOutputId { .value = u32() };
    if (! sink->Wants(output)) return Ok(empty {});
    auto matrices = m_layer->genFrame(context->Frame()->elapsed.to_primitive());
    if (matrices.is_empty()) return Ok(empty {});
    auto value = UniformValue::fromMatrixArray(matrices[usize()].data(),
                                               u32(4),
                                               u32(4),
                                               matrices.len(),
                                               UniformMatrixStorage::ColumnMajor);
    return sink->Write(output, value.View());
}

} // namespace owe
