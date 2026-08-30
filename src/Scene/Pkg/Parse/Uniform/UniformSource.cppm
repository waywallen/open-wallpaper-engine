export module wescene.pkg.parse:uniform_source;
export import owe.scene_audio_response;
import rstd;
import rstd.cppstd;
import wescene.json;
import wescene.pkg.puppet;
import wescene.scene;
import :global_uniform;
import :particle_runtime;

using namespace rstd::prelude;
using rstd::collections::HashMap;
using rstd::sync::Arc;

export namespace owe
{

enum class TransformUniformOutput : rstd::uint32_t
{
    ModelInverse,
    Model,
    NormalModel,
    AlternateModel,
    ModelViewProjection,
    ModelViewProjectionInverse,
    EyePosition,
    EffectModel,
    EffectModelViewProjection,
    EffectModelViewProjectionInverse,
    LayerModel,
    EffectTextureViewProjection,
    EffectTextureViewProjectionInverse,
    ViewProjection,
};

enum class FrameUniformOutput : rstd::uint32_t
{
    Time,
    FrameTime,
    DayTime,
    PointerPosition,
    PointerPositionLast,
    ParallaxPosition,
    TexelSize,
    TexelSizeHalf,
    Screen,
};

enum class LightUniformOutput : rstd::uint32_t
{
    Position,
    ColorLegacy,
    ColorRadius,
    DirectionType,
    ConeExponent,
    CastShadow,
};

enum class ShadowUniformOutput : rstd::uint32_t
{
    ViewProjectionMatrices,
    AtlasTransforms,
};

enum class ColorUniformOutput : rstd::uint32_t
{
    UserAlpha,
    Color4,
    Color,
    Alpha,
    Brightness,
};

enum class AudioUniformOutput : rstd::uint32_t
{
    Spectrum16Left,
    Spectrum16Right,
    Spectrum32Left,
    Spectrum32Right,
    Spectrum64Left,
    Spectrum64Right,
};

enum class TextureUniformOutput : rstd::uint32_t
{
    Resolution0  = 0,
    Mipmap0      = 16,
    Rotation0    = 32,
    Translation0 = 48,
    Texel0       = 64,
};

enum class ParticleTrailUniformOutput : rstd::uint32_t
{
    RenderVar0,
};

template<typename Output>
inline auto ToUniformOutput(Output output) -> UniformOutputId {
    return { .value = u32(static_cast<rstd::uint32_t>(output)) };
}

inline auto TextureResolutionOutput(std::size_t index) -> UniformOutputId {
    const auto value = static_cast<rstd::uint32_t>(TextureUniformOutput::Resolution0) +
                       static_cast<rstd::uint32_t>(index);
    return { .value = u32(value) };
}

inline auto TextureMipmapOutput(std::size_t index) -> UniformOutputId {
    const auto value = static_cast<rstd::uint32_t>(TextureUniformOutput::Mipmap0) +
                       static_cast<rstd::uint32_t>(index);
    return { .value = u32(value) };
}

inline auto TextureRotationOutput(std::size_t index) -> UniformOutputId {
    const auto value = static_cast<rstd::uint32_t>(TextureUniformOutput::Rotation0) +
                       static_cast<rstd::uint32_t>(index);
    return { .value = u32(value) };
}

inline auto TextureTranslationOutput(std::size_t index) -> UniformOutputId {
    const auto value = static_cast<rstd::uint32_t>(TextureUniformOutput::Translation0) +
                       static_cast<rstd::uint32_t>(index);
    return { .value = u32(value) };
}

inline auto TextureTexelOutput(std::size_t index) -> UniformOutputId {
    const auto value = static_cast<rstd::uint32_t>(TextureUniformOutput::Texel0) +
                       static_cast<rstd::uint32_t>(index);
    return { .value = u32(value) };
}

struct UniformCameraParallax {
    bool  enable { false };
    float amount { 0.0f };
    float delay { 0.0f };
    float mouse_influence { 0.0f };
};

struct UniformCameraShake {
    bool  enable { false };
    float amplitude { 0.0f };
    float speed { 0.0f };
    float roughness { 1.0f };
};

struct UniformNodeConfigDraft {
    bool                    configured { false };
    i32                     object_id { 0 };
    array<float, 2>         parallax_depth { 0.0f, 0.0f };
    bool                    parallax_depth_authored { false };
    bool                    propagate_parallax_to_children { true };
    bool                    ride_parent_parallax { false };
    bool                    use_camera_eye_position { false };
    Option<array<float, 3>> eye_position_override;
    bool                    vertices_in_world_space { false };
    Option<Arc<SceneNode>>  effect_projection_node;
    array<float, 2>         effect_projection_size { 0.0f, 0.0f };

