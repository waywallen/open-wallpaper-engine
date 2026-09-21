module;

export module wescene.vulkan_render;
import wescene.types;
import rstd;
import wescene.load_bench;
import wescene.vulkan;
import wescene.scene;
import wescene.resource_registry;

import wescene.rgraph;

export import :vulkan_pass;
export import :program;
export import :pipeline_layout;
export import :shader_reflection_cache;
export import :uniform_buffer;
export import :resource;
export import :buffer_resolver;
export import :pass_common;
export import :copy_pass;
export import :custom_shader_pass;
export import :fin_pass;
export import :pre_pass;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace owe
{

using ReDrawCB              = Option<Box<dyn<FnMut<void()>>>>;
using CreateSurfaceCallback = Box<dyn<FnOnce<VkResult(VkInstance, VkSurfaceKHR*)>>>;

struct VulkanSurfaceInfo {
    Option<CreateSurfaceCallback> createSurfaceOp;
    Vec<String>                   instanceExts;
};

struct RenderInitInfo {
    bool enable_valid_layer { false };
    bool offscreen { false };

    Option<array<rstd::uint8_t, VK_UUID_SIZE>> uuid;
    TexTiling                                  offscreen_tiling { TexTiling::OPTIMAL };
    /* When true, allocate the offscreen ExSwapchain images out of
     * HOST_VISIBLE && !DEVICE_LOCAL (true GTT) so the exported dmabuf
     * fds are importable by a foreign GPU (cross-GPU PRIME). Ignored
     * when offscreen == false. */
    bool              offscreen_host_visible { false };
    VulkanSurfaceInfo surface_info;

    rstd::uint16_t width { 1920 };
    rstd::uint16_t height { 1080 };
    String         video_hwdec { "auto"_Str };
    String         video_render_node;
    // MSAA samples for the screen RT only. 1 disables. Clamped down to
    // device's framebufferColorSampleCounts at init.
    rstd::uint32_t msaa_samples { 1 };
    ReDrawCB       redraw_callback;

    /* When set AND `offscreen == true`, VulkanRender invokes this factory
     * after picking the GPU and creating the VkDevice, and adopts the
     * returned swapchain instead of allocating its own LocalExSwapchain.
     * Used by the waywallen-wescene host to construct a BridgeExSwapchain
     * around a ww_bridge_pool created with the just-picked device.
     * Ignored on the on-screen path. */
    struct ExSwapchainHandles {
        VkInstance       instance;
        VkPhysicalDevice physical_device;
        VkDevice         device;
        VkQueue          graphics_queue;
        rstd::uint32_t   graphics_queue_family;
        // Borrowed from the renderer; valid for the external swapchain lifetime.
        PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    };
    Option<Box<dyn<FnOnce<Option<ExSwapchainOwner>(const ExSwapchainHandles&)>>>>
        ex_swapchain_factory;
};

Box<rg::RenderGraph> sceneToRenderGraph(Scene&);
Box<rg::RenderGraph> sceneToRenderGraph(Scene&, const RenderSceneSnapshot&);

namespace vulkan
{

class FinPass;

enum class RenderGraphResourceRetention
{
    KeepSceneTextures,
    ReleaseSceneTextures,
};

class VulkanRender {
public:
    VulkanRender();
    ~VulkanRender();

    bool init(RenderInitInfo, SceneLoadBenchRecorderView load_bench = {});

    void destroy();

    void drawFrame(Scene&);

    void clearLastRenderGraph(
        RenderGraphResourceRetention retention = RenderGraphResourceRetention::KeepSceneTextures);
    void configureRenderTargets(Scene&);
    void compileRenderGraph(Scene&, rg::RenderGraph&);
    void compileRenderGraph(Scene&, rg::RenderGraph&, const RenderSceneSnapshot&);
    void compileRenderGraph(Scene&, rg::RenderGraph&, const RenderSceneSnapshot&,
                            SceneLoadBenchRecorderView);
    void refreshPreparedResources(Scene&);
    void refreshPreparedResources(Scene&, const RenderSceneSnapshot&);
    void refreshPreparedTextures(Scene&, const RenderSceneSnapshot&);
    void invalidatePreparedRenderItems(slice<RenderItemId>, PassInvalidationFlags);
    void refreshPreparedRenderItems(Scene&, const RenderSceneSnapshot&, slice<RenderItemId>,
                                    PassInvalidationFlags);
    void refreshPreparedMaterial(Scene&, const RenderSceneSnapshot&, SceneMaterialId,
                                 PassInvalidationFlags);
    bool refreshPreparedMaterialTextures(Scene&, const RenderSceneSnapshot&, SceneMaterialId);
    bool refreshPreparedMaterialTextures(Scene&, const RenderSceneSnapshot&,
                                         slice<SceneMaterialId>);
    void refreshPreparedMesh(Scene&, const RenderSceneSnapshot&, SceneMeshId,
                             PassInvalidationFlags);
    Vec<PreparedPassDiagnostic> preparedPassDiagnostics() const;
    // Free buffer generations no longer referenced by prepared work.
    void evictUnusedMeshes();
    void UpdateCameraFillMode(Scene&, owe::FillMode);

    bool onSwapchainReady(unsigned width, unsigned height);

    ExSwapchain* exSwapchain() const;
    bool         inited() const;

    int takeLastFrameSyncFd();

    bool               getDrmRenderNode(rstd::uint32_t& out_major, rstd::uint32_t& out_minor) const;
    DeviceCapabilities deviceCapabilities() const;

    /* Tick all registered video-tex decoders. No-op when no scene
     * texture has been recognised as a VIDEO container. Invoked from
     * SceneWallpaper's per-frame RenderDraw handler. */
    void pumpVideoTextures(double dt_seconds);

    // Uploads dirty atlas regions; retains them until the upload succeeds.
    void pumpFontAtlases(Scene& scene);

    VkInstance       vkInstance() const;
    VkPhysicalDevice vkPhysicalDevice() const;
    VkDevice         vkDevice() const;
    VkQueue          vkGraphicsQueue() const;
    rstd::uint32_t   vkGraphicsQueueFamily() const;

    void deviceUuid(rstd::uint8_t out[16]) const;
    void driverUuid(rstd::uint8_t out[16]) const;

private:
    struct Impl;
    Box<Impl> pImpl;
};

} // namespace vulkan
} // namespace owe
