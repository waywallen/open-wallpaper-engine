export module wescene.vulkan_render:uniform_buffer;
export import vrento;
import rstd;
import wescene.resource;
import wescene.resource_registry;
import wescene.scene;
import wescene.types;

using namespace rstd::prelude;
using rstd::sync::Arc;

export namespace owe::vulkan
{

using vrento::CompileUniformBufferLayout;
using vrento::SerializeUniformValue;
using vrento::UniformBindingPlan;
using vrento::UniformBufferLayout;
using vrento::UniformBufferUpdateError;
using vrento::UniformSlot;

struct UniformBufferFrameContext {
    using Trait                  = UniformBufferFrameContext;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = UniformBufferFrameContext;

        auto Frame() const -> ref<SceneFrame> { return rstd::trait_call<0>(this); }
        auto Viewport() const -> rstd::array<float, 2> { return rstd::trait_call<1>(this); }
        auto TextureFrame(SceneDrawItemId draw, usize texture_index) const
            -> Option<SceneTextureFrameView> {
            return rstd::trait_call<2>(this, draw, texture_index);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Frame, &T::Viewport, &T::TextureFrame>;
};

class SceneRenderUniformFrameContext {
public:
    SceneRenderUniformFrameContext(const SceneFrame& frame, rstd::array<float, 2> viewport,
                                   ref<dyn<SceneTextureAnimationView>> textures)
        : m_frame(ref<SceneFrame>::from_raw_parts(rstd::addressof(frame))),
          m_viewport(viewport),
          m_textures(textures) {}

    auto Frame() const -> ref<SceneFrame> { return m_frame; }
    auto Viewport() const -> rstd::array<float, 2> { return m_viewport; }
    auto TextureFrame(SceneDrawItemId draw, usize texture_index) const
        -> Option<SceneTextureFrameView>;

private:
    ref<SceneFrame>                             m_frame;
    rstd::array<float, 2>                       m_viewport;
    mutable ref<dyn<SceneTextureAnimationView>> m_textures;
};

struct UniformBufferUpdate {
    using Trait                  = UniformBufferUpdate;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = UniformBufferUpdate;

        auto Update(ref<dyn<UniformBufferFrameContext>>         context,
                    mut_ref<dyn<resource::BufferContentWriter>> buffers) const
            -> Result<empty, UniformBufferUpdateError> {
            return rstd::trait_call<0>(this, context, buffers);
        }
        auto Buffer() const -> resource::BufferUseHandle { return rstd::trait_call<1>(this); }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Update, &T::Buffer>;
};

class UniformBufferUpdates {
public:
    UniformBufferUpdates(u64 generation, usize count): m_generation(generation) {
        m_claimed.resize(count, u8());
    }
    auto claim(resource::BufferUseHandle use) -> Option<bool> {
        if (use.generation != m_generation || use.index >= rstd::as_cast<u64>(m_claimed.len()))
            return None();
        auto slot = m_claimed[rstd::as_cast<usize>(use.index)];
        if (*slot != u8()) return Some(false);
        *slot = u8(1);
        return Some(true);
    }
    void add(Box<dyn<UniformBufferUpdate>> binding) { m_ordered.push(rstd::move(binding)); }
    auto take() -> Vec<Box<dyn<UniformBufferUpdate>>> { return rstd::move(m_ordered); }

private:
    u64                                m_generation;
    Vec<u8>                            m_claimed;
    Vec<Box<dyn<UniformBufferUpdate>>> m_ordered;
};

using vrento::BoundUniformSource;

struct PreparedUniformTextureMetadata {
    bool                  available { false };
    rstd::array<float, 2> source_extent { 0.0f, 0.0f };
    rstd::array<float, 2> sample_extent { 0.0f, 0.0f };
    bool                  has_mipmap { false };
    float                 mipmap_level { 0.0f };
    u64                   revision { 1 };
};

struct UniformPrepareDraw {
    SceneDrawItemId    draw_item;
    SceneNodeId        node_id;
    ref<SceneNode>     node;
    ref<SceneMaterial> material;
};

struct UniformBindingPrepareContext {
    using Trait                  = UniformBindingPrepareContext;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = UniformBindingPrepareContext;

        auto ResolveDraw(SceneDrawItemId draw) const -> Option<UniformPrepareDraw> {
            return rstd::trait_call<0>(this, draw);
        }
        auto DrawItemFor(ref<SceneNode> node, u32 submesh_index) const -> Option<SceneDrawItemId> {
            return rstd::trait_call<1>(this, node, submesh_index);
        }
        auto GlobalSources() const -> slice<UniformSourceAttachment> {
            return rstd::trait_call<2>(this);
        }
        auto NodeSources(SceneNodeId node) const -> slice<UniformSourceAttachment> {
            return rstd::trait_call<3>(this, node);
        }
        auto ResolveSource(UniformSourceId source) const -> Option<vrento::UniformSourceOwner> {
            return rstd::trait_call<4>(this, source);
        }
        auto ResolveBlock(u64 identity) const -> Option<ref<UniformBlockDefinition>> {
            return rstd::trait_call<5>(this, identity);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::ResolveDraw, &T::DrawItemFor, &T::GlobalSources, &T::NodeSources,
                             &T::ResolveSource, &T::ResolveBlock>;
};

class SceneUniformBindingPrepareContext {
public:
    explicit SceneUniformBindingPrepareContext(Scene& scene)
        : m_scene(ref<Scene>::from_raw_parts(rstd::addressof(scene))) {}

