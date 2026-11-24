module;

#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

module owe.cli.web.waywallen.web_producer_device;

import rstd;
import vvk;
import weweb;

using rstd::mem::memcpy;

using namespace rstd::prelude;
using rstd::ffi::CStr;
using rstd::ffi::CString;
using rstd::ffi::OsStr;
using rstd::io::eprint;
using rstd::io::error::Error;

namespace ww_wescene
{

namespace
{

#define VK_CHECK(expr)                                                                           \
    do {                                                                                         \
        VkResult _r = (expr);                                                                    \
        if (_r != VK_SUCCESS) {                                                                  \
            eprint("WebProducerDevice: {} failed (VkResult={}) at {}:{}\n",                      \
                   ref<OsStr>::from_encoded_bytes_unchecked(CStr::from_ptr(#expr).to_bytes())    \
                       .display(),                                                               \
                   static_cast<int>(_r),                                                         \
                   ref<OsStr>::from_encoded_bytes_unchecked(CStr::from_ptr(__FILE__).to_bytes()) \
                       .display(),                                                               \
                   __LINE__);                                                                    \
            return false;                                                                        \
        }                                                                                        \
    } while (0)

constexpr uint64_t kFenceTimeoutNs = 5'000'000'000ull; // 5s

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
            memcpy(out + y * dst_stride, src + y * src_stride, rstd::usize(dst_stride));
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

void WebProducerDevice::SetRenderNode(ref<str> path) { render_node_ = String::make(path); }

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
        eprint("WebProducerDevice: Vulkan loader open failed\n");
        return false;
    }
    loader_      = rstd::Some(loaded.unwrap_unchecked());
    auto created = vvk::Instance::Create(
        instance_owner_, loader_.as_ref().unwrap_unchecked().global(), ci, instance_dispatch_);
    if (created.is_err()) {
        const auto error = created.unwrap_err_unchecked();
        eprint("WebProducerDevice: instance creation failed ({}, VkResult={})\n",
               ref<OsStr>::from_encoded_bytes_unchecked(
                   CStr::from_ptr(error.command ? error.command : "dispatch").to_bytes())
                   .display(),
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
        eprint("WebProducerDevice: no Vulkan physical devices\n");
        return false;
    }
    Vec<VkPhysicalDevice> devs;
    devs.resize(usize(count), VkPhysicalDevice {});
    VK_CHECK(instance_dispatch_.vkEnumeratePhysicalDevices(instance_, &count, devs.data()));

    const bool pinning             = ! render_node_.is_empty();
    uint32_t   wanted_render_major = 0;
    uint32_t   wanted_render_minor = 0;
    auto       path = CString::make(Vec<u8>::from(render_node_.as_str().as_bytes())).unwrap();
    if (pinning) {
        struct stat st {};
        if (::stat(path.as_ptr(), &st) != 0) {
            const auto error = Error::last_os_error();
            eprint(
                "WebProducerDevice: stat({}) failed: {}\n",
                ref<OsStr>::from_encoded_bytes_unchecked(CStr::from_ptr(path.as_ptr()).to_bytes())
                    .display(),
                error);
            return false;
        }
        wanted_render_major = static_cast<uint32_t>(major(st.st_rdev));
        wanted_render_minor = static_cast<uint32_t>(minor(st.st_rdev));
    }

    for (auto pd : devs) {
        uint32_t qcount = 0;
        instance_dispatch_.vkGetPhysicalDeviceQueueFamilyProperties2(pd, &qcount, nullptr);
        Vec<VkQueueFamilyProperties2> qfp;
        qfp.resize(
            usize(qcount),
            VkQueueFamilyProperties2 { .sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2 });
        instance_dispatch_.vkGetPhysicalDeviceQueueFamilyProperties2(pd, &qcount, qfp.data());

        uint32_t picked_qf = rstd::u32::MAX.to_primitive();
        for (uint32_t i = 0; i < qcount; ++i) {
            // GRAPHICS_BIT implies TRANSFER_BIT; the bridge wants both
            // for vkCmdBlitImage and the producer needs no presentation
            // support because there's no surface.
            if (qfp[usize(i)].queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                picked_qf = i;
                break;
            }
        }
        if (picked_qf == rstd::u32::MAX.to_primitive()) continue;

        uint32_t ecount = 0;
        instance_dispatch_.vkEnumerateDeviceExtensionProperties(pd, nullptr, &ecount, nullptr);
        Vec<VkExtensionProperties> exts;
        exts.resize(usize(ecount), VkExtensionProperties {});
        instance_dispatch_.vkEnumerateDeviceExtensionProperties(pd, nullptr, &ecount, exts.data());

        bool has_ext_mem_fd = false, has_dma_buf = false, has_modifier = false,
             has_ext_sem_fd = false, has_q_foreign = false, has_fmt_list = false,
             has_drm_props = false;
        for (auto& e : exts) {
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
                 CStr::from_ptr(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME).to_bytes()))
                has_ext_sem_fd = true;
            if ((CStr::from_ptr(e.extensionName).to_bytes() ==
                 CStr::from_ptr(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME).to_bytes()))
                has_q_foreign = true;
            if ((CStr::from_ptr(e.extensionName).to_bytes() ==
                 CStr::from_ptr(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME).to_bytes()))
                has_fmt_list = true;
            if ((CStr::from_ptr(e.extensionName).to_bytes() ==
                 CStr::from_ptr(VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME).to_bytes()))
                has_drm_props = true;
        }
        // VK_EXT_image_drm_format_modifier is optional: without it CEF
        // frames are imported as VK_IMAGE_TILING_LINEAR.
        if (! has_ext_mem_fd || ! has_dma_buf || ! has_ext_sem_fd || ! has_q_foreign ||
            ! has_fmt_list)
            continue;

        if (! pinning) {
            // Never auto-pick a software rasterizer (llvmpipe has no render
            // node, so the bridge pool cannot be created on it).
            VkPhysicalDeviceProperties2 dtype {};
            dtype.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            instance_dispatch_.vkGetPhysicalDeviceProperties2(pd, &dtype);
            if (dtype.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        }

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

        phys_              = pd;
        queue_family_      = picked_qf;
        have_modifier_ext_ = has_modifier;
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
        memcpy(device_uuid_, id_props.deviceUUID, rstd::usize(16));
        memcpy(driver_uuid_, id_props.driverUUID, rstd::usize(16));
        return true;
    }
    if (pinning) {
        eprint("WebProducerDevice: no suitable physical device matching render_node {} (need "
               "external_memory_fd + dma_buf + modifier + external_semaphore_fd + "
               "queue_family_foreign + physical_device_drm)\n",
               ref<OsStr>::from_encoded_bytes_unchecked(CStr::from_ptr(path.as_ptr()).to_bytes())
                   .display());
    } else {
        eprint("WebProducerDevice: no suitable physical device (need external_memory_fd + dma_buf "
               "+ modifier + external_semaphore_fd + queue_family_foreign)\n");
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

    const char* dev_exts[6];
    uint32_t    dev_ext_count = 0;
    dev_exts[dev_ext_count++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
    dev_exts[dev_ext_count++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
    // Required by VK_EXT_image_drm_format_modifier on Vulkan 1.1
    // (promoted to core in 1.2). Validation rejects the device otherwise.
    dev_exts[dev_ext_count++] = VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME;
    if (have_modifier_ext_)
        dev_exts[dev_ext_count++] = VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME;
    dev_exts[dev_ext_count++] = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
    dev_exts[dev_ext_count++] = VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME;
    VkDeviceCreateInfo ci {};
    ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount    = 1;
    ci.pQueueCreateInfos       = &qi;
    ci.enabledExtensionCount   = dev_ext_count;
    ci.ppEnabledExtensionNames = dev_exts;

    auto created =
        vvk::Device::Create(device_owner_, phys_, instance_dispatch_, ci, device_dispatch_);
    if (created.is_err()) {
        const auto error = created.unwrap_err_unchecked();
        eprint("WebProducerDevice: device creation failed ({}, VkResult={})\n",
               ref<OsStr>::from_encoded_bytes_unchecked(
                   CStr::from_ptr(error.command ? error.command : "dispatch").to_bytes())
                   .display(),
               static_cast<int>(error.api_result));
        return false;
    }
    device_ = *device_owner_;
    device_dispatch_.vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    auto allocated = vvk::MemoryAllocator::Create(phys_, instance_dispatch_, device_dispatch_);
    if (allocated.is_err()) {
        eprint("WebProducerDevice: allocator creation failed\n");
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
        eprint("WebProducerDevice: CPU upload fence wait timed out\n");
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
        eprint("WebProducerDevice: staging allocation failed\n");
        return false;
    }
    cpu_staging_buffer_ = allocated.unwrap_unchecked();
    auto mapped         = cpu_staging_buffer_.allocation().map();
    if (mapped.is_err()) {
        eprint("WebProducerDevice: staging mapping failed\n");
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
        eprint("WebProducerDevice: prior {} fence wait timed out\n",
               ref<OsStr>::from_encoded_bytes_unchecked(CStr::from_ptr(op).to_bytes()).display());
        return false;
    }
    device_dispatch_.vkResetFences(device_, 1, &blit_fence_);
    device_dispatch_.vkResetCommandBuffer(blit_cmd_, 0);

    VkCommandBufferBeginInfo bi {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (device_dispatch_.vkBeginCommandBuffer(blit_cmd_, &bi) != VK_SUCCESS) {
        eprint("WebProducerDevice: vkBeginCommandBuffer({}) failed\n",
               ref<OsStr>::from_encoded_bytes_unchecked(CStr::from_ptr(op).to_bytes()).display());
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
        eprint("WebProducerDevice: vkEndCommandBuffer({}) failed\n",
               ref<OsStr>::from_encoded_bytes_unchecked(CStr::from_ptr(op).to_bytes()).display());
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
        eprint("WebProducerDevice: vkQueueSubmit({}) failed\n",
               ref<OsStr>::from_encoded_bytes_unchecked(CStr::from_ptr(op).to_bytes()).display());
        RestoreTransferFence();
        return -1;
    }

    VkSemaphoreGetFdInfoKHR gi {};
    gi.sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    gi.semaphore  = blit_sem_;
    gi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;

    int sync_fd = -1;
    if (device_dispatch_.vkGetSemaphoreFdKHR(device_, &gi, &sync_fd) != VK_SUCCESS || sync_fd < 0) {
        eprint("WebProducerDevice: vkGetSemaphoreFdKHR after {} failed\n",
               ref<OsStr>::from_encoded_bytes_unchecked(CStr::from_ptr(op).to_bytes()).display());
        const auto timeout = wait_for_completion ? u64::MAX.to_primitive() : kFenceTimeoutNs;
        device_dispatch_.vkWaitForFences(device_, 1, &blit_fence_, VK_TRUE, timeout);
        return -1;
    }

    if (wait_for_completion &&
        device_dispatch_.vkWaitForFences(
            device_, 1, &blit_fence_, VK_TRUE, u64::MAX.to_primitive()) != VK_SUCCESS) {
        ::close(sync_fd);
        return -1;
    }
    return sync_fd;
}

Option<::weweb::DmaBufImage> WebProducerDevice::Import(const ::weweb::DmaBufFrame& frame) {
    return ::weweb::DmaBufImage::Import(device_owner_, frame, have_modifier_ext_);
}

int WebProducerDevice::UploadToSlot(const ::weweb::CpuPaintFrame& frame, VkImage slot_image,
                                    VkExtent2D slot_extent, VkFormat slot_format) {
    if (! frame.buffer || slot_image == VK_NULL_HANDLE) return -1;
    if (frame.width <= 0 || frame.height <= 0) return -1;
    if (slot_format != VK_FORMAT_B8G8R8A8_UNORM && slot_format != VK_FORMAT_R8G8B8A8_UNORM) {
        eprint("WebProducerDevice: CPU paint unsupported slot format {}\n",
               static_cast<int>(slot_format));
        return -1;
    }
    if (slot_extent.width != static_cast<uint32_t>(frame.width) ||
        slot_extent.height != static_cast<uint32_t>(frame.height)) {
        eprint("WebProducerDevice: CPU paint extent mismatch frame={}x{} slot={}x{}\n",
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

int WebProducerDevice::BlitToSlot(const ::weweb::DmaBufImage& imported, VkImage slot_image,
                                  VkExtent2D slot_extent) {
    if (slot_image == VK_NULL_HANDLE || ! BeginTransferCommands("blit")) return -1;

    imported.Acquire(device_dispatch_, blit_cmd_, queue_family_);

    VkImageMemoryBarrier b_dst {};
    b_dst.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b_dst.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    b_dst.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    b_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b_dst.subresourceRange.levelCount = 1;
    b_dst.subresourceRange.layerCount = 1;
    b_dst.oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED;
    b_dst.newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b_dst.image                       = slot_image;
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

    VkImageBlit blit {};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.srcOffsets[0]             = { 0, 0, 0 };
    blit.srcOffsets[1]             = { static_cast<int32_t>(imported.extent().width),
                                       static_cast<int32_t>(imported.extent().height),
                                       1 };
    blit.dstOffsets[0]             = { 0, 0, 0 };
    blit.dstOffsets[1]             = { static_cast<int32_t>(slot_extent.width),
                                       static_cast<int32_t>(slot_extent.height),
                                       1 };
    device_dispatch_.vkCmdBlitImage(blit_cmd_,
                                    imported.image(),
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    slot_image,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    1,
                                    &blit,
                                    VK_FILTER_LINEAR);

    imported.Release(device_dispatch_, blit_cmd_, queue_family_);

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

    return SubmitTransferCommands("blit", /*wait_for_completion=*/true);
}

} // namespace ww_wescene
