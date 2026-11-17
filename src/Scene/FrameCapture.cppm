export module wescene.frame_capture;
export import rstd;

using namespace rstd::prelude;

export namespace owe
{

enum class FrameCaptureErrorKind
{
    NotReady,
    Busy,
    Cancelled,
    Unsupported,
    Device,
    InvalidInput,
    Encode,
    Io
};

struct FrameCaptureError {
    FrameCaptureErrorKind kind;
    String                message;
};

struct CapturedFrame {
    u32 width {}, height {}, stride {};
    // Top-down RGBA8, straight alpha, with display-encoded color channels.
    Vec<u8> rgba;
};

using FrameCaptureResult   = Result<CapturedFrame, FrameCaptureError>;
using FrameCaptureCallback = Box<dyn<FnOnce<void(FrameCaptureResult)>>>;

auto WriteFrameCapturePng(const CapturedFrame&, ref<rstd::path::Path>)
    -> Result<empty, FrameCaptureError>;

} // namespace owe

export namespace rstd
{

template<>
struct Impl<fmt::Display, owe::FrameCaptureError> : ImplBase<owe::FrameCaptureError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return formatter.write_str(this->self().message.as_str());
    }
};

template<>
struct Impl<fmt::Debug, owe::FrameCaptureError> : ImplBase<owe::FrameCaptureError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return formatter.write_fmt(fmt::Arguments::make("FrameCaptureError(kind={}, message={})",
                                                        static_cast<int>(this->self().kind),
                                                        this->self().message));
    }
};

template<>
struct Impl<error::Error, owe::FrameCaptureError>
    : DefaultInImpl<error::Error, owe::FrameCaptureError> {};

} // namespace rstd
