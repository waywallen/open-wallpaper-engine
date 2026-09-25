module;

#include <rstd/macro.hpp>
#include "vvk/macros.hpp"

module wescene.vulkan_render;
import wescene.spec_names;
import wescene.core;
import rstd.log;
import wescene.vulkan;
import wescene.scene;

using namespace owe::vulkan;
using namespace rstd::prelude;
using namespace rstd::literals;

CustomShaderPass::CustomShaderPass(Desc&& desc): m_desc(rstd::move(desc)) {}
CustomShaderPass::~CustomShaderPass() {}

namespace
{
Option<TextureRequest> TextureRequestFromScene(owe::Scene& scene, ref<str> name) {
    if (name.is_empty()) return None();
    auto text = name;
    if (! owe::IsSpecTex(text)) return Some(MakeImportedTextureRequest(name));
    auto target = scene.RenderTarget(text);
    if (target.is_none()) return None();
    return Some(MakeRenderTargetTextureRequest(name, **target));
}

bool IsDepthSampled(const TextureBindingRequest& binding) {
    return binding.request.is_some() && binding.request->definition.is_some() &&
           owe::resource::HasTextureUsage(binding.request->definition->usage,
                                          owe::resource::TextureUsage::DepthAttachment);
}

VkImageLayout SampledLayout(const TextureBindingRequest& binding) {
    return IsDepthSampled(binding) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                   : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

owe::SceneMaterial* ResolvePassMaterial(const CustomShaderPass::Desc& desc) {
    if (desc.material_override) return desc.material_override->as_ptr().as_raw_ptr();
    if (desc.node.is_none() || ! (*desc.node)->Mesh()) return nullptr;
    const auto& mesh          = *(*desc.node)->Mesh();
    const auto  submesh_index = desc.submesh_index.to_primitive();
    if (submesh_index >= mesh.Submeshes().len().to_primitive()) return nullptr;
    const auto& submesh       = mesh.Submeshes()[usize(submesh_index)];
    const auto& slots         = mesh.MaterialSlots();
    const auto  material_slot = submesh.material_slot.to_primitive();
    if (material_slot >= slots.len().to_primitive() || ! slots[usize(material_slot)])
        return nullptr;
    return slots[usize(material_slot)].as_ptr().as_raw_ptr();
}

} // namespace

PassInvalidationFlags CustomShaderPass::finalizeResourceRequests(Scene& scene) {
    PassInvalidationFlags flags = PassInvalidationNone;
    for (auto& binding : m_desc.texture_bindings) {
        auto name = binding.name.as_str();
        if (name.is_empty() || ! IsSpecTex(name)) continue;
        if (SetTextureRequestIfChanged(binding.request, TextureRequestFromScene(scene, name))) {
            flags |= ToPassInvalidationFlags(PassInvalidation::Resources);
        }
    }

    auto output_name = m_desc.output.as_str();
    if (! m_desc.output.is_empty() && IsSpecTex(output_name)) {
        auto target = scene.RenderTarget(output_name);
        if (target.is_some()) {
            const auto& rt = **target;
            if (m_desc.depth_only) {
                auto depth_request = MakeRenderTargetTextureRequest(m_desc.output.as_str(), rt);
                if (SetTextureRequestIfChanged(m_desc.depth_request,
                                               Some(rstd::move(depth_request)))) {
                    flags |= ToPassInvalidationFlags(PassInvalidation::Resources) |
                             ToPassInvalidationFlags(PassInvalidation::Framebuffer);
                }
                if (SetTextureRequestIfChanged(m_desc.output_request, None<TextureRequest>()) ||
                    SetTextureRequestIfChanged(m_desc.output_msaa_request,
                                               None<TextureRequest>())) {
                    flags |= ToPassInvalidationFlags(PassInvalidation::Resources) |
                             ToPassInvalidationFlags(PassInvalidation::Framebuffer);
                }
                if (! m_desc.has_depth_attachment || m_desc.samples != VK_SAMPLE_COUNT_1_BIT) {
                    m_desc.has_depth_attachment = true;
                    m_desc.samples              = VK_SAMPLE_COUNT_1_BIT;
                    flags |= PassInvalidationAll;
                }
                return flags;
            }
            auto output_request = MakeRenderTargetTextureRequest(m_desc.output.as_str(), rt);
            if (SetTextureRequestIfChanged(m_desc.output_request, rstd::move(output_request))) {
                flags |= ToPassInvalidationFlags(PassInvalidation::Resources) |
                         ToPassInvalidationFlags(PassInvalidation::Framebuffer);
            }

            auto samples = TextureSampleCount(rt.sample_count);
            if (m_desc.samples != samples) {
                m_desc.samples = samples;
                flags |= PassInvalidationAll;
            }

            Option<TextureRequest> msaa_request;
            if (samples != VK_SAMPLE_COUNT_1_BIT) {
                auto twin_name = MsaaTwinName(m_desc.output.as_str(), samples);
                msaa_request   = Some(MakeMsaaTextureRequest(twin_name.as_str(), rt, samples));
            }
            if (SetTextureRequestIfChanged(m_desc.output_msaa_request, rstd::move(msaa_request))) {
                flags |= ToPassInvalidationFlags(PassInvalidation::Resources) |
                         ToPassInvalidationFlags(PassInvalidation::Framebuffer);
            }

            auto* material = ResolvePassMaterial(m_desc);
            bool  has_depth_attachment =
                rt.withDepth && material != nullptr && UsesDepthAttachment(material->Pipeline());
            if (m_desc.has_depth_attachment != has_depth_attachment) {
                m_desc.has_depth_attachment = has_depth_attachment;
                flags |= PassInvalidationAll;
            }

            Option<TextureRequest> depth_request;
            if (has_depth_attachment) {
                depth_request = Some(
                    MakeDepthTextureRequest(rstd::format("{}::depth", m_desc.output).as_str(), rt));
            }
            if (SetTextureRequestIfChanged(m_desc.depth_request, rstd::move(depth_request))) {
                flags |= ToPassInvalidationFlags(PassInvalidation::Resources) |
                         ToPassInvalidationFlags(PassInvalidation::Framebuffer);
            }
        }
    }
    return flags;
}

void CustomShaderPass::declareResources(ResourceDeclarationContext& context) {
    m_desc.shader_use         = None();
    m_desc.buffer_uses        = {};
    m_desc.uniform_block_uses = {};
    m_desc.pipeline_use       = None();
    m_desc.render_pass_use    = None();
    m_desc.framebuffer_use    = None();
    if (m_desc.node.is_none() || (*m_desc.node)->Mesh() == nullptr) return;

    auto&              mesh          = *(*m_desc.node)->Mesh();
    const rstd::size_t submesh_index = m_desc.submesh_index.to_primitive();
    if (submesh_index >= mesh.Submeshes().len().to_primitive()) return;
    const auto& submesh  = mesh.Submeshes()[usize(submesh_index)];
    auto*       material = ResolvePassMaterial(m_desc);
    if (material == nullptr || ! material->customShader.shader) return;

    m_desc.pipeline_use    = Some(context.ReservePipeline());
    m_desc.render_pass_use = Some(context.ReserveRenderPass());
    m_desc.framebuffer_use = Some(context.ReserveFramebuffer());
    auto shader_request    = MakeSceneShaderRequest(**material->customShader.shader);
    auto artifact_request  = shader_request.clone();
    m_desc.shader_use =
        Some(context.AddShader(rstd::move(shader_request), **material->customShader.shader));

    auto artifact = context.ShaderArtifact(artifact_request);
    if (artifact.is_some()) {
        for (usize block_index {}; block_index < (**artifact).uniform_blocks.len(); ++block_index) {
            const auto& block = (**artifact).uniform_blocks[block_index];
            auto        size  = block.size;
            if (size != usize()) {
                auto name =
                    block.scope == resource::ShaderArtifactUniformBlock::Scope::Shared
                        ? rstd::format("shared-uniform:{}", block.identity)
                    : m_desc.draw_item.Valid() && ! m_desc.material_override
                        ? (m_desc.render_view == SceneRenderViewKind::Primary
                               ? rstd::format("{}:{}:{}",
                                              BuildDrawBufferResourceName(m_desc.draw_item,
                                                                          DrawBufferRole::Uniform),
                                              block.set,
                                              block.binding)
                               : rstd::format("draw:{}:{}:uniform:{}:{}:{}",
                                              m_desc.draw_item.generation,
                                              m_desc.draw_item.index,
                                              static_cast<rstd::uint32_t>(m_desc.render_view),
                                              block.set,
                                              block.binding))
                        : context.ScopeResourceName(rstd::format("pass:{}:{}:uniform:{}:{}",
                                                                 m_desc.graph_pass_index,
                                                                 m_desc.submesh_index,
                                                                 block.set,
                                                                 block.binding));
                auto request = resource::BufferRequest {
                    .name = rstd::move(name),
                    .definition =
                        resource::BufferDefinition {
                            .size      = size,
                            .usage     = resource::BufferUsage::Uniform,
                            .alignment = usize(4),
                        },
                    .lifetime = resource::BufferLifetimeClass::Dynamic,
                };
                auto use = block.scope == resource::ShaderArtifactUniformBlock::Scope::Shared
                               ? context.AddSharedBuffer(rstd::move(request))
                               : context.AddBuffer(rstd::move(request));
                m_desc.uniform_block_uses.push(UniformBlockUse {
                    .block_index = block_index,
                    .use         = use,
                });
            }
        }
    }

    for (rstd::size_t index = 0; index < submesh.vertex_arrays.len().to_primitive(); ++index) {
        const auto& vertex = submesh.vertex_arrays[usize(index)];
        auto name = m_desc.draw_item.Valid()
                        ? BuildDrawBufferResourceName(m_desc.draw_item,
                                                      DrawBufferRole::Vertex,
                                                      u32(static_cast<rstd::uint32_t>(index)))
                        : context.ScopeResourceName(rstd::format("pass:{}:{}:vertex:{}",
                                                                 m_desc.graph_pass_index,
                                                                 m_desc.submesh_index,
                                                                 index));
        auto use  = context.AddBuffer(
            resource::BufferRequest {
                .name = rstd::move(name),
                .definition =
                    resource::BufferDefinition {
                        .size      = vertex.CapacitySizeOf(),
                        .usage     = resource::BufferUsage::Vertex,
                        .alignment = usize(4),
                    },
                .lifetime         = mesh.Dynamic() ? resource::BufferLifetimeClass::Dynamic
                                                   : resource::BufferLifetimeClass::Retained,
                .content_version  = vertex.DataGeneration(),
                .content_identity = vertex.BufferView().storage_generation,
            },
            slice<u8>::from_raw_parts(reinterpret_cast<const byte*>(vertex.Data()),
                                      vertex.CapacitySizeOf()));
        m_desc.buffer_uses.push(rstd::move(use));
    }
    if (submesh.index_arrays.is_empty()) return;

    const auto& index = submesh.index_arrays[usize(0)];
    auto name = m_desc.draw_item.Valid()
                    ? BuildDrawBufferResourceName(m_desc.draw_item, DrawBufferRole::Index)
                    : context.ScopeResourceName(rstd::format(
                          "pass:{}:{}:index", m_desc.graph_pass_index, m_desc.submesh_index));
    auto use  = context.AddBuffer(
        resource::BufferRequest {
            .name = rstd::move(name),
            .definition =
                resource::BufferDefinition {
                    .size      = index.CapacitySizeof(),
                    .usage     = resource::BufferUsage::Index,
                    .alignment = usize(4),
                },
            .lifetime         = mesh.Dynamic() ? resource::BufferLifetimeClass::Dynamic
                                               : resource::BufferLifetimeClass::Retained,
            .content_version  = index.DataGeneration(),
            .content_identity = index.BufferView().storage_generation,
        },
        slice<u8>::from_raw_parts(reinterpret_cast<const byte*>(index.Data()),
                                  index.CapacitySizeof()));
    m_desc.buffer_uses.push(rstd::move(use));
}

PassResourceUses CustomShaderPass::resourceUses() const {
    PassResourceUses uses;
    for (const auto& binding : m_desc.texture_bindings) {
        if (binding.use.is_some()) {
            uses.textures.push(resource::TextureUseHandle(*binding.use));
        }
    }
    if (m_desc.output_use.is_some()) {
        uses.textures.push(resource::TextureUseHandle(*m_desc.output_use));
    }
    if (m_desc.output_msaa_use.is_some()) {
        uses.textures.push(resource::TextureUseHandle(*m_desc.output_msaa_use));
    }
    if (m_desc.depth_use.is_some()) {
        uses.textures.push(resource::TextureUseHandle(*m_desc.depth_use));
    }
    for (const auto& use : m_desc.buffer_uses) {
        uses.buffers.push(resource::BufferUseHandle(use));
    }
    for (const auto& block : m_desc.uniform_block_uses)
        uses.buffers.push(resource::BufferUseHandle(block.use));
    if (m_desc.shader_use.is_some()) {
        uses.shaders.push(resource::ShaderUseHandle(*m_desc.shader_use));
    }
    if (m_desc.pipeline_use.is_some()) {
        uses.pipelines.push(resource::PipelineUseHandle(*m_desc.pipeline_use));
    }
    if (m_desc.render_pass_use.is_some()) {
        uses.render_passes.push(resource::RenderPassUseHandle(*m_desc.render_pass_use));
    }
    if (m_desc.framebuffer_use.is_some()) {
        uses.framebuffers.push(resource::FramebufferUseHandle(*m_desc.framebuffer_use));
    }
    for (const auto& descriptor : m_desc.descriptor_bindings)
        uses.descriptors.push(resource::DescriptorBindingHandle(descriptor.binding));
    return uses;
}

auto CustomShaderPass::pipelineLayoutRequirement(const PreparedPassResources& resources) const
    -> Result<Option<PipelineLayoutRequirement>, resource::ResourceError> {
    if (m_desc.pipeline_use.is_none() || m_desc.shader_use.is_none()) return Ok(None());
    auto prepared_shader = resources.Resolve(*m_desc.shader_use);
    if (prepared_shader.is_none()) {
        return Err(resource::ResourceError {
            .kind    = resource::ResourceErrorKind::MissingContent,
            .message = rstd::format("prepared shader artifact unavailable"),
        });
    }
    const auto& artifact = (**prepared_shader).shader.physical->artifact;
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
        .pipeline = *m_desc.pipeline_use,
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
    return Ok(Some(rstd::move(requirement)));
}

auto CustomShaderPass::globalDescriptorBufferUses(const PreparedPassResources& resources) const
    -> Result<Vec<GlobalDescriptorBufferUse>, resource::ResourceError> {
    auto uses = Vec<GlobalDescriptorBufferUse>::make();
    if (m_desc.shader_use.is_none()) return Ok(rstd::move(uses));
    auto prepared_shader = resources.Resolve(*m_desc.shader_use);
    if (prepared_shader.is_none()) {
        return Err(resource::ResourceError {
            .kind    = resource::ResourceErrorKind::MissingContent,
            .message = rstd::format("prepared shader artifact unavailable"),
        });
    }
    const auto& artifact = (**prepared_shader).shader.physical->artifact;
    for (const auto& use : m_desc.uniform_block_uses) {
        if (use.block_index >= artifact.uniform_blocks.len()) continue;
        const auto& block = artifact.uniform_blocks[use.block_index];
        if (block.scope != resource::ShaderArtifactUniformBlock::Scope::Shared) continue;
        if (block.set != u32()) {
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingDefinition,
                .message = rstd::format("shared resources must use descriptor set 0"),
            });
        }
        uses.push(GlobalDescriptorBufferUse {
            .binding  = block.binding,
            .identity = block.identity,
            .buffer   = use.use,
        });
    }
    return Ok(rstd::move(uses));
}

