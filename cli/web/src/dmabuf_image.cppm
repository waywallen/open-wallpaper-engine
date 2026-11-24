module;

#if __is_target_os(linux)
#    include <sys/stat.h>
#    include <unistd.h>
#endif

export module owe.cli.web.dmabuf_image;

#if __is_target_os(linux)
import rstd;
import vvk;
import weweb;

using namespace rstd::prelude;
using rstd::io::eprint;
using rstd::os::fd::BorrowedFd;

export namespace weweb
{

class DmaBufImage {
public:
    DmaBufImage(DmaBufImage&&) = default;

    static Option<DmaBufImage> Import(const vvk::Device& device, const DmaBufFrame& frame,
                                      bool explicit_modifier) {
        VkSubresourceLayout layouts[4] {};
        if (frame.plane_count < 1 || frame.plane_count > 4 || frame.coded_width <= 0 ||
            frame.coded_height <= 0) {
            return None<DmaBufImage>();
        }

        constexpr rstd::uint64_t kInvalidModifier = 0x00ffffffffffffffULL;
        const auto               modifier = frame.modifier == kInvalidModifier ? 0 : frame.modifier;
        if ((! explicit_modifier || frame.modifier == kInvalidModifier) &&
            (modifier != 0 || frame.plane_count != 1)) {
            eprint("weweb: DMA-BUF requires an explicit modifier (0x{:x}, {} planes)\n",
                   frame.modifier,
                   frame.plane_count);
            return None<DmaBufImage>();
        }

        const auto fd = frame.planes[0].fd;
        if (frame.plane_count > 1) {
            struct stat first {};
            if (::fstat(fd, &first) != 0) {
                eprint("weweb: fstat of CEF DMA-BUF failed\n");
                return None<DmaBufImage>();
            }
            for (int i = 1; i < frame.plane_count; ++i) {
                struct stat plane {};
                if (::fstat(frame.planes[i].fd, &plane) != 0 || plane.st_dev != first.st_dev ||
                    plane.st_ino != first.st_ino) {
                    eprint("weweb: CEF DMA-BUF planes must share one allocation\n");
                    return None<DmaBufImage>();
                }
            }
        }
        for (int i = 0; i < frame.plane_count; ++i) {
            layouts[i].offset   = frame.planes[i].offset;
            layouts[i].rowPitch = frame.planes[i].stride;
        }

        const auto size = ::lseek(fd, 0, SEEK_END);
        if (size <= 0) {
            eprint("weweb: cannot determine CEF DMA-BUF size\n");
            return None<DmaBufImage>();
        }
        (void)::lseek(fd, 0, SEEK_SET);

        DmaBufImage output;
        output.extent_ = { static_cast<rstd::uint32_t>(frame.coded_width),
                           static_cast<rstd::uint32_t>(frame.coded_height) };
        output.format_ = frame.format == DmaBufFormat::BGRA8_UNORM ? VK_FORMAT_B8G8R8A8_UNORM
                                                                   : VK_FORMAT_R8G8B8A8_UNORM;

        VkImageDrmFormatModifierExplicitCreateInfoEXT drm {};
        drm.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
        drm.drmFormatModifier           = modifier;
        drm.drmFormatModifierPlaneCount = static_cast<rstd::uint32_t>(frame.plane_count);
        drm.pPlaneLayouts               = layouts;

        constexpr auto handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkExternalMemoryImageCreateInfo external {};
        external.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        external.pNext       = explicit_modifier ? &drm : nullptr;
        external.handleTypes = handle_type;

        VkImageCreateInfo info {};
        info.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.pNext       = &external;
        info.imageType   = VK_IMAGE_TYPE_2D;
        info.format      = output.format_;
        info.extent      = { output.extent_.width, output.extent_.height, 1 };
        info.mipLevels   = 1;
        info.arrayLayers = 1;
        info.samples     = VK_SAMPLE_COUNT_1_BIT;
        info.tiling =
            explicit_modifier ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_LINEAR;
        info.usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (auto result = device.CreateImage(info, output.image_); result != VK_SUCCESS) {
            eprint("weweb: vkCreateImage(CEF DMA-BUF)={} (modifier 0x{:x}, {} planes)\n",
                   static_cast<int>(result),
                   modifier,
                   frame.plane_count);
            return None<DmaBufImage>();
        }
        if (! explicit_modifier) {
            const auto layout = device.GetImageSubresourceLayout(
                output.image(), { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 });
            if (layout.offset != layouts[0].offset || layout.rowPitch != layouts[0].rowPitch) {
                eprint("weweb: CEF DMA-BUF does not match the implicit linear layout\n");
                return None<DmaBufImage>();
            }
        }

        const auto requirements    = device.GetImageMemoryRequirements(output.image());
        const auto allocation_size = static_cast<VkDeviceSize>(size);
        if (allocation_size < requirements.size) {
            eprint("weweb: CEF DMA-BUF is smaller than the Vulkan image allocation\n");
            return None<DmaBufImage>();
        }
        auto duplicate = BorrowedFd::borrow_raw(fd).try_clone_to_owned();
        if (duplicate.is_err()) {
            eprint("weweb: failed to duplicate CEF DMA-BUF fd\n");
            return None<DmaBufImage>();
        }
        auto                    owned_fd = rstd::move(duplicate).unwrap_unchecked();
        VkMemoryFdPropertiesKHR properties {};
        properties.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
        if (auto result =
                device.GetMemoryFdPropertiesKHR(handle_type, owned_fd.as_raw_fd(), properties);
            result != VK_SUCCESS) {
            eprint("weweb: vkGetMemoryFdPropertiesKHR(CEF DMA-BUF)={}\n", static_cast<int>(result));
            return None<DmaBufImage>();
        }
        const auto compatible = requirements.memoryTypeBits & properties.memoryTypeBits;
        if (compatible == 0) {
            eprint("weweb: no compatible memory type for CEF DMA-BUF\n");
            return None<DmaBufImage>();
        }
        VkMemoryDedicatedAllocateInfo dedicated {};
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.image = output.image();
        VkImportMemoryFdInfoKHR import {};
        import.sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
        import.pNext      = &dedicated;
        import.handleType = handle_type;
        import.fd         = owned_fd.as_raw_fd();
        VkMemoryAllocateInfo allocate {};
        allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.pNext           = &import;
        allocate.allocationSize  = allocation_size;
        allocate.memoryTypeIndex = u32(compatible).trailing_zeros().to_primitive();
        if (auto result = device.AllocateMemory(allocate, output.memory_); result != VK_SUCCESS) {
            eprint("weweb: vkAllocateMemory(CEF DMA-BUF)={}\n", static_cast<int>(result));
            return None<DmaBufImage>();
        }
        (void)rstd::move(owned_fd).into_raw_fd();

        // The explicit plane layouts already include their offsets into the allocation.
        if (auto result = output.image_.BindMemory(*output.memory_, 0); result != VK_SUCCESS) {
            eprint("weweb: vkBindImageMemory(CEF DMA-BUF)={}\n", static_cast<int>(result));
            return None<DmaBufImage>();
        }
        return Some(rstd::move(output));
    }

