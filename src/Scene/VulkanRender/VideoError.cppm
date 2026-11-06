module;
#include <rstd/enum.hpp>
export module wescene.video_error;
import rstd;
import vrento;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace owe::video
{
enum class MediaOperation
{
    Prepare,
    Open,
    Seek,
    Decode,
    Import,
    Convert
};
class MediaCause {
    RSTD_ENUM(MediaCause, (Texture, (vrento::texture::Error error;)), (Decoder, (String message;)),
              (Invalid, (String detail;)), (Backend, (vrento::vulkan::BackendError error;)))
};
struct MediaError {
    MediaOperation operation;
    MediaCause     cause;
    static auto    texture(vrento::texture::Error error,
                           MediaOperation         operation = MediaOperation::Prepare) -> MediaError {
        return { operation, MediaCause::Texture(rstd::move(error)) };
    }
    static auto decoder(MediaOperation operation, String message) -> MediaError {
        return { operation, MediaCause::Decoder(rstd::move(message)) };
    }
};
} // namespace owe::video
export namespace rstd
{
template<>
struct Impl<fmt::Display, owe::video::MediaCause> : ImplBase<owe::video::MediaCause> {
    auto fmt(fmt::Formatter& f) const -> bool {
        const auto& c = this->self();
        if (c.is_Texture()) return as<fmt::Display>(c.as_Texture().error).fmt(f);
        if (c.is_Backend()) return as<fmt::Display>(c.as_Backend().error).fmt(f);
        if (c.is_Decoder())
            return f.write_fmt(fmt::Arguments::make("decoder: {}", c.as_Decoder().message));
        return f.write_fmt(fmt::Arguments::make("invalid media: {}", c.as_Invalid().detail));
    }
};
template<>
struct Impl<fmt::Debug, owe::video::MediaCause> : ImplBase<owe::video::MediaCause> {
    auto fmt(fmt::Formatter& f) const -> bool { return as<fmt::Display>(this->self()).fmt(f); }
};
template<>
struct Impl<error::Error, owe::video::MediaCause> : ImplBase<owe::video::MediaCause> {
    auto source() const noexcept -> Option<error::ErrorRef> {
        const auto& c = this->self();
        if (c.is_Texture()) return Some(dyn<error::Error>::from_ref(c.as_Texture().error));
        if (c.is_Backend()) return Some(dyn<error::Error>::from_ref(c.as_Backend().error));
        return None();
    }
};
template<>
struct Impl<fmt::Display, owe::video::MediaError> : ImplBase<owe::video::MediaError> {
    auto fmt(fmt::Formatter& f) const -> bool {
        return f.write_fmt(
            fmt::Arguments::make("media operation={} failed", int(this->self().operation)));
    }
};
template<>
struct Impl<fmt::Debug, owe::video::MediaError> : ImplBase<owe::video::MediaError> {
    auto fmt(fmt::Formatter& f) const -> bool { return as<fmt::Display>(this->self()).fmt(f); }
};
template<>
struct Impl<error::Error, owe::video::MediaError> : ImplBase<owe::video::MediaError> {
    auto source() const noexcept -> Option<error::ErrorRef> {
        return Some(dyn<error::Error>::from_ref(this->self().cause));
    }
};
} // namespace rstd
