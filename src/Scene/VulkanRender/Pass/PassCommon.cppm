module;

export module wescene.vulkan_render:pass_common;
import rstd;
import wescene.types;
import wescene.vulkan;
import vrento.material;

using namespace rstd::prelude;

export namespace owe::vulkan
{

inline void SetBlend(BlendMode bm, VkPipelineColorBlendAttachmentState& state) {
    state.blendEnable  = true;
    state.colorBlendOp = VK_BLEND_OP_ADD;
    state.alphaBlendOp = VK_BLEND_OP_ADD;
    switch (bm) {
    case BlendMode::Disable:
    case BlendMode::AlphaToCoverage:
    case BlendMode::Normal: state.blendEnable = false; break;
    case BlendMode::Translucent:
        state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        state.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BlendMode::Additive:
        state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        state.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    }
}

inline void SetAlphaToCoverage(BlendMode bm, VkPipelineMultisampleStateCreateInfo& state) {
    state.alphaToCoverageEnable = bm == BlendMode::AlphaToCoverage;
}

inline void SetAlphaBlendWritePolicy(VkPipelineColorBlendAttachmentState& state,
                                     bool                                 writes_alpha) {
    if (writes_alpha) return;
    state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
}

inline void SetAttachmentLoadOp(BlendMode bm, VkAttachmentLoadOp& load_op) {
    switch (bm) {
    case BlendMode::Disable:
    case BlendMode::Normal: load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE; break;
    case BlendMode::Additive:
    case BlendMode::AlphaToCoverage:
    case BlendMode::Translucent: load_op = VK_ATTACHMENT_LOAD_OP_LOAD; break;
    }
}

inline bool EffectiveDepthWrite(const vrento::MaterialPipelineDesc& material) {
    // Transparent materials can intentionally write depth to mask later geometry.
    return material.depth_write;
}

inline bool UsesDepthAttachment(const vrento::MaterialPipelineDesc& material) {
    return material.depth_test || EffectiveDepthWrite(material);
}

inline void SetDepthState(const vrento::MaterialPipelineDesc&    material,
                          VkPipelineDepthStencilStateCreateInfo& state) {
    state.depthTestEnable  = material.depth_test;
    state.depthWriteEnable = EffectiveDepthWrite(material);
    state.depthCompareOp   = ToVkType(material.depth_compare);
}

inline void SetCullMode(CullMode mode, VkPipelineRasterizationStateCreateInfo& state) {
    switch (mode) {
    case CullMode::Front: state.cullMode = VK_CULL_MODE_FRONT_BIT; break;
    case CullMode::Back: state.cullMode = VK_CULL_MODE_BACK_BIT; break;
    case CullMode::None: state.cullMode = VK_CULL_MODE_NONE; break;
    }
}

inline void SetRasterState(const vrento::MaterialPipelineDesc& material, bool depth_clamp_supported,
                           VkPipelineRasterizationStateCreateInfo& state) {
    SetCullMode(material.cull_mode, state);
    state.depthClampEnable        = material.depth_clamp && depth_clamp_supported;
    state.depthBiasEnable         = material.depth_bias;
    state.depthBiasConstantFactor = material.depth_bias_constant;
    state.depthBiasClamp          = material.depth_bias_clamp;
    state.depthBiasSlopeFactor    = material.depth_bias_slope;
}

inline String MsaaTwinName(ref<str> tex_name, VkSampleCountFlagBits samples) {
    return rstd::format("{}::msaa{}", tex_name, static_cast<unsigned>(samples));
}

} // namespace owe::vulkan
