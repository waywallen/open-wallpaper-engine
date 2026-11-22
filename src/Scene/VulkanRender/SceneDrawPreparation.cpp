module wescene.vulkan_render;
import rstd;
import wescene.vulkan;
import wescene.scene;
import wescene.resource;
import :pipeline_layout;
import :scene_draw_preparation;

using namespace rstd::prelude;
using rstd::collections::HashMap;
using rstd::sync::Arc;

namespace owe::vulkan
{
namespace
{

struct DrawInterfaceKey {
    Vec<u64>    scalars;
    Vec<String> names;
};

// The projection excludes shader code, uniform members and graph-owned state.
template<typename Sink>
void VisitDrawInterface(const resource::ShaderArtifact& artifact, slice<String> sampler_members,
                        slice<DrawVertexLayout> vertex_layouts, Sink& sink) {
    const auto scalar = [&](auto value) {
        sink.scalar(u64(value.to_primitive()));
    };
    const auto binding = [&](const resource::ShaderArtifactDescriptorBinding& value) {
        sink.text(value.name.as_str());
        scalar(value.set);
        scalar(value.binding);
        scalar(value.descriptor_type);
        scalar(value.descriptor_count);
        scalar(value.stage_flags);
    };

    scalar(artifact.descriptor_bindings.len());
    for (const auto& value : artifact.descriptor_bindings) binding(value);
    scalar(artifact.descriptor_sets.len());
    for (const auto& set : artifact.descriptor_sets) {
        scalar(set.set);
        sink.scalar(u64(set.push_descriptor ? 1 : 0));
        scalar(set.bindings.len());
        for (const auto& value : set.bindings) binding(value);
    }
    scalar(artifact.vertex_inputs.len());
    for (const auto& input : artifact.vertex_inputs) {
        sink.text(input.name.as_str());
        scalar(input.location);
        scalar(input.format);
    }
    scalar(artifact.uniform_blocks.len());
    for (const auto& block : artifact.uniform_blocks) {
        sink.text(block.name.as_str());
        scalar(block.set);
        scalar(block.binding);
        sink.scalar(u64(static_cast<rstd::uint8_t>(block.scope)));
        scalar(block.identity);
    }
    scalar(sampler_members.len());
    for (const auto& member : sampler_members) sink.text(member.as_str());
    scalar(vertex_layouts.len());
    for (const auto& layout : vertex_layouts) {
        scalar(layout.stride);
        scalar(layout.attributes.len());
        for (const auto& attribute : layout.attributes) {
            sink.text(attribute.name.as_str());
            scalar(attribute.offset);
        }
    }
}

struct InterfaceHasher {
    rstd::hash::DefaultHasher state;

    void scalar(u64 value) { rstd::hash::hash_into(value, state); }
    void text(ref<str> value) { rstd::hash::hash_into(value, state); }
};

struct InterfaceCapture {
    DrawInterfaceKey key;

    void scalar(u64 value) { key.scalars.push(rstd::move(value)); }
    void text(ref<str> value) { key.names.push(String::make(value)); }
};

struct InterfaceComparison {
    const DrawInterfaceKey& key;
    usize                   scalar_index {};
    usize                   name_index {};
    bool                    equal { true };

