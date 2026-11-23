module;

module wescene.vulkan_render;
import wescene.core;
import wescene.types;
import rstd;
import rstd.cppstd;
import wescene.vulkan;
import wescene.scene;

using namespace rstd::prelude;
using rstd::cppstd::as_str;
using rstd::slice_::sort_unstable_by;

namespace owe::vulkan
{

namespace
{

ShaderReflectionKey MakeShaderReflectionKey(const SceneShader& shader) {
    return ShaderReflectionKey {
        .shader_id = shader.id,
        .code_hash = SceneShaderCodeHash(shader.codes.as_slice()),
    };
}

auto MakeCachedReflection(Vec<Uni_ShaderSpv> spvs, ShaderReflected reflected)
    -> rstd::sync::Arc<CachedShaderReflection> {
    auto stages = Vec<CachedShaderStage>::with_capacity(spvs.len());
    for (auto& spv : spvs) {
        stages.push(CachedShaderStage {
            .entry_point = rstd::move(spv->entry_point),
            .stage       = spv->stage,
            .spirv       = rstd::move(spv->spirv),
        });
    }
    return rstd::sync::Arc<CachedShaderReflection>::make(rstd::move(stages), rstd::move(reflected));
}

} // namespace

auto ShaderReflectionCache::Query(const SceneShader& shader)
    -> Option<ref<CachedShaderReflection>> {
    auto key    = MakeShaderReflectionKey(shader);
    auto cached = m_entries.get(key);
    if (cached.is_some() && (**cached)->Matches(shader.codes.as_slice()))
        return Some((**cached).deref());

    if (shader.reflection.is_some() && (*shader.reflection)->Matches(shader.codes.as_slice())) {
        (void)m_entries.insert(key, shader.reflection->clone());
        return Some((**m_entries.get(key)).deref());
    }

    Vec<Uni_ShaderSpv> spvs;
    ShaderReflected    reflected;
    if (! m_backend->GenReflect(shader.codes.as_slice(), spvs, reflected)) return rstd::None();

    (void)m_entries.insert(key, MakeCachedReflection(rstd::move(spvs), rstd::move(reflected)));
    return Some((**m_entries.get(key)).deref());
}

void ShaderReflectionCache::Clear() { m_entries.clear(); }

SceneShaderArtifactProvider::SceneShaderArtifactProvider(ShaderReflectionCache& cache,
                                                         const SceneShader&     shader)
    : m_cache(mut_ref<ShaderReflectionCache>::from_raw_parts(rstd::addressof(cache))),
      m_shader(ref<SceneShader>::from_raw_parts(rstd::addressof(shader))) {}

auto MakeSceneShaderRequest(const SceneShader& shader) -> resource::ShaderRequest {
    return resource::ShaderRequest {
        .name = shader.name.clone(),
        .source =
            resource::ShaderDefinitionId {
                .index      = shader.id,
                .generation = u64(1),
            },
        .content_version = rstd::as_cast<u64>(SceneShaderCodeHash(shader)),
    };
}

auto SceneShaderArtifactProvider::Request() const -> resource::ShaderRequest {
    return MakeSceneShaderRequest(*m_shader);
}

namespace
{
auto BuildSceneShaderArtifact(ShaderReflectionCache& cache, const SceneShader& shader,
                              const resource::ShaderRequest& request)
    -> Result<resource::ShaderArtifact, resource::ResourceError> {
    auto reflection = cache.Query(shader);
    if (reflection.is_none()) {
        return rstd::Err(resource::ResourceError {
            .kind    = resource::ResourceErrorKind::BackendFailure,
            .message = rstd::format("compile shader {} failed", shader.name),
        });
    }

    resource::ShaderArtifact artifact {
        .source            = request.source,
        .content_version   = request.content_version,
        .matrix_convention = shader.matrix_convention,
        .matrix_abi        = shader.matrix_abi,
    };
    artifact.stages.reserve((**reflection).Stages().len());
    for (const auto& stage : (**reflection).Stages()) {
        artifact.stages.push(resource::ShaderArtifactStage {
            .stage       = stage.stage,
            .entry_point = stage.entry_point.clone(),
            .code        = stage.spirv.clone(),
        });
    }
    artifact.uniform_blocks.reserve((**reflection).Reflected().blocks.len());
    for (const auto& block : (**reflection).Reflected().blocks) {
        auto scope = resource::ShaderArtifactUniformBlock::Scope::Local;
        u64  identity {};
        for (const auto& declared : shader.uniform_blocks) {
            if (declared.name != block.name || declared.set != u32(block.set) ||
                declared.binding != u32(block.binding)) {
                continue;
            }
            scope    = declared.scope == SceneShaderUniformBlockScope::Shared
                           ? resource::ShaderArtifactUniformBlock::Scope::Shared
                           : resource::ShaderArtifactUniformBlock::Scope::Local;
            identity = declared.identity;
            break;
        }
        auto members =
            Vec<resource::ShaderArtifactUniformMember>::with_capacity(block.member_map.len());
        for (const auto& [name, member_ref] : block.member_map.iter()) {
            const auto& member = *member_ref;
            members.push(resource::ShaderArtifactUniformMember {
                .name              = name->clone(),
                .offset            = u32(member.offset),
                .size              = member.size,
                .count             = member.num,
                .scalar_kind       = member.scalar_kind,
                .scalar_width      = u32(member.scalar_width),
                .vector_components = u32(member.vector_components),
                .matrix_rows       = u32(member.matrix_rows),
                .matrix_columns    = u32(member.matrix_columns),
                .matrix_stride     = u32(member.matrix_stride),
                .matrix_major      = member.matrix_major,
                .array_stride      = u32(member.array_stride),
                .array_dimensions  = member.array_dimensions.clone(),
            });
        }
        artifact.uniform_blocks.push(resource::ShaderArtifactUniformBlock {
            .name     = block.name.clone(),
            .size     = usize(block.size),
            .set      = u32(block.set),
            .binding  = u32(block.binding),
            .scope    = scope,
            .identity = identity,
            .members  = rstd::move(members),
        });
    }
    artifact.descriptor_bindings.reserve((**reflection).Reflected().binding_map.len());
    for (const auto& [name, binding_ref] : (**reflection).Reflected().binding_map.iter()) {
        const auto& binding = *binding_ref;
        artifact.descriptor_bindings.push(resource::ShaderArtifactDescriptorBinding {
            .name             = name->clone(),
            .set              = u32(binding.set),
            .binding          = u32(binding.layout.binding),
            .descriptor_type  = u32(static_cast<uint32_t>(binding.layout.descriptorType)),
            .descriptor_count = u32(binding.layout.descriptorCount),
            .stage_flags      = u32(binding.layout.stageFlags),
        });
    }
    if (! shader.descriptor_sets.is_empty()) {
        artifact.descriptor_sets.reserve(shader.descriptor_sets.len());
        for (const auto& declared : shader.descriptor_sets) {
            auto bindings = Vec<resource::ShaderArtifactDescriptorBinding>::with_capacity(
                declared.bindings.len());
            for (const auto& binding : declared.bindings) {
                bindings.push(resource::ShaderArtifactDescriptorBinding {
                    .name             = binding.name.clone(),
                    .set              = declared.set,
                    .binding          = binding.binding,
                    .descriptor_type  = binding.descriptor_type,
                    .descriptor_count = binding.descriptor_count,
                    .stage_flags      = binding.stage_flags,
                });
            }
            artifact.descriptor_sets.push(resource::ShaderArtifactDescriptorSet {
                .set             = declared.set,
                .push_descriptor = declared.push_descriptor,
                .identity        = declared.identity,
                .bindings        = rstd::move(bindings),
            });
        }
    } else {
        for (const auto& binding : artifact.descriptor_bindings) {
            resource::ShaderArtifactDescriptorSet* target = nullptr;
            for (auto& set : artifact.descriptor_sets) {
                if (set.set == binding.set) target = rstd::addressof(set);
            }
            if (target == nullptr) {
                artifact.descriptor_sets.push(resource::ShaderArtifactDescriptorSet {
                    .set = binding.set,
                });
                target = rstd::addressof(artifact.descriptor_sets.last_mut().unwrap().get_mut());
            }
            target->bindings.push(binding.clone());
        }
        if (! artifact.descriptor_sets.is_empty()) {
            auto push_set = artifact.descriptor_sets.first().unwrap()->set;
            for (const auto& set : artifact.descriptor_sets) {
                if (set.set > push_set) push_set = set.set;
            }
            for (auto& set : artifact.descriptor_sets) {
                set.push_descriptor = set.set == push_set;
            }
        }
    }
    sort_unstable_by(artifact.descriptor_sets.as_mut_slice().as_mut_ref(),
                     [](const auto& lhs, const auto& rhs) {
                         return lhs.set < rhs.set;
                     });
    for (const auto& active : artifact.descriptor_bindings) {
        bool compatible = false;
        for (const auto& set : artifact.descriptor_sets) {
            if (set.set != active.set) continue;
            for (const auto& declared : set.bindings) {
                if (declared.binding != active.binding) continue;
                compatible = declared.descriptor_type == active.descriptor_type &&
                             declared.descriptor_count == active.descriptor_count &&
                             (declared.stage_flags & active.stage_flags) == active.stage_flags;
            }
        }
        if (! compatible) {
            return rstd::Err(resource::ResourceError {
                .kind = resource::ResourceErrorKind::MissingDefinition,
                .message =
                    rstd::format("shader descriptor requirement does not cover binding {}:{}",
                                 active.set,
                                 active.binding),
            });
        }
    }
    artifact.vertex_inputs.reserve((**reflection).Reflected().input_location_map.len());
    for (const auto& [name, input_ref] : (**reflection).Reflected().input_location_map.iter()) {
        const auto& input = *input_ref;
        artifact.vertex_inputs.push(resource::ShaderArtifactVertexInput {
            .name     = name->clone(),
            .location = u32(input.location),
            .format   = u32(static_cast<uint32_t>(input.format)),
        });
    }
    return rstd::Ok(rstd::move(artifact));
}
} // namespace

auto SceneShaderArtifactProvider::LoadShader(const resource::ShaderRequest& request)
    -> Result<resource::ShaderArtifact, resource::ResourceError> {
    if (request != Request())
        return Err(resource::ResourceError {
            .kind    = resource::ResourceErrorKind::MissingDefinition,
            .message = rstd::format("shader request does not match {}", m_shader->name),
        });
    return BuildSceneShaderArtifact(*m_cache, *m_shader, request);
}

bool ShaderArtifactCache::Entry::Matches(const SceneShader& shader) const {
    if (shader.matrix_convention != artifact->matrix_convention ||
        shader.matrix_abi != artifact->matrix_abi || shader.codes.len() != codes.len() ||
        shader.uniform_blocks.len() != blocks.len() || shader.descriptor_sets.len() != sets.len())
        return false;
    for (usize i {}; i < codes.len(); ++i)
        if (codes[i] != shader.codes[i]) return false;
    for (usize i {}; i < blocks.len(); ++i)
        if (blocks[i] != shader.uniform_blocks[i]) return false;
    for (usize i {}; i < sets.len(); ++i) {
        const auto& old  = sets[i];
        const auto& next = shader.descriptor_sets[i];
        if (old.set != next.set || old.identity != next.identity ||
            old.push_descriptor != next.push_descriptor ||
            old.bindings.len() != next.bindings.len())
            return false;
        for (usize j {}; j < old.bindings.len(); ++j)
            if (old.bindings[j] != next.bindings[j]) return false;
    }
    return true;
}

auto ShaderArtifactCache::Prepare(const SceneShader& shader)
    -> Result<PublishedShaderArtifact, resource::ResourceError> {
    auto request = MakeSceneShaderRequest(shader);
    if (auto cached = m_artifacts.get(request)) {
        if (! (*cached)->Matches(shader))
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::RevisionMismatch,
                .message = rstd::format("conflicting shader artifact {}", request.name),
            });
        ++m_stats.hits;
        return Ok(PublishedShaderArtifact { rstd::move(request), (*cached)->artifact.clone() });
    }
    auto prepared = BuildSceneShaderArtifact(m_reflection, shader, request);
    if (prepared.is_err()) return Err(prepared.unwrap_err());
    auto artifact = Arc<resource::ShaderArtifact>::make(prepared.unwrap());
    ++m_stats.builds;
    (void)m_artifacts.insert(request.clone(),
                             Entry { shader.codes.clone(),
                                     shader.uniform_blocks.clone(),
                                     shader.descriptor_sets.clone(),
                                     artifact.clone() });
    return Ok(PublishedShaderArtifact { rstd::move(request), rstd::move(artifact) });
}

