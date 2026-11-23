module;
#include <rstd/macro.hpp>
module wescene.vulkan_render;
import rstd.log;
using namespace rstd::prelude;
using namespace owe::vulkan;
namespace vk = vrento::vulkan;
namespace fg = vrento::fg;

auto SceneRenderState::captureFrame(
    RenderingResources& rr, const vk::CompiledGraph& compiled, const vk::TextureOutput& output,
    vk::SurfaceTarget target, Option<resource::DescriptorBindingHandle> global_descriptor_binding)
    -> Result<vk::GraphFrameRequest, vk::GraphExecutionError> {
    auto failure = [] {
        return Err(vk::GraphExecutionError { vk::GraphExecutionStage::Prepare });
    };
    if (! input_ || compiled.description().scope() != input_->graph().scope()) return failure();
    auto& resources = rr.resources;
    auto& prepared  = resources.Prepared();
    auto  captured  = output.capture(compiled,
                                     surface.resource,
                                     output_pass,
                                     release_pass,
                                     rstd::move(target),
                                     rr.scopes.clone());
    if (captured.is_err()) return Err(captured.unwrap_err());
    auto               request = captured.unwrap();
    auto&              frames  = request.parameters;
    DrawBindingContext context {
        .resources = ref<resource_registry::PreparedResourceTable>::from_raw_parts(&prepared),
        .global_descriptor =
            global_descriptor_binding ? prepared.Resolve(*global_descriptor_binding) : None(),
    };
    Vec<vk::TextureSelection> selections;
    for (const auto& draw : draws) {
        auto desc = compiled.description(draw.id);
        if (! desc || ! (*desc)->get<vk::GraphicsDrawDesc>()) return failure();
        auto value = draw.source->frameData(context, selections);
        if (! value) return failure();
        auto frame =
            compiled.frame<vk::GraphicsDrawPass>(draw.id, (*desc)->revision(), value.unwrap());
        if (frame.is_err()) return Err(frame.unwrap_err());
        frames.push(frame.unwrap());
    }
    auto bound = vk::capture_resources(
        compiled.input().resources(), prepared, selections.as_slice(), request);
    if (bound.is_err()) return Err(bound.unwrap_err());
    return Ok(rstd::move(request));
}
