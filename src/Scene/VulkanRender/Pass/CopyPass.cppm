module;

export module wescene.vulkan_render:copy_pass;
import wescene.vulkan;
import wescene.scene;

import :vulkan_pass;
import :resource;

export namespace owe::vulkan
{

class CopyPass : public VulkanPass {
public:
    struct Desc {
        String                             src;
        String                             dst;
        Option<resource::TextureUseHandle> src_use;
        Option<resource::TextureUseHandle> dst_use;
        Option<TextureRequest>             src_request;
        Option<TextureRequest>             dst_request;
        bool                               dst_matches_src { false };

        resource_registry::PreparedBarrierBatch before_barriers;
        resource_registry::PreparedBarrierBatch after_barriers;
    };

    CopyPass(Desc&&);
    virtual ~CopyPass();

    PassInvalidationFlags finalizeResourceRequests(Scene&) override;
    PassResourceUses      resourceUses() const override;
    bool prepareResourceStates(mut_ref<dyn<resource_registry::TextureStatePreparer>>) override;
    Vec<PassTextureRequestDiagnostic> textureRequestDiagnostics() const override;

    void prepare(Scene&, const Device&, PassPrepareContext&) override;
    void record(PassRecordContext&) override;
    void destory(const Device&) override;

private:
    Desc m_desc;
};

} // namespace owe::vulkan