Vec<Uni_ShaderSpv> ShaderSpvsFromArtifact(const resource::ShaderArtifact& artifact) {
    Vec<Uni_ShaderSpv> out;
    out.reserve(artifact.stages.len());
    for (const auto& stage : artifact.stages) {
        auto spv         = Box<ShaderSpv>::make();
        spv->entry_point = stage.entry_point.clone();
        spv->stage       = stage.stage;
        spv->spirv       = stage.code.clone();
        out.push(rstd::move(spv));
    }
    return out;
}

auto ShaderReflectionFromArtifact(const resource::ShaderArtifact& artifact) -> ShaderReflected {
    ShaderReflected reflected;
    reflected.blocks.reserve(artifact.uniform_blocks.len());
    for (usize index {}; index < artifact.uniform_blocks.len(); ++index) {
        const auto&            block = artifact.uniform_blocks[index];
        ShaderReflected::Block prepared {
            .index   = static_cast<int>(index.to_primitive()),
            .size    = static_cast<unsigned>(block.size.to_primitive()),
            .name    = block.name.clone(),
            .set     = block.set.to_primitive(),
            .binding = block.binding.to_primitive(),
        };
        for (const auto& member : block.members) {
            ShaderReflected::BlockedUniform reflected_member {
                .block_index       = static_cast<int>(index.to_primitive()),
                .offset            = member.offset.to_primitive(),
                .size              = member.size,
                .num               = member.count,
                .scalar_kind       = member.scalar_kind,
                .scalar_width      = member.scalar_width.to_primitive(),
                .vector_components = member.vector_components.to_primitive(),
                .matrix_rows       = member.matrix_rows.to_primitive(),
                .matrix_columns    = member.matrix_columns.to_primitive(),
                .matrix_stride     = member.matrix_stride.to_primitive(),
                .matrix_major      = member.matrix_major,
                .array_stride      = member.array_stride.to_primitive(),
                .array_dimensions  = member.array_dimensions.clone(),
            };
            (void)prepared.member_map.insert(member.name.clone(), rstd::move(reflected_member));
        }
        reflected.blocks.push(rstd::move(prepared));
    }
    for (const auto& binding : artifact.descriptor_bindings) {
        (void)reflected.binding_map.insert(
            binding.name.clone(),
            ShaderReflected::Binding {
                .set = binding.set.to_primitive(),
                .layout =
                    VkDescriptorSetLayoutBinding {
                        .binding = binding.binding.to_primitive(),
                        .descriptorType =
                            static_cast<VkDescriptorType>(binding.descriptor_type.to_primitive()),
                        .descriptorCount    = binding.descriptor_count.to_primitive(),
                        .stageFlags         = binding.stage_flags.to_primitive(),
                        .pImmutableSamplers = nullptr,
                    },
            });
    }
    for (const auto& input : artifact.vertex_inputs) {
        (void)reflected.input_location_map.insert(
            input.name.clone(),
            ShaderReflected::Input {
                .location = input.location.to_primitive(),
                .format   = static_cast<VkFormat>(input.format.to_primitive()),
            });
    }
    return reflected;
}

} // namespace owe::vulkan
