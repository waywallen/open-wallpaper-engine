export module waywallen.web_settings;

import rstd;

namespace waywallen::web_settings
{
using namespace rstd::prelude;

export template<typename T>
auto Parse(ref<str> text, T fallback) -> T {
    auto parsed = rstd::from_str<T>(text);
    if (parsed.is_err()) return fallback;
    if constexpr (rstd::mtp::same_as<T, f32>) {
        if (! parsed->is_finite()) return fallback;
    }
    return parsed.unwrap();
}

export template<typename T>
auto Parse(const char* text, T fallback) -> T {
    if (! text) return fallback;
    auto value = rstd::ffi::CStr::from_ptr(text).to_str();
    return value.is_ok() ? Parse(*value, fallback) : fallback;
}

} // namespace waywallen::web_settings
