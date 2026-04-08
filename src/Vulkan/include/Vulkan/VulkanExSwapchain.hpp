#pragma once

#include "Swapchain/ExSwapchain.hpp"
#include "Device.hpp"
#include <cstdio>

namespace wallpaper
{
namespace vulkan
{

struct VulkanExHandle : NoCopy {
    ExHandle          handle;
    ExImageParameters image;

    VulkanExHandle()  = default;
    ~VulkanExHandle() = default;
    VulkanExHandle(VulkanExHandle&& o) noexcept: handle(o.handle), image(std::move(o.image)) {}
    VulkanExHandle& operator=(VulkanExHandle&& o) noexcept {
        handle = o.handle;
        image  = std::move(o.image);
        return *this;
    }
};
struct VulkanExHandleSemaphore {
    ExHandle    handle;
    VkSemaphore semaphore;
};

class VulkanExSwapchain : public ExSwapchain {
    using atomic_ = std::atomic<ExHandle*>;

public:
    VulkanExSwapchain(std::array<VulkanExHandle, 3> handles, VkExtent2D ext)
        : m_handles(std::move(handles)), m_extent(ext) {
        int index = 0;
        for (auto& h : m_handles) {
            auto& handle         = h.handle;
            handle               = ExHandle(index++);
            handle.width         = (i32)h.image.extent.width;
            handle.height        = (i32)h.image.extent.height;
            handle.fd            = h.image.fd;
            handle.size          = h.image.mem_reqs.size;
            handle.drm_fourcc    = h.image.drm_fourcc;
            handle.drm_modifier  = h.image.drm_modifier;
            handle.plane0_offset = h.image.plane0_offset;
            handle.plane0_stride = h.image.plane0_stride;
        }
        m_presented  = &m_handles[0].handle;
        m_ready      = &m_handles[1].handle;
        m_inprogress = &m_handles[2].handle;
    }

    // Iteration 1b — synchronization story (documented, not yet code):
    //
    // This swapchain is a triple-buffer ring. `eatFrame()` returns the
    // most recently *completed* render target, and the producer is already
    // writing into a different slot by the time a consumer picks it up.
    // That provides *implicit* ordering between the rendering queue submit
    // and the consumer read, because the triple-buffer guarantees the
    // producer cannot wrap all the way back to the slot a consumer still
    // holds within a single frame.
    //
    // Consequently, iteration 1a ships with implicit sync: the IPC
    // `FrameReady` event is a "this slot's rendering is done, you may
    // read it" signal, and consumers do not receive a semaphore/sync_file
    // FD. If tearing shows up in practice (e.g. a consumer that imports
    // the DMA-BUF into its own Vulkan instance and renders from it), the
    // follow-on work is:
    //   1. Add `VK_KHR_external_semaphore_fd` to the offscreen device
    //      extension list.
    //   2. Create one VkSemaphore per ExImage with a
    //      VkExportSemaphoreCreateInfo chain.
    //   3. Signal each semaphore on the queue submit that finishes the
    //      corresponding frame.
    //   4. Export a fresh sync_file FD via vkGetSemaphoreFdKHR(
    //      VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT_KHR) on each
    //      `eatFrame()` and pass it as a second SCM_RIGHTS fd on the
    //      `FrameReady` message.
    // We deliberately defer that work until there is observable tearing
    // to validate it against, because building it blind risks shipping a
    // fence semantics bug that's hard to reproduce.
    virtual ~VulkanExSwapchain() = default;

    uint width() const override { return m_extent.width; }
    uint height() const override { return m_extent.height; }

    const auto& handles() const { return m_handles; }

    ExImageParameters& GetInprogressImage() {
        return m_handles.at((usize)(*inprogress()).id()).image;
    }

    constexpr VkFormat format() const { return VK_FORMAT_R8G8B8A8_UNORM; };

protected:
    atomic_& presented() override { return m_presented; };
    atomic_& ready() override { return m_ready; };
    atomic_& inprogress() override { return m_inprogress; };

private:
    std::array<VulkanExHandle, 3> m_handles;
    atomic_                       m_presented { nullptr };
    atomic_                       m_ready { nullptr };
    atomic_                       m_inprogress { nullptr };
    VkExtent2D                    m_extent;
};

inline std::unique_ptr<VulkanExSwapchain> CreateExSwapchain(const Device& device, uint w, uint h,
                                                            VkImageTiling tiling) {
    std::array<VulkanExHandle, 3> handles;
    for (auto& handle : handles) {
        if (auto rv = device.tex_cache().CreateExTex(w, h, VK_FORMAT_R8G8B8A8_UNORM, tiling);
            rv.has_value())
            handle.image = std::move(rv.value());
        else
            return nullptr;
    }
    /*
    VulkanExHandleSemaphore handle_sem;
    {
        vk::SemaphoreCreateInfo info;
        vk::ExportSemaphoreCreateInfo esci { vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueFd };
        info.setPNext(&esci);
        VK_CHECK_RESULT_ACT(return nullptr, device.handle().createSemaphore(&info, nullptr,
    &handle_sem.semaphore)); vk::SemaphoreGetFdInfoKHR fd_info; fd_info.semaphore =
    handle_sem.semaphore; fd_info.handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueFd;
        VK_CHECK_RESULT_ACT(return nullptr, device.handle().getSemaphoreFdKHR(&fd_info,
    &handle_sem.handle.fd));
    }*/
    return std::make_unique<VulkanExSwapchain>(std::move(handles), VkExtent2D { w, h });
}

} // namespace vulkan
} // namespace wallpaper
