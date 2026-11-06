module;

export module wescene.vulkan_render;
import wescene.types;
import rstd;
import wescene.load_bench;
import wescene.vulkan;
import wescene.scene;
import wescene.resource_registry;

export import :scene_render_resources;
export import :scene_render_state;
export import :scene_render_state_input;
export import :pipeline_layout;
export import :shader_reflection_cache;
export import :uniform_buffer;
export import :resource;
export import :buffer_resolver;
export import :scene_draw_policy;
export import :scene_draw;
export import :prepare_error;

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
    vrento::vulkan::PipelinePreparationMode pipeline_preparation {
        vrento::vulkan::PipelinePreparationMode::Synchronous
    };
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

auto BuildSceneRenderState(Scene&) -> Result<vulkan::SceneRenderStateInput, vrento::fg::Error>;
auto BuildSceneRenderState(Scene&, const RenderSceneSnapshot&,
                           vrento::fg::AnalysisStatistics* = nullptr)
    -> Result<vulkan::SceneRenderStateInput, vrento::fg::Error>;

namespace vulkan
{

enum class RenderGraphResourceRetention
{
    KeepSceneTextures,
    ReleaseSceneTextures,
};

struct RenderItemPreparation {
    RenderItemId          item;
    PassInvalidationFlags flags { PassInvalidationNone };
};
struct ResourcePreparation {
    slice<RenderItemPreparation> items;
    slice<SceneMaterialId>       texture_materials;
    bool                         textures_only {};
};

struct RenderDiagnostics {
    vrento::vulkan::CompileStatus                       compilation;
    vrento::vulkan::RendererStatistics                  renderer;
    vrento::resource_registry::ResourcePrepareStatus    preparation;
    vrento::resource_registry::ResourceStatistics       resources;
    vrento::resource_registry::PipelineCacheStats       pipelines;
    Option<vrento::vulkan::FrameState>                  frame;
    Option<vrento::vulkan::GraphExecutionErrorSnapshot> frame_error;
};

class VulkanRender {
public:
    VulkanRender();
    ~VulkanRender();

    bool hasSceneRenderState() const;
    bool init(RenderInitInfo, SceneLoadBenchRecorderView load_bench = {});
    auto tryClearLastRenderGraph(RenderGraphResourceRetention)
        -> Result<PrepareOutcome, RenderPrepareError>;
    auto tryInstallSceneRenderState(Scene&, vulkan::SceneRenderStateInput&&,
                                    const RenderSceneSnapshot&, SceneLoadBenchRecorderView = {})
        -> Result<PrepareOutcome, RenderPrepareError>;
    auto tryPrepareResources(Scene&, const RenderSceneSnapshot&, const ResourcePreparation&)
        -> Result<PrepareOutcome, RenderPrepareError>;
    auto tryPumpFontAtlases(Scene&) -> Result<PrepareOutcome, RenderPrepareError>;
    auto tryPumpVideoTextures(double) -> Result<PrepareOutcome, RenderPrepareError>;
    auto tryContinuePrepare(Scene&) -> Result<PrepareOutcome, RenderPrepareError>;

    void destroy();

    void drawFrame(Scene&);
    bool framePending() const;
    bool frameSubmitted() const;
    bool maintainResources();

    void configureRenderTargets(Scene&);
    void invalidatePreparedRenderItems(slice<RenderItemId>, PassInvalidationFlags);
    Vec<PreparedPassDiagnostic> preparedPassDiagnostics() const;
    auto                        diagnostics() const -> RenderDiagnostics;
    // Free buffer generations no longer referenced by prepared work.
    void evictUnusedMeshes();
    void UpdateCameraFillMode(Scene&, owe::FillMode);

    bool onSwapchainReady(unsigned width, unsigned height);

    ExSwapchain* exSwapchain() const;
    bool         inited() const;
    auto         deviceSession() const -> Result<vrento::vulkan::DeviceSession, RenderPrepareError>;
    auto         renderedTarget(ref<str>)
        -> Result<vrento::Preparation<vrento::texture::TextureViewRef>, RenderPrepareError>;

    int takeLastFrameSyncFd();

    bool               getDrmRenderNode(rstd::uint32_t& out_major, rstd::uint32_t& out_minor) const;
    DeviceCapabilities deviceCapabilities() const;

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