Option<owe::RenderItemId> CustomShaderPass::renderItemId() const {
    if (! m_desc.render_item.Valid()) return None();
    return Some<owe::RenderItemId>(m_desc.render_item);
}

Option<PipelineCacheKey> CustomShaderPass::pipelineCacheKey() const {
    if (m_desc.pipeline_cache_key.is_none()) return None();
    return Some<PipelineCacheKey>(m_desc.pipeline_cache_key->clone());
}

bool CustomShaderPass::pipelineCacheHit() const { return m_desc.pipeline_cache_hit; }

u64 CustomShaderPass::pipelineCacheObservedCount() const {
    return m_desc.pipeline_cache_observed_count;
}

Option<RenderPassCacheKey> CustomShaderPass::renderPassCacheKey() const {
    if (m_desc.render_pass_cache_key.is_none()) return None();
    return Some<RenderPassCacheKey>(m_desc.render_pass_cache_key->clone());
}

bool CustomShaderPass::renderPassCacheHit() const { return m_desc.render_pass_cache_hit; }

u64 CustomShaderPass::renderPassCacheObservedCount() const {
    return m_desc.render_pass_cache_observed_count;
}

Option<FramebufferCacheKey> CustomShaderPass::framebufferCacheKey() const {
    if (m_desc.framebuffer_cache_key.is_none()) return None();
    return Some<FramebufferCacheKey>(m_desc.framebuffer_cache_key->clone());
}

