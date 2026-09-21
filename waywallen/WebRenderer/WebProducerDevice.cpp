module;

#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

module waywallen.web_producer_device;

import rstd.cppstd;
import rstd;
import vvk;
import weweb;

namespace ww_wescene
{

namespace
{

#define VK_CHECK(expr)                                                            \
    do {                                                                          \
        VkResult _r = (expr);                                                     \
        if (_r != VK_SUCCESS) {                                                   \
            std::fprintf(stderr,                                                  \
                         "WebProducerDevice: %s failed (VkResult=%d) at %s:%d\n", \
                         #expr,                                                   \
                         static_cast<int>(_r),                                    \
                         __FILE__,                                                \
                         __LINE__);                                               \
            return false;                                                         \
        }                                                                         \
    } while (0)

constexpr uint64_t kFenceTimeoutNs = 5'000'000'000ull; // 5s

VkFormat FormatToVk(::weweb::DmaBufFormat f) {
    switch (f) {
    case ::weweb::DmaBufFormat::BGRA8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
    case ::weweb::DmaBufFormat::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    }
    return VK_FORMAT_B8G8R8A8_UNORM;
}

bool CopyCpuPaintToStaging(const ::weweb::CpuPaintFrame& frame, VkFormat slot_format, void* dst) {
    if (! frame.buffer || ! dst) return false;
    if (frame.width <= 0 || frame.height <= 0) return false;
    const uint32_t width      = static_cast<uint32_t>(frame.width);
    const uint32_t height     = static_cast<uint32_t>(frame.height);
    const uint32_t src_stride = frame.row_stride ? frame.row_stride : width * 4u;
    if (src_stride < width * 4u) return false;

    const auto* src      = static_cast<const uint8_t*>(frame.buffer);
    auto*       out      = static_cast<uint8_t*>(dst);
    const bool  src_bgra = frame.format == ::weweb::DmaBufFormat::BGRA8_UNORM;
    const bool  src_rgba = frame.format == ::weweb::DmaBufFormat::RGBA8_UNORM;
    const bool  dst_bgra = slot_format == VK_FORMAT_B8G8R8A8_UNORM;
    const bool  dst_rgba = slot_format == VK_FORMAT_R8G8B8A8_UNORM;
    if ((! src_bgra && ! src_rgba) || (! dst_bgra && ! dst_rgba)) return false;

    const uint32_t dst_stride = width * 4u;
    if ((src_bgra && dst_bgra) || (src_rgba && dst_rgba)) {
        for (uint32_t y = 0; y < height; ++y) {
            std::memcpy(out + y * dst_stride, src + y * src_stride, dst_stride);
        }
        return true;
    }

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = src + y * src_stride;
        uint8_t*       wr  = out + y * dst_stride;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t c0 = row[x * 4 + 0];
            const uint8_t c1 = row[x * 4 + 1];
            const uint8_t c2 = row[x * 4 + 2];
            const uint8_t c3 = row[x * 4 + 3];
            wr[x * 4 + 0]    = c2;
            wr[x * 4 + 1]    = c1;
            wr[x * 4 + 2]    = c0;
            wr[x * 4 + 3]    = c3;
        }
    }
    return true;
}

} // namespace

WebProducerDevice::WebProducerDevice() = default;
WebProducerDevice::~WebProducerDevice() { Shutdown(); }

void WebProducerDevice::SetRenderNode(const std::string& path) { render_node_ = path; }

bool WebProducerDevice::Init() {
    Shutdown();
    if (CreateInstance() && PickPhysicalDevice() && CreateDevice() && CreateCommandPool() &&
        CreateSyncObjects())
        return true;
    Shutdown();
    return false;
}

