module;
#include <rstd/enum.hpp>

export module wescene.scene;
import eigen;
import rstd;
import wescene.core;
import wescene.json;
import rstd.cppstd;
import wescene.types;
import wescene.spec_names;

// SceneLight + SceneLightType live in this partition. Re-exported here so
// existing `import wescene.scene` consumers see them transparently.
export import :visibility;
export import :lighting;
export import :id;
export import :runtime;
export import :uniform;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::any::Any;
using rstd::collections::BTreeMap;
using rstd::collections::BTreeSet;
using rstd::collections::HashMap;
using rstd::collections::HashSet;
using rstd::sync::Arc;
using rstd::sync::atomic::Atomic;
using rstd::sync::atomic::Ordering;

export namespace owe
{

// ============================================================================
// SceneShader.h
// ============================================================================
using ShaderCode = std::vector<unsigned int>;

struct ShaderAttribute {
public:
    std::string name;
    u32         location;
};

struct SceneSamplerBinding {
    std::size_t texture_slot { 0 };
    std::string shader_member;

    bool operator==(const SceneSamplerBinding&) const = default;
};

enum class SceneShaderUniformBlockScope : rstd::uint8_t
{
    Shared,
    Local,
};

struct SceneShaderUniformBlockInterface {
    std::string                  name;
    u32                          set {};
    u32                          binding {};
    SceneShaderUniformBlockScope scope { SceneShaderUniformBlockScope::Local };
    u64                          identity {};

    bool operator==(const SceneShaderUniformBlockInterface&) const = default;
};

struct SceneShaderDescriptorBindingInterface {
    std::string name;
    u32         binding {};
    u32         descriptor_type {};
    u32         descriptor_count { u32(1) };
    u32         stage_flags {};

    bool operator==(const SceneShaderDescriptorBindingInterface&) const = default;
};

struct SceneShaderDescriptorSetInterface {
    u32                                                set {};
    bool                                               push_descriptor { false };
    u64                                                identity {};
    std::vector<SceneShaderDescriptorBindingInterface> bindings;

    bool operator==(const SceneShaderDescriptorSetInterface&) const = default;
};

struct SceneShader {
public:
    u32                    id { 0 };
    std::string            name;
    ShaderMatrixConvention matrix_convention { ShaderMatrixConvention::ColumnVector };
    ShaderMatrixAbi        matrix_abi { ShaderMatrixAbi::NativeSpirv };

    std::vector<ShaderCode> codes;

    std::vector<ShaderAttribute>                   attrs;
    std::vector<SceneSamplerBinding>               sampler_bindings;
    std::vector<SceneShaderUniformBlockInterface>  uniform_blocks;
    std::vector<SceneShaderDescriptorSetInterface> descriptor_sets;
    ShaderValues                                   default_uniforms;

    std::string_view SamplerMember(std::size_t texture_slot) const {
        for (const auto& binding : sampler_bindings) {
            if (binding.texture_slot == texture_slot) return binding.shader_member;
        }
        return {};
    }
};

inline usize SceneShaderStageCodeHash(const ShaderCode& code) {
    std::size_t seed { 0 };
    utils::hash_combine(seed, code.size());
    for (auto word : code) utils::hash_combine(seed, word);
    return usize(seed);
}

inline usize SceneShaderCodeHash(std::span<const ShaderCode> codes) {
    std::size_t seed { 0 };
    utils::hash_combine(seed, codes.size());
    for (const auto& code : codes) {
        utils::hash_combine(seed, SceneShaderStageCodeHash(code).to_primitive());
    }
    return usize(seed);
}

inline usize SceneShaderCodeHash(const SceneShader& shader) {
    std::size_t seed = SceneShaderCodeHash(shader.codes).to_primitive();
    utils::hash_combine(seed, static_cast<unsigned>(shader.matrix_convention));
    utils::hash_combine(seed, static_cast<unsigned>(shader.matrix_abi));
    for (const auto& set : shader.descriptor_sets) {
        utils::hash_combine(seed, set.set.to_primitive());
        utils::hash_combine(seed, set.identity.to_primitive());
        utils::hash_combine(seed, set.push_descriptor);
    }
    for (const auto& block : shader.uniform_blocks) {
        utils::hash_combine(seed, block.set.to_primitive());
        utils::hash_combine(seed, block.binding.to_primitive());
        utils::hash_combine(seed, block.identity.to_primitive());
        utils::hash_combine(seed, static_cast<unsigned>(block.scope));
    }
    return usize(seed);
}

struct SceneShaderTextureCompileInfo {
    bool                 enabled { false };
    rstd::array<bool, 4> components { false, false, false, false };
};

struct SceneShaderVariantStage {
    ShaderType                    stage { ShaderType::VERTEX };
    std::string                   source_key;
    std::string                   source;
    Set<unsigned>                 active_texture_slots;
    Map<std::string, std::string> uniforms;
    usize                         code_hash { 0 };
};

struct SceneShaderDefaultTexture {
    i32         slot {};
    std::string texture;
};

struct SceneShaderVariantDesc {
    std::string scene_id;
    std::string shader_name;

    Map<std::string, std::string>                  input_combos;
    Map<std::string, std::string>                  resolved_combos;
    Map<std::string, std::string>                  uniform_aliases;
    ShaderValues                                   default_uniforms;
    std::vector<SceneShaderDefaultTexture>         default_textures;
    std::vector<std::string>                       texture_slots;
    std::vector<SceneSamplerBinding>               sampler_bindings;
    std::vector<SceneShaderUniformBlockInterface>  uniform_blocks;
    std::vector<SceneShaderDescriptorSetInterface> descriptor_sets;

    std::vector<SceneShaderTextureCompileInfo> texture_infos;
    std::vector<SceneShaderVariantStage>       stages;
    std::size_t                                descriptor_layout_hash { 0 };
    bool                                       geometry_shader_enabled { false };

    bool Valid() const { return ! shader_name.empty() && ! stages.empty(); }
};

// ============================================================================
// SceneTexture.h
// ============================================================================

struct SceneTexture {
    std::string     url;
    TextureSample   sample;
    bool            isSprite { false };
    bool            isVideo { false };
    SpriteAnimation spriteAnim;
};

// ============================================================================
// SceneRenderTarget.h
// ============================================================================

enum class SceneRenderTargetKind
{
    Color,
    DepthSampled,
};

struct SceneRenderTarget {
    struct Bind {
        bool        enable { false };
        std::string name {};
        bool        screen { false };
        double      scale { 1.0 };
    };

    i32 width {};
    i32 height {};
    // Keep authored layout dimensions separate from backend-limited allocation dimensions.
    i32                   physical_width {};
    i32                   physical_height {};
    bool                  allowReuse { false };
    bool                  withDepth { false };
    SceneRenderTargetKind kind { SceneRenderTargetKind::Color };
    float                 depth_clear_value { 1.0f };
    bool                  has_mipmap { false };
    unsigned              mipmap_level { 1 };
    // 1 disables MSAA; only screen RT is opted-in by VulkanRender at init.
    unsigned      sample_count { 1 };
    TextureSample sample { TextureWrap::CLAMP_TO_EDGE,
                           TextureWrap::CLAMP_TO_EDGE,
                           TextureFilter::LINEAR,
                           TextureFilter::LINEAR };
    Bind          bind {};

    // Force VK_ATTACHMENT_LOAD_OP_CLEAR with transparent clear color on
    // every pass that writes to this RT. Needed for per-layer compose RTs
    // where the only writer is a Translucent draw and so the default
    // load-op (LOAD) would leak the previous frame's pixels through —
    // visible as ghosting when the rendered string changes (e.g. clock
    // text "12:00" → "12:01" leaves "12:00"'s glyphs underneath).
    bool force_clear { false };
    bool clear_on_first_write { false };
    bool initialize_transparent { false };
    // Later graph versions of this RT keep earlier color content. Use this
    // for composition targets, not transient effect outputs.
    bool preserve_on_write { false };

    i32 PhysicalWidth() const { return physical_width > i32() ? physical_width : width; }
    i32 PhysicalHeight() const { return physical_height > i32() ? physical_height : height; }
};

// ============================================================================
// SceneIndexArray.h
// ============================================================================

class SceneIndexArray : NoCopy {
    constexpr static usize Unit_Byte_Size { sizeof(rstd::uint32_t) };

public:
    SceneIndexArray(usize indexCount);
    SceneIndexArray(slice<rstd::uint32_t> data);

    SceneIndexArray(SceneIndexArray&&) noexcept;
    ~SceneIndexArray() = default;

    void Assign(usize index, slice<rstd::uint32_t> data) {
        if (! IncreaseCheckSet((index + data.len()) * Unit_Byte_Size)) return;
        for (usize source_index {}; source_index < data.len(); ++source_index) {
            m_data[index + source_index] = data[source_index];
        }
        BumpDataGeneration();
    }

    const rstd::uint32_t* Data() const { return m_data.is_empty() ? nullptr : m_data.begin(); }
    usize                 DataCount() const { return m_size; }
    usize                 DataSizeOf() const { return m_size * Unit_Byte_Size; }

    usize RenderDataCount() const noexcept {
        return m_render_size > m_size ? m_size : m_render_size;
    }
    void SetRenderDataCount(usize val) noexcept { m_render_size = val; }

    usize CapacityCount() const { return m_data.len(); }
    usize CapacitySizeof() const { return m_data.len() * Unit_Byte_Size; }
    u64   DataGeneration() const { return m_generation; }

    u32  ID() const { return m_id; }
    void SetID(u32 id) { m_id = id; }

private:
    bool IncreaseCheckSet(usize size);
    void BumpDataGeneration() noexcept { ++m_generation; }

    Vec<rstd::uint32_t> m_data;
    usize               m_size { 0 };

    usize m_render_size { usize::MAX };

    u32 m_id { u32::MAX };
    u64 m_generation { 1 };
};

// ============================================================================
// SceneVertexArray.h
// ============================================================================

struct SceneVertexWriteResult {
    usize vertex_count {};
    usize capacity {};
    bool  overflowed { false };
};

class SceneVertexWriter : NoCopy, NoMove {
public:
    auto AppendZeroedVertex() noexcept -> Option<mut_ref<float[]>>;

    usize Stride() const noexcept { return m_stride; }
    usize Capacity() const noexcept { return m_capacity; }
    usize Written() const noexcept { return m_written; }
    bool  Overflowed() const noexcept { return m_overflowed; }

private:
    friend class SceneVertexArray;

    SceneVertexWriter(mut_ref<float[]> data, usize stride) noexcept
        : m_data(data),
          m_stride(stride),
          m_capacity(stride == usize() ? usize() : data.len() / stride) {}

    mut_ref<float[]> m_data;
    usize            m_stride {};
    usize            m_capacity {};
    usize            m_written {};
    bool             m_overflowed { false };
};

class SceneVertexArray : NoCopy {
public:
    struct SceneVertexAttribute {
        std::string name;
        VertexType  type;
        bool        padding { true };
    };
    struct SceneVertexAttributeOffset {
        SceneVertexAttribute attr;
        usize                offset;
    };

    SceneVertexArray(const std::vector<SceneVertexAttribute>& attrs, usize count);
    ~SceneVertexArray() = default;

    SceneVertexArray(SceneVertexArray&&) noexcept;
    SceneVertexArray& operator=(SceneVertexArray&&) noexcept;

    bool AddVertex(const float*);
    bool SetVertex(std::string_view name, slice<float> data) noexcept;
    bool SetVertexs(usize index, slice<float> data) noexcept;

    template<typename Fill>
    [[nodiscard]] auto RewriteVertices(Fill&& fill) -> SceneVertexWriteResult {
        SceneVertexWriter writer(m_data.as_mut_slice().as_mut_ref(), m_oneSize);
        try {
            fill(writer);
        } catch (...) {
            (void)FinishVertexRewrite(writer);
            throw;
        }
        return FinishVertexRewrite(writer);
    }

    // Drops the active size to zero without releasing capacity. Subsequent
    // SetVertexs calls regrow it.
    void ResetSize() noexcept;

    bool GetOption(std::string_view) const;
    void SetOption(std::string_view, bool);

    const float* Data() const { return m_data.is_empty() ? nullptr : m_data.begin(); }
    usize        DataSize() const { return m_size; }
    usize        DataSizeOf() const { return m_size * usize(sizeof(float)); }
    usize        VertexCount() const { return m_oneSize == usize() ? usize() : m_size / m_oneSize; }
    usize        CapacitySize() const { return m_data.len(); }
    usize        CapacitySizeOf() const { return m_data.len() * usize(sizeof(float)); }
    usize        OneSize() const { return m_oneSize; }
    usize        OneSizeOf() const { return m_oneSize * usize(sizeof(float)); }
    u64          DataGeneration() const { return m_generation; }

    const auto&                                  Attributes() const { return m_attributes; }
    Map<std::string, SceneVertexAttributeOffset> GetAttrOffsetMap() const;

    u32  ID() const { return m_id; }
    void SetID(u32 id) { m_id = id; }

    static std::size_t TypeCount(VertexType);
    static std::size_t RealAttributeSize(const SceneVertexAttribute&);

private:
    bool TrySetSize(usize) noexcept;
    auto FinishVertexRewrite(const SceneVertexWriter&) noexcept -> SceneVertexWriteResult;
    void BumpDataGeneration() noexcept { ++m_generation; }

    std::vector<SceneVertexAttribute> m_attributes;

    Map<std::string, bool> m_options;

    Vec<float> m_data;
    usize      m_oneSize { 0 };
    usize      m_size { 0 };

    u32 m_id;
    u64 m_generation { 1 };
};

// Build a SceneVertexAttribute vector from compile-time VertexAttrSpec literals.
// Lets callsites write `MakeAttrSet({VAttr::Position, VAttr::TexCoord})` instead
// of hand-typing string/type pairs.
inline std::vector<SceneVertexArray::SceneVertexAttribute>
MakeAttrSet(std::span<const VertexAttrSpec> specs) {
    std::vector<SceneVertexArray::SceneVertexAttribute> out;
    out.reserve(specs.size());
    for (auto& s : specs) out.push_back({ rstd::cppstd::to_string(s.name), s.type, s.padding });
    return out;
}

inline std::vector<SceneVertexArray::SceneVertexAttribute>
MakeAttrSet(std::initializer_list<VertexAttrSpec> specs) {
    return MakeAttrSet(std::span<const VertexAttrSpec>(specs.begin(), specs.size()));
}

// ============================================================================
// SceneMaterial.h
// ============================================================================

struct SceneAnimationCurve;

struct SceneShaderValueAnimation {
    ShaderValue                      base;
    Option<Arc<SceneAnimationCurve>> curve;