bool CustomShaderPass::framebufferCacheHit() const { return m_desc.framebuffer_cache_hit; }

u64 CustomShaderPass::framebufferCacheObservedCount() const {
    return m_desc.framebuffer_cache_observed_count;
}

Vec<PassTextureRequestDiagnostic> CustomShaderPass::textureRequestDiagnostics() const {
    Vec<PassTextureRequestDiagnostic> out;
    out.reserve(usize(m_desc.texture_bindings.len().to_primitive() + 3));
    for (rstd::size_t i = 0; i < m_desc.texture_bindings.len().to_primitive(); ++i) {
        const auto& binding = m_desc.texture_bindings[usize(i)];
        if (binding.name.is_empty() && binding.request.is_none()) continue;
        out.push(PassTextureRequestDiagnostic {
            .role = "sampled"_Str,
            .slot = u32(static_cast<rstd::uint32_t>(i)),
            .name = binding.name.clone(),
            .use  = binding.use,
            .request =
                binding.request.is_some() ? Some(binding.request->clone()) : None<TextureRequest>(),
        });
    }
    if (! m_desc.output.is_empty() || m_desc.output_request.is_some()) {
        out.push(PassTextureRequestDiagnostic {
            .role    = "output"_Str,
            .name    = m_desc.output.clone(),
            .use     = m_desc.output_use,
            .request = m_desc.output_request.is_some() ? Some(m_desc.output_request->clone())
                                                       : None<TextureRequest>(),
        });
    }
    if (m_desc.output_msaa_request.is_some()) {
        out.push(PassTextureRequestDiagnostic {
            .role    = "output-msaa"_Str,
            .name    = m_desc.output_msaa_request->name.clone(),
            .use     = m_desc.output_msaa_use,
            .request = Some(m_desc.output_msaa_request->clone()),
        });
    }
    if (m_desc.depth_request.is_some()) {
        out.push(PassTextureRequestDiagnostic {
            .role    = "depth"_Str,
            .name    = m_desc.depth_request->name.clone(),
            .use     = m_desc.depth_use,
            .request = Some(m_desc.depth_request->clone()),
        });
    }
    return out;
}

MaterialTextureBindingRefresh
CustomShaderPass::refreshMaterialTextureBindings(const RenderSceneSnapshot& render_scene) {
    MaterialTextureBindingRefresh result;
    if (m_desc.material_override) {
        // This pass renders a build-time clone of the material (effect chain);
        // an in-place refresh cannot see the mutated base material (e.g. a new
        // $mediaThumbnail), so rebuild the graph to regenerate the override.
        // Only reached when the caller matched this pass to a changed material.
        result.requires_graph_rebuild = true;
        return result;
    }
    if (m_desc.node.is_none() || (*m_desc.node)->Mesh() == nullptr) {
        return result;
    }

    auto&              mesh          = *(*m_desc.node)->Mesh();
    const rstd::size_t submesh_index = m_desc.submesh_index.to_primitive();
    if (submesh_index >= mesh.Submeshes().len().to_primitive()) return result;
    const auto& submesh       = mesh.Submeshes()[usize(submesh_index)];
    const auto& slots         = mesh.MaterialSlots();
    const auto  material_slot = submesh.material_slot.to_primitive();
    if (material_slot >= slots.len().to_primitive() || ! slots[usize(material_slot)]) return result;

    const auto& textures = slots[usize(material_slot)]->textures;
    if (textures.len().to_primitive() != m_desc.texture_bindings.len().to_primitive()) {
        result.requires_graph_rebuild = true;
        return result;
    }

    for (rstd::size_t i = 0; i < textures.len().to_primitive(); ++i) {
        const auto& next     = textures[usize(i)].as_str();
        const auto& old      = m_desc.texture_bindings[usize(i)];
        auto        old_name = old.name.as_str();
        if (! CanRefreshSceneMaterialTextureBinding(old_name, next, m_desc.output.as_str())) {
            result.requires_graph_rebuild = true;
            return result;
        }
    }

    for (rstd::size_t i = 0; i < textures.len().to_primitive(); ++i) {
        const auto& next     = textures[usize(i)].as_str();
        auto&       old      = m_desc.texture_bindings[usize(i)];
        auto        next_dep = ClassifySceneMaterialTexture(next);
        if (old.name == next && ! IsLocalSceneMaterialTextureDependency(next_dep)) continue;

        if (! next.is_empty()) {
            // A slot that was empty at graph build (e.g. $mediaThumbnail before
            // any track played) has no texture use slot or graph read edge, so
            // an in-place refresh cannot bind the new image. Likewise a texture
            // registered after the snapshot was taken (runtime media art) is
            // unknown to this snapshot. Both need a graph rebuild to bind.
            if (old.use.is_none() || render_scene.textureDescId(next).is_none()) {
                result.requires_graph_rebuild = true;
                return result;
            }
        }

        TextureBindingRequest binding;
        if (! next.is_empty()) {
            binding.name = String::make(next);
            binding.use  = old.use;
            binding.request =
                Some(MakeImportedTextureRequest(next, render_scene.textureDescId(next)));
        }

        if (! SameTextureBindingRequest(old, binding)) {
            old = rstd::move(binding);
            result.invalidation_flags |= ToPassInvalidationFlags(PassInvalidation::Resources);
        }
    }

    return result;
}

