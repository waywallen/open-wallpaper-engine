module;
#include <rstd/macro.hpp>

#include "vvk/macros.hpp"

#include <cerrno>
#include <unistd.h>
#include <vulkan/vulkan.h>

module wescene.vulkan_render;
import wescene.core;
import wescene.types;
import rstd.log;
import rstd.cppstd;
import wescene.load_bench;
import wescene.resource_registry;
import wescene.vulkan;
import wescene.utils;
import wescene.scene;
import wescene.text;

import wescene.rgraph;

using namespace owe::vulkan;
using namespace rstd::prelude;
using namespace rstd::literals;

constexpr std::uint64_t        vk_wait_time { static_cast<std::uint64_t>(
    rstd::time::Duration::from_secs(u64(10)).as_nanos().to_primitive()) };
constexpr std::uint32_t        vk_upload_command_num { 3 };
constexpr std::uint32_t        vk_command_num { vk_upload_command_num + 1 };
constexpr VkPipelineStageFlags vk_upload_wait_stages { VK_PIPELINE_STAGE_TRANSFER_BIT |
                                                       VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                                       VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT };

bool SameRenderItemId(owe::RenderItemId lhs, owe::RenderItemId rhs) {
    return lhs.index == rhs.index && lhs.generation == rhs.generation;
}

void PushUniqueRenderItem(Vec<owe::RenderItemId>& items, owe::RenderItemId id) {
    for (usize index {}; index < items.len(); ++index) {
        if (SameRenderItemId(items[index], id)) return;
    }
    items.push(rstd::move(id));
}

Vec<owe::RenderItemId> RenderItemsForMaterials(const owe::RenderSceneSnapshot& render_scene,
                                               slice<owe::SceneMaterialId>     materials) {
    Vec<owe::RenderItemId> render_items;
    for (usize material_index {}; material_index < materials.len(); ++material_index) {
        auto material       = materials[material_index];
        auto material_items = render_scene.renderItemsFor(material);
        for (usize index {}; index < material_items.len(); ++index) {
            PushUniqueRenderItem(render_items, material_items[index]);
        }
    }
    return render_items;
}

constexpr rstd::array<Extension, 4> base_inst_exts {
    Extension { false, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME },
    Extension { false, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME },
    Extension { false, VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME },
    Extension { false, VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME },
};
#if __is_target_os(macos)
constexpr rstd::array<Extension, 7> base_device_exts {
    // MoltenVK's swapchain→Metal present path uses the portability subset.
    Extension { false, "VK_KHR_portability_subset" },
    Extension { false, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME },
    // VideoToolbox frames are imported through MoltenVK's Metal-object bridge.
    Extension { false, "VK_EXT_metal_objects" },
    Extension { false, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME },
    Extension { false, VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME },
    Extension { true, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME },
    Extension { false, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME },
};
#else
constexpr rstd::array<Extension, 8> base_device_exts {
    Extension { false, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME },
    Extension { false, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME },
    Extension { false, VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME },
    Extension { true, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME },
    Extension { true, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME },
    Extension { true, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME },
    Extension { false, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME },
    // Optional. When present we can report the picked physical device's
    // DRM render-node major/minor via `getDrmRenderNode()` so the
    // waywallen daemon can match it against each connected display's
    // GPU. When absent the accessor returns false and callers report
    // (0, 0); the daemon then conservatively assumes cross-GPU.
    Extension { false, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME },
};
#endif

void AppendVideoDeviceExtensions(std::vector<Extension>& device_exts) {
#if __is_target_os(macos)
    // VideoToolbox frames use the Metal-object import path. The DMA-BUF/DRM
    // and Vulkan-video extensions below are Linux-only interop requirements.
    (void)device_exts;
#else
    device_exts.push_back({ false, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME });
    device_exts.push_back({ false, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME });
    device_exts.push_back({ false, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME });
    device_exts.push_back({ false, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME });
    device_exts.push_back({ false, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME });
    device_exts.push_back({ false, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME });
    device_exts.push_back({ false, VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME });
    device_exts.push_back({ false, VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME });
    device_exts.push_back({ false, VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME });
    device_exts.push_back({ false, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME });
    device_exts.push_back({ false, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME });
    device_exts.push_back({ false, VK_EXT_SHADER_OBJECT_EXTENSION_NAME });
#endif
}

void ReleaseCompletedRetiredResources(Device& device, RenderingResources& rr) {
    auto memory = rstd::dyn<owe::vulkan::MemoryBudgetSource>::from_ref(device);
    rr.resources.Collect(memory.as_mut_ref());
}

struct VulkanRender::Impl {
    Impl()  = default;
    ~Impl() = default;

    bool init(RenderInitInfo, SceneLoadBenchRecorderView);
    void destroy();

    void drawFrame(Scene&);

    bool CreateRenderingResource(RenderingResources&);
    void DestroyRenderingResource(RenderingResources&);

    void clearLastRenderGraph(RenderGraphResourceRetention);
    void configureRenderTargets(Scene&);
    void compileRenderGraph(Scene&, rg::RenderGraph&);
    void compileRenderGraph(Scene&, rg::RenderGraph&, const RenderSceneSnapshot&);
    void compileRenderGraph(Scene&, rg::RenderGraph&, const RenderSceneSnapshot&,
                            SceneLoadBenchRecorderView);
    void refreshPreparedResources(Scene&);
    void refreshPreparedResources(Scene&, const RenderSceneSnapshot&);
    void refreshPreparedResources(Scene&, const RenderSceneSnapshot&,
                                  resource::ResourcePlanSections);
    void refreshPreparedTextures(Scene&, const RenderSceneSnapshot&);
    void invalidatePreparedRenderItems(slice<owe::RenderItemId>, PassInvalidationFlags);
    void refreshPreparedRenderItems(Scene&, const RenderSceneSnapshot&, slice<owe::RenderItemId>,
                                    PassInvalidationFlags);
    void refreshPreparedMaterial(Scene&, const RenderSceneSnapshot&, owe::SceneMaterialId,
                                 PassInvalidationFlags);
    bool refreshPreparedMaterialTextures(Scene&, const RenderSceneSnapshot&, owe::SceneMaterialId);
    bool refreshPreparedMaterialTextures(Scene&, const RenderSceneSnapshot&,
                                         slice<owe::SceneMaterialId>);
    void refreshPreparedMesh(Scene&, const RenderSceneSnapshot&, owe::SceneMeshId,
                             PassInvalidationFlags);
    std::vector<PreparedPassDiagnostic> preparedPassDiagnostics() const;
    void                                UpdateCameraFillMode(Scene&, owe::FillMode);

