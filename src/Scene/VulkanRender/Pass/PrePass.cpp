module;

#include <rstd/macro.hpp>

module wescene.vulkan_render;
import rstd.log;
import wescene.types;
import wescene.vulkan;
import wescene.scene;

using namespace owe::vulkan;
using namespace rstd::prelude;
using namespace rstd::literals;

PrePass::PrePass(Desc&& desc): m_desc(rstd::move(desc)) {}
PrePass::~PrePass() {}

bool PrePass::setResultRequest(Option<TextureRequest> request,
                               Option<TextureRequest> msaa_request) {
    bool changed = SetTextureRequestIfChanged(m_desc.result_request, rstd::move(request));
    changed =
        SetTextureRequestIfChanged(m_desc.result_msaa_request, rstd::move(msaa_request)) || changed;
    return changed;
}

void PrePass::resetResourceUses() {
    m_desc.result_use      = None();
    m_desc.result_msaa_use = None();
    m_desc.render_pass_use = None();
    m_desc.framebuffer_use = None();
}

void PrePass::declareResources(ResourceDeclarationContext& context) {
    resetResourceUses();

    if (m_desc.result_request.is_some()) {
        m_desc.result_use = Some(
            context.AddTexture(m_desc.result_request->clone(), resource::ResourceAccess::Write));
    }
    if (m_desc.result_msaa_request.is_none()) return;
    m_desc.result_msaa_use = Some(
        context.AddTexture(m_desc.result_msaa_request->clone(), resource::ResourceAccess::Write));
    m_desc.render_pass_use = Some(context.ReserveRenderPass());
    m_desc.framebuffer_use = Some(context.ReserveFramebuffer());
}

PassResourceUses PrePass::resourceUses() const {
    PassResourceUses uses;
    if (m_desc.result_use.is_some()) {
        uses.textures.push(resource::TextureUseHandle(*m_desc.result_use));
    }
    if (m_desc.result_msaa_use.is_some()) {
        uses.textures.push(resource::TextureUseHandle(*m_desc.result_msaa_use));
    }
    if (m_desc.render_pass_use.is_some()) {
        uses.render_passes.push(resource::RenderPassUseHandle(*m_desc.render_pass_use));
    }
    if (m_desc.framebuffer_use.is_some()) {
        uses.framebuffers.push(resource::FramebufferUseHandle(*m_desc.framebuffer_use));
    }
    return uses;
}

bool PrePass::prepareResourceStates(mut_ref<dyn<resource_registry::TextureStatePreparer>> states) {
    m_desc.before_clear.Clear();
    m_desc.after_clear.Clear();
    if (m_desc.result_use.is_none()) return false;
    auto before = states->Prepare(
        *m_desc.result_use, resource_registry::TextureStateKind::TransferDestination, {}, true);
    auto after = states->Prepare(*m_desc.result_use, resource_registry::TextureStateKind::Sampled);
    if (before.is_none() || after.is_none()) return false;
    m_desc.before_clear.Add(rstd::move(before).unwrap_unchecked());
    m_desc.after_clear.Add(rstd::move(after).unwrap_unchecked());
    return m_desc.result_msaa_use.is_none() ||
           states->Set(*m_desc.result_msaa_use,
                       resource_registry::TextureStateKind::ColorAttachment);
}

Vec<PassTextureRequestDiagnostic> PrePass::textureRequestDiagnostics() const {
    Vec<PassTextureRequestDiagnostic> out;
    out.reserve(usize(m_desc.result_msaa_request.is_some() ? 2 : 1));
    out.push(PassTextureRequestDiagnostic {
        .role    = "frame-result"_Str,
        .name    = m_desc.result.clone(),
        .use     = m_desc.result_use,
        .request = m_desc.result_request.is_some() ? Some(m_desc.result_request->clone())
                                                   : None<TextureRequest>(),
    });
    if (m_desc.result_msaa_request.is_some()) {
        out.push(PassTextureRequestDiagnostic {
            .role    = "frame-result-msaa"_Str,
            .name    = m_desc.result_msaa_request->name.clone(),
            .use     = m_desc.result_msaa_use,
            .request = Some(m_desc.result_msaa_request->clone()),
        });
    }
    return out;
}