    auto Clone() const -> SceneShaderValueAnimation {
        return {
            .base  = base,
            .curve = curve.is_some() ? Some((*curve).clone()) : None(),
        };
    }
};

using SceneShaderValueAnimationMap = BTreeMap<String, SceneShaderValueAnimation>;

struct SceneMaterialCustomShader {
    std::shared_ptr<SceneShader>   shader;
    ShaderValues                   constValues;
    SceneShaderValueAnimationMap   valueAnimations;
    Option<SceneShaderVariantDesc> variant;
    u64                            value_version { 1 };

    auto Clone() const -> SceneMaterialCustomShader;
};

struct SceneMaterialTextureMetadata {
    bool                  has_extent { false };
    rstd::array<float, 2> source_extent { 0.0f, 0.0f };
    rstd::array<float, 2> sample_extent { 0.0f, 0.0f };
};

enum class SceneMaterialTextureDependency
{
    Empty,
    Imported,
    RenderTarget,
    LinkRenderTarget,
    MipMappedFramebuffer,
};

enum class SceneMaterialTextureSourceKind
{
    Empty,
    Imported,
    SceneSurface,
    LayerPrevious,
    LayerStage,
    LayerOutput,
    EffectLocal,
    MipMappedFramebuffer,
    UnsupportedSpecial,
};

struct SceneMaterialTextureSource {
    SceneMaterialTextureSource() = default;
    SceneMaterialTextureSource(const SceneMaterialTextureSource& other)
        : kind(other.kind),
          key(other.key.clone()),
          binding_key(other.binding_key.clone()),
          layer(other.layer),
          effect(other.effect),
          wallpaper_layer(other.wallpaper_layer) {}
    SceneMaterialTextureSource(SceneMaterialTextureSource&&) noexcept = default;
    SceneMaterialTextureSource& operator=(const SceneMaterialTextureSource& other) {
        if (this == &other) return *this;
        kind            = other.kind;
        key             = other.key.clone();
        binding_key     = other.binding_key.clone();
        layer           = other.layer;
        effect          = other.effect;
        wallpaper_layer = other.wallpaper_layer;
        return *this;
    }
    SceneMaterialTextureSource& operator=(SceneMaterialTextureSource&&) noexcept = default;

    SceneMaterialTextureSourceKind kind { SceneMaterialTextureSourceKind::Empty };
    String                         key;
    String                         binding_key;
    Option<SceneNodeId>            layer;
    Option<SceneEffectId>          effect;
    i32                            wallpaper_layer { -1 };
};

inline SceneMaterialTextureDependency ClassifySceneMaterialTexture(std::string_view texture) {
    if (texture.empty()) return SceneMaterialTextureDependency::Empty;
    auto text = rstd::cppstd::as_str(texture).unwrap();
    if (IsSpecLinkTex(text)) return SceneMaterialTextureDependency::LinkRenderTarget;
    if (text.starts_with(WE_MIP_MAPPED_FRAME_BUFFER))
        return SceneMaterialTextureDependency::MipMappedFramebuffer;
    if (IsSpecTex(text)) return SceneMaterialTextureDependency::RenderTarget;
    return SceneMaterialTextureDependency::Imported;
}

inline bool IsLocalSceneMaterialTextureDependency(SceneMaterialTextureDependency dep) {
    return dep == SceneMaterialTextureDependency::Empty ||
           dep == SceneMaterialTextureDependency::Imported;
}

inline bool CanRefreshSceneMaterialTextureBinding(std::string_view old_texture,
                                                  std::string_view new_texture,
                                                  std::string_view pass_output = {}) {
    if (old_texture == new_texture) return true;
    if ((! old_texture.empty() && old_texture == pass_output) ||
        (! new_texture.empty() && new_texture == pass_output))
        return false;
    auto old_dep = ClassifySceneMaterialTexture(old_texture);
    auto new_dep = ClassifySceneMaterialTexture(new_texture);
    return IsLocalSceneMaterialTextureDependency(old_dep) &&
           IsLocalSceneMaterialTextureDependency(new_dep);
}

using SceneMaterialDirtyFlags = rstd::uint32_t;

enum class SceneMaterialDirty : SceneMaterialDirtyFlags
{
    Resources       = 1u << 0u,
    Pipeline        = 1u << 1u,
    Graph           = 1u << 2u,
    TextureBindings = 1u << 3u,
};

inline constexpr SceneMaterialDirtyFlags SceneMaterialDirtyNone { 0u };
inline constexpr SceneMaterialDirtyFlags SceneMaterialDirtyResources {
    static_cast<SceneMaterialDirtyFlags>(SceneMaterialDirty::Resources),
};
inline constexpr SceneMaterialDirtyFlags SceneMaterialDirtyPipeline {
    static_cast<SceneMaterialDirtyFlags>(SceneMaterialDirty::Pipeline),
};
inline constexpr SceneMaterialDirtyFlags SceneMaterialDirtyGraph {
    static_cast<SceneMaterialDirtyFlags>(SceneMaterialDirty::Graph),
};
inline constexpr SceneMaterialDirtyFlags SceneMaterialDirtyTextureBindings {
    static_cast<SceneMaterialDirtyFlags>(SceneMaterialDirty::TextureBindings),
};
inline constexpr SceneMaterialDirtyFlags SceneMaterialDirtyAll {
    SceneMaterialDirtyResources | SceneMaterialDirtyPipeline | SceneMaterialDirtyGraph |
        SceneMaterialDirtyTextureBindings,
};

inline bool SceneShaderVariantHasActiveTextureMetadata(const SceneShaderVariantDesc& desc) {
    for (const auto& stage : desc.stages) {
        if (! stage.active_texture_slots.empty()) return true;
    }
    return false;
}

inline Set<unsigned> SceneShaderVariantActiveTextureSlots(const SceneShaderVariantDesc& desc) {
    Set<unsigned> slots;
    for (const auto& stage : desc.stages) {
        slots.insert(stage.active_texture_slots.begin(), stage.active_texture_slots.end());
    }
    return slots;
}

inline Map<std::string, std::string>
SceneShaderVariantUniformLayout(const SceneShaderVariantDesc& desc) {
    Map<std::string, std::string> uniforms;
    for (const auto& stage : desc.stages) {
        for (const auto& [name, ty] : stage.uniforms) uniforms[name] = ty;
    }
    return uniforms;
}

inline bool SameSceneShaderVariantStageSet(const SceneShaderVariantDesc& lhs,
                                           const SceneShaderVariantDesc& rhs) {
    if (lhs.stages.size() != rhs.stages.size()) return false;
    for (std::size_t i = 0; i < lhs.stages.size(); ++i) {
        if (lhs.stages[i].stage != rhs.stages[i].stage) return false;
    }
    return true;
}

inline bool SameSceneShaderVariantUniformShape(const SceneShaderVariantDesc& lhs,
                                               const SceneShaderVariantDesc& rhs) {
    auto lhs_layout = SceneShaderVariantUniformLayout(lhs);
    auto rhs_layout = SceneShaderVariantUniformLayout(rhs);
    if (! lhs_layout.empty() || ! rhs_layout.empty()) return lhs_layout == rhs_layout;

    if (lhs.default_uniforms.size() != rhs.default_uniforms.size()) return false;
    for (const auto& [name, value] : lhs.default_uniforms) {
        auto it = rhs.default_uniforms.find(name);
        if (it == rhs.default_uniforms.end() || it->second.size() != value.size()) return false;
    }
    return true;
}

inline bool SceneShaderVariantHasCodeHashes(const SceneShaderVariantDesc& desc) {
    for (const auto& stage : desc.stages) {
        if (stage.code_hash != usize()) return true;
    }
    return false;
}

inline bool SameSceneShaderVariantCodeHashes(const SceneShaderVariantDesc& lhs,
                                             const SceneShaderVariantDesc& rhs) {
    const bool lhs_has_hashes = SceneShaderVariantHasCodeHashes(lhs);
    const bool rhs_has_hashes = SceneShaderVariantHasCodeHashes(rhs);
    if (! lhs_has_hashes && ! rhs_has_hashes) return true;
    if (lhs.stages.size() != rhs.stages.size()) return false;
    for (std::size_t i = 0; i < lhs.stages.size(); ++i) {
        if (lhs.stages[i].code_hash != rhs.stages[i].code_hash) return false;
    }
    return true;
}

inline bool SameSceneShaderVariantDescriptorLayout(const SceneShaderVariantDesc& lhs,
                                                   const SceneShaderVariantDesc& rhs) {
    if (lhs.descriptor_layout_hash == 0 && rhs.descriptor_layout_hash == 0) return true;
    return lhs.descriptor_layout_hash == rhs.descriptor_layout_hash;
}

inline SceneMaterialDirtyFlags
ClassifySceneShaderVariantMutation(const SceneShaderVariantDesc& current,
                                   const SceneShaderVariantDesc& next) {
    if (! current.Valid() || ! next.Valid()) return SceneMaterialDirtyGraph;
    if (current.shader_name != next.shader_name) return SceneMaterialDirtyGraph;
    if (SceneShaderVariantHasActiveTextureMetadata(current) ||
        SceneShaderVariantHasActiveTextureMetadata(next)) {
        if (SceneShaderVariantActiveTextureSlots(current) !=
            SceneShaderVariantActiveTextureSlots(next))
            return SceneMaterialDirtyGraph;
    }

    SceneMaterialDirtyFlags flags = SceneMaterialDirtyNone;
    if (! SameSceneShaderVariantStageSet(current, next)) flags |= SceneMaterialDirtyPipeline;
    if (! SameSceneShaderVariantUniformShape(current, next)) {
        flags |= SceneMaterialDirtyResources | SceneMaterialDirtyPipeline;
    }
    if (current.resolved_combos != next.resolved_combos ||
        current.input_combos != next.input_combos ||
        current.texture_infos.size() != next.texture_infos.size() ||
        current.geometry_shader_enabled != next.geometry_shader_enabled) {
        flags |= SceneMaterialDirtyResources | SceneMaterialDirtyPipeline;
    }
    if (current.sampler_bindings != next.sampler_bindings) {
        flags |= SceneMaterialDirtyResources;
    }
    if (! SameSceneShaderVariantCodeHashes(current, next)) flags |= SceneMaterialDirtyPipeline;
    if (! SameSceneShaderVariantDescriptorLayout(current, next)) {
        flags |= SceneMaterialDirtyResources | SceneMaterialDirtyPipeline;
    }
    if (flags == SceneMaterialDirtyNone && current.stages.size() == next.stages.size()) {
        for (std::size_t i = 0; i < current.stages.size(); ++i) {
            if (current.stages[i].source_key != next.stages[i].source_key ||
                current.stages[i].source != next.stages[i].source) {
                flags |= SceneMaterialDirtyPipeline;
                break;
            }
        }
    }
    return flags;
}

struct SceneMaterial {
public:
    SceneMaterial() = default;
    SceneMaterial(const SceneMaterial& other) { copyFrom(other); }
    SceneMaterial(SceneMaterial&& other) noexcept { moveFrom(std::move(other)); }
    SceneMaterial& operator=(const SceneMaterial& other) {
        if (this != &other) copyFrom(other);
        return *this;
    }
    SceneMaterial& operator=(SceneMaterial&& other) noexcept {
        if (this != &other) moveFrom(std::move(other));
        return *this;
    }

    void SetDirty(SceneMaterialDirtyFlags flags) { m_dirty_flags.fetch_or(flags); }
    void SetResourceDirty() { SetDirty(SceneMaterialDirtyResources); }
    void SetPipelineDirty() { SetDirty(SceneMaterialDirtyPipeline); }
    void SetGraphDirty() { SetDirty(SceneMaterialDirtyGraph); }
    void SetTextureBindingsDirty() { SetDirty(SceneMaterialDirtyTextureBindings); }
    SceneMaterialDirtyFlags DirtyFlags() const { return m_dirty_flags.load(); }
    SceneMaterialDirtyFlags
    ConsumeDirtyFlags(SceneMaterialDirtyFlags mask = SceneMaterialDirtyAll) {
        SceneMaterialDirtyFlags old = m_dirty_flags.load();
        while (! m_dirty_flags.compare_exchange_weak(old, old & ~mask)) {
        }
        return old & mask;
    }

    bool SetBlendMode(BlendMode value) {
        if (blenmode == value) return false;
        blenmode = value;
        SetPipelineDirty();
        return true;
    }
    bool SetDepthTest(bool value) {
        if (depth_test == value) return false;
        depth_test = value;
        SetPipelineDirty();
        return true;
    }
    bool SetDepthWrite(bool value) {
        if (depth_write == value) return false;
        depth_write = value;
        SetPipelineDirty();
        return true;
    }
    bool SetDepthCompare(CompareOp value) {
        if (depth_compare == value) return false;
        depth_compare = value;
        SetPipelineDirty();
        return true;
    }
    bool SetCullMode(CullMode value) {
        if (cull_mode == value) return false;
        cull_mode = value;
        SetPipelineDirty();
        return true;
    }
    bool SetShaderValue(std::string uniform_name, const ShaderValue& value) {
        if (uniform_name.empty()) return false;
        auto shaped                            = ShapeShaderValue(uniform_name, value);
        customShader.constValues[uniform_name] = shaped;
        auto animation =
            customShader.valueAnimations.get_mut(rstd::cppstd::as_str(uniform_name).unwrap());
        if (animation.is_some()) (**animation).base = shaped;
        TouchShaderValues();
        return true;
    }
    void TouchShaderValues() {
        ++customShader.value_version;
        if (customShader.value_version == u64()) customShader.value_version = u64(1);
    }
    bool SetShaderValueAnimation(String uniform_name, Arc<SceneAnimationCurve> curve);
    bool TickShaderValueAnimations(double runtime);
    bool SetShaderVariant(std::shared_ptr<SceneShader> shader, SceneShaderVariantDesc variant) {
        if (! shader || ! variant.Valid()) return false;
        SceneMaterialDirtyFlags flags =
            customShader.variant.is_some()
                ? ClassifySceneShaderVariantMutation(*customShader.variant, variant)
                : SceneMaterialDirtyGraph;
        if (flags == SceneMaterialDirtyNone) return false;
        if (! variant.texture_slots.empty() &&
            SceneShaderVariantHasActiveTextureMetadata(variant)) {
            auto previous_textures = textures;
            auto previous_metadata = texture_metadata;
            auto previous_sources  = texture_sources.clone();
            textures               = variant.texture_slots;
            texture_metadata.resize(textures.size());
            texture_sources.resize(usize(textures.size()), SceneMaterialTextureSource {});
            for (std::size_t i = 0; i < textures.size(); ++i) {
                if (i >= previous_textures.size() || previous_textures[i] != textures[i]) {
                    texture_metadata[i]       = {};
                    texture_sources[usize(i)] = {};
                } else if (i < previous_metadata.size()) {
                    texture_metadata[i] = previous_metadata[i];
                    if (i < previous_sources.len().to_primitive())
                        texture_sources[usize(i)] = previous_sources[usize(i)];
                }
            }
            auto active = SceneShaderVariantActiveTextureSlots(variant);
            for (std::size_t i = 0; i < textures.size(); ++i) {
                if (! active.contains(static_cast<unsigned>(i))) {
                    textures[i].clear();
                    texture_metadata[i]       = {};
                    texture_sources[usize(i)] = {};
                }
            }
        }
        customShader.shader  = std::move(shader);
        customShader.variant = Some(rstd::move(variant));
        SetDirty(flags);
        return true;
    }