    auto Clone() const -> UniformNodeConfigDraft;
    auto CloneForRuntimeLayer(i32 owner) const -> UniformNodeConfigDraft;
    void SetParallaxContract(array<float, 2> depth,
                             i32             owner       = i32(),
                             bool            authored    = false,
                             bool            propagate_to_children = true);
};

class UniformCameraResolver {
public:
    explicit UniformCameraResolver(Arc<SceneCamera> active_camera)
        : m_active_camera(rstd::move(active_camera)) {}

    void Add(String name, Arc<SceneCamera> camera);
    void Reserve(usize count) { m_cameras.reserve(count); }

    auto Resolve(const SceneNode& node) const -> Option<mut_ref<SceneCamera>>;
    auto Active() const -> mut_ref<SceneCamera> { return m_active_camera.deref_mut(); }

private:
    HashMap<String, Arc<SceneCamera>> m_cameras;
    Arc<SceneCamera>                  m_active_camera;
};

struct UniformNodeState {
    Arc<SceneNode>             node;
    Arc<UniformCameraResolver> camera_resolver;
    i32                        object_id { 0 };
    array<float, 2>            parallax_depth { 0.0f, 0.0f };
    bool                       parallax_depth_authored { false };
    bool                       propagate_parallax_to_children { true };
    bool                       ride_parent_parallax { false };
    bool                       use_camera_eye_position { false };
    Option<array<float, 3>>    eye_position_override;
    bool                       vertices_in_world_space { false };
    Option<Arc<SceneNode>>     effect_projection_node;
    array<float, 2>            effect_projection_size { 0.0f, 0.0f };

    UniformNodeState(Arc<SceneNode> value, Arc<UniformCameraResolver> resolver)
        : node(rstd::move(value)), camera_resolver(rstd::move(resolver)) {}
};

struct UniformFrameInputs {
    array<float, 2>      pointer { 0.5f, 0.5f };
    array<float, 2>      pointer_last { 0.5f, 0.5f };
    scene_audio::Buffers audio;
};

class UniformSceneState {
public:
    explicit UniformSceneState(Arc<AudioResponseDemand> demand)
        : m_audio_demand(rstd::move(demand)) {}

    void SetNodeState(SceneNodeId, Arc<UniformNodeState>);
    void RegisterNodeParallaxContract(const SceneNode&, i32, array<float, 2>, bool authored);
    bool SetEffectProjectionSize(SceneNodeId, array<float, 2>);
    bool SetObjectParallaxDepth(i32, array<float, 2>);
    bool SetNodeParallaxDepth(const SceneNode&, array<float, 2>);
    bool ApplyObjectParallaxDepth(i32, const Json&);
    auto NodeParallaxDepth(const SceneNode&) const -> Option<array<float, 2>>;
    auto FindNodeState(const SceneNode*) const -> const UniformNodeState*;
    auto ComputeParallaxOffset(const UniformNodeState&, const SceneCamera&,
                               SceneRenderViewKind) const -> array<float, 2>;

    UniformCameraParallax&       CameraParallax() noexcept { return m_camera_parallax; }
    const UniformCameraParallax& CameraParallax() const noexcept { return m_camera_parallax; }
    UniformCameraShake&          CameraShake() noexcept { return m_camera_shake; }
    const UniformCameraShake&    CameraShake() const noexcept { return m_camera_shake; }
    const UniformFrameInputs&    Inputs() const noexcept { return m_inputs; }
    array<float, 2>              Ortho() const noexcept { return m_ortho; }

    void SetOrtho(float width, float height) { m_ortho = { width, height }; }
    void SetLayerParallaxPolicy(bool enabled, bool orthographic_implicit) {
        m_layer_parallax_enabled          = enabled;
        m_orthographic_implicit_parallax  = orthographic_implicit;
    }
    void SetPointerInput(double, double);
    void SetAudioSpectrum(const scene_audio::Buffers&);
    void Advance(const SceneFrame&);
    void ApplyUserProperty(std::string_view, const Json&);
    auto AcquireAudioResponse() const -> Box<dyn<UniformBindingLease>> {
        return m_audio_demand->Acquire();
    }

private:
    static u64 Key(SceneNodeId node) {
        const auto generation = static_cast<rstd::uint64_t>(node.generation.to_primitive());
        const auto index      = static_cast<rstd::uint64_t>(node.index.to_primitive());
        return u64((generation << 32U) | index);
    }

