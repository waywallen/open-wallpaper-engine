module;

#include <rstd/macro.hpp>

module wescene.vulkan_render;
import wescene.spec_names;
import wescene.core;
import rstd;
import rstd.log;
import wescene.vulkan;
import wescene.scene;

using namespace owe::vulkan;
using namespace rstd::prelude;
using namespace rstd::literals;

CopyPass::CopyPass(Desc&& desc): m_desc(rstd::move(desc)) {}

CopyPass::~CopyPass() {};

PassInvalidationFlags CopyPass::finalizeResourceRequests(Scene& scene) {
    PassInvalidationFlags flags   = PassInvalidationNone;
    auto                  refresh = [&scene](ref<str> name) -> Option<TextureRequest> {
        auto text = name;
        if (name.is_empty() || ! IsSpecTex(text)) return None();
        auto target = scene.RenderTarget(text);
        if (target.is_none()) return None();
        return Some(MakeRenderTargetTextureRequest(name, **target));
    };

    if (auto request = refresh(m_desc.src.as_str());
        request.is_some() && SetTextureRequestIfChanged(m_desc.src_request, rstd::move(request))) {
        flags |= ToPassInvalidationFlags(PassInvalidation::Resources);
    }
    auto dst_request = refresh(m_desc.dst.as_str());
    if (m_desc.dst_matches_src && m_desc.src_request.is_some()) {
        dst_request       = Some(m_desc.src_request->clone());
        dst_request->name = m_desc.dst.clone();
    }
    if (dst_request.is_some() &&
        SetTextureRequestIfChanged(m_desc.dst_request, rstd::move(dst_request))) {
        flags |= ToPassInvalidationFlags(PassInvalidation::Resources);
    }
    return flags;
}

PassResourceUses CopyPass::resourceUses() const {
    PassResourceUses uses;
    if (m_desc.src_use.is_some()) uses.textures.push(resource::TextureUseHandle(*m_desc.src_use));
    if (m_desc.dst_use.is_some()) uses.textures.push(resource::TextureUseHandle(*m_desc.dst_use));
    return uses;
}

Vec<PassTextureRequestDiagnostic> CopyPass::textureRequestDiagnostics() const {
    Vec<PassTextureRequestDiagnostic> out;
    out.reserve(usize(2));
    out.push(PassTextureRequestDiagnostic {
        .role    = "copy-src"_Str,
        .name    = m_desc.src.clone(),
        .use     = m_desc.src_use,
        .request = m_desc.src_request.is_some() ? Some(m_desc.src_request->clone())
                                                : None<TextureRequest>(),
    });
    out.push(PassTextureRequestDiagnostic {
        .role    = "copy-dst"_Str,
        .name    = m_desc.dst.clone(),
        .use     = m_desc.dst_use,
        .request = m_desc.dst_request.is_some() ? Some(m_desc.dst_request->clone())
                                                : None<TextureRequest>(),
    });
    return out;
}

bool CopyPass::prepareResourceStates(mut_ref<dyn<resource_registry::TextureStatePreparer>> states) {
    m_desc.before_barriers.Clear();
    m_desc.after_barriers.Clear();
    if (m_desc.src_use.is_none() || m_desc.dst_use.is_none()) return false;

    auto range = resource_registry::TextureSubresourceRange {
        .level_count = u32(1),
        .layer_count = u32(1),
    };
    auto src_before = states->Prepare(
        *m_desc.src_use, resource_registry::TextureStateKind::TransferSource, range);
    auto dst_before = states->Prepare(
        *m_desc.dst_use, resource_registry::TextureStateKind::TransferDestination, range, true);
    auto src_after =
        states->Prepare(*m_desc.src_use, resource_registry::TextureStateKind::Sampled, range);
    auto dst_after =
        states->Prepare(*m_desc.dst_use, resource_registry::TextureStateKind::Sampled, range);
    if (src_before.is_none() || dst_before.is_none() || src_after.is_none() ||
        dst_after.is_none()) {
        return false;
    }
    m_desc.before_barriers.Add(rstd::move(src_before).unwrap_unchecked());
    m_desc.before_barriers.Add(rstd::move(dst_before).unwrap_unchecked());
    m_desc.after_barriers.Add(rstd::move(src_after).unwrap_unchecked());
    m_desc.after_barriers.Add(rstd::move(dst_after).unwrap_unchecked());
    return true;
}

void CopyPass::prepare(Scene&, const Device&, PassPrepareContext& context) {
    rstd::array<ref<str>, 2> textures { m_desc.src.as_str(), m_desc.dst.as_str() };
    rstd::array<Option<resource::TextureUseHandle>*, 2> texture_uses {
        &m_desc.src_use,
        &m_desc.dst_use,
    };
    for (usize i {}; i < textures.len(); i++) {
        auto& tex_name = textures[i];
        if (tex_name.is_empty()) continue;

        if (texture_uses[i]->is_none()) {
            rstd_error("copy texture {} has no resource use", tex_name);
            return;
        }
        auto prepared = context.resources->Resolve(**texture_uses[i]);
        if (prepared.is_none()) {
            rstd_error("prepared copy texture {} not found", tex_name);
            return;
        }
    }

    setPrepared();
};
void CopyPass::record(PassRecordContext& context) {
    if (m_desc.src_use.is_none() || m_desc.dst_use.is_none()) return;
    auto prepared_src = context.resources->Resolve(*m_desc.src_use);
    auto prepared_dst = context.resources->Resolve(*m_desc.dst_use);
    if (prepared_src.is_none() || prepared_dst.is_none()) return;
    auto& cmd = *context.command;
    auto& src = (**prepared_src).image.getActive();
    auto& dst = (**prepared_dst).image.getActive();

    if (! (src.handle && dst.handle)) {
        rstd_assert(src.handle && dst.handle);
        return;
    }

    VkImageSubresourceRange srang {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,

    };
    VkImageCopy copy {
        .srcSubresource =
            VkImageSubresourceLayers {
                .aspectMask     = srang.aspectMask,
                .mipLevel       = 0,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        .dstSubresource =
            VkImageSubresourceLayers {
                .aspectMask     = srang.aspectMask,
                .mipLevel       = 0,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        .extent = { src.extent.width, src.extent.height, 1 },
    };
    m_desc.before_barriers.Record(cmd);
    cmd.CopyImage(src.handle,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  dst.handle,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  copy);
    m_desc.after_barriers.Record(cmd);

    if (dst.mipmap_level > 1) {
        RecordGenerateMipmaps(cmd, dst);
    }
};
void CopyPass::destory(const Device&) {}
