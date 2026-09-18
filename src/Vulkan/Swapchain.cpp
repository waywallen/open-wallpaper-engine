module;

#include <rstd/macro.hpp>
#include "vvk/macros.hpp"

module wescene.vulkan;
import wescene.types;
import rstd;
import rstd.log;
import rstd.cppstd;

using namespace rstd::prelude;
using namespace owe::vulkan;

struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR           capabilities;
    rstd::vec::Vec<VkSurfaceFormatKHR> formats;
    rstd::vec::Vec<VkPresentModeKHR>   presentModes;
};

namespace
{

bool querySwapChainSupport(const vvk::PhysicalDevice& gpu, VkSurfaceKHR surface,
                           SwapChainSupportDetails& details) {
    VVK_CHECK_BOOL_RE(gpu.GetSurfaceCapabilitiesKHR(surface, details.capabilities));
    VVK_CHECK_BOOL_RE(gpu.GetSurfaceFormatsKHR(surface, details.formats));
    VVK_CHECK_BOOL_RE(gpu.GetSurfacePresentModesKHR(surface, details.presentModes));
    return true;
}

VkSurfaceFormatKHR chooseSwapSurfaceFormat(rstd::slice<VkSurfaceFormatKHR> availableFormats) {
    for (rstd::usize i {}; i < availableFormats.len(); ++i) {
        const auto& availableFormat = availableFormats[i];
        if (availableFormat.format == VK_FORMAT_B8G8R8A8_UNORM ||
            availableFormat.format == VK_FORMAT_R8G8B8A8_UNORM) {
            if (availableFormat.colorSpace == VK_COLORSPACE_SRGB_NONLINEAR_KHR)
                return availableFormat;
        }
    }
    auto& format = availableFormats[rstd::usize()];
    rstd_info("swapchain format: {}, color space: {}", format.format, format.colorSpace);
    return format;
}

VkExtent2D GetSwapChainExtent(VkSurfaceCapabilitiesKHR& surface_capabilities, VkExtent2D ext) {
    auto min     = surface_capabilities.minImageExtent;
    auto max     = surface_capabilities.maxImageExtent;
    auto currExt = surface_capabilities.currentExtent;

    if (currExt.width == 0 || currExt.width < min.width || currExt.width > max.width ||
        currExt.height < min.height || currExt.height > max.height) {
        if (ext.width < min.width) ext.width = min.width;
        if (ext.height < min.height) ext.height = min.height;
        if (ext.width > max.width) ext.width = max.width;
        if (ext.height > max.height) ext.height = max.height;
        return ext;
    }
    return surface_capabilities.currentExtent;
}

Option<vvk::ImageView> CreateSwapImageView(const vvk::Device& device, VkFormat format,
                                           VkImage image) {
    VkImageViewCreateInfo ci {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext    = nullptr,
        .image    = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = format,
        .subresourceRange =
            VkImageSubresourceRange {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
    };
    vvk::ImageView view;
    if (auto res = device.CreateImageView(ci, view); res == VK_SUCCESS) {
        return Some(rstd::move(view));
    }
    return None();
}
} // namespace

const vvk::SwapchainKHR& Swapchain::handle() const { return m_handle; }
VkFormat                 Swapchain::format() const { return m_format.format; }
VkExtent2D               Swapchain::extent() const { return m_extent; }

std::span<const ImageParameters> Swapchain::images() const { return m_images; }

VkPresentModeKHR Swapchain::presentMode() const { return m_present_mode; }

bool Swapchain::Create(Device& device, VkSurfaceKHR surface, VkExtent2D extent, Swapchain& swap) {
    SwapChainSupportDetails swap_details;
    if (! querySwapChainSupport(device.gpu(), surface, swap_details)) return false;

    swap.m_format = chooseSwapSurfaceFormat(swap_details.formats.as_slice());

    auto& surfaceCapabilities = swap_details.capabilities;

    rstd::uint32_t image_count = surfaceCapabilities.minImageCount + 1;
    if (surfaceCapabilities.maxImageCount > 0 && image_count > surfaceCapabilities.maxImageCount)
        image_count = surfaceCapabilities.maxImageCount;
#if ! __is_target_os(macos)
    surfaceCapabilities.currentExtent = swap.m_extent;
#endif
    swap.m_extent = GetSwapChainExtent(surfaceCapabilities, extent);

    swap.m_present_mode                          = VK_PRESENT_MODE_FIFO_KHR;
    VkSurfaceTransformFlagBitsKHR preTransform   = surfaceCapabilities.currentTransform;
    VkCompositeAlphaFlagBitsKHR   compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

    VkSwapchainCreateInfoKHR sci {
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext            = nullptr,
        .surface          = surface,
        .minImageCount    = image_count,
        .imageFormat      = swap.m_format.format,
        .imageColorSpace  = swap.m_format.colorSpace,
        .imageExtent      = swap.m_extent,
        .imageArrayLayers = 1,
        .imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform     = preTransform,
        .compositeAlpha   = compositeAlpha,
        .presentMode      = swap.m_present_mode,
        .clipped          = true,
        .oldSwapchain     = nullptr,
    };

    VVK_CHECK_BOOL_RE(device.device().CreateSwapchainKHR(sci, swap.m_handle));
    {
        rstd::vec::Vec<VkImage> images;
        VVK_CHECK_BOOL_RE(swap.m_handle.GetImages(images));
        std::transform(
            images.begin(), images.end(), std::back_inserter(swap.m_images), [&](auto image) {
                ImageParameters image_paras {};
                image_paras.handle = image;
                image_paras.extent = { swap.m_extent.width, swap.m_extent.height, 1 };
                if (auto opt = CreateSwapImageView(device.device(), swap.m_format.format, image);
                    opt.is_some()) {
                    swap.m_imageviews.emplace_back(rstd::move(opt).unwrap());
                    image_paras.view = *swap.m_imageviews.back();
                }
                return image_paras;
            });
    }
    return true;
}