void WebProducerDevice::Shutdown() {
    if (device_) device_dispatch_.vkDeviceWaitIdle(device_);

    DestroyCpuUploadResources();

    if (blit_sem_) {
        device_dispatch_.vkDestroySemaphore(device_, blit_sem_, nullptr);
        blit_sem_ = VK_NULL_HANDLE;
    }
    if (blit_fence_) {
        device_dispatch_.vkDestroyFence(device_, blit_fence_, nullptr);
        blit_fence_ = VK_NULL_HANDLE;
    }
    if (cmd_pool_) {
        device_dispatch_.vkDestroyCommandPool(device_, cmd_pool_, nullptr);
        cmd_pool_ = VK_NULL_HANDLE;
        blit_cmd_ = VK_NULL_HANDLE;
    }
    allocator_.reset();
    device_owner_.reset();
    device_ = VK_NULL_HANDLE;
    queue_  = VK_NULL_HANDLE;
    instance_owner_.reset();
    instance_          = VK_NULL_HANDLE;
    phys_              = VK_NULL_HANDLE;
    device_dispatch_   = {};
    instance_dispatch_ = {};
    loader_            = rstd::None();
}

bool WebProducerDevice::CreateInstance() {
    VkApplicationInfo app {};
    app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName   = "waywallen-weweb-renderer";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName        = "weweb-producer";
    app.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
    // 1.1 promotes external_memory_capabilities + external_semaphore
    // into core, so no instance extension is required.
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci {};
    ci.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;

    auto loaded = vvk::VulkanLoader::Open();
    if (loaded.is_err()) {
        std::fprintf(stderr, "WebProducerDevice: Vulkan loader open failed\n");
        return false;
    }
    loader_      = rstd::Some(loaded.unwrap_unchecked());
    auto created = vvk::Instance::Create(
        instance_owner_, loader_.as_ref().unwrap_unchecked().global(), ci, instance_dispatch_);
    if (created.is_err()) {
        const auto error = created.unwrap_err_unchecked();
        std::fprintf(stderr,
                     "WebProducerDevice: instance creation failed (%s, VkResult=%d)\n",
                     error.command ? error.command : "dispatch",
                     static_cast<int>(error.api_result));
        return false;
    }
    instance_ = *instance_owner_;
    return true;
}