    HashMap<u64, Arc<UniformNodeState>>              m_nodes;
    HashMap<const SceneNode*, Arc<UniformNodeState>> m_nodes_by_address;
    HashMap<i32, Vec<Arc<UniformNodeState>>>         m_nodes_by_object;
    HashMap<i32, array<float, 2>>                    m_object_parallax_depths;
    HashMap<const SceneNode*, i32>                   m_parallax_owners;
    HashMap<const SceneNode*, array<float, 2>>       m_node_parallax_depths;

    auto LogicalParallaxState(const UniformNodeState&) const -> const UniformNodeState*;
    auto ParentParallaxState(const UniformNodeState&) const -> const UniformNodeState*;

    UniformFrameInputs       m_inputs;
    UniformCameraParallax    m_camera_parallax;
    UniformCameraShake       m_camera_shake;
    bool                     m_layer_parallax_enabled { false };
    bool                     m_orthographic_implicit_parallax { false };
    array<float, 2>          m_ortho { 1920.0f, 1080.0f };
    array<float, 2>          m_pointer_input { 0.5f, 0.5f };
    Arc<AudioResponseDemand> m_audio_demand;
};

class UniformRuntimeInput {
public:
    explicit UniformRuntimeInput(Arc<UniformSceneState> state): m_state(rstd::move(state)) {}

    void SetPointerInput(double x, double y) { m_state->SetPointerInput(x, y); }
    void SetAudioSpectrum(const scene_audio::Buffers& buffers) {
        m_state->SetAudioSpectrum(buffers);
    }

private:
    Arc<UniformSceneState> m_state;
};

class UniformRuntimeSystem {
public:
    explicit UniformRuntimeSystem(Arc<UniformSceneState> state): m_state(rstd::move(state)) {}

    void Update(ref<SceneFrame> frame) { m_state->Advance(*frame); }

private:
    Arc<UniformSceneState> m_state;
};

class TransformUniformSource {
public:
    TransformUniformSource(Arc<UniformSceneState> state, Arc<UniformNodeState> node)
        : m_state(rstd::move(state)), m_node(rstd::move(node)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>> { return None(); }

private:
    Arc<UniformSceneState> m_state;
    Arc<UniformNodeState>  m_node;
};

class FrameUniformSource {
public:
    explicit FrameUniformSource(Arc<UniformSceneState> state): m_state(rstd::move(state)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>> { return None(); }

private:
    Arc<UniformSceneState> m_state;
};

class AudioUniformSource {
public:
    explicit AudioUniformSource(Arc<UniformSceneState> state): m_state(rstd::move(state)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>>;

private:
    Arc<UniformSceneState> m_state;
};

class ColorUniformSource {
public:
    explicit ColorUniformSource(Arc<SceneNode> node): m_node(rstd::move(node)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>> { return None(); }

private:
    Arc<SceneNode> m_node;
};

class LightUniformSource {
public:
    explicit LightUniformSource(Vec<ref<SceneLight>> lights): m_lights(rstd::move(lights)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>> { return None(); }

private:
    Vec<ref<SceneLight>> m_lights;
};

class ShadowUniformSource {
public:
    ShadowUniformSource(Arc<SceneCamera> camera, ref<SceneLight> light)
        : m_camera(rstd::move(camera)), m_light(light) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>> { return None(); }

private:
    Arc<SceneCamera> m_camera;
    ref<SceneLight>  m_light;
};

class TextureUniformSource {
public:
    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>> { return None(); }
};

class ParticleTrailUniformSource {
public:
    explicit ParticleTrailUniformSource(Arc<ParticleTrailUniformState> state)
        : m_state(rstd::move(state)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>>;

private:
    Arc<ParticleTrailUniformState> m_state;
};

class PuppetUniformSource {
public:
    explicit PuppetUniformSource(Arc<PuppetLayer> layer): m_layer(rstd::move(layer)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>> { return None(); }

private:
    Arc<PuppetLayer> m_layer;
};

} // namespace owe