    bool                      initRes();
    rstd::Option<std::size_t> acquireUploadCommandSlot(RenderingResources&);
    bool                      commitPreparedUploads(SceneLoadBenchRecorderView load_bench = {});
    bool prepareProgram(Scene&, const RenderSceneSnapshot&, resource::ResourcePlanSections,
                        SceneLoadBenchRecorderView = {});
    bool waitForPreparedUploads(RenderingResources&);
    void drawFrameSwapchain(Scene&);
    void drawFrameOffscreen(Scene&);
    bool onSwapchainReady(unsigned width, unsigned height);

    Instance     m_instance;
    Box<Device>  m_device { Box<Device>::make() };
    Box<PrePass> m_prepass { Box<PrePass>::make(PrePass::Desc {}) };
    Box<FinPass> m_finpass { Box<FinPass>::make(FinPass::Desc {}) };
    ReDrawCB     m_redraw_cb;

    Box<dyn<ShaderBackend>>    m_shader_backend { MakeShaderBackend() };
    ShaderReflectionCache      m_shader_reflection_cache { m_shader_backend.as_ref() };
    SceneLoadBenchRecorderView m_pending_load_bench;

    vvk::CommandBuffers             m_cmds;
    std::vector<vvk::CommandBuffer> m_upload_cmds;
    std::vector<std::uint64_t>      m_upload_cmd_values;
    std::size_t                     m_next_upload_cmd { 0 };
    vvk::CommandBuffer              m_render_cmd;

    bool m_with_surface { false };
    bool m_inited { false };

    // MSAA sample count for the screen RT only. 1bit = disabled.
    // Resolved against device's framebufferColorSampleCounts in init().
    VkSampleCountFlagBits m_msaa_samples { VK_SAMPLE_COUNT_1_BIT };

    std::shared_ptr<ExSwapchain> m_ex_swapchain;
    RenderingResources           m_rendering_resources;
    u64                          m_next_surface_acquire_serial { 1 };

    // for VUID-vkQueueSubmit-pSignalSemaphores-00067
    std::vector<vvk::Semaphore> m_sem_swap_finish_per_image;

    RenderProgram m_program;
};

VulkanRender::VulkanRender(): pImpl(Box<Impl>::make()) {}
VulkanRender::~VulkanRender() {};

bool VulkanRender::inited() const { return pImpl->m_inited; }

int VulkanRender::takeLastFrameSyncFd() {
    return pImpl->m_ex_swapchain ? pImpl->m_ex_swapchain->takeLastFrameSyncFd() : -1;
}

bool VulkanRender::getDrmRenderNode(std::uint32_t& out_major, std::uint32_t& out_minor) const {
    if (! pImpl->m_inited) return false;
    VkPhysicalDeviceDrmPropertiesEXT drm {};
    drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2KHR props {};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR;
    props.pNext = &drm;
    pImpl->m_device->gpu().GetProperties2KHR(props);
    if (! drm.hasRender) return false;
    if (drm.renderMajor < 0 || drm.renderMinor < 0 ||
        static_cast<std::uint64_t>(drm.renderMajor) > UINT32_MAX ||
        static_cast<std::uint64_t>(drm.renderMinor) > UINT32_MAX) {
        return false;
    }
    out_major = static_cast<std::uint32_t>(drm.renderMajor);
    out_minor = static_cast<std::uint32_t>(drm.renderMinor);
    return true;
}

DeviceCapabilities VulkanRender::deviceCapabilities() const {
    if (! pImpl->m_inited) return {};
    return pImpl->m_device->capabilities();
}

VkInstance VulkanRender::vkInstance() const {
    if (! pImpl->m_inited) return VK_NULL_HANDLE;
    return *pImpl->m_instance.inst();
}

VkPhysicalDevice VulkanRender::vkPhysicalDevice() const {
    if (! pImpl->m_inited) return VK_NULL_HANDLE;
    return *pImpl->m_device->gpu();
}

VkDevice VulkanRender::vkDevice() const {
    if (! pImpl->m_inited) return VK_NULL_HANDLE;
    return *pImpl->m_device->handle();
}

VkQueue VulkanRender::vkGraphicsQueue() const {
    if (! pImpl->m_inited) return VK_NULL_HANDLE;
    return *pImpl->m_device->graphics_queue().handle;
}

std::uint32_t VulkanRender::vkGraphicsQueueFamily() const {
    if (! pImpl->m_inited) return 0;
    return pImpl->m_device->graphics_queue().family_index;
}

void VulkanRender::deviceUuid(std::uint8_t out[16]) const {
    std::memset(out, 0, 16);
    if (! pImpl->m_inited) return;
    VkPhysicalDeviceIDPropertiesKHR id {};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2KHR props {};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR;
    props.pNext = &id;
    pImpl->m_device->gpu().GetProperties2KHR(props);
    std::memcpy(out, id.deviceUUID, 16);
}

void VulkanRender::pumpVideoTextures(double dt_seconds) {
    if (! pImpl->m_inited) return;
    pImpl->m_rendering_resources.resources.PumpVideoTextures(dt_seconds);
}

void VulkanRender::pumpFontAtlases(Scene& scene) {
    if (! pImpl->m_inited) return;
    auto* fc = owe::text::SceneFontCache(scene);
    if (fc == nullptr) return;
    for (auto* face : fc->Faces()) {
        if (face == nullptr) continue;
        auto rects = face->DirtyRects();
        if (rects.empty()) continue;
        // Coalesce all dirty rects into one AABB. Typical: ≤ a handful of
        // glyph slots per frame, so a single upload covering the union beats
        // submitting one copy per rect.
        std::uint32_t min_x = rects[0].x;
        std::uint32_t min_y = rects[0].y;
        std::uint32_t max_x = rects[0].x + rects[0].w;
        std::uint32_t max_y = rects[0].y + rects[0].h;
        for (auto& r : rects.subspan(1)) {
            if (r.x < min_x) min_x = r.x;
            if (r.y < min_y) min_y = r.y;
            const std::uint32_t rx2 = r.x + r.w;
            const std::uint32_t ry2 = r.y + r.h;
            if (rx2 > max_x) max_x = rx2;
            if (ry2 > max_y) max_y = ry2;
        }
        const auto fm     = face->Metrics();
        const auto pixels = face->AtlasPixels();
        const bool uploaded =
            pImpl->m_rendering_resources.resources.UploadFontAtlasRegion(face->AtlasUrl(),
                                                                         pixels.data(),
                                                                         fm.atlas_w,
                                                                         min_x,
                                                                         min_y,
                                                                         max_x - min_x,
                                                                         max_y - min_y);
        if (uploaded) face->ClearDirtyRects();
    }
}