    void scalar(u64 value) {
        if (scalar_index >= key.scalars.len() || key.scalars[scalar_index] != value) equal = false;
        ++scalar_index;
    }
    void text(ref<str> value) {
        if (name_index >= key.names.len() || key.names[name_index] != value) equal = false;
        ++name_index;
    }
    auto matches() const -> bool {
        return equal && scalar_index == key.scalars.len() && name_index == key.names.len();
    }
};

struct DrawPreparationEntry {
    DrawInterfaceKey          key;
    Arc<SceneDrawPreparation> prepared;
};

auto PreparePipelineLayout(const resource::ShaderArtifact& artifact)
    -> Result<PipelineLayoutRequirement, resource::ResourceError> {
    for (const auto& block : artifact.uniform_blocks) {
        const bool shared = block.scope == resource::ShaderArtifactUniformBlock::Scope::Shared;
        if ((shared && block.set != u32()) || (! shared && block.set == u32())) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingDefinition,
                .message = rstd::format("uniform block {} has invalid descriptor scope",
                                        block.name.as_str()),
            });
        }
        bool described = false;
        for (const auto& set : artifact.descriptor_sets) {
            if (set.set != block.set) continue;
            for (const auto& binding : set.bindings) {
                if (binding.binding == block.binding &&
                    binding.descriptor_type == u32(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)) {
                    described = true;
                }
            }
        }
        if (! described) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingDefinition,
                .message = rstd::format("uniform block {} is outside the descriptor requirement",
                                        block.name.as_str()),
            });
        }
    }

    PipelineLayoutRequirement requirement {
        .pipeline = { .index = u64(), .generation = u64() },
    };
    requirement.descriptor_sets.reserve(artifact.descriptor_sets.len());
    for (const auto& set : artifact.descriptor_sets) {
        PipelineLayoutSetRequirement set_requirement {
            .set             = set.set,
            .push_descriptor = set.push_descriptor,
        };
        set_requirement.bindings.reserve(set.bindings.len());
        for (const auto& binding : set.bindings) {
            if (binding.set != set.set) {
                return Err(resource::ResourceError {
                    .kind    = resource::ResourceErrorKind::MissingDefinition,
                    .message = rstd::format("descriptor binding set does not match its set"),
                });
            }
            Option<u64> shared_identity;
            for (const auto& block : artifact.uniform_blocks) {
                if (block.set != set.set || block.binding != binding.binding) continue;
                if (block.scope == resource::ShaderArtifactUniformBlock::Scope::Shared) {
                    shared_identity = Some(u64(block.identity.to_primitive()));
                }
                break;
            }
            if (set.set == u32() && shared_identity.is_none()) {
                return Err(resource::ResourceError {
                    .kind    = resource::ResourceErrorKind::MissingDefinition,
                    .message = rstd::format("descriptor set 0 binding {} is not a shared resource",
                                            binding.binding),
                });
            }
            if (set.set != u32() && shared_identity.is_some()) {
                return Err(resource::ResourceError {
                    .kind    = resource::ResourceErrorKind::MissingDefinition,
                    .message = rstd::format("shared resources must use descriptor set 0"),
                });
            }
            set_requirement.bindings.push(PipelineLayoutBindingRequirement {
                .binding          = binding.binding,
                .descriptor_type  = binding.descriptor_type,
                .descriptor_count = binding.descriptor_count,
                .stage_flags      = binding.stage_flags,
                .shared_identity  = shared_identity,
            });
        }
        requirement.descriptor_sets.push(rstd::move(set_requirement));
    }
    return Ok(rstd::move(requirement));
}

auto WithoutPipeline(PipelineLayoutRequirement requirement) -> PipelineLayoutRequirement {
    requirement.pipeline = { .index = u64(), .generation = u64() };
    return requirement;
}

} // namespace

SceneDrawPreparation::SceneDrawPreparation(Vec<DrawSamplerLocation>               sampler_locations,
                                           Vec<VkVertexInputBindingDescription>   bindings,
                                           Vec<VkVertexInputAttributeDescription> attributes,
                                           PipelineLayoutRequirement              layout)
    : samplers(rstd::move(sampler_locations)),
      vertex_bindings(rstd::move(bindings)),
      vertex_attributes(rstd::move(attributes)),
      layout_template_(WithoutPipeline(rstd::move(layout))) {}

auto SceneDrawPreparation::pipelineLayout(resource::PipelineUseHandle pipeline) const
    -> PipelineLayoutRequirement {
    auto requirement     = layout_template_.clone();
    requirement.pipeline = pipeline;
    return requirement;
}

auto PrepareSceneDraw(const resource::ShaderArtifact& artifact, slice<String> sampler_members,
                      slice<DrawVertexLayout> vertex_layouts)
    -> Result<Arc<SceneDrawPreparation>, resource::ResourceError> {
    auto layout = PreparePipelineLayout(artifact);
    if (layout.is_err()) return Err(rstd::move(layout).unwrap_err_unchecked());

    auto samplers = Vec<DrawSamplerLocation>::with_capacity(sampler_members.len());
    for (const auto& member : sampler_members) {
        DrawSamplerLocation location;
        if (! member.is_empty()) {
            // Reflection maps keep the last binding for a repeated name.
            for (const auto& binding : artifact.descriptor_bindings) {
                if (binding.name != member) continue;
                location.binding = i32(static_cast<rstd::int32_t>(binding.binding.to_primitive()));
                location.set     = binding.set;
                location.stages  = vrento::texture::Stages::None;
                const auto flags = binding.stage_flags.to_primitive();
                if (flags & VK_SHADER_STAGE_VERTEX_BIT)
                    location.stages = location.stages | vrento::texture::Stages::Vertex;
                if (flags & VK_SHADER_STAGE_FRAGMENT_BIT)
                    location.stages = location.stages | vrento::texture::Stages::Fragment;
                if (flags & VK_SHADER_STAGE_GEOMETRY_BIT)
                    location.stages = location.stages | vrento::texture::Stages::Geometry;
            }
        }
        samplers.push(rstd::move(location));
    }

    auto bindings = Vec<VkVertexInputBindingDescription>::with_capacity(vertex_layouts.len());
    Vec<VkVertexInputAttributeDescription> attributes;
    for (usize index {}; index < vertex_layouts.len(); ++index) {
        const auto& vertex  = vertex_layouts[index];
        const auto  binding = static_cast<rstd::uint32_t>(index.to_primitive());
        bindings.push({
            .binding   = binding,
            .stride    = static_cast<rstd::uint32_t>(vertex.stride.to_primitive()),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        });
        for (usize input_index {}; input_index < artifact.vertex_inputs.len(); ++input_index) {
            const auto& input    = artifact.vertex_inputs[input_index];
            bool        replaced = false;
            for (auto later = input_index + usize(1); later < artifact.vertex_inputs.len();
                 ++later) {
                if (artifact.vertex_inputs[later].name == input.name) {
                    replaced = true;
                    break;
                }
            }
            if (replaced) continue;
            usize offset {};
            for (const auto& attribute : vertex.attributes) {
                if (attribute.name == input.name) offset = attribute.offset;
            }
            attributes.push({
                .location = input.location.to_primitive(),
                .binding  = binding,
                .format   = static_cast<VkFormat>(input.format.to_primitive()),
                .offset   = static_cast<rstd::uint32_t>(offset.to_primitive()),
            });
        }
    }
    return Ok(Arc<SceneDrawPreparation>::make(rstd::move(samplers),
                                              rstd::move(bindings),
                                              rstd::move(attributes),
                                              rstd::move(layout).unwrap_unchecked()));
}