bool WebProducerDevice::PickPhysicalDevice() {
    uint32_t count = 0;
    VK_CHECK(instance_dispatch_.vkEnumeratePhysicalDevices(instance_, &count, nullptr));
    if (count == 0) {
        std::fprintf(stderr, "WebProducerDevice: no Vulkan physical devices\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devs(count);
    VK_CHECK(instance_dispatch_.vkEnumeratePhysicalDevices(instance_, &count, devs.data()));

    const bool pinning             = ! render_node_.empty();
    uint32_t   wanted_render_major = 0;
    uint32_t   wanted_render_minor = 0;
    if (pinning) {
        struct stat st {};
        if (::stat(render_node_.c_str(), &st) != 0) {
            std::fprintf(stderr,
                         "WebProducerDevice: stat(%s) failed: %s\n",
                         render_node_.c_str(),
                         std::strerror(errno));
            return false;
        }
        wanted_render_major = static_cast<uint32_t>(major(st.st_rdev));
        wanted_render_minor = static_cast<uint32_t>(minor(st.st_rdev));
    }

    for (auto pd : devs) {
        uint32_t qcount = 0;
        instance_dispatch_.vkGetPhysicalDeviceQueueFamilyProperties2(pd, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties2> qfp(
            qcount,
            VkQueueFamilyProperties2 { .sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2 });
        instance_dispatch_.vkGetPhysicalDeviceQueueFamilyProperties2(pd, &qcount, qfp.data());

        uint32_t picked_qf = rstd::u32::MAX.to_primitive();
        for (uint32_t i = 0; i < qcount; ++i) {
            // GRAPHICS_BIT implies TRANSFER_BIT; the bridge wants both
            // for vkCmdBlitImage and the producer needs no presentation
            // support because there's no surface.
            if (qfp[i].queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                picked_qf = i;
                break;
            }
        }
        if (picked_qf == rstd::u32::MAX.to_primitive()) continue;

        uint32_t ecount = 0;
        instance_dispatch_.vkEnumerateDeviceExtensionProperties(pd, nullptr, &ecount, nullptr);
        std::vector<VkExtensionProperties> exts(ecount);
        instance_dispatch_.vkEnumerateDeviceExtensionProperties(pd, nullptr, &ecount, exts.data());

        bool has_ext_mem_fd = false, has_dma_buf = false, has_modifier = false,
             has_ext_sem_fd = false, has_q_foreign = false, has_fmt_list = false,
             has_drm_props = false;
        for (auto& e : exts) {
            if (std::strcmp(e.extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0)
                has_ext_mem_fd = true;
            if (std::strcmp(e.extensionName, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) == 0)
                has_dma_buf = true;
            if (std::strcmp(e.extensionName, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) == 0)
                has_modifier = true;
            if (std::strcmp(e.extensionName, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME) == 0)
                has_ext_sem_fd = true;
            if (std::strcmp(e.extensionName, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME) == 0)
                has_q_foreign = true;
            if (std::strcmp(e.extensionName, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME) == 0)
                has_fmt_list = true;
            if (std::strcmp(e.extensionName, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) == 0)
                has_drm_props = true;
        }
        if (! has_ext_mem_fd || ! has_dma_buf || ! has_modifier || ! has_ext_sem_fd ||
            ! has_q_foreign || ! has_fmt_list)
            continue;

        if (pinning) {
            if (! has_drm_props) continue;
            VkPhysicalDeviceDrmPropertiesEXT drm {};
            drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
            VkPhysicalDeviceProperties2 props2 {};
            props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            props2.pNext = &drm;
            instance_dispatch_.vkGetPhysicalDeviceProperties2(pd, &props2);
            if (! drm.hasRender) continue;
            if (static_cast<uint32_t>(drm.renderMajor) != wanted_render_major ||
                static_cast<uint32_t>(drm.renderMinor) != wanted_render_minor)
                continue;
        }

        phys_         = pd;
        queue_family_ = picked_qf;
        VkPhysicalDeviceMemoryProperties2 mem_props2 {};
        mem_props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        instance_dispatch_.vkGetPhysicalDeviceMemoryProperties2(phys_, &mem_props2);
        mem_props_ = mem_props2.memoryProperties;

        VkPhysicalDeviceIDProperties id_props {};
        id_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 props2 {};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &id_props;
        instance_dispatch_.vkGetPhysicalDeviceProperties2(phys_, &props2);
        std::memcpy(device_uuid_, id_props.deviceUUID, 16);
        std::memcpy(driver_uuid_, id_props.driverUUID, 16);
        return true;
    }
    if (pinning) {
        std::fprintf(stderr,
                     "WebProducerDevice: no suitable physical device matching render_node %s "
                     "(need external_memory_fd + dma_buf + modifier + "
                     "external_semaphore_fd + queue_family_foreign + physical_device_drm)\n",
                     render_node_.c_str());
    } else {
        std::fprintf(stderr,
                     "WebProducerDevice: no suitable physical device "
                     "(need external_memory_fd + dma_buf + modifier + "
                     "external_semaphore_fd + queue_family_foreign)\n");
    }
    return false;
}

bool WebProducerDevice::CreateDevice() {
    float                   prio = 1.0f;
    VkDeviceQueueCreateInfo qi {};
    qi.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = queue_family_;
    qi.queueCount       = 1;
    qi.pQueuePriorities = &prio;

    const char* dev_exts[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        // Required by VK_EXT_image_drm_format_modifier on Vulkan 1.1
        // (promoted to core in 1.2). Validation rejects the device otherwise.
        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    };
    VkDeviceCreateInfo ci {};
    ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount    = 1;
    ci.pQueueCreateInfos       = &qi;
    ci.enabledExtensionCount   = static_cast<uint32_t>(std::size(dev_exts));
    ci.ppEnabledExtensionNames = dev_exts;

    auto created =
        vvk::Device::Create(device_owner_, phys_, instance_dispatch_, ci, device_dispatch_);
    if (created.is_err()) {
        const auto error = created.unwrap_err_unchecked();
        std::fprintf(stderr,
                     "WebProducerDevice: device creation failed (%s, VkResult=%d)\n",
                     error.command ? error.command : "dispatch",
                     static_cast<int>(error.api_result));
        return false;
    }
    device_ = *device_owner_;
    device_dispatch_.vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    auto allocated = vvk::MemoryAllocator::Create(phys_, instance_dispatch_, device_dispatch_);
    if (allocated.is_err()) {
        std::fprintf(stderr, "WebProducerDevice: allocator creation failed\n");
        return false;
    }
    allocator_ = allocated.unwrap_unchecked();
    return true;
}

bool WebProducerDevice::CreateCommandPool() {
    VkCommandPoolCreateInfo ci {};
    ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = queue_family_;
    VK_CHECK(device_dispatch_.vkCreateCommandPool(device_, &ci, nullptr, &cmd_pool_));

    VkCommandBufferAllocateInfo ai {};
    ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool        = cmd_pool_;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VK_CHECK(device_dispatch_.vkAllocateCommandBuffers(device_, &ai, &blit_cmd_));
    return true;
}

bool WebProducerDevice::CreateSyncObjects() {
    VkFenceCreateInfo fi {};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_CHECK(device_dispatch_.vkCreateFence(device_, &fi, nullptr, &blit_fence_));

    VkExportSemaphoreCreateInfo exp_si {};
    exp_si.sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    exp_si.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;

    VkSemaphoreCreateInfo si {};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    si.pNext = &exp_si;
    VK_CHECK(device_dispatch_.vkCreateSemaphore(device_, &si, nullptr, &blit_sem_));
    return true;
}

void WebProducerDevice::DestroyCpuUploadResources() {
    cpu_staging_mapping_ = rstd::None();
    cpu_staging_buffer_.reset();
    cpu_staging_size_ = 0;
}

bool WebProducerDevice::EnsureCpuUploadResources(const ::weweb::CpuPaintFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0) return false;
    const VkDeviceSize need =
        static_cast<VkDeviceSize>(frame.width) * static_cast<VkDeviceSize>(frame.height) * 4u;
    if (need == 0) return false;
    if (cpu_staging_buffer_.valid() && cpu_staging_size_ >= need) return true;

    if (device_dispatch_.vkWaitForFences(device_, 1, &blit_fence_, VK_TRUE, kFenceTimeoutNs) !=
        VK_SUCCESS) {
        std::fprintf(stderr, "WebProducerDevice: CPU upload fence wait timed out\n");
        return false;
    }
    DestroyCpuUploadResources();

    VkBufferCreateInfo bi {};
    bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size        = need;
    bi.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    auto allocated = allocator_.create_buffer(bi, vvk::MemoryRequest::Upload());
    if (allocated.is_err()) {
        std::fprintf(stderr, "WebProducerDevice: staging allocation failed\n");
        return false;
    }
    cpu_staging_buffer_ = allocated.unwrap_unchecked();
    auto mapped         = cpu_staging_buffer_.allocation().map();
    if (mapped.is_err()) {
        std::fprintf(stderr, "WebProducerDevice: staging mapping failed\n");
        DestroyCpuUploadResources();
        return false;
    }
    cpu_staging_mapping_ = rstd::Some(mapped.unwrap_unchecked());
    cpu_staging_size_    = need;
    return true;
}

bool WebProducerDevice::BeginTransferCommands(const char* op) {
    if (device_dispatch_.vkWaitForFences(device_, 1, &blit_fence_, VK_TRUE, kFenceTimeoutNs) !=
        VK_SUCCESS) {
        std::fprintf(stderr, "WebProducerDevice: prior %s fence wait timed out\n", op);
        return false;
    }
    device_dispatch_.vkResetFences(device_, 1, &blit_fence_);
    device_dispatch_.vkResetCommandBuffer(blit_cmd_, 0);

    VkCommandBufferBeginInfo bi {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (device_dispatch_.vkBeginCommandBuffer(blit_cmd_, &bi) != VK_SUCCESS) {
        std::fprintf(stderr, "WebProducerDevice: vkBeginCommandBuffer(%s) failed\n", op);
        RestoreTransferFence();
        return false;
    }
    return true;
}

void WebProducerDevice::RestoreTransferFence() {
    if (blit_fence_) {
        device_dispatch_.vkDestroyFence(device_, blit_fence_, nullptr);
        blit_fence_ = VK_NULL_HANDLE;
    }
    VkFenceCreateInfo fi {};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    (void)device_dispatch_.vkCreateFence(device_, &fi, nullptr, &blit_fence_);
}

int WebProducerDevice::SubmitTransferCommands(const char* op, bool wait_for_completion) {
    if (device_dispatch_.vkEndCommandBuffer(blit_cmd_) != VK_SUCCESS) {
        std::fprintf(stderr, "WebProducerDevice: vkEndCommandBuffer(%s) failed\n", op);
        RestoreTransferFence();
        return -1;
    }

    VkSubmitInfo submit {};
    submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount   = 1;
    submit.pCommandBuffers      = &blit_cmd_;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores    = &blit_sem_;
    if (device_dispatch_.vkQueueSubmit(queue_, 1, &submit, blit_fence_) != VK_SUCCESS) {
        std::fprintf(stderr, "WebProducerDevice: vkQueueSubmit(%s) failed\n", op);
        RestoreTransferFence();
        return -1;
    }

    VkSemaphoreGetFdInfoKHR gi {};
    gi.sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    gi.semaphore  = blit_sem_;
    gi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;

    int sync_fd = -1;
    if (device_dispatch_.vkGetSemaphoreFdKHR(device_, &gi, &sync_fd) != VK_SUCCESS || sync_fd < 0) {
        std::fprintf(stderr, "WebProducerDevice: vkGetSemaphoreFdKHR after %s failed\n", op);
        device_dispatch_.vkWaitForFences(device_, 1, &blit_fence_, VK_TRUE, kFenceTimeoutNs);
        return -1;
    }

    if (wait_for_completion) {
        device_dispatch_.vkWaitForFences(device_, 1, &blit_fence_, VK_TRUE, kFenceTimeoutNs);
    }
    return sync_fd;
}

WebProducerDevice::ImportedFrame WebProducerDevice::Import(const ::weweb::DmaBufFrame& frame) {
    ImportedFrame imp {};
    if (frame.plane_count < 1) return imp;
    if (frame.coded_width <= 0) return imp;
    if (frame.coded_height <= 0) return imp;

    int dup_fd = ::dup(frame.planes[0].fd);
    if (dup_fd < 0) {
        std::fprintf(stderr, "WebProducerDevice: dup(dmabuf fd) failed\n");
        return imp;
    }

    VkMemoryFdPropertiesKHR fd_props {};
    fd_props.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    if (device_dispatch_.vkGetMemoryFdPropertiesKHR(
            device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dup_fd, &fd_props) !=
        VK_SUCCESS) {
        std::fprintf(stderr, "WebProducerDevice: vkGetMemoryFdPropertiesKHR failed\n");
        ::close(dup_fd);
        return imp;
    }

    // CEF's modifier == DRM_FORMAT_MOD_INVALID is functionally LINEAR
    // for its current emit path (stride = width * bpp, no padding).
    constexpr uint64_t DRM_FORMAT_MOD_INVALID = 0x00ffffffffffffffULL;
    constexpr uint64_t DRM_FORMAT_MOD_LINEAR  = 0x0;
    uint64_t           modifier =
        (frame.modifier == DRM_FORMAT_MOD_INVALID) ? DRM_FORMAT_MOD_LINEAR : frame.modifier;

    // VUID-VkImageDrmFormatModifierExplicitCreateInfoEXT-size-02267:
    // size must be 0; driver derives it from rowPitch + extent + format.
    VkSubresourceLayout plane_layout {};
    plane_layout.offset   = frame.planes[0].offset;
    plane_layout.rowPitch = frame.planes[0].stride;

    VkImageDrmFormatModifierExplicitCreateInfoEXT mod_info {};
    mod_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    mod_info.drmFormatModifier           = modifier;
    mod_info.drmFormatModifierPlaneCount = 1;
    mod_info.pPlaneLayouts               = &plane_layout;

    VkExternalMemoryImageCreateInfo ext_img {};
    ext_img.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext_img.pNext       = &mod_info;
    ext_img.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo ii {};
    ii.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.pNext         = &ext_img;
    ii.imageType     = VK_IMAGE_TYPE_2D;
    ii.format        = FormatToVk(frame.format);
    ii.extent        = { static_cast<uint32_t>(frame.coded_width),
                         static_cast<uint32_t>(frame.coded_height),
                         1 };
    ii.mipLevels     = 1;
    ii.arrayLayers   = 1;
    ii.samples       = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling        = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ii.usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage img = VK_NULL_HANDLE;
    if (device_dispatch_.vkCreateImage(device_, &ii, nullptr, &img) != VK_SUCCESS) {
        std::fprintf(stderr, "WebProducerDevice: vkCreateImage(import) failed\n");
        ::close(dup_fd);
        return imp;
    }

    VkMemoryRequirements mr {};
    device_dispatch_.vkGetImageMemoryRequirements(device_, img, &mr);

    uint32_t allowed = mr.memoryTypeBits & fd_props.memoryTypeBits;
    uint32_t mtype   = rstd::u32::MAX.to_primitive();
    for (uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i) {
        if (allowed & (1u << i)) {
            mtype = i;
            break;
        }
    }
    if (mtype == rstd::u32::MAX.to_primitive()) {
        std::fprintf(stderr, "WebProducerDevice: no compatible memory type for DMA-BUF\n");
        device_dispatch_.vkDestroyImage(device_, img, nullptr);
        ::close(dup_fd);
        return imp;
    }

    VkMemoryDedicatedAllocateInfo dedicated {};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = img;

    VkImportMemoryFdInfoKHR import_fd {};
    import_fd.sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import_fd.pNext      = &dedicated;
    import_fd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import_fd.fd         = dup_fd;

    VkMemoryAllocateInfo mi {};
    mi.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mi.pNext           = &import_fd;
    mi.allocationSize  = frame.planes[0].size > 0 ? frame.planes[0].size : mr.size;
    mi.memoryTypeIndex = mtype;

    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (VkResult ar = device_dispatch_.vkAllocateMemory(device_, &mi, nullptr, &mem);
        ar != VK_SUCCESS) {
        std::fprintf(
            stderr, "WebProducerDevice: vkAllocateMemory(import)=%d\n", static_cast<int>(ar));
        device_dispatch_.vkDestroyImage(device_, img, nullptr);
        ::close(dup_fd);
        return imp;
    }
    // From here Vulkan owns dup_fd; no manual close.

    if (device_dispatch_.vkBindImageMemory(device_, img, mem, frame.planes[0].offset) !=
        VK_SUCCESS) {
        std::fprintf(stderr, "WebProducerDevice: vkBindImageMemory(import) failed\n");
        device_dispatch_.vkFreeMemory(device_, mem, nullptr);
        device_dispatch_.vkDestroyImage(device_, img, nullptr);
        return imp;
    }

    imp.image  = img;
    imp.memory = mem;
    imp.width  = static_cast<uint32_t>(frame.coded_width);
    imp.height = static_cast<uint32_t>(frame.coded_height);
    imp.format = FormatToVk(frame.format);
    imp.ok     = true;
    return imp;
}

void WebProducerDevice::DestroyImported(ImportedFrame& imp) {
    if (imp.image) device_dispatch_.vkDestroyImage(device_, imp.image, nullptr);
    if (imp.memory) device_dispatch_.vkFreeMemory(device_, imp.memory, nullptr);
    imp = {};
}

int WebProducerDevice::UploadToSlot(const ::weweb::CpuPaintFrame& frame, VkImage slot_image,
                                    VkExtent2D slot_extent, VkFormat slot_format) {
    if (! frame.buffer || slot_image == VK_NULL_HANDLE) return -1;
    if (frame.width <= 0 || frame.height <= 0) return -1;
    if (slot_format != VK_FORMAT_B8G8R8A8_UNORM && slot_format != VK_FORMAT_R8G8B8A8_UNORM) {
        std::fprintf(stderr,
                     "WebProducerDevice: CPU paint unsupported slot format %d\n",
                     static_cast<int>(slot_format));
        return -1;
    }
    if (slot_extent.width != static_cast<uint32_t>(frame.width) ||
        slot_extent.height != static_cast<uint32_t>(frame.height)) {
        std::fprintf(stderr,
                     "WebProducerDevice: CPU paint extent mismatch frame=%dx%d slot=%ux%u\n",
                     frame.width,
                     frame.height,
                     slot_extent.width,
                     slot_extent.height);
        return -1;
    }
    if (! EnsureCpuUploadResources(frame)) return -1;
    // A previous upload can still read the persistent mapping even when capacity is reused.
    if (device_dispatch_.vkWaitForFences(device_, 1, &blit_fence_, VK_TRUE, kFenceTimeoutNs) !=
        VK_SUCCESS)
        return -1;
    if (! CopyCpuPaintToStaging(
            frame, slot_format, cpu_staging_mapping_.as_ref().unwrap_unchecked().data()))
        return -1;
    if (cpu_staging_buffer_.allocation().flush().is_err()) return -1;

    if (! BeginTransferCommands("cpu-paint")) return -1;

    VkImageMemoryBarrier b_dst {};
    b_dst.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b_dst.oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED;
    b_dst.newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b_dst.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    b_dst.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    b_dst.image                       = slot_image;
    b_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b_dst.subresourceRange.levelCount = 1;
    b_dst.subresourceRange.layerCount = 1;
    b_dst.srcAccessMask               = 0;
    b_dst.dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT;
    device_dispatch_.vkCmdPipelineBarrier(blit_cmd_,
                                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          0,
                                          0,
                                          nullptr,
                                          0,
                                          nullptr,
                                          1,
                                          &b_dst);

    VkBufferImageCopy copy {};
    copy.bufferOffset                    = 0;
    copy.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.mipLevel       = 0;
    copy.imageSubresource.baseArrayLayer = 0;
    copy.imageSubresource.layerCount     = 1;
    copy.imageOffset                     = { 0, 0, 0 };
    copy.imageExtent                     = { slot_extent.width, slot_extent.height, 1 };
    device_dispatch_.vkCmdCopyBufferToImage(blit_cmd_,
                                            cpu_staging_buffer_.handle(),
                                            slot_image,
                                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            1,
                                            &copy);

    VkImageMemoryBarrier b_release {};
    b_release.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b_release.oldLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b_release.newLayout                   = VK_IMAGE_LAYOUT_GENERAL;
    b_release.srcQueueFamilyIndex         = queue_family_;
    b_release.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_FOREIGN_EXT;
    b_release.image                       = slot_image;
    b_release.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b_release.subresourceRange.levelCount = 1;
    b_release.subresourceRange.layerCount = 1;
    b_release.srcAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT;
    b_release.dstAccessMask               = 0;
    device_dispatch_.vkCmdPipelineBarrier(blit_cmd_,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                          0,
                                          0,
                                          nullptr,
                                          0,
                                          nullptr,
                                          1,
                                          &b_release);

    return SubmitTransferCommands("cpu-paint", /*wait_for_completion=*/false);
}

int WebProducerDevice::BlitToSlot(const ImportedFrame& imp, VkImage slot_image,
                                  VkExtent2D slot_extent) {
    if (! imp.ok || imp.image == VK_NULL_HANDLE || slot_image == VK_NULL_HANDLE) return -1;

    if (device_dispatch_.vkWaitForFences(device_, 1, &blit_fence_, VK_TRUE, kFenceTimeoutNs) !=
        VK_SUCCESS) {
        std::fprintf(stderr, "WebProducerDevice: prior blit fence wait timed out\n");
        return -1;
    }
    device_dispatch_.vkResetFences(device_, 1, &blit_fence_);
    device_dispatch_.vkResetCommandBuffer(blit_cmd_, 0);

    VkCommandBufferBeginInfo bi {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (device_dispatch_.vkBeginCommandBuffer(blit_cmd_, &bi) != VK_SUCCESS) return -1;

    // imp.image: UNDEFINED → TRANSFER_SRC.
    VkImageMemoryBarrier b_src {};
    b_src.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b_src.oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED;
    b_src.newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b_src.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    b_src.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    b_src.image                       = imp.image;
    b_src.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b_src.subresourceRange.levelCount = 1;
    b_src.subresourceRange.layerCount = 1;
    b_src.srcAccessMask               = 0;
    b_src.dstAccessMask               = VK_ACCESS_TRANSFER_READ_BIT;
    device_dispatch_.vkCmdPipelineBarrier(blit_cmd_,
                                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          0,
                                          0,
                                          nullptr,
                                          0,
                                          nullptr,
                                          1,
                                          &b_src);

    // slot_image: UNDEFINED → TRANSFER_DST. Bridge slot's prior layout
    // is FOREIGN-released by the consumer and the spec lets us treat
    // that as UNDEFINED on re-acquire as long as we don't expect to
    // preserve contents — the producer always overwrites the whole slot.
    VkImageMemoryBarrier b_dst = b_src;
    b_dst.oldLayout            = VK_IMAGE_LAYOUT_UNDEFINED;
    b_dst.newLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b_dst.image                = slot_image;
    b_dst.srcAccessMask        = 0;
    b_dst.dstAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
    device_dispatch_.vkCmdPipelineBarrier(blit_cmd_,
                                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          0,
                                          0,
                                          nullptr,
                                          0,
                                          nullptr,
                                          1,
                                          &b_dst);

    VkImageBlit blit {};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.srcOffsets[0]             = { 0, 0, 0 };
    blit.srcOffsets[1] = { static_cast<int32_t>(imp.width), static_cast<int32_t>(imp.height), 1 };
    blit.dstOffsets[0] = { 0, 0, 0 };
    blit.dstOffsets[1] = { static_cast<int32_t>(slot_extent.width),
                           static_cast<int32_t>(slot_extent.height),
                           1 };
    device_dispatch_.vkCmdBlitImage(blit_cmd_,
                                    imp.image,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    slot_image,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    1,
                                    &blit,
                                    VK_FILTER_LINEAR);

    // slot_image: TRANSFER_DST → GENERAL, releasing queue ownership to
    // VK_QUEUE_FAMILY_FOREIGN_EXT so the non-Vulkan consumer (KMS /
    // wlroots / Plasma compositor) can read. GENERAL matches
    // GENERAL matches the frame-surface lease published by both renderers.
    VkImageMemoryBarrier b_release {};
    b_release.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b_release.oldLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b_release.newLayout                   = VK_IMAGE_LAYOUT_GENERAL;
    b_release.srcQueueFamilyIndex         = queue_family_;
    b_release.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_FOREIGN_EXT;
    b_release.image                       = slot_image;
    b_release.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b_release.subresourceRange.levelCount = 1;
    b_release.subresourceRange.layerCount = 1;
    b_release.srcAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT;
    b_release.dstAccessMask               = 0;
    device_dispatch_.vkCmdPipelineBarrier(blit_cmd_,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                          0,
                                          0,
                                          nullptr,
                                          0,
                                          nullptr,
                                          1,
                                          &b_release);

    if (device_dispatch_.vkEndCommandBuffer(blit_cmd_) != VK_SUCCESS) return -1;

    VkSubmitInfo submit {};
    submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount   = 1;
    submit.pCommandBuffers      = &blit_cmd_;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores    = &blit_sem_;
    if (device_dispatch_.vkQueueSubmit(queue_, 1, &submit, blit_fence_) != VK_SUCCESS) {
        std::fprintf(stderr, "WebProducerDevice: vkQueueSubmit(blit) failed\n");
        return -1;
    }

    // Export the sync_file fd from the still-pending semaphore signal.
    // SYNC_FD export is allowed before the GPU completes — bridge /
    // consumer wait on the sync_file. Exporting also resets the
    // semaphore payload, so next iteration reuses the same handle.
    VkSemaphoreGetFdInfoKHR gi {};
    gi.sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    gi.semaphore  = blit_sem_;
    gi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;

    int sync_fd = -1;
    if (device_dispatch_.vkGetSemaphoreFdKHR(device_, &gi, &sync_fd) != VK_SUCCESS || sync_fd < 0) {
        std::fprintf(stderr, "WebProducerDevice: vkGetSemaphoreFdKHR failed\n");
        // Still need to wait the fence so the temp image is safe to
        // destroy on caller's `DestroyImported`.
        device_dispatch_.vkWaitForFences(device_, 1, &blit_fence_, VK_TRUE, kFenceTimeoutNs);
        return -1;
    }

    // CEF reclaims the input dma-buf when the OnAcceleratedPaint
    // callback returns, so the GPU MUST be done reading the imported
    // VkImage before this function unwinds. CPU-block on the fence.
    device_dispatch_.vkWaitForFences(device_, 1, &blit_fence_, VK_TRUE, kFenceTimeoutNs);
    return sync_fd;
}

} // namespace ww_wescene