void VulkanRender::driverUuid(std::uint8_t out[16]) const {
    std::memset(out, 0, 16);
    if (! pImpl->m_inited) return;
    VkPhysicalDeviceIDPropertiesKHR id {};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2KHR props {};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR;
    props.pNext = &id;
    pImpl->m_device->gpu().GetProperties2KHR(props);
    std::memcpy(out, id.driverUUID, 16);
}

bool VulkanRender::init(RenderInitInfo info, SceneLoadBenchRecorderView load_bench) {
    return pImpl->init(rstd::move(info), load_bench);
}
void VulkanRender::destroy() { pImpl->destroy(); }
void VulkanRender::drawFrame(Scene& scene) { pImpl->drawFrame(scene); };
void VulkanRender::clearLastRenderGraph(RenderGraphResourceRetention retention) {
    pImpl->clearLastRenderGraph(retention);
};
void VulkanRender::configureRenderTargets(Scene& scene) { pImpl->configureRenderTargets(scene); }
void VulkanRender::compileRenderGraph(Scene& scene, rg::RenderGraph& rg) {
    pImpl->compileRenderGraph(scene, rg);
}
void VulkanRender::compileRenderGraph(Scene& scene, rg::RenderGraph& rg,
                                      const RenderSceneSnapshot& render_scene) {
    pImpl->compileRenderGraph(scene, rg, render_scene);
}
void VulkanRender::compileRenderGraph(Scene& scene, rg::RenderGraph& rg,
                                      const RenderSceneSnapshot& render_scene,
                                      SceneLoadBenchRecorderView load_bench) {
    pImpl->compileRenderGraph(scene, rg, render_scene, load_bench);
}
void VulkanRender::refreshPreparedResources(Scene& scene) {
    pImpl->refreshPreparedResources(scene);
}
void VulkanRender::refreshPreparedResources(Scene& scene, const RenderSceneSnapshot& render_scene) {
    pImpl->refreshPreparedResources(scene, render_scene);
}
void VulkanRender::refreshPreparedTextures(Scene& scene, const RenderSceneSnapshot& render_scene) {
    pImpl->refreshPreparedTextures(scene, render_scene);
}
void VulkanRender::invalidatePreparedRenderItems(slice<owe::RenderItemId> render_items,
                                                 PassInvalidationFlags    flags) {
    pImpl->invalidatePreparedRenderItems(render_items, flags);
}
void VulkanRender::refreshPreparedRenderItems(Scene& scene, const RenderSceneSnapshot& render_scene,
                                              slice<owe::RenderItemId> render_items,
                                              PassInvalidationFlags    flags) {
    pImpl->refreshPreparedRenderItems(scene, render_scene, render_items, flags);
}
void VulkanRender::refreshPreparedMaterial(Scene& scene, const RenderSceneSnapshot& render_scene,
                                           owe::SceneMaterialId  material,
                                           PassInvalidationFlags flags) {
    pImpl->refreshPreparedMaterial(scene, render_scene, material, flags);
}
bool VulkanRender::refreshPreparedMaterialTextures(Scene&                     scene,
                                                   const RenderSceneSnapshot& render_scene,
                                                   owe::SceneMaterialId       material) {
    return pImpl->refreshPreparedMaterialTextures(scene, render_scene, material);
}
bool VulkanRender::refreshPreparedMaterialTextures(Scene&                      scene,
                                                   const RenderSceneSnapshot&  render_scene,
                                                   slice<owe::SceneMaterialId> materials) {
    return pImpl->refreshPreparedMaterialTextures(scene, render_scene, materials);
}
void VulkanRender::refreshPreparedMesh(Scene& scene, const RenderSceneSnapshot& render_scene,
                                       owe::SceneMeshId mesh, PassInvalidationFlags flags) {
    pImpl->refreshPreparedMesh(scene, render_scene, mesh, flags);
}
std::vector<PreparedPassDiagnostic> VulkanRender::preparedPassDiagnostics() const {
    return pImpl->preparedPassDiagnostics();
}
void VulkanRender::evictUnusedMeshes() {
    if (pImpl->m_inited) pImpl->m_rendering_resources.resources.EvictUnusedBuffers();
};
void VulkanRender::UpdateCameraFillMode(Scene& scene, owe::FillMode fill) {
    pImpl->UpdateCameraFillMode(scene, fill);
};

bool VulkanRender::onSwapchainReady(unsigned width, unsigned height) {
    return pImpl->onSwapchainReady(width, height);
}

owe::ExSwapchain* VulkanRender::exSwapchain() const { return pImpl->m_ex_swapchain.get(); };

bool VulkanRender::Impl::init(RenderInitInfo info, SceneLoadBenchRecorderView load_bench) {
    if (m_inited) return true;

    m_redraw_cb = info.redraw_callback;
    VkExtent2D extent { info.width, info.height };
    if (extent.width * extent.height < 500 * 500) {
        rstd_error("too small swapchain image size: {}x{}", extent.width, extent.height);
    } else {
        rstd_info("set swapchain image size: {}x{}", extent.width, extent.height);
    }

    std::vector<Extension> inst_exts;
    std::vector<Extension> device_exts;
    for (const auto& extension : base_inst_exts) inst_exts.push_back(extension);
    for (const auto& extension : base_device_exts) device_exts.push_back(extension);
    if (info.video_hwdec != "none") {
        AppendVideoDeviceExtensions(device_exts);
    }

    if (! info.offscreen) {
        std::transform(info.surface_info.instanceExts.begin(),
                       info.surface_info.instanceExts.end(),
                       std::back_inserter(inst_exts),
                       [](const auto& s) {
                           return Extension { true, s.c_str() };
                       });
        device_exts.push_back({ true, VK_KHR_SWAPCHAIN_EXTENSION_NAME });
    } else {
        // Iteration 1a: offscreen FDs are real Linux DMA-BUFs so they can be
        // imported by arbitrary external consumers. These extensions are
        // strictly required on the offscreen path; if a driver lacks them
        // we fail fast in Device::CheckGPU.
        device_exts.push_back({ true, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME });
        // Required by VK_EXT_image_drm_format_modifier on Vulkan 1.1 (promoted
        // to core in 1.2). Validation layer rejects the device otherwise.
        device_exts.push_back({ true, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME });
        device_exts.push_back({ true, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME });
        device_exts.push_back({ true, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME });
    }

    std::vector<InstanceLayer> inst_layers;
    // valid layer
    if (info.enable_valid_layer) {
        inst_layers.push_back({ true, VALIDATION_LAYER_NAME });
        rstd_info("vulkan valid layer \"{}\" enabled", VALIDATION_LAYER_NAME);
    }

    const auto instance_api_version =
        info.video_hwdec == "none" ? WP_VULKAN_VERSION : VK_API_VERSION_1_3;
    {
        auto instance_span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::vulkan_instance);
        if (! Instance::Create(m_instance, inst_exts, inst_layers, instance_api_version)) {
            rstd_error("init vulkan failed");
            return false;
        }
        if (! info.offscreen) {
            VkSurfaceKHR surface;
            VVK_CHECK_ACT(
                {
                    rstd_error("create vulkan surface failed");
                    return false;
                },
                info.surface_info.createSurfaceOp(*m_instance.inst(), &surface));
            m_instance.setSurface(VkSurfaceKHR(surface));
            m_with_surface = true;
        }
        auto surface   = *m_instance.surface();
        auto check_gpu = [&device_exts, surface](const vvk::PhysicalDevice& gpu) {
            return Device::CheckGPU(gpu, device_exts, surface);
        };
        if (! m_instance.ChoosePhysicalDevice(check_gpu, info.uuid)) return false;
    }

    {
        auto device_span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::vulkan_device);
        if (! Device::Create(m_instance, device_exts, extent, *m_device)) {
            rstd_error("init vulkan device failed");
            return false;
        }
        if (! m_rendering_resources.resources.Initialize(*m_device)) {
            rstd_error("resource registry init failed");
            return false;
        }
