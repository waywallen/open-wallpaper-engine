module;

module viewer.common;

import rstd;
import viewer.glfw_vulkan;

using namespace viewer::glfw;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::ffi::CStr;
using rstd::os::unix::ffi::OsStrExt;
using rstd::path::Path;
using rstd::path::PathBuf;

namespace viewer
{

PathBuf ExecutableDir(const char* argv0) {
    auto executable = rstd::fs::read_link("/proc/self/exe"_str);
    auto path       = executable.is_ok() ? executable.unwrap()
                                         : PathBuf::from(ref<Path>(OsStrExt::from_bytes(
                                               CStr::from_ptr(argv0 ? argv0 : "").to_bytes())));
    auto parent     = path.as_path().parent();
    return parent ? PathBuf::from(*parent) : PathBuf {};
}

PathBuf DefaultCacheDir(ref<str> name) {
    auto cache = rstd::env::var_os("XDG_CACHE_HOME"_str);
    if (cache && ! cache->is_empty()) return PathBuf::from(cache.unwrap()).join(name);
    auto home = rstd::env::var_os("HOME"_str);
    if (home && ! home->is_empty())
        return PathBuf::from(home.unwrap()).join(".cache"_str).join(name);
    return {};
}

void InitGlfwPlatformHint(bool force_x11) {
    if (force_x11) {
        glfwInitHint(viewer::glfw::Platform, viewer::glfw::PlatformX11);
        return;
    }
    auto x11_env = rstd::env::var_os("WP_GLFW_X11"_str);
    if (x11_env && ! x11_env->is_empty() &&
        x11_env->as_os_str().as_encoded_bytes()[usize()] == u8('1')) {
        glfwInitHint(viewer::glfw::Platform, viewer::glfw::PlatformX11);
    }
}

} // namespace viewer