auto CustomShaderPass::createUniformBufferUpdates(ref<dyn<UniformBindingPrepareContext>> prepare,
                                                  const PreparedPassResources&           resources)
    -> Result<Vec<Box<dyn<UniformBufferUpdate>>>, UniformBufferUpdateError> {
    auto updates = Vec<Box<dyn<UniformBufferUpdate>>>::make();
    if (m_desc.uniform_block_uses.is_empty() || m_desc.shader_use.is_none()) {
        return Ok(rstd::move(updates));
    }
    auto prepared = resources.Resolve(*m_desc.shader_use);
    if (prepared.is_none()) {
        return Err(UniformBufferUpdateError {
            .message = "prepared uniform shader is unavailable"_Str,
        });
    }
    const auto& artifact = (**prepared).shader.physical->artifact;
    if (artifact.uniform_blocks.is_empty()) {
        return Ok(rstd::move(updates));
    }
    auto draw_item = m_desc.draw_item;
    if (prepare->ResolveDraw(draw_item).is_none() && m_desc.node.is_some()) {
        auto node    = ref<SceneNode>::from_raw_parts((*m_desc.node).as_ptr());
        auto current = prepare->DrawItemFor(node, m_desc.submesh_index);
        if (current.is_some()) draw_item = *current;
    }
    auto draw = prepare->ResolveDraw(draw_item);
    if (draw.is_none()) {
        return Err(UniformBufferUpdateError {
            .message = "uniform texture metadata draw is unavailable"_Str,
        });
    }
    auto textures = Vec<PreparedUniformTextureMetadata>::with_capacity(
        usize(m_desc.texture_bindings.len().to_primitive()));
    for (rstd::size_t index = 0; index < m_desc.texture_bindings.len().to_primitive(); ++index) {
        PreparedUniformTextureMetadata metadata;
        const auto&                    binding = m_desc.texture_bindings[usize(index)];
        if (binding.use.is_some()) {
            auto prepared = resources.Resolve(*binding.use);
            if (prepared.is_none()) {
                return Err(UniformBufferUpdateError {
                    .message = rstd::format("prepared texture metadata {} is unavailable",
                                            binding.name.as_str()),
                });
            }
            const auto image       = (**prepared).image.getActive();
            metadata.available     = true;
            metadata.source_extent = { static_cast<float>(image.extent.width),
                                       static_cast<float>(image.extent.height) };
            metadata.sample_extent = metadata.source_extent;
            metadata.has_mipmap    = (**prepared).request.kind == TextureRequestKind::RenderTarget;
            metadata.mipmap_level  = static_cast<float>(image.mipmap_level);
            metadata.revision      = (**prepared).physical_generation ^ image.generation;
            if (metadata.revision == u64()) metadata.revision = u64(1);
        }
        if (index < draw->material->texture_metadata.len().to_primitive()) {
            const auto& authored = draw->material->texture_metadata[usize(index)];
            if (authored.has_extent) {
                metadata.available     = true;
                metadata.source_extent = authored.source_extent;
                metadata.sample_extent = authored.sample_extent;
            }
        }
        textures.push(rstd::move(metadata));
    }
    for (const auto& use : m_desc.uniform_block_uses) {
        if (use.block_index >= artifact.uniform_blocks.len()) continue;
        const auto& block = artifact.uniform_blocks[use.block_index];
        auto        binding =
            block.scope == resource::ShaderArtifactUniformBlock::Scope::Shared
                ? MakeSharedUniformBufferBinding(
                      prepare, use.use, block, artifact.matrix_convention, artifact.matrix_abi)
                : MakeUniformBufferBinding(
                      prepare,
                      draw_item,
                      use.use,
                      block,
                      textures.clone(),
                      m_desc.render_view,
                      artifact.matrix_convention,
                      artifact.matrix_abi,
                      m_desc.material_override
                          ? Some(ref<SceneMaterial>::from_raw_parts(
                                m_desc.material_override->as_ptr().as_raw_ptr()))
                          : None<ref<SceneMaterial>>());
        if (binding.is_err()) return Err(rstd::move(binding).unwrap_err_unchecked());
        updates.push(rstd::move(binding).unwrap_unchecked());
    }
    return Ok(rstd::move(updates));
}

bool CustomShaderPass::prepareResourceStates(
    mut_ref<dyn<resource_registry::TextureStatePreparer>> states) {
    m_desc.sampled_barriers.Clear();
    for (const auto& binding : m_desc.texture_bindings) {
        if (binding.use.is_none()) continue;
        const bool depth = IsDepthSampled(binding);
        auto       range = resource_registry::TextureSubresourceRange {
            .aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
        };
        auto barrier = states->Prepare(*binding.use,
                                       depth ? resource_registry::TextureStateKind::DepthSampled
                                             : resource_registry::TextureStateKind::Sampled,
                                       range);
        if (barrier.is_none()) {
            rstd_error("prepare sampled texture state failed for {}", binding.name);
            return false;
        }
        m_desc.sampled_barriers.Add(rstd::move(barrier).unwrap_unchecked());
    }
    if (m_desc.output_use.is_some() &&
        ! states->Set(*m_desc.output_use, resource_registry::TextureStateKind::Sampled)) {
        rstd_error("prepare output texture state failed for {}", m_desc.output);
        return false;
    }
    if (m_desc.output_msaa_use.is_some() &&
        ! states->Set(*m_desc.output_msaa_use,
                      resource_registry::TextureStateKind::ColorAttachment)) {
        rstd_error("prepare MSAA output texture state failed for {}", m_desc.output);
        return false;
    }
    if (m_desc.depth_use.is_some() &&
        ! states->Set(*m_desc.depth_use,
                      m_desc.depth_only ? resource_registry::TextureStateKind::DepthSampled
                                        : resource_registry::TextureStateKind::DepthAttachment)) {
        rstd_error("prepare depth texture state failed for {}", m_desc.output);
        return false;
    }
    return true;
}