#if __is_target_os(macos)
        if (! info.offscreen) {
            // Surface-mode FinPass uses a fullscreen graphics pass. The
            // swapchain format is fixed when Device::Create returns, so it
            // can be part of the render-pass/pipeline resource plan.
            m_finpass->setPresentFormat(m_device->swapchain().format());
        }
#endif
        m_rendering_resources.resources.SetVideoDecodeOptions(TextureCache::VideoDecodeOptions {
            .hwdec       = info.video_hwdec,
            .render_node = info.video_render_node,
        });
    }

    {
        // Map requested integer to a bit; clamp down to highest supported bit
        // not exceeding the request, given device's framebufferColorSampleCounts.
        const std::uint32_t      requested = info.msaa_samples == 0 ? 1u : info.msaa_samples;
        const VkSampleCountFlags supported = m_device->limits().framebufferColorSampleCounts;
        VkSampleCountFlagBits    chosen    = VK_SAMPLE_COUNT_1_BIT;
        constexpr rstd::array<VkSampleCountFlagBits, 6> ladder {
            VK_SAMPLE_COUNT_64_BIT, VK_SAMPLE_COUNT_32_BIT, VK_SAMPLE_COUNT_16_BIT,
            VK_SAMPLE_COUNT_8_BIT,  VK_SAMPLE_COUNT_4_BIT,  VK_SAMPLE_COUNT_2_BIT,
        };
        for (auto bit : ladder) {
            if (static_cast<std::uint32_t>(bit) <= requested && (supported & bit)) {
                chosen = bit;
                break;
            }
        }
        m_msaa_samples = chosen;
        rstd_info(
            "msaa requested={} actual={}", requested, static_cast<std::uint32_t>(m_msaa_samples));
    }

    if (info.offscreen) {
        auto swapchain_span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::vulkan_swapchain);
        if (info.ex_swapchain_factory) {
            RenderInitInfo::ExSwapchainHandles h {
                *m_instance.inst(),
                *m_device->gpu(),
                *m_device->handle(),
                *m_device->graphics_queue().handle,
                m_device->graphics_queue().family_index,
                m_device->instance_dispatch().resolver,
            };
            m_ex_swapchain = info.ex_swapchain_factory(h);
            if (! m_ex_swapchain) {
                rstd_error("ex_swapchain_factory returned null");
                return false;
            }
        } else {
            m_ex_swapchain = m_rendering_resources.resources.CreateLocalSwapchain(
                *m_device,
                extent.width,
                extent.height,
                (info.offscreen_tiling == TexTiling::OPTIMAL ? VK_IMAGE_TILING_OPTIMAL
                                                             : VK_IMAGE_TILING_LINEAR));
        }
        m_with_surface = false;
    }

    {
        auto resources_span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::vulkan_resources);
        if (! initRes()) return false;
    }

    m_inited = true;
    return m_inited;
}

bool VulkanRender::Impl::initRes() {
    {
        auto& pool = m_device->cmd_pool();
        VVK_CHECK_BOOL_RE(
            pool.Allocate(usize(vk_command_num), VK_COMMAND_BUFFER_LEVEL_PRIMARY, m_cmds));
        m_upload_cmds.clear();
        m_upload_cmds.reserve(vk_upload_command_num);
        for (std::uint32_t i = 0; i < vk_upload_command_num; ++i) {
            m_upload_cmds.emplace_back(m_cmds[usize(i)], m_device->handle().Dispatch());
        }
        m_upload_cmd_values.assign(vk_upload_command_num, 0);
        m_next_upload_cmd = 0;
        m_render_cmd =
            vvk::CommandBuffer(m_cmds[usize(vk_upload_command_num)], m_device->handle().Dispatch());
    }
    if (! CreateRenderingResource(m_rendering_resources)) return false;

    return true;
}

void VulkanRender::Impl::destroy() {
    if (! m_inited) return;
    if (m_device->handle()) {
        VVK_CHECK(m_device->handle().WaitIdle());

        // res
        m_program.destroyPasses(*m_device);
        ReleaseCompletedRetiredResources(*m_device, m_rendering_resources);
        m_program.clear();
        m_rendering_resources.resources.Reset();

        m_device->Destroy();
    }
    m_instance.Destroy();
}

