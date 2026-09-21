import vvk;

#include "GlfwVulkan.hpp"

#if __is_target_os(macos)

#    include <CoreGraphics/CoreGraphics.h>
#    include <dispatch/dispatch.h>
#    include <objc/message.h>
#    include <objc/objc.h>
#    include <objc/runtime.h>
#    include <pthread.h>
#    include <cmath>

struct GLFWwindow;
extern "C" void* glfwGetCocoaView(GLFWwindow*);

namespace
{

template<typename Return, typename... Arguments>
Return Send(id object, SEL selector, Arguments... arguments) {
    using Function = Return (*)(id, SEL, Arguments...);
    return reinterpret_cast<Function>(objc_msgSend)(object, selector, arguments...);
}

void ConfigureLayer(GLFWwindow* window, int fallback_width, int fallback_height) {
    if (window == nullptr || fallback_width <= 0 || fallback_height <= 0) return;

    id view = reinterpret_cast<id>(glfwGetCocoaView(window));
    if (view == nil) return;

    int framebuffer_width  = 0;
    int framebuffer_height = 0;
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    glfwGetWindowContentScale(window, &scale_x, &scale_y);
    if (scale_x <= 0.0f) scale_x = 1.0f;
    if (scale_y <= 0.0f) scale_y = scale_x;
    if (framebuffer_width <= 0) {
        framebuffer_width = static_cast<int>(std::lround(fallback_width * scale_x));
    }
    if (framebuffer_height <= 0) {
        framebuffer_height = static_cast<int>(std::lround(fallback_height * scale_y));
    }

    // GLFW has already created and attached the CAMetalLayer while creating
    // the Vulkan surface. Reuse that exact layer; never replace view.layer
    // after vkCreateMetalSurfaceEXT has captured it.
    id    layer             = Send<id>(view, sel_registerName("layer"));
    Class metal_layer_class = objc_getClass("CAMetalLayer");
    if (layer == nil || metal_layer_class == Nil ||
        ! Send<BOOL>(layer, sel_registerName("isKindOfClass:"), metal_layer_class)) {
        return;
    }

    const CGRect bounds = Send<CGRect>(view, sel_registerName("bounds"));
    Send<void>(layer, sel_registerName("setFrame:"), bounds);
    Send<void>(layer, sel_registerName("setContentsScale:"), CGFloat(scale_x));
    Send<void>(layer,
               sel_registerName("setDrawableSize:"),
               CGSizeMake(CGFloat(framebuffer_width), CGFloat(framebuffer_height)));
}

struct LayerConfigureContext {
    GLFWwindow* window;
    int         width;
    int         height;
};

void ConfigureLayerDispatch(void* opaque) {
    auto& context = *static_cast<LayerConfigureContext*>(opaque);
    ConfigureLayer(context.window, context.width, context.height);
}

extern "C" void oweConfigureGlfwCocoaLayer(GLFWwindow* window, int width, int height) {
    if (window == nullptr) return;
    LayerConfigureContext context { window, width, height };
    if (pthread_main_np() != 0) {
        ConfigureLayerDispatch(&context);
    } else {
        dispatch_sync_f(dispatch_get_main_queue(), &context, ConfigureLayerDispatch);
    }
}

struct SurfaceCreateContext {
    GLFWwindow*   window;
    VkInstance    instance;
    VkSurfaceKHR* surface;
    VkResult      result;
    int           width;
    int           height;
};

void CreateSurfaceOnMain(void* opaque) {
    auto& context = *static_cast<SurfaceCreateContext*>(opaque);
    context.result =
        glfwCreateWindowSurface(context.instance, context.window, nullptr, context.surface);
    if (context.result == VK_SUCCESS) {
        oweConfigureGlfwCocoaLayer(context.window, context.width, context.height);
    }
}

} // namespace

extern "C" VkResult oweCreateGlfwCocoaSurface(GLFWwindow* window, VkInstance instance,
                                              VkSurfaceKHR* surface, int width, int height) {
    if (window == nullptr || surface == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    SurfaceCreateContext context {
        .window   = window,
        .instance = instance,
        .surface  = surface,
        .result   = VK_ERROR_INITIALIZATION_FAILED,
        .width    = width,
        .height   = height,
    };
    if (pthread_main_np() != 0) {
        CreateSurfaceOnMain(&context);
    } else {
        dispatch_sync_f(dispatch_get_main_queue(), &context, CreateSurfaceOnMain);
    }
    return context.result;
}

#else

struct GLFWwindow;
extern "C" VkResult oweCreateGlfwCocoaSurface(GLFWwindow*, VkInstance, VkSurfaceKHR*, int, int) {
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

#endif