void CustomShaderPass::prepare(Scene& scene, const Device& device, PassPrepareContext& context) {
    Vec<ImageSlotsRef> vk_textures;
    vk_textures.resize(m_desc.texture_bindings.len(), ImageSlotsRef {});
    ImageParameters vk_output;
    ImageParameters vk_output_msaa;
    ImageParameters vk_depth;
    for (rstd::size_t i = 0; i < m_desc.texture_bindings.len().to_primitive(); i++) {
        auto& binding = m_desc.texture_bindings[usize(i)];
        if (binding.empty()) continue;

        if (binding.use.is_none()) {
            rstd_error("sampled texture {} has no resource use", binding.name);
            return;
        }
        auto prepared = context.resources->Resolve(*binding.use);
        if (prepared.is_none()) {
            rstd_error("prepared sampled texture {} not found", binding.name);
            return;
        }
        vk_textures[usize(i)] = (**prepared).image;
    }
    bool                   out_force_clear { false };
    Option<bool>           out_blend_alpha_write;
    Option<TextureRequest> output_attachment_request;
    Option<TextureRequest> msaa_attachment_request;
    Option<TextureRequest> depth_attachment_request;
    {
        auto& tex_name = m_desc.output;
        auto  name     = tex_name.as_str();
        rstd_assert(IsSpecTex(name));
        auto target = scene.RenderTarget(name);
        rstd_assert(target.is_some());
        if (target.is_none()) return;
        const auto& rt        = **target;
        out_force_clear       = rt.force_clear && ! m_desc.preserve_output;
        out_blend_alpha_write = rt.blend_alpha_write;
        if (m_desc.depth_only) {
            if (m_desc.depth_use.is_none()) return;
            auto prepared = context.resources->Resolve(*m_desc.depth_use);
            if (prepared.is_none()) {
                rstd_error("prepared depth output texture {} not found", tex_name);
                return;
            }
            vk_depth                 = (**prepared).image.getActive();
            m_desc.output_extent     = { vk_depth.extent.width, vk_depth.extent.height };
            depth_attachment_request = Some((**prepared).request.clone());
            m_desc.samples           = VK_SAMPLE_COUNT_1_BIT;
        } else {
            if (m_desc.output_use.is_none()) return;
            auto prepared = context.resources->Resolve(*m_desc.output_use);
            if (prepared.is_none()) {
                rstd_error("prepared output texture {} not found", tex_name);
                return;
            }
            vk_output                 = (**prepared).image.getActive();
            m_desc.output_extent      = { vk_output.extent.width, vk_output.extent.height };
            output_attachment_request = Some((**prepared).request.clone());
            m_desc.samples            = TextureSampleCount(rt.sample_count);
            if (m_desc.samples != VK_SAMPLE_COUNT_1_BIT) {
                if (m_desc.output_msaa_use.is_none()) return;
                auto msaa = context.resources->Resolve(*m_desc.output_msaa_use);
                if (msaa.is_none()) {
                    rstd_error("prepared MSAA texture {} not found", tex_name);
                    return;
                }
                vk_output_msaa          = (**msaa).image.getActive();
                msaa_attachment_request = Some((**msaa).request.clone());
            }
        }
    }

    if (m_desc.node.is_none() || (*m_desc.node)->Mesh() == nullptr) return;
    SceneMesh&         mesh          = *(*m_desc.node)->Mesh();
    const rstd::size_t submesh_index = m_desc.submesh_index.to_primitive();
    if (mesh.Submeshes().is_empty() || submesh_index >= mesh.Submeshes().len().to_primitive())
        return;
    const auto& submesh  = mesh.Submeshes()[usize(submesh_index)];
    auto*       material = ResolvePassMaterial(m_desc);
    if (material == nullptr) return;
    SceneMaterial& material_ref = *material;
    auto           output_rt    = scene.RenderTarget(m_desc.output.as_str());
    if (output_rt.is_none()) return;
    m_desc.depth_clear_value = (**output_rt).depth_clear_value;
    const bool has_depth_attachment =
        m_desc.depth_only ||
        ((**output_rt).withDepth && UsesDepthAttachment(material_ref.Pipeline()));
    m_desc.has_depth_attachment = has_depth_attachment;
    VkAttachmentLoadOp depthLoadOp { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
    if (has_depth_attachment) {
        depthLoadOp = m_desc.clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        m_desc.depth_load_op = depthLoadOp;

        if (! m_desc.depth_only) {
            if (m_desc.depth_use.is_none()) return;
            auto depth = context.resources->Resolve(*m_desc.depth_use);
            if (depth.is_none()) {
                rstd_error("prepared depth texture {} not found", m_desc.output);
                return;
            }
            vk_depth                 = (**depth).image.getActive();
            depth_attachment_request = Some((**depth).request.clone());
        }
    }

    Vec<Uni_ShaderSpv>     spvs;
    ShaderReflected        shader_reflection;
    const ShaderReflected* ref { nullptr };
    {
        SceneShader& shader = **material_ref.customShader.shader;

        if (m_desc.shader_use.is_none()) {
            rstd_error("shader artifact provider unavailable, {}", shader.name);
            return;
        }
        auto prepared_shader = context.resources->Resolve(*m_desc.shader_use);
        if (prepared_shader.is_none()) {
            rstd_error("prepared shader artifact unavailable, {}", shader.name);
            return;
        }
        const auto& artifact = (**prepared_shader).shader.physical->artifact;
        shader_reflection    = ShaderReflectionFromArtifact(artifact);
        spvs                 = ShaderSpvsFromArtifact(artifact);
        if (spvs.is_empty()) {
            rstd_error("prepared shader artifact is empty, {}", shader.name);
            return;
        }
        ref = &shader_reflection;

        m_desc.vk_tex_binding.clear();
        m_desc.vk_tex_binding.reserve(vk_textures.len());
        m_desc.vk_tex_set.clear();
        m_desc.vk_tex_set.reserve(vk_textures.len());

        for (rstd::size_t i = 0; i < vk_textures.len().to_primitive(); i++) {
            rstd::int32_t binding { -1 };
            u32           set {};
            const auto    member = shader.SamplerMember(i);
            if (! member.is_empty()) {
                auto reflected = ref->binding_map.get(member);
                if (reflected.is_some()) {
                    binding = static_cast<rstd::int32_t>((**reflected).layout.binding);
                    set     = u32((**reflected).set);
                }
            }
            m_desc.vk_tex_binding.push(rstd::move(binding));
            m_desc.vk_tex_set.push(rstd::move(set));
        }
    }

    Vec<VkVertexInputBindingDescription>   bind_descriptions;
    Vec<VkVertexInputAttributeDescription> attr_descriptions;
    {
        RenderBufferResolver buffer_resolver(*context.resources);
        DrawBufferRequest    buffer_request { .render_item   = m_desc.render_item,
                                              .mesh          = &mesh,
                                              .submesh_index = m_desc.submesh_index,
                                              .buffer_uses   = m_desc.buffer_uses.as_slice() };
        auto                 draw_buffers = buffer_resolver.prepareDrawBuffers(buffer_request);
        if (! draw_buffers) return;
        m_desc.draw_buffers = rstd::move(*draw_buffers);

        for (unsigned i = 0; i < submesh.vertex_arrays.len().to_primitive(); i++) {
            const auto& vertex = submesh.vertex_arrays[usize(i)];

            VkVertexInputBindingDescription bind_desc {
                .binding   = i,
                .stride    = static_cast<rstd::uint32_t>(vertex.OneSizeOf().to_primitive()),
                .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
            };
            bind_descriptions.push(rstd::move(bind_desc));

            for (const auto& [name, input_ref] : ref->input_location_map.iter()) {
                const auto& input  = *input_ref;
                usize       offset = vertex.AttributeOffset(name->as_str()).unwrap_or(usize());

                VkVertexInputAttributeDescription attr_desc {
                    .location = input.location,
                    .binding  = i,
                    .format   = input.format,
                    .offset   = static_cast<rstd::uint32_t>(offset.to_primitive()),
                };
                attr_descriptions.push(rstd::move(attr_desc));
            }
        }
    }
    {
        VkPipelineColorBlendAttachmentState color_blend {};
        VkAttachmentLoadOp                  loadOp { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
        const auto                          blendmode = material_ref.Pipeline().blend_mode;
        {
            VkColorComponentFlags colorMask =
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
            const bool default_writes_alpha =
                ! ((*m_desc.node)->Camera().is_empty() ||
                   (*m_desc.node)->Camera().starts_with("global"_str));
            SetBlend(blendmode, color_blend);
            const auto& alpha_write = material_ref.Pipeline().alpha_write;
            const bool  writes_alpha =
                alpha_write.is_some() ? *alpha_write
                                      : (color_blend.blendEnable
                                             ? out_blend_alpha_write.unwrap_or(default_writes_alpha)
                                             : default_writes_alpha);

            if (writes_alpha) colorMask |= VK_COLOR_COMPONENT_A_BIT;
            color_blend.colorWriteMask = colorMask;

            SetAlphaBlendWritePolicy(color_blend, writes_alpha);
            m_desc.blending = color_blend.blendEnable;

            SetAttachmentLoadOp(blendmode, loadOp);
            if (m_desc.preserve_output) loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            if (m_desc.clear_output) loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            if (out_force_clear) loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        }
        m_desc.color_load_op                       = loadOp;
        constexpr VkFormat      color_format       = VK_FORMAT_R8G8B8A8_UNORM;
        constexpr VkImageLayout color_final_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        GraphicsPipeline pipeline_state;
        pipeline_state.toDefault();
        pipeline_state.setSampleCount(m_desc.samples);
        SetAlphaToCoverage(blendmode, pipeline_state.multisample);
        if (has_depth_attachment) SetDepthState(material_ref.Pipeline(), pipeline_state.depth);
        SetRasterState(
            material_ref.Pipeline(), device.capabilities().depth_clamp, pipeline_state.raster);
        const bool          has_index = m_desc.draw_buffers.hasIndex();
        VkPrimitiveTopology topology  = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        switch (mesh.Primitive()) {
        case MeshPrimitive::POINT: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case MeshPrimitive::TRIANGLE:
            topology = has_index ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                                 : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            break;
        }
        if (m_desc.pipeline_use.is_none()) return;
        auto pipeline_layout = context.pipeline_layouts->Resolve(*m_desc.pipeline_use);
        if (pipeline_layout.is_none()) {
            rstd_error("pipeline layout assignment unavailable");
            return;
        }
        PipelineResourceRequest pipeline_request {
            .pipeline_layout = *pipeline_layout,
            .vertex_bindings = rstd::move(bind_descriptions),
            .vertex_attrs    = rstd::move(attr_descriptions),
            .shader_stages   = rstd::move(spvs),
            .color_blend     = color_blend,
            .depth           = pipeline_state.depth,
            .raster          = pipeline_state.raster,
            .depth_clip      = material_ref.Pipeline().depth_clip,
            .multisample     = pipeline_state.multisample,
            .topology        = topology,
            .viewport_count =
                m_desc.viewports.is_empty()
                    ? 1u
                    : static_cast<rstd::uint32_t>(m_desc.viewports.len().to_primitive()),
            .scissor_count = m_desc.scissors.is_empty() ? 1u
                                                        : static_cast<rstd::uint32_t>(
                                                              m_desc.scissors.len().to_primitive()),
            .color_format  = color_format,
            .color_final_layout   = color_final_layout,
            .color_load_op        = loadOp,
            .depth_load_op        = depthLoadOp,
            .depth_store_op       = VK_ATTACHMENT_STORE_OP_STORE,
            .depth_final_layout   = m_desc.depth_only
                                        ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                        : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .has_color_attachment = ! m_desc.depth_only,
            .has_depth_attachment = has_depth_attachment,
        };
        m_desc.pipeline_cache_key               = None();
        m_desc.render_pass_cache_key            = None();
        m_desc.pipeline_cache_hit               = false;
        m_desc.pipeline_cache_observed_count    = u64();
        m_desc.render_pass_cache_hit            = false;
        m_desc.render_pass_cache_observed_count = u64();
        if (m_desc.render_pass_use.is_none()) return;
        auto prepared = context.graphics->PreparePipeline(
            *m_desc.pipeline_use, *m_desc.render_pass_use, device, rstd::move(pipeline_request));
        if (prepared.is_err()) {
            auto error = rstd::move(prepared).unwrap_err_unchecked();
            rstd_error("prepare pipeline failed: {}", error.message);
            return;
        }
        auto pipeline                           = rstd::move(prepared).unwrap_unchecked();
        m_desc.pipeline_cache_key               = Some(rstd::move(pipeline.cache_key));
        m_desc.render_pass_cache_key            = Some(rstd::move(pipeline.render_pass_key));
        m_desc.pipeline_cache_hit               = pipeline.cache_hit;
        m_desc.pipeline_cache_observed_count    = pipeline.cache_observed_count;
        m_desc.render_pass_cache_hit            = pipeline.render_pass_cache_hit;
        m_desc.render_pass_cache_observed_count = pipeline.render_pass_cache_observed_count;
    }

    {
        const bool has_msaa = m_desc.samples != VK_SAMPLE_COUNT_1_BIT;
        if (! m_desc.depth_only && output_attachment_request.is_none()) return;
        if (has_msaa && msaa_attachment_request.is_none()) return;
        if (has_depth_attachment && depth_attachment_request.is_none()) return;

        Vec<FramebufferAttachmentDesc> attachments;
        attachments.reserve(usize((m_desc.depth_only ? 0u : (has_msaa ? 2u : 1u)) +
                                  (has_depth_attachment ? 1u : 0u)));
        if (! m_desc.depth_only && has_msaa) {
            attachments.push(MakeFramebufferAttachment(*msaa_attachment_request, vk_output_msaa));
        }
        if (! m_desc.depth_only)
            attachments.push(MakeFramebufferAttachment(*output_attachment_request, vk_output));
        if (has_depth_attachment) {
            attachments.push(MakeFramebufferAttachment(*depth_attachment_request, vk_depth));
        }

        m_desc.framebuffer_cache_key            = None();
        m_desc.framebuffer_cache_hit            = false;
        m_desc.framebuffer_cache_observed_count = u64();
        if (m_desc.framebuffer_use.is_none() || m_desc.render_pass_use.is_none()) return;
        auto prepared = context.graphics->PrepareFramebuffer(*m_desc.framebuffer_use,
                                                             *m_desc.render_pass_use,
                                                             device,
                                                             rstd::move(attachments),
                                                             m_desc.output_extent);
        if (prepared.is_err()) {
            auto error = rstd::move(prepared).unwrap_err_unchecked();
            rstd_error("prepare framebuffer failed: {}", error.message);
            return;
        }
        auto framebuffer                        = rstd::move(prepared).unwrap_unchecked();
        m_desc.framebuffer_cache_key            = Some(rstd::move(framebuffer.cache_key));
        m_desc.framebuffer_cache_hit            = framebuffer.cache_hit;
        m_desc.framebuffer_cache_observed_count = framebuffer.cache_observed_count;
    }

    {
        if (m_desc.shader_use.is_none() || m_desc.pipeline_use.is_none()) return;
        auto prepared_shader = context.resources->Resolve(*m_desc.shader_use);
        if (prepared_shader.is_none()) return;
        const auto& artifact = (**prepared_shader).shader.physical->artifact;
        m_desc.descriptor_bindings.clear();
        m_desc.descriptor_image_slots.clear();
        m_desc.descriptor_image_slots.resize(vk_textures.len(), 0);
        for (const auto& set : artifact.descriptor_sets) {
            if (set.set == u32()) continue;
            auto images = Vec<resource_registry::DescriptorImageBinding>::make();
            for (rstd::size_t index = 0; index < vk_textures.len().to_primitive(); ++index) {
                const auto binding = m_desc.vk_tex_binding[usize(index)];
                auto&      slots   = vk_textures[usize(index)];
                if (binding < 0 || slots.slots.is_empty() ||
                    m_desc.vk_tex_set[usize(index)] != set.set)
                    continue;
                auto frame = scene.TextureFrame(m_desc.draw_item, usize(index));
                if (frame.is_some() && frame->image_slot < slots.slots.len()) {
                    slots.active = static_cast<rstd::ptrdiff_t>(frame->image_slot.to_primitive());
                    m_desc.descriptor_image_slots[usize(index)] = slots.active;
                }
                images.push(resource_registry::DescriptorImageBinding {
                    .binding = static_cast<rstd::uint32_t>(binding),
                    .image   = slots.getActive(),
                    .layout  = SampledLayout(m_desc.texture_bindings[usize(index)]),
                });
            }
            auto buffers = Vec<resource_registry::DescriptorBufferBinding>::make();
            for (const auto& use : m_desc.uniform_block_uses) {
                if (use.block_index >= artifact.uniform_blocks.len()) continue;
                const auto& block = artifact.uniform_blocks[use.block_index];
                if (block.set != set.set) continue;
                auto prepared = context.resources->Resolve(use.use);
                if (prepared.is_none()) return;
                auto& allocation = (**prepared).buffer.physical->buffer;
                buffers.push(resource_registry::DescriptorBufferBinding {
                    .binding = block.binding.to_primitive(),
                    .buffer  = allocation.buffer(),
                    .offset  = allocation.offset(),
                    .size    = allocation.size(),
                });
            }
            if (images.is_empty() && buffers.is_empty()) continue;
            auto pipeline_layout = context.pipeline_layouts->Resolve(*m_desc.pipeline_use);
            if (pipeline_layout.is_none()) return;
            auto descriptor = context.graphics->PrepareDescriptor(
                device,
                *pipeline_layout,
                set.set,
                images.as_slice(),
                buffers.as_slice(),
                resource_registry::DescriptorBindingReuse::Exclusive);
            if (descriptor.is_err()) {
                auto error = rstd::move(descriptor).unwrap_err_unchecked();
                rstd_error("prepare descriptor binding failed: {}", error.message);
                return;
            }
            m_desc.descriptor_bindings.push(DescriptorSetUse {
                .set     = set.set,
                .binding = rstd::move(descriptor).unwrap_unchecked(),
            });
        }
    }

    {
        if (out_force_clear || m_desc.transparent_clear) {
            // Some offscreen RTs need a transparent reset, not the scene's
            // opaque clear color.
            m_desc.clear_value =
                VkClearValue { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } } };
            m_desc.clear_value_src = None();
        } else {
            auto sc            = scene.ClearColor();
            m_desc.clear_value = VkClearValue {
                .color = { .float32 = { sc[usize()], sc[usize(1)], sc[usize(2)], 1.0f } }
            };
            m_desc.clear_value_src = Some(scene.ClearColor());
        }
    }
    setPrepared();
}

