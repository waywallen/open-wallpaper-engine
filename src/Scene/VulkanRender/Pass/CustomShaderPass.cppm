module;

export module wescene.vulkan_render:custom_shader_pass;
import wescene.core;
import rstd;
import wescene.vulkan;
import wescene.scene;

import :vulkan_pass;
import :resource;
import :buffer_resolver;
import :uniform_buffer;

using namespace rstd::prelude;
using rstd::sync::Arc;

export namespace owe::vulkan
{

class CustomShaderPass : public VulkanPass {
public:
    struct UniformBlockUse {
        usize                     block_index {};
        resource::BufferUseHandle use;
    };
    struct DescriptorSetUse {
        u32                               set {};
        resource::DescriptorBindingHandle binding;
    };

    struct Desc {
        // in
        Option<mut_ref<SceneNode>> node;
        SceneDrawItemId            draw_item;
        RenderItemId               render_item;
        SceneRenderViewKind        render_view { SceneRenderViewKind::Primary };
        Option<Arc<SceneMaterial>> material_override;
        bool                       depth_only { false };
        u32                        instance_count { 1 };
        Vec<VkViewport>            viewports;
        Vec<VkRect2D>              scissors;
        usize                      graph_pass_index { 0 };
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

        // -----prepared
        // vulkan texs
        Vec<rstd::int32_t>                      vk_tex_binding;
        Vec<u32>                                vk_tex_set;
        Vec<rstd::ptrdiff_t>                    descriptor_image_slots;
        resource_registry::PreparedBarrierBatch sampled_barriers;
        VkExtent2D                              output_extent {};
        VkSampleCountFlagBits                   samples { VK_SAMPLE_COUNT_1_BIT };
        bool                                    has_depth_attachment { false };

        // bufs
        DrawBufferRefs draw_buffers;

        // pipeline
        VkClearValue clear_value;
        // Scene clear color snapshot; None keeps the pass-owned clear value.
        Option<array<float, 3>>     clear_value_src;
        bool                        blending { false };
        bool                        clear_output { false };
        bool                        transparent_clear { false };
        bool                        clear_depth { false };
        float                       depth_clear_value { 1.0f };
        bool                        preserve_output { false };
        VkAttachmentLoadOp          color_load_op { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
        VkAttachmentLoadOp          depth_load_op { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
        Vec<DescriptorSetUse>       descriptor_bindings;
        Option<PipelineCacheKey>    pipeline_cache_key;
        Option<RenderPassCacheKey>  render_pass_cache_key;
        Option<FramebufferCacheKey> framebuffer_cache_key;
        bool                        pipeline_cache_hit { false };
        u64                         pipeline_cache_observed_count { 0 };
        bool                        render_pass_cache_hit { false };
        u64                         render_pass_cache_observed_count { 0 };
        bool                        framebuffer_cache_hit { false };
        u64                         framebuffer_cache_observed_count { 0 };
    };

    CustomShaderPass(Desc&&);
    virtual ~CustomShaderPass();

    PassInvalidationFlags finalizeResourceRequests(Scene&) override;
    void                  declareResources(ResourceDeclarationContext&) override;
    PassResourceUses      resourceUses() const override;
    auto                  pipelineLayoutRequirement(const PreparedPassResources&) const
        -> Result<Option<PipelineLayoutRequirement>, resource::ResourceError> override;
    auto globalDescriptorBufferUses(const PreparedPassResources&) const
        -> Result<Vec<GlobalDescriptorBufferUse>, resource::ResourceError> override;
    auto createUniformBufferUpdates(ref<dyn<UniformBindingPrepareContext>>,
                                    const PreparedPassResources&)
        -> Result<Vec<Box<dyn<UniformBufferUpdate>>>, UniformBufferUpdateError> override;
    bool prepareResourceStates(mut_ref<dyn<resource_registry::TextureStatePreparer>>) override;
    Option<RenderItemId>              renderItemId() const override;
    Option<PipelineCacheKey>          pipelineCacheKey() const override;
    bool                              pipelineCacheHit() const override;
    u64                               pipelineCacheObservedCount() const override;
    Option<RenderPassCacheKey>        renderPassCacheKey() const override;
    bool                              renderPassCacheHit() const override;
    u64                               renderPassCacheObservedCount() const override;
    Option<FramebufferCacheKey>       framebufferCacheKey() const override;
    bool                              framebufferCacheHit() const override;
    u64                               framebufferCacheObservedCount() const override;
    Vec<PassTextureRequestDiagnostic> textureRequestDiagnostics() const override;
    MaterialTextureBindingRefresh
         refreshMaterialTextureBindings(const RenderSceneSnapshot&) override;
    bool setTextureBinding(u32 index, TextureBindingRequest binding) override;

    void prepare(Scene&, const Device&, PassPrepareContext&) override;
    bool update(PassUpdateContext&) override;
    void completeUpdate() override;
    void record(PassRecordContext&) override;
    void destory(const Device&) override;
    bool supportsRenderScope() const override;
    bool canJoinRenderScopeAfter(const VulkanPass& previous) const override;
    void prepareRenderScopeDraw(PassRecordContext&) override;
    void recordSampledImageBarriers(PassRecordContext&);
    void beginRenderScope(PassRecordContext&) override;
    void recordRenderScopeDraw(PassRecordContext&) override;
    void endRenderScope(PassRecordContext&) override;

private:
    Desc m_desc;
};

} // namespace owe::vulkan
