module;
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#if defined(__linux__)
#    define GLFW_EXPOSE_NATIVE_X11
#    define GLFW_EXPOSE_NATIVE_WAYLAND
#    include <GLFW/glfw3native.h>
#endif

export module viewer.glfw_vulkan;

import vvk;

// GLFW's Vulkan declarations require imported vvk types, unavailable in the global fragment.
extern "C" {
void     glfwInitVulkanLoader(PFN_vkGetInstanceProcAddr loader);
VkResult glfwCreateWindowSurface(VkInstance instance, GLFWwindow* window,
                                 const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface);
}

export namespace viewer::glfw
{
using ::glfwCreateWindow;
using ::glfwCreateWindowSurface;
using ::glfwDestroyWindow;
using ::glfwGetCursorPos;
using ::glfwGetFramebufferSize;
using ::glfwGetMouseButton;
using ::glfwGetRequiredInstanceExtensions;
using ::glfwGetWindowContentScale;
using ::glfwGetWindowSize;
using ::glfwGetWindowUserPointer;
using ::glfwInit;
using ::glfwInitHint;
using ::glfwInitVulkanLoader;
using ::glfwPollEvents;
using ::glfwSetCursorEnterCallback;
using ::glfwSetCursorPos;
using ::glfwSetCursorPosCallback;
using ::glfwSetFramebufferSizeCallback;
using ::glfwSetMouseButtonCallback;
using ::glfwSetScrollCallback;
using ::glfwSetWindowFocusCallback;
using ::glfwSetWindowUserPointer;
using ::glfwTerminate;
using ::glfwVulkanSupported;
using ::glfwWaitEventsTimeout;
using ::GLFWwindow;
using ::glfwWindowHint;
using ::glfwWindowShouldClose;
inline constexpr int Press                  = GLFW_PRESS;
inline constexpr int Release                = GLFW_RELEASE;
inline constexpr int TrueValue              = GLFW_TRUE;
inline constexpr int FalseValue             = GLFW_FALSE;
inline constexpr int ClientApi              = GLFW_CLIENT_API;
inline constexpr int NoApi                  = GLFW_NO_API;
inline constexpr int Visible                = GLFW_VISIBLE;
inline constexpr int CocoaRetinaFramebuffer = GLFW_COCOA_RETINA_FRAMEBUFFER;
inline constexpr int MouseButtonLeft        = GLFW_MOUSE_BUTTON_LEFT;
inline constexpr int MouseButtonMiddle      = GLFW_MOUSE_BUTTON_MIDDLE;
inline constexpr int MouseButtonRight       = GLFW_MOUSE_BUTTON_RIGHT;
inline constexpr int Platform               = GLFW_PLATFORM;
inline constexpr int PlatformX11            = GLFW_PLATFORM_X11;
inline constexpr int PlatformWayland        = GLFW_PLATFORM_WAYLAND;
#if defined(__linux__)
using ::glfwGetPlatform;
using ::glfwGetWaylandDisplay;
using ::glfwGetWaylandWindow;
using ::glfwGetX11Display;
using ::glfwGetX11Window;
using ::Window;
#endif
} // namespace viewer::glfw
