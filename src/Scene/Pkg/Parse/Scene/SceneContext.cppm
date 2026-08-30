export module wescene.pkg.parse:scene_context;
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

using namespace rstd::prelude;
using rstd::collections::HashMap;
using rstd::collections::HashSet;
using rstd::sync::Arc;

export namespace owe
{

using SceneObjectVar      = wpscene::SceneObject;
using EffectRenderTargets = HashMap<String, String>;

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
                   GeometryStageRequirement = GeometryStageRequirement::None)
    -> Result<MaterialBuild, MaterialBuildError>;
auto ApplyImageColorBlend(wpscene::Material&, const wpscene::ImageObject&) -> Option<BlendMode>;
auto NeutralColorUniforms(ShaderValueMap) -> ShaderValueMap;
auto CountVisibleImageEffects(std::span<const wpscene::ImageEffect>) -> i32;
void ParseSpecTexName(std::string&, const wpscene::Material&, const ShaderInfo&, Scene&);
auto IsLegacyAtmosphereShadowValue(const wpscene::Material&, std::string_view) -> bool;
void RegisterMaterialBindings(Scene&, const std::shared_ptr<SceneMaterial>&,
                              const wpscene::Material&, const ShaderInfo&,
                              Option<ref<wpscene::Material>> user_texture_fallback = None());
void RegisterLayerPreviousBindings(Scene&, SceneMaterial&, const wpscene::Material&, SceneNodeId,
                                   ref<str> composite_target);
void ApplyTextureBinds(wpscene::Material&, std::span<const wpscene::MaterialPassBindItem>,
                       const EffectRenderTargets&);
void LoadConstvalue(SceneParseContext&, SceneMaterial&, const wpscene::Material&, const ShaderInfo&,
                    SceneShaderValueAnimationMap* = nullptr);

using DirectDrawQuad = array<array<float, 2>, 4>;
void GenCardMesh(SceneMesh&, array<float, 2>, array<float, 2> = { 1.0f, 1.0f },
                 const Eigen::Vector3f& = Eigen::Vector3f::Zero());
auto ReadDirectDrawQuad(const wpscene::Material&) -> Option<DirectDrawQuad>;
void GenDirectDrawQuadMesh(SceneMesh&, float, const DirectDrawQuad&);
void SetParticleMesh(SceneMesh&, u32, bool);
void SetRopeParticleMesh(SceneMesh&, const wpscene::Particle&, u32, bool, bool);

struct SceneUniformConfigDraft {
    Arc<SceneNode>         node;
    UniformNodeConfigDraft config;
};

struct ParticleTrailUniformConfigDraft {
    Arc<SceneNode>                 node;
    Arc<ParticleTrailUniformState> uniform_state;
};

class ParseSceneHandle {
public:
    ParseSceneHandle(): m_scene(Box<Scene>::make()) {}

    Scene*       get() noexcept { return m_scene.get(); }
    const Scene* get() const noexcept { return m_scene.as_ptr().as_raw_ptr(); }
    Scene*       operator->() noexcept { return m_scene.get(); }
    const Scene* operator->() const noexcept { return m_scene.as_ptr().as_raw_ptr(); }
    Scene&       operator*() noexcept { return *m_scene; }
    const Scene& operator*() const noexcept { return *m_scene; }

    auto Take() -> Box<Scene> { return rstd::move(m_scene); }

private:
    Box<Scene> m_scene;
};

struct SceneParseContext {
    ParseSceneHandle                               scene;
    Option<Arc<ParticleRuntime>>                   particle_runtime;
    i32                                            ortho_w { 0 };
    i32                                            ortho_h { 0 };
    bool                                           orthographic_scene { false };
    wpscene::SceneVersion                          pkg_version { wpscene::kSceneVersionUnknown };
    fs::VFS*                                       vfs { nullptr };
    Option<ref<rstd::json::Map>>                   user_properties;
    Arc<ShaderCache>                               shader_cache { Arc<ShaderCache>::make() };
    HashMap<String, text::FontCache::ResolvedBlob> font_sources;

