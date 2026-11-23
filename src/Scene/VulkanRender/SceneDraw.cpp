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

SceneDraw::SceneDraw(Desc&& desc, UpdateSource source)
    : m_desc(rstd::move(desc)), updates_(rstd::move(source)) {
    captureGeometry();
}
SceneDraw::SceneDraw(Input&& input)
    : SceneDraw(rstd::move(input.description), rstd::move(input.updates)) {}
SceneDraw::~SceneDraw() {}

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

owe::SceneMaterial* ResolvePassMaterial(const SceneDraw::Desc&         desc,
                                        const SceneDraw::UpdateSource& source) {
    if (source.material_override) return source.material_override->as_ptr().as_raw_ptr();
    if (source.node.is_none() || ! (*source.node)->Mesh()) return nullptr;
    const auto& mesh          = *(*source.node)->Mesh();
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

void SceneDraw::captureGeometry() {
    material_    = None();
    geometry_    = None();
    preparation_ = None();
    vertex_layouts_.clear();
    auto* material = ResolvePassMaterial(m_desc, updates_);
    if (! material || ! updates_.node || ! (*updates_.node)->Mesh()) return;
    material_ = Some(
        MaterialSnapshot { material->customShader.shader.clone(), material->PipelineSnapshot() });
    auto& mesh = *(*updates_.node)->Mesh();
    geometry_  = mesh.CaptureGeometry(m_desc.submesh_index);
    if (! geometry_) return;
    primitive_          = mesh.Primitive();
    default_alpha_      = ! ((*updates_.node)->Camera().is_empty() ||
                             (*updates_.node)->Camera().starts_with("global"_str));
    const auto& submesh = mesh.Submeshes()[rstd::as_cast<usize>(m_desc.submesh_index)];
    for (const auto& vertex : submesh.vertex_arrays) {
        DrawVertexLayout layout { .stride = vertex.OneSizeOf() };
        layout.attributes.reserve(vertex.Attributes().len());
        for (const auto& attribute : vertex.Attributes())
            layout.attributes.push(
                { attribute.name.clone(),
                  vertex.AttributeOffset(attribute.name.as_str()).unwrap_or(usize()) });
        vertex_layouts_.push(rstd::move(layout));
    }
}

PassInvalidationFlags SceneDraw::finalizeResourceRequests(Scene& scene) {
    PassInvalidationFlags flags    = PassInvalidationNone;
    auto*                 material = ResolvePassMaterial(m_desc, updates_);
    for (usize index {}; index < m_desc.texture_bindings.len(); ++index) {
        auto& binding = m_desc.texture_bindings[index];
        if (material && index < material->texture_sources.len() &&
            material->texture_sources[index].kind ==
                SceneMaterialTextureSourceKind::DepthAttachment) {
            auto target  = scene.RenderTarget(material->texture_sources[index].key.as_str());
            auto request = target
                               ? scene.TargetResource(material->texture_sources[index].key.as_str(),
                                                      resource::TextureRequestKind::DepthAttachment,
                                                      u32((**target).sample_count))
                               : None<resource::TextureResourceRef>();
            if (SetTextureRequestIfChanged(binding.request,
                                           request ? Some(request->definition().clone())
                                                   : None<TextureRequest>()))
                flags |= ToPassInvalidationFlags(PassInvalidation::Resources);
            continue;
        }
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
            const auto& rt           = **target;
            force_clear_             = rt.force_clear && ! m_desc.preserve_output;
            output_alpha_            = rt.blend_alpha_write;
            output_depth_            = rt.withDepth;
            clear_color_             = scene.ClearColor();
            m_desc.depth_clear_value = rt.depth_clear_value;
            if (material)
                m_desc.depth_clear_value =
                    DepthClearValue(material->Pipeline(), m_desc.depth_clear_value);
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

            auto* material = ResolvePassMaterial(m_desc, updates_);
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

auto SceneDraw::declareResources(ResourceDeclarationContext& context)
    -> Option<vrento::vulkan::GraphicsDrawDesc> {
    m_desc.shader_use         = None();
    m_desc.buffer_uses        = {};
    m_desc.uniform_block_uses = {};
    m_desc.pipeline_use       = None();
    m_desc.render_pass_use    = None();
    m_desc.framebuffer_use    = None();
    if (! geometry_) captureGeometry();
    if (! geometry_ || ! material_ || ! material_->shader) return None();
    auto  geometry         = geometry_->layout();
    auto* material         = &*material_;
    m_desc.pipeline_use    = Some(context.ReservePipeline());
    m_desc.render_pass_use = Some(context.ReserveRenderPass());
    auto shader_request    = MakeSceneShaderRequest(**material->shader);
    auto artifact_request  = shader_request.clone();
    m_desc.shader_use = Some(context.AddShader(rstd::move(shader_request), **material->shader));

    auto artifact = context.ShaderArtifact(artifact_request);
    if (artifact.is_some()) {
        Vec<String> sampler_members;
        sampler_members.reserve(m_desc.texture_bindings.len());
        for (usize index {}; index < m_desc.texture_bindings.len(); ++index)
            sampler_members.push(
                String::make((**material->shader).SamplerMember(index.to_primitive())));
        auto prepared =
            context.PrepareDraw(**artifact, sampler_members.as_slice(), vertex_layouts_.as_slice());
        if (prepared.is_err()) return None();
        preparation_ = Some(rstd::move(prepared).unwrap());
        for (usize block_index {}; block_index < (**artifact).uniform_blocks.len(); ++block_index) {
            const auto& block = (**artifact).uniform_blocks[block_index];
            auto        size  = block.size;
            if (size != usize()) {
                auto name =
                    block.scope == resource::ShaderArtifactUniformBlock::Scope::Shared
                        ? rstd::format("shared-uniform:{}", block.identity)
                    : m_desc.draw_item.Valid() && ! updates_.material_override
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

    auto declare_buffer = [&](const vrento::GeometryBufferLayout& buffer,
                              DrawBufferRole                      role,
                              u32                                 slot,
                              resource::BufferContent             content) {
        auto name = m_desc.draw_item.Valid()
                        ? BuildDrawBufferResourceName(m_desc.draw_item, role, slot)
                        : context.ScopeResourceName(rstd::format("pass:{}:{}:buffer:{}:{}",
                                                                 m_desc.graph_pass_index,
                                                                 m_desc.submesh_index,
                                                                 unsigned(role),
                                                                 slot));
        auto use  = context.AddBuffer(
            resource::BufferRequest { .name       = rstd::move(name),
                                      .definition = { .size  = buffer.capacity,
                                                      .usage = role == DrawBufferRole::Index
                                                                   ? resource::BufferUsage::Index
                                                                   : resource::BufferUsage::Vertex,
                                                      .alignment = usize(4) },
                                      .lifetime   = geometry.dynamic
                                                        ? resource::BufferLifetimeClass::Dynamic
                                                        : resource::BufferLifetimeClass::Retained,
                                      .content_version  = buffer.data_generation,
                                      .content_identity = buffer.storage_generation },
            rstd::move(content));
        m_desc.buffer_uses.push(resource::BufferUseHandle(use));
    };
    for (usize index {}; index < geometry.vertices.len(); ++index)
        declare_buffer(geometry.vertices[index],
                       DrawBufferRole::Vertex,
                       rstd::as_cast<u32>(index),
                       geometry_->vertexContent(index));
    if (geometry.index)
        declare_buffer(
            *geometry.index, DrawBufferRole::Index, u32(), geometry_->indexContent().unwrap());
    return artifact ? describeGraphics(**artifact) : None();
}

bool SceneDraw::samplesTexture(usize index) const {
    return index < m_desc.vk_tex_binding.len() && m_desc.vk_tex_binding[index] >= 0 &&
           index < m_desc.texture_stages.len() &&
           m_desc.texture_stages[index] != vrento::texture::Stages::None;
}

auto SceneDraw::textureAccesses() const -> Vec<PassTextureAccess> {
    Vec<PassTextureAccess> uses;
    for (usize index {}; index < m_desc.texture_bindings.len(); ++index) {
        const auto& binding = m_desc.texture_bindings[index];
        if (binding.use && samplesTexture(index))
            uses.push({ *binding.use, vrento::fg::Access::Read });
    }
    if (! m_desc.depth_only) {
        if (m_desc.output_use) uses.push({ *m_desc.output_use, vrento::fg::Access::Write });
        if (m_desc.output_msaa_use)
            uses.push({ *m_desc.output_msaa_use, vrento::fg::Access::Write });
    }
    if (m_desc.depth_use) uses.push({ *m_desc.depth_use, vrento::fg::Access::Write });
    return uses;
}

PassResourceUses SceneDraw::resourceUses() const {
    PassResourceUses uses;
    // Texture metadata can supply uniforms even when the sampler is inactive.
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

auto SceneDraw::globalDescriptorBufferUses(const PreparedPassResources& resources) const
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

Option<owe::RenderItemId> SceneDraw::renderItemId() const {
    if (! m_desc.render_item.Valid()) return None();
    return Some<owe::RenderItemId>(m_desc.render_item);
}

Vec<PassTextureRequestDiagnostic> SceneDraw::textureRequestDiagnostics() const {
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
SceneDraw::refreshMaterialTextureBindings(const RenderSceneSnapshot& render_scene) {
    MaterialTextureBindingRefresh result;
    if (updates_.material_override) {
        // This pass renders a build-time clone of the material (effect chain);
        // an in-place refresh cannot see the mutated base material (e.g. a new
        // $mediaThumbnail), so rebuild the graph to regenerate the override.
        // Only reached when the caller matched this pass to a changed material.
        result.requires_graph_rebuild = true;
        return result;
    }
    if (updates_.node.is_none() || (*updates_.node)->Mesh() == nullptr) {
        return result;
    }

    auto&              mesh          = *(*updates_.node)->Mesh();
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

auto SceneDraw::createUniformBufferUpdates(ref<dyn<UniformBindingPrepareContext>> prepare,
                                           const PreparedPassResources&           resources,
                                           SceneUniformPreparationCache&          cache)
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
    if (prepare->ResolveDraw(draw_item).is_none() && updates_.node.is_some()) {
        auto node    = ref<SceneNode>::from_raw_parts((*updates_.node).as_ptr());
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
            metadata.has_mipmap   = (**prepared).request().kind == TextureRequestKind::RenderTarget;
            metadata.mipmap_level = static_cast<float>(image.mipmap_level);
            metadata.revision     = (**prepared).physical_generation ^ image.generation;
            if (metadata.revision == u64()) metadata.revision = u64(1);
        } else if (binding.request && binding.request->definition) {
            const auto& definition = *binding.request->definition;
            metadata.available     = true;
            metadata.source_extent = { static_cast<float>(definition.width.to_primitive()),
                                       static_cast<float>(definition.height.to_primitive()) };
            metadata.sample_extent = metadata.source_extent;
            metadata.has_mipmap    = binding.request->kind == TextureRequestKind::RenderTarget;
            metadata.mipmap_level  = static_cast<float>(definition.mip_levels.to_primitive());
            metadata.revision      = u64(1);
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
        auto plan = cache.prepare(prepare, draw->node_id, (**prepared).shader, use.block_index);
        if (plan.is_err()) return Err(plan.unwrap_err());
        if (block.scope == resource::ShaderArtifactUniformBlock::Scope::Shared) {
            updates.push(Box<dyn<UniformBufferUpdate>>::make(
                SharedUniformBufferBinding(use.use, plan.unwrap())));
            continue;
        }
        auto binding = BindUniformBuffer(
            *draw,
            use.use,
            plan.unwrap(),
            textures.clone(),
            m_desc.render_view,
            updates_.material_override ? Some(ref<SceneMaterial>::from_raw_parts(
                                             updates_.material_override->as_ptr().as_raw_ptr()))
                                       : None<ref<SceneMaterial>>());
        if (binding.is_err()) return Err(rstd::move(binding).unwrap_err_unchecked());
        updates.push(rstd::move(binding).unwrap_unchecked());
    }
    return Ok(rstd::move(updates));
}

auto SceneDraw::describeGraphics(const resource::ShaderArtifact& artifact)
    -> Option<vrento::vulkan::GraphicsDrawDesc> {
    if (! geometry_ || ! material_ || ! preparation_) return None();
    auto        geometry     = geometry_->layout();
    const auto& material_ref = *material_;
    const bool  has_depth_attachment =
        m_desc.depth_only || (output_depth_ && UsesDepthAttachment(material_ref.Pipeline()));
    m_desc.has_depth_attachment = has_depth_attachment;
    const auto depthLoadOp =
        has_depth_attachment
            ? (m_desc.clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD)
            : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    m_desc.depth_load_op             = depthLoadOp;
    const bool out_force_clear       = force_clear_;
    const auto out_blend_alpha_write = output_alpha_;
    auto       spvs                  = ShaderSpvsFromArtifact(artifact);
    if (spvs.is_empty()) {
        rstd_error("prepared shader artifact is empty, {}", (**material_ref.shader).name);
        return None();
    }
    const auto& prepared = **preparation_;
    m_desc.vk_tex_binding.clear();
    m_desc.vk_tex_set.clear();
    m_desc.texture_stages.clear();
    m_desc.vk_tex_binding.reserve(prepared.samplers.len());
    m_desc.vk_tex_set.reserve(prepared.samplers.len());
    m_desc.texture_stages.reserve(prepared.samplers.len());
    for (const auto& sampler : prepared.samplers) {
        m_desc.vk_tex_binding.push(sampler.binding.to_primitive());
        m_desc.vk_tex_set.push(u32(sampler.set));
        m_desc.texture_stages.push(vrento::texture::Stages(sampler.stages));
    }
    {
        VkPipelineColorBlendAttachmentState color_blend {};
        VkAttachmentLoadOp                  loadOp { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
        const auto                          blendmode = material_ref.Pipeline().blend_mode;
        {
            VkColorComponentFlags colorMask =
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
            const bool default_writes_alpha = default_alpha_;
            SetBlend(blendmode, color_blend);
            const auto& alpha_write = material_ref.Pipeline().alpha_write;
            const bool  writes_alpha =
                alpha_write.is_some()
                    ? *alpha_write
                    : (color_blend.blendEnable
                           ? (out_blend_alpha_write ? *out_blend_alpha_write : default_writes_alpha)
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
        m_desc.color_load_op        = loadOp;
        const VkFormat color_format = m_desc.output_request && m_desc.output_request->definition
                                          ? ToVkType(m_desc.output_request->definition->format)
                                          : VK_FORMAT_R8G8B8A8_UNORM;
        constexpr VkImageLayout color_final_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        GraphicsPipeline pipeline_state;
        pipeline_state.toDefault();
        pipeline_state.setSampleCount(m_desc.samples);
        SetAlphaToCoverage(blendmode, pipeline_state.multisample);
        if (has_depth_attachment) SetDepthState(material_ref.Pipeline(), pipeline_state.depth);
        SetRasterState(material_ref.Pipeline(), true, pipeline_state.raster);
        const bool          has_index = geometry.index.is_some();
        VkPrimitiveTopology topology  = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        switch (primitive_) {
        case MeshPrimitive::POINT: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case MeshPrimitive::TRIANGLE:
            topology = has_index ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                                 : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            break;
        }
        if (m_desc.pipeline_use.is_none()) return None();
        PipelineResourceRequest pipeline_request {
            .pipeline_layout = {},
            .vertex_bindings = prepared.vertex_bindings.clone(),
            .vertex_attrs    = prepared.vertex_attributes.clone(),
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
            .color_initial_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .color_final_layout   = color_final_layout,
            .color_load_op        = loadOp,
            .depth_load_op        = depthLoadOp,
            .depth_store_op       = VK_ATTACHMENT_STORE_OP_STORE,
            .depth_initial_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .depth_final_layout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .has_color_attachment = ! m_desc.depth_only,
            .has_depth_attachment = has_depth_attachment,
        };
        if (m_desc.render_pass_use.is_none()) return None();
        return Some(vrento::vulkan::GraphicsDrawDesc {
            {},
            *m_desc.pipeline_use,
            *m_desc.render_pass_use,
            rstd::move(pipeline_request),
            Some(prepared.pipelineLayout(*m_desc.pipeline_use)) });
    }
}

auto SceneDraw::resolveResources(const Device& device, DrawResourceContext& context)
    -> Result<empty, DrawPrepareError> {
    auto missing = [&](DrawResourceKind kind) {
        return Err(DrawPrepareError::Missing(kind, m_desc.render_item));
    };
    const bool out_force_clear = force_clear_;
    auto       output_use      = m_desc.depth_only ? m_desc.depth_use : m_desc.output_use;
    if (! output_use) return missing(DrawResourceKind::Output);
    auto target = context.resources->Resolve(*output_use);
    if (! target) return Err(DrawPrepareError::Texture(*output_use));
    auto extent          = (**target).image.getActive().extent;
    m_desc.output_extent = { extent.width, extent.height };
    if (! geometry_) return missing(DrawResourceKind::Geometry);
    vrento::RenderBufferResolver     buffer_resolver(*context.resources);
    auto                             layout = geometry_->layout();
    vrento::DrawBufferPrepareRequest buffer_request { .render_item   = m_desc.render_item,
                                                      .geometry      = layout,
                                                      .submesh_index = m_desc.submesh_index,
                                                      .buffer_uses =
                                                          m_desc.buffer_uses.as_slice() };
    auto draw_buffers = buffer_resolver.tryPrepareDrawBuffers(buffer_request);
    if (draw_buffers.is_err())
        return Err(DrawPrepareError::Buffers(rstd::move(draw_buffers).unwrap_err()));
    m_desc.draw_buffers = rstd::move(*draw_buffers);

    {
        if (! m_desc.shader_use) return missing(DrawResourceKind::Shader);
        if (! m_desc.pipeline_use) return missing(DrawResourceKind::Pipeline);
        auto prepared_shader = context.resources->Resolve(*m_desc.shader_use);
        if (prepared_shader.is_none()) return Err(DrawPrepareError::Shader(*m_desc.shader_use));
        const auto& artifact = (**prepared_shader).shader.physical->artifact;
        m_desc.descriptor_bindings.clear();
        m_desc.descriptor_image_slots.clear();
        m_desc.descriptor_image_slots.resize(m_desc.texture_bindings.len(), 0);
        for (const auto& set : artifact.descriptor_sets) {
            if (set.set == u32()) continue;
            Vec<resource_registry::DescriptorTextureBinding> textures;
            for (usize index {}; index < m_desc.texture_bindings.len(); ++index) {
                const auto& source  = m_desc.texture_bindings[index];
                const auto  binding = m_desc.vk_tex_binding[index];
                if (source.empty() || ! samplesTexture(index) ||
                    m_desc.vk_tex_set[index] != set.set)
                    continue;
                if (! source.use) return missing(DrawResourceKind::Texture);
                textures.push({ static_cast<rstd::uint32_t>(binding), *source.use, usize() });
            }
            auto buffers = Vec<resource_registry::DescriptorBufferBinding>::make();
            for (const auto& use : m_desc.uniform_block_uses) {
                if (use.block_index >= artifact.uniform_blocks.len()) continue;
                const auto& block = artifact.uniform_blocks[use.block_index];
                if (block.set != set.set) continue;
                auto prepared = context.resources->Resolve(use.use);
                if (prepared.is_none()) return Err(DrawPrepareError::Buffer(use.use));
                auto& allocation = (**prepared).buffer.physical->buffer;
                buffers.push(resource_registry::DescriptorBufferBinding {
                    .binding = block.binding.to_primitive(),
                    .buffer  = allocation.buffer(),
                    .offset  = allocation.offset(),
                    .size    = allocation.size(),
                });
            }
            if (textures.is_empty() && buffers.is_empty()) continue;
            auto pipeline_layout = context.pipeline_layouts->Resolve(*m_desc.pipeline_use);
            if (pipeline_layout.is_none())
                return Err(DrawPrepareError::Layout(*m_desc.pipeline_use));
            auto descriptor = context.graphics->PrepareTextureDescriptor(
                device, *pipeline_layout, set.set, textures.as_slice(), buffers.as_slice());
            if (descriptor.is_err()) {
                auto error = rstd::move(descriptor).unwrap_err_unchecked();
                return Err(DrawPrepareError::Descriptor(set.set, rstd::move(error)));
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
            auto sc            = clear_color_;
            m_desc.clear_value = VkClearValue {
                .color = { .float32 = { sc[usize()], sc[usize(1)], sc[usize(2)], 1.0f } }
            };
            m_desc.clear_value_src = Some(clear_color_);
        }
    }
    bindings_ready_ = true;
    return Ok(empty {});
}

bool SceneDraw::update(DrawUpdateContext& context) {
    if (m_desc.clear_value_src) {
        const auto& sc                      = *m_desc.clear_value_src;
        m_desc.clear_value.color.float32[0] = sc[usize()];
        m_desc.clear_value.color.float32[1] = sc[usize(1)];
        m_desc.clear_value.color.float32[2] = sc[usize(2)];
        m_desc.clear_value.color.float32[3] = 1.0f;
    }

    if (! m_desc.draw_buffers.vertices.is_empty() || m_desc.draw_buffers.index) {
        if (updates_.node.is_none() || (*updates_.node)->Mesh() == nullptr) return false;
        DrawBufferRequest request {
            .render_item   = m_desc.render_item,
            .mesh          = (*updates_.node)->Mesh(),
            .submesh_index = m_desc.submesh_index,
            .buffer_uses   = m_desc.buffer_uses.as_slice(),
        };
        if (! RenderBufferResolver::updateDrawBuffers(
                request, m_desc.draw_buffers, context.buffers)) {
            return false;
        }
    }

    if (! m_desc.descriptor_bindings.is_empty()) {
        Option<Vec<rstd::ptrdiff_t>> slots;
        const bool reset = m_desc.descriptor_image_slots.len() != m_desc.texture_bindings.len();
        if (reset) {
            slots = Some(Vec<rstd::ptrdiff_t>());
            slots->resize(m_desc.texture_bindings.len(), 0);
        }
        Vec<u32> changed_sets;
        for (usize index {}; index < m_desc.texture_bindings.len(); ++index) {
            const auto& binding = m_desc.texture_bindings[index];
            if (binding.empty() || ! binding.use || ! samplesTexture(index)) continue;
            auto prepared = context.resources->Resolve(*binding.use);
            if (! prepared || (*prepared)->image.slots.is_empty()) return false;
            rstd::ptrdiff_t slot {};
            auto            frame = context.textures->TextureFrame(m_desc.draw_item, index);
            if (frame && frame->image_slot < (*prepared)->image.slots.len())
                slot = static_cast<rstd::ptrdiff_t>(frame->image_slot.to_primitive());
            if (reset || m_desc.descriptor_image_slots[index] != slot) {
                if (! slots) slots = Some(m_desc.descriptor_image_slots.clone());
                changed_sets.push(u32(m_desc.vk_tex_set[index]));
                (*slots)[index] = slot;
            }
        }
        for (const auto& descriptor : m_desc.descriptor_bindings) {
            bool changed {};
            for (auto set : changed_sets) changed |= set == descriptor.set;
            if (! changed) continue;
            Vec<resource_registry::DescriptorTextureBinding> textures;
            for (usize index {}; index < m_desc.texture_bindings.len(); ++index) {
                const auto& binding = m_desc.texture_bindings[index];
                if (binding.empty() || ! binding.use ||
                    m_desc.vk_tex_set[index] != descriptor.set || ! samplesTexture(index))
                    continue;
                textures.push({ static_cast<rstd::uint32_t>(m_desc.vk_tex_binding[index]),
                                *binding.use,
                                usize((*slots)[index]) });
            }
            auto updated =
                context.graphics->UpdateDescriptorTextures(descriptor.binding, textures.as_slice());
            if (updated.is_err()) {
                rstd_error("update descriptor textures failed: {}", updated.unwrap_err().message);
                return false;
            }
        }
        if (slots) m_desc.descriptor_image_slots = slots.unwrap();
    }

    visible_ = true;
    if (m_desc.depth_only && updates_.node.is_some())
        for (auto* node = updates_.node->as_raw_ptr(); node != nullptr; node = node->Parent())
            visible_ &= node->Visible();
    draws_.clear();
    if (m_desc.draw_buffers.index) {
        if (updates_.node.is_none() || (*updates_.node)->Mesh() == nullptr) return false;
        const auto& submeshes     = (*updates_.node)->Mesh()->Submeshes();
        auto        submesh_index = usize(m_desc.submesh_index.to_primitive());
        if (submesh_index >= submeshes.len()) return false;
        const auto& submesh = submeshes[submesh_index];
        const auto& ranges  = submesh.draw_ranges;
        if (ranges.is_empty())
            draws_.push({ m_desc.draw_buffers.draw_count.to_primitive(), 0 });
        else if (submesh.draw_range_order) {
            auto order = (*submesh.draw_range_order)->operator()();
            for (auto index : order)
                if (index < ranges.len())
                    draws_.push({ ranges[index].index_count.to_primitive(),
                                  ranges[index].first_index.to_primitive() });
        } else
            for (auto range : ranges)
                draws_.push({ range.index_count.to_primitive(), range.first_index.to_primitive() });
    } else
        draws_.push({ m_desc.draw_buffers.draw_count.to_primitive(), 0 });

    return true;
}

void SceneDraw::acknowledgeGeometry() {
    if (updates_.node.is_none()) return;
    auto* mesh = (*updates_.node)->Mesh();
    if (mesh == nullptr) return;
    (void)mesh->ConsumeDirtyFlags(SceneMeshDirtyData);
}

auto SceneDraw::frameData(const DrawBindingContext& context) const -> Option<Arc<ShaderDrawInput>> {
    if (! bindings_ready_) return None();
    auto session     = context.session;
    auto input       = Arc<ShaderDrawInput>::make();
    input->instances = m_desc.instance_count.to_primitive();
    input->visible   = visible_;
    auto descriptor  = [&](const resource_registry::PreparedDescriptorBinding& source) {
        auto& owner = session.graphics_resources();
        auto  frozen =
            owner.descriptor_snapshots.Snapshot(session.device(), source, owner.descriptors);
        if (frozen.is_err()) return false;
        input->descriptors.push(rstd::move(frozen).unwrap());
        return true;
    };
    if (context.global_descriptor && ! descriptor(**context.global_descriptor)) return None();
    for (const auto& use : m_desc.descriptor_bindings) {
        auto prepared = context.resources->Resolve(use.binding);
        if (prepared.is_none() || ! descriptor(**prepared)) return None();
    }
    for (auto use : m_desc.draw_buffers.vertices) {
        auto prepared = context.resources->Resolve(use);
        if (prepared.is_none()) return None();
        input->vertices.push((**prepared).buffer.physical->buffer.clone());
    }
    if (m_desc.draw_buffers.index) {
        auto prepared = context.resources->Resolve(*m_desc.draw_buffers.index);
        if (prepared.is_none()) return None();
        input->indices = Some((**prepared).buffer.physical->buffer.clone());
    }
    for (auto use : resourceUses().buffers) {
        auto prepared = context.resources->Resolve(use);
        if (prepared.is_none()) return None();
        input->buffers.push((**prepared).buffer.physical->buffer.clone());
    }
    for (usize index {}; index < m_desc.texture_bindings.len(); ++index) {
        const auto& binding = m_desc.texture_bindings[index];
        if (binding.use.is_none() || ! samplesTexture(index)) continue;
        auto prepared = context.resources->Resolve(*binding.use);
        if (prepared.is_none()) return None();
        auto slot    = index < m_desc.descriptor_image_slots.len()
                           ? usize(m_desc.descriptor_image_slots[index])
                           : usize();
        auto sampled = (**prepared).physical->SampledBinding(slot);
        if (sampled.is_err()) return None();
        input->textures.push(vrento::texture::TextureViewRef(sampled->view));
        input->texture_uses.push(resource::TextureUseHandle(*binding.use));
        input->texture_slots.push(usize(slot));
        input->samplers.push(vrento::texture::SamplerRef(sampled->sampler));
        input->texture_stages.push(vrento::texture::Stages(m_desc.texture_stages[index]));
    }
    auto extent = m_desc.output_extent;
    if (m_desc.viewports.is_empty())
        input->viewports.push(
            { 0, float(extent.height), float(extent.width), -float(extent.height), 0, 1 });
    else
        input->viewports = m_desc.viewports.clone();
    if (m_desc.scissors.is_empty())
        input->scissors.push({ { 0, 0 }, extent });
    else
        input->scissors = m_desc.scissors.clone();
    input->draws = draws_.clone();
    return Some(rstd::move(input));
}

void SceneDraw::clearResources() {
    bindings_ready_ = false;
    m_desc.descriptor_bindings.clear();
    m_desc.draw_buffers = {};
}

bool SceneDraw::setTextureBinding(u32 index, TextureBindingRequest binding) {
    const rstd::size_t native_index = index.to_primitive();
    if (native_index >= m_desc.texture_bindings.len().to_primitive()) return false;
    m_desc.texture_bindings[usize(native_index)] = rstd::move(binding);
    return true;
}

auto SceneDraw::describePass(vrento::fg::FrameGraphBuilder& graph,
                             const SceneRenderTextures& textures, vrento::fg::PassId id) const
    -> Result<vrento::vulkan::GraphicsPassResources, vrento::fg::Error> {
    namespace vk = vrento::vulkan;
    vk::GraphicsPassResources resources;
    auto                      attachment =
        [&](Option<resource::TextureUseHandle> use, bool depth, bool resolve) -> bool {
        if (! use) return false;
        auto content = textures.content(graph, *use);
        if (content.is_err()) return false;
        auto load = vk::AttachmentLoad::Discard;
        if (depth)
            load = m_desc.clear_depth ? vk::AttachmentLoad::Clear : vk::AttachmentLoad::Load;
        else if (! resolve) {
            VkAttachmentLoadOp policy { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
            if (material_) SetAttachmentLoadOp(material_->Pipeline().blend_mode, policy);
            if (m_desc.preserve_output) policy = VK_ATTACHMENT_LOAD_OP_LOAD;
            if (m_desc.clear_output || force_clear_) policy = VK_ATTACHMENT_LOAD_OP_CLEAR;
            load = policy == VK_ATTACHMENT_LOAD_OP_LOAD    ? vk::AttachmentLoad::Load
                   : policy == VK_ATTACHMENT_LOAD_OP_CLEAR ? vk::AttachmentLoad::Clear
                                                           : vk::AttachmentLoad::Discard;
        }
        vk::GraphicsAttachment value { .target = content.unwrap(), .load = load };
        if (depth) {
            value.clear[usize()]        = m_desc.depth_clear_value;
            resources.attachments.depth = Some(rstd::move(value));
        } else if (resolve)
            resources.attachments.resolve = Some(rstd::move(value));
        else {
            if (force_clear_ || m_desc.transparent_clear)
                value.clear = { 0.0f, 0.0f, 0.0f, 0.0f };
            else
                value.clear = {
                    clear_color_[usize()], clear_color_[usize(1)], clear_color_[usize(2)], 1.0f
                };
            resources.attachments.color = Some(rstd::move(value));
        }
        return true;
    };
    if (! m_desc.depth_only) {
        if (! attachment(
                m_desc.output_msaa_use ? m_desc.output_msaa_use : m_desc.output_use, false, false))
            return Err(vrento::fg::Error { vrento::fg::ErrorKind::MissingResource, id });
        if (m_desc.output_msaa_use && ! attachment(m_desc.output_use, false, true))
            return Err(vrento::fg::Error { vrento::fg::ErrorKind::MissingResource, id });
    }
    if (m_desc.has_depth_attachment && ! attachment(m_desc.depth_use, true, false))
        return Err(vrento::fg::Error { vrento::fg::ErrorKind::MissingResource, id });
    for (usize index {}; index < m_desc.texture_bindings.len(); ++index) {
        const auto& binding = m_desc.texture_bindings[index];
        if (! binding.use || ! samplesTexture(index)) continue;
        auto content = textures.content(graph, *binding.use);
        if (content.is_err())
            return Err(vrento::fg::Error { vrento::fg::ErrorKind::MissingResource, id });
        auto description = textures.shape(graph, *content);
        if (! description)
            return Err(vrento::fg::Error { vrento::fg::ErrorKind::MissingResource, id });
        auto       stages  = vk::PassStages {};
        const auto sampled = vrento::texture::Count(m_desc.texture_stages[index]);
        if (sampled & vrento::texture::Count(vrento::texture::Stages::Vertex))
            stages = stages | vk::PassStages::Vertex;
        if (sampled & vrento::texture::Count(vrento::texture::Stages::Fragment))
            stages = stages | vk::PassStages::Fragment;
        if (sampled & vrento::texture::Count(vrento::texture::Stages::Geometry))
            stages = stages | vk::PassStages::Geometry;
        resources.textures.push({ content.unwrap(), vrento::fg::full_range(*description), stages });
    }
    return Ok(rstd::move(resources));
}