    std::string                               name;
    std::vector<std::string>                  textures;
    std::vector<SceneMaterialTextureMetadata> texture_metadata;
    Vec<SceneMaterialTextureSource>           texture_sources;
    std::vector<std::string>                  defines;

    bool hasSprite { false };

    SceneMaterialCustomShader      customShader;
    BlendMode                      blenmode { BlendMode::Disable };
    bool                           depth_test { false };
    bool                           depth_write { false };
    CompareOp                      depth_compare { CompareOp::LessEqual };
    CullMode                       cull_mode { CullMode::None };
    bool                           depth_clamp { false };
    bool                           depth_bias { false };
    float                          depth_bias_constant { 0.0f };
    float                          depth_bias_clamp { 0.0f };
    float                          depth_bias_slope { 0.0f };
    std::shared_ptr<SceneMaterial> shadow_variant;

private:
    ShaderValue ShapeShaderValue(std::string_view uniform_name, const ShaderValue& value) const {
        if (value.size() == usize()) return value;

        usize target_size {};
        if (auto it = customShader.constValues.find(std::string(uniform_name));
            it != customShader.constValues.end()) {
            target_size = it->second.size();
        }
        if (customShader.shader) {
            if (auto it = customShader.shader->default_uniforms.find(std::string(uniform_name));
                it != customShader.shader->default_uniforms.end()) {
                if (it->second.size() > target_size) target_size = it->second.size();
            }
        }
        if (target_size <= value.size() || target_size > usize(4)) return value;

        const auto         fill = value.size() == usize(1) ? value[usize()] : 0.0f;
        std::vector<float> shaped(target_size.to_primitive(), fill);
        for (usize index {}; index < value.size(); ++index) {
            shaped[index.to_primitive()] = value[index];
        }
        return ShaderValue(std::span<const float>(shaped));
    }

    void copyFrom(const SceneMaterial& other) {
        name                = other.name;
        textures            = other.textures;
        texture_metadata    = other.texture_metadata;
        texture_sources     = other.texture_sources.clone();
        defines             = other.defines;
        hasSprite           = other.hasSprite;
        customShader        = other.customShader.Clone();
        blenmode            = other.blenmode;
        depth_test          = other.depth_test;
        depth_write         = other.depth_write;
        depth_compare       = other.depth_compare;
        cull_mode           = other.cull_mode;
        depth_clamp         = other.depth_clamp;
        depth_bias          = other.depth_bias;
        depth_bias_constant = other.depth_bias_constant;
        depth_bias_clamp    = other.depth_bias_clamp;
        depth_bias_slope    = other.depth_bias_slope;
        shadow_variant      = other.shadow_variant;
        m_dirty_flags.store(other.m_dirty_flags.load());
    }
    void moveFrom(SceneMaterial&& other) {
        name                = std::move(other.name);
        textures            = std::move(other.textures);
        texture_metadata    = std::move(other.texture_metadata);
        texture_sources     = rstd::move(other.texture_sources);
        defines             = std::move(other.defines);
        hasSprite           = other.hasSprite;
        customShader        = std::move(other.customShader);
        blenmode            = other.blenmode;
        depth_test          = other.depth_test;
        depth_write         = other.depth_write;
        depth_compare       = other.depth_compare;
        cull_mode           = other.cull_mode;
        depth_clamp         = other.depth_clamp;
        depth_bias          = other.depth_bias;
        depth_bias_constant = other.depth_bias_constant;
        depth_bias_clamp    = other.depth_bias_clamp;
        depth_bias_slope    = other.depth_bias_slope;
        shadow_variant      = rstd::move(other.shadow_variant);
        m_dirty_flags.store(other.m_dirty_flags.load());
    }

    std::atomic<SceneMaterialDirtyFlags> m_dirty_flags { SceneMaterialDirtyNone };
};

// ============================================================================
// SceneMesh.h
// ============================================================================

using SceneMeshDirtyFlags = rstd::uint32_t;

enum class SceneMeshDirty : SceneMeshDirtyFlags
{
    Data   = 1u << 0u,
    Layout = 1u << 1u,
};

inline constexpr SceneMeshDirtyFlags SceneMeshDirtyNone { 0u };
inline constexpr SceneMeshDirtyFlags SceneMeshDirtyData {
    static_cast<SceneMeshDirtyFlags>(SceneMeshDirty::Data),
};
inline constexpr SceneMeshDirtyFlags SceneMeshDirtyLayout {
    static_cast<SceneMeshDirtyFlags>(SceneMeshDirty::Layout),
};
inline constexpr SceneMeshDirtyFlags SceneMeshDirtyAll {
    SceneMeshDirtyData | SceneMeshDirtyLayout,
};

class SceneMesh {
public:
    // Per-part draw ranges into one submesh's index array. When empty, the
    // submesh is drawn as one DrawIndexed call covering all its indices; when
    // populated (V21 puppets with parts[] block), one DrawIndexed call is
    // issued per range in vector order — matching the file's z-order. All
    // ranges in a submesh share the submesh's material slot.
    struct DrawRange {
        u32 first_index;
        u32 index_count;
    };

    // = glTF "primitive": one vertex-stream set + one index array + one
    // material slot. A SceneMesh holds >= 1 Submesh; today most paths emit
    // exactly one (single-slot compat); SceneParser will emit N for
    // .mdl meshes with mesh_count > 1.
    struct Submesh {
        std::vector<SceneVertexArray> vertex_arrays;
        std::vector<SceneIndexArray>  index_arrays;
        std::vector<DrawRange>        draw_ranges;
        u32                           material_slot {};
        // Non-empty value redirects this submesh's pass output to the
        // named RT (instead of the SceneNode's default). Used by puppet
        // clipping-mask submeshes to write into a shared `_rt_puppet_mask`
        // that the main puppet pass samples via g_Texture8.
        std::string output_override;
        bool        preserve_output { false };
    };

    SceneMesh(bool dynamic = false)
        : m_dynamic(dynamic), m_dirty(false), m_data(std::make_shared<Data>()) {}

    MeshPrimitive Primitive() const { return m_primitive; }
    u32           PointSize() const { return m_pointSize; }

    bool        Dynamic() const { return m_dynamic; }
    const auto& Dirty() const { return m_dirty; }
    auto&       Dirty() { return m_dirty; }
    void        SetDirty(SceneMeshDirtyFlags flags = SceneMeshDirtyData) {
        m_dirty_flags.fetch_or(flags);
        m_dirty.store(true);
    }
    void                SetLayoutDirty() { SetDirty(SceneMeshDirtyLayout); }
    SceneMeshDirtyFlags DirtyFlags() const { return m_dirty_flags.load(); }
    SceneMeshDirtyFlags ConsumeDirtyFlags(SceneMeshDirtyFlags mask = SceneMeshDirtyAll) {
        SceneMeshDirtyFlags old = m_dirty_flags.load();
        while (! m_dirty_flags.compare_exchange_weak(old, old & ~mask)) {
        }
        auto consumed = old & mask;
        if ((old & ~mask) == SceneMeshDirtyNone) m_dirty.store(false);
        return consumed;
    }

    u32  ID() const { return m_id; };
    void SetID(u32 v) { m_id = v; };

    void SetPrimitive(MeshPrimitive v) { m_primitive = v; }
    void SetPointSize(u32 v) { m_pointSize = v; }

    // ---- New submesh API ----
    const std::vector<Submesh>& Submeshes() const { return m_data->submeshes; }
    std::vector<Submesh>&       Submeshes() { return m_data->submeshes; }

    // Materials are per-mesh-instance, NOT shared via ChangeMeshDataFrom — same
    // contract as the legacy m_material field.
    const std::vector<std::shared_ptr<SceneMaterial>>& MaterialSlots() const { return m_materials; }
    std::vector<std::shared_ptr<SceneMaterial>>&       MaterialSlots() { return m_materials; }

    // ---- Legacy single-slot compat (routes through submeshes[0] / materials[0]) ----
    usize VertexCount() const { return usize(submesh0().vertex_arrays.size()); }
    usize IndexCount() const { return usize(submesh0().index_arrays.size()); }

    const SceneVertexArray& GetVertexArray(usize index) const {
        return submesh0().vertex_arrays[index.to_primitive()];
    }
    const SceneIndexArray& GetIndexArray(usize index) const {
        return submesh0().index_arrays[index.to_primitive()];
    }
    SceneVertexArray& GetVertexArray(usize index) {
        return ensureSubmesh0().vertex_arrays[index.to_primitive()];
    }
    SceneIndexArray& GetIndexArray(usize index) {
        return ensureSubmesh0().index_arrays[index.to_primitive()];
    }

    void AddIndexArray(SceneIndexArray&& array) {
        ensureSubmesh0().index_arrays.emplace_back(std::move(array));
    }
    void AddVertexArray(SceneVertexArray&& array) {
        ensureSubmesh0().vertex_arrays.emplace_back(std::move(array));
    }
    void AddMaterial(SceneMaterial&& material) {
        m_materials.push_back(std::make_shared<SceneMaterial>(std::move(material)));
    }

    SceneMaterial* Material() { return m_materials.empty() ? nullptr : m_materials[0].get(); }

    const Eigen::Matrix4d& GeometryTransform() const { return m_data->geometry_transform; }
    void                   SetGeometryTransform(Eigen::Matrix4d transform) {
        m_data->geometry_transform = rstd::move(transform);
    }

    void ChangeMeshDataFrom(const SceneMesh& o) { m_data = o.m_data; }

    std::shared_ptr<SceneMesh> CloneInstance() const {
        auto clone         = std::make_shared<SceneMesh>(m_dynamic);
        clone->m_primitive = m_primitive;
        clone->m_pointSize = m_pointSize;
        clone->m_data      = m_data;
        clone->m_materials.reserve(m_materials.size());
        for (const auto& material : m_materials) {
            clone->m_materials.push_back(material ? std::make_shared<SceneMaterial>(*material)
                                                  : nullptr);
        }
        return clone;
    }

    const std::vector<DrawRange>& DrawRanges() const {
        static const std::vector<DrawRange> kEmpty;
        return m_data->submeshes.empty() ? kEmpty : m_data->submeshes[0].draw_ranges;
    }
    void SetDrawRanges(std::vector<DrawRange> ranges) {
        ensureSubmesh0().draw_ranges = std::move(ranges);
    }

private:
    struct Data {
        Eigen::Matrix4d      geometry_transform { Eigen::Matrix4d::Identity() };
        std::vector<Submesh> submeshes;
    };

    Submesh& ensureSubmesh0() {
        if (m_data->submeshes.empty()) m_data->submeshes.emplace_back();
        return m_data->submeshes[0];
    }
    const Submesh& submesh0() const {
        static const Submesh kEmpty;
        return m_data->submeshes.empty() ? kEmpty : m_data->submeshes[0];
    }

    u32               m_id { u32::MAX };
    MeshPrimitive     m_primitive { MeshPrimitive::TRIANGLE };
    u32               m_pointSize { 1 };
    bool              m_dynamic;
    std::atomic<bool> m_dirty;

    std::shared_ptr<Data>                       m_data;      // shared via ChangeMeshDataFrom
    std::vector<std::shared_ptr<SceneMaterial>> m_materials; // per-instance
    std::atomic<SceneMeshDirtyFlags>            m_dirty_flags { SceneMeshDirtyNone };
};

class SceneNode;
struct SceneImageEffect;
class SceneNodeLayer;
struct ScenePostProcess;

struct WallpaperLayerId {
    i32 value { -1 };
};

// ============================================================================
// SceneCamera.h
// ============================================================================

struct SceneCameraTransforms {
    Eigen::Vector3d eye { Eigen::Vector3d::Zero() };
    Eigen::Vector3d center { -Eigen::Vector3d::UnitZ() };
    Eigen::Vector3d up { Eigen::Vector3d::UnitY() };
};

class SceneCamera {
public:
    static auto MakeOrthographic(double width, double height, double near, double far)
        -> SceneCamera {
        return SceneCamera(OrthographicTag {}, width, height, near, far);
    }

    static auto MakePerspective(double aspect, double near, double far, double fov) -> SceneCamera {
        return SceneCamera(PerspectiveTag {}, aspect, near, far, fov);
    }

    SceneCamera(const SceneCamera& cam) { Clone(cam); }

    void Update();

    void AttatchNode(SceneNode*);

    bool   IsPerspective() const { return m_perspective; }
    double Aspect() const { return m_aspect; }
    double Width() const { return m_width; }
    double Height() const { return m_height; }
    double NearClip() const { return m_nearClip; }
    double FarClip() const { return m_farClip; }
    double Fov() const { return m_fov; }

    void SetWidth(double value) {
        m_width  = value;
        m_aspect = m_width / m_height;
    }
    void SetHeight(double value) {
        m_height = value;
        m_aspect = m_width / m_height;
    }
    void SetAspect(double aspect) { m_aspect = aspect; }
    void SetFov(double value) { m_fov = value; }