bool CustomShaderPass::supportsRenderScope() const { return prepared(); }

bool CustomShaderPass::canJoinRenderScopeAfter(const VulkanPass& previous) const {
    const auto* prev_pass = dynamic_cast<const CustomShaderPass*>(&previous);
    if (prev_pass == nullptr) return false;
    if (! prepared() || ! prev_pass->prepared()) return false;
    if (m_desc.clear_output || m_desc.clear_depth) return false;
    if (m_desc.depth_only != prev_pass->m_desc.depth_only) return false;
    if (! m_desc.depth_only && m_desc.color_load_op != VK_ATTACHMENT_LOAD_OP_LOAD) return false;
    if (m_desc.has_depth_attachment && m_desc.depth_load_op != VK_ATTACHMENT_LOAD_OP_LOAD)
        return false;

    const auto& prev = prev_pass->m_desc;
    if (m_desc.output != prev.output) return false;
    if (m_desc.samples != prev.samples) return false;
    if (m_desc.has_depth_attachment != prev.has_depth_attachment) return false;
    if (m_desc.output_extent.width != prev.output_extent.width ||
        m_desc.output_extent.height != prev.output_extent.height) {
        return false;
    }
    return true;
}

bool CustomShaderPass::update(PassUpdateContext& context) {
    if (m_desc.clear_value_src) {
        const auto& sc                      = *m_desc.clear_value_src;
        m_desc.clear_value.color.float32[0] = sc[usize()];
        m_desc.clear_value.color.float32[1] = sc[usize(1)];
        m_desc.clear_value.color.float32[2] = sc[usize(2)];
        m_desc.clear_value.color.float32[3] = 1.0f;
    }

    if (m_desc.draw_buffers.dynamic) {
        if (m_desc.node.is_none() || (*m_desc.node)->Mesh() == nullptr) return false;
        DrawBufferRequest request {
            .render_item   = m_desc.render_item,
            .mesh          = (*m_desc.node)->Mesh(),
            .submesh_index = m_desc.submesh_index,
            .buffer_uses   = m_desc.buffer_uses.as_slice(),
        };
        if (! RenderBufferResolver::updateDynamicDrawBuffers(
                request, m_desc.draw_buffers, context.buffers)) {
            return false;
        }
    }

    if (! m_desc.descriptor_bindings.is_empty()) {
        bool changed = m_desc.descriptor_image_slots.len().to_primitive() !=
                       m_desc.texture_bindings.len().to_primitive();
        if (changed) {
            m_desc.descriptor_image_slots.clear();
            m_desc.descriptor_image_slots.resize(m_desc.texture_bindings.len(), 0);
        }

        for (rstd::size_t index = 0; index < m_desc.texture_bindings.len().to_primitive();
             ++index) {
            const auto& binding = m_desc.texture_bindings[usize(index)];
            if (binding.empty() || binding.use.is_none()) continue;
            auto prepared = context.resources->Resolve(*binding.use);
            if (prepared.is_none() || (**prepared).image.slots.is_empty()) return false;

            auto slot  = rstd::ptrdiff_t {};
            auto frame = context.textures->TextureFrame(m_desc.draw_item, usize(index));
            if (frame.is_some() && frame->image_slot < (**prepared).image.slots.len()) {
                slot = static_cast<rstd::ptrdiff_t>(frame->image_slot.to_primitive());
            }
            changed |= m_desc.descriptor_image_slots[usize(index)] != slot;
            m_desc.descriptor_image_slots[usize(index)] = slot;
        }

        if (changed) {
            for (const auto& descriptor : m_desc.descriptor_bindings) {
                auto images = Vec<resource_registry::DescriptorImageBinding>::make();
                for (rstd::size_t index = 0; index < m_desc.texture_bindings.len().to_primitive();
                     ++index) {
                    const auto& binding = m_desc.texture_bindings[usize(index)];
                    if (binding.empty() || binding.use.is_none() ||
                        m_desc.vk_tex_set[usize(index)] != descriptor.set) {
                        continue;
                    }
                    auto prepared = context.resources->Resolve(*binding.use);
                    if (prepared.is_none() || (**prepared).image.slots.is_empty()) return false;
                    const auto vk_binding = m_desc.vk_tex_binding[usize(index)];
                    if (vk_binding < 0) continue;
                    images.push(resource_registry::DescriptorImageBinding {
                        .binding = static_cast<rstd::uint32_t>(vk_binding),
                        .image =
                            (**prepared)
                                .image.slots[usize(m_desc.descriptor_image_slots[usize(index)])],
                        .layout = SampledLayout(binding),
                    });
                }
                auto updated =
                    context.graphics->UpdateDescriptorImages(descriptor.binding, images.as_slice());
                if (updated.is_err()) {
                    auto error = rstd::move(updated).unwrap_err_unchecked();
                    rstd_error("update descriptor images failed: {}", error.message.as_str());
                    return false;
                }
            }
        }
    }

    return true;
}

