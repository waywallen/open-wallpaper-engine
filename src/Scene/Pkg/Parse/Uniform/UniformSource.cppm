export module wescene.pkg.parse:uniform_source;
export import owe.scene_audio_response;
import rstd;
import wescene.json;
import wescene.pkg.puppet;
import wescene.scene;
import wescene.pkg.scene_obj;
export import :global_uniform;
import :particle_runtime;

using namespace rstd::prelude;
using rstd::collections::HashMap;
using rstd::sync::Arc;

export namespace owe
{

struct ShadowLightBinding {
    i32   object_id;
    usize first_view;
    usize view_count;
};

class ShadowAtlasLayout {
public:
    auto Add(i32 object_id, SceneLightType type) -> bool {
        if (Find(object_id)) return false;
        const auto count = type == SceneLightType::Directional ? usize(3) : usize(1);
        if ((type != SceneLightType::Spot && type != SceneLightType::Directional) ||
            m_view_count + count > usize(6))
            return false;
        m_lights.push(ShadowLightBinding { object_id, m_view_count, count });
        m_view_count += count;
        return true;
    }
    auto Find(i32 object_id) const -> Option<ref<ShadowLightBinding>> {
        for (const auto& light : m_lights)
            if (light.object_id == object_id)
                return Some(ref<ShadowLightBinding>::from_raw_parts(&light));
        return None();
    }
    auto ViewCount() const -> usize { return m_view_count; }
    auto Transform(usize view) const -> array<float, 4> {
        if (view >= m_view_count) return {};
        const auto scale = 1.0f / static_cast<float>(m_view_count.to_primitive());
        return { static_cast<float>(view.to_primitive()) * scale, 0.0f, scale, 1.0f };
    }

private:
    Vec<ShadowLightBinding> m_lights;
    usize                   m_view_count {};
};

auto SpotLightViewProjection(const SceneLight&) -> Eigen::Matrix4d;

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
    ViewRight,
    ViewUp,
    ViewForward,
    OrientationRight,
    OrientationUp,
    OrientationForward,
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
    AmbientColor,
    SkylightColor,
    FogDistanceColor,
    FogHeightColor,
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
    Resolution0    = 0,
    Mipmap0        = 16,
    Rotation0      = 32,
    Translation0   = 48,
    Texel0         = 64,
    ReductionScale = 80,
};

enum class ParticleTrailUniformOutput : rstd::uint32_t
{
    RenderVar0,
};

enum class VolumetricUniformOutput : rstd::uint32_t
{
    EyePosition,
    ViewProjection,
    LightTransform,
    InverseViewProjection,
    Parameters,
    OriginDensity,
    ColorExponent,
    ShadowProjection,
    ShadowTransform,
    Direction,
};

enum class VolumetricDrawKind
{
    Back,
    Front,
    Fullscreen,
};

enum class PuppetUniformOutput : rstd::uint32_t
{
    Bones,
    BlendMap,
};

template<typename Output>
inline auto ToUniformOutput(Output output) -> UniformOutputId {
    return { .value = u32(static_cast<rstd::uint32_t>(output)) };
}

inline auto TextureResolutionOutput(rstd::size_t index) -> UniformOutputId {
    const auto value = static_cast<rstd::uint32_t>(TextureUniformOutput::Resolution0) +
                       static_cast<rstd::uint32_t>(index);
    return { .value = u32(value) };
}

inline auto TextureMipmapOutput(rstd::size_t index) -> UniformOutputId {
    const auto value = static_cast<rstd::uint32_t>(TextureUniformOutput::Mipmap0) +
                       static_cast<rstd::uint32_t>(index);
    return { .value = u32(value) };
}

inline auto TextureRotationOutput(rstd::size_t index) -> UniformOutputId {
    const auto value = static_cast<rstd::uint32_t>(TextureUniformOutput::Rotation0) +
                       static_cast<rstd::uint32_t>(index);
    return { .value = u32(value) };
}

inline auto TextureTranslationOutput(rstd::size_t index) -> UniformOutputId {
    const auto value = static_cast<rstd::uint32_t>(TextureUniformOutput::Translation0) +
                       static_cast<rstd::uint32_t>(index);
    return { .value = u32(value) };
}

inline auto TextureTexelOutput(rstd::size_t index) -> UniformOutputId {
    const auto value = static_cast<rstd::uint32_t>(TextureUniformOutput::Texel0) +
                       static_cast<rstd::uint32_t>(index);
    return { .value = u32(value) };
}

