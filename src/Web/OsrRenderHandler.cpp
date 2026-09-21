module weweb;

import rstd;

import :cef;
import :cef_internal;

using namespace rstd::prelude;

namespace weweb
{

void OsrRenderHandler::SetAcceleratedPaintCallback(Option<AcceleratedPaintCallback> cb) {
    Option<AcceleratedPaintCallback> previous;
    {
        auto state      = state_.lock().unwrap();
        previous        = state->accel_cb.take();
        state->accel_cb = rstd::move(cb);
    }
}

void OsrRenderHandler::SetCpuPaintCallback(Option<CpuPaintCallback> cb) {
    Option<CpuPaintCallback> previous;
    {
        auto state    = state_.lock().unwrap();
        previous      = state->cpu_cb.take();
        state->cpu_cb = rstd::move(cb);
    }
}

void OsrRenderHandler::SetViewSize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    auto state    = state_.lock().unwrap();
    state->view_w = width;
    state->view_h = height;
}

void OsrRenderHandler::SetDeviceScaleFactor(float scale) {
    if (! f32(scale).is_finite() || scale <= 0.0f) return;
    auto state                 = state_.lock().unwrap();
    state->device_scale_factor = scale;
}

void OsrRenderHandler::GetViewRect(CefRefPtr<CefBrowser> /*browser*/, CefRect& rect) {
    auto state  = state_.lock().unwrap();
    rect.x      = 0;
    rect.y      = 0;
    rect.width  = state->view_w;
    rect.height = state->view_h;
}

bool OsrRenderHandler::GetScreenInfo(CefRefPtr<CefBrowser> /*browser*/, CefScreenInfo& info) {
    auto state               = state_.lock().unwrap();
    info.device_scale_factor = state->device_scale_factor;
    info.depth               = 32;
    info.depth_per_component = 8;
    info.is_monochrome       = false;
    info.rect.x              = 0;
    info.rect.y              = 0;
    info.rect.width          = state->view_w;
    info.rect.height         = state->view_h;
    info.available_rect      = info.rect;
    return true;
}

void OsrRenderHandler::OnPaint(CefRefPtr<CefBrowser> /*browser*/, PaintElementType type,
                               const RectList& /*dirtyRects*/, const void* buffer, int width,
                               int height) {
    if (type != PET_VIEW) return;
    if (! buffer || width <= 0 || height <= 0) return;
    Option<CpuPaintCallback> callback;
    {
        auto state = state_.lock().unwrap();
        callback   = state->cpu_cb.clone();
    }
    if (callback.is_none()) return;

    CpuPaintFrame frame;
    frame.buffer     = buffer;
    frame.width      = width;
    frame.height     = height;
    frame.row_stride = static_cast<uint32_t>(width) * 4u;
    frame.format     = DmaBufFormat::BGRA8_UNORM;
    (*callback)->operator()(frame);
}

void OsrRenderHandler::OnAcceleratedPaint(CefRefPtr<CefBrowser> /*browser*/, PaintElementType type,
                                          const RectList& /*dirtyRects*/,
                                          const CefAcceleratedPaintInfo& info) {
    if (type != PET_VIEW) return;
#if __is_target_os(macos)
    // CEF 149 exposes accelerated IOSurface metadata on macOS, but its
    // windowless shared-texture switch is not implemented for this platform.
    // The supported macOS path is OnPaint followed by a GPU upload.
    (void)info;
#else
    Option<AcceleratedPaintCallback> callback;
    {
        auto state = state_.lock().unwrap();
        callback   = state->accel_cb.clone();
    }
    if (callback.is_none()) return;

    DmaBufFrame frame;
    frame.plane_count = info.plane_count;
    if (frame.plane_count > 4) frame.plane_count = 4;
    for (int i = 0; i < frame.plane_count; ++i) {
        frame.planes[i].fd     = info.planes[i].fd;
        frame.planes[i].stride = info.planes[i].stride;
        frame.planes[i].offset = info.planes[i].offset;
        frame.planes[i].size   = info.planes[i].size;
    }
    frame.modifier       = info.modifier;
    frame.format         = info.format == CEF_COLOR_TYPE_BGRA_8888 ? DmaBufFormat::BGRA8_UNORM
                                                                   : DmaBufFormat::RGBA8_UNORM;
    frame.coded_width    = info.extra.coded_size.width;
    frame.coded_height   = info.extra.coded_size.height;
    frame.visible_width  = info.extra.visible_rect.width;
    frame.visible_height = info.extra.visible_rect.height;

    // Synchronous: callback must finish before this returns; CEF
    // reclaims the DMA-BUF the moment we exit.
    (*callback)->operator()(frame);
#endif
}

} // namespace weweb
