module;

#include <rstd/macro.hpp>
module wescene.vulkan_render;
import rstd.log;
import rstd.cppstd;
import wescene.vulkan;
import wescene.scene;

using namespace owe::vulkan;
using namespace rstd::prelude;
using rstd::cppstd::as_str;

namespace
{

constexpr std::string_view fullscreen_vertex = R"(#version 320 es
layout(location = 0) out vec2 v_Texcoord;

const vec2 k_Position[4] = vec2[4](
    vec2(-1.0, -1.0),
    vec2(-1.0,  1.0),
    vec2( 1.0, -1.0),
    vec2( 1.0,  1.0));
const vec2 k_Texcoord[4] = vec2[4](
    vec2(0.0, 1.0),
    vec2(0.0, 0.0),
    vec2(1.0, 1.0),
    vec2(1.0, 0.0));

void main()
{
    v_Texcoord = k_Texcoord[gl_VertexIndex];
    gl_Position = vec4(k_Position[gl_VertexIndex], 0.0, 1.0);
}
)";

constexpr std::string_view fullscreen_fragment = R"(#version 320 es
layout(location = 0) in vec2 v_Texcoord;
layout(location = 0) out vec4 out_FragColor;
layout(set = 1, binding = 1) uniform sampler2D u_Texture;

void main()
{
    // Swapchain consumers treat XRGB/ARGB consistently only when the
    // producer writes an opaque pixel. Keep this normalization from the
    // historical FinPass implementation.
    out_FragColor = vec4(texture(u_Texture, v_Texcoord).rgb, 1.0);
}
)";

} // namespace

FinPass::FinPass(Desc&& desc): m_desc(std::move(desc)) {}
FinPass::~FinPass() {}

void FinPass::setPresentFormat(VkFormat format) {
    if (m_present_format == format) return;
    m_present_format      = format;
    m_graphics_path       = format != VK_FORMAT_UNDEFINED;
    m_present_render_pass = VK_NULL_HANDLE;
    m_present_framebuffer.reset();
    m_present_framebuffer_view   = VK_NULL_HANDLE;
    m_present_framebuffer_extent = {};
}

bool FinPass::setFrameSurface(
    owe::FrameSurfaceLease                                                lease,
    rstd::mut_ref<rstd::dyn<resource_registry::ExternalResourcePreparer>> resources,
    const DeviceCapabilities& capabilities, rstd::uint32_t graphics_queue_family) {
    if (m_desc.external_use.is_none() || m_desc.result_use.is_none()) return false;
    m_frame_graphics_path = false;
    if (m_graphics_path && lease.format == m_present_format) {
        m_frame_graphics_path = ensurePresentFramebuffer(lease);
        if (! m_frame_graphics_path) {
            rstd_error("FinPass: present framebuffer unavailable; falling back to transfer path");
        }
    }
    auto prepared = resources->PrepareExternal(*m_desc.external_use,
                                               *m_desc.result_use,
                                               capabilities,
                                               rstd::move(lease),
                                               graphics_queue_family);
    if (prepared.is_err()) {
        auto error = rstd::move(prepared).unwrap_err_unchecked();
        rstd_error("prepare external frame failed: {}", error.message);
        return false;
    }
    return true;
}
bool FinPass::setResultRequest(rstd::Option<TextureRequest> request) {
    return SetTextureRequestIfChanged(m_desc.result_request, std::move(request));
}

void FinPass::resetResourceUses() {
    m_desc.result_use      = rstd::None();
    m_desc.external_use    = rstd::None();
    m_desc.pipeline_use    = rstd::None();
    m_desc.render_pass_use = rstd::None();
    m_desc.descriptor_use  = rstd::None();
}

void FinPass::declareResources(ResourceDeclarationContext& context) {
    resetResourceUses();
    if (m_desc.result_request.is_some()) {
        m_desc.result_use = rstd::Some(
            context.AddTexture(m_desc.result_request->clone(), resource::ResourceAccess::Read));
    }
    m_desc.external_use = rstd::Some(context.ReserveExternal());
    if (m_present_format != VK_FORMAT_UNDEFINED) {
        m_desc.pipeline_use    = rstd::Some(context.ReservePipeline());
        m_desc.render_pass_use = rstd::Some(context.ReserveRenderPass());
    }
}

PassResourceUses FinPass::resourceUses() const {
    PassResourceUses uses;
    if (m_desc.result_use.is_some()) {
        uses.textures.push(resource::TextureUseHandle(*m_desc.result_use));
    }
    if (m_desc.external_use.is_some()) {
        uses.externals.push(resource::ExternalUseHandle(*m_desc.external_use));
    }
    if (m_desc.pipeline_use.is_some()) {
        uses.pipelines.push(resource::PipelineUseHandle(*m_desc.pipeline_use));
    }
    if (m_desc.render_pass_use.is_some()) {
        uses.render_passes.push(resource::RenderPassUseHandle(*m_desc.render_pass_use));
    }
    if (m_desc.descriptor_use.is_some()) {
        uses.descriptors.push(resource::DescriptorBindingHandle(*m_desc.descriptor_use));
    }
    return uses;
}

