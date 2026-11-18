export module wescene.render_cli;

import rstd;
import rstd.argparse;

using namespace rstd::prelude;
using namespace rstd::literals;
using namespace rstd::argparse;
using rstd::path::Path;
using rstd::path::PathBuf;

export namespace owe::cli
{

struct Resolution {
    unsigned w;
    unsigned h;
};

struct ParsedResolution {
    Resolution value;
    bool       valid;
};

auto ResolutionParser(ref<rstd::ffi::OsStr> raw) -> Result<ParsedResolution, ValueError> {
    auto text = raw.to_str();
    if (text.is_none()) return Err(ValueError::InvalidUtf8());

    auto parts = text->split_once("x"_str);
    if (! parts) return Ok(ParsedResolution { { 1280, 720 }, false });
    auto [width_text, height_text] = *parts;
    if (width_text.starts_with("+"_str) || height_text.starts_with("+"_str))
        return Ok(ParsedResolution { { 1280, 720 }, false });
    auto width  = rstd::from_str<u32>(width_text);
    auto height = rstd::from_str<u32>(height_text);
    if (width.is_err() || height.is_err()) return Ok(ParsedResolution { { 1280, 720 }, false });
    const bool valid =
        *width > u32() && *height > u32() && *width <= u32(65535) && *height <= u32(65535) &&
        u64(width->to_primitive()) * u64(height->to_primitive()) <= u64(64 * 1024 * 1024);
    return Ok(ParsedResolution { { width->to_primitive(), height->to_primitive() }, valid });
}

auto ScreenshotPathParser(ref<rstd::ffi::OsStr> raw) -> Result<PathBuf, ValueError> {
    if (raw.is_empty())
        return Err(ValueError::Message("screenshot output path must not be empty"_Str));
    for (auto byte : raw.as_encoded_bytes()) {
        if (byte == u8())
            return Err(ValueError::Message("screenshot output path contains a NUL byte"_Str));
    }
    auto path = PathBuf::from(raw);
    if (path.as_path().file_name().is_none())
        return Err(ValueError::Message("screenshot output path must name a file"_Str));
    return Ok(rstd::move(path));
}

auto ScreenshotTimeoutParser(ref<rstd::ffi::OsStr> raw) -> Result<f64, ValueError> {
    auto text = raw.to_str();
    if (text.is_none()) return Err(ValueError::InvalidUtf8());
    auto value = rstd::from_str<f64>(*text);
    if (value.is_err() || ! value->is_finite() || *value <= f64() || *value > f64(86400) ||
        rstd::time::Duration::from_secs_f64(value->to_primitive()).is_zero())
        return Err(ValueError::Message(
            "screenshot timeout must be positive and at most 86400 seconds"_Str));
    return Ok(*value);
}

auto ValidateScreenshotOutput(ref<Path> path) -> Result<empty, String> {
    auto target = rstd::fs::metadata(path);
    if (target.is_ok() && ! target->is_file())
        return Err(rstd::format("'{}' is not a regular file", path.as_os_str().display()));
    if (auto parent = path.parent(); parent && ! parent->is_empty()) {
        auto directory = rstd::fs::metadata(*parent);
        if (directory.is_err() || ! directory->is_dir())
            return Err(rstd::format("parent directory '{}' does not exist or is not a directory",
                                    parent->as_os_str().display()));
    }
    return Ok(empty {});
}

} // namespace owe::cli