    VkImage    image() const { return *image_; }
    VkFormat   format() const { return format_; }
    VkExtent2D extent() const { return extent_; }

    void Acquire(const vvk::DeviceDispatch& dispatch, VkCommandBuffer cmd,
                 rstd::uint32_t queue_family) const {
        // Acquiring from GENERAL preserves the producer's pixels and compression metadata.
        const VkImageMemoryBarrier barrier {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
            .dstQueueFamilyIndex = queue_family,
            .image               = image(),
            .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        dispatch.vkCmdPipelineBarrier(cmd,
                                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      0,
                                      0,
                                      nullptr,
                                      0,
                                      nullptr,
                                      1,
                                      &barrier);
    }

    void Release(const vvk::DeviceDispatch& dispatch, VkCommandBuffer cmd,
                 rstd::uint32_t queue_family) const {
        const VkImageMemoryBarrier barrier {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = queue_family,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
            .image               = image(),
            .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        dispatch.vkCmdPipelineBarrier(cmd,
                                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                      0,
                                      0,
                                      nullptr,
                                      0,
                                      nullptr,
                                      1,
                                      &barrier);
    }

private:
    DmaBufImage() = default;

    vvk::DeviceMemory memory_;
    vvk::Image        image_;
    VkExtent2D        extent_ {};
    VkFormat          format_ { VK_FORMAT_UNDEFINED };
};

} // namespace weweb
#endif
