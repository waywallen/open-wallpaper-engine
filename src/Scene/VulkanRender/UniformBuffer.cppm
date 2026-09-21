export module wescene.vulkan_render:uniform_buffer;
export import vrento.uniform_buffer;
export import vrento.uniform_binding;
import rstd;
import rstd.cppstd;
import wescene.resource;
import wescene.scene;
import wescene.types;

using namespace rstd::prelude;

export namespace owe::vulkan
{

using vrento::CompileUniformBufferLayout;
using vrento::SerializeUniformValue;
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

class ProgramUniformFrameContext {
public:
    ProgramUniformFrameContext(const SceneFrame& frame, rstd::array<float, 2> viewport,
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

class UniformBufferBinding {
public:
    UniformBufferBinding(SceneDrawItemId, resource::BufferUseHandle, UniformBufferLayout,
                         Vec<BoundUniformSource>, ShaderValues, ref<SceneMaterial>,
                         Vec<PreparedUniformTextureMetadata>, SceneRenderViewKind,
                         ShaderMatrixConvention, ShaderMatrixAbi);
    auto Update(ref<dyn<UniformBufferFrameContext>>,
                mut_ref<dyn<resource::BufferContentWriter>>) const
        -> Result<empty, UniformBufferUpdateError>;
    auto Buffer() const -> resource::BufferUseHandle { return m_binding.Buffer(); }

private:
    SceneDrawItemId                     m_draw_item;
    mutable vrento::UniformBinding      m_binding;
    ShaderValues                        m_defaults;
    ref<SceneMaterial>                  m_material;
    Vec<PreparedUniformTextureMetadata> m_textures;
    SceneRenderViewKind                 m_render_view;
    mutable Option<u64>                 m_parameter_version;
};

class SharedUniformBufferBinding {
public:
    SharedUniformBufferBinding(resource::BufferUseHandle buffer, UniformBufferLayout layout,
                               Vec<BoundUniformSource> sources, ShaderMatrixConvention convention,
                               ShaderMatrixAbi abi)
        : m_binding(buffer, rstd::move(layout), rstd::move(sources), convention, abi) {}
    auto Update(ref<dyn<UniformBufferFrameContext>>,
                mut_ref<dyn<resource::BufferContentWriter>>) const
        -> Result<empty, UniformBufferUpdateError>;
    auto Buffer() const -> resource::BufferUseHandle { return m_binding.Buffer(); }

private:
    mutable vrento::UniformBinding m_binding;
};

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
