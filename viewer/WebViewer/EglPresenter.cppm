module;

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

struct wl_egl_window;

export module viewer.web:egl_presenter;

import weweb;
import :presenter;
import viewer.glfw_vulkan;

using viewer::glfw::GLFWwindow;

export namespace weweb
{

// EGL + GLES3 alternative to VulkanBlitter. Imports each CEF DMA-BUF
// frame into an EGLImage, blits it onto a persistent owned texture, then
// presents that texture to the GLFW window via glBlitFramebuffer +
// eglSwapBuffers. Picks X11 or Wayland EGL platform at runtime to match
// the active GLFW backend.
//
// Lifetime semantics match VulkanBlitter: AcceptDmaBuf calls glFinish
// before returning, so CEF can safely reclaim the FD once the
// OnAcceleratedPaint callback returns.
class EglPresenter : public Presenter {
public:
    EglPresenter();
    ~EglPresenter() override;

    EglPresenter(const EglPresenter&)            = delete;
    EglPresenter& operator=(const EglPresenter&) = delete;

    bool Init(GLFWwindow* window) override;
    void Shutdown() override;

    rstd::uint32_t Width() const override { return width_; }
    rstd::uint32_t Height() const override { return height_; }

    bool Resize() override;
    bool AcceptDmaBuf(const DmaBufFrame& frame) override;
    bool RenderFrame() override;

private:
    bool LoadFunctionPointers();
    bool EnsureOwnedTexture(int w, int h);
    void DestroyOwnedTexture();

    GLFWwindow* window_ { nullptr };

    EGLDisplay egl_display_ { EGL_NO_DISPLAY };
    EGLContext egl_context_ { EGL_NO_CONTEXT };
    EGLSurface egl_surface_ { EGL_NO_SURFACE };
    EGLConfig  egl_config_ { nullptr };

    // Only set on Wayland: the wl_egl_window wrapping the GLFW wl_surface.
    // EGL window surface tracks size via wl_egl_window_resize on Resize().
    wl_egl_window* wl_egl_window_ { nullptr };

    PFNEGLCREATEIMAGEKHRPROC            fn_eglCreateImageKHR_ { nullptr };
    PFNEGLDESTROYIMAGEKHRPROC           fn_eglDestroyImageKHR_ { nullptr };
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC fn_glEGLImageTargetTexture2DOES_ { nullptr };
    // Optional: only present if EGL_EXT_image_dma_buf_import_modifiers
    // is exposed. Used at Init for a one-shot capability dump.
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC fn_eglQueryDmaBufModifiersEXT_ { nullptr };

    GLuint owned_tex_ { 0 };
    GLuint blit_read_fbo_ { 0 };
    GLuint blit_draw_fbo_ { 0 };
    int    owned_w_ { 0 };
    int    owned_h_ { 0 };
    bool   owned_has_data_ { false };

    rstd::uint32_t width_ { 0 };
    rstd::uint32_t height_ { 0 };

    unsigned import_count_ { 0 };
    unsigned render_count_ { 0 };
};

} // namespace weweb