bool VulkanRender::Impl::CreateRenderingResource(RenderingResources& rr) {
    rr.command = m_render_cmd;
    VVK_CHECK_BOOL_RE(m_device->handle().CreateFence(
        VkFenceCreateInfo {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        },
        rr.fence_frame));

    rr.fence_frame.Reset();

    {
        VkSemaphoreTypeCreateInfo type_info {
            .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .pNext         = nullptr,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue  = 0,
        };
        VkSemaphoreCreateInfo ci {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &type_info,
            .flags = 0,
        };
        VVK_CHECK_BOOL_RE(m_device->handle().CreateSemaphore(ci, rr.sem_upload));
    }

    if (m_with_surface) {
        VkSemaphoreCreateInfo ci { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                   .pNext = nullptr };
        VVK_CHECK_BOOL_RE(m_device->handle().CreateSemaphore(ci, rr.sem_swap_wait_image));

        const std::size_t n_images = m_device->swapchain().images().size();
        m_sem_swap_finish_per_image.clear();
        m_sem_swap_finish_per_image.resize(n_images);
        for (auto& s : m_sem_swap_finish_per_image) {
            VVK_CHECK_BOOL_RE(m_device->handle().CreateSemaphore(ci, s));
        }
    }

    // Frame signal semaphore. On Linux it is created exportable (SYNC_FD) so
    // the waywallen-renderer host can ship a dma_fence sync_file to display
    // clients on each FrameReady event. macOS portability drivers don't
    // support fd export — there the semaphore is created WITHOUT the export
    // pNext: it still signals every frame submission (the surface path
    // depends on it), only the fd export stays unavailable.
    {
#if __is_target_os(macos)
        VkSemaphoreCreateInfo ci {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        };
        VkExportSemaphoreCreateInfo export_info {
            .sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
            .pNext       = nullptr,
            .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT_KHR,
        };
        if (m_device->handle().Dispatch().vkGetSemaphoreFdKHR != nullptr) {
            ci.pNext = &export_info;
        }
        VkResult vr = m_device->handle().CreateSemaphore(ci, rr.sem_export);
        if (vr != VK_SUCCESS) {
            rstd_error("vulkan: frame signal semaphore create failed (vr={})",
                       static_cast<int>(vr));
            return false;
        }
#else
        VkExportSemaphoreCreateInfo export_info {
            .sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
            .pNext       = nullptr,
            .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT_KHR,
        };
        VkSemaphoreCreateInfo ci {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &export_info,
            .flags = 0,
        };
        VVK_CHECK_BOOL_RE(m_device->handle().CreateSemaphore(ci, rr.sem_export));
#endif
    }

    rr.shader_reflection_cache = rstd::Some(rstd::mut_ref<ShaderReflectionCache>::from_raw_parts(
        rstd::addressof(m_shader_reflection_cache)));
    return true;
}

void VulkanRender::Impl::DestroyRenderingResource(RenderingResources&) {}

rstd::Option<std::size_t> VulkanRender::Impl::acquireUploadCommandSlot(RenderingResources& rr) {
    if (m_upload_cmds.empty()) return rstd::None();
    const std::size_t slot = m_next_upload_cmd;
    m_next_upload_cmd      = (m_next_upload_cmd + 1) % m_upload_cmds.size();

    const std::uint64_t wait_value = m_upload_cmd_values[slot];
    if (wait_value != 0) {
        std::uint64_t counter = 0;
        VVK_CHECK_ACT(return rstd::None(), rr.sem_upload.GetCounter(&counter));
        if (counter < wait_value) {
            const auto result = rr.sem_upload.Wait(wait_value, vk_wait_time);
            if (result == VK_TIMEOUT) return rstd::None();
            VVK_CHECK_ACT(return rstd::None(), result);
        }
        rr.resources.CompleteUploadsThrough(u64(wait_value));
        m_upload_cmd_values[slot] = 0;
    }
    return rstd::Some<std::size_t>(slot);
}

bool VulkanRender::Impl::commitPreparedUploads(SceneLoadBenchRecorderView load_bench) {
    auto upload_span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_upload_submit);
    if (! m_rendering_resources.resources.HasPendingUploads()) return true;
    auto slot = acquireUploadCommandSlot(m_rendering_resources);
    if (slot.is_none()) return false;
    auto committed =
        m_program.commitUploads(*m_device, m_rendering_resources, m_upload_cmds[*slot]);
    if (! committed.success) return false;
    if (committed.signal_value != u64()) {
        m_upload_cmd_values[*slot] = committed.signal_value.to_primitive();
    }
    return true;
}

bool VulkanRender::Impl::waitForPreparedUploads(RenderingResources& rr) {
    auto wait_span =
        SceneLoadSpan(m_pending_load_bench, &SceneLoadProbeIds::render_texture_upload_wait);
    auto pending = rr.resources.PendingUpload();
    if (pending.is_none()) {
        m_pending_load_bench = {};
        return true;
    }
    std::uint64_t counter = 0;
    VVK_CHECK_ACT(return false, rr.sem_upload.GetCounter(&counter));
    if (counter < pending->value.to_primitive()) {
        const auto result = rr.sem_upload.Wait(pending->value.to_primitive(), vk_wait_time);
        if (result == VK_TIMEOUT) return false;
        VVK_CHECK_ACT(return false, result);
    }
    rr.resources.CompleteUploadsThrough(pending->value);
    m_pending_load_bench = {};
    return true;
}

void VulkanRender::Impl::drawFrame(Scene& scene) {
    if (! (m_inited && m_program.loaded)) return;

    if (m_instance.offscreen()) {
        drawFrameOffscreen(scene);
    } else {
        drawFrameSwapchain(scene);
    }

    if (m_redraw_cb) m_redraw_cb();
}

