module;

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <wayland-egl.h>

module viewer.web;

import rstd;
import weweb;
import viewer.glfw_vulkan;

using namespace viewer::glfw;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::ffi::CStr;
using rstd::ffi::OsStr;
using rstd::io::eprint;

namespace weweb
{

namespace
{

// DRM fourcc codes — keep inline to avoid a libdrm header dependency.
// Mapping matches the user-supplied reference: CEF reverses RGBA/BGRA.
constexpr rstd::uint32_t kDrmFmtBgra8888 = 0x34324142; // 'BA24'
constexpr rstd::uint32_t kDrmFmtAbgr8888 = 0x34324241; // 'AB24'
// Used only by the init-time capability dump (NVIDIA may advertise a
// different alpha-channel ordering than what CEF actually emits).
constexpr rstd::uint32_t kDrmFmtArgb8888 = 0x34325241; // 'AR24'
constexpr rstd::uint32_t kDrmFmtXrgb8888 = 0x34325258; // 'XR24'

// 'invalid' modifier per drm_fourcc.h. Vulkan path treats it as LINEAR;
// for EGL the cleaner equivalent is to omit modifier attrs entirely so
// the driver picks its own layout from stride+fourcc.
constexpr rstd::uint64_t kDrmModInvalid = 0x00ffffffffffffffULL;

constexpr EGLint kPlaneFD[] = {
    EGL_DMA_BUF_PLANE0_FD_EXT,
    EGL_DMA_BUF_PLANE1_FD_EXT,
    EGL_DMA_BUF_PLANE2_FD_EXT,
    EGL_DMA_BUF_PLANE3_FD_EXT,
};
constexpr EGLint kPlaneOff[] = {
    EGL_DMA_BUF_PLANE0_OFFSET_EXT,
    EGL_DMA_BUF_PLANE1_OFFSET_EXT,
    EGL_DMA_BUF_PLANE2_OFFSET_EXT,
    EGL_DMA_BUF_PLANE3_OFFSET_EXT,
};
constexpr EGLint kPlanePitch[] = {
    EGL_DMA_BUF_PLANE0_PITCH_EXT,
    EGL_DMA_BUF_PLANE1_PITCH_EXT,
    EGL_DMA_BUF_PLANE2_PITCH_EXT,
    EGL_DMA_BUF_PLANE3_PITCH_EXT,
};
constexpr EGLint kPlaneModLo[] = {
    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
    EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
    EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
    EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
};
constexpr EGLint kPlaneModHi[] = {
    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
    EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
    EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
    EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
};

rstd::uint32_t DrmFourccFor(DmaBufFormat f) {
    switch (f) {
    case DmaBufFormat::BGRA8_UNORM: return kDrmFmtBgra8888;
    case DmaBufFormat::RGBA8_UNORM: return kDrmFmtAbgr8888;
    }
    return 0;
}

const char* FormatStr(DmaBufFormat f) {
    switch (f) {
    case DmaBufFormat::BGRA8_UNORM: return "BGRA8";
    case DmaBufFormat::RGBA8_UNORM: return "RGBA8";
    }
    return "?";
}

const char* EglErrStr(EGLint e) {
    switch (e) {
    case EGL_SUCCESS: return "SUCCESS";
    case EGL_NOT_INITIALIZED: return "NOT_INITIALIZED";
    case EGL_BAD_ACCESS: return "BAD_ACCESS";
    case EGL_BAD_ALLOC: return "BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE: return "BAD_ATTRIBUTE";
    case EGL_BAD_CONFIG: return "BAD_CONFIG";
    case EGL_BAD_CONTEXT: return "BAD_CONTEXT";
    case EGL_BAD_CURRENT_SURFACE: return "BAD_CURRENT_SURFACE";
    case EGL_BAD_DISPLAY: return "BAD_DISPLAY";
    case EGL_BAD_MATCH: return "BAD_MATCH";
    case EGL_BAD_NATIVE_PIXMAP: return "BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW: return "BAD_NATIVE_WINDOW";
    case EGL_BAD_PARAMETER: return "BAD_PARAMETER";
    case EGL_BAD_SURFACE: return "BAD_SURFACE";
    case EGL_CONTEXT_LOST: return "CONTEXT_LOST";
    default: return "?";
    }
}

const char* GlErrStr(GLenum e) {
    switch (e) {
    case GL_NO_ERROR: return "NO_ERROR";
    case GL_INVALID_ENUM: return "INVALID_ENUM";
    case GL_INVALID_VALUE: return "INVALID_VALUE";
    case GL_INVALID_OPERATION: return "INVALID_OPERATION";
    case GL_INVALID_FRAMEBUFFER_OPERATION: return "INVALID_FRAMEBUFFER_OPERATION";
    case GL_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
    default: return "?";
    }
}

const char* FboStatusStr(GLenum s) {
    switch (s) {
    case GL_FRAMEBUFFER_COMPLETE: return "COMPLETE";
    case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT: return "INCOMPLETE_ATTACHMENT";
    case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: return "INCOMPLETE_MISSING_ATTACHMENT";
    case GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS: return "INCOMPLETE_DIMENSIONS";
    case GL_FRAMEBUFFER_UNSUPPORTED: return "UNSUPPORTED";
    case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE: return "INCOMPLETE_MULTISAMPLE";
    default: return "?";
    }
}

bool DebugVerbose() {
    static bool v = rstd::env::var_os("WW_EGL_DEBUG"_str).is_some();
    return v;
}

} // namespace

EglPresenter::EglPresenter() = default;
EglPresenter::~EglPresenter() { Shutdown(); }

bool EglPresenter::Init(GLFWwindow* window) {
    window_ = window;

    const int platform       = glfwGetPlatform();
    void*     native_display = nullptr;
    EGLenum   egl_platform   = 0;

    if (platform == viewer::glfw::PlatformWayland) {
        native_display = glfwGetWaylandDisplay();
        egl_platform   = EGL_PLATFORM_WAYLAND_KHR;
    } else if (platform == viewer::glfw::PlatformX11) {
        native_display = glfwGetX11Display();
        egl_platform   = EGL_PLATFORM_X11_KHR;
    } else {
        eprint("weweb-egl: unsupported GLFW platform {} (need X11 or Wayland)\n", platform);
        return false;
    }
    if (! native_display) {
        eprint("weweb-egl: GLFW returned a null native display\n");
        return false;
    }

    egl_display_ = eglGetPlatformDisplay(egl_platform, native_display, nullptr);
    if (egl_display_ == EGL_NO_DISPLAY) {
        eprint("weweb-egl: eglGetPlatformDisplay({}) failed (0x{:x})\n",
               egl_platform == EGL_PLATFORM_WAYLAND_KHR ? "Wayland"_str : "X11"_str,
               eglGetError());
        return false;
    }

    EGLint egl_major = 0, egl_minor = 0;
    if (! eglInitialize(egl_display_, &egl_major, &egl_minor)) {
        eprint("weweb-egl: eglInitialize failed (0x{:x})\n", eglGetError());
        return false;
    }

    if (! eglBindAPI(EGL_OPENGL_ES_API)) {
        eprint("weweb-egl: eglBindAPI(GLES) failed (0x{:x})\n", eglGetError());
        return false;
    }

    const EGLint cfg_attribs[] = {
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE,
        EGL_WINDOW_BIT,
        EGL_RED_SIZE,
        8,
        EGL_GREEN_SIZE,
        8,
        EGL_BLUE_SIZE,
        8,
        EGL_ALPHA_SIZE,
        8,
        EGL_NONE,
    };
    EGLint num_cfg = 0;
    if (! eglChooseConfig(egl_display_, cfg_attribs, &egl_config_, 1, &num_cfg) || num_cfg < 1) {
        eprint("weweb-egl: eglChooseConfig found no RGBA8 window config\n");
        return false;
    }

    const EGLint ctx_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION,
        3,
        EGL_NONE,
    };
    egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT, ctx_attribs);
    if (egl_context_ == EGL_NO_CONTEXT) {
        eprint("weweb-egl: eglCreateContext(GLES3) failed (0x{:x})\n", eglGetError());
        return false;
    }

    int fbw_init = 0, fbh_init = 0;
    glfwGetFramebufferSize(window_, &fbw_init, &fbh_init);
    if (fbw_init <= 0) fbw_init = 1;
    if (fbh_init <= 0) fbh_init = 1;

    EGLNativeWindowType native_window = 0;
    if (platform == viewer::glfw::PlatformWayland) {
        wl_surface* wls = glfwGetWaylandWindow(window_);
        if (! wls) {
            eprint("weweb-egl: glfwGetWaylandWindow returned null\n");
            return false;
        }
        wl_egl_window_ = wl_egl_window_create(wls, fbw_init, fbh_init);
        if (! wl_egl_window_) {
            eprint("weweb-egl: wl_egl_window_create failed\n");
            return false;
        }
        native_window = reinterpret_cast<EGLNativeWindowType>(wl_egl_window_);
    } else {
        Window x_window = glfwGetX11Window(window_);
        if (! x_window) {
            eprint("weweb-egl: glfwGetX11Window returned null\n");
            return false;
        }
        native_window = static_cast<EGLNativeWindowType>(x_window);
    }

    egl_surface_ = eglCreateWindowSurface(egl_display_, egl_config_, native_window, nullptr);
    if (egl_surface_ == EGL_NO_SURFACE) {
        eprint("weweb-egl: eglCreateWindowSurface failed (0x{:x})\n", eglGetError());
        return false;
    }

    if (! eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_)) {
        eprint("weweb-egl: eglMakeCurrent failed (0x{:x})\n", eglGetError());
        return false;
    }

    if (! LoadFunctionPointers()) return false;

    {
        const char* plat_str    = (platform == viewer::glfw::PlatformWayland) ? "wayland"
                                  : (platform == viewer::glfw::PlatformX11)   ? "x11"
                                                                              : "?";
        const char* egl_vendor  = eglQueryString(egl_display_, EGL_VENDOR);
        const char* egl_version = eglQueryString(egl_display_, EGL_VERSION);
        const char* egl_apis    = eglQueryString(egl_display_, EGL_CLIENT_APIS);
        const char* egl_exts    = eglQueryString(egl_display_, EGL_EXTENSIONS);
        eprint(
            "weweb-egl: platform={} egl={}.{} vendor={}\n",
            ref<OsStr>::from_encoded_bytes_unchecked(CStr::from_ptr(plat_str).to_bytes()).display(),
            egl_major,
            egl_minor,
            ref<OsStr>::from_encoded_bytes_unchecked(
                CStr::from_ptr(egl_vendor ? egl_vendor : "?").to_bytes())
                .display());
        eprint("weweb-egl: EGL_VERSION = {}\n",
               ref<OsStr>::from_encoded_bytes_unchecked(
                   CStr::from_ptr(egl_version ? egl_version : "?").to_bytes())
                   .display());
        eprint("weweb-egl: EGL_CLIENT_APIS = {}\n",
               ref<OsStr>::from_encoded_bytes_unchecked(
                   CStr::from_ptr(egl_apis ? egl_apis : "?").to_bytes())
                   .display());
        const bool has_dmabuf = egl_exts && CStr::from_ptr(egl_exts).to_str().unwrap().contains(
                                                "EGL_EXT_image_dma_buf_import"_str);
        const bool has_dmabuf_mods =
            egl_exts && CStr::from_ptr(egl_exts).to_str().unwrap().contains(
                            "EGL_EXT_image_dma_buf_import_modifiers"_str);
        eprint("weweb-egl: EGL_EXT_image_dma_buf_import = {}\n",
               ref<OsStr>::from_encoded_bytes_unchecked(
                   CStr::from_ptr(has_dmabuf ? "yes" : "MISSING").to_bytes())
                   .display());
        eprint("weweb-egl: EGL_EXT_image_dma_buf_import_modifiers = {}\n",
               ref<OsStr>::from_encoded_bytes_unchecked(
                   CStr::from_ptr(has_dmabuf_mods ? "yes" : "no").to_bytes())
                   .display());
        if (DebugVerbose() && egl_exts) {
            eprint("weweb-egl: EGL_EXTENSIONS = {}\n",
                   ref<OsStr>::from_encoded_bytes_unchecked(CStr::from_ptr(egl_exts).to_bytes())
                       .display());
        }

        const auto* gl_vendor   = glGetString(GL_VENDOR);
        const auto* gl_renderer = glGetString(GL_RENDERER);
        const auto* gl_version  = glGetString(GL_VERSION);
        eprint("weweb-egl: GL_VENDOR   = {}\n",
               ref<OsStr>::from_encoded_bytes_unchecked(
                   CStr::from_ptr(gl_vendor ? reinterpret_cast<const char*>(gl_vendor) : "?")
                       .to_bytes())
                   .display());
        eprint("weweb-egl: GL_RENDERER = {}\n",
               ref<OsStr>::from_encoded_bytes_unchecked(
                   CStr::from_ptr(gl_renderer ? reinterpret_cast<const char*>(gl_renderer) : "?")
                       .to_bytes())
                   .display());
        eprint("weweb-egl: GL_VERSION  = {}\n",
               ref<OsStr>::from_encoded_bytes_unchecked(
                   CStr::from_ptr(gl_version ? reinterpret_cast<const char*>(gl_version) : "?")
                       .to_bytes())
                   .display());

        // Dump the modifier list NVIDIA's EGL will accept for the formats
        // CEF emits. Helps spot mismatches before the first import — e.g.
        // if NV only advertises NVIDIA-specific tilings and CEF will hand
        // us LINEAR, the import is doomed regardless of attribute layout.
        if (has_dmabuf_mods && fn_eglQueryDmaBufModifiersEXT_) {
            const rstd::uint32_t formats[] = {
                kDrmFmtArgb8888, kDrmFmtAbgr8888, kDrmFmtBgra8888, kDrmFmtXrgb8888
            };
            const char* fmt_names[] = { "ARGB8888", "ABGR8888", "BGRA8888", "XRGB8888" };
            for (rstd::size_t fi = 0; fi < (sizeof(formats) / sizeof(formats[0])); ++fi) {
                EGLint n = 0;
                if (! fn_eglQueryDmaBufModifiersEXT_(
                        egl_display_, static_cast<EGLint>(formats[fi]), 0, nullptr, nullptr, &n) ||
                    n <= 0) {
                    eprint("weweb-egl: {}: 0 modifiers (unsupported)\n",
                           ref<OsStr>::from_encoded_bytes_unchecked(
                               CStr::from_ptr(fmt_names[fi]).to_bytes())
                               .display());
                    continue;
                }
                Vec<EGLuint64KHR> mods;
                mods.resize(usize(n), EGLuint64KHR {});
                if (! fn_eglQueryDmaBufModifiersEXT_(egl_display_,
                                                     static_cast<EGLint>(formats[fi]),
                                                     n,
                                                     mods.data(),
                                                     nullptr,
                                                     &n)) {
                    continue;
                }
                eprint("weweb-egl: {}: {} modifiers",
                       ref<OsStr>::from_encoded_bytes_unchecked(
                           CStr::from_ptr(fmt_names[fi]).to_bytes())
                           .display(),
                       n);
                for (EGLint mi = 0; mi < n; ++mi) {
                    eprint(" 0x{:016x}", mods[usize(mi)]);
                }
                eprint("\n");
            }
        } else {
            eprint("weweb-egl: cannot query supported modifiers "
                   "(EGL_EXT_image_dma_buf_import_modifiers missing)\n");
        }
    }

    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(window_, &fbw, &fbh);
    width_  = static_cast<rstd::uint32_t>(fbw > 0 ? fbw : 0);
    height_ = static_cast<rstd::uint32_t>(fbh > 0 ? fbh : 0);

    glGenFramebuffers(1, &blit_read_fbo_);
    glGenFramebuffers(1, &blit_draw_fbo_);
    return true;
}

