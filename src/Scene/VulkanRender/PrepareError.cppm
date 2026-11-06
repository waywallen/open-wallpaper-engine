module;
#include <rstd/enum.hpp>
export module wescene.vulkan_render:prepare_error;
import rstd;
export import vrento;
import wescene.scene;
using namespace rstd::prelude;
using namespace rstd::literals;

export namespace owe::vulkan
{
enum class PrepareOutcome
{
    Ready,
    Pending,
    RebuildRequired
};
enum class PrepareStateError
{
    NotInitialized,
    MissingSession,
    FrameUpdateActive,
    NoSceneState,
    MissingGraphics,
    InvalidSections,
    NoTransaction,
    NoTarget
};
enum class ResourcePrepareOperation
{
    Begin,
    Continue,
    SharedBuffers,
    Graphics,
    Commit,
    Video,
    Target
};
enum class DrawResourceKind
{
    Output,
    Geometry,
    Shader,
    Pipeline,
    Texture,
    UniformBuffer,
    Layout
};
inline auto PrepareStateName(PrepareStateError reason) -> ref<str> {
    switch (reason) {
    case PrepareStateError::NotInitialized: return "renderer not initialized"_str;
    case PrepareStateError::MissingSession: return "device session unavailable"_str;
    case PrepareStateError::FrameUpdateActive: return "frame update already active"_str;
    case PrepareStateError::NoSceneState: return "scene render state unavailable"_str;
    case PrepareStateError::MissingGraphics: return "prepared graphics unavailable"_str;
    case PrepareStateError::InvalidSections:
        return "incompatible resource preparation sections"_str;
    case PrepareStateError::NoTransaction: return "no active preparation transaction"_str;
    case PrepareStateError::NoTarget: return "render target unavailable"_str;
    }
    return "unknown preparation state"_str;
}
inline auto ResourcePrepareName(ResourcePrepareOperation operation) -> ref<str> {
    switch (operation) {
    case ResourcePrepareOperation::Begin: return "begin"_str;
    case ResourcePrepareOperation::Continue: return "continue"_str;
    case ResourcePrepareOperation::SharedBuffers: return "shared buffers"_str;
    case ResourcePrepareOperation::Graphics: return "graphics resources"_str;
    case ResourcePrepareOperation::Commit: return "commit"_str;
    case ResourcePrepareOperation::Video: return "video"_str;
    case ResourcePrepareOperation::Target: return "target"_str;
    }
    return "unknown resource operation"_str;
}
class DrawPrepareError {
    RSTD_ENUM(DrawPrepareError, (Missing, (DrawResourceKind kind; RenderItemId item;)),
              (Texture, (vrento::resource::TextureUseHandle use;)),
              (Buffer, (vrento::resource::BufferUseHandle use;)),
              (Shader, (vrento::resource::ShaderUseHandle use;)),
              (Layout, (vrento::resource::PipelineUseHandle use;)),
              (Buffers, (vrento::resource::ResourceError source;)),
              (Descriptor, (u32 set; vrento::resource::ResourceError source;)),
              (Uniform, (vrento::UniformBufferUpdateError source;)))
};
struct PrepareContext {
    u64                         resource_transaction {};
    vrento::vulkan::GraphSerial compilation {}, scope {};
};
class RenderPrepareError {
    RSTD_ENUM(RenderPrepareError, (State, (PrepareStateError reason;)),
              (Graph, (vrento::fg::Error source;)),
              (Resources,
               (ResourcePrepareOperation operation; vrento::resource::ResourceError source;)),
              (Draw, (vrento::vulkan::PassId pass; RenderItemId item; DrawPrepareError source;)),
              (OutputResources, (vrento::resource::ResourceError source;)),
              (OutputDescription, (vrento::vulkan::GraphExecutionError source;)),
              (Compile, (vrento::vulkan::GraphExecutionError source;)),
              (Upload, (vrento::texture::Error source;)))
public:
    PrepareContext context;
};
} // namespace owe::vulkan

