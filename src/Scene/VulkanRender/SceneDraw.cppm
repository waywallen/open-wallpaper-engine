module;

export module wescene.vulkan_render:scene_draw;
import wescene.core;
import rstd;
import wescene.vulkan;
import wescene.scene;
import vrento;

import :scene_render_resources;
import :scene_render_textures;
import :resource;
import :prepare_error;
import :buffer_resolver;
import :uniform_buffer;
import :scene_draw_preparation;

using namespace rstd::prelude;
using rstd::sync::Arc;

export namespace owe::vulkan
{

struct ShaderDrawInput : vrento::vulkan::GraphicsFrameData {
    Vec<resource::TextureUseHandle> texture_uses;
    Vec<vrento::texture::Stages>    texture_stages;
};

class SceneDraw {
public:
    struct UniformBlockUse {
        usize                     block_index {};
        resource::BufferUseHandle use;
    };
    struct DescriptorSetUse {
        u32                               set {};
        resource::DescriptorBindingHandle binding;
    };

    struct UpdateSource {
        Option<mut_ref<SceneNode>> node;
        Option<Arc<SceneMaterial>> material_override;
    };
    struct Desc {
        SceneDrawItemId     draw_item;
        RenderItemId        render_item;
        SceneRenderViewKind render_view { SceneRenderViewKind::Primary };
        bool                depth_only { false };
        u32                 instance_count { 1 };
        Vec<VkViewport>     viewports;
        Vec<VkRect2D>       scissors;
        usize               graph_pass_index { 0 };
        // Which submesh of node->Mesh() this pass renders. SceneToRenderGraph
        // emits one pass per (node, submesh).
        u32                                    submesh_index { 0 };
        Vec<TextureBindingRequest>             texture_bindings;
        String                                 output;
        Option<resource::TextureUseHandle>     output_use;
        Option<TextureRequest>                 output_request;
        Option<resource::TextureUseHandle>     output_msaa_use;
        Option<TextureRequest>                 output_msaa_request;
        Option<resource::TextureUseHandle>     depth_use;
        Option<TextureRequest>                 depth_request;
        Option<resource::ShaderUseHandle>      shader_use;
        Vec<resource::BufferUseHandle>         buffer_uses;
        Vec<UniformBlockUse>                   uniform_block_uses;
        Option<resource::PipelineUseHandle>    pipeline_use;
        Option<resource::RenderPassUseHandle>  render_pass_use;
        Option<resource::FramebufferUseHandle> framebuffer_use;

        Vec<rstd::int32_t>           vk_tex_binding;
        Vec<u32>                     vk_tex_set;
        Vec<vrento::texture::Stages> texture_stages;
        Vec<rstd::ptrdiff_t>         descriptor_image_slots;
        VkExtent2D                   output_extent {};
        VkSampleCountFlagBits        samples { VK_SAMPLE_COUNT_1_BIT };
        bool                         has_depth_attachment { false };

        DrawBufferRefs draw_buffers;

        VkClearValue clear_value;
        // Scene clear color snapshot; None keeps the pass-owned clear value.
        Option<array<float, 3>> clear_value_src;
        bool                    blending { false };
        bool                    clear_output { false };
        bool                    transparent_clear { false };
        bool                    clear_depth { false };
        float                   depth_clear_value { 1.0f };
        bool                    preserve_output { false };
        VkAttachmentLoadOp      color_load_op { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
        VkAttachmentLoadOp      depth_load_op { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
        Vec<DescriptorSetUse>   descriptor_bindings;
    };

    struct Input {
        Desc         description;
        UpdateSource updates;
    };
    SceneDraw(Desc&&, UpdateSource = {});
    SceneDraw(Input&&);
    ~SceneDraw();

    PassInvalidationFlags finalizeResourceRequests(Scene&);
    auto declareResources(ResourceDeclarationContext&) -> Option<vrento::vulkan::GraphicsDrawDesc>;
    auto textureAccesses() const -> Vec<PassTextureAccess>;
    PassResourceUses resourceUses() const;
    auto             globalDescriptorBufferUses(const PreparedPassResources&) const
        -> Result<Vec<GlobalDescriptorBufferUse>, resource::ResourceError>;
    auto createUniformBufferUpdates(ref<dyn<UniformBindingPrepareContext>>,
                                    const PreparedPassResources&)
        -> Result<Vec<Box<dyn<UniformBufferUpdate>>>, UniformBufferUpdateError>;
    Option<RenderItemId>              renderItemId() const;
    Vec<PassTextureRequestDiagnostic> textureRequestDiagnostics() const;
    MaterialTextureBindingRefresh     refreshMaterialTextureBindings(const RenderSceneSnapshot&);
    bool                              setTextureBinding(u32 index, TextureBindingRequest binding);

    auto describePass(vrento::fg::FrameGraphBuilder&, const SceneRenderTextures&,
                      vrento::fg::PassId) const
        -> Result<vrento::vulkan::GraphicsPassResources, vrento::fg::Error>;
    auto resolveResources(const Device&, DrawResourceContext&) -> Result<empty, DrawPrepareError>;
    bool update(DrawUpdateContext&);
    void acknowledgeGeometry();
    auto frameData(const DrawBindingContext&) const -> Option<Arc<ShaderDrawInput>>;
    void clearResources();
    auto parameters() const -> const Desc& { return m_desc; }
    void refreshSnapshot() { captureGeometry(); }

private:
    struct MaterialSnapshot {
        Option<Arc<SceneShader>>         shader;
        vrento::MaterialPipelineSnapshot pipeline;

        auto Pipeline() const -> const vrento::MaterialPipelineDesc& { return pipeline.value; }
    };
    bool bindings_ready_ {};
    bool samplesTexture(usize) const;
    auto describeGraphics(const resource::ShaderArtifact&)
        -> Option<vrento::vulkan::GraphicsDrawDesc>;
    Vec<vrento::vulkan::GraphicsDrawRange> draws_;
    bool                                   visible_ { true };
    void                                   captureGeometry();
    Option<MaterialSnapshot>               material_;
    Option<vrento::GeometrySnapshot>       geometry_;
    Vec<DrawVertexLayout>                  vertex_layouts_;
    Option<Arc<SceneDrawPreparation>>      preparation_;
    MeshPrimitive                          primitive_ { MeshPrimitive::TRIANGLE };
    bool                                   default_alpha_ {}, force_clear_ {}, output_depth_ {};
    Option<bool>                           output_alpha_;
    array<float, 3>                        clear_color_ {};
    Vec<Option<array<float, 2>>>           texture_authored_extents_;
    Desc                                   m_desc;
    UpdateSource                           updates_;
};

} // namespace owe::vulkan
