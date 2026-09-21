#include <rstd/macro.hpp>

import rstd;
import vvk;
import viewer.web;
import viewer.common;

#include "GlfwVulkan.hpp"

int main() {
    auto loaded = vvk::VulkanLoader::Open();
    rstd_assert(loaded.is_ok());
    auto loader = loaded.unwrap_unchecked();
    viewer::InitGlfwPlatformHint(false);
    glfwInitVulkanLoader(loader.global().vkGetInstanceProcAddr);
    rstd_assert(glfwInit());
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    auto* window = glfwCreateWindow(128, 96, "Vulkan presenter test", nullptr, nullptr);
    rstd_assert(window != nullptr);
    {
        weweb::VulkanBlitter presenter;
        rstd_assert(presenter.Init(window));
        rstd_assert(presenter.Width() > 0 && presenter.Height() > 0);
        rstd_assert(presenter.RenderFrame());
        rstd_assert(presenter.RenderFrame());
        glfwSetWindowSize(window, 192, 128);
        glfwPollEvents();
        rstd_assert(presenter.Resize());
        rstd_assert(presenter.RenderFrame());
        presenter.Shutdown();
        presenter.Shutdown();
        rstd_assert(presenter.Init(window));
        rstd_assert(presenter.RenderFrame());
    }
    glfwDestroyWindow(window);
    glfwTerminate();
}