struct UniformCameraParallax {
    bool  enable { false };
    float amount { 0.5f };
    float delay { 0.1f };
    float mouse_influence { 0.0f };
};

struct UniformCameraShake {
    bool  enable { false };
    float amplitude { 0.0f };
    float speed { 0.0f };
    float roughness { 1.0f };

    auto Sample(float runtime, bool orthographic, float height) const -> Eigen::Vector3d;
};

struct UniformNodeConfigDraft {
    bool                          configured { false };
    i32                           object_id { 0 };
    wpscene::ParallaxDepthBinding parallax;
    bool                          propagate_parallax_to_children { true };
    bool                          ride_parent_parallax { false };
    bool                          use_camera_eye_position { false };
    Option<array<float, 3>>       eye_position_override;
    bool                          vertices_in_world_space { false };
    bool                          reverse_depth { false };
    Option<Arc<SceneNode>>        effect_projection_node;
    array<float, 2>               effect_projection_size { 0.0f, 0.0f };

    void SetParallaxContract(wpscene::ParallaxDepthBinding binding, i32 owner = i32(),
                             bool propagate_to_children = true);
};

class UniformCameraResolver {
public:
    explicit UniformCameraResolver(Arc<SceneCamera> active_camera)
        : m_active_camera(rstd::move(active_camera)) {}
    UniformCameraResolver(Arc<SceneCamera> active_camera, Arc<SceneCameraRegistry> cameras)
        : m_cameras(Some(rstd::move(cameras))), m_active_camera(rstd::move(active_camera)) {}

    void Add(String name, Arc<SceneCamera> camera);

    auto Resolve(const SceneNode& node) const -> Option<mut_ref<SceneCamera>>;
    auto Active() const -> mut_ref<SceneCamera>;

private:
    Option<Arc<SceneCameraRegistry>>  m_cameras;
    HashMap<String, Arc<SceneCamera>> m_local_cameras;
    Arc<SceneCamera>                  m_active_camera;
};

struct UniformNodeState {
    Arc<SceneNode>                node;
    Arc<UniformCameraResolver>    camera_resolver;
    i32                           object_id { 0 };
    wpscene::ParallaxDepthBinding parallax;
    bool                          propagate_parallax_to_children { true };
    bool                          ride_parent_parallax { false };
    bool                          use_camera_eye_position { false };
    Option<array<float, 3>>       eye_position_override;
    bool                          vertices_in_world_space { false };
    bool                          reverse_depth { false };
    Option<Arc<SceneNode>>        effect_projection_node;
    array<float, 2>               effect_projection_size { 0.0f, 0.0f };

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
    void RegisterNodeParallaxContract(const SceneNode&, i32, const wpscene::ParallaxDepthBinding&);
    bool RemoveNodeState(SceneNodeId);
    void RemoveNodeParallaxContract(const SceneNode&);
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
    float                        MouseParallaxInfluence() const noexcept {
        return m_mouse_parallax ? m_camera_parallax.mouse_influence : 0.0f;
    }
    void                      SetMouseParallax(bool enabled) { m_mouse_parallax = enabled; }
    UniformCameraShake&       CameraShake() noexcept { return m_camera_shake; }
    const UniformCameraShake& CameraShake() const noexcept { return m_camera_shake; }
    const UniformFrameInputs& Inputs() const noexcept { return m_inputs; }
    array<float, 2>           Ortho() const noexcept { return m_ortho; }
    array<float, 3>           AmbientColor() const { return m_ambient_color; }
    array<float, 3>           SkylightColor() const { return m_skylight_color; }
    array<float, 3>           FogDistanceColor() const { return m_fog_distance_color; }
    array<float, 3>           FogHeightColor() const { return m_fog_height_color; }
    void                      SetAmbientColor(array<float, 3> color) { m_ambient_color = color; }
    void                      SetSkylightColor(array<float, 3> color) { m_skylight_color = color; }
    void SetFogDistanceColor(array<float, 3> color) { m_fog_distance_color = color; }
    void SetFogHeightColor(array<float, 3> color) { m_fog_height_color = color; }