void VulkanRender::Impl::drawFrameSwapchain(Scene& scene) {
    static std::size_t resource_index = 0;

    RenderingResources& rr = m_rendering_resources;
    // Failed preparation must not consume a swapchain image or signal its acquire semaphore.
    if (! waitForPreparedUploads(rr)) return;
    auto texture_frames = rstd::dyn<SceneTextureAnimationView>::from_ref(scene);
    if (! m_program.update(
            scene.Runtime().Frame(), m_device->out_extent(), texture_frames.as_ref(), rr))
        return;
    resource_index            = (resource_index + 1) % 3;
    std::uint32_t image_index = 0;
    {
        VVK_CHECK_VOID_RE(m_device->handle().AcquireNextImageKHR(*m_device->swapchain().handle(),
                                                                 vk_wait_time,
                                                                 *rr.sem_swap_wait_image,
                                                                 {},
                                                                 &image_index));
    }
    const auto& image          = m_device->swapchain().images()[image_index];
    const u64   acquire_serial = m_next_surface_acquire_serial++;
    if (acquire_serial == u64()) {
        rstd_error("window frame surface acquire serial overflow");
        return;
    }
    owe::FrameSurfaceLease frame_surface {
        .identity             = { .owner_generation = u64(1),
                                  .image_index      = u32(image_index),
                                  .acquire_serial   = acquire_serial },
        .reuse                = { .kind = owe::FrameSurfaceReuseKind::PresentationAcquired },
        .image                = image,
        .format               = m_device->swapchain().format(),
        .initial_layout       = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .initial_queue_family = m_device->graphics_queue().family_index,
        .acquire              = { .kind      = owe::FrameSurfaceAcquireKind::BinarySemaphore,
                                  .semaphore = *rr.sem_swap_wait_image },
        .final_layout         = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .final_queue_family   = m_device->present_queue().family_index,
        .discard_content      = true,
    };
    auto external_preparer =
        rstd::dyn<resource_registry::ExternalResourcePreparer>::from_ref(rr.resources);
    if (! m_finpass->setFrameSurface(std::move(frame_surface),
                                     external_preparer.as_mut_ref(),
                                     m_device->capabilities(),
                                     m_device->graphics_queue().family_index)) {
        rstd_error("window frame surface lease rejected");
        return;
    }
    (void)rr.command.Begin(VkCommandBufferBeginInfo {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    });
    RecordedBufferUploads recorded_uploads;
    if (! m_program.record(rr, recorded_uploads)) {
        (void)rr.command.End();
        return;
    }
    (void)rr.command.End();

    auto& sem_present_done = m_sem_swap_finish_per_image[image_index];

    // Surface-mode FinPass writes the acquired image as a color attachment;
    // the acquire wait therefore has to cover COLOR_ATTACHMENT_OUTPUT. The
    // offscreen transfer path does not use this submission branch.
    auto                        pending_upload = rr.resources.PendingUpload();
    const bool                  wait_upload    = pending_upload.is_some();
    rstd::array<VkSemaphore, 2> wait_semaphores {
        *rr.sem_swap_wait_image,
        *rr.sem_upload,
    };
    rstd::array<VkPipelineStageFlags, 2> wait_stages {
#if __is_target_os(macos)
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
#else
        VK_PIPELINE_STAGE_TRANSFER_BIT,
#endif
        vk_upload_wait_stages,
    };
    rstd::array<std::uint64_t, 2> wait_values {
        std::uint64_t { 0 },
        wait_upload ? pending_upload->value.to_primitive() : std::uint64_t { 0 },
    };
    rstd::array<std::uint64_t, 1> signal_values { std::uint64_t { 0 } };
    VkTimelineSemaphoreSubmitInfo timeline_info {
        .sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .pNext                     = nullptr,
        .waitSemaphoreValueCount   = wait_upload ? 2u : 0u,
        .pWaitSemaphoreValues      = wait_upload ? wait_values.data() : nullptr,
        .signalSemaphoreValueCount = wait_upload ? 1u : 0u,
        .pSignalSemaphoreValues    = wait_upload ? signal_values.data() : nullptr,
    };
    VkSubmitInfo sub_info {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext                = wait_upload ? &timeline_info : nullptr,
        .waitSemaphoreCount   = wait_upload ? 2u : 1u,
        .pWaitSemaphores      = wait_semaphores.data(),
        .pWaitDstStageMask    = wait_stages.data(),
        .commandBufferCount   = 1,
        .pCommandBuffers      = rr.command.address(),
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = sem_present_done.address(),
    };

    VVK_CHECK_VOID_RE(m_device->present_queue().handle.Submit(sub_info, *rr.fence_frame));
    auto submission_completion = rr.resources.BeginSubmission(rstd::move(recorded_uploads));
    if (! submission_completion.Valid()) {
        rstd_error("track frame submission failed");
    }
    VkPresentInfoKHR present_info {
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext              = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = sem_present_done.address(),
        .swapchainCount     = 1,
        .pSwapchains        = m_device->swapchain().handle().address(),
        .pImageIndices      = &image_index,
    };
    VVK_CHECK_VOID_RE(m_device->present_queue().handle.Present(present_info));

    VVK_CHECK_VOID_RE(rr.fence_frame.Wait(vk_wait_time));
    if (submission_completion.Valid() &&
        rr.resources.CompleteSubmission(submission_completion).is_some()) {
        ReleaseCompletedRetiredResources(*m_device, rr);
    }
    if (pending_upload.is_some()) {
        rr.resources.CompleteUploadsThrough(pending_upload->value);
    }
    VVK_CHECK_VOID_RE(rr.fence_frame.Reset());
}
void VulkanRender::Impl::drawFrameOffscreen(Scene& scene) {
    if (! m_ex_swapchain) return;

    // Drain any pending bridge directive *before* committing to a slot.
    // Previous frame's GPU work has fenced at the tail of the last
    // drawFrameOffscreen, so the cmd pool is idle.
    m_ex_swapchain->poll();

    // Skip until both the swapchain has slots and the scene has loaded
    // (FinPass.prepare runs from compileRenderGraph). FinPass itself is
    // format-agnostic now — vkCmdBlitImage handles cross-format channel
    // mapping, no rebuild needed on renegotiation.
    if (! m_ex_swapchain->ready() || ! m_finpass->prepared()) {
        return;
    }

    RenderingResources& rr                    = m_rendering_resources;
    auto                frame_surface_acquire = m_ex_swapchain->acquireRenderTarget();
    if (! frame_surface_acquire.acquired()) {
        if (frame_surface_acquire.status == owe::FrameSurfaceAcquireStatus::ProtocolError) {
            rstd_error("offscreen frame surface acquisition failed: {}",
                       frame_surface_acquire.error_code);
        }
        return;
    }

    auto external_preparer =
        rstd::dyn<resource_registry::ExternalResourcePreparer>::from_ref(rr.resources);
    if (! m_finpass->setFrameSurface(frame_surface_acquire.lease,
                                     external_preparer.as_mut_ref(),
                                     m_device->capabilities(),
                                     m_device->graphics_queue().family_index)) {
        rstd_error("offscreen frame surface lease rejected");
        return;
    }
    if (! waitForPreparedUploads(rr)) return;
    auto texture_frames = rstd::dyn<SceneTextureAnimationView>::from_ref(scene);
    if (! m_program.update(
            scene.Runtime().Frame(), m_device->out_extent(), texture_frames.as_ref(), rr))
        return;

    (void)rr.command.Begin(VkCommandBufferBeginInfo {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    });
    RecordedBufferUploads recorded_uploads;
    if (! m_program.record(rr, recorded_uploads)) {
        (void)rr.command.End();
        return;
    }

    (void)rr.command.End();

    auto                        pending_upload = rr.resources.PendingUpload();
    const bool                  wait_upload    = pending_upload.is_some();
    rstd::array<VkSemaphore, 1> wait_semaphores {
        *rr.sem_upload,
    };
    rstd::array<VkPipelineStageFlags, 1> wait_stages {
        vk_upload_wait_stages,
    };
    rstd::array<std::uint64_t, 1> wait_values {
        wait_upload ? pending_upload->value.to_primitive() : std::uint64_t { 0 },
    };
    rstd::array<std::uint64_t, 1> signal_values { std::uint64_t { 0 } };
    VkTimelineSemaphoreSubmitInfo timeline_info {
        .sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .pNext                     = nullptr,
        .waitSemaphoreValueCount   = wait_upload ? 1u : 0u,
        .pWaitSemaphoreValues      = wait_upload ? wait_values.data() : nullptr,
        .signalSemaphoreValueCount = wait_upload ? 1u : 0u,
        .pSignalSemaphoreValues    = wait_upload ? signal_values.data() : nullptr,
    };
    VkSubmitInfo sub_info {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext                = wait_upload ? &timeline_info : nullptr,
        .waitSemaphoreCount   = wait_upload ? 1u : 0u,
        .pWaitSemaphores      = wait_upload ? wait_semaphores.data() : nullptr,
        .pWaitDstStageMask    = wait_upload ? wait_stages.data() : nullptr,
        .commandBufferCount   = 1,
        .pCommandBuffers      = rr.command.address(),
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = rr.sem_export.address(),
    };
    VVK_CHECK_VOID_RE(m_device->graphics_queue().handle.Submit(sub_info, *rr.fence_frame));
    auto submission_completion = rr.resources.BeginSubmission(rstd::move(recorded_uploads));
    if (! submission_completion.Valid()) {
        rstd_error("track offscreen submission failed");
    }

    VVK_CHECK_VOID_RE(rr.fence_frame.Wait(vk_wait_time));
    if (submission_completion.Valid() &&
        rr.resources.CompleteSubmission(submission_completion).is_some()) {
        ReleaseCompletedRetiredResources(*m_device, rr);
    }
    if (pending_upload.is_some()) {
        rr.resources.CompleteUploadsThrough(pending_upload->value);
    }
    VVK_CHECK_VOID_RE(rr.fence_frame.Reset());

    // Export the signaled semaphore as a dma_fence sync_file fd and hand
    // it to the swapchain along with the slot. LocalExSwapchain stashes
    // it for the host's takeLastFrameSyncFd; BridgeExSwapchain forwards
    // it to ww_bridge_pool_submit_slot.
    //
    // Diagnostics: sync_fd export failure is silent in production but
    // the result is the consumer reading a buffer the producer hasn't
    // finished writing — exactly the "blank frame" symptom. We log the
    // first failure loudly and rate-limit subsequent ones.
    int sync_fd = -1;
    {
        VkSemaphoreGetFdInfoKHR gi {
            .sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            .pNext      = nullptr,
            .semaphore  = *rr.sem_export,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT_KHR,
        };
        VkResult vr = m_device->handle().GetSemaphoreFdKHR(gi, &sync_fd);
        if (vr != VK_SUCCESS) {
            static std::atomic<std::uint64_t> n_failed { 0 };
            std::uint64_t                     k = n_failed.fetch_add(1, std::memory_order_relaxed);
            if (k == 0 || (k & (k - 1)) == 0) { // 1st, 2nd, 4th, 8th...
                rstd_error("VulkanRender: vkGetSemaphoreFdKHR failed (vr={}, count={})",
                           (int)vr,
                           (unsigned long long)(k + 1));
            }
            sync_fd = -1;
        } else if (sync_fd < 0) {
            // The driver returned VK_SUCCESS with fd=-1 — spec says this
            // means "the semaphore was unsignaled". Should not happen
            // because we just waited the fence; flag loudly.
            static std::atomic<std::uint64_t> n_unsig { 0 };
            std::uint64_t                     k = n_unsig.fetch_add(1, std::memory_order_relaxed);
            if (k == 0 || (k & (k - 1)) == 0) {
                rstd_error("VulkanRender: GetSemaphoreFdKHR returned fd=-1 "
                           "(semaphore not signaled? count={})",
                           (unsigned long long)(k + 1));
            }
        }
    }

    auto completion = frame_surface_acquire.completion.Submit(sync_fd);
    if (completion.status != owe::FrameSurfaceCompletionStatus::Submitted &&
        completion.status != owe::FrameSurfaceCompletionStatus::SessionLost) {
        rstd_error("offscreen frame surface completion failed: status={}, error={}",
                   static_cast<int>(completion.status),
                   completion.error_code);
    }
}

