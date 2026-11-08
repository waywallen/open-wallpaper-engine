export module wescene.pkg.parse:scene_context;
import wescene.texture_memory;
import eigen;

import rstd;
import wavsen.audio;
import wescene.fs;
import wescene.json;
import wescene.load_bench;
import wescene.scene;
import wescene.script;
import wescene.text;
import wescene.pkg.scene_obj;
import :scene_stages;

import wescene.pkg.puppet;
import :particle_runtime;
import :shader_parser;
import :uniform_source;
import :sound_parser;

using namespace rstd::prelude;
using rstd::collections::HashMap;
using rstd::collections::HashSet;
using rstd::sync::Arc;

export namespace owe
{

using SceneObjectVar      = wpscene::SceneObject;
using EffectRenderTargets = HashMap<String, String>;

struct SceneLayerBuildContext;
struct SceneParseContext;

struct PuppetLayerRegistry {
    HashMap<SceneNode*, Arc<PuppetLayer>> by_node;
    HashMap<SceneNode*, Arc<PuppetLayer>> fallback_by_node;
};

struct SceneShaderEnvironment {
    bool fog_distance { false };
    bool fog_height { false };
    bool directional_shadow { false };
};

struct GeometryShaderLimits {
    u32 max_output_vertices { 256 };
    u32 max_total_output_components { 1024 };
};

enum class GeometryStageRequirement
{
    None,
    Required,
    Disabled,
};

struct MaterialBuildError {
    String message;
};

struct MaterialBuild {
    SceneMaterial material;
    ShaderInfo    shader_info;
};

auto BuildMaterial(fs::VFS&, ShaderCache&, const SceneShaderEnvironment&, const wpscene::Material&,
                   Scene&, ShaderInfo = {},
                   GeometryStageRequirement   = GeometryStageRequirement::None,
                   SceneLoadBenchRecorderView = {}) -> Result<MaterialBuild, MaterialBuildError>;
auto ApplyLayerColorBlend(wpscene::Material&, i32 color_blend_mode) -> Option<BlendMode>;
auto NeutralColorUniforms(ShaderValueMap) -> ShaderValueMap;
void ParseSpecTexName(String&, const wpscene::Material&, const ShaderInfo&, Scene&);
auto IsLegacyAtmosphereShadowValue(const wpscene::Material&, ref<str>) -> bool;
void RegisterMaterialBindings(Scene&, const Arc<SceneMaterial>&, const wpscene::Material&,
                              const ShaderInfo&,
                              Option<ref<wpscene::Material>> user_texture_fallback = None(),
                              SceneNodeId                    owner                 = {});
void RegisterLayerPreviousBindings(Scene&, SceneMaterial&, const wpscene::Material&, SceneNodeId,
                                   ref<str> composite_target);
void ApplyTextureBinds(wpscene::Material&, slice<wpscene::MaterialPassBindItem>,
                       const EffectRenderTargets&);
void LoadConstvalue(SceneLayerBuildContext&, SceneMaterial&, const wpscene::Material&,
                    const ShaderInfo&, SceneShaderValueAnimationMap* = nullptr);

using DirectDrawQuad = array<array<float, 2>, 4>;
void GenCardMesh(SceneMesh&, array<float, 2>, array<float, 2> = { 1.0f, 1.0f },
                 const Eigen::Vector3f& = Eigen::Vector3f::Zero());
auto ReadDirectDrawQuad(const wpscene::Material&) -> Option<DirectDrawQuad>;
void GenDirectDrawQuadMesh(SceneMesh&, float, const DirectDrawQuad&);
void SetParticleMesh(SceneMesh&, u32, bool, bool geometry_shader_supported);
void SetRopeParticleMesh(SceneMesh&, const wpscene::Particle&, u32, bool, bool,
                         bool geometry_shader_supported);

struct SceneUniformConfigDraft {
    Arc<SceneNode>         node;
    UniformNodeConfigDraft config;
};

struct ParticleTrailUniformConfigDraft {
    Arc<SceneNode>                 node;
    Arc<ParticleTrailUniformState> uniform_state;
};

struct SceneLayerNodeDraft {
    u32                                            parent_id { 0 };
    Option<Arc<SceneNode>>                         node;
    Option<Arc<Puppet>>                            puppet;
    String                                         attachment;
    Option<Arc<PuppetLayer>>                       puppet_layer;
    Option<Box<dyn<FnMut<void(Eigen::Vector3f)>>>> apply_attachment_offset;
    Vec<Arc<SceneNode>>                            ordered_before_nodes;
};

struct TextUniformConfigDraft {
    Arc<SceneNode>                               node;
    Option<Arc<text::TextEffectProjectionState>> effect_projection;
};

struct SceneLayerBuildResult {
    HashMap<i32, SceneLayerNodeDraft>    node_id_map;
    Vec<i32>                             node_id_order;
    Vec<SceneUniformConfigDraft>         uniform_configs;
    Vec<TextUniformConfigDraft>          text_uniform_configs;
    Vec<ParticleTrailUniformConfigDraft> particle_trail_uniform_configs;
};

struct SceneLayerResources {
    explicit SceneLayerResources(Arc<AudioResponseDemand> demand)
        : audio_response_demand(rstd::move(demand)),
          uniform_state(Arc<UniformSceneState>::make(audio_response_demand.clone())) {}