    // Explicit eye/center/up view, used by perspective scenes (general
    // isOrtho==false) whose camera is given in WE world units rather than the
    // 2D pixel-space node placement. Once set, the view comes from LookAt and
    // the attached node (if any) is ignored.
    void SetLookAt(const Eigen::Vector3d& eye, const Eigen::Vector3d& center,
                   const Eigen::Vector3d& up) {
        m_eye    = eye;
        m_center = center;
        m_up     = up;
        m_lookat = true;
    }
    bool IsLookAt() const { return m_lookat; }
    auto Transforms() const -> SceneCameraTransforms;
    bool SetTransforms(const SceneCameraTransforms& transforms);
    auto AuthoredTransforms() const -> SceneCameraTransforms;
    bool SetAuthoredTransforms(const SceneCameraTransforms& transforms);

    Eigen::Vector3d GetPosition(SceneRenderViewKind view = SceneRenderViewKind::Primary) const;
    Eigen::Vector3d GetDirection() const;

    // Lazy: recomputes from m_node->ModelTrans() on every call. Cheap when
    // the attached node hasn't moved (UpdateTrans early-exits on clean
    // m_dirty), correctness-keeping when scripts / parent-chain attachment
    // shift the node between frames.
    Eigen::Matrix4d GetViewMatrix();
    Eigen::Matrix4d
    GetViewProjectionMatrix(SceneRenderViewKind view = SceneRenderViewKind::Primary);

    rstd::Option<SceneNode*> GetAttachedNode() const {
        if (m_node == nullptr) return rstd::None();
        return rstd::Some<SceneNode*>(m_node);
    }

    void Clone(const SceneCamera& cam) {
        m_width       = cam.m_width;
        m_height      = cam.m_height;
        m_aspect      = cam.m_aspect;
        m_nearClip    = cam.m_nearClip;
        m_farClip     = cam.m_farClip;
        m_fov         = cam.m_fov;
        m_perspective = cam.m_perspective;
        m_lookat      = cam.m_lookat;
        m_eye         = cam.m_eye;
        m_center      = cam.m_center;
        m_up          = cam.m_up;
        m_node        = cam.m_node;
    }

private:
    struct OrthographicTag {};
    struct PerspectiveTag {};

    explicit SceneCamera(OrthographicTag, double width, double height, double near, double far)
        : m_width(width),
          m_height(height),
          m_aspect(m_width / m_height),
          m_nearClip(near),
          m_farClip(far),
          m_perspective(false) {}

    explicit SceneCamera(PerspectiveTag, double aspect, double near, double far, double fov)
        : m_aspect(aspect), m_nearClip(near), m_farClip(far), m_fov(fov), m_perspective(true) {}
    void            CalculateViewProjectionMatrix();
    Eigen::Matrix4d CalculateReflectionViewProjectionMatrix();

    double m_width { 1.0f };
    double m_height { 1.0f };
    double m_aspect { 16.0f / 9.0f };
    double m_nearClip { 0.01f };
    double m_farClip { 1000.0f };
    double m_fov { 45.0f };
    bool   m_perspective { false };

    bool            m_lookat { false };
    Eigen::Vector3d m_eye { Eigen::Vector3d::Zero() };
    Eigen::Vector3d m_center { -Eigen::Vector3d::UnitZ() };
    Eigen::Vector3d m_up { Eigen::Vector3d::UnitY() };

    Eigen::Matrix4d m_viewMat { Eigen::Matrix4d::Identity() };
    Eigen::Matrix4d m_viewProjectionMat { Eigen::Matrix4d::Identity() };

    SceneNode* m_node { nullptr };
};

struct SceneAnimationKey {
    i32   frame {};
    float value { 0.0f };
    bool  front_enabled { false };
    float front_x { 0.0f };
    float front_y { 0.0f };
    bool  back_enabled { false };
    float back_x { 0.0f };
    float back_y { 0.0f };
};

// A marker on the curve's timeline. Playback crossing `frame` is what a
// wallpaper's `animationEvent(event, value)` export reacts to.
struct SceneAnimationEvent {
    i32    frame {};
    String name;
};

struct SceneAnimationCurve {
    Vec<SceneAnimationKey>   c0;
    Vec<SceneAnimationKey>   c1;
    Vec<SceneAnimationKey>   c2;
    Vec<SceneAnimationEvent> events;
    float                    fps { 30.0f };
    i32                      length {};
    String                   mode;
    bool                     wraploop { false };
    bool                     relative { false };
    bool                     startpaused { false };

    bool            Empty() const;
    float           EvaluateScalar(float base, double runtime) const;
    Eigen::Vector3f EvaluateVec3(const Eigen::Vector3f& base, double runtime) const;
};

// Playhead that reports which of a curve's markers were passed since the
// previous tick. It runs on the same frame clock the curve is evaluated
// on, so a marker fires on the frame the curve actually reaches it —
// including once per pass under `loop`, and once per direction under
// `mirror`. Markers of a `startpaused` timeline stay silent: nothing
// advances that playhead until named playback control exists.
class SceneAnimationEventCursor {
public:
    explicit SceneAnimationEventCursor(const SceneAnimationCurve& curve);

    bool Empty() const { return m_events.is_empty(); }

    // Move the playhead to `runtime` and append every marker passed on the
    // way, in the order they were passed.
    void Advance(double runtime, Vec<ref<str>>& out);

private:
    Vec<SceneAnimationEvent> m_events;
    float                    m_fps { 30.0f };
    float                    m_end { 0.0f };
    bool                     m_loop { false };
    bool                     m_mirror { false };
    bool                     m_primed { false };
    double                   m_previous { 0.0 };
};

struct SceneCameraLookAtKey {
    float           frame { 0.0f };
    Eigen::Vector3f eye { Eigen::Vector3f::Zero() };
    Eigen::Vector3f center { Eigen::Vector3f::Zero() };
    Eigen::Vector3f up { Eigen::Vector3f::UnitY() };
};

struct SceneCameraLookAtTrack {
    float                     duration { 0.0f };
    Vec<SceneCameraLookAtKey> keys;
};

class SceneCameraPath : NoCopy, NoMove {
public:
    String                      camera_name;
    Option<Arc<SceneCamera>>    camera;
    SceneNode*                  node { nullptr };
    Eigen::Vector3f             default_translate { Eigen::Vector3f::Zero() };
    Eigen::Vector3f             default_rotation { Eigen::Vector3f::Zero() };
    Eigen::Vector3f             path_translate_bias { Eigen::Vector3f::Zero() };
    Eigen::Vector3f             path_rotation_bias { Eigen::Vector3f::Zero() };
    double                      default_width { 1.0 };
    double                      default_height { 1.0 };
    double                      default_fov { 50.0 };
    Eigen::Vector3f             origin_base { Eigen::Vector3f::Zero() };
    Eigen::Vector3f             rotation_base { Eigen::Vector3f::Zero() };
    float                       zoom_base { 1.0f };
    float                       fov_base { 50.0f };
    bool                        perspective { false };
    bool                        enabled { true };
    bool                        default_lookat { false };
    Eigen::Vector3f             default_eye { Eigen::Vector3f::Zero() };
    Eigen::Vector3f             default_center { -Eigen::Vector3f::UnitZ() };
    Eigen::Vector3f             default_up { Eigen::Vector3f::UnitY() };
    float                       lookat_fps { 1.0f };
    Vec<SceneCameraLookAtTrack> lookat_tracks;
    SceneAnimationCurve         origin_curve;
    SceneAnimationCurve         rotation_curve;
    SceneAnimationCurve         zoom_curve;
    SceneAnimationCurve         fov_curve;
    SceneUserVisibilityBinding  visible_user_binding;

    void CaptureViewport();
    void SetEnabled(bool value) { enabled = value; }
    bool Tick(double runtime);
    bool ApplyDefault();
};

class SceneSoundControl {
public:
    using Trait                  = SceneSoundControl;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = SceneSoundControl;

        void Play() { rstd::trait_call<0>(this); }
        void Stop() { rstd::trait_call<1>(this); }
        void Pause() { rstd::trait_call<2>(this); }
        bool IsPlaying() const { return rstd::trait_call<3>(this); }
        void SetVolume(float volume) { rstd::trait_call<4>(this, volume); }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Play, &T::Stop, &T::Pause, &T::IsPlaying, &T::SetVolume>;
};

class SceneParticleOverrideControl {
public:
    using Trait                  = SceneParticleOverrideControl;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = SceneParticleOverrideControl;

        void Apply(slice<float> value) { rstd::trait_call<0>(this, value); }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Apply>;
};

class SceneParticleControl {
public:
    using Trait                  = SceneParticleControl;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = SceneParticleControl;

        Vec<float> Get(ref<str> field) const { return rstd::trait_call<0>(this, field); }
        void Apply(ref<str> field, slice<float> value) { rstd::trait_call<1>(this, field, value); }
        void Play() { rstd::trait_call<2>(this); }
        void Stop() { rstd::trait_call<3>(this); }
        void Pause() { rstd::trait_call<4>(this); }
        bool IsPlaying() const { return rstd::trait_call<5>(this); }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Get, &T::Apply, &T::Play, &T::Stop, &T::Pause, &T::IsPlaying>;
};

// ============================================================================
// SceneNode.h
// ============================================================================

// Lifetime invariant — nodes are never removed after publication.
//
// `m_children` / `m_parent` are written during parsing and by Scene-owned
// runtime factories. Runtime-created nodes stay attached for the Scene's
// lifetime; destroyLayer hides and recycles them. `SetParentAnchor` is used by
// SceneNodeLayer::ResolveEffect to re-anchor an effect's composite
// node onto its layer's worldNode for transform inheritance — both nodes
// survive for the Scene's lifetime, so the re-anchor never dangles.
//
// post-parse mutations restricted to render thread: m_translate / m_scale /
// m_rotation, m_visible, m_user_alpha, m_brightness, m_color, m_tex_anim,
// m_dirty (all driven by script ticks and shader-value updates).
//
// Practical consequence: every non-owning `SceneNode*` reference held by
// downstream subsystems (FieldScript::Impl::node, EngineHostState::text_setters,
// SceneNodeLayer::m_worldNode, SceneCamera::m_node, m_parent itself)
// is valid for the Scene's lifetime by construction. The dtor's parent
// back-link clearing below is a defence against Scene teardown ordering,
// where a child held by an external Arc (e.g. actuator closures)
// can outlive its parent during the children Vec destructor.
class SceneNode : NoCopy, NoMove {
public:
    struct ShadowParticipation {
        bool cast { false };
    };

    ShadowParticipation shadow;

    SceneNode()
        : m_name(),
          m_dirty(true),
          m_translate(Eigen::Vector3f::Zero()),
          m_scale { 1.0f, 1.0f, 1.0f },
          m_rotation(Eigen::Vector3f::Zero()) {}
    SceneNode(const Eigen::Vector3f& translate, const Eigen::Vector3f& scale,
              const Eigen::Vector3f& rotation, const std::string& name = "")
        : m_name(name),
          m_dirty(true),
          m_translate(translate),
          m_scale(scale),
          m_rotation(rotation) {};

    // Scene-teardown safety: an external holder (SceneCamera::m_node,
    // particle runtime bindings, actuator closures) can keep a
    // child alive past its parent's destruction while the children Vec is being
    // torn down. Clear the back-link so the survivor's UpdateTrans /
    // HitTestNode falls back to local trans instead of dereferencing freed
    // memory.
    ~SceneNode() {
        if (m_parent) {
            auto& anchors = m_parent->m_transform_anchors;
            anchors.retain([this](SceneNode* anchor) {
                return anchor != this;
            });
        }
        for (auto& c : m_children) {
            if (c) c->m_parent = nullptr;
        }
        for (auto* anchor : m_transform_anchors) {
            if (anchor && anchor->m_parent == this) anchor->m_parent = nullptr;
        }
    }

    const auto& Camera() const { return m_cameraName; }
    void        SetCamera(const std::string& name) { m_cameraName = name; }
    bool        Perspective() const { return m_perspective; }
    void        SetPerspective(bool value) { m_perspective = value; }
    bool        Reflected() const { return m_reflected; }
    void        SetReflected(bool value) { m_reflected = value; }
    void        AddMesh(std::shared_ptr<SceneMesh> mesh) { m_mesh = mesh; }
    void        AppendChild(Arc<SceneNode> sub) {
        sub->m_parent = this;
        // Stale ModelTrans on the child (cached without this new
        // parent context) would persist for the rest of the frame
        // otherwise — force a recompute on next UpdateTrans.
        sub->MarkTransDirty();
        m_children.push(rstd::move(sub));
    }
    auto            ChildIndex(const SceneNode& child) const -> Option<usize>;
    bool            MoveChild(SceneNode& child, usize index);
    Eigen::Matrix4d GetLocalTrans() const;

    const auto& Translate() const { return m_translate; }
    const auto& Rotation() const { return m_rotation; }
    const auto& Scale() const { return m_scale; }
    const auto& LocalFrame() const { return m_local_frame; }
    void        SetLocalFrame(Eigen::Matrix4d frame) {
        m_local_frame = rstd::move(frame);
        MarkTransDirty();
    }
    void SetRotation(Eigen::Vector3f v) {
        m_rotation = v;
        MarkTransDirty();
    }
    void RotateObjectSpace(const Eigen::Vector3f& rotation);
    void SetTranslate(Eigen::Vector3f v) {
        m_translate = v;
        MarkTransDirty();
    }
    void SetScale(Eigen::Vector3f v) {
        m_scale = v;
        MarkTransDirty();
    }

    // Local content size (image / text bbox). Zero means "unknown"; scripts
    // reading `thisLayer.size` then fall back to the legacy 100×100 stub.
    const auto& Size() const { return m_size; }
    void        SetSize(Eigen::Vector2f v) { m_size = v; }
    const auto& GeometryTransform() const { return m_geometry_transform; }
    void        SetGeometryTransform(Eigen::Matrix4d transform) {
        m_geometry_transform = rstd::move(transform);
    }