    ShaderValueMap         global_base_uniforms;
    SceneShaderEnvironment shader_environment;
    GeometryShaderLimits   geometry_shader_limits;
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
    Vec<ImageAlignmentBinding>   image_alignment_bindings;
    Arc<PuppetLayerRegistry>     puppet_layers { Arc<PuppetLayerRegistry>::make() };
    Vec<SceneUniformConfigDraft> uniform_configs;
    Arc<AudioResponseDemand>     audio_response_demand { scene->AudioDemandHandle() };
    Arc<UniformSceneState>       uniform_state { Arc<UniformSceneState>::make(
        audio_response_demand.clone()) };
    struct TextUniformConfigDraft {
        Arc<SceneNode>                                   node;
        std::shared_ptr<text::TextEffectProjectionState> effect_projection;
    };
    Vec<TextUniformConfigDraft>          text_uniform_configs;
    Vec<ParticleTrailUniformConfigDraft> particle_trail_uniform_configs;
    SceneAnimationBindingScope           animation_bindings;

    struct NodeRef {
        u32                                            parent_id { 0 };
        Option<Arc<SceneNode>>                         node;
        Option<Arc<Puppet>>                            puppet;
        String                                         attachment;
        Option<Arc<PuppetLayer>>                       puppet_layer;
        Option<Box<dyn<FnMut<void(Eigen::Vector3f)>>>> apply_attachment_offset;
        Vec<Arc<SceneNode>>                            ordered_before_nodes;
    };
    HashMap<i32, NodeRef>       node_id_map;
    HashMap<i32, u32>           object_parent_ids;
    HashSet<i32>                solid_layer_ids;
    Vec<i32>                    node_id_order;
    HashMap<i32, std::uint64_t> script_initialization_orders;
    HashMap<i32, Json>          initial_layer_configs;
    HashSet<i32>                parallax_depth_user_binding_ids;
    HashSet<i32>                ride_parent_parallax_ids;

    i32                             next_synthetic_object_id { -1 };
    Vec<owe::script::FieldScript*>  registered_asset_scripts;
    HashMap<String, Arc<SceneNode>> dynamic_model_prototypes;
    struct DynamicImagePrototype {
        Arc<SceneNode>         node;
        UniformNodeConfigDraft uniform_config;
    };
    HashMap<String, DynamicImagePrototype>   dynamic_image_prototypes;
    HashMap<String, wpscene::ParticleObject> dynamic_particle_prototypes;
    wavsen::audio::SoundManager*             sound_manager { nullptr };

    HashMap<i32, String> system_media_image_fallbacks;
    HashSet<i32>         linked_source_ids;
    HashSet<i32>         hidden_link_source_ids;
    bool                 scene_has_scripts { false };
    bool                 scene_layer_text_writes { false };

    bool IsLinkedSource(i32 id) const { return linked_source_ids.contains(i32(id)); }
    auto NextSyntheticObjectId() -> i32;
};

void SetUniformConfig(SceneParseContext&, const Arc<SceneNode>&, UniformNodeConfigDraft);
void ApplyParallaxUniformConfig(SceneParseContext&, const Arc<SceneNode>&,
                                const wpscene::ParallaxDepthBinding& parallax, i32 object_id,
                                bool propagate_to_children = true);
auto FindUniformConfig(const SceneParseContext&, const SceneNode&) -> const UniformNodeConfigDraft*;
void RegisterNodeRef(SceneParseContext&, i32, SceneParseContext::NodeRef);

