module;

struct GLFWwindow;

export module viewer.web:vulkan_blitter;

import rstd.cppstd;
import weweb;
import vvk;
import :presenter;

export namespace weweb
{

// Vulkan presenter for CEF accelerated-paint frames.
//
// AcceptDmaBuf imports a CEF DMA-BUF as a temporary linear VkImage,
// vkCmdCopyImage's it into a persistent device-local "owned" VkImage,
// waits on a fence (CEF reclaims the buffer the moment the callback
// returns), and tears down the temporaries.
//
// On macOS, AcceptCpuPaint copies CEF's OnPaint rows into bounded host storage
// and the render thread uploads them into the same owned image.
//
// RenderFrame vkCmdBlitImage's owned → swapchain image and presents.
//
// No render pass / pipeline / shaders.
class VulkanBlitter : public Presenter {
public:
    VulkanBlitter();
    ~VulkanBlitter() override;

    VulkanBlitter(const VulkanBlitter&)            = delete;
    VulkanBlitter& operator=(const VulkanBlitter&) = delete;

    bool Init(GLFWwindow* window) override;
    void Shutdown() override;

    std::uint32_t Width() const override { return extent_.width; }
    std::uint32_t Height() const override { return extent_.height; }

    bool Resize() override;
    bool AcceptDmaBuf(const DmaBufFrame& frame) override;
    bool AcceptCpuPaint(const CpuPaintFrame& frame) override;
    bool RenderFrame() override;

private:
    bool CreateInstance();
    bool CreateSurface(GLFWwindow* window);
    bool PickPhysicalDevice();
    bool CreateDevice();
    bool CreateSwapchain();
    bool CreateCommandPool();
    bool CreateSyncObjects();

#if __is_target_os(macos)
    bool EnsureCpuStaging(std::size_t size);
    void DestroyCpuStaging();
    bool UploadPendingCpuPaint();
    bool CopyCpuStagingToOwned(int width, int height);
#endif

    bool EnsureOwnedImage(int width, int height);
    void DestroyOwnedImage();

    void DestroySwapchain();

    std::uint32_t FindMemoryType(std::uint32_t type_bits, VkMemoryPropertyFlags props) const;

    // Instance / device.
    rstd::Option<vvk::VulkanLoader>  loader_;
    vvk::InstanceDispatch            instance_dispatch_ {};
    vvk::DeviceDispatch              device_dispatch_ {};
    vvk::Instance                    instance_owner_;
    vvk::Device                      device_owner_;
    VkInstance                       instance_ { VK_NULL_HANDLE };
    VkSurfaceKHR                     surface_ { VK_NULL_HANDLE };
    VkPhysicalDevice                 phys_ { VK_NULL_HANDLE };
    VkPhysicalDeviceMemoryProperties mem_props_ {};
    VkDevice                         device_ { VK_NULL_HANDLE };
    std::uint32_t                    queue_family_ { 0 };
    VkQueue                          queue_ { VK_NULL_HANDLE };

    // Swapchain.
    VkSwapchainKHR                 swapchain_ { VK_NULL_HANDLE };
    VkFormat                       swap_format_ { VK_FORMAT_UNDEFINED };
    VkColorSpaceKHR                swap_colorspace_ { VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    VkPresentModeKHR               present_mode_ { VK_PRESENT_MODE_FIFO_KHR };
    VkExtent2D                     extent_ { 0, 0 };
    std::vector<VkImage>           swap_images_;
    std::uint32_t                  frame_index_ { 0 };
    static constexpr std::uint32_t kMaxFramesInFlight = 2;

    // Owned device-local image — CEF DMA-BUFs get copied into this. The
    // swapchain blit reads from here. Recreated when CEF's coded size
    // changes.
    VkImage        owned_image_ { VK_NULL_HANDLE };
    VkDeviceMemory owned_image_mem_ { VK_NULL_HANDLE };
    VkImageLayout  owned_layout_ { VK_IMAGE_LAYOUT_UNDEFINED };
    int            owned_width_ { 0 };
    int            owned_height_ { 0 };
    bool           owned_has_data_ { false };

#if __is_target_os(macos)
    bool portability_subset_supported_ { false };

    VkBuffer               cpu_staging_ { VK_NULL_HANDLE };
    VkDeviceMemory         cpu_staging_mem_ { VK_NULL_HANDLE };
    void*                  cpu_staging_mapped_ { nullptr };
    VkDeviceSize           cpu_staging_size_ { 0 };
    bool                   cpu_staging_coherent_ { false };
    std::vector<std::byte> cpu_paint_data_;
    std::mutex             cpu_paint_mutex_;
    int                    cpu_paint_width_ { 0 };
    int                    cpu_paint_height_ { 0 };
    bool                   cpu_paint_pending_ { false };
#endif

    // Commands + sync. One per in-flight frame.
    VkCommandPool   cmd_pool_ { VK_NULL_HANDLE };
    VkCommandBuffer cmd_bufs_[kMaxFramesInFlight] { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkSemaphore     img_avail_sem_[kMaxFramesInFlight] { VK_NULL_HANDLE, VK_NULL_HANDLE };
    // Per-swapchain-image render-done semaphore, indexed by acquired
    // image index. Reusing one binary semaphore across frames violates
    // VUID-vkQueueSubmit-pSignalSemaphores-00067 because vkQueuePresentKHR
    // keeps the wait semaphore busy until the image is re-acquired.
    std::vector<VkSemaphore> render_done_sem_;
    VkFence in_flight_fence_[kMaxFramesInFlight] { VK_NULL_HANDLE, VK_NULL_HANDLE };

    GLFWwindow* window_ { nullptr };
};

} // namespace weweb