bool VulkanRender::Impl::onSwapchainReady(unsigned width, unsigned height) {
    if (! m_inited) return false;
    auto& cur            = m_device->out_extent();
    bool  extent_changed = (width != cur.width) || (height != cur.height);
    if (! extent_changed) {
        // Format-only changes flow through ExSwapchain::format() and
        // are handled by drawFrameOffscreen's head check.
        return false;
    }
    if (! waitForPreparedUploads(m_rendering_resources)) return false;
    m_device->set_out_extent(VkExtent2D { width, height });
    return true;
}

void VulkanRender::Impl::UpdateCameraFillMode(owe::Scene& scene, owe::FillMode fillmode) {
    using namespace owe;
    auto width  = m_device->out_extent().width;
    auto height = m_device->out_extent().height;

    if (width == 0) return;
    auto   projection_extent = scene.OrthographicProjectionExtent();
    double sw = projection_extent[usize()], sh = projection_extent[usize(1)];
    double fboAspect = width / (double)height, sAspect = sw / sh;
    auto   global      = scene.CameraMut("global"_str);
    auto   perspective = scene.CameraMut("global_perspective"_str);
    if (global.is_none() || perspective.is_none()) return;
    auto& gCam    = **global;
    auto& gPerCam = **perspective;
    // assum cam
    switch (fillmode) {
    case FillMode::STRETCH:
        gCam.SetWidth(sw);
        gCam.SetHeight(sh);
        gPerCam.SetAspect(sAspect);
        if (! gPerCam.IsLookAt())
            gPerCam.SetFov(algorism::CalculatePersperctiveFov(1000.0f, gCam.Height()));
        break;
    case FillMode::ASPECTFIT:
        if (fboAspect < sAspect) {
            // scale height
            gCam.SetWidth(sw);
            gCam.SetHeight(sw / fboAspect);
        } else {
            gCam.SetWidth(sh * fboAspect);
            gCam.SetHeight(sh);
        }
        gPerCam.SetAspect(fboAspect);
        if (! gPerCam.IsLookAt())
            gPerCam.SetFov(algorism::CalculatePersperctiveFov(1000.0f, gCam.Height()));
        break;
    case FillMode::ASPECTCROP:
    default:
        if (fboAspect > sAspect) {
            // scale height
            gCam.SetWidth(sw);
            gCam.SetHeight(sw / fboAspect);
        } else {
            gCam.SetWidth(sh * fboAspect);
            gCam.SetHeight(sh);
        }
        gPerCam.SetAspect(fboAspect);
        if (! gPerCam.IsLookAt())
            gPerCam.SetFov(algorism::CalculatePersperctiveFov(1000.0f, gCam.Height()));
        break;
    }
    gCam.Update();
    gPerCam.Update();
    scene.UpdateLinkedCamera("global"_str);
    scene.CaptureCameraPathViewports();
}

void VulkanRender::Impl::clearLastRenderGraph(RenderGraphResourceRetention retention) {
    m_program.destroyPasses(*m_device);
    ReleaseCompletedRetiredResources(*m_device, m_rendering_resources);
    m_program.clear();
    m_rendering_resources.resources.ClearPreparedGraphics();
    if (retention == RenderGraphResourceRetention::ReleaseSceneTextures) {
        m_rendering_resources.resources.ClearTextures();
        m_shader_reflection_cache.Clear();
    } else {
        m_rendering_resources.resources.ClearTransientTextures();
    }
    m_rendering_resources.resources.EvictUnusedBuffers();
}