bool EglPresenter::LoadFunctionPointers() {
    fn_eglCreateImageKHR_ =
        reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    fn_eglDestroyImageKHR_ =
        reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    fn_glEGLImageTargetTexture2DOES_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (! fn_eglCreateImageKHR_ || ! fn_eglDestroyImageKHR_ || ! fn_glEGLImageTargetTexture2DOES_) {
        eprint("weweb-egl: missing required extension entry points (EGL_KHR_image_base + "
               "GL_OES_EGL_image)\n");
        return false;
    }
    // Optional: only used by the capability dump in Init.
    fn_eglQueryDmaBufModifiersEXT_ = reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(
        eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
    return true;
}

void EglPresenter::Shutdown() {
    if (egl_display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    DestroyOwnedTexture();
    if (blit_read_fbo_) {
        glDeleteFramebuffers(1, &blit_read_fbo_);
        blit_read_fbo_ = 0;
    }
    if (blit_draw_fbo_) {
        glDeleteFramebuffers(1, &blit_draw_fbo_);
        blit_draw_fbo_ = 0;
    }
    if (egl_surface_ != EGL_NO_SURFACE) {
        eglDestroySurface(egl_display_, egl_surface_);
        egl_surface_ = EGL_NO_SURFACE;
    }
    if (wl_egl_window_) {
        wl_egl_window_destroy(wl_egl_window_);
        wl_egl_window_ = nullptr;
    }
    if (egl_context_ != EGL_NO_CONTEXT) {
        eglDestroyContext(egl_display_, egl_context_);
        egl_context_ = EGL_NO_CONTEXT;
    }
    if (egl_display_ != EGL_NO_DISPLAY) {
        eglTerminate(egl_display_);
        egl_display_ = EGL_NO_DISPLAY;
    }
}

bool EglPresenter::Resize() {
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(window_, &fbw, &fbh);
    width_  = static_cast<rstd::uint32_t>(fbw > 0 ? fbw : 0);
    height_ = static_cast<rstd::uint32_t>(fbh > 0 ? fbh : 0);
    // Wayland: EGL won't notice the GLFW window resize on its own — the
    // wl_egl_window has to be told. X11 picks it up via the X server.
    if (wl_egl_window_ && fbw > 0 && fbh > 0) {
        wl_egl_window_resize(wl_egl_window_, fbw, fbh, 0, 0);
    }
    return true;
}

bool EglPresenter::EnsureOwnedTexture(int w, int h) {
    if (owned_tex_ && owned_w_ == w && owned_h_ == h) return true;
    DestroyOwnedTexture();

    glGenTextures(1, &owned_tex_);
    glBindTexture(GL_TEXTURE_2D, owned_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    owned_w_        = w;
    owned_h_        = h;
    owned_has_data_ = false;
    return true;
}

void EglPresenter::DestroyOwnedTexture() {
    if (owned_tex_) {
        glDeleteTextures(1, &owned_tex_);
        owned_tex_ = 0;
    }
    owned_w_ = owned_h_ = 0;
    owned_has_data_     = false;
}

bool EglPresenter::AcceptDmaBuf(const DmaBufFrame& frame) {
    if (frame.plane_count < 1) return false;
    if (frame.coded_width <= 0 || frame.coded_height <= 0) return false;

    const rstd::uint32_t fourcc = DrmFourccFor(frame.format);
    if (! fourcc) return false;

    const bool verbose    = DebugVerbose() || import_count_ == 0;
    auto       dump_frame = [&]() {
        eprint("weweb-egl: import #{} {}x{} (visible {}x{}) fmt={} fourcc=0x{:08x} "
               "mod=0x{:016x} planes={}\n",
               import_count_,
               frame.coded_width,
               frame.coded_height,
               frame.visible_width,
               frame.visible_height,
               ref<OsStr>::from_encoded_bytes_unchecked(
                   CStr::from_ptr(FormatStr(frame.format)).to_bytes())
                   .display(),
               fourcc,
               frame.modifier,
               frame.plane_count);
        for (int i = 0; i < frame.plane_count; ++i) {
            eprint("weweb-egl:   plane[{}] fd={} offset={} stride={} size={}\n",
                   i,
                   frame.planes[i].fd,
                   static_cast<unsigned long long>(frame.planes[i].offset),
                   frame.planes[i].stride,
                   static_cast<unsigned long long>(frame.planes[i].size));
        }
    };
    if (verbose) dump_frame();

    // NVIDIA's EGL refuses imports that omit the modifier attribute or
    // pass DRM_FORMAT_MOD_INVALID; Mesa is happy to derive layout from
    // stride alone. CEF reports INVALID when no modifier was negotiated —
    // in our observed cases that's effectively LINEAR (stride == width*bpp).
    // Mirror VulkanBlitter::AcceptDmaBuf and substitute LINEAR explicitly
    // so both backends agree and NVIDIA accepts the import.
    constexpr rstd::uint64_t kDrmModLinear = 0x0;
    const rstd::uint64_t     modifier =
        (frame.modifier == kDrmModInvalid) ? kDrmModLinear : frame.modifier;

    auto attrs = Vec<EGLint>::from(array<EGLint, 6> {
        EGL_WIDTH,
        static_cast<EGLint>(frame.coded_width),
        EGL_HEIGHT,
        static_cast<EGLint>(frame.coded_height),
        EGL_LINUX_DRM_FOURCC_EXT,
        static_cast<EGLint>(fourcc),
    }
                                       .as_slice());
    for (int i = 0; i < frame.plane_count; ++i) {
        attrs.emplace_back(kPlaneFD[i]);
        attrs.emplace_back(static_cast<EGLint>(frame.planes[i].fd));
        attrs.emplace_back(kPlaneOff[i]);
        attrs.emplace_back(static_cast<EGLint>(frame.planes[i].offset));
        attrs.emplace_back(kPlanePitch[i]);
        attrs.emplace_back(static_cast<EGLint>(frame.planes[i].stride));
        attrs.emplace_back(kPlaneModLo[i]);
        attrs.emplace_back(static_cast<EGLint>(modifier & 0xffffffffu));
        attrs.emplace_back(kPlaneModHi[i]);
        attrs.emplace_back(static_cast<EGLint>((modifier >> 32) & 0xffffffffu));
    }
    attrs.emplace_back(EGL_NONE);

    EGLImageKHR image = fn_eglCreateImageKHR_(egl_display_,
                                              EGL_NO_CONTEXT,
                                              EGL_LINUX_DMA_BUF_EXT,
                                              static_cast<EGLClientBuffer>(nullptr),
                                              attrs.data());
    if (image == EGL_NO_IMAGE_KHR) {
        EGLint e = eglGetError();
        eprint("weweb-egl: eglCreateImageKHR(DMA-BUF) failed: {} (0x{:x})\n",
               ref<OsStr>::from_encoded_bytes_unchecked(CStr::from_ptr(EglErrStr(e)).to_bytes())
                   .display(),
               e);
        if (! verbose) dump_frame();
        ++import_count_;
        return false;
    }

    GLuint temp_tex = 0;
    glGenTextures(1, &temp_tex);
    glBindTexture(GL_TEXTURE_2D, temp_tex);
    fn_glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, image);
    GLenum target_err = glGetError();
    if (verbose || target_err != GL_NO_ERROR) {
        eprint("weweb-egl: glEGLImageTargetTexture2DOES glerr={}\n",
               ref<OsStr>::from_encoded_bytes_unchecked(
                   CStr::from_ptr(GlErrStr(target_err)).to_bytes())
                   .display());
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (! EnsureOwnedTexture(frame.coded_width, frame.coded_height)) {
        glDeleteTextures(1, &temp_tex);
        fn_eglDestroyImageKHR_(egl_display_, image);
        ++import_count_;
        return false;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, blit_read_fbo_);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, temp_tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, blit_draw_fbo_);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, owned_tex_, 0);

    GLenum read_status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    GLenum draw_status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (verbose || read_status != GL_FRAMEBUFFER_COMPLETE ||
        draw_status != GL_FRAMEBUFFER_COMPLETE) {
        eprint("weweb-egl: import FBO read={} draw={}\n",
               ref<OsStr>::from_encoded_bytes_unchecked(
                   CStr::from_ptr(FboStatusStr(read_status)).to_bytes())
                   .display(),
               ref<OsStr>::from_encoded_bytes_unchecked(
                   CStr::from_ptr(FboStatusStr(draw_status)).to_bytes())
                   .display());
    }

    glBlitFramebuffer(0,
                      0,
                      frame.coded_width,
                      frame.coded_height,
                      0,
                      0,
                      frame.coded_width,
                      frame.coded_height,
                      GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);

    GLenum blit_err = glGetError();
    if (verbose || blit_err != GL_NO_ERROR) {
        eprint(
            "weweb-egl: import-blit glerr={}\n",
            ref<OsStr>::from_encoded_bytes_unchecked(CStr::from_ptr(GlErrStr(blit_err)).to_bytes())
                .display());
    }

    // Block until the GPU is done reading the imported buffer — CEF
    // reclaims the FD as soon as we return.
    glFinish();

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDeleteTextures(1, &temp_tex);
    fn_eglDestroyImageKHR_(egl_display_, image);

    owned_has_data_ = true;
    ++import_count_;
    return true;
}

bool EglPresenter::RenderFrame() {
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(window_, &fbw, &fbh);
    if (fbw <= 0 || fbh <= 0) return false;
    width_  = static_cast<rstd::uint32_t>(fbw);
    height_ = static_cast<rstd::uint32_t>(fbh);

    const bool verbose = DebugVerbose() || render_count_ == 0;
    if (verbose) {
        eprint("weweb-egl: present #{} fb={}x{} owned={}x{} has_data={}\n",
               render_count_,
               fbw,
               fbh,
               owned_w_,
               owned_h_,
               owned_has_data_ ? 1 : 0);
    }

    glViewport(0, 0, fbw, fbh);

    if (owned_has_data_ && owned_tex_) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, blit_read_fbo_);
        glFramebufferTexture2D(
            GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, owned_tex_, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        GLenum read_status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
        if (verbose || read_status != GL_FRAMEBUFFER_COMPLETE) {
            eprint("weweb-egl: present read FBO={}\n",
                   ref<OsStr>::from_encoded_bytes_unchecked(
                       CStr::from_ptr(FboStatusStr(read_status)).to_bytes())
                       .display());
        }
        // CEF data is top-down; default FB origin is bottom-left. Flip Y
        // by writing the dst Y range in reverse.
        glBlitFramebuffer(0, 0, owned_w_, owned_h_, 0, fbh, fbw, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        GLenum blit_err = glGetError();
        if (verbose || blit_err != GL_NO_ERROR) {
            eprint("weweb-egl: present-blit glerr={}\n",
                   ref<OsStr>::from_encoded_bytes_unchecked(
                       CStr::from_ptr(GlErrStr(blit_err)).to_bytes())
                       .display());
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    } else {
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    if (! eglSwapBuffers(egl_display_, egl_surface_)) {
        EGLint e = eglGetError();
        eprint("weweb-egl: eglSwapBuffers failed: {} (0x{:x})\n",
               ref<OsStr>::from_encoded_bytes_unchecked(CStr::from_ptr(EglErrStr(e)).to_bytes())
                   .display(),
               e);
        ++render_count_;
        return false;
    }
    ++render_count_;
    return true;
}

} // namespace weweb