    // Script-driven per-frame overrides. The renderer maps these onto the
    // shader's available runtime tint uniforms without touching baked values
    // until script writes occur.
    //
    // Visibility writes are runtime alpha updates too: hidden nodes force 0,
    // and visible nodes restore the baked alpha after a prior hide.
    bool IsAlphaOverridden() const {
        return m_alpha_overridden || m_visible_overridden ||
               (m_alpha_source != nullptr && m_alpha_source->IsAlphaOverridden());
    }
    float EffectiveAlpha() const {
        float alpha = ! m_visible && m_visibility_affects_alpha
                          ? 0.0f
                          : (m_alpha_overridden ? m_user_alpha : m_base_alpha);
        if (m_alpha_source != nullptr && m_alpha_source->IsAlphaOverridden())
            alpha *= m_alpha_source->EffectiveAlpha();
        return alpha;
    }
    bool  Visible() const { return m_visible; }
    float UserAlpha() const { return m_user_alpha; }
    void  SetVisible(bool v) {
        // A sound layer is audible only while its layer is visible, which is
        // how scenes implement track selectors: the selector binds each sound
        // layer's visibility to a user property.
        if (m_sound_control.is_some() && v != m_visible) {
            if (v) {
                (*m_sound_control)->Play();
            } else {
                (*m_sound_control)->Stop();
            }
        }
        m_visible            = v;
        m_visible_overridden = true;
    }
    void SetUserAlpha(float v) {
        m_user_alpha       = v;
        m_alpha_overridden = true;
    }
    void SetOriginAnimation(SceneAnimationCurve curve) {
        m_origin_base  = m_translate;
        m_origin_curve = Some(rstd::move(curve));
    }
    void SetScaleAnimation(SceneAnimationCurve curve) {
        m_scale_base  = m_scale;
        m_scale_curve = Some(rstd::move(curve));
    }
    void SetRotationAnimation(SceneAnimationCurve curve) {
        m_rotation_base  = m_rotation;
        m_rotation_curve = Some(rstd::move(curve));
    }
    void SetAlphaAnimation(SceneAnimationCurve curve) { m_alpha_curve = Some(rstd::move(curve)); }
    void TickFieldAnimations(double runtime);
    void SetAlphaSource(SceneNode* node) { m_alpha_source = node; }

    ref<str> VisibleUserKey() const { return m_visible_user_binding.key.as_str(); }
    void     SetVisibleUserKey(String key) {
        m_visible_user_binding = SceneUserVisibilityBinding { .key = rstd::move(key) };
    }
    const SceneUserVisibilityBinding& VisibleUserBinding() const { return m_visible_user_binding; }
    void                              SetVisibleUserBinding(SceneUserVisibilityBinding binding) {
        m_visible_user_binding = std::move(binding);
    }

    bool  IsBrightnessOverridden() const { return m_brightness_overridden; }
    float Brightness() const { return m_brightness; }
    void  SetBrightness(float v) {
        m_brightness            = v;
        m_brightness_overridden = true;
    }

    bool                   IsColorOverridden() const { return m_color_overridden; }
    const Eigen::Vector3f& Color() const { return m_color; }
    void                   SetColor(Eigen::Vector3f v) {
        m_color            = v;
        m_color_overridden = true;
    }
    const Eigen::Vector3f& BaseColor() const { return m_base_color; }
    float                  BaseAlpha() const { return m_base_alpha; }
    void                   SetBaseColor(Eigen::Vector3f color, float alpha) {
        m_base_color = color;
        m_base_alpha = alpha;
    }

    // Per-texture-slot script-driven sprite-animation override. Wallpaper
    // Engine's setFrame(n) / play() / stop() control map onto these:
    //   current_frame >= 0  → renderer pins to that frame, ignoring elapsed-
    //                         time advancement.
    //   playing == false    → renderer holds the current auto-advance frame.
    //   default { -1, true }→ regular auto-advance.
    struct TextureAnimatorState {
        int  current_frame { -1 };
        bool playing { true };
    };
    TextureAnimatorState&       TexAnim() { return m_tex_anim; }
    const TextureAnimatorState& TexAnim() const { return m_tex_anim; }

    void Play() {
        if (m_sound_control) {
            (*m_sound_control)->Play();
            return;
        }
        if (m_particle_control) {
            (*m_particle_control)->Play();
            return;
        }
        if (m_video_control) {
            (*m_video_control)->Play();
            return;
        }
        m_layer_playing = true;
    }
    void Stop() {
        if (m_sound_control) {
            (*m_sound_control)->Stop();
            return;
        }
        if (m_particle_control) {
            (*m_particle_control)->Stop();
            return;
        }
        if (m_video_control) {
            (*m_video_control)->Stop();
            return;
        }
        m_layer_playing = false;
    }
    void Pause() {
        if (m_sound_control) {
            (*m_sound_control)->Pause();
            return;
        }
        if (m_particle_control) {
            (*m_particle_control)->Pause();
            return;
        }
        if (m_video_control) {
            (*m_video_control)->Pause();
            return;
        }
        m_layer_playing = false;
    }
    bool IsPlaying() const {
        if (m_sound_control) return (*m_sound_control)->IsPlaying();
        if (m_particle_control) return (*m_particle_control)->IsPlaying();
        if (m_video_control) return (*m_video_control)->Snapshot().playing;
        return m_layer_playing;
    }
    void SetSoundControl(Arc<dyn<SceneSoundControl>> control) {
        m_sound_control = Some(rstd::move(control));
    }
    Option<ref<dyn<SceneSoundControl>>> SoundControl() const {
        if (m_sound_control.is_none()) return None();
        return Some(m_sound_control->deref());
    }
    float Volume() const { return m_volume; }
    void  SetVolume(float volume) {
        m_volume = rstd::f32(volume).clamp(rstd::f32(), rstd::f32(1.0f)).to_primitive();
        if (m_sound_control) (*m_sound_control)->SetVolume(m_volume);
    }
    void SetParticleControl(Arc<dyn<SceneParticleControl>> control) {
        m_particle_control = Some(rstd::move(control));
    }
    Option<ref<dyn<SceneParticleControl>>> ParticleControl() const {
        if (m_particle_control.is_none()) return None();
        return Some(m_particle_control->deref());
    }
    Option<Arc<dyn<SceneParticleControl>>> ParticleControlHandle() const {
        if (m_particle_control.is_none()) return None();
        return Some((*m_particle_control).clone());
    }
    void SetVideoControl(Arc<VideoPlaybackState> control) {
        m_video_control = Some(rstd::move(control));
    }
    Option<Arc<VideoPlaybackState>> VideoControlHandle() const {
        return m_video_control.is_some() ? Some(m_video_control->clone())
                                         : None<Arc<VideoPlaybackState>>();
    }

    void CopyTrans(const SceneNode& node) {
        m_local_frame = node.m_local_frame;
        m_translate   = node.m_translate;
        m_scale       = node.m_scale;
        m_rotation    = node.m_rotation;
        MarkTransDirty();
    }

    void            UpdateTrans();
    Eigen::Matrix4d ModelTrans() const { return m_trans; };

    SceneMesh*                        Mesh() { return m_mesh.get(); }
    const std::shared_ptr<SceneMesh>& MeshShared() const { return m_mesh; }
    bool HasMaterial() const { return m_mesh && m_mesh->Material() != nullptr; };

    const auto& GetChildren() const { return m_children; }
    auto&       GetChildren() { return m_children; }

    const std::string& Name() const { return m_name; }
    SceneNode*         Parent() const { return m_parent; }

    // Anchor for transform-only inheritance. The node does NOT join `p`'s
    // children, so TraverseNode never visits it through `p`. Used for the
    // SceneNodeLayer composite quad: the quad needs spImgNode's
    // world transform but must not be rendered twice in scene-tree traversal.
    void SetParentAnchor(SceneNode* p) {
        if (m_parent == p) {
            MarkTransDirty();
            return;
        }
        if (m_parent) {
            auto& anchors = m_parent->m_transform_anchors;
            anchors.retain([this](SceneNode* anchor) {
                return anchor != this;
            });
        }
        m_parent = p;
        if (m_parent) {
            auto& anchors = m_parent->m_transform_anchors;
            bool  found { false };
            for (auto* anchor : anchors) found = found || anchor == this;
            if (! found) anchors.push(this);
        }
        MarkTransDirty();
    }

    // BFS over self + descendants; returns first node whose Name() matches.
    SceneNode* FindByName(std::string_view name);

    SceneNodeId              Identity() const { return m_identity; }
    Option<WallpaperLayerId> WallpaperIdentity() const { return m_wallpaper_identity; }
    void        AttachLayer(std::shared_ptr<SceneNodeLayer> layer) { m_layer = rstd::move(layer); }
    bool        HasLayer() const { return static_cast<bool>(m_layer); }
    auto&       Layer() { return m_layer; }
    const auto& Layer() const { return m_layer; }

    i32  ID() const { return m_id; }
    i32& ID() { return m_id; }

private:
    friend class Scene;

    void MarkTransDirty();

    SceneNodeId              m_identity;
    Option<WallpaperLayerId> m_wallpaper_identity;
    i32                      m_id { -1 };
    std::string              m_name;

    bool            m_dirty;
    Eigen::Matrix4d m_trans;

    Eigen::Vector3f m_translate { 0.0f, 0.0f, 0.0f };
    Eigen::Vector3f m_scale { 1.0f, 1.0f, 1.0f };
    Eigen::Vector3f m_rotation { 0.0f, 0.0f, 0.0f };
    Eigen::Matrix4d m_local_frame { Eigen::Matrix4d::Identity() };
    Eigen::Vector2f m_size { 0.0f, 0.0f };
    Eigen::Matrix4d m_geometry_transform { Eigen::Matrix4d::Identity() };

    bool                                   m_visible { true };
    bool                                   m_visibility_affects_alpha { true };
    SceneUserVisibilityBinding             m_visible_user_binding {};
    bool                                   m_visible_overridden { false };
    float                                  m_user_alpha { 1.0f };
    bool                                   m_alpha_overridden { false };
    SceneNode*                             m_alpha_source { nullptr };
    Eigen::Vector3f                        m_origin_base { 0.0f, 0.0f, 0.0f };
    Eigen::Vector3f                        m_scale_base { 1.0f, 1.0f, 1.0f };
    Eigen::Vector3f                        m_rotation_base { 0.0f, 0.0f, 0.0f };
    Option<SceneAnimationCurve>            m_origin_curve;
    Option<SceneAnimationCurve>            m_scale_curve;
    Option<SceneAnimationCurve>            m_rotation_curve;
    Option<SceneAnimationCurve>            m_alpha_curve;
    float                                  m_brightness { 1.0f };
    bool                                   m_brightness_overridden { false };
    Eigen::Vector3f                        m_color { 1.0f, 1.0f, 1.0f };
    bool                                   m_color_overridden { false };
    Eigen::Vector3f                        m_base_color { 1.0f, 1.0f, 1.0f };
    float                                  m_base_alpha { 1.0f };
    TextureAnimatorState                   m_tex_anim {};
    bool                                   m_layer_playing { true };
    float                                  m_volume { 1.0f };
    Option<Arc<dyn<SceneSoundControl>>>    m_sound_control;
    Option<Arc<dyn<SceneParticleControl>>> m_particle_control;
    Option<Arc<VideoPlaybackState>>        m_video_control;

    std::shared_ptr<SceneMesh> m_mesh;

    std::string m_cameraName;
    bool        m_perspective { false };
    bool        m_reflected { false };

    // Raw back-link. Safe because tree topology is frozen post-parse (see
    // class header) and the dtor clears children's m_parent before any
    // out-of-order teardown can dereference a stale pointer.
    SceneNode* m_parent { nullptr };

    Vec<Arc<SceneNode>>             m_children;
    Vec<SceneNode*>                 m_transform_anchors;
    std::shared_ptr<SceneNodeLayer> m_layer;
};

// ============================================================================
// SceneNodeLayer.h
// ============================================================================

enum class SceneEffectTargetKind
{
    Named,
    LayerPrevious,
    LayerNext,
};

struct SceneEffectTarget {
    SceneEffectTarget() = default;
    SceneEffectTarget(std::string value)
        : kind(SceneEffectTargetKind::Named), key(std::move(value)) {}
    SceneEffectTarget(std::string_view value): kind(SceneEffectTargetKind::Named), key(value) {}

    static auto Named(std::string value) -> SceneEffectTarget {
        return SceneEffectTarget(std::move(value));
    }
    static auto LayerPrevious() -> SceneEffectTarget {
        SceneEffectTarget target;
        target.kind = SceneEffectTargetKind::LayerPrevious;
        return target;
    }
    static auto LayerNext() -> SceneEffectTarget {
        SceneEffectTarget target;
        target.kind = SceneEffectTargetKind::LayerNext;
        return target;
    }

    SceneEffectTargetKind kind { SceneEffectTargetKind::Named };
    std::string           key;
};

struct SceneImageEffectNode {
    SceneEffectTarget            output;
    Arc<SceneNode>               sceneNode;
    bool                         uses_unit_final_quad { false };
    SceneShaderValueAnimationMap final_quad_shader_values;
};

struct SceneImageEffect {
    enum class CmdType
    {
        Copy,
    };
    struct Command {
        CmdType           cmd { CmdType::Copy };
        SceneEffectTarget dst;
        SceneEffectTarget src;
        i32               afterpos { 0 };
    };
    std::string                     name;
    SceneEffectId                   id;
    SceneNodeId                     owner;
    std::vector<Command>            commands;
    std::list<SceneImageEffectNode> nodes;
    SceneUserVisibilityBinding      visible_user_binding;
    bool                            runtime_visible { true };
};

class SceneNodeLayer {
public:
    SceneNodeLayer(SceneNode* node, float w, float h, std::string_view composite_target);

