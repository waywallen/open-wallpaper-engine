#pragma once

// Import vvk before this header. GLFW gates its Vulkan declarations on a macro,
// while Vulkan types and constants come from the module.
#define GLFW_INCLUDE_NONE
#define VK_VERSION_1_0 1
extern "C++" {
#include <GLFW/glfw3.h>
}
#undef VK_VERSION_1_0