    i32                               ortho_w { 0 };
    i32                               ortho_h { 0 };
    bool                              orthographic_scene { false };
    wpscene::SceneVersion             pkg_version { wpscene::kSceneVersionUnknown };
    Option<Arc<ParticleRuntime>>      particle_runtime;
    Option<SceneSoundMountCallback>   sound_mount;
    Option<SceneSoundUnmountCallback> sound_unmount;
    Arc<ShaderCache>                  shader_cache { Arc<ShaderCache>::make() };
    ShaderValueMap                    global_base_uniforms;
    SceneShaderEnvironment            shader_environment;
    GeometryShaderLimits              geometry_shader_limits;
    bool                              geometry_shader_supported { true };
    Arc<AudioResponseDemand>          audio_response_demand;
    Arc<UniformSceneState>            uniform_state;
    Arc<PuppetLayerRegistry>          puppet_layers { Arc<PuppetLayerRegistry>::make() };
    HashMap<i32, u32>                 object_parent_ids;
    HashSet<i32>                      solid_layer_ids;
    HashMap<i32, String>              system_media_image_fallbacks;
    HashSet<i32>                      linked_source_ids;
    HashSet<i32>                      hidden_link_source_ids;
    bool                              scene_has_scripts { false };
    bool                              scene_layer_text_writes { false };
};

struct SceneLayerBuildContext {
    SceneLayerBuildContext(Scene& owner, fs::VFS& filesystem, Arc<SceneLayerResources> resources)
        : scene(rstd::addressof(owner)),
          layer_resources(rstd::move(resources)),
          vfs(rstd::addressof(filesystem)) {}

    SceneLoadBenchRecorderView           load_bench;
    Scene*                               scene;
    Arc<SceneLayerResources>             layer_resources;
    fs::VFS*                             vfs { nullptr };
    Option<ref<rstd::json::Map>>         user_properties;
    HashMap<String, vectext::FontSource> font_sources;

    Option<Arc<SceneNode>> effect_camera_node;
    Option<Arc<SceneNode>> global_camera_node;
    Option<Arc<SceneNode>> global_perspective_camera_node;

    Option<Box<owe::script::ScriptScene>> script_scene;
    using ImageAlignmentSetter = Arc<dyn<FnMut<void(SceneNode*, ref<str>)>>>;
    struct ImageAlignmentBinding {
        SceneNode*           node { nullptr };
        String               alignment;
        ImageAlignmentSetter setter;
    };
    Vec<ImageAlignmentBinding> image_alignment_bindings;
    SceneLayerBuildResult      layer_build;
    SceneAnimationBindingScope animation_bindings;

    HashMap<i32, rstd::uint64_t> script_initialization_orders;
    HashSet<i32>                 parallax_depth_user_binding_ids;
    HashSet<i32>                 ride_parent_parallax_ids;

