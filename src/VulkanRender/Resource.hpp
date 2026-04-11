#pragma once
#include "Core/NoCopyMove.hpp"
#include "Vulkan/StagingBuffer.hpp"
#include <memory>

namespace wallpaper
{
namespace vulkan
{

struct RenderingResources {
    vvk::CommandBuffer command;

    vvk::Semaphore sem_swap_wait_image;
    vvk::Semaphore sem_swap_finish;
    // Exportable (SYNC_FD) semaphore signaled on every offscreen frame
    // submit. The host exports it via vkGetSemaphoreFdKHR and ships the
    // resulting dma_fence sync_file to the waywallen daemon on
    // FrameReady events.
    vvk::Semaphore sem_export;
    vvk::Fence     fence_frame;

    StagingBuffer* vertex_buf;
    StagingBuffer* dyn_buf;
};
} // namespace vulkan
} // namespace wallpaper