    void AddEffect(const std::shared_ptr<SceneImageEffect>& node) {
        m_effects.push_back(node);
        m_resolved = false;
    }
    bool SetEffectRuntimeVisible(SceneImageEffect& effect, bool visible) {
        if (effect.runtime_visible == visible) return false;
        effect.runtime_visible = visible;
        m_resolved             = false;
        return true;
    }
    usize EffectCount() const { return usize(m_effects.size()); }
    auto& GetEffect(usize index) { return m_effects.at(index.to_primitive()); }
    std::shared_ptr<SceneImageEffect> FindEffect(std::string_view name) {
        auto it = std::find_if(m_effects.begin(), m_effects.end(), [name](const auto& effect) {
            return effect && effect->name == name;
        });
        return it == m_effects.end() ? nullptr : *it;
    }
    bool HasRuntimeVisibleEffect() const {
        return std::any_of(m_effects.begin(), m_effects.end(), [](const auto& effect) {
            return effect && effect->runtime_visible;
        });
    }
    bool RequiresIntermediateTarget() const {
        return m_final_resolve_effect || m_published_effect || m_visible_resolve_effect ||
               m_effects.empty() || HasRuntimeVisibleEffect();
    }
    bool HasRenderEffects() const {
        return m_final_resolve_effect || m_published_effect || m_visible_resolve_effect ||
               HasRuntimeVisibleEffect();
    }
    void             SetSourceDraw(SceneNode& node);
    void             ConfigureSourceDraw(bool intermediate);
    const auto&      CompositeTarget() const { return m_composite_target; }
    SceneMesh&       FinalMesh() { return *m_final_mesh.get(); }
    const SceneMesh& FinalMesh() const { return *m_final_mesh.as_ptr(); }
    void             AddPrefillNode(SceneImageEffectNode node) {
        m_prefill_nodes.push_back(std::move(node));
        m_resolved = false;
    }
    auto& PrefillNodes() { return m_prefill_nodes; }
    void  SetFullscreen(bool value) {
        fullscreen = value;
        m_resolved = false;
    }
    void SetFinalBlend(BlendMode m) {
        m_final_blend = m;
        m_resolved    = false;
    }
    void SetFinalMaterialState(const SceneMaterial& material) {
        m_final_blend       = material.blenmode;
        m_final_depth_test  = material.depth_test;
        m_final_depth_write = material.depth_write;
        m_final_cull_mode   = material.cull_mode;
        m_resolved          = false;
    }
    void SetFinalTarget(std::string t) {
        m_final_target = std::move(t);
        m_resolved     = false;
    }
    const auto& FinalTarget() const { return m_final_target; }
    void        SetFinalCamera(std::string camera) {
        if (m_final_camera == camera) return;
        m_final_camera = std::move(camera);
        m_resolved     = false;
    }
    void SetFinalResolveEffect(std::shared_ptr<SceneImageEffect> effect) {
        m_final_resolve_effect = std::move(effect);
        m_resolved             = false;
    }
    const auto& FinalResolveEffect() const { return m_final_resolve_effect; }
    void        SetPublishedEffect(std::shared_ptr<SceneImageEffect> effect) {
        m_published_effect = rstd::move(effect);
        m_resolved         = false;
    }
    const auto& PublishedEffect() const { return m_published_effect; }
    void        SetVisibleResolveEffect(std::shared_ptr<SceneImageEffect> effect) {
        m_visible_resolve_effect = rstd::move(effect);
        m_resolved               = false;
    }
    const auto& VisibleResolveEffect() const { return m_visible_resolve_effect; }
    bool        PublishesOutput() const { return static_cast<bool>(m_published_effect); }
    void        SetVisibleOutputEnabled(bool value) {
        if (m_visible_output_enabled == value) return;
        m_visible_output_enabled = value;
        m_resolved               = false;
    }
    bool        VisibleOutputEnabled() const { return m_visible_output_enabled; }
    const auto& ResolvedEffects() const { return m_resolved_effects; }
    auto        ResolvedTarget(const SceneImageEffectNode& node) const -> SceneEffectTarget {
        if (m_direct_final_output == &node) return SceneEffectTarget::Named(m_final_target);
        return node.output;
    }
    void SetFinalLocal(bool value) {
        m_final_local = value;
        m_resolved    = false;
    }
    void SetSkipWhenNoRuntimeEffect(bool value) { m_skip_when_no_runtime_effect = value; }
    bool SkipWhenNoRuntimeEffect() const { return m_skip_when_no_runtime_effect; }
    void SetRequiresSourceDraw(bool value) {
        if (m_requires_source_draw == value) return;
        m_requires_source_draw = value;
        m_resolved             = false;
    }
    bool RequiresSourceDraw() const { return m_requires_source_draw; }
    void SetIntermediateSourceBlend(BlendMode value) {
        m_intermediate_source_blend = Some(value);
        m_resolved                  = false;
    }
    Option<BlendMode> IntermediateSourceBlend() const { return m_intermediate_source_blend; }

    // Idempotent: second and later calls are no-ops until any of the
    // mutating setters above (or AddEffect) flips m_resolved back to false.
    void ResolveEffect(const SceneMesh& defualt_mesh, std::string_view effect_cam);

private:
    SceneNode*  m_worldNode;
    SceneNode*  m_sourceNode;
    float       m_width { 1.0f };
    float       m_height { 1.0f };
    std::string m_composite_target;
    std::string m_source_camera;

    bool              fullscreen { false };
    bool              m_final_local { false };
    Box<SceneMesh>    m_final_mesh;
    BlendMode         m_final_blend;
    bool              m_final_depth_test { false };
    bool              m_final_depth_write { false };
    CullMode          m_final_cull_mode { CullMode::None };
    std::string       m_final_target { rstd::cppstd::to_string(SpecTex_Default) };
    std::string       m_final_camera;
    bool              m_skip_when_no_runtime_effect { false };
    bool              m_requires_source_draw { true };
    Option<BlendMode> m_intermediate_source_blend;
    bool              m_visible_output_enabled { true };
    bool              m_resolved { false };

    std::vector<std::shared_ptr<SceneImageEffect>> m_effects;
    std::shared_ptr<SceneImageEffect>              m_final_resolve_effect;
    std::shared_ptr<SceneImageEffect>              m_published_effect;
    std::shared_ptr<SceneImageEffect>              m_visible_resolve_effect;
    std::vector<SceneImageEffect*>                 m_resolved_effects;
    std::vector<SceneImageEffectNode>              m_prefill_nodes;
    SceneImageEffectNode*                          m_direct_final_output { nullptr };
};

struct SceneImageEffectRef {
    SceneEffectId id;
};

// ============================================================================
// ScenePostProcess.h
// First-class global post-process. Each pass uses the scene's default
// fullscreen NDC quad as its mesh - no camera, no transform, no image.
// Runs after main scene-graph traversal and writes through SpecTex_Default.
// ============================================================================

struct ScenePostProcessPass {
    Arc<SceneNode> node;   // synthetic; mesh + material only
    std::string    output; // RT key; empty -> SpecTex_Default
};

struct ScenePostProcessCopy {
    std::string src;
    std::string dst;
};

class ScenePostProcessStep {
    RSTD_ENUM(ScenePostProcessStep, (Pass, (ScenePostProcessPass value;)),
              (Copy, (ScenePostProcessCopy value;)))
};

struct ScenePostProcess {
    std::string               name;
    Vec<ScenePostProcessStep> steps;
};

// SceneLight + SceneLightType live in the `wescene.scene:lighting` partition
// (see src/Scene/Lighting/Lighting.cppm).

class Scene;

// ============================================================================
// Interface/IImageParser.h (relocated)
// ============================================================================

enum class ImageParseErrorKind
{
    MissingContent,
    InvalidData,
    DecodeFailed,
};

struct ImageParseError {
    ImageParseErrorKind kind { ImageParseErrorKind::InvalidData };
    String              message;
};

struct IImageParser {
    using Trait                  = IImageParser;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = IImageParser;

        auto Parse(ref<str> name) const -> Result<Arc<Image>, ImageParseError> {
            return rstd::trait_call<0>(this, name);
        }
        auto ParseMany(slice<String> names) const -> Vec<Result<Arc<Image>, ImageParseError>> {
            return rstd::trait_call<1>(this, names);
        }
        auto ParseHeader(ref<str> name) const -> Result<ImageHeader, ImageParseError> {
            return rstd::trait_call<2>(this, name);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Parse, &T::ParseMany, &T::ParseHeader>;
};

struct SceneDrawItemRecord {
    SceneDrawItemId id;
    SceneNodeId     node;
    SceneMeshId     mesh;
    SceneMaterialId material;
    u32             submesh_index { 0 };
};

struct DrawItemView {
    SceneNode*                node { nullptr };
    SceneMesh*                mesh { nullptr };
    const SceneMesh::Submesh* submesh { nullptr };
    SceneMaterial*            material { nullptr };
    u32                       submesh_index { 0 };
};

class SceneResourceIndex : NoCopy, NoMove {
public:
    SceneResourceIndex() = default;

    void Rebuild(Scene& scene, u32 generation);

    u32  Generation() const { return m_generation; }
    bool Empty() const { return m_scene == nullptr; }

    slice<SceneDrawItemRecord> DrawItems() const { return m_draw_items.as_slice(); }
    slice<SceneNode*>          Nodes() const { return m_nodes.as_slice(); }

    Option<SceneNodeId>         nodeId(const SceneNode& node) const;
    Option<SceneMeshId>         meshId(const SceneMesh& mesh) const;
    Option<SceneMaterialId>     materialId(const SceneMaterial& material) const;
    Option<SceneDrawItemId>     drawItemFor(SceneNodeId node, u32 submesh_index) const;
    Option<SceneTextureId>      textureId(ref<str> url) const;
    Option<SceneRenderTargetId> renderTargetId(ref<str> key) const;
    Option<SceneCameraId>       cameraId(ref<str> name) const;

    Option<DrawItemView>     resolve(SceneDrawItemId id) const;
    SceneNode*               node(SceneNodeId id) const;
    SceneMesh*               mesh(SceneMeshId id) const;
    SceneMaterial*           material(SceneMaterialId id) const;
    const SceneTexture*      texture(SceneTextureId id) const;
    const SceneRenderTarget* renderTarget(SceneRenderTargetId id) const;
    SceneRenderTarget*       mutableRenderTarget(SceneRenderTargetId id) const;
    SceneCamera*             camera(SceneCameraId id) const;
    slice<SceneMesh*>        Meshes() const { return m_meshes.as_slice(); }
    slice<SceneMaterial*>    Materials() const { return m_materials.as_slice(); }

private:
    Scene* m_scene { nullptr };
    u32    m_generation { 0 };

    Vec<SceneNode*>          m_nodes;
    Vec<SceneMesh*>          m_meshes;
    Vec<SceneMaterial*>      m_materials;
    Vec<String>              m_texture_keys;
    Vec<String>              m_render_target_keys;
    Vec<String>              m_camera_keys;
    Vec<SceneDrawItemRecord> m_draw_items;

    HashMap<const SceneNode*, SceneNodeId>         m_node_ids;
    HashMap<const SceneMesh*, SceneMeshId>         m_mesh_ids;
    HashMap<const SceneMaterial*, SceneMaterialId> m_material_ids;
    HashMap<String, SceneTextureId>                m_texture_ids;
    HashMap<String, SceneRenderTargetId>           m_render_target_ids;
    HashMap<String, SceneCameraId>                 m_camera_ids;
};

struct SceneTextureFrameView {
    rstd::array<float, 4> rotation { 1.0f, 0.0f, 0.0f, 1.0f };
    rstd::array<float, 2> translation { 0.0f, 0.0f };
    usize                 image_slot { 0 };
    u64                   revision { 1 };
};

struct SceneTextureAnimationView {
    using Trait                  = SceneTextureAnimationView;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = SceneTextureAnimationView;

        auto TextureFrame(SceneDrawItemId draw, usize texture_index) const
            -> Option<SceneTextureFrameView> {
            return rstd::trait_call<0>(this, draw, texture_index);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::TextureFrame>;
};

class SceneTextureAnimationRegistry : NoCopy, NoMove {
public:
    void Rebuild(const Scene&);
    void Advance(f64 delta);
    auto Frame(SceneDrawItemId, usize texture_index) const -> Option<SceneTextureFrameView>;

private:
    struct Animation {
        SpriteAnimation sprite;
        u64             revision { 1 };
    };

    struct Binding {
        String texture;
        usize  held_frame { 0 };
        bool   was_playing { true };
    };

    struct Entry {
        SceneNode*              node { nullptr };
        HashMap<usize, Binding> bindings;
    };

    static u64 Key(SceneDrawItemId draw) {
        return (rstd::as_cast<u64>(draw.generation) << u64(32)) | rstd::as_cast<u64>(draw.index);
    }

    HashMap<String, Animation> m_animations;
    HashMap<u64, Entry>        m_entries;
};

class SceneTextureAnimationRuntime {
public:
    explicit SceneTextureAnimationRuntime(SceneTextureAnimationRegistry& animations)
        : m_animations(rstd::addressof(animations)) {}

    void Update(ref<SceneFrame> frame) { m_animations->Advance(frame->delta); }

private:
    SceneTextureAnimationRegistry* m_animations;
};

struct RenderSceneVersion {
    u64 value { 0 };
};

struct RenderItemId {
    u32 index { u32::MAX };
    u64 generation { 0 };

    bool Valid() const noexcept { return index != u32::MAX && generation != u64(); }
};

struct RenderTextureDescId {
    u32 index { u32::MAX };
    u64 generation { 0 };
};

struct RenderTargetDescId {
    u32 index { u32::MAX };
    u64 generation { 0 };
};

struct RenderItemRecord {
    RenderItemId               id;
    SceneDrawItemId            scene_draw_item;
    SceneNodeId                scene_node;
    SceneMeshId                scene_mesh;
    SceneMaterialId            scene_material;
    WallpaperLayerId           source_layer;
    u32                        submesh_index { 0 };
    Option<RenderTargetDescId> output_override;
};

struct RenderTextureDescRecord {
    RenderTextureDescId             id;
    SceneTextureId                  scene_texture;
    String                          key;
    SceneTexture                    desc;
    u64                             content_revision { 0 };
    Option<Arc<VideoPlaybackState>> video_control;
};

struct RenderTargetDescRecord {
    RenderTargetDescId  id;
    SceneRenderTargetId scene_render_target;
    String              key;
    SceneRenderTarget   desc;
};

struct RenderLinkSourceRecord {
    WallpaperLayerId   source_layer;
    SceneNodeId        scene_node;
    String             render_target_key;
    RenderTargetDescId render_target;
};

struct SceneShadowViewport {
    f32 x {};
    f32 y {};
    f32 width {};
    f32 height {};
    i32 scissor_x {};
    i32 scissor_y {};
    u32 scissor_width {};
    u32 scissor_height {};
};

} // namespace owe

export namespace rstd
{

template<>
struct Impl<Copy, owe::SceneShadowViewport> {};

} // namespace rstd

export namespace owe
{

struct SceneShadowDefinition {
    String                   target;
    Vec<SceneShadowViewport> viewports;

