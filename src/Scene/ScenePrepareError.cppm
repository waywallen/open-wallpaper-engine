module;
#include <rstd/enum.hpp>
export module wescene.scene_prepare_error;
import rstd;
export import wescene.vulkan_render;
using namespace rstd::prelude;
export namespace owe
{
using PrepareOutcome = vulkan::PrepareOutcome;
class ScenePrepareError {
    RSTD_ENUM(ScenePrepareError, (GraphBuild, (vrento::fg::Error source;)),
              (Renderer, (vulkan::RenderPrepareError source;)))
};
class ScenePrepareFailure {
    u64                     attempt_;
    vrento::ErrorDiagnostic error_;

public:
    ScenePrepareFailure(u64 attempt, const ScenePrepareError& error)
        : attempt_(attempt), error_(vrento::ErrorDiagnostic::capture(error)) {}
    auto attempt() const -> u64 { return attempt_; }
    auto error() const -> const vrento::ErrorDiagnostic& { return error_; }
};
} // namespace owe

export namespace rstd
{
template<>
struct Impl<fmt::Display, owe::ScenePrepareError> : ImplBase<owe::ScenePrepareError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        const auto& error = this->self();
        if (error.is_GraphBuild())
            return formatter.write_str(
                (rstd::format("scene graphbuild preparation failed")).as_str());
        if (error.is_Renderer())
            return formatter.write_str(
                (rstd::format("scene renderer preparation failed")).as_str());
        return false;
    }
};
template<>
struct Impl<fmt::Debug, owe::ScenePrepareError> : ImplBase<owe::ScenePrepareError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return as<fmt::Display>(this->self()).fmt(formatter);
    }
};
template<>
struct Impl<error::Error, owe::ScenePrepareError> : ImplBase<owe::ScenePrepareError> {
    auto source() const noexcept -> Option<error::ErrorRef> {
        const auto& value = this->self();
        if (value.is_GraphBuild())
            return Some(dyn<error::Error>::from_ref(value.as_GraphBuild().source));
        if (value.is_Renderer())
            return Some(dyn<error::Error>::from_ref(value.as_Renderer().source));
        return None();
    }
};
} // namespace rstd

export namespace rstd
{
template<>
struct Impl<convert::From<owe::vulkan::RenderPrepareError>, owe::ScenePrepareError> {
    static auto from(owe::vulkan::RenderPrepareError error) -> owe::ScenePrepareError {
        return owe::ScenePrepareError::Renderer(rstd::move(error));
    }
};
} // namespace rstd
