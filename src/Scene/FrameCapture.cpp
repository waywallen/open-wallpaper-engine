module wescene.frame_capture;
import wavsen.image;

using namespace rstd::prelude;
using namespace rstd::literals;

namespace owe
{

namespace image = wavsen::image;

static auto CaptureImageError(image::Error error) -> FrameCaptureError {
    switch (error) {
    case image::Error::InvalidInput:
        return { FrameCaptureErrorKind::InvalidInput, "invalid frame image input"_Str };
    case image::Error::Unsupported:
        return { FrameCaptureErrorKind::Unsupported, "PNG encoder is unavailable"_Str };
    case image::Error::ResourceLimit:
        return { FrameCaptureErrorKind::Encode, "PNG encoding exceeded its resource budget"_Str };
    case image::Error::Cancelled:
        return { FrameCaptureErrorKind::Cancelled, "PNG encoding was cancelled"_Str };
    case image::Error::Io:
        return { FrameCaptureErrorKind::Io, "PNG encoder input/output failed"_Str };
    case image::Error::Decode:
    case image::Error::Encode: return { FrameCaptureErrorKind::Encode, "PNG encoding failed"_Str };
    }
    return { FrameCaptureErrorKind::Encode, "unknown PNG encoding error"_Str };
}

auto WriteFrameCapturePng(const CapturedFrame& frame, ref<rstd::path::Path> path)
    -> Result<empty, FrameCaptureError> {
    constexpr u64 max_bytes { 256 * 1024 * 1024 };
    if (frame.width == u32() || frame.height == u32() || frame.rgba.is_empty())
        return Err(FrameCaptureError { FrameCaptureErrorKind::InvalidInput,
                                       "frame capture is empty"_Str });
    const auto row_bytes = u64(frame.width.to_primitive()) * u64(4);
    if (row_bytes > u64(0x7fffffff) || frame.height > u32(0x7fffffff) ||
        u64(frame.stride.to_primitive()) < row_bytes)
        return Err(FrameCaptureError { FrameCaptureErrorKind::InvalidInput,
                                       "frame capture dimensions or stride are invalid"_Str });

    // The stride check bounds both products by u32::MAX squared before multiplication.
    const auto packed_bytes = row_bytes * u64(frame.height.to_primitive());
    const auto backing_bytes =
        u64(frame.height.to_primitive() - 1) * u64(frame.stride.to_primitive()) + row_bytes;
    const auto owned_bytes = u64(frame.rgba.capacity().to_primitive());
    if (backing_bytes > u64(frame.rgba.len().to_primitive()) || owned_bytes > max_bytes ||
        packed_bytes > max_bytes)
        return Err(
            FrameCaptureError { FrameCaptureErrorKind::InvalidInput,
                                "frame capture backing storage is invalid or too large"_Str });

    const auto available = max_bytes - owned_bytes;
    if (packed_bytes >= available / u64(2))
        return Err(FrameCaptureError { FrameCaptureErrorKind::InvalidInput,
                                       "frame capture exceeds the 256 MiB encoding budget"_Str });
    // The encoder holds two RGBA copies and a packet before copying the encoded output.
    const auto output_limit = (available - packed_bytes * u64(2)) / u64(2);
    auto       budget =
        image::Budget::try_make(usize((packed_bytes * u64(2) + output_limit).to_primitive()));
    if (budget.is_err())
        return Err(FrameCaptureError { FrameCaptureErrorKind::Encode,
                                       "cannot allocate the PNG encoding budget"_Str });
    auto cancellation = image::Cancellation::make();
    if (cancellation.is_err()) return Err(CaptureImageError(cancellation.unwrap_err()));
    auto pixels = image::Pixels::allocate(budget->clone(), usize(packed_bytes.to_primitive()));
    if (pixels.is_err()) return Err(CaptureImageError(pixels.unwrap_err()));
    for (u32 y {}; y < frame.height; ++y) {
        const auto source = usize(y.to_primitive()) * usize(frame.stride.to_primitive());
        const auto target = usize(y.to_primitive()) * usize(row_bytes.to_primitive());
        rstd::mem::memcpy(pixels->data_mut() + target.to_primitive(),
                          frame.rgba.as_slice().as_raw_ptr() + source.to_primitive(),
                          usize(row_bytes.to_primitive()));
    }
    image::StillImage input { rstd::move(pixels).unwrap(),
                              u32(row_bytes.to_primitive()),
                              { frame.width,
                                frame.height,
                                image::PixelFormat::Rgba8,
                                image::Encoding::Png,
                                u32(1),
                                true },
                              None() };
    image::Limits     limits;
    limits.source_pixels = packed_bytes / u64(4);
    limits.output_bytes  = output_limit;
    auto encoded =
        image::encode(input, { image::Encoding::Png }, budget->clone(), *cancellation, limits);
    if (encoded.is_err()) return Err(CaptureImageError(encoded.unwrap_err()));
    if (encoded->size() == usize() || u64(encoded->size().to_primitive()) > output_limit)
        return Err(FrameCaptureError { FrameCaptureErrorKind::Encode,
                                       "PNG encoder returned invalid output storage"_Str });
    auto written = rstd::fs::write_atomic(
        path,
        slice<u8>::from_raw_parts(reinterpret_cast<const rstd::byte*>(encoded->data()),
                                  encoded->size()));
    if (written.is_err())
        return Err(FrameCaptureError {
            FrameCaptureErrorKind::Io,
            rstd::format("cannot write frame capture PNG: {}", written.unwrap_err()) });
    return Ok(empty {});
}

} // namespace owe