    auto Clone() const -> SceneShadowDefinition {
        return {
            .target    = target.clone(),
            .viewports = viewports.clone(),
        };
    }
};

struct RenderShadowCasterRecord {
    RenderItemId                   render_item;
    std::shared_ptr<SceneMaterial> material;
    u32                            instance_count { 1 };
};

struct SceneMeshDirtyEvent {
    SceneMeshId         mesh;
    SceneMeshDirtyFlags flags { SceneMeshDirtyNone };
};

struct SceneMaterialDirtyEvent {
    SceneMaterialId         material;
    SceneMaterialDirtyFlags flags { SceneMaterialDirtyNone };
};

struct SceneRenderTargetDirtyEvent {
    String name;
    i32    old_width { 0 };
    i32    old_height { 0 };
    i32    width { 0 };
    i32    height { 0 };
};

struct SceneMaterialTextureSlotMutation {
    bool                    changed { false };
    Option<SceneMaterialId> material;
};

struct SceneShaderVariantMutation {
    std::shared_ptr<SceneShader> shader;
    SceneShaderVariantDesc       variant;
};

struct SceneMaterialShaderVariantMutation {
    bool                    changed { false };
    Option<SceneMaterialId> material;
};

enum class SceneUserPropertyDiagnosticCode
{
    SceneVfsUnavailable,
    UnsupportedShaderComboValue,
    MissingShaderVariantDescriptor,
    ShaderComboCompileFailed,
};

struct SceneUserPropertyDiagnostic {
    String                          key;
    SceneUserPropertyDiagnosticCode code {
        SceneUserPropertyDiagnosticCode::UnsupportedShaderComboValue
    };
    String material;
    String combo;
    String message;

    auto Clone() const -> SceneUserPropertyDiagnostic {
        return {
            .key      = key.clone(),
            .code     = code,
            .material = material.clone(),
            .combo    = combo.clone(),
            .message  = message.clone(),
        };
    }
};

class AudioResponseDemand : NoCopy, NoMove {
public:
    using Callback = Arc<dyn<rstd::Fn<void(bool)>>>;

    AudioResponseDemand();
    ~AudioResponseDemand();

    auto Acquire() -> Box<dyn<UniformBindingLease>>;
    void SetCallback(Option<Callback> callback);
    template<typename F>
    void SetCallback(F callback) {
        SetCallback(Some(Callback::make(rstd::move(callback))));
    }
    void SetEnabled(bool enabled);
    bool Active() const;

private:
    struct State;
    struct Lease;
    static void Update(State& state, i32 delta);
    Arc<State>  m_state;
};

class SceneAudioAverage : NoCopy, NoMove {
public:
    auto Len() const -> usize { return m_values.len(); }
    auto Load(usize index) const -> f32 { return m_values[index].load(Ordering::Relaxed); }
    void Store(usize index, f32 value) { m_values[index].store(value, Ordering::Relaxed); }

private:
    array<Atomic<f32>, 16> m_values {};
};

class RenderSceneSnapshot {
public:
    RenderSceneSnapshot() = default;

    void Rebuild(Scene& scene, RenderSceneVersion version);

    RenderSceneVersion Version() const { return m_version; }

    slice<RenderItemRecord>        RenderItems() const { return m_render_items.as_slice(); }
    slice<RenderTextureDescRecord> TextureDescs() const { return m_texture_descs.as_slice(); }
    slice<RenderTargetDescRecord>  RenderTargetDescs() const {
        return m_render_target_descs.as_slice();
    }
    slice<SceneShadowDefinition> ShadowDefinitions() const {
        return m_shadow_definitions.as_slice();
    }
    slice<RenderShadowCasterRecord> ShadowCasters() const { return m_shadow_casters.as_slice(); }

    const RenderItemRecord*        renderItem(RenderItemId id) const;
    const RenderTextureDescRecord* textureDesc(RenderTextureDescId id) const;
    const RenderTargetDescRecord*  renderTargetDesc(RenderTargetDescId id) const;

    Option<RenderItemId>          renderItemFor(SceneDrawItemId id) const;
    Option<RenderTextureDescId>   textureDescId(ref<str> key) const;
    Option<RenderTargetDescId>    renderTargetDescId(ref<str> key) const;
    slice<RenderItemId>           renderItemsFor(WallpaperLayerId id) const;
    slice<RenderItemId>           renderItemsFor(SceneMaterialId id) const;
    slice<RenderItemId>           renderItemsFor(SceneMeshId id) const;
    const RenderLinkSourceRecord* linkSource(WallpaperLayerId id) const;
    const BTreeSet<i32>&          LinkedLayerIds() const { return m_linked_layer_ids; }
    bool                          HasLinkConsumer(WallpaperLayerId id) const;

private:
    RenderSceneVersion m_version;

    Vec<RenderItemRecord>         m_render_items;
    Vec<RenderTextureDescRecord>  m_texture_descs;
    Vec<RenderTargetDescRecord>   m_render_target_descs;
    Vec<SceneShadowDefinition>    m_shadow_definitions;
    Vec<RenderShadowCasterRecord> m_shadow_casters;

    HashMap<rstd::uint64_t, RenderItemId>      m_render_item_ids;
    HashMap<String, RenderTextureDescId>       m_texture_desc_ids;
    HashMap<String, RenderTargetDescId>        m_render_target_desc_ids;
    HashMap<i32, Vec<RenderItemId>>            m_source_layer_items;
    HashMap<rstd::uint64_t, Vec<RenderItemId>> m_material_render_items;
    HashMap<rstd::uint64_t, Vec<RenderItemId>> m_mesh_render_items;
    Vec<RenderLinkSourceRecord>                m_link_sources;
    HashMap<i32, u32>                          m_link_source_ids;
    BTreeSet<i32>                              m_linked_layer_ids;
};

RenderSceneSnapshot ExtractRenderSceneSnapshot(Scene& scene);

// ============================================================================
// Scene.h
// ============================================================================

class Scene : NoCopy, NoMove {
public:
    Scene();
    ~Scene();

    auto RegisterLight(Box<SceneLight> light) -> mut_ref<SceneLight>;
    auto Lights() const -> slice<Box<SceneLight>>;
    auto RegisterPostProcess(Box<ScenePostProcess> post_process) -> mut_ref<ScenePostProcess>;
    auto PostProcesses() const -> slice<Box<ScenePostProcess>>;
    void RegisterShadowDefinition(SceneShadowDefinition definition);
    auto ShadowDefinitions() const -> slice<SceneShadowDefinition>;

    struct ShaderUserBinding {
        std::shared_ptr<SceneMaterial> material;
        String                         uniform;
    };

    void RegisterShaderUserBinding(String key, std::shared_ptr<SceneMaterial> material,
                                   String uniform);
    auto ShaderUserBindings(ref<str> key) const -> slice<ShaderUserBinding>;

    struct ShaderComboUserBinding {
        std::shared_ptr<SceneMaterial> material;
        String                         combo;
        String                         fallback;
        HashMap<String, String>        options;
    };
    void RegisterShaderComboUserBinding(String key, ShaderComboUserBinding binding);
    auto ShaderComboUserBindings(ref<str> key) const -> slice<ShaderComboUserBinding>;

    // Like the material bindings above, this index is scene-wide and outlives
    // the parse, but not every parsed node reaches the finalized scene: the
    // prototype objects ResolveRegisteredAsset parses purely as spawn
    // templates are dropped from the node map and die with the parse context.
    // Retain both the node and its materials so a binding left pointing at
    // such an object stays valid instead of dangling.
    struct ImagePropertyBinding {
        Arc<SceneNode>                      node;
        Vec<std::shared_ptr<SceneMaterial>> materials;
    };
    void RegisterImageColorUserBinding(String key, const Arc<SceneNode>& node,
                                       slice<std::shared_ptr<SceneMaterial>> materials);
    void RegisterImageAlphaUserBinding(String key, const Arc<SceneNode>& node,
                                       slice<std::shared_ptr<SceneMaterial>> materials);
    auto ImageColorUserBindings(ref<str> key) const -> slice<ImagePropertyBinding>;
    auto ImageAlphaUserBindings(ref<str> key) const -> slice<ImagePropertyBinding>;

    void RegisterUserTextBinding(String key, Box<dyn<FnMut<void(ref<str>)>>> setter);
    bool ApplyUserTextBindings(ref<str> key, const Json& property);

    void RegisterUserPropertyBinding(String key, Box<dyn<FnMut<void(ref<Json>)>>> setter);
    bool ApplyUserPropertyBindings(ref<str> key, const Json& property);

    struct MaterialTextureUserBinding {
        std::shared_ptr<SceneMaterial> material;
        u32                            slot { 0 };
        String                         fallback;
    };
    void RegisterMaterialTextureUserBinding(String key, MaterialTextureUserBinding binding);
    auto MaterialTextureUserBindings(ref<str> key) const -> slice<MaterialTextureUserBinding>;

    void RegisterCameraPath(Arc<SceneCameraPath>);
    void RegisterCameraPathUserBinding(String key, Arc<SceneCameraPath>);
    void RegisterTexture(String name, SceneTexture texture);
    auto Texture(ref<str> name) const -> Option<ref<SceneTexture>>;
    auto TextureContentRevision(ref<str> name) const -> u64;
    auto VideoControl(ref<str> name) const -> Option<Arc<VideoPlaybackState>>;
    auto TextureNames() const -> slice<String> { return m_texture_names.as_slice(); }
    void RegisterRenderTarget(String name, SceneRenderTarget target);
    auto RenderTarget(ref<str> name) const -> Option<ref<SceneRenderTarget>>;
    auto RenderTargetMut(ref<str> name) -> Option<mut_ref<SceneRenderTarget>>;
    auto RenderTargetNames() const -> slice<String> { return m_render_target_names.as_slice(); }
    void RegisterCamera(String name, Arc<SceneCamera> camera);
    auto Camera(ref<str> name) const -> Option<ref<SceneCamera>>;
    auto CameraMut(ref<str> name) -> Option<mut_ref<SceneCamera>>;
    auto CameraHandle(ref<str> name) const -> Option<Arc<SceneCamera>>;
    auto CameraNames() const -> slice<String> { return m_camera_names.as_slice(); }
    bool SetActiveCamera(ref<str> name);
    auto ActiveCamera() const -> Option<ref<SceneCamera>>;
    auto ActiveCameraHandle() const -> Option<Arc<SceneCamera>>;
    auto ActiveCameraTransforms() const -> Option<SceneCameraTransforms>;
    bool SetActiveCameraTransforms(const SceneCameraTransforms& transforms);

    Option<SceneImageEffectRef> FindNodeImageEffect(const SceneNode& node, std::string_view name);
    Option<SceneImageEffectRef> FindNodeImageEffect(const SceneNode& node, usize index);
    usize                       NodeImageEffectCount(const SceneNode& node);
    String                      ImageEffectName(const SceneImageEffectRef& ref) const;
    bool                        ImageEffectRuntimeVisible(const SceneImageEffectRef& ref) const;
    SceneMaterial*              ImageEffectMaterial(const SceneImageEffectRef& ref, usize index);
    bool SetImageEffectRuntimeVisible(const SceneImageEffectRef& ref, bool visible);
    void EnablePlanarReflection();
    bool PlanarReflectionEnabled() const { return m_planar_reflection_enabled; }
    bool ConsumeRenderGraphDirty();
    bool ApplyUserNodeVisibilityBindings(std::string_view key, const Json& property);
    bool ApplyUserImageEffectVisibilityBindings(std::string_view key, const Json& property);
    bool ApplyUserLightVisibilityBindings(std::string_view key, const Json& property);
    bool ApplyUserCameraPathVisibilityBindings(std::string_view key, const Json& property);

    void RegisterSoundVolumeBinding(ref<str> key, Arc<dyn<SceneSoundControl>> control) {
        auto controls = m_sound_volume_user_index.get_mut(key);
        if (controls.is_none()) {
            (void)m_sound_volume_user_index.insert(String::make(key),
                                                   Vec<Arc<dyn<SceneSoundControl>>> {});
            controls = m_sound_volume_user_index.get_mut(key);
        }
        (*controls)->push(rstd::move(control));
    }
    auto SoundVolumeBindings(ref<str> key) const -> slice<Arc<dyn<SceneSoundControl>>> {
        auto controls = m_sound_volume_user_index.get(key);
        if (controls.is_none()) return {};
        return (*controls)->as_slice();
    }
    void RegisterParticleOverrideBinding(String                                 key,
                                         Arc<dyn<SceneParticleOverrideControl>> control) {
        auto controls = m_particle_override_user_index.get_mut(key.as_str());
        if (controls.is_none()) {
            (void)m_particle_override_user_index.insert(
                key.clone(), Vec<Arc<dyn<SceneParticleOverrideControl>>> {});
            controls = m_particle_override_user_index.get_mut(key.as_str());
        }
        (*controls)->push(rstd::move(control));
    }
    auto ParticleOverrideBindings(ref<str> key) const
        -> slice<Arc<dyn<SceneParticleOverrideControl>>> {
        auto controls = m_particle_override_user_index.get(key);
        if (controls.is_none()) return {};
        return (*controls)->as_slice();
    }

    auto Root() const -> ref<SceneNode> { return m_scene_graph.deref(); }
    auto RootMut() -> mut_ref<SceneNode> { return m_scene_graph.deref_mut(); }
    void SetRoot(Box<SceneNode> root) {
        RegisterNode(*root);
        m_scene_graph = rstd::move(root);
    }
    SceneNodeId   RegisterNode(SceneNode& node, Option<WallpaperLayerId> wallpaper = None());
    SceneEffectId RegisterEffect(SceneNodeId owner, SceneNodeLayer& layer,
                                 std::shared_ptr<SceneImageEffect> effect);
    void          AttachRuntimeNode(SceneNode& parent, Arc<SceneNode> node);
    auto          LayerIndex(const SceneNode& node) const -> Option<usize>;
    bool          SortLayer(SceneNode& node, usize index);
    auto AudioDemand() const -> ref<AudioResponseDemand> { return m_audio_response_demand.deref(); }
    auto AudioDemandMut() -> mut_ref<AudioResponseDemand> {
        return m_audio_response_demand.deref_mut();
    }
    auto AudioDemandHandle() const -> Arc<AudioResponseDemand> {
        return m_audio_response_demand.clone();
    }

    void SetImageParser(Box<dyn<IImageParser>> parser) {
        m_image_parser = Some(rstd::move(parser));
    }
    auto ParseImage(ref<str> name) const -> Result<Arc<Image>, ImageParseError>;
    auto ParseImages(slice<String> names) const -> Vec<Result<Arc<Image>, ImageParseError>>;
    auto ParseImageHeader(ref<str> name) const -> Result<ImageHeader, ImageParseError>;
    void RegisterRuntimeImage(String name, Arc<Image> image);

