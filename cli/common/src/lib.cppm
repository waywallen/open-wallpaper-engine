module;

export module owe.cli;

import rstd;

export import :audio;

#if defined(LITO_FEAT_WAYWALLEN) && __is_target_os(linux)
export import owe.cli.waywallen.bridge_producer_core;
#endif

using namespace rstd::prelude;
using rstd::path::PathBuf;

export namespace viewer
{

PathBuf ExecutableDir(const char* argv0);
PathBuf DefaultCacheDir(ref<str> name);

bool EnvEnabled(ref<str> name);

} // namespace viewer