    Vec<owe::script::FieldScript*>    registered_asset_scripts;
    script::FieldScriptInitialization script_initialization {
        script::FieldScriptInitialization::Automatic
    };
    Vec<owe::script::FieldScript*> deferred_scripts;
    Vec<ScenePreparedSound>        pending_sounds;
    bool                           runtime_created { false };
    bool                           track_created_nodes { false };
    Vec<Arc<SceneNode>>            created_nodes;

    void TrackCreatedNode(const Arc<SceneNode>& node) {
        if (! track_created_nodes) return;
        for (const auto& tracked : created_nodes)
            if (tracked.as_ptr() == node.as_ptr()) return;
        created_nodes.push(node.clone());
    }

    bool IsLinkedSource(i32 id) const {
        return layer_resources->linked_source_ids.contains(i32(id));
    }
};

struct SceneParseContext : SceneLayerBuildContext {
    SceneParseContext(Scene& owner, fs::VFS& filesystem)
        : SceneLayerBuildContext(owner, filesystem,
                                 Arc<SceneLayerResources>::make(owner.AudioDemandHandle())) {}

    HashMap<i32, Json>                       initial_layer_configs;
    Option<ref<rstd::json::Map>>             general_settings;
    i32                                      next_synthetic_object_id { -1 };
    HashMap<String, wpscene::ParticleObject> dynamic_particle_prototypes;
    wavsen::audio::SoundManager*             sound_manager { nullptr };