    void SetOrtho(float width, float height) { m_ortho = { width, height }; }
    void SetOrthographicImplicitParallax(bool enabled) {
        m_orthographic_implicit_parallax = enabled;
    }
    void SetPointerInput(double, double);
    void SetAudioSpectrum(const scene_audio::Buffers&);
    void Advance(const SceneFrame&);
    void ApplyUserProperty(ref<str>, const Json&);
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
    HashMap<i32, usize>                              m_parallax_owner_counts;
    HashMap<const SceneNode*, array<float, 2>>       m_node_parallax_depths;

    auto LogicalParallaxState(const UniformNodeState&) const -> const UniformNodeState*;
    auto ParentParallaxState(const UniformNodeState&) const -> const UniformNodeState*;

    UniformFrameInputs       m_inputs;
    UniformCameraParallax    m_camera_parallax;
    UniformCameraShake       m_camera_shake;
    array<float, 3>          m_ambient_color {};
    array<float, 3>          m_skylight_color {};
    array<float, 3>          m_fog_distance_color {};
    array<float, 3>          m_fog_height_color {};
    bool                     m_orthographic_implicit_parallax { false };
    bool                     m_mouse_parallax { true };
    array<float, 2>          m_ortho { 1920.0f, 1080.0f };
    array<float, 2>          m_pointer_input { 0.5f, 0.5f };
    Arc<AudioResponseDemand> m_audio_demand;
};

class UniformRuntimeInput {
public:
    explicit UniformRuntimeInput(Arc<UniformSceneState> state): m_state(rstd::move(state)) {}

    void SetPointerInput(double x, double y) { m_state->SetPointerInput(x, y); }
    void SetMouseParallax(bool enabled) { m_state->SetMouseParallax(enabled); }
    void SetAudioSpectrum(const scene_audio::Buffers& buffers) {
        m_state->SetAudioSpectrum(buffers);
    }

private:
    Arc<UniformSceneState> m_state;
};

class UniformRuntimeSystem {
public:
    UniformRuntimeSystem(Arc<UniformSceneState> state, Scene& scene, bool orthographic)
        : m_state(rstd::move(state)), m_scene(scene), m_orthographic(orthographic) {}

    void Update(ref<SceneFrame> frame);

private:
    Arc<UniformSceneState> m_state;
    Scene&                 m_scene;
    bool                   m_orthographic;
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
    explicit LightUniformSource(ref<Scene> scene): m_scene(scene) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>> { return None(); }

private:
    ref<Scene> m_scene;
};

class SceneLightUniformSource {
public:
    SceneLightUniformSource(ref<Scene> scene, SceneLightingUniformConfig config,
                            Arc<ShadowAtlasLayout> shadows = Arc<ShadowAtlasLayout>::make())
        : m_scene(scene), m_config(config), m_shadows(rstd::move(shadows)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>> { return None(); }

private:
    ref<Scene>                 m_scene;
    SceneLightingUniformConfig m_config;
    Arc<ShadowAtlasLayout>     m_shadows;
};

class VolumetricUniformSource {
public:
    VolumetricUniformSource(ref<Scene> scene, Arc<SceneNode> light_node, VolumetricDrawKind kind,
                            Arc<ShadowAtlasLayout> shadows = Arc<ShadowAtlasLayout>::make())
        : m_scene(scene),
          m_light_node(rstd::move(light_node)),
          m_kind(kind),
          m_shadows(rstd::move(shadows)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>> { return None(); }

private:
    ref<Scene>             m_scene;
    Arc<SceneNode>         m_light_node;
    VolumetricDrawKind     m_kind;
    Arc<ShadowAtlasLayout> m_shadows;
};

class ShadowUniformSource {
public:
    ShadowUniformSource(Arc<SceneCamera> camera, ref<Scene> scene, Arc<ShadowAtlasLayout> shadows)
        : m_camera(rstd::move(camera)), m_scene(scene), m_shadows(rstd::move(shadows)) {}

    auto Describe(mut_ref<dyn<UniformBindingSink>>) const -> Result<empty, UniformError>;
    auto Version(ref<dyn<UniformUpdateContext>>) const -> u64;
    auto Evaluate(ref<dyn<UniformUpdateContext>>, mut_ref<dyn<UniformValueSink>>) const
        -> Result<empty, UniformError>;
    auto AcquireBindingLease() const -> Option<Box<dyn<UniformBindingLease>>> { return None(); }

private:
    Arc<SceneCamera>       m_camera;
    ref<Scene>             m_scene;
    Arc<ShadowAtlasLayout> m_shadows;
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