void PrePass::prepare(Scene& scene, const Device& device, PassPrepareContext& context) {
    {
        auto tex_name = m_desc.result.as_str();
        if (scene.RenderTarget(tex_name).is_none()) {
            rstd_error("frame result render target {} not found", tex_name);
            return;
        }
        if (m_desc.result_use.is_none()) {
            rstd_error("frame result texture use {} not found", tex_name);
            return;
        }
        if (context.resources->Resolve(*m_desc.result_use).is_none()) {
            rstd_error("prepared frame result texture {} not found", tex_name);
            return;
        }
    }
    {
        auto tex_name = m_desc.result.as_str();
        auto target   = scene.RenderTarget(tex_name);
        if (target.is_none()) return;
        m_desc.samples = TextureSampleCount((**target).sample_count);
        if (m_desc.samples != VK_SAMPLE_COUNT_1_BIT) {
            if (m_desc.result_msaa_use.is_none()) {
                rstd_error("frame MSAA texture use {} not found", tex_name);
                return;
            }
            auto prepared = context.resources->Resolve(*m_desc.result_msaa_use);
            if (prepared.is_none() || m_desc.result_msaa_request.is_none() ||
                m_desc.render_pass_use.is_none() || m_desc.framebuffer_use.is_none()) {
                rstd_error("prepared frame MSAA resources {} incomplete", tex_name);
                return;
            }
            auto render_pass = context.graphics->PrepareRenderPass(
                *m_desc.render_pass_use,
                device,
                RenderPassResourceDesc {
                    .samples                = m_desc.samples,
                    .color_initial_layout   = VK_IMAGE_LAYOUT_UNDEFINED,
                    .color_final_layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    .color_load_op          = VK_ATTACHMENT_LOAD_OP_CLEAR,
                    .has_resolve_attachment = false,
                    .has_depth_attachment   = false,
                });
            if (render_pass.is_err()) {
                auto error = rstd::move(render_pass).unwrap_err_unchecked();
                rstd_error("prepare frame MSAA render pass failed: {}", error.message);
                return;
            }
            auto                           image = (**prepared).image.getActive();
            Vec<FramebufferAttachmentDesc> attachments;
            attachments.push(MakeFramebufferAttachment(*m_desc.result_msaa_request, image));
            auto framebuffer =
                context.graphics->PrepareFramebuffer(*m_desc.framebuffer_use,
                                                     *m_desc.render_pass_use,
                                                     device,
                                                     rstd::move(attachments),
                                                     { image.extent.width, image.extent.height });
            if (framebuffer.is_err()) {
                auto error = rstd::move(framebuffer).unwrap_err_unchecked();
                rstd_error("prepare frame MSAA framebuffer failed: {}", error.message);
                return;
            }
        }
    }
    {
        auto sc            = scene.ClearColor();
        m_desc.clear_value = VkClearValue {
            .color = { .float32 = { sc[usize()], sc[usize(1)], sc[usize(2)], 1.0f } }
        };
    }
    setPrepared();
}

void PrePass::record(PassRecordContext& context) {
    if (m_desc.result_use.is_none()) return;
    auto prepared_result = context.resources->Resolve(*m_desc.result_use);
    if (prepared_result.is_none()) return;
    const auto&             result = (**prepared_result).image.getActive();
    auto&                   cmd    = *context.command;
    VkImageSubresourceRange base_srang {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = VK_REMAINING_ARRAY_LAYERS,
        .baseArrayLayer = 0,
        .layerCount     = VK_REMAINING_MIP_LEVELS,

    };
    m_desc.before_clear.Record(cmd);
    cmd.ClearColorImage(
        result.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &m_desc.clear_value.color, base_srang);
    m_desc.after_clear.Record(cmd);
    if (m_desc.samples != VK_SAMPLE_COUNT_1_BIT) {
        if (m_desc.result_msaa_use.is_none() || m_desc.render_pass_use.is_none() ||
            m_desc.framebuffer_use.is_none()) {
            return;
        }
        auto prepared_msaa = context.resources->Resolve(*m_desc.result_msaa_use);
        auto render_pass   = context.resources->Resolve(*m_desc.render_pass_use);
        auto framebuffer   = context.resources->Resolve(*m_desc.framebuffer_use);
        if (prepared_msaa.is_none() || render_pass.is_none() || framebuffer.is_none()) return;
        const auto&           msaa = (**prepared_msaa).image.getActive();
        VkRenderPassBeginInfo pass_begin_info {
            .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass  = **(**render_pass).physical,
            .framebuffer = **(**framebuffer).physical,
            .renderArea =
                VkRect2D {
                    .offset = { 0, 0 },
                    .extent = { msaa.extent.width, msaa.extent.height },
                },
            .clearValueCount = 1,
            .pClearValues    = &m_desc.clear_value,
        };
        cmd.BeginRenderPass(pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
        cmd.EndRenderPass();
    }
}
void PrePass::destory(const Device&) {
    m_desc.before_clear.Clear();
    m_desc.after_clear.Clear();
    m_desc.samples = VK_SAMPLE_COUNT_1_BIT;
    setPrepared(false);
}