auto FinPass::pipelineLayoutRequirement(const PreparedPassResources&) const
    -> Result<Option<PipelineLayoutRequirement>, resource::ResourceError> {
    if (m_desc.pipeline_use.is_none()) return Ok(None());

    PipelineLayoutRequirement requirement {
        .pipeline = *m_desc.pipeline_use,
    };
    PipelineLayoutSetRequirement set_requirement {
        .set             = u32(1),
        .push_descriptor = false,
    };
    set_requirement.bindings.push(PipelineLayoutBindingRequirement {
        .binding          = u32(1),
        .descriptor_type  = u32(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        .descriptor_count = u32(1),
        .stage_flags      = u32(VK_SHADER_STAGE_FRAGMENT_BIT),
        .shared_identity  = None(),
    });
    requirement.descriptor_sets.push(rstd::move(set_requirement));
    return Ok(Some(rstd::move(requirement)));
}

std::vector<PassTextureRequestDiagnostic> FinPass::textureRequestDiagnostics() const {
    std::vector<PassTextureRequestDiagnostic> out;
    out.push_back(PassTextureRequestDiagnostic {
        .role    = "frame-result",
        .name    = std::string(m_desc.result),
        .use     = m_desc.result_use,
        .request = m_desc.result_request.is_some() ? rstd::Some(m_desc.result_request->clone())
                                                   : rstd::None<TextureRequest>(),
    });
    return out;
}

bool FinPass::prepareResourceStates(
    rstd::mut_ref<rstd::dyn<resource_registry::TextureStatePreparer>> states) {
    m_desc.result_barrier.Clear();
    if (m_desc.result_use.is_none()) return false;
#if __is_target_os(macos)
    // Surface-mode MoltenVK uses a fullscreen graphics pass and samples the
    // scene result.  The offscreen path still uses the original transfer
    // state, as does every non-Apple build.
    const auto state = m_graphics_path ? resource_registry::TextureStateKind::Sampled
                                       : resource_registry::TextureStateKind::TransferSource;
#else
    const auto state = resource_registry::TextureStateKind::TransferSource;
#endif
    auto barrier = states->Prepare(*m_desc.result_use, state);
    if (barrier.is_none()) return false;
    m_desc.result_barrier.Add(rstd::move(barrier).unwrap_unchecked());
    return true;
}

void FinPass::prepare(Scene& scene, const Device& device, PassPrepareContext& context) {
    m_device      = &device;
    auto tex_name = std::string(m_desc.result);
    if (scene.RenderTarget(as_str(tex_name).unwrap()).is_none()) {
        rstd_error("FinPass: scene render target \"{}\" not found", tex_name);
        return;
    }
    if (m_desc.result_use.is_none()) return;
    auto prepared_result = context.resources->Resolve(*m_desc.result_use);
    if (prepared_result.is_none()) {
        rstd_error("FinPass: prepared texture \"{}\" unavailable", tex_name);
        return;
    }
    m_desc.descriptor_use = rstd::None();

    if (m_graphics_path) {
        if (m_desc.pipeline_use.is_none() || m_desc.render_pass_use.is_none()) {
            rstd_error("FinPass: graphics resources were not declared");
            m_graphics_path = false;
        } else {
            std::vector<ShaderCompUnit> units {
                ShaderCompUnit { ShaderType::VERTEX, std::string(fullscreen_vertex) },
                ShaderCompUnit { ShaderType::FRAGMENT, std::string(fullscreen_fragment) },
            };
            std::vector<Uni_ShaderSpv> spvs;
            if (! context.shader_backend->CompileAndLinkShaderUnits(
                    units,
                    ShaderCompOpt { .target = VulkanTarget::Vulkan_1_1, .optimize = true },
                    spvs)) {
                rstd_error("FinPass: fullscreen shader compilation failed");
                m_graphics_path = false;
            } else {
                auto layout = context.pipeline_layouts->Resolve(*m_desc.pipeline_use);
                if (layout.is_none()) {
                    rstd_error("FinPass: fullscreen pipeline layout unavailable");
                    m_graphics_path = false;
                } else {
                    GraphicsPipeline defaults;
                    defaults.toDefault();
                    VkPipelineColorBlendAttachmentState color_blend {
                        .blendEnable         = VK_FALSE,
                        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
                        .colorBlendOp        = VK_BLEND_OP_ADD,
                        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                        .alphaBlendOp        = VK_BLEND_OP_ADD,
                        .colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
                    };
                    PipelineResourceRequest request {
                        .pipeline_layout = *layout,
                        .shader_stages   = std::move(spvs),
                        .color_blend     = color_blend,
                        .depth           = defaults.depth,
                        .raster          = defaults.raster,
                        .multisample     = defaults.multisample,
                        .topology        = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
                        .color_format    = m_present_format,
                        // Window surfaces follow the old producer path:
                        // discard the acquired swapchain contents and let
                        // the render pass transition the image directly to
                        // PRESENT_SRC_KHR. The external bridge barriers are
                        // transfer-oriented and are only used by the
                        // offscreen fallback below.
                        .color_initial_layout = VK_IMAGE_LAYOUT_UNDEFINED,
                        .color_final_layout   = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        .color_load_op        = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                        .has_color_attachment = true,
                        .has_depth_attachment = false,
                    };
                    auto prepared = context.graphics->PreparePipeline(
                        *m_desc.pipeline_use, *m_desc.render_pass_use, device, std::move(request));
                    if (prepared.is_err()) {
                        auto error = rstd::move(prepared).unwrap_err_unchecked();
                        rstd_error("FinPass: fullscreen pipeline preparation failed: {}",
                                   error.message);
                        m_graphics_path = false;
                    } else {
                        auto render_pass = context.resources->Resolve(*m_desc.render_pass_use);
                        if (render_pass.is_none()) {
                            rstd_error("FinPass: fullscreen render pass unavailable");
                            m_graphics_path = false;
                        } else {
                            m_present_render_pass = **(**render_pass).physical;
                            auto images =
                                rstd::vec::Vec<resource_registry::DescriptorImageBinding>::make();
                            images.push(resource_registry::DescriptorImageBinding {
                                .binding = 1,
                                .image   = (**prepared_result).image.getActive(),
                                .layout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            });
                            auto buffers =
                                rstd::vec::Vec<resource_registry::DescriptorBufferBinding>::make();
                            auto descriptor = context.graphics->PrepareDescriptor(
                                device,
                                *layout,
                                u32(1),
                                images.as_slice(),
                                buffers.as_slice(),
                                resource_registry::DescriptorBindingReuse::Exclusive);
                            if (descriptor.is_err()) {
                                auto error = rstd::move(descriptor).unwrap_err_unchecked();
                                rstd_error("FinPass: fullscreen descriptor preparation failed: {}",
                                           error.message);
                                m_graphics_path = false;
                            } else {
                                m_desc.descriptor_use =
                                    rstd::Some(rstd::move(descriptor).unwrap_unchecked());
                            }
                        }
                    }
                }
            }
        }
    }
    setPrepared();
}

bool FinPass::ensurePresentFramebuffer(const owe::FrameSurfaceLease& lease) {
    if (! m_device || m_present_render_pass == VK_NULL_HANDLE ||
        lease.image.view == VK_NULL_HANDLE) {
        return false;
    }
    if (m_present_framebuffer && m_present_framebuffer_view == lease.image.view &&
        m_present_framebuffer_extent.width == lease.image.extent.width &&
        m_present_framebuffer_extent.height == lease.image.extent.height) {
        return true;
    }

    m_present_framebuffer.reset();
    VkFramebufferCreateInfo create_info {
        .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass      = m_present_render_pass,
        .attachmentCount = 1,
        .pAttachments    = &lease.image.view,
        .width           = lease.image.extent.width,
        .height          = lease.image.extent.height,
        .layers          = 1,
    };
    if (m_device->handle().CreateFramebuffer(create_info, m_present_framebuffer) != VK_SUCCESS) {
        rstd_error("FinPass: create fullscreen present framebuffer failed");
        m_present_framebuffer_view   = VK_NULL_HANDLE;
        m_present_framebuffer_extent = {};
        return false;
    }
    m_present_framebuffer_view   = lease.image.view;
    m_present_framebuffer_extent = {
        lease.image.extent.width,
        lease.image.extent.height,
    };
    return true;
}

void FinPass::record(PassRecordContext& context) {
    if (m_desc.result_use.is_none() || m_desc.external_use.is_none()) return;
    auto source   = context.resources->Resolve(*m_desc.result_use);
    auto external = context.resources->Resolve(*m_desc.external_use);
    if (source.is_none() || external.is_none()) return;
    const auto& result        = (**source).image.getActive();
    const auto& prepared      = (**external).frame;
    const auto& frame_surface = prepared.lease;
    const auto& present       = frame_surface.image;
    auto&       cmd           = *context.command;
    m_desc.result_barrier.Record(cmd);
    if (m_frame_graphics_path) {
        auto pipeline    = context.resources->Resolve(*m_desc.pipeline_use);
        auto render_pass = context.resources->Resolve(*m_desc.render_pass_use);
        auto descriptor  = context.resources->Resolve(*m_desc.descriptor_use);
        if (pipeline.is_none() || render_pass.is_none() || descriptor.is_none() ||
            ! m_present_framebuffer) {
            return;
        }

        VkRenderPassBeginInfo begin {
            .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass  = **(**render_pass).physical,
            .framebuffer = *m_present_framebuffer,
            .renderArea =
                VkRect2D {
                    .offset = { 0, 0 },
                    .extent = m_present_framebuffer_extent,
                },
        };
        cmd.BeginRenderPass(begin, VK_SUBPASS_CONTENTS_INLINE);
        const auto& physical = (**pipeline).physical;
        cmd.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *physical->pipeline.handle);
        VkViewport viewport {
            .x        = 0.0f,
            .y        = static_cast<float>(present.extent.height),
            .width    = static_cast<float>(present.extent.width),
            .height   = -static_cast<float>(present.extent.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        VkRect2D scissor { { 0, 0 }, { present.extent.width, present.extent.height } };
        cmd.SetViewport(0, viewport);
        cmd.SetScissor(0, scissor);
        auto& layout = *physical->layout;
        context.descriptor_state->UsePipeline(
            layout.handle, layout.descriptor_layouts.as_slice(), layout.push_constant_identity);
        (**descriptor).Record(cmd, physical->pipeline.layout, *context.descriptor_state);
        cmd.Draw(4, 1, 0, 0);
        cmd.EndRenderPass();
        if (frame_surface.final_queue_family != m_device->graphics_queue().family_index) {
            VkImageSubresourceRange range {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            };
            VkImageMemoryBarrier release {
                .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .dstAccessMask       = 0,
                .oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                .newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                .srcQueueFamilyIndex = m_device->graphics_queue().family_index,
                .dstQueueFamilyIndex = frame_surface.final_queue_family,
                .image               = present.handle,
                .subresourceRange    = range,
            };
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                VK_DEPENDENCY_BY_REGION_BIT,
                                release);
        }
        if (! m_path_logged) {
            rstd_info("FinPass: graphics fullscreen");
            m_path_logged = true;
        }
        return;
    }

    // Transfer fallback for offscreen ExSwapchain surfaces (and for a
    // surface-format/framebuffer mismatch on Apple). Non-Apple builds keep
    // the original transfer-only path below.
#if __is_target_os(macos)
    VkImageSubresourceRange range {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };

    if (m_graphics_path) {
        VkImageMemoryBarrier source_to_transfer {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = result.handle,
            .subresourceRange    = range,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            source_to_transfer);
    }
#endif
    prepared.before_copy.Record(cmd);

    const bool can_copy = result.extent.width == present.extent.width &&
                          result.extent.height == present.extent.height &&
                          frame_surface.format == VK_FORMAT_R8G8B8A8_UNORM;
    if (can_copy) {
        VkImageCopy region {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffset      = { 0, 0, 0 },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffset      = { 0, 0, 0 },
            .extent         = { result.extent.width, result.extent.height, 1 },
        };
        cmd.CopyImage(result.handle,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      present.handle,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      region);
    } else {
        VkImageBlit region {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets     = {
                VkOffset3D { 0, 0, 0 },
                VkOffset3D { static_cast<rstd::int32_t>(result.extent.width),
                             static_cast<rstd::int32_t>(result.extent.height),
                             1 },
            },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets     = {
                VkOffset3D { 0, 0, 0 },
                VkOffset3D { static_cast<rstd::int32_t>(present.extent.width),
                             static_cast<rstd::int32_t>(present.extent.height),
                             1 },
            },
        };
        cmd.BlitImage(result.handle,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      present.handle,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      region,
                      VK_FILTER_LINEAR);
    }

#if __is_target_os(macos)
    if (m_graphics_path) {
        VkImageMemoryBarrier transfer_to_source {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = result.handle,
            .subresourceRange    = range,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            transfer_to_source);
    }
#endif
    prepared.after_copy.Record(cmd);

    if (! m_path_logged) {
        rstd_info("FinPass: {}", can_copy ? "copy" : "blit");
        m_path_logged = true;
    }
}

void FinPass::destory(const Device&) {
#if __is_target_os(macos)
    m_present_framebuffer.reset();
    m_present_render_pass        = VK_NULL_HANDLE;
    m_present_framebuffer_view   = VK_NULL_HANDLE;
    m_present_framebuffer_extent = {};
    m_frame_graphics_path        = false;
    m_device                     = nullptr;
#endif
    m_desc.result_barrier.Clear();
    setPrepared(false);
}
