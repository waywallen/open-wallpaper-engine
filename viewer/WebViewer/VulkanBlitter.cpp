module;

#include <cstdio>
#include <cstring>
#include <unistd.h>

module viewer.web;

import rstd;
import weweb;
import vvk;

import viewer.glfw_vulkan;

using rstd::mem::memcpy;

using namespace viewer::glfw;

using namespace rstd::prelude;
using rstd::ffi::CStr;
using rstd::ffi::OsStr;
using rstd::io::eprint;

namespace weweb
{

namespace
{

#define VK_CHECK(expr)                                                                           \
    do {                                                                                         \
        VkResult _r = (expr);                                                                    \
        if (_r != VK_SUCCESS) {                                                                  \
            eprint("weweb: {} failed (VkResult={}) at {}:{}\n",                                  \
                   ref<OsStr>::from_encoded_bytes_unchecked(CStr::from_ptr(#expr).to_bytes())    \
                       .display(),                                                               \
                   static_cast<int>(_r),                                                         \
                   ref<OsStr>::from_encoded_bytes_unchecked(CStr::from_ptr(__FILE__).to_bytes()) \
                       .display(),                                                               \
                   __LINE__);                                                                    \
            return false;                                                                        \
        }                                                                                        \
    } while (0)

constexpr rstd::uint64_t kFenceTimeoutNs = 5'000'000'000ull; // 5s

#if __is_target_os(macos)
constexpr const char* kPortabilitySubsetExtension = "VK_KHR_portability_subset";

bool HasInstanceExtension(slice<VkExtensionProperties> extensions, const char* name) {
    return extensions.iter().any([name](auto extension) {
        return (CStr::from_ptr(extension->extensionName).to_bytes() ==
                CStr::from_ptr(name).to_bytes());
    });
}
#endif

VkFormat FormatToVk(DmaBufFormat f) {
    switch (f) {
    case DmaBufFormat::BGRA8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
    case DmaBufFormat::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    }
    return VK_FORMAT_B8G8R8A8_UNORM;
}

} // namespace

VulkanBlitter::VulkanBlitter() = default;

VulkanBlitter::~VulkanBlitter() { Shutdown(); }

bool VulkanBlitter::Init(GLFWwindow* window) {
    Shutdown();
    window_ = window;
    if (CreateInstance() && CreateSurface(window) && PickPhysicalDevice() && CreateDevice() &&
        CreateCommandPool() && CreateSwapchain() && CreateSyncObjects())
        return true;
    Shutdown();
    return false;
}

void VulkanBlitter::Shutdown() {
    if (device_) device_dispatch_.vkDeviceWaitIdle(device_);

#if __is_target_os(macos)
    DestroyCpuStaging();
#endif
    DestroyOwnedImage();

    for (auto& s : img_avail_sem_)
        if (s) {
            device_dispatch_.vkDestroySemaphore(device_, s, nullptr);
            s = VK_NULL_HANDLE;
        }
    for (auto& s : render_done_sem_)
        if (s) device_dispatch_.vkDestroySemaphore(device_, s, nullptr);
    render_done_sem_.clear();
    for (auto& f : in_flight_fence_)
        if (f) {
            device_dispatch_.vkDestroyFence(device_, f, nullptr);
            f = VK_NULL_HANDLE;
        }

    if (cmd_pool_ != VK_NULL_HANDLE) {
        device_dispatch_.vkDestroyCommandPool(device_, cmd_pool_, nullptr);
        cmd_pool_ = VK_NULL_HANDLE;
    }
    DestroySwapchain();
    device_owner_.reset();
    device_ = VK_NULL_HANDLE;
    if (surface_ && instance_) {
        instance_dispatch_.vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    instance_owner_.reset();
    instance_          = VK_NULL_HANDLE;
    phys_              = VK_NULL_HANDLE;
    queue_             = VK_NULL_HANDLE;
    window_            = nullptr;
    frame_index_       = 0;
    device_dispatch_   = {};
    instance_dispatch_ = {};
    loader_            = rstd::None();
}

bool VulkanBlitter::CreateInstance() {
    auto loaded = vvk::VulkanLoader::Open();
    if (loaded.is_err()) {
        eprint("weweb: Vulkan loader open failed\n");
        return false;
    }
    loader_ = rstd::Some(loaded.unwrap_unchecked());
    VkApplicationInfo app {};
    app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName   = "weweb";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName        = "weweb-blitter";
    app.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
    // 1.1 promotes external_memory_capabilities into core, so we don't
    // need the instance extension.
    app.apiVersion = VK_API_VERSION_1_1;

    rstd::uint32_t glfw_count = 0;
    const char**   glfw_exts  = glfwGetRequiredInstanceExtensions(&glfw_count);
    if (! glfw_exts) {
        eprint("weweb: glfwGetRequiredInstanceExtensions failed\n");
        return false;
    }

    Vec<VkExtensionProperties> available_exts;
    rstd::uint32_t             available_count = 0;
    VK_CHECK(loader_.as_ref().unwrap_unchecked().global().vkEnumerateInstanceExtensionProperties(
        nullptr, &available_count, nullptr));
    available_exts.resize(usize(available_count), VkExtensionProperties {});
    VK_CHECK(loader_.as_ref().unwrap_unchecked().global().vkEnumerateInstanceExtensionProperties(
        nullptr, &available_count, available_exts.data()));

    auto enabled_exts =
        Vec<const char*>::from(slice<const char*>::from_raw_parts(glfw_exts, usize(glfw_count)));
#if __is_target_os(macos)
    if (HasInstanceExtension(available_exts.as_slice(),
                             VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        enabled_exts.emplace_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }
#endif

    VkInstanceCreateInfo ci {};
    ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo        = &app;
    ci.enabledExtensionCount   = static_cast<rstd::uint32_t>(enabled_exts.len().to_primitive());
    ci.ppEnabledExtensionNames = enabled_exts.data();
#if __is_target_os(macos)
    if (HasInstanceExtension(available_exts, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif

    auto created = vvk::Instance::Create(
        instance_owner_, loader_.as_ref().unwrap_unchecked().global(), ci, instance_dispatch_);
    if (created.is_err()) {
        const auto error = created.unwrap_err_unchecked();
        eprint("weweb: instance creation failed ({}, VkResult={})\n",
               ref<OsStr>::from_encoded_bytes_unchecked(
                   CStr::from_ptr(error.command ? error.command : "dispatch").to_bytes())
                   .display(),
               static_cast<int>(error.api_result));
        return false;
    }
    instance_ = *instance_owner_;
    return true;
}

bool VulkanBlitter::CreateSurface(GLFWwindow* window) {
    VK_CHECK(viewer::glfw::glfwCreateWindowSurface(instance_, window, nullptr, &surface_));
    return true;
}

bool VulkanBlitter::PickPhysicalDevice() {
    rstd::uint32_t count = 0;
    VK_CHECK(instance_dispatch_.vkEnumeratePhysicalDevices(instance_, &count, nullptr));
    if (count == 0) {
        eprint("weweb: no Vulkan physical devices\n");
        return false;
    }
    Vec<VkPhysicalDevice> devs;
    devs.resize(usize(count), VkPhysicalDevice {});
    VK_CHECK(instance_dispatch_.vkEnumeratePhysicalDevices(instance_, &count, devs.data()));

    for (auto pd : devs) {
        rstd::uint32_t qcount = 0;
        instance_dispatch_.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, nullptr);
        Vec<VkQueueFamilyProperties> qfp;
        qfp.resize(usize(qcount), VkQueueFamilyProperties {});
        instance_dispatch_.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, qfp.data());

        for (rstd::uint32_t i = 0; i < qcount; ++i) {
            if (! (qfp[usize(i)].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present = VK_FALSE;
            instance_dispatch_.vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface_, &present);
            if (! present) continue;

            rstd::uint32_t ecount = 0;
            instance_dispatch_.vkEnumerateDeviceExtensionProperties(pd, nullptr, &ecount, nullptr);
            Vec<VkExtensionProperties> exts;
            exts.resize(usize(ecount), VkExtensionProperties {});
            instance_dispatch_.vkEnumerateDeviceExtensionProperties(
                pd, nullptr, &ecount, exts.data());
#if __is_target_os(macos)
            bool has_swapchain          = false;
            bool has_portability_subset = false;
#else
            bool has_swapchain = false, has_ext_mem_fd = false, has_dma_buf = false,
                 has_modifier = false, has_fmt_list = false;
#endif
            for (auto& e : exts) {
                if ((CStr::from_ptr(e.extensionName).to_bytes() ==
                     CStr::from_ptr(VK_KHR_SWAPCHAIN_EXTENSION_NAME).to_bytes()))
                    has_swapchain = true;
#if __is_target_os(macos)
                if ((CStr::from_ptr(e.extensionName).to_bytes() ==
                     CStr::from_ptr(kPortabilitySubsetExtension).to_bytes()))
                    has_portability_subset = true;
#else
                if ((CStr::from_ptr(e.extensionName).to_bytes() ==
                     CStr::from_ptr(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME).to_bytes()))
                    has_ext_mem_fd = true;
                if ((CStr::from_ptr(e.extensionName).to_bytes() ==
                     CStr::from_ptr(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME).to_bytes()))
                    has_dma_buf = true;
                if ((CStr::from_ptr(e.extensionName).to_bytes() ==
                     CStr::from_ptr(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME).to_bytes()))
                    has_modifier = true;
                if ((CStr::from_ptr(e.extensionName).to_bytes() ==
                     CStr::from_ptr(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME).to_bytes()))
                    has_fmt_list = true;
#endif
            }
#if __is_target_os(macos)
            if (! has_swapchain) continue;
            portability_subset_supported_ = has_portability_subset;
#else
            if (! has_swapchain || ! has_ext_mem_fd || ! has_dma_buf || ! has_modifier ||
                ! has_fmt_list)
                continue;
#endif

            phys_         = pd;
            queue_family_ = i;
            instance_dispatch_.vkGetPhysicalDeviceMemoryProperties(phys_, &mem_props_);
            return true;
        }
    }
#if __is_target_os(macos)
    eprint("weweb: no suitable Vulkan device with graphics/present support\n");
#else
    eprint("weweb: no suitable Vulkan device (need swapchain + external_memory_fd + dma_buf)\n");
#endif
    return false;
}

bool VulkanBlitter::CreateDevice() {
    float                   prio = 1.0f;
    VkDeviceQueueCreateInfo qi {};
    qi.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = queue_family_;
    qi.queueCount       = 1;
    qi.pQueuePriorities = &prio;

#if __is_target_os(macos)
    Vec<const char*> dev_exts;
    dev_exts.push(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (portability_subset_supported_) {
        dev_exts.emplace_back(kPortabilitySubsetExtension);
    }
#else
    const char* dev_exts[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        // Required by VK_EXT_image_drm_format_modifier on Vulkan 1.1
        // (promoted to core in 1.2). Validation rejects the device otherwise.
        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    };
#endif
    VkDeviceCreateInfo ci {};
    ci.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos    = &qi;
#if __is_target_os(macos)
    ci.enabledExtensionCount = static_cast<rstd::uint32_t>(dev_exts.len().to_primitive());
#else
    ci.enabledExtensionCount =
        static_cast<rstd::uint32_t>((sizeof(dev_exts) / sizeof(dev_exts[0])));
#endif
#if __is_target_os(macos)
    ci.ppEnabledExtensionNames = dev_exts.data();
#else
    ci.ppEnabledExtensionNames = dev_exts;
#endif

    auto created =
        vvk::Device::Create(device_owner_, phys_, instance_dispatch_, ci, device_dispatch_);
    if (created.is_err()) {
        const auto error = created.unwrap_err_unchecked();
        eprint("weweb: device creation failed ({}, VkResult={})\n",
               ref<OsStr>::from_encoded_bytes_unchecked(
                   CStr::from_ptr(error.command ? error.command : "dispatch").to_bytes())
                   .display(),
               static_cast<int>(error.api_result));
        return false;
    }
    device_ = *device_owner_;
    device_dispatch_.vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

#if ! __is_target_os(macos)
    if (! device_dispatch_.vkGetMemoryFdPropertiesKHR) {
        eprint("weweb: vkGetMemoryFdPropertiesKHR not available\n");
        return false;
    }
#endif
    return true;
}

#if __is_target_os(macos)
bool VulkanBlitter::EnsureCpuStaging(rstd::size_t size) {
    if (size == 0) return false;
    if (cpu_staging_ != VK_NULL_HANDLE && cpu_staging_size_ >= size) return true;

    DestroyCpuStaging();

    VkBufferCreateInfo buffer_info {};
    buffer_info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size        = static_cast<VkDeviceSize>(size);
    buffer_info.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (device_dispatch_.vkCreateBuffer(device_, &buffer_info, nullptr, &cpu_staging_) !=
        VK_SUCCESS) {
        eprint("weweb: failed to create CPU paint staging buffer\n");
        return false;
    }

    VkMemoryRequirements requirements {};
    device_dispatch_.vkGetBufferMemoryRequirements(device_, cpu_staging_, &requirements);
    rstd::uint32_t memory_type =
        FindMemoryType(requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    cpu_staging_coherent_ = memory_type != u32::MAX.to_primitive();
    if (! cpu_staging_coherent_) {
        memory_type =
            FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    }
    if (memory_type == u32::MAX.to_primitive()) {
        eprint("weweb: no host-visible memory for CPU paint staging\n");
        device_dispatch_.vkDestroyBuffer(device_, cpu_staging_, nullptr);
        cpu_staging_ = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo allocate_info {};
    allocate_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize  = requirements.size;
    allocate_info.memoryTypeIndex = memory_type;
    if (device_dispatch_.vkAllocateMemory(device_, &allocate_info, nullptr, &cpu_staging_mem_) !=
        VK_SUCCESS) {
        eprint("weweb: failed to allocate CPU paint staging memory\n");
        device_dispatch_.vkDestroyBuffer(device_, cpu_staging_, nullptr);
        cpu_staging_ = VK_NULL_HANDLE;
        return false;
    }
    if (device_dispatch_.vkBindBufferMemory(device_, cpu_staging_, cpu_staging_mem_, 0) !=
            VK_SUCCESS ||
        device_dispatch_.vkMapMemory(
            device_, cpu_staging_mem_, 0, requirements.size, 0, &cpu_staging_mapped_) !=
            VK_SUCCESS) {
        eprint("weweb: failed to map CPU paint staging memory\n");
        if (cpu_staging_mapped_ != nullptr)
            device_dispatch_.vkUnmapMemory(device_, cpu_staging_mem_);
        device_dispatch_.vkFreeMemory(device_, cpu_staging_mem_, nullptr);
        device_dispatch_.vkDestroyBuffer(device_, cpu_staging_, nullptr);
        cpu_staging_mem_    = VK_NULL_HANDLE;
        cpu_staging_        = VK_NULL_HANDLE;
        cpu_staging_mapped_ = nullptr;
        return false;
    }
    cpu_staging_size_ = requirements.size;
    return true;
}

void VulkanBlitter::DestroyCpuStaging() {
    if (cpu_staging_mapped_ != nullptr && cpu_staging_mem_ != VK_NULL_HANDLE) {
        device_dispatch_.vkUnmapMemory(device_, cpu_staging_mem_);
    }
    cpu_staging_mapped_ = nullptr;
    if (cpu_staging_ != VK_NULL_HANDLE) {
        device_dispatch_.vkDestroyBuffer(device_, cpu_staging_, nullptr);
        cpu_staging_ = VK_NULL_HANDLE;
    }
    if (cpu_staging_mem_ != VK_NULL_HANDLE) {
        device_dispatch_.vkFreeMemory(device_, cpu_staging_mem_, nullptr);
        cpu_staging_mem_ = VK_NULL_HANDLE;
    }
    cpu_staging_size_     = 0;
    cpu_staging_coherent_ = false;
}

bool VulkanBlitter::UploadPendingCpuPaint() {
    auto cpu = cpu_paint_.lock().unwrap();
    if (! cpu->pending) return true;
    const auto staging_size =
        static_cast<rstd::size_t>(cpu->width) * 4u * static_cast<rstd::size_t>(cpu->height);
    if (! EnsureOwnedImage(cpu->width, cpu->height) || ! EnsureCpuStaging(staging_size)) {
        return false;
    }
    memcpy(cpu_staging_mapped_, cpu->data.data(), rstd::usize(staging_size));
    if (! CopyCpuStagingToOwned(cpu->width, cpu->height)) return false;
    cpu->pending = false;
    return true;
}

bool VulkanBlitter::CopyCpuStagingToOwned(int width, int height) {
    if (cpu_staging_ == VK_NULL_HANDLE || owned_image_ == VK_NULL_HANDLE || width <= 0 ||
        height <= 0) {
        return false;
    }
    if (! cpu_staging_coherent_) {
        VkMappedMemoryRange range {};
        range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = cpu_staging_mem_;
        range.size   = VK_WHOLE_SIZE;
        if (device_dispatch_.vkFlushMappedMemoryRanges(device_, 1, &range) != VK_SUCCESS)
            return false;
    }
    if (device_dispatch_.vkWaitForFences(
            device_, 1, &in_flight_fence_[0], VK_TRUE, kFenceTimeoutNs) != VK_SUCCESS ||
        device_dispatch_.vkWaitForFences(
            device_, 1, &in_flight_fence_[1], VK_TRUE, kFenceTimeoutNs) != VK_SUCCESS) {
        eprint("weweb: timed out waiting for the CPU paint copy\n");
        return false;
    }

    VkCommandBuffer cmd = cmd_bufs_[0];
    device_dispatch_.vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin_info {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (device_dispatch_.vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) return false;

    VkImageMemoryBarrier destination_barrier {};
    destination_barrier.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    destination_barrier.oldLayout                   = owned_layout_;
    destination_barrier.newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    destination_barrier.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    destination_barrier.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    destination_barrier.image                       = owned_image_;
    destination_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    destination_barrier.subresourceRange.levelCount = 1;
    destination_barrier.subresourceRange.layerCount = 1;
    destination_barrier.srcAccessMask =
        owned_layout_ == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ? VK_ACCESS_TRANSFER_READ_BIT : 0;
    destination_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    device_dispatch_.vkCmdPipelineBarrier(cmd,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          0,
                                          0,
                                          nullptr,
                                          0,
                                          nullptr,
                                          1,
                                          &destination_barrier);

    VkBufferImageCopy region {};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent                 = { static_cast<rstd::uint32_t>(width),
                                           static_cast<rstd::uint32_t>(height),
                                           1 };
    device_dispatch_.vkCmdCopyBufferToImage(
        cmd, cpu_staging_, owned_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier source_barrier = destination_barrier;
    source_barrier.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    source_barrier.newLayout            = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    source_barrier.srcAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
    source_barrier.dstAccessMask        = VK_ACCESS_TRANSFER_READ_BIT;
    device_dispatch_.vkCmdPipelineBarrier(cmd,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          0,
                                          0,
                                          nullptr,
                                          0,
                                          nullptr,
                                          1,
                                          &source_barrier);

    if (device_dispatch_.vkEndCommandBuffer(cmd) != VK_SUCCESS) return false;
    device_dispatch_.vkResetFences(device_, 1, &in_flight_fence_[0]);
    VkSubmitInfo submit {};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cmd;
    if (device_dispatch_.vkQueueSubmit(queue_, 1, &submit, in_flight_fence_[0]) != VK_SUCCESS)
        return false;
    if (device_dispatch_.vkWaitForFences(
            device_, 1, &in_flight_fence_[0], VK_TRUE, kFenceTimeoutNs) != VK_SUCCESS) {
        eprint("weweb: timed out waiting for the CPU paint copy completion\n");
        return false;
    }
    owned_layout_   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    owned_has_data_ = true;
    return true;
}
#endif

bool VulkanBlitter::CreateCommandPool() {
    VkCommandPoolCreateInfo ci {};
    ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = queue_family_;
    VK_CHECK(device_dispatch_.vkCreateCommandPool(device_, &ci, nullptr, &cmd_pool_));

    VkCommandBufferAllocateInfo ai {};
    ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool        = cmd_pool_;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = kMaxFramesInFlight;
    VK_CHECK(device_dispatch_.vkAllocateCommandBuffers(device_, &ai, cmd_bufs_));
    return true;
}

bool VulkanBlitter::CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps {};
    VK_CHECK(instance_dispatch_.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps));

    rstd::uint32_t fcount = 0;
    VK_CHECK(
        instance_dispatch_.vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fcount, nullptr));
    Vec<VkSurfaceFormatKHR> formats;
    formats.resize(usize(fcount), VkSurfaceFormatKHR {});
    VK_CHECK(instance_dispatch_.vkGetPhysicalDeviceSurfaceFormatsKHR(
        phys_, surface_, &fcount, formats.data()));

    swap_format_     = formats.first().unwrap()->format;
    swap_colorspace_ = formats.first().unwrap()->colorSpace;
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) {
            swap_format_     = f.format;
            swap_colorspace_ = f.colorSpace;
            break;
        }
    }
    present_mode_ = VK_PRESENT_MODE_FIFO_KHR;

    if (caps.currentExtent.width != rstd::u32::MAX.to_primitive()) {
        extent_ = caps.currentExtent;
    } else {
        int w = 0, h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        extent_.width = rstd::cmp::min<rstd::uint32_t>(
            caps.maxImageExtent.width,
            rstd::cmp::max<rstd::uint32_t>(caps.minImageExtent.width,
                                           static_cast<rstd::uint32_t>(w)));
        extent_.height = rstd::cmp::min<rstd::uint32_t>(
            caps.maxImageExtent.height,
            rstd::cmp::max<rstd::uint32_t>(caps.minImageExtent.height,
                                           static_cast<rstd::uint32_t>(h)));
    }
    if (extent_.width == 0 || extent_.height == 0) return true;

    rstd::uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR ci {};
    ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface          = surface_;
    ci.minImageCount    = image_count;
    ci.imageFormat      = swap_format_;
    ci.imageColorSpace  = swap_colorspace_;
    ci.imageExtent      = extent_;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform     = caps.currentTransform;
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode      = present_mode_;
    ci.clipped          = VK_TRUE;
    ci.oldSwapchain     = VK_NULL_HANDLE;

    VK_CHECK(device_dispatch_.vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_));

    rstd::uint32_t scount = 0;
    VK_CHECK(device_dispatch_.vkGetSwapchainImagesKHR(device_, swapchain_, &scount, nullptr));
    swap_images_.resize(usize(scount), VkImage {});
    VK_CHECK(device_dispatch_.vkGetSwapchainImagesKHR(
        device_, swapchain_, &scount, swap_images_.data()));

    // Per-image render-done semaphore (see hpp comment).
    VkSemaphoreCreateInfo si {};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    render_done_sem_.resize(usize(scount), VkSemaphore {});
    for (rstd::uint32_t i = 0; i < scount; ++i) {
        VK_CHECK(
            device_dispatch_.vkCreateSemaphore(device_, &si, nullptr, &render_done_sem_[usize(i)]));
    }
    return true;
}

void VulkanBlitter::DestroySwapchain() {
    for (auto& s : render_done_sem_)
        if (s) device_dispatch_.vkDestroySemaphore(device_, s, nullptr);
    render_done_sem_.clear();
    if (swapchain_ != VK_NULL_HANDLE) {
        device_dispatch_.vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    swap_images_.clear();
}

bool VulkanBlitter::CreateSyncObjects() {
    VkSemaphoreCreateInfo si {};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fi {};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    // render_done_sem_ is created per swapchain image in CreateSwapchain.
    for (rstd::uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        VK_CHECK(device_dispatch_.vkCreateSemaphore(device_, &si, nullptr, &img_avail_sem_[i]));
        VK_CHECK(device_dispatch_.vkCreateFence(device_, &fi, nullptr, &in_flight_fence_[i]));
    }
    return true;
}

bool VulkanBlitter::Resize() {
    if (! device_) return false;
    device_dispatch_.vkDeviceWaitIdle(device_);
    DestroySwapchain();
    if (! CreateSwapchain()) return false;
    return true;
}

rstd::uint32_t VulkanBlitter::FindMemoryType(rstd::uint32_t        type_bits,
                                             VkMemoryPropertyFlags props) const {
    for (rstd::uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && (mem_props_.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return u32::MAX.to_primitive();
}

bool VulkanBlitter::EnsureOwnedImage(int width, int height) {
    if (owned_image_ != VK_NULL_HANDLE && owned_width_ == width && owned_height_ == height) {
        return true;
    }
#if __is_target_os(macos)
    if (owned_image_ != VK_NULL_HANDLE) {
        if (in_flight_fence_[0] != VK_NULL_HANDLE &&
            device_dispatch_.vkWaitForFences(
                device_, 1, &in_flight_fence_[0], VK_TRUE, kFenceTimeoutNs) != VK_SUCCESS) {
            eprint("weweb: timed out waiting for the old CPU paint image\n");
            return false;
        }
        if (in_flight_fence_[1] != VK_NULL_HANDLE &&
            device_dispatch_.vkWaitForFences(
                device_, 1, &in_flight_fence_[1], VK_TRUE, kFenceTimeoutNs) != VK_SUCCESS) {
            eprint("weweb: timed out waiting for the old presented image\n");
            return false;
        }
    }
#endif
    DestroyOwnedImage();

    VkImageCreateInfo ii {};
    ii.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType   = VK_IMAGE_TYPE_2D;
    ii.format      = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent      = { static_cast<rstd::uint32_t>(width), static_cast<rstd::uint32_t>(height), 1 };
    ii.mipLevels   = 1;
    ii.arrayLayers = 1;
    ii.samples     = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ii.usage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(device_dispatch_.vkCreateImage(device_, &ii, nullptr, &owned_image_));

    VkMemoryRequirements mr {};
    device_dispatch_.vkGetImageMemoryRequirements(device_, owned_image_, &mr);
    VkMemoryAllocateInfo mi {};
    mi.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mi.allocationSize  = mr.size;
    mi.memoryTypeIndex = FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mi.memoryTypeIndex == u32::MAX.to_primitive()) {
        eprint("weweb: no DEVICE_LOCAL memory type for owned image\n");
        return false;
    }
    VK_CHECK(device_dispatch_.vkAllocateMemory(device_, &mi, nullptr, &owned_image_mem_));
    VK_CHECK(device_dispatch_.vkBindImageMemory(device_, owned_image_, owned_image_mem_, 0));

    owned_width_    = width;
    owned_height_   = height;
    owned_layout_   = VK_IMAGE_LAYOUT_UNDEFINED;
    owned_has_data_ = false;
    return true;
}

void VulkanBlitter::DestroyOwnedImage() {
    if (owned_image_ != VK_NULL_HANDLE) {
        device_dispatch_.vkDestroyImage(device_, owned_image_, nullptr);
        owned_image_ = VK_NULL_HANDLE;
    }
    if (owned_image_mem_ != VK_NULL_HANDLE) {
        device_dispatch_.vkFreeMemory(device_, owned_image_mem_, nullptr);
        owned_image_mem_ = VK_NULL_HANDLE;
    }
    owned_width_ = owned_height_ = 0;
    owned_layout_                = VK_IMAGE_LAYOUT_UNDEFINED;
    owned_has_data_              = false;
}

bool VulkanBlitter::AcceptCpuPaint(const CpuPaintFrame& frame) {
#if ! __is_target_os(macos)
    (void)frame;
    return false;
#else
    if (frame.buffer == nullptr || frame.width <= 0 || frame.height <= 0 ||
        frame.format != DmaBufFormat::BGRA8_UNORM) {
        return false;
    }
    const rstd::size_t packed_stride = static_cast<rstd::size_t>(frame.width) * 4u;
    if (frame.row_stride < packed_stride ||
        static_cast<rstd::size_t>(frame.height) > SIZE_MAX / packed_stride) {
        return false;
    }
    const rstd::size_t paint_size = packed_stride * static_cast<rstd::size_t>(frame.height);
    auto               cpu        = cpu_paint_.lock().unwrap();
    cpu->data.resize(usize(paint_size), rstd::byte {});

    const auto* source = static_cast<const rstd::byte*>(frame.buffer);
    auto*       target = cpu->data.data();
    for (int y = 0; y < frame.height; ++y) {
        memcpy(target + static_cast<rstd::size_t>(y) * packed_stride,
               source + static_cast<rstd::size_t>(y) * frame.row_stride,
               rstd::usize(packed_stride));
    }
    cpu->width   = frame.width;
    cpu->height  = frame.height;
    cpu->pending = true;
    return true;
#endif
}

bool VulkanBlitter::AcceptDmaBuf(const DmaBufFrame& frame) {
    if (frame.plane_count < 1) return false;
    if (frame.coded_width <= 0 || frame.coded_height <= 0) return false;

    // Wait for *any* in-flight GPU work on owned_image_ from a prior
    // RenderFrame blit to finish. Single-threaded design, sub-ms cost.
    if (device_) device_dispatch_.vkDeviceWaitIdle(device_);

    if (! EnsureOwnedImage(frame.coded_width, frame.coded_height)) return false;

    // Dup the FD — vkAllocateMemory consumes ownership on success and
    // CEF reclaims its own FD when the callback returns.
    int dup_fd = ::dup(frame.planes[0].fd);
    if (dup_fd < 0) {
        eprint("weweb: dup(dmabuf fd) failed\n");
        return false;
    }

    // Look up which Vulkan memory types are valid for this FD.
    VkMemoryFdPropertiesKHR fd_props {};
    fd_props.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    VkResult fdr   = device_dispatch_.vkGetMemoryFdPropertiesKHR(
        device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dup_fd, &fd_props);
    if (fdr != VK_SUCCESS) {
        eprint("weweb: vkGetMemoryFdPropertiesKHR={}\n", static_cast<int>(fdr));
        ::close(dup_fd);
        return false;
    }

    // Create a temporary image that references the imported memory.
    // VK_EXT_image_drm_format_modifier is required on Mesa radv: a plain
    // TILING_LINEAR image with VK_EXT_external_memory_dma_buf isn't
    // enough — radv needs the explicit modifier + plane layout to map
    // the imported pages into the GPU page table correctly. CEF's
    // INVALID modifier (= no negotiated modifier; in practice the
    // implementation picked LINEAR with stride matching width*bpp) is
    // substituted with DRM_FORMAT_MOD_LINEAR (0).
    constexpr rstd::uint64_t DRM_FORMAT_MOD_INVALID = 0x00ffffffffffffffULL;
    constexpr rstd::uint64_t DRM_FORMAT_MOD_LINEAR  = 0x0;
    rstd::uint64_t           modifier =
        (frame.modifier == DRM_FORMAT_MOD_INVALID) ? DRM_FORMAT_MOD_LINEAR : frame.modifier;

    // VUID-VkImageDrmFormatModifierExplicitCreateInfoEXT-size-02267:
    // size must be 0; driver derives it from rowPitch + extent + format.
    VkSubresourceLayout plane_layout {};
    plane_layout.offset     = frame.planes[0].offset;
    plane_layout.rowPitch   = frame.planes[0].stride;
    plane_layout.arrayPitch = 0;
    plane_layout.depthPitch = 0;

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
    ii.extent        = { static_cast<rstd::uint32_t>(frame.coded_width),
                         static_cast<rstd::uint32_t>(frame.coded_height),
                         1 };
    ii.mipLevels     = 1;
    ii.arrayLayers   = 1;
    ii.samples       = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling        = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ii.usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage temp_img = VK_NULL_HANDLE;
    if (device_dispatch_.vkCreateImage(device_, &ii, nullptr, &temp_img) != VK_SUCCESS) {
        eprint("weweb: vkCreateImage(temp dma-buf) failed\n");
        ::close(dup_fd);
        return false;
    }

    VkMemoryRequirements mr {};
    device_dispatch_.vkGetImageMemoryRequirements(device_, temp_img, &mr);

    // Memory type must satisfy *both* the image's requirements AND the
    // FD's allowable types. ANDing memoryTypeBits accomplishes that.
    rstd::uint32_t allowed = mr.memoryTypeBits & fd_props.memoryTypeBits;
    rstd::uint32_t mtype   = u32::MAX.to_primitive();
    for (rstd::uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i) {
        if (allowed & (1u << i)) {
            mtype = i;
            break;
        }
    }
    if (mtype == u32::MAX.to_primitive()) {
        eprint("weweb: no compatible memory type for DMA-BUF\n");
        device_dispatch_.vkDestroyImage(device_, temp_img, nullptr);
        ::close(dup_fd);
        return false;
    }

    // Dedicated allocation for an imported DMA-BUF is the safe path.
    VkMemoryDedicatedAllocateInfo dedicated {};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = temp_img;

    VkImportMemoryFdInfoKHR import {};
    import.sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import.pNext      = &dedicated;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import.fd         = dup_fd;

    VkMemoryAllocateInfo mi {};
    mi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mi.pNext = &import;
    // Use the size CEF reported for the plane — Mesa ignores the
    // allocationSize for imports but the spec wants a valid-looking value.
    mi.allocationSize  = frame.planes[0].size > 0 ? frame.planes[0].size : mr.size;
    mi.memoryTypeIndex = mtype;

    VkDeviceMemory imported_mem = VK_NULL_HANDLE;
    VkResult       ar = device_dispatch_.vkAllocateMemory(device_, &mi, nullptr, &imported_mem);
    if (ar != VK_SUCCESS) {
        eprint("weweb: vkAllocateMemory(import fd)={}\n", static_cast<int>(ar));
        device_dispatch_.vkDestroyImage(device_, temp_img, nullptr);
        ::close(dup_fd);
        return false;
    }
    // From here on Vulkan owns the FD; we MUST NOT close dup_fd.

    if (device_dispatch_.vkBindImageMemory(
            device_, temp_img, imported_mem, frame.planes[0].offset) != VK_SUCCESS) {
        eprint("weweb: vkBindImageMemory(temp) failed\n");
        device_dispatch_.vkFreeMemory(device_, imported_mem, nullptr);
        device_dispatch_.vkDestroyImage(device_, temp_img, nullptr);
        return false;
    }

    // Copy temp_img → owned_image_. Use cmd_bufs_[0] as a scratch
    // buffer; we wait for it inline.
    VkCommandBuffer cmd = cmd_bufs_[0];
    device_dispatch_.vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (device_dispatch_.vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
        device_dispatch_.vkFreeMemory(device_, imported_mem, nullptr);
        device_dispatch_.vkDestroyImage(device_, temp_img, nullptr);
        return false;
    }

    // temp_img: UNDEFINED → TRANSFER_SRC_OPTIMAL.
    VkImageMemoryBarrier b_src {};
    b_src.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b_src.oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED;
    b_src.newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b_src.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    b_src.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    b_src.image                       = temp_img;
    b_src.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b_src.subresourceRange.levelCount = 1;
    b_src.subresourceRange.layerCount = 1;
    b_src.srcAccessMask               = 0;
    b_src.dstAccessMask               = VK_ACCESS_TRANSFER_READ_BIT;
    device_dispatch_.vkCmdPipelineBarrier(cmd,
                                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          0,
                                          0,
                                          nullptr,
                                          0,
                                          nullptr,
                                          1,
                                          &b_src);

    // owned_image_: <prev_layout> → TRANSFER_DST_OPTIMAL.
    VkImageMemoryBarrier b_dst = b_src;
    b_dst.oldLayout            = owned_layout_;
    b_dst.newLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b_dst.image                = owned_image_;
    b_dst.srcAccessMask        = 0;
    b_dst.dstAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
    device_dispatch_.vkCmdPipelineBarrier(cmd,
                                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          0,
                                          0,
                                          nullptr,
                                          0,
                                          nullptr,
                                          1,
                                          &b_dst);

    VkImageCopy region {};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.layerCount = 1;
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.layerCount = 1;
    region.extent                    = { static_cast<rstd::uint32_t>(frame.coded_width),
                                         static_cast<rstd::uint32_t>(frame.coded_height),
                                         1 };
    device_dispatch_.vkCmdCopyImage(cmd,
                                    temp_img,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    owned_image_,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    1,
                                    &region);

    // owned_image_: TRANSFER_DST → TRANSFER_SRC (so RenderFrame can read).
    VkImageMemoryBarrier b_owned_src {};
    b_owned_src.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b_owned_src.oldLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b_owned_src.newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b_owned_src.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    b_owned_src.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    b_owned_src.image                       = owned_image_;
    b_owned_src.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b_owned_src.subresourceRange.levelCount = 1;
    b_owned_src.subresourceRange.layerCount = 1;
    b_owned_src.srcAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT;
    b_owned_src.dstAccessMask               = VK_ACCESS_TRANSFER_READ_BIT;
    device_dispatch_.vkCmdPipelineBarrier(cmd,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          0,
                                          0,
                                          nullptr,
                                          0,
                                          nullptr,
                                          1,
                                          &b_owned_src);

    if (device_dispatch_.vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        device_dispatch_.vkFreeMemory(device_, imported_mem, nullptr);
        device_dispatch_.vkDestroyImage(device_, temp_img, nullptr);
        return false;
    }

    VkSubmitInfo submit {};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cmd;

    // Use the in_flight_fence_[0] which CreateSyncObjects signaled
    // initially. Reset, submit, then wait — must be done before this
    // function returns since CEF reclaims the FD.
    device_dispatch_.vkResetFences(device_, 1, &in_flight_fence_[0]);
    if (device_dispatch_.vkQueueSubmit(queue_, 1, &submit, in_flight_fence_[0]) != VK_SUCCESS) {
        eprint("weweb: vkQueueSubmit(import-copy) failed\n");
        device_dispatch_.vkFreeMemory(device_, imported_mem, nullptr);
        device_dispatch_.vkDestroyImage(device_, temp_img, nullptr);
        return false;
    }
    device_dispatch_.vkWaitForFences(device_, 1, &in_flight_fence_[0], VK_TRUE, kFenceTimeoutNs);

    device_dispatch_.vkDestroyImage(device_, temp_img, nullptr);
    device_dispatch_.vkFreeMemory(device_, imported_mem, nullptr);

    owned_layout_   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    owned_has_data_ = true;
    return true;
}

bool VulkanBlitter::RenderFrame() {
    if (! swapchain_ || extent_.width == 0 || extent_.height == 0) {
        return false;
    }

#if __is_target_os(macos)
    if (! UploadPendingCpuPaint()) return false;
#endif

    // Use cmd_bufs_[1] as the present cmd buffer to keep it disjoint
    // from the import-copy buffer at index 0.
    const rstd::uint32_t fi      = 1;
    VkFence              fence   = in_flight_fence_[fi];
    VkSemaphore          img_sem = img_avail_sem_[fi];
    VkCommandBuffer      cmd     = cmd_bufs_[fi];

    device_dispatch_.vkWaitForFences(device_, 1, &fence, VK_TRUE, kFenceTimeoutNs);

    rstd::uint32_t img_idx = 0;
    VkResult       acq     = device_dispatch_.vkAcquireNextImageKHR(
        device_, swapchain_, kFenceTimeoutNs, img_sem, VK_NULL_HANDLE, &img_idx);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) return false;
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        eprint("weweb: vkAcquireNextImageKHR={}\n", static_cast<int>(acq));
        return true;
    }
    VkSemaphore done_sem = render_done_sem_[usize(img_idx)];

    device_dispatch_.vkResetFences(device_, 1, &fence);
    device_dispatch_.vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo bi {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(device_dispatch_.vkBeginCommandBuffer(cmd, &bi));

    VkImage swap_img = swap_images_[usize(img_idx)];

    // swapchain_img: UNDEFINED → TRANSFER_DST.
    VkImageMemoryBarrier b {};
    b.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    b.image                       = swap_img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask               = 0;
    b.dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT;
    // srcStage matches the wait dstStage on img_sem (TRANSFER) so the
    // validator can chain "acquire-read → wait → barrier"; using
    // TOP_OF_PIPE produces a SYNC-HAZARD-WRITE-AFTER-READ.
    device_dispatch_.vkCmdPipelineBarrier(cmd,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          0,
                                          0,
                                          nullptr,
                                          0,
                                          nullptr,
                                          1,
                                          &b);

    if (owned_has_data_ && owned_image_ != VK_NULL_HANDLE) {
        VkImageBlit blit {};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1;
        blit.srcOffsets[0]             = { 0, 0, 0 };
        blit.srcOffsets[1]             = { owned_width_, owned_height_, 1 };
        blit.dstOffsets[0]             = { 0, 0, 0 };
        blit.dstOffsets[1]             = { static_cast<int32_t>(extent_.width),
                                           static_cast<int32_t>(extent_.height),
                                           1 };
        device_dispatch_.vkCmdBlitImage(cmd,
                                        owned_image_,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        swap_img,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        1,
                                        &blit,
                                        VK_FILTER_LINEAR);
    } else {
        VkClearColorValue       clear {};
        VkImageSubresourceRange r {};
        r.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.levelCount = 1;
        r.layerCount = 1;
        device_dispatch_.vkCmdClearColorImage(
            cmd, swap_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &r);
    }

    // swapchain_img: TRANSFER_DST → PRESENT_SRC.
    VkImageMemoryBarrier bp = b;
    bp.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bp.newLayout            = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    bp.srcAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
    bp.dstAccessMask        = 0;
    device_dispatch_.vkCmdPipelineBarrier(cmd,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                          0,
                                          0,
                                          nullptr,
                                          0,
                                          nullptr,
                                          1,
                                          &bp);

    VK_CHECK(device_dispatch_.vkEndCommandBuffer(cmd));

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo         submit {};
    submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount   = 1;
    submit.pWaitSemaphores      = &img_sem;
    submit.pWaitDstStageMask    = &wait_stage;
    submit.commandBufferCount   = 1;
    submit.pCommandBuffers      = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores    = &done_sem;
    VK_CHECK(device_dispatch_.vkQueueSubmit(queue_, 1, &submit, fence));

    VkPresentInfoKHR present {};
    present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores    = &done_sem;
    present.swapchainCount     = 1;
    present.pSwapchains        = &swapchain_;
    present.pImageIndices      = &img_idx;

    VkResult pres = device_dispatch_.vkQueuePresentKHR(queue_, &present);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) {
        return false;
    }
    return true;
}

} // namespace weweb