bool SceneWritesLayerText(slice<SceneObjectVar>);
bool SceneHasScripts(slice<SceneObjectVar>);
auto LoadJsonFile(fs::VFS&, const std::string&) -> Option<Json>;
bool AppendLayerCompositePassthroughEffect(fs::VFS&, wpscene::ImageObject&);
auto MakePuppetLayer(Arc<Puppet>, std::span<PuppetLayer::AnimationLayer>) -> Arc<PuppetLayer>;
void RegisterPuppetLayer(SceneParseContext&, SceneNode*, Arc<PuppetLayer>);
void MarkHiddenLinkSource(SceneParseContext&, i32);
auto ToSceneUserVisibilityBinding(const wpscene::VisibleUserBinding&) -> SceneUserVisibilityBinding;
auto Texture0UvScale(const SceneMaterial&, bool = false) -> array<float, 2>;
auto ParticleTextureRatio(const SceneMaterial&) -> float;
void RegisterImageAlignmentBinding(SceneParseContext&, SceneNode*, ref<str>,
                                   SceneParseContext::ImageAlignmentSetter);
auto EnsureScriptScene(SceneParseContext&) -> script::ScriptScene&;
void SetScriptInitializationOrder(SceneParseContext&, script::FieldScript&, const SceneNode*);
void TrackRegisteredAssets(SceneParseContext&, script::FieldScript*);
auto ScriptValueAsFloat(const script::ScriptValue&) -> Option<float>;
auto ScriptValueAsVec2(const script::ScriptValue&) -> Option<array<float, 2>>;
auto ScriptValueAsVec3(const script::ScriptValue&, const Eigen::Vector3f&)
    -> Option<Eigen::Vector3f>;
void WireFieldScripts(SceneParseContext&, const Arc<SceneNode>&, const wpscene::FieldBindings&,
                      std::function<void(const script::ScriptValue&)> = {},
                      std::function<void(const script::ScriptValue&)> = {});
void WireImageEffectVisibilityScript(SceneParseContext&, SceneNode*, const wpscene::ImageEffect&,
                                     SceneEffectId);
void WireCameraShakeScripts(SceneParseContext&, const wpscene::FieldBindings&);
void WireCameraFieldScripts(SceneParseContext&, const Arc<SceneNode>&, const Arc<SceneCamera>&,
                            const Arc<SceneCameraPath>&, const wpscene::FieldBindings&,
                            const Eigen::Vector3f&, const Eigen::Vector3f&);
void WireMaterialShaderValueScripts(SceneParseContext&, const Arc<SceneNode>&,
                                    const std::shared_ptr<SceneMaterial>&, const wpscene::Material&,
                                    const ShaderInfo&);
auto UsesUnitFinalQuad(const wpscene::Material&) -> bool;
auto HasSolidCompositeContext(const SceneParseContext&, const wpscene::ImageObject&) -> bool;
auto CanCompositeFinalEffectMaterial(std::string_view, const ShaderInfo&, bool) -> bool;
auto AlignmentOffset(ref<str>, Eigen::Vector2f) -> Eigen::Vector3f;
void ApplyUserTextureBindings(SceneParseContext&, wpscene::Material&);
void IndexSystemMediaImageFallbacks(SceneParseContext&, slice<SceneObjectVar>);

struct ParticleObjectParseServices {
    Scene*                 scene { nullptr };
    fs::VFS*               vfs { nullptr };
    Arc<ShaderCache>       shader_cache;
    SceneShaderEnvironment shader_environment;
    GeometryShaderLimits   geometry_shader_limits;
    ShaderValueMap         global_base_uniforms;
    Arc<ParticleRuntime>   particle_runtime;
    i32                    ortho_w { 0 };
    i32                    ortho_h { 0 };
    SceneParseContext*     construction_context { nullptr };
};

struct ParticleObjectParseOutput {
    Option<Arc<SceneNode>>               root;
    Vec<SceneUniformConfigDraft>         uniform_configs;
    Vec<ParticleTrailUniformConfigDraft> trail_uniform_configs;
};

auto BuildParticleObject(ParticleObjectParseServices&, wpscene::ParticleObject&)
    -> ParticleObjectParseOutput;
auto CloneRegisteredNode(Scene&, ref<SceneNode>, ref<str>) -> Arc<SceneNode>;
auto WorkshopAssetPath(const script::LayerAssetReference&) -> Option<String>;
auto InstantiateRegisteredAsset(SceneParseContext&, SceneNode*, const script::LayerAssetReference&)
    -> Option<Arc<SceneNode>>;