struct SceneDrawPreparationCache::State {
    HashMap<u64, Vec<Arc<DrawPreparationEntry>>> buckets;
    Option<Arc<DrawPreparationEntry>>            last;
    SceneDrawPreparationStats                    statistics;
};

SceneDrawPreparationCache::SceneDrawPreparationCache(): state_(Box<State>::make()) {}
SceneDrawPreparationCache::~SceneDrawPreparationCache() = default;

auto SceneDrawPreparationCache::prepare(const resource::ShaderArtifact& artifact,
                                        slice<String>                   sampler_members,
                                        slice<DrawVertexLayout>         vertex_layouts)
    -> Result<Arc<SceneDrawPreparation>, resource::ResourceError> {
    // Consecutive draws often share an interface; compare it before hashing again.
    if (state_->last) {
        const auto&         entry = **state_->last;
        InterfaceComparison comparison { entry.key };
        VisitDrawInterface(artifact, sampler_members, vertex_layouts, comparison);
        if (comparison.matches()) {
            ++state_->statistics.hits;
            return Ok(entry.prepared.clone());
        }
    }
    InterfaceHasher hasher;
    VisitDrawInterface(artifact, sampler_members, vertex_layouts, hasher);
    const auto hash   = hasher.state.finish();
    auto       bucket = state_->buckets.get(hash);
    if (bucket.is_some()) {
        for (const auto& entry : **bucket) {
            InterfaceComparison comparison { entry->key };
            VisitDrawInterface(artifact, sampler_members, vertex_layouts, comparison);
            if (! comparison.matches()) continue;
            ++state_->statistics.hits;
            state_->last = Some(entry.clone());
            return Ok(entry->prepared.clone());
        }
    }

    ++state_->statistics.misses;
    auto result = PrepareSceneDraw(artifact, sampler_members, vertex_layouts);
    if (result.is_err()) return result;
    auto             prepared = rstd::move(result).unwrap_unchecked();
    InterfaceCapture capture;
    VisitDrawInterface(artifact, sampler_members, vertex_layouts, capture);
    auto entry   = Arc<DrawPreparationEntry>::make(rstd::move(capture.key), prepared.clone());
    state_->last = Some(entry.clone());
    auto target  = state_->buckets.get_mut(hash);
    if (target.is_some()) {
        (**target).push(rstd::move(entry));
    } else {
        Vec<Arc<DrawPreparationEntry>> entries;
        entries.push(rstd::move(entry));
        (void)state_->buckets.insert(hash, rstd::move(entries));
    }
    ++state_->statistics.entries;
    return Ok(rstd::move(prepared));
}

void SceneDrawPreparationCache::prune() {
    state_->last = None();
    state_->buckets.retain([&](const u64&, Vec<Arc<DrawPreparationEntry>>& entries) {
        entries.retain([&](const Arc<DrawPreparationEntry>& entry) {
            if (entry->prepared.strong_count() != usize(1)) return true;
            --state_->statistics.entries;
            return false;
        });
        return ! entries.is_empty();
    });
}

void SceneDrawPreparationCache::clear() {
    state_->last = None();
    state_->buckets.clear();
    state_->statistics = {};
}

auto SceneDrawPreparationCache::stats() const -> SceneDrawPreparationStats {
    return state_->statistics;
}

} // namespace owe::vulkan
