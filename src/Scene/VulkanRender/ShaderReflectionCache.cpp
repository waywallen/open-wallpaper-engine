module;

module wescene.vulkan_render;
import wescene.core;
import wescene.types;
import rstd;
import rstd.cppstd;
import wescene.vulkan;
import wescene.scene;

using namespace rstd::prelude;

namespace owe::vulkan
{

namespace
{

ShaderReflectionKey MakeShaderReflectionKey(const SceneShader& shader) {
    return ShaderReflectionKey {
        .shader_id = shader.id,
        .code_hash = SceneShaderCodeHash(shader),
    };
}

CachedShaderReflection MakeCachedReflection(std::vector<Uni_ShaderSpv> spvs,
                                            ShaderReflected            reflected) {
    CachedShaderReflection out;
    out.stages.reserve(spvs.size());
    for (auto& spv : spvs) {
        out.stages.push_back(CachedShaderStage {
            .entry_point = spv->entry_point,
            .stage       = spv->stage,
            .spirv       = std::move(spv->spirv),
        });
    }
    out.reflected = std::move(reflected);
    return out;
}

} // namespace

auto ShaderReflectionCache::Query(const SceneShader& shader)
    -> rstd::Option<rstd::ref<CachedShaderReflection>> {
    auto key    = MakeShaderReflectionKey(shader);
    auto cached = m_entries.get(key);
    if (cached.is_some()) return cached;

    std::vector<Uni_ShaderSpv> spvs;
    ShaderReflected            reflected;
    if (! m_backend->GenReflect(shader.codes, spvs, reflected)) return rstd::None();

    (void)m_entries.insert(key, MakeCachedReflection(std::move(spvs), std::move(reflected)));
    return m_entries.get(key);
}

void ShaderReflectionCache::Clear() { m_entries.clear(); }

SceneShaderArtifactProvider::SceneShaderArtifactProvider(ShaderReflectionCache& cache,
                                                         const SceneShader&     shader)
    : m_cache(rstd::mut_ref<ShaderReflectionCache>::from_raw_parts(rstd::addressof(cache))),
      m_shader(rstd::ref<SceneShader>::from_raw_parts(rstd::addressof(shader))) {}

auto MakeSceneShaderRequest(const SceneShader& shader) -> resource::ShaderRequest {
    return resource::ShaderRequest {
        .name = rstd::string::String::make(rstd::cppstd::as_str(shader.name).unwrap()),
        .source =
            resource::ShaderDefinitionId {
                .index      = shader.id,
                .generation = u64(1),
            },
        .content_version = rstd::as_cast<rstd::u64>(SceneShaderCodeHash(shader)),
    };
}

auto SceneShaderArtifactProvider::Request() const -> resource::ShaderRequest {
    return MakeSceneShaderRequest(*m_shader);
}

auto SceneShaderArtifactProvider::LoadShader(const resource::ShaderRequest& request)
    -> rstd::Result<resource::ShaderArtifact, resource::ResourceError> {
    if (request.source.index != m_shader->id ||
        request.content_version != rstd::as_cast<u64>(SceneShaderCodeHash(*m_shader))) {
        return rstd::Err(resource::ResourceError {
            .kind    = resource::ResourceErrorKind::MissingDefinition,
            .message = rstd::format("shader request does not match {}", m_shader->name),
        });
    }
    auto reflection = m_cache->Query(*m_shader);
    if (reflection.is_none()) {
        return rstd::Err(resource::ResourceError {
            .kind    = resource::ResourceErrorKind::BackendFailure,
            .message = rstd::format("compile shader {} failed", m_shader->name),
        });
    }

    resource::ShaderArtifact artifact {
        .source            = request.source,
        .content_version   = request.content_version,
        .matrix_convention = m_shader->matrix_convention,
        .matrix_abi        = m_shader->matrix_abi,
    };
    artifact.stages.reserve(usize((**reflection).stages.size()));
    for (const auto& stage : (**reflection).stages) {
        auto code = rstd::vec::Vec<rstd::u32>::with_capacity(usize(stage.spirv.size()));
        for (auto word : stage.spirv) code.push(rstd::u32(word));
        artifact.stages.push(resource::ShaderArtifactStage {
            .stage = stage.stage,
            .entry_point =
                rstd::string::String::make(rstd::cppstd::as_str(stage.entry_point).unwrap()),
            .code = rstd::move(code),
        });
    }
    artifact.uniform_blocks.reserve(usize((**reflection).reflected.blocks.size()));
    for (const auto& block : (**reflection).reflected.blocks) {
        auto scope = resource::ShaderArtifactUniformBlock::Scope::Local;
        u64  identity {};
        for (const auto& declared : m_shader->uniform_blocks) {
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
        auto members = rstd::vec::Vec<resource::ShaderArtifactUniformMember>::with_capacity(
            usize(block.member_map.size()));
        for (const auto& [name, member] : block.member_map) {
            auto dimensions =
                rstd::vec::Vec<u32>::with_capacity(usize(member.array_dimensions.size()));
            for (auto dimension : member.array_dimensions) dimensions.push(u32(dimension));
            members.push(resource::ShaderArtifactUniformMember {
                .name         = rstd::string::String::make(rstd::cppstd::as_str(name).unwrap()),
                .offset       = u32(member.offset),
                .size         = member.size,
                .count        = member.num,
                .scalar_kind  = member.scalar_kind,
                .scalar_width = u32(member.scalar_width),
                .vector_components = u32(member.vector_components),
                .matrix_rows       = u32(member.matrix_rows),
                .matrix_columns    = u32(member.matrix_columns),
                .matrix_stride     = u32(member.matrix_stride),
                .matrix_major      = member.matrix_major,
                .array_stride      = u32(member.array_stride),
                .array_dimensions  = rstd::move(dimensions),
            });
        }
        artifact.uniform_blocks.push(resource::ShaderArtifactUniformBlock {
            .name     = rstd::string::String::make(rstd::cppstd::as_str(block.name).unwrap()),
            .size     = usize(block.size),
            .set      = u32(block.set),
            .binding  = u32(block.binding),
            .scope    = scope,
            .identity = identity,
            .members  = rstd::move(members),
        });
    }
    artifact.descriptor_bindings.reserve(usize((**reflection).reflected.binding_map.size()));
    for (const auto& [name, binding] : (**reflection).reflected.binding_map) {
        artifact.descriptor_bindings.push(resource::ShaderArtifactDescriptorBinding {
            .name             = rstd::string::String::make(rstd::cppstd::as_str(name).unwrap()),
            .set              = u32(binding.set),
            .binding          = u32(binding.layout.binding),
            .descriptor_type  = u32(static_cast<rstd::uint32_t>(binding.layout.descriptorType)),
            .descriptor_count = u32(binding.layout.descriptorCount),
            .stage_flags      = u32(binding.layout.stageFlags),
        });
    }
    if (! m_shader->descriptor_sets.empty()) {
        artifact.descriptor_sets.reserve(usize(m_shader->descriptor_sets.size()));
        for (const auto& declared : m_shader->descriptor_sets) {
            auto bindings =
                rstd::vec::Vec<resource::ShaderArtifactDescriptorBinding>::with_capacity(
                    usize(declared.bindings.size()));
            for (const auto& binding : declared.bindings) {
                bindings.push(resource::ShaderArtifactDescriptorBinding {
                    .name             = String::make(rstd::cppstd::as_str(binding.name).unwrap()),
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
                target = rstd::addressof(
                    artifact.descriptor_sets[artifact.descriptor_sets.len() - usize(1)]);
            }
            target->bindings.push(binding.clone());
        }
        if (! artifact.descriptor_sets.is_empty()) {
            auto push_set = artifact.descriptor_sets[usize()].set;
            for (const auto& set : artifact.descriptor_sets) {
                if (set.set > push_set) push_set = set.set;
            }
            for (auto& set : artifact.descriptor_sets) {
                set.push_descriptor = set.set == push_set;
            }
        }
    }
    rstd::slice_::sort_unstable_by(artifact.descriptor_sets.as_mut_slice().as_mut_ref(),
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
    artifact.vertex_inputs.reserve(usize((**reflection).reflected.input_location_map.size()));
    for (const auto& [name, input] : (**reflection).reflected.input_location_map) {
        artifact.vertex_inputs.push(resource::ShaderArtifactVertexInput {
            .name     = rstd::string::String::make(rstd::cppstd::as_str(name).unwrap()),
            .location = u32(input.location),
            .format   = u32(static_cast<rstd::uint32_t>(input.format)),
        });
    }
    return rstd::Ok(rstd::move(artifact));
}

std::vector<Uni_ShaderSpv> ShaderSpvsFromArtifact(const resource::ShaderArtifact& artifact) {
    std::vector<Uni_ShaderSpv> out;
    out.reserve(artifact.stages.len().to_primitive());
    for (const auto& stage : artifact.stages) {
        auto spv         = Box<ShaderSpv>::make();
        spv->entry_point = rstd::cppstd::to_string(stage.entry_point.as_str());
        spv->stage       = stage.stage;
        spv->spirv.reserve(stage.code.len().to_primitive());
        for (auto word : stage.code) spv->spirv.push_back(word.to_primitive());
        out.push_back(std::move(spv));
    }
    return out;
}

auto ShaderReflectionFromArtifact(const resource::ShaderArtifact& artifact) -> ShaderReflected {
    ShaderReflected reflected;
    reflected.blocks.reserve(artifact.uniform_blocks.len().to_primitive());
    for (rstd::usize index {}; index < artifact.uniform_blocks.len(); ++index) {
        const auto&            block = artifact.uniform_blocks[index];
        ShaderReflected::Block prepared {
            .index   = static_cast<int>(index.to_primitive()),
            .size    = static_cast<unsigned>(block.size.to_primitive()),
            .name    = rstd::cppstd::to_string(block.name.as_str()),
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
            };
            reflected_member.array_dimensions.reserve(member.array_dimensions.len().to_primitive());
            for (auto dimension : member.array_dimensions) {
                reflected_member.array_dimensions.push_back(dimension.to_primitive());
            }
            prepared.member_map.emplace(rstd::cppstd::to_string(member.name.as_str()),
                                        std::move(reflected_member));
        }
        reflected.blocks.push_back(std::move(prepared));
    }
    for (const auto& binding : artifact.descriptor_bindings) {
        reflected.binding_map.emplace(
            rstd::cppstd::to_string(binding.name.as_str()),
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
        reflected.input_location_map.emplace(
            rstd::cppstd::to_string(input.name.as_str()),
            ShaderReflected::Input {
                .location = input.location.to_primitive(),
                .format   = static_cast<VkFormat>(input.format.to_primitive()),
            });
    }
    return reflected;
}

} // namespace owe::vulkan
