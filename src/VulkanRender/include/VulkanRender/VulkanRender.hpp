#pragma once

#include "RenderGraph/RenderGraph.hpp"
#include "SceneWallpaperSurface.hpp"
#include "Swapchain/ExSwapchain.hpp"
#include "Type.hpp"

#include <cstdio>
#include <memory>

namespace wallpaper
{
class Scene;

namespace vulkan
{
class FinPass;

class VulkanRender {
public:
    VulkanRender();
    ~VulkanRender();

    bool init(RenderInitInfo);

    void destroy();

    void drawFrame(Scene&);

    void clearLastRenderGraph();
    void compileRenderGraph(Scene&, rg::RenderGraph&);
    void UpdateCameraFillMode(Scene&, wallpaper::FillMode);

    ExSwapchain* exSwapchain() const;
    bool inited() const;

    // Transfer ownership of the most recent frame's exported dma_fence
    // sync_file fd. Returns -1 if no frame has been rendered since the
    // last call (or export failed). Caller owns the returned fd and
    // must close() it. Thread-safe.
    int takeLastFrameSyncFd();

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};
} // namespace vulkan
} // namespace wallpaper