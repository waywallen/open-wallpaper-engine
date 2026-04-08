#pragma once
#include "TripleSwapchain.hpp"
#include <cstdint>

namespace wallpaper
{

enum class TexTiling
{
    OPTIMAL,
    LINEAR
};

struct ExHandle {
    int         fd { -1 };
    int32_t     width { 0 };
    int32_t     height { 0 };
    std::size_t size { 0 };

    // DMA-BUF import metadata, populated by VulkanExSwapchain from the
    // backing ExImageParameters at swapchain construction time. These
    // fields are what the waywallen-renderer host forwards to the daemon
    // in the IPC `BindBuffers` message (see host/proto.hpp).
    //
    // drm_fourcc:     DRM format code, e.g. DRM_FORMAT_ABGR8888.
    // drm_modifier:   DRM_FORMAT_MOD_LINEAR (0) today; other modifiers
    //                 come online when the OPTIMAL tiling path lands.
    // plane0_offset:  Offset of plane 0 inside the buffer allocation.
    // plane0_stride:  Plane 0 row pitch in bytes.
    uint32_t drm_fourcc { 0 };
    uint64_t drm_modifier { 0 };
    uint64_t plane0_offset { 0 };
    uint32_t plane0_stride { 0 };

    ExHandle() = default;
    ExHandle(int id): m_id(id) {};

    int32_t id() const { return m_id; }

private:
    int32_t m_id { 0 };
};

// class ExSwapchain : public TripleSwapchain<ExHandle> {};
using ExSwapchain = TripleSwapchain<ExHandle>;
} // namespace wallpaper
