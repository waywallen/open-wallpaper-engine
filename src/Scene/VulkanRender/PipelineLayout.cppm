export module wescene.vulkan_render:pipeline_layout;
export import vrento.pipeline_layout;
import rstd;
import wescene.resource;

using namespace rstd::prelude;
export namespace owe::vulkan
{
using vrento::vulkan::GlobalBindingIdentity;
using vrento::vulkan::PipelineLayoutAssignment;
using vrento::vulkan::PipelineLayoutAssignments;
using vrento::vulkan::PipelineLayoutBindingRequirement;
using vrento::vulkan::PipelineLayoutConflict;
using vrento::vulkan::PipelineLayoutPlan;
using vrento::vulkan::PipelineLayoutRequirement;
using vrento::vulkan::PipelineLayoutSetRequirement;
using vrento::vulkan::PlannedPipelineLayoutFamily;

inline auto PlanPipelineLayouts(slice<PipelineLayoutRequirement> requirements,
                                bool push_descriptor_supported, u32 max_push_descriptors = u32::MAX,
                                u32                           max_push_constant_size = u32::MAX,
                                const VkPhysicalDeviceLimits* descriptor_limits      = nullptr)
    -> Result<PipelineLayoutPlan, resource::ResourceError> {
    return vrento::vulkan::PlanPipelineLayouts(
        requirements,
        vrento::vulkan::PipelineLayoutPolicy { .shared_set = Some(u32()) },
        push_descriptor_supported,
        max_push_descriptors,
        max_push_constant_size,
        descriptor_limits);
}
} // namespace owe::vulkan