auto InstantiateLayerConfiguration(SceneParseContext&, SceneNode*, const Json&)
    -> Option<Arc<SceneNode>>;
void ResolveRegisteredAssets(SceneParseContext&);
void ParseImageObj(SceneParseContext&, wpscene::ImageObject&);
void ParseShapeObj(SceneParseContext&, wpscene::ShapeObject&);
void ParseParticleObj(SceneParseContext&, wpscene::ParticleObject&);
void ParseSoundObj(SceneParseContext&, wpscene::SoundObject&, wavsen::audio::SoundManager&);
void ParseModelObj(SceneParseContext&, wpscene::ModelObject&);
void ParseTextObj(SceneParseContext&, wpscene::TextObject&);

struct ExpandedSceneObjects {
    Vec<SceneObjectVar> objects;
    HashSet<i32>        linked_source_ids;
    HashSet<i32>        hidden_link_source_ids;
};

auto ExpandSceneObjects(ref<wpscene::SceneDocument>, mut_ref<fs::VFS>, Option<ref<rstd::json::Map>>)
    -> ExpandedSceneObjects;

void PrepareAnimationBindings(SceneParseContext&, const wpscene::FieldBindings&);
void PrepareAnimationBindings(SceneParseContext&, const wpscene::ContainerObject&);
void PrepareAnimationBindings(SceneParseContext&, const wpscene::ImageObject&);
void PrepareAnimationBindings(SceneParseContext&, const wpscene::ShapeObject&);
void PrepareAnimationBindings(SceneParseContext&, const wpscene::ParticleObject&);
void PrepareAnimationBindings(SceneParseContext&, const wpscene::SoundObject&);
void PrepareAnimationBindings(SceneParseContext&, const wpscene::LightObject&);
void PrepareAnimationBindings(SceneParseContext&, const wpscene::TextObject&);
void PrepareAnimationBindings(SceneParseContext&, const wpscene::ModelObject&);
void PrepareAnimationBindings(SceneParseContext&, const wpscene::CameraObject&);
auto ResolveAnimationTrack(const SceneParseContext&, const wpscene::FieldBindingSpec&)
    -> SceneAnimationTrack;
void AssignNodeFieldAnimations(SceneParseContext&, SceneNode&, const wpscene::FieldBindings&);
void AssignCameraFieldAnimations(SceneParseContext&, SceneNode&, SceneCameraPath&,
                                 const wpscene::FieldBindings&);
void LoadCameraObjectPath(SceneParseContext&, const wpscene::CameraObject&, SceneCameraPath&);
auto ToSceneAnimationCurve(const wpscene::AnimCurve&) -> SceneAnimationCurve;
auto ToSceneAnimationTrack(const wpscene::AnimCurve&) -> SceneAnimationTrack;
void LoadRootCameraPaths(SceneParseContext&, const wpscene::SceneMetadata&);

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

SceneParseContext BuildContext(fs::VFS&, ref<str> scene_id, const wpscene::SceneMetadata&,
                               array<i32, 2>                ortho_extent,
                               Option<ref<rstd::json::Map>> user_properties    = None(),
                               Option<rstd::path::PathBuf>  shader_cache_dir   = None(),
                               GeometryShaderLimits         geometry_limits    = {},
                               bool                         directional_shadow = false);

void IndexSceneDocument(SceneParseContext&, ref<wpscene::SceneDocument>, slice<SceneObjectVar>);
void ProcessContainers(SceneParseContext&, mut_ref<SceneObjectVar[]>);

void ProcessObjects(SceneParseContext&, mut_ref<SceneObjectVar[]>, wavsen::audio::SoundManager* sm,
                    ProcessOpts opts = {}, SceneLoadBenchRecorderView load_bench = {});

Box<Scene> FinalizeScene(SceneParseContext&);
void       BuildBloomPostProcess(SceneParseContext&, fs::VFS&, const wpscene::SceneGeneral&);

} // namespace owe