export namespace rstd
{
template<>
struct Impl<fmt::Display, owe::vulkan::DrawPrepareError> : ImplBase<owe::vulkan::DrawPrepareError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        const auto& error = this->self();
        if (error.is_Missing())
            return formatter.write_str((rstd::format("draw resource missing kind={} item={}:{}",
                                                     int(error.as_Missing().kind),
                                                     error.as_Missing().item.index,
                                                     error.as_Missing().item.generation))
                                           .as_str());
        if (error.is_Texture())
            return formatter.write_str((rstd::format("draw texture use unavailable {}:{}",
                                                     error.as_Texture().use.index,
                                                     error.as_Texture().use.generation))
                                           .as_str());
        if (error.is_Buffer())
            return formatter.write_str((rstd::format("draw buffer use unavailable {}:{}",
                                                     error.as_Buffer().use.index,
                                                     error.as_Buffer().use.generation))
                                           .as_str());
        if (error.is_Shader())
            return formatter.write_str((rstd::format("draw shader use unavailable {}:{}",
                                                     error.as_Shader().use.index,
                                                     error.as_Shader().use.generation))
                                           .as_str());
        if (error.is_Layout())
            return formatter.write_str((rstd::format("draw layout use unavailable {}:{}",
                                                     error.as_Layout().use.index,
                                                     error.as_Layout().use.generation))
                                           .as_str());
        if (error.is_Buffers())
            return formatter.write_str((rstd::format("draw buffers preparation failed")).as_str());
        if (error.is_Descriptor())
            return formatter.write_str(
                (rstd::format("draw descriptor preparation failed")).as_str());
        if (error.is_Uniform())
            return formatter.write_str((rstd::format("draw uniform preparation failed")).as_str());
        return false;
    }
};
template<>
struct Impl<fmt::Debug, owe::vulkan::DrawPrepareError> : ImplBase<owe::vulkan::DrawPrepareError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return as<fmt::Display>(this->self()).fmt(formatter);
    }
};
template<>
struct Impl<error::Error, owe::vulkan::DrawPrepareError> : ImplBase<owe::vulkan::DrawPrepareError> {
    auto source() const noexcept -> Option<error::ErrorRef> {
        const auto& value = this->self();
        if (value.is_Buffers()) return Some(dyn<error::Error>::from_ref(value.as_Buffers().source));
        if (value.is_Descriptor())
            return Some(dyn<error::Error>::from_ref(value.as_Descriptor().source));
        if (value.is_Uniform()) return Some(dyn<error::Error>::from_ref(value.as_Uniform().source));
        return None();
    }
};
} // namespace rstd

export namespace rstd
{
template<>
struct Impl<fmt::Display, owe::vulkan::RenderPrepareError>
    : ImplBase<owe::vulkan::RenderPrepareError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        const auto& error = this->self();
        if (error.is_State())
            return formatter.write_str(
                (rstd::format("render preparation precondition failed reason={}",
                              owe::vulkan::PrepareStateName(error.as_State().reason)))
                    .as_str());
        if (error.is_Resources())
            return formatter.write_str(
                (rstd::format("resource preparation failed operation={} transaction={}",
                              owe::vulkan::ResourcePrepareName(error.as_Resources().operation),
                              error.context.resource_transaction))
                    .as_str());
        if (error.is_Draw())
            return formatter.write_str((rstd::format("draw preparation failed pass={} item={}:{}",
                                                     error.as_Draw().pass.value,
                                                     error.as_Draw().item.index,
                                                     error.as_Draw().item.generation))
                                           .as_str());
        if (error.is_Graph())
            return formatter.write_str(
                (rstd::format("Graph preparation failed transaction={} compilation={} scope={}",
                              error.context.resource_transaction,
                              error.context.compilation,
                              error.context.scope))
                    .as_str());
        if (error.is_OutputResources())
            return formatter.write_str(
                (rstd::format(
                     "OutputResources preparation failed transaction={} compilation={} scope={}",
                     error.context.resource_transaction,
                     error.context.compilation,
                     error.context.scope))
                    .as_str());
        if (error.is_OutputDescription())
            return formatter.write_str(
                (rstd::format(
                     "OutputDescription preparation failed transaction={} compilation={} scope={}",
                     error.context.resource_transaction,
                     error.context.compilation,
                     error.context.scope))
                    .as_str());
        if (error.is_Compile())
            return formatter.write_str(
                (rstd::format("Compile preparation failed transaction={} compilation={} scope={}",
                              error.context.resource_transaction,
                              error.context.compilation,
                              error.context.scope))
                    .as_str());
        if (error.is_Upload())
            return formatter.write_str(
                (rstd::format("Upload preparation failed transaction={} compilation={} scope={}",
                              error.context.resource_transaction,
                              error.context.compilation,
                              error.context.scope))
                    .as_str());
        return false;
    }
};
template<>
struct Impl<fmt::Debug, owe::vulkan::RenderPrepareError>
    : ImplBase<owe::vulkan::RenderPrepareError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return as<fmt::Display>(this->self()).fmt(formatter);
    }
};
template<>
struct Impl<error::Error, owe::vulkan::RenderPrepareError>
    : ImplBase<owe::vulkan::RenderPrepareError> {
    auto source() const noexcept -> Option<error::ErrorRef> {
        const auto& value = this->self();
        if (value.is_Graph()) return Some(dyn<error::Error>::from_ref(value.as_Graph().source));
        if (value.is_Resources())
            return Some(dyn<error::Error>::from_ref(value.as_Resources().source));
        if (value.is_Draw()) return Some(dyn<error::Error>::from_ref(value.as_Draw().source));
        if (value.is_OutputResources())
            return Some(dyn<error::Error>::from_ref(value.as_OutputResources().source));
        if (value.is_OutputDescription())
            return Some(dyn<error::Error>::from_ref(value.as_OutputDescription().source));
        if (value.is_Compile()) return Some(dyn<error::Error>::from_ref(value.as_Compile().source));
        if (value.is_Upload()) return Some(dyn<error::Error>::from_ref(value.as_Upload().source));
        return None();
    }
};
} // namespace rstd