    template<typename T>
    void InstallExtension(Box<T> extension) {
        for (usize index {}; index < m_extensions.len(); ++index) {
            if (! rstd::any::is<Box<T>>(m_extensions[index].as_ref())) continue;
            m_extensions[index] = Box<dyn<Any>>::make(rstd::move(extension));
            return;
        }
        m_extensions.push(Box<dyn<Any>>::make(rstd::move(extension)));
    }

    template<typename T>
    auto Extension() const -> Option<ref<T>> {
        for (usize index {}; index < m_extensions.len(); ++index) {
            auto holder = rstd::any::downcast_ref<Box<T>>(m_extensions[index].as_ref());
            if (holder.is_some()) return Some((**holder).as_ref());
        }
        return None();
    }

    template<typename T>
    auto ExtensionMut() -> Option<mut_ref<T>> {
        for (usize index {}; index < m_extensions.len(); ++index) {
            auto holder = rstd::any::downcast_mut<Box<T>>(m_extensions[index].deref_mut());
            if (holder.is_some()) return Some((**holder).deref_mut());
        }
        return None();
    }

    auto Register(Box<dyn<UniformSource>> source) -> UniformSourceId {
        return m_uniforms.Register(rstd::move(source));
    }
    bool AttachGlobal(UniformSourceId source, i32 priority = i32()) {
        return m_uniforms.AttachGlobal(source, priority);
    }
    bool AttachNode(SceneNodeId node, UniformSourceId source, i32 priority = i32()) {
        return m_uniforms.AttachNode(node, source, priority);
    }
    auto Resolve(UniformSourceId source) const -> Option<ref<dyn<UniformSource>>> {
        return m_uniforms.Resolve(source);
    }
    auto GlobalSources() const -> slice<UniformSourceAttachment> {
        return m_uniforms.GlobalSources();
    }
    auto NodeSources(SceneNodeId node) const -> slice<UniformSourceAttachment> {
        return m_uniforms.NodeSources(node);
    }
    bool RegisterUniformBlock(UniformBlockDefinition definition) {
        return m_uniforms.RegisterBlock(rstd::move(definition));
    }
    auto ResolveUniformBlock(u64 identity) const -> Option<ref<UniformBlockDefinition>> {
        return m_uniforms.ResolveBlock(identity);
    }
    SceneRuntime&       Runtime() noexcept { return m_runtime; }
    const SceneRuntime& Runtime() const noexcept { return m_runtime; }
    auto                TextureFrame(SceneDrawItemId draw, usize texture_index) const
        -> Option<SceneTextureFrameView> {
        return m_texture_animations.Frame(draw, texture_index);
    }

    void SetSceneId(String scene_id) { m_scene_id = rstd::move(scene_id); }
    auto SceneId() const -> ref<str> { return m_scene_id.as_str(); }

    auto DefaultEffectMesh() const -> ref<SceneMesh> {
        return ref<SceneMesh>::from_raw_parts(&m_default_effect_mesh);
    }
    auto DefaultEffectMeshMut() -> mut_ref<SceneMesh> {
        return mut_ref<SceneMesh>::from_raw_parts(&m_default_effect_mesh);
    }

    auto AudioAverage(usize index) const -> f32 { return m_audio_average->Load(index); }
    auto AudioAverageHandle() const -> Arc<SceneAudioAverage> { return m_audio_average.clone(); }

    void SetOrtho(array<i32, 2> extent) { m_ortho = rstd::move(extent); }
    auto Ortho() const -> array<i32, 2> { return m_ortho; }
    void SetViewportScale(f32 scale) {
        m_viewport_scale = scale.is_finite() && scale > f32() ? scale : f32(1.0f);
    }
    auto ViewportScale() const -> f32 { return m_viewport_scale; }
    auto OrthographicProjectionExtent() const -> array<double, 2> {
        const double scale = static_cast<double>(m_viewport_scale.to_primitive());
        return { static_cast<double>(m_ortho[usize()].to_primitive()) / scale,
                 static_cast<double>(m_ortho[usize(1)].to_primitive()) / scale };
    }
    void SetPointerPosition(array<float, 2> position) { m_pointer_position = rstd::move(position); }
    auto PointerPosition() const -> array<float, 2> { return m_pointer_position; }
    void SetClearColor(array<float, 3> color) { m_clear_color = rstd::move(color); }
    auto ClearColor() const -> array<float, 3> { return m_clear_color.clone(); }
    void SetClearColorUserKey(String key) { m_clear_color_user_key = rstd::move(key); }
    auto ClearColorUserKey() const -> ref<str> { return m_clear_color_user_key.as_str(); }

    void PassFrameTime(double delta) { m_runtime.Advance(f64(delta)); }
    void TickNodeFieldAnimations();
    void RegisterTransformUpdater(Box<dyn<FnMut<void(f64)>>> updater);
    void TickTransformUpdaters();

    void RegisterLinkedCamera(String source, String linked);
    void UpdateLinkedCamera(ref<str> name);

    void        TickCameraPaths();
    void        TickMaterialShaderAnimations();
    void        CaptureCameraPathViewports();
    std::string EnsureLinkRenderTarget(WallpaperLayerId source_layer, const SceneNode& source_node);
    bool        EnsureTextureDescriptor(std::string_view key);
    bool        SetMaterialShaderValue(SceneMaterial& material, ref<str> uniform_name,
                                       const ShaderValue& value);
    bool        SetMaterialShaderValueByKey(SceneMaterial& material, ref<str> material_key,
                                            const ShaderValue& value);
    SceneMaterialTextureSlotMutation SetMaterialTextureSlot(SceneMaterial& material, u32 slot,
                                                            std::string_view texture);
    bool SetMaterialLayerPreviousSource(SceneMaterial& material, u32 slot, SceneNodeId layer,
                                        ref<str> composite_target);
    void ResolveMaterialTextureSources(SceneMaterial& material);
    SceneMaterialShaderVariantMutation
         SetMaterialShaderVariant(SceneMaterial& material, SceneShaderVariantMutation mutation);
    void MarkLayerStaticElidable(WallpaperLayerId id);
    void MarkLayerVisibilityElidable(WallpaperLayerId id);
    bool IsLayerElidable(WallpaperLayerId id) const {
        return m_elidable_layer_ids.contains(id.value);
    }
    bool IsLayerStaticElidable(WallpaperLayerId id) const {
        return m_static_elidable_layer_ids.contains(id.value);
    }
    bool IsLayerVisibilityElidable(WallpaperLayerId id) const {
        return m_visibility_elidable_layer_ids.contains(id.value);
    }
    void       RegisterLayerLinkSource(WallpaperLayerId id, SceneNode& node);
    void       RegisterLayerLinkSource(WallpaperLayerId id, SceneNode& node, array<i32, 2> extent);
    SceneNode* RegisteredLayerLinkSource(WallpaperLayerId id) const;
    Option<SceneNodeId>      RegisteredLayerLinkSourceId(WallpaperLayerId id) const;
    Option<WallpaperLayerId> ResolveLayerLinkSource(const SceneNode& node) const;
    void                     RegisterRenderGroup(WallpaperLayerId id, String camera) {
        (void)m_render_group_cameras.insert(id.value, rstd::move(camera));
    }
    Option<ref<str>> RenderGroupCamera(WallpaperLayerId id) const {
        auto camera = m_render_group_cameras.get(id.value);
        if (camera.is_none()) return None();
        return Some((**camera).as_str());
    }
    bool                             SetNodeVisible(SceneNode& node, bool visible);
    bool                             ResizeRenderTarget(ref<str> name, i32 width, i32 height);
    Vec<SceneMeshDirtyEvent>         ConsumePreparedMeshDirtyEvents();
    Vec<SceneMaterialDirtyEvent>     ConsumePreparedMaterialDirtyEvents();
    Vec<SceneRenderTargetDirtyEvent> ConsumePreparedRenderTargetDirtyEvents();
    void                             ClearUserPropertyDiagnostics(ref<str> key);
    void AddUserPropertyDiagnostic(SceneUserPropertyDiagnostic diagnostic);
    slice<SceneUserPropertyDiagnostic> UserPropertyDiagnostics() const {
        return m_user_property_diagnostics.as_slice();
    }

    void                      RebuildResourceIndex();
    SceneResourceIndex&       ResourceIndex() { return m_resource_index; }
    const SceneResourceIndex& ResourceIndex() const { return m_resource_index; }
    u32                       ResourceGeneration() const { return m_resource_generation; }

    String NodeResourceKey(SceneNodeId node, ref<str> role) const;
    String EffectResourceKey(SceneEffectId effect, ref<str> local_name) const;

private:
    struct ImageEffectRecord {
        SceneNodeId                       owner;
        SceneNodeLayer*                   layer { nullptr };
        std::shared_ptr<SceneImageEffect> effect;
    };

    SceneUniformRegistry          m_uniforms;
    SceneTextureAnimationRegistry m_texture_animations;
    SceneRuntime                  m_runtime;
    String                        m_scene_id { String::make("unknown_id"_str) };
    SceneMesh                     m_default_effect_mesh;
    array<i32, 2>                 m_ortho { i32(1920), i32(1080) };
    f32                           m_viewport_scale { 1.0f };
    array<float, 2>               m_pointer_position { 0.5f, 0.5f };
    array<float, 3>               m_clear_color { 1.0f, 1.0f, 1.0f };
    String                        m_clear_color_user_key;
    Box<SceneNode>                m_scene_graph;
    Arc<AudioResponseDemand>      m_audio_response_demand { Arc<AudioResponseDemand>::make() };
    Arc<SceneAudioAverage>        m_audio_average { Arc<SceneAudioAverage>::make() };
    Vec<Box<SceneLight>>          m_lights;
    Vec<Box<ScenePostProcess>>    m_post_processes;
    Vec<SceneShadowDefinition>    m_shadow_definitions;
    HashMap<String, Vec<Box<dyn<FnMut<void(ref<str>)>>>>>        m_text_user_index;
    HashMap<String, Vec<Box<dyn<FnMut<void(ref<Json>)>>>>>       m_user_property_index;
    Vec<Box<dyn<FnMut<void(f64)>>>>                              m_transform_updaters;
    HashMap<String, Vec<ShaderUserBinding>>                      m_shader_user_index;
    HashMap<String, Vec<ShaderComboUserBinding>>                 m_shader_combo_user_index;
    HashMap<String, Vec<MaterialTextureUserBinding>>             m_material_texture_user_index;
    HashMap<String, Vec<ImagePropertyBinding>>                   m_image_color_user_index;
    HashMap<String, Vec<ImagePropertyBinding>>                   m_image_alpha_user_index;
    HashMap<String, Vec<Arc<dyn<SceneParticleOverrideControl>>>> m_particle_override_user_index;
    HashMap<String, Vec<Arc<dyn<SceneSoundControl>>>>            m_sound_volume_user_index;
    Option<Box<dyn<IImageParser>>>                               m_image_parser;
    Vec<Box<dyn<Any>>>                                           m_extensions;
    HashMap<String, Arc<Image>>                                  m_runtime_images;
    HashMap<String, SceneTexture>                                m_textures;
    HashMap<String, u64>                                         m_texture_content_revisions;
    HashMap<String, Arc<VideoPlaybackState>>                     m_video_controls;
    Vec<String>                                                  m_texture_names;
    HashMap<String, SceneRenderTarget>                           m_render_targets;
    Vec<String>                                                  m_render_target_names;
    HashMap<String, Arc<SceneCamera>>                            m_cameras;
    Vec<String>                                                  m_camera_names;
    Option<String>                                               m_active_camera;
    Vec<Arc<SceneCameraPath>>                                    m_camera_paths;
    HashMap<String, Vec<Arc<SceneCameraPath>>>                   m_camera_path_user_index;

    void RebuildElidableLayerIds();

    u32                                          m_resource_generation { 0 };
    u32                                          m_next_node_index { 0 };
    u32                                          m_next_effect_index { 0 };
    HashMap<rstd::uint64_t, ImageEffectRecord>   m_image_effects;
    HashMap<i32, SceneNodeId>                    m_wallpaper_node_ids;
    SceneResourceIndex                           m_resource_index;
    bool                                         m_render_graph_dirty { false };
    bool                                         m_planar_reflection_enabled { false };
    HashMap<i32, String>                         m_render_group_cameras;
    HashMap<String, Vec<String>>                 m_linked_cameras;
    HashMap<i32, SceneNodeId>                    m_layer_link_source_ids;
    HashMap<i32, SceneNode*>                     m_layer_link_source_nodes;
    HashMap<i32, array<i32, 2>>                  m_layer_link_source_extents;
    HashMap<rstd::uint64_t, WallpaperLayerId>    m_node_link_sources;
    Vec<SceneUserPropertyDiagnostic>             m_user_property_diagnostics;
    HashMap<String, SceneRenderTargetDirtyEvent> m_render_target_dirty_events;

    // User-hidden layers and static identity layers share the same graph-elision
    // result while retaining their independent reasons.
    HashSet<i32>            m_elidable_layer_ids;
    HashSet<i32>            m_render_graph_elidable_layer_ids;
    HashSet<i32>            m_static_elidable_layer_ids;
    HashSet<i32>            m_visibility_elidable_layer_ids;
    HashSet<rstd::uint64_t> m_hidden_scene_node_ids;
    HashSet<rstd::uint64_t> m_render_graph_hidden_scene_node_ids;
};

} // namespace owe

export namespace rstd
{

template<>
struct Impl<fmt::Display, owe::ImageParseError> : ImplBase<owe::ImageParseError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return formatter.write_fmt(fmt::Arguments::make("{}", this->self().message));
    }
};

template<>
struct Impl<fmt::Debug, owe::ImageParseError> : ImplBase<owe::ImageParseError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return formatter.write_fmt(fmt::Arguments::make("ImageParseError(kind={}, message={})",
                                                        static_cast<int>(this->self().kind),
                                                        this->self().message));
    }
};

template<>
struct Impl<error::Error, owe::ImageParseError>
    : DefaultInImpl<error::Error, owe::ImageParseError> {};

} // namespace rstd

static_assert(rstd::Impled<owe::ImageParseError, rstd::error::Error>);
