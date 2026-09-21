export module weweb:frame;

import rstd;

using namespace rstd::prelude;
using rstd::sync::Arc;

export namespace weweb
{

struct DmaBufPlane {
    int            fd { -1 };
    rstd::uint32_t stride { 0 };
    rstd::uint64_t offset { 0 };
    rstd::uint64_t size { 0 };
};

enum class DmaBufFormat : int
{
    BGRA8_UNORM = 0,
    RGBA8_UNORM = 1,
};

struct DmaBufFrame {
    DmaBufPlane    planes[4] {};
    int            plane_count { 0 };
    rstd::uint64_t modifier { 0 };
    DmaBufFormat   format { DmaBufFormat::BGRA8_UNORM };

    int coded_width { 0 };
    int coded_height { 0 };
    int visible_width { 0 };
    int visible_height { 0 };
};

struct CpuPaintFrame {
    const void*    buffer { nullptr };
    int            width { 0 };
    int            height { 0 };
    rstd::uint32_t row_stride { 0 };
    DmaBufFormat   format { DmaBufFormat::BGRA8_UNORM };
};

using AcceleratedPaintCallback = Arc<dyn<Fn<void(const DmaBufFrame&)>>>;
using CpuPaintCallback         = Arc<dyn<Fn<void(const CpuPaintFrame&)>>>;

using AudioDemandCallback = Arc<dyn<Fn<void(bool)>>>;

} // namespace weweb
