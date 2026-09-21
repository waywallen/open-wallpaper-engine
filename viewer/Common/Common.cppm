module;

export module viewer.common;

import rstd;

export import :arg;

using namespace rstd::prelude;
using rstd::path::PathBuf;

export namespace viewer
{

PathBuf ExecutableDir(const char* argv0);
PathBuf DefaultCacheDir(ref<str> name);

void InitGlfwPlatformHint(bool force_x11);

} // namespace viewer
