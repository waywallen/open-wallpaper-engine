export module owe.cli.waywallen.render_resolution;

export import owe.cli.waywallen.bridge;
import rstd;

export namespace ww_wescene
{

struct RenderExtent {
    rstd::uint32_t width;
    rstd::uint32_t height;

    auto normalizePointer(float x, float y) const -> rstd::Option<rstd::array<double, 2>> {
        if (! width || ! height || ! (x >= 0.0f && y >= 0.0f && x <= width && y <= height))
            return rstd::None();
        return rstd::Some(rstd::array<double, 2> { double(x) / width, double(y) / height });
    }
};

struct RenderResolution {
    waywallen_render_target_capabilities_t capabilities;
    bool                                   has_origin;

    static RenderResolution describe(rstd::uint32_t width, rstd::uint32_t height, bool has_origin) {
        // A scene canvas defines coordinates, not a ceiling on generated pixel detail.
        return { { .revision         = 1,
                   .reference_width  = width,
                   .reference_height = height,
                   .mode             = 2,
                   .min_width        = 1,
                   .min_height       = 1,
                   .max_width        = width >= height ? 7680u : 4320u,
                   .max_height       = width >= height ? 4320u : 7680u,
                   .width_alignment  = 1,
                   .height_alignment = 1 },
                 has_origin };
    }

    RenderExtent resolve(rstd::int32_t resolution, rstd::uint32_t custom_extent) const {
        RenderExtent extent { capabilities.reference_width, capabilities.reference_height };
        const auto cap = resolution == WW_RESOLUTION_CUSTOM ? custom_extent
                                                            : ww_resolution_short_edge(resolution);
        if (has_origin && resolution != WW_RESOLUTION_AUTO) {
            ww_resolution_apply_short_edge(
                cap, WW_RESOLUTION_CAP_ALLOW_UPSCALE, &extent.width, &extent.height);
            return extent;
        }
        // Clamp the scale before rounding either dimension.
        rstd::uint64_t numerator = cap ? cap : 1;
        rstd::uint64_t denominator =
            cap ? (extent.width < extent.height ? extent.width : extent.height) : 1;
        if (extent.width * numerator > capabilities.max_width * denominator) {
            numerator   = capabilities.max_width;
            denominator = extent.width;
        }
        if (extent.height * numerator > capabilities.max_height * denominator) {
            numerator   = capabilities.max_height;
            denominator = extent.height;
        }
        extent.width =
            static_cast<rstd::uint32_t>((extent.width * numerator + denominator - 1) / denominator);
        extent.height = static_cast<rstd::uint32_t>((extent.height * numerator + denominator - 1) /
                                                    denominator);
        return extent;
    }
};

} // namespace ww_wescene
