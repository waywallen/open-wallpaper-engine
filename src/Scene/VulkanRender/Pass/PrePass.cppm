module;

export module wescene.vulkan_render:pre_pass;
import wescene.spec_names;
import wescene.vulkan;
import wescene.scene;

import :vulkan_pass;
import :resource;

export namespace owe::vulkan
{

class PrePass : public VulkanPass {
public:
    struct Desc {
        // in
        String                                  result { rstd::into(SpecTex_Default) };
        Option<resource::TextureUseHandle>      result_use;
        Option<resource::TextureUseHandle>      result_msaa_use;
        Option<TextureRequest>                  result_request;
        Option<TextureRequest>                  result_msaa_request;
        resource_registry::PreparedBarrierBatch before_clear;
        resource_registry::PreparedBarrierBatch after_clear;

        // prepared
        Option<resource::RenderPassUseHandle>  render_pass_use;
        Option<resource::FramebufferUseHandle> framebuffer_use;
        VkSampleCountFlagBits                  samples { VK_SAMPLE_COUNT_1_BIT };
        VkClearValue                           clear_value;
    };

    PrePass(Desc&&);
    virtual ~PrePass();

    bool setResultRequest(Option<TextureRequest>, Option<TextureRequest> msaa_request = None());
    void resetResourceUses();
    void declareResources(ResourceDeclarationContext&) override;
    PassResourceUses resourceUses() const override;
    bool prepareResourceStates(mut_ref<dyn<resource_registry::TextureStatePreparer>>) override;
    Vec<PassTextureRequestDiagnostic> textureRequestDiagnostics() const override;

    void prepare(Scene&, const Device&, PassPrepareContext&) override;
    void record(PassRecordContext&) override;
    void destory(const Device&) override;

private:
    Desc m_desc;
};

} // namespace owe::vulkan