    auto NextSyntheticObjectId() -> i32;
};

void SetUniformConfig(SceneLayerBuildContext&, const Arc<SceneNode>&, UniformNodeConfigDraft);
void ApplyParallaxUniformConfig(SceneLayerBuildContext&, const Arc<SceneNode>&,
                                const wpscene::ParallaxDepthBinding& parallax, i32 object_id,
                                bool propagate_to_children = true);
void RegisterNodeRef(SceneLayerBuildContext&, i32, SceneLayerNodeDraft);

bool SceneWritesLayerText(slice<SceneObjectVar>);
bool SceneHasScripts(slice<SceneObjectVar>);
auto LoadJsonFile(fs::VFS&, ref<str>) -> Option<Json>;
bool ResolveVisibleUserBinding(bool&, const wpscene::VisibleUserBinding&,
                               Option<ref<rstd::json::Map>>);
bool AppendLayerCompositePassthroughEffect(fs::VFS&, wpscene::ImageObject&);
auto MakePuppetLayer(Arc<Puppet>, slice<wpscene::PuppetAnimationLayer>) -> Arc<PuppetLayer>;
void WirePuppetAnimationScripts(SceneLayerBuildContext&, SceneNode*, Arc<PuppetLayer>,
                                slice<wpscene::PuppetAnimationLayer>);
void RegisterPuppetLayer(SceneLayerBuildContext&, SceneNode*, Arc<PuppetLayer>);
void MarkHiddenLinkSource(SceneLayerBuildContext&, i32);
auto ToSceneUserVisibilityBinding(const wpscene::VisibleUserBinding&) -> SceneUserVisibilityBinding;
auto Texture0UvScale(const SceneMaterial&, bool = false) -> array<float, 2>;
auto ParticleTextureRatio(const SceneMaterial&) -> float;
void RegisterImageAlignmentBinding(SceneLayerBuildContext&, SceneNode*, ref<str>,
                                   SceneLayerBuildContext::ImageAlignmentSetter);
auto EnsureScriptScene(SceneLayerBuildContext&) -> script::ScriptScene&;
void SetScriptInitializationOrder(SceneLayerBuildContext&, script::FieldScript&, const SceneNode*);
void TrackFieldScript(SceneLayerBuildContext&, script::FieldScript*);
auto ScriptValueAsFloat(const script::ScriptValue&) -> Option<float>;
auto ScriptValueAsVec2(const script::ScriptValue&) -> Option<array<float, 2>>;
auto ScriptValueAsVec3(const script::ScriptValue&, const Eigen::Vector3f&)
    -> Option<Eigen::Vector3f>;
void WireFieldScripts(SceneLayerBuildContext&, const Arc<SceneNode>&, const wpscene::FieldBindings&,
                      Option<Arc<dyn<FnMut<void(const script::ScriptValue&)>>>> = {},
                      Option<Arc<dyn<FnMut<void(const script::ScriptValue&)>>>> = {});
void WireImageEffectVisibilityScript(SceneLayerBuildContext&, SceneNode*,
                                     const wpscene::ImageEffect&, SceneEffectId);
void WireGeneralFieldScripts(SceneLayerBuildContext&, const wpscene::FieldBindings&);
void WireCameraFieldScripts(SceneLayerBuildContext&, const Arc<SceneNode>&, const Arc<SceneCamera>&,
                            const Arc<SceneCameraPath>&, const wpscene::FieldBindings&,
                            const Eigen::Vector3f&, const Eigen::Vector3f&);
void WireMaterialShaderValueScripts(SceneLayerBuildContext&, const Arc<SceneNode>&,
                                    const Arc<SceneMaterial>&, const wpscene::Material&,
                                    const ShaderInfo&);
auto ScriptValueAsShaderValue(const script::ScriptValue&) -> Option<ShaderValue>;
auto UsesUnitFinalQuad(const wpscene::Material&) -> bool;
auto HasSolidCompositeContext(const SceneLayerBuildContext&, const wpscene::ImageObject&) -> bool;
auto CanCompositeFinalEffectMaterial(ref<str>, const ShaderInfo&, bool) -> bool;
auto AlignmentOffset(ref<str>, Eigen::Vector2f) -> Eigen::Vector3f;
void ApplyUserTextureBindings(SceneLayerBuildContext&, wpscene::Material&);
void IndexSystemMediaImageFallbacks(SceneLayerBuildContext&, slice<SceneObjectVar>);

struct ParticleObjectParseServices {
    Scene*                  scene { nullptr };
    fs::VFS*                vfs { nullptr };
    Arc<ShaderCache>        shader_cache;
    SceneShaderEnvironment  shader_environment;
    GeometryShaderLimits    geometry_shader_limits;
    bool                    geometry_shader_supported { true };
    ShaderValueMap          global_base_uniforms;
    Arc<ParticleRuntime>    particle_runtime;
    i32                     ortho_w { 0 };
    i32                     ortho_h { 0 };
    SceneLayerBuildContext& construction_context;
    bool                    wire_scripts { true };
};

struct ParticleObjectParseOutput {
    Option<Arc<SceneNode>>               root;
    Vec<SceneUniformConfigDraft>         uniform_configs;
    Vec<ParticleTrailUniformConfigDraft> trail_uniform_configs;
};

auto BuildParticleObject(ParticleObjectParseServices&, wpscene::ParticleObject&)
    -> ParticleObjectParseOutput;
bool RegisterUniformNodeSources(Scene&, const Arc<UniformSceneState>&,
                                const Arc<UniformCameraResolver>&, const Arc<SceneNode>&,
                                const UniformNodeConfigDraft&);
bool RegisterParticleTrailUniformSource(Scene&, const Arc<SceneNode>&,
                                        const Arc<ParticleTrailUniformState>&);
void RegisterLayerUniformSources(Scene&, const Arc<UniformSceneState>&,
                                 const Arc<UniformCameraResolver>&, const SceneLayerBuildResult&);
void InstallRuntimeLayerFactory(SceneParseContext&, const Arc<UniformCameraResolver>&);
auto WorkshopAssetPath(const script::LayerAssetReference&) -> Option<String>;
auto ResolveTextureSize(Scene&, ref<str>) -> Option<array<float, 2>>;
auto InstantiateRegisteredAsset(SceneParseContext&, SceneNode*, const script::LayerAssetReference&)
    -> Option<Arc<SceneNode>>;
auto InstantiateLayerConfiguration(SceneParseContext&, SceneNode*, const Json&)
    -> Option<Arc<SceneNode>>;
void ResolveRegisteredAssets(SceneParseContext&);
void ParseImageObj(SceneLayerBuildContext&, wpscene::ImageObject&);
void ParseShapeObj(SceneLayerBuildContext&, wpscene::ShapeObject&);
void ParseParticleObj(SceneParseContext&, wpscene::ParticleObject&);
void ParseSoundObj(SceneParseContext&, wpscene::SoundObject&, wavsen::audio::SoundManager&);
bool ParseSoundObj(SceneLayerBuildContext&, wpscene::SoundObject&);
void ParseModelObj(SceneParseContext&, wpscene::ModelObject&);
void ParseModelObj(SceneLayerBuildContext&, wpscene::ModelObject&);
bool RegisterRuntimePuppetUniformSources(Scene&, SceneLayerResources&, slice<Arc<SceneNode>>);
void ParseTextObj(SceneLayerBuildContext&, wpscene::TextObject&);

struct ExpandedSceneObjects {
    Vec<SceneObjectVar> objects;
    HashSet<i32>        linked_source_ids;
    HashSet<i32>        hidden_link_source_ids;
};

auto ExpandSceneObjects(ref<wpscene::SceneDocument>, mut_ref<fs::VFS>, Option<ref<rstd::json::Map>>)
    -> ExpandedSceneObjects;

void PrepareAnimationBindings(SceneLayerBuildContext&, const wpscene::FieldBindings&);
void PrepareAnimationBindings(SceneLayerBuildContext&, const wpscene::ContainerObject&);
void PrepareAnimationBindings(SceneLayerBuildContext&, const wpscene::ImageObject&);
void PrepareAnimationBindings(SceneLayerBuildContext&, const wpscene::ShapeObject&);
void PrepareAnimationBindings(SceneLayerBuildContext&, const wpscene::ParticleObject&);
void PrepareAnimationBindings(SceneLayerBuildContext&, const wpscene::SoundObject&);
void PrepareAnimationBindings(SceneLayerBuildContext&, const wpscene::LightObject&);
void PrepareAnimationBindings(SceneLayerBuildContext&, const wpscene::TextObject&);
void PrepareAnimationBindings(SceneLayerBuildContext&, const wpscene::ModelObject&);
void PrepareAnimationBindings(SceneLayerBuildContext&, const wpscene::CameraObject&);
auto ResolveAnimationTrack(const SceneLayerBuildContext&, const wpscene::FieldBindingSpec&)
    -> SceneAnimationTrack;
void AssignNodeFieldAnimations(SceneLayerBuildContext&, SceneNode&, const wpscene::FieldBindings&);
void AssignCameraFieldAnimations(SceneLayerBuildContext&, SceneNode&, SceneCameraPath&,
                                 const wpscene::FieldBindings&);
void LoadCameraObjectPath(SceneLayerBuildContext&, const wpscene::CameraObject&, SceneCameraPath&);
auto ToSceneAnimationCurve(const wpscene::AnimCurve&) -> SceneAnimationCurve;
auto ToSceneAnimationTrack(const wpscene::AnimCurve&) -> SceneAnimationTrack;
void LoadRootCameraPaths(SceneLayerBuildContext&, const wpscene::SceneMetadata&);

struct ProcessOpts {
    enum Kind : unsigned
    {
        Image    = 1u << 0,
        Particle = 1u << 1,
        Sound    = 1u << 2,
        Light    = 1u << 3,
        Text     = 1u << 4,
        Model    = 1u << 5,
        All      = 0x3Fu,
    };
    unsigned kinds { All };
};

SceneParseContext
BuildContext(Scene&, fs::VFS&, ref<str> scene_id, const wpscene::SceneMetadata&,
             array<i32, 2> ortho_extent, Option<ref<rstd::json::Map>> user_properties = None(),
             Option<rstd::path::PathBuf> shader_cache_dir = None(),
             GeometryShaderLimits geometry_limits = {}, bool directional_shadow = false,
             bool geometry_shader_supported = true, Option<ImageReadyCallback> image_ready = None(),
             TextureMemoryOptions texture_memory = {});

void IndexSceneDocument(SceneParseContext&, ref<wpscene::SceneDocument>, slice<SceneObjectVar>);
void ProcessContainers(SceneParseContext&, mut_ref<SceneObjectVar[]>);

void ProcessObjects(SceneParseContext&, mut_ref<SceneObjectVar[]>, wavsen::audio::SoundManager* sm,
                    ProcessOpts opts = {}, SceneLoadBenchRecorderView load_bench = {});

void FinalizeScene(SceneParseContext&);
void BuildBloomPostProcess(SceneParseContext&, fs::VFS&, const wpscene::SceneGeneral&);

} // namespace owe