    auto ResolveDraw(SceneDrawItemId) const -> Option<UniformPrepareDraw>;
    auto DrawItemFor(ref<SceneNode>, u32 submesh_index) const -> Option<SceneDrawItemId>;
    auto GlobalSources() const -> slice<UniformSourceAttachment>;
    auto NodeSources(SceneNodeId) const -> slice<UniformSourceAttachment>;
    auto ResolveSource(UniformSourceId) const -> Option<vrento::UniformSourceOwner>;
    auto ResolveBlock(u64 identity) const -> Option<ref<UniformBlockDefinition>>;

private:
    mutable ref<Scene> m_scene;
};

struct UniformPreparationStats {
    u64 hits {}, misses {};
};

class SceneUniformPreparationCache {
public:
    void begin(u32 scene_generation);
    auto prepare(ref<dyn<UniformBindingPrepareContext>>, SceneNodeId,
                 const resource_registry::PreparedShader&, usize block_index)
        -> Result<Arc<UniformBindingPlan>, UniformBufferUpdateError>;
    void prune();
    void clear();
    auto stats() const -> UniformPreparationStats { return m_stats; }

private:
    struct Entry {
        Arc<resource_registry::ShaderPhysical> shader;
        usize                                  block_index;
        Vec<UniformSourceAttachment>           global, local;
        Arc<UniformBindingPlan>                plan;
        u64                                    used;
    };
    rstd::collections::HashMap<u32, Vec<Entry>> m_nodes;
    u32                                         m_scene_generation {};
    u64                                         m_use {};
    UniformPreparationStats                     m_stats;
};

class UniformBufferBinding {
public:
    UniformBufferBinding(SceneDrawItemId, resource::BufferUseHandle, Arc<UniformBindingPlan>,
                         SceneShaderDefaults, ref<SceneMaterial>,
                         Vec<PreparedUniformTextureMetadata>, SceneRenderViewKind);
    auto Update(ref<dyn<UniformBufferFrameContext>>,
                mut_ref<dyn<resource::BufferContentWriter>>) const
        -> Result<empty, UniformBufferUpdateError>;
    auto Buffer() const -> resource::BufferUseHandle { return m_binding.Buffer(); }

private:
    SceneDrawItemId                     m_draw_item;
    mutable vrento::UniformBinding      m_binding;
    SceneShaderDefaults                 m_defaults;
    ref<SceneMaterial>                  m_material;
    Vec<PreparedUniformTextureMetadata> m_textures;
    SceneRenderViewKind                 m_render_view;
    mutable Option<u64>                 m_parameter_version;
};

class SharedUniformBufferBinding {
public:
    SharedUniformBufferBinding(resource::BufferUseHandle buffer, Arc<UniformBindingPlan> plan)
        : m_binding(buffer, rstd::move(plan)) {}
    auto Update(ref<dyn<UniformBufferFrameContext>>,
                mut_ref<dyn<resource::BufferContentWriter>>) const
        -> Result<empty, UniformBufferUpdateError>;
    auto Buffer() const -> resource::BufferUseHandle { return m_binding.Buffer(); }

private:
    mutable vrento::UniformBinding m_binding;
};

auto BindUniformBuffer(const UniformPrepareDraw&, resource::BufferUseHandle,
                       Arc<UniformBindingPlan>, Vec<PreparedUniformTextureMetadata>,
                       SceneRenderViewKind, Option<ref<SceneMaterial>> material_override)
    -> Result<Box<dyn<UniformBufferUpdate>>, UniformBufferUpdateError>;

auto MakeUniformBufferBinding(
    ref<dyn<UniformBindingPrepareContext>>, SceneDrawItemId, resource::BufferUseHandle,
    const resource::ShaderArtifactUniformBlock&, Vec<PreparedUniformTextureMetadata> textures = {},
    SceneRenderViewKind        render_view       = SceneRenderViewKind::Primary,
    ShaderMatrixConvention     matrix_convention = ShaderMatrixConvention::ColumnVector,
    ShaderMatrixAbi            matrix_abi        = ShaderMatrixAbi::NativeSpirv,
    Option<ref<SceneMaterial>> material_override = None<ref<SceneMaterial>>())
    -> Result<Box<dyn<UniformBufferUpdate>>, UniformBufferUpdateError>;

auto MakeSharedUniformBufferBinding(ref<dyn<UniformBindingPrepareContext>>,
                                    resource::BufferUseHandle,
                                    const resource::ShaderArtifactUniformBlock&,
                                    ShaderMatrixConvention, ShaderMatrixAbi)
    -> Result<Box<dyn<UniformBufferUpdate>>, UniformBufferUpdateError>;

} // namespace owe::vulkan
