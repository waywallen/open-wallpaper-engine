module;

module owe.cli;

import rstd;

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

bool EnvEnabled(ref<str> name) {
    const auto value = rstd::env::var_os(name);
    return value && ! value->is_empty() &&
           value->as_os_str().as_encoded_bytes()[usize()] == u8('1');
}

} // namespace viewer