void VulkanRender::Impl::configureRenderTargets(Scene& scene) {
    if (! m_inited) return;
    const auto&      limits = m_device->limits();
    const VkExtent2D max_framebuffer_extent {
        std::min(limits.maxImageDimension2D, limits.maxFramebufferWidth),
        std::min(limits.maxImageDimension2D, limits.maxFramebufferHeight),
    };
    m_program.finalizeRenderTargetSizes(
        scene, m_device->out_extent(), max_framebuffer_extent, m_msaa_samples);
}

void VulkanRender::Impl::compileRenderGraph(Scene& scene, rg::RenderGraph& rg) {
    auto render_scene = ExtractRenderSceneSnapshot(scene);
    compileRenderGraph(scene, rg, render_scene);
}

void VulkanRender::Impl::compileRenderGraph(Scene& scene, rg::RenderGraph& rg,
                                            const RenderSceneSnapshot& render_scene) {
    compileRenderGraph(scene, rg, render_scene, {});
}

bool VulkanRender::Impl::prepareProgram(Scene& scene, const RenderSceneSnapshot& render_scene,
                                        resource::ResourcePlanSections sections,
                                        SceneLoadBenchRecorderView     load_bench) {
    // Prepared refresh replaces binding owners before recreating equivalent bindings.
    [[maybe_unused]] auto audio_demand_reconciliation =
        scene.AudioDemandMut()->BeginReconciliation();
    auto status = m_program.beginPrepare(
        scene, *m_device, m_rendering_resources, render_scene, sections, load_bench);
    while (status == RenderProgramPrepareStatus::BatchReady) {
        if (! commitPreparedUploads(load_bench)) {
            m_program.abortPrepare(m_rendering_resources);
            return false;
        }
        status = m_program.continuePrepare(scene, *m_device, m_rendering_resources, load_bench);
    }
    if (status == RenderProgramPrepareStatus::Failed) {
        m_program.abortPrepare(m_rendering_resources);
        return false;
    }

    {
        auto scopes_span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_scopes);
        m_program.rebuildScopes();
    }
    if (! commitPreparedUploads(load_bench)) {
        m_program.abortPrepare(m_rendering_resources);
        return false;
    }
    m_rendering_resources.resources.CommitPreparePlan();
    m_program.loaded = true;
    return true;
}

void VulkanRender::Impl::compileRenderGraph(Scene& scene, rg::RenderGraph& rg,
                                            const RenderSceneSnapshot& render_scene,
                                            SceneLoadBenchRecorderView load_bench) {
    if (! m_inited) return;
    m_pending_load_bench = load_bench;
    auto compile_span    = SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_graph_compile);
    m_program.loaded     = false;

    {
        auto program_span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_program_build);
        if (! m_program.buildFromGraph(rg)) {
            rstd_error("compile render graph failed: dependency cycle");
            return;
        }
        m_program.injectFramePasses(*m_prepass, *m_finpass);
    }

    {
        auto requests_span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_requests);
        configureRenderTargets(scene);
        m_program.finalizeFramePassRequests(scene);
        m_program.finalizeResourceRequests(scene);
    }
    (void)prepareProgram(scene, render_scene, resource::ResourcePlanAll, load_bench);
};

void VulkanRender::Impl::refreshPreparedResources(Scene& scene) {
    auto render_scene = ExtractRenderSceneSnapshot(scene);
    refreshPreparedResources(scene, render_scene);
}

void VulkanRender::Impl::refreshPreparedResources(Scene&                     scene,
                                                  const RenderSceneSnapshot& render_scene) {
    refreshPreparedResources(scene, render_scene, resource::ResourcePlanAll);
}

void VulkanRender::Impl::refreshPreparedTextures(Scene&                     scene,
                                                 const RenderSceneSnapshot& render_scene) {
    refreshPreparedResources(scene, render_scene, resource::ResourcePlanTextures);
}

void VulkanRender::Impl::refreshPreparedResources(Scene&                         scene,
                                                  const RenderSceneSnapshot&     render_scene,
                                                  resource::ResourcePlanSections sections) {
    if (! m_inited || m_program.pass_records.is_empty()) return;

    configureRenderTargets(scene);
    m_program.finalizeFramePassRequests(scene);
    m_program.finalizeResourceRequests(scene);
    (void)prepareProgram(scene, render_scene, sections);
}

void VulkanRender::Impl::invalidatePreparedRenderItems(slice<owe::RenderItemId> render_items,
                                                       PassInvalidationFlags    flags) {
    if (! m_inited) return;
    m_program.invalidateRenderItems(render_items, flags);
}

void VulkanRender::Impl::refreshPreparedRenderItems(Scene&                     scene,
                                                    const RenderSceneSnapshot& render_scene,
                                                    slice<owe::RenderItemId>   render_items,
                                                    PassInvalidationFlags      flags) {
    invalidatePreparedRenderItems(render_items, flags);
    refreshPreparedResources(scene, render_scene);
}

void VulkanRender::Impl::refreshPreparedMaterial(Scene&                     scene,
                                                 const RenderSceneSnapshot& render_scene,
                                                 owe::SceneMaterialId       material,
                                                 PassInvalidationFlags      flags) {
    refreshPreparedRenderItems(scene, render_scene, render_scene.renderItemsFor(material), flags);
}

bool VulkanRender::Impl::refreshPreparedMaterialTextures(Scene&                     scene,
                                                         const RenderSceneSnapshot& render_scene,
                                                         owe::SceneMaterialId       material) {
    rstd::array<owe::SceneMaterialId, 1> materials { material };
    return refreshPreparedMaterialTextures(scene, render_scene, materials.as_slice());
}

bool VulkanRender::Impl::refreshPreparedMaterialTextures(Scene&                      scene,
                                                         const RenderSceneSnapshot&  render_scene,
                                                         slice<owe::SceneMaterialId> materials) {
    if (! m_inited || m_program.pass_records.is_empty()) return true;
    auto render_items = RenderItemsForMaterials(render_scene, materials);
    bool requires_graph_rebuild =
        m_program.refreshMaterialTextureBindings(render_scene, render_items.as_slice());
    if (requires_graph_rebuild) return false;
    refreshPreparedResources(scene, render_scene);
    return true;
}

void VulkanRender::Impl::refreshPreparedMesh(Scene& scene, const RenderSceneSnapshot& render_scene,
                                             owe::SceneMeshId mesh, PassInvalidationFlags flags) {
    refreshPreparedRenderItems(scene, render_scene, render_scene.renderItemsFor(mesh), flags);
}

std::vector<PreparedPassDiagnostic> VulkanRender::Impl::preparedPassDiagnostics() const {
    return m_program.diagnostics();
}