void CustomShaderPass::completeUpdate() {
    if (! m_desc.draw_buffers.dynamic || m_desc.node.is_none()) return;
    auto* mesh = (*m_desc.node)->Mesh();
    if (mesh == nullptr) return;
    (void)mesh->ConsumeDirtyFlags(SceneMeshDirtyData);
}

void CustomShaderPass::prepareRenderScopeDraw(PassRecordContext& context) {
    recordSampledImageBarriers(context);
}

void CustomShaderPass::recordSampledImageBarriers(PassRecordContext& context) {
    m_desc.sampled_barriers.Record(*context.command);
}

void CustomShaderPass::beginRenderScope(PassRecordContext& context) {
    if (m_desc.render_pass_use.is_none() || m_desc.framebuffer_use.is_none()) return;
    auto render_pass = context.resources->Resolve(*m_desc.render_pass_use);
    auto framebuffer = context.resources->Resolve(*m_desc.framebuffer_use);
    if (render_pass.is_none() || framebuffer.is_none()) return;
    auto&                cmd      = *context.command;
    auto&                outext   = m_desc.output_extent;
    const bool           has_msaa = m_desc.samples != VK_SAMPLE_COUNT_1_BIT;
    const rstd::uint32_t clear_count =
        (m_desc.depth_only ? 0u : (has_msaa ? 2u : 1u)) + (m_desc.has_depth_attachment ? 1u : 0u);
    rstd::array<VkClearValue, 3> clears {};
    if (! m_desc.depth_only) clears[usize()] = m_desc.clear_value;
    if (m_desc.has_depth_attachment) {
        const rstd::uint32_t depth_index        = m_desc.depth_only ? 0u : (has_msaa ? 2u : 1u);
        clears[usize(depth_index)].depthStencil = { m_desc.depth_clear_value, 0 };
    }
    VkRenderPassBeginInfo pass_begin_info {
        .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext       = nullptr,
        .renderPass  = **(**render_pass).physical,
        .framebuffer = **(**framebuffer).physical,
        .renderArea =
            VkRect2D {
                .offset = { 0, 0 },
                .extent = { outext.width, outext.height },
            },
        .clearValueCount = clear_count,
        .pClearValues    = clears.data(),
    };
    cmd.BeginRenderPass(pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
}

void CustomShaderPass::recordRenderScopeDraw(PassRecordContext& context) {
    if (m_desc.depth_only && m_desc.node.is_some()) {
        for (auto* node = m_desc.node->as_raw_ptr(); node != nullptr; node = node->Parent()) {
            if (! node->Visible()) return;
        }
    }
    if (m_desc.pipeline_use.is_none()) return;
    auto pipeline = context.resources->Resolve(*m_desc.pipeline_use);
    if (pipeline.is_none()) return;
    auto& cmd    = *context.command;
    auto& outext = m_desc.output_extent;
    auto& layout = *(**pipeline).physical->layout;
    context.descriptor_state->UsePipeline(
        layout.handle, layout.descriptor_layouts.as_slice(), layout.push_constant_identity);
    if (context.global_descriptor.is_some()) {
        (*context.global_descriptor)
            ->Record(cmd, (**pipeline).physical->pipeline.layout, *context.descriptor_state);
    }
    for (const auto& use : m_desc.descriptor_bindings) {
        auto descriptor = context.resources->Resolve(use.binding);
        if (descriptor.is_none()) return;
        (**descriptor)
            .Record(cmd, (**pipeline).physical->pipeline.layout, *context.descriptor_state);
    }

    cmd.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *(**pipeline).physical->pipeline.handle);
    VkViewport viewport {
        .x        = 0,
        .y        = (float)outext.height,
        .width    = (float)outext.width,
        .height   = -(float)outext.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor { { 0, 0 }, { outext.width, outext.height } };

    if (m_desc.viewports.is_empty()) {
        cmd.SetViewport(0, viewport);
    } else {
        cmd.SetViewport(0, m_desc.viewports.as_slice());
    }
    if (m_desc.scissors.is_empty()) {
        cmd.SetScissor(0, scissor);
    } else {
        cmd.SetScissor(0, m_desc.scissors.as_slice());
    }

    auto& draw_buffers = m_desc.draw_buffers;
    for (usize i {}; i < draw_buffers.vertices.len(); i++) {
        auto prepared = context.resources->Resolve(draw_buffers.vertices[i]);
        if (prepared.is_none()) return;
        auto&        mref = (**prepared).buffer.physical->buffer;
        VkBuffer     vb   = mref.buffer();
        VkDeviceSize off  = mref.offset();
        cmd.BindVertexBuffers(static_cast<rstd::uint32_t>(i.to_primitive()), 1, &vb, &off);
    }
    if (draw_buffers.index.is_some()) {
        auto prepared = context.resources->Resolve(*draw_buffers.index);
        if (prepared.is_none()) return;
        auto&        mref = (**prepared).buffer.physical->buffer;
        VkBuffer     ib   = mref.buffer();
        VkDeviceSize off  = mref.offset();
        cmd.BindIndexBuffer(ib, off, VK_INDEX_TYPE_UINT32);
    }

    const bool has_index = draw_buffers.hasIndex();
    if (has_index) {
        const auto&                            submeshes = (*m_desc.node)->Mesh()->Submeshes();
        static const Vec<SceneMesh::DrawRange> kEmpty;
        const rstd::size_t                     submesh_index = m_desc.submesh_index.to_primitive();
        const auto& ranges = (submesh_index < submeshes.len().to_primitive())
                                 ? submeshes[usize(submesh_index)].draw_ranges
                                 : kEmpty;
        if (ranges.is_empty()) {
            cmd.DrawIndexed(draw_buffers.draw_count.to_primitive(),
                            m_desc.instance_count.to_primitive(),
                            0,
                            0,
                            0);
        } else {
            auto draw_range = [&](const SceneMesh::DrawRange& r) {
                cmd.DrawIndexed(r.index_count.to_primitive(),
                                m_desc.instance_count.to_primitive(),
                                r.first_index.to_primitive(),
                                0,
                                0);
            };
            const auto& source = submeshes[usize(submesh_index)].draw_range_order;
            if (source.is_some()) {
                const auto order = (*source)->operator()();
                for (auto index : order) {
                    if (index < ranges.len()) draw_range(ranges[index]);
                }
            } else {
                for (const auto& range : ranges) draw_range(range);
            }
        }
    } else {
        cmd.Draw(
            draw_buffers.draw_count.to_primitive(), m_desc.instance_count.to_primitive(), 0, 0);
    }
}

void CustomShaderPass::endRenderScope(PassRecordContext& context) {
    context.command->EndRenderPass();
}

void CustomShaderPass::record(PassRecordContext& context) {
    prepareRenderScopeDraw(context);
    beginRenderScope(context);
    recordRenderScopeDraw(context);
    endRenderScope(context);
}

void CustomShaderPass::destory(const Device&) {
    m_desc.descriptor_bindings.clear();
    m_desc.pipeline_cache_key               = None();
    m_desc.render_pass_cache_key            = None();
    m_desc.framebuffer_cache_key            = None();
    m_desc.pipeline_cache_hit               = false;
    m_desc.pipeline_cache_observed_count    = u64();
    m_desc.render_pass_cache_hit            = false;
    m_desc.render_pass_cache_observed_count = u64();
    m_desc.framebuffer_cache_hit            = false;
    m_desc.framebuffer_cache_observed_count = u64();
    m_desc.draw_buffers                     = {};
}

bool CustomShaderPass::setTextureBinding(u32 index, TextureBindingRequest binding) {
    const rstd::size_t native_index = index.to_primitive();
    if (native_index >= m_desc.texture_bindings.len().to_primitive()) return false;
    m_desc.texture_bindings[usize(native_index)] = rstd::move(binding);
    return true;
}
