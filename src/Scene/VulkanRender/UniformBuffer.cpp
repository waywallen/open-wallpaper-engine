module;

#include <rstd/macro.hpp>

module wescene.vulkan_render;
import rstd;
import rstd.cppstd;
import rstd.log;
import wescene.resource;
import wescene.scene;

using namespace rstd::prelude;
using namespace rstd::literals;

namespace owe::vulkan
{

auto ProgramUniformFrameContext::TextureFrame(SceneDrawItemId draw, usize texture_index) const
    -> Option<SceneTextureFrameView> {
    return m_textures->TextureFrame(draw, texture_index);
}

auto SceneUniformBindingPrepareContext::ResolveDraw(SceneDrawItemId id) const
    -> Option<UniformPrepareDraw> {
    auto draw = m_scene->ResourceIndex().resolve(id);
    if (draw.is_none() || draw->node == nullptr || draw->material == nullptr) return None();
    auto node_id = m_scene->ResourceIndex().nodeId(*draw->node);
    if (node_id.is_none()) return None();
    return Some(UniformPrepareDraw {
        .draw_item = id,
        .node_id   = *node_id,
        .node      = ref<SceneNode>::from_raw_parts(draw->node),
        .material  = ref<SceneMaterial>::from_raw_parts(draw->material),
    });
}

auto SceneUniformBindingPrepareContext::DrawItemFor(ref<SceneNode> node, u32 submesh_index) const
    -> Option<SceneDrawItemId> {
    auto node_id = m_scene->ResourceIndex().nodeId(*node);
    if (node_id.is_none()) return None();
    return m_scene->ResourceIndex().drawItemFor(*node_id, submesh_index);
}

auto SceneUniformBindingPrepareContext::GlobalSources() const -> slice<UniformSourceAttachment> {
    return m_scene->GlobalSources();
}

auto SceneUniformBindingPrepareContext::NodeSources(SceneNodeId node) const
    -> slice<UniformSourceAttachment> {
    return m_scene->NodeSources(node);
}

auto SceneUniformBindingPrepareContext::ResolveSource(UniformSourceId source) const
    -> Option<vrento::UniformSourceOwner> {
    return m_scene->RetainUniformSource(source);
}

auto SceneUniformBindingPrepareContext::ResolveBlock(u64 identity) const
    -> Option<ref<UniformBlockDefinition>> {
    return m_scene->ResolveUniformBlock(identity);
}

namespace detail
{

class ResourceSnapshot {
public:
    ResourceSnapshot(ref<dyn<UniformBufferFrameContext>> frame, SceneDrawItemId draw,
                     slice<PreparedUniformTextureMetadata> textures)
        : m_frame(frame), m_draw(draw), m_textures(textures) {}

    auto Texture(usize index) const -> Option<UniformTextureView> {
        UniformTextureView view;
        bool               available = false;
        if (index < m_textures.len() && m_textures[index].available) {
            const auto& prepared = m_textures[index];
            view.has_extent      = true;
            view.source_extent   = prepared.source_extent;
            view.sample_extent   = prepared.sample_extent;
            view.has_mipmap      = prepared.has_mipmap;
            view.mipmap_level    = prepared.mipmap_level;
            view.revision        = prepared.revision;
            available            = true;
        }
        auto frame = m_frame->TextureFrame(m_draw, index);
        if (frame.is_some()) {
            view.has_transform = true;
            view.rotation      = frame->rotation;
            view.translation   = frame->translation;
            auto revision      = view.revision.to_primitive();
            revision ^= frame->revision.to_primitive() + 0x9e3779b97f4a7c15ULL + (revision << 6U) +
                        (revision >> 2U);
            view.revision = u64(revision);
            if (view.revision == u64()) view.revision = u64(1);
            available = true;
        }
        return available ? Some(view) : None<UniformTextureView>();
    }
    auto Viewport() const -> rstd::array<float, 2> { return m_frame->Viewport(); }
    auto TexelSize() const -> rstd::array<float, 2> {
        const auto viewport = m_frame->Viewport();
        return { viewport[usize(0)] > 0.0f ? 1.0f / viewport[usize(0)] : 0.0f,
                 viewport[usize(1)] > 0.0f ? 1.0f / viewport[usize(1)] : 0.0f };
    }

private:
    mutable ref<dyn<UniformBufferFrameContext>> m_frame;
    SceneDrawItemId                             m_draw;
    slice<PreparedUniformTextureMetadata>       m_textures;
};

class UpdateContext {
public:
    UpdateContext(ref<SceneFrame> frame, const ResourceSnapshot& resources,
                  SceneRenderViewKind render_view)
        : m_frame(frame),
          m_resources(dyn<UniformResourceView>::from_ref(resources)),
          m_render_view(render_view) {}
    UpdateContext(ref<SceneFrame> frame, ref<dyn<UniformResourceView>> resources,
                  SceneRenderViewKind render_view)
        : m_frame(frame), m_resources(resources), m_render_view(render_view) {}

    auto Frame() const -> ref<SceneFrame> { return m_frame; }
    auto Resources() const -> ref<dyn<UniformResourceView>> { return m_resources; }
    auto RenderView() const -> SceneRenderViewKind { return m_render_view; }

private:
    ref<SceneFrame>               m_frame;
    ref<dyn<UniformResourceView>> m_resources;
    SceneRenderViewKind           m_render_view;
};

class EmptyResourceView {
public:
    auto Texture(usize) const -> Option<UniformTextureView> { return None(); }
    auto Viewport() const -> rstd::array<float, 2> { return { 0.0f, 0.0f }; }
    auto TexelSize() const -> rstd::array<float, 2> { return { 0.0f, 0.0f }; }
};

} // namespace detail

} // namespace owe::vulkan

namespace owe::vulkan
{

UniformBufferBinding::UniformBufferBinding(
    SceneDrawItemId draw_item, resource::BufferUseHandle buffer, UniformBufferLayout layout,
    Vec<BoundUniformSource> sources, ShaderValues defaults, ref<SceneMaterial> material,
    Vec<PreparedUniformTextureMetadata> textures, SceneRenderViewKind render_view,
    ShaderMatrixConvention convention, ShaderMatrixAbi abi)
    : m_draw_item(draw_item),
      m_binding(buffer, rstd::move(layout), rstd::move(sources), convention, abi),
      m_defaults(rstd::move(defaults)),
      m_material(material),
      m_textures(rstd::move(textures)),
      m_render_view(render_view) {}

auto UniformBufferBinding::Update(ref<dyn<UniformBufferFrameContext>>         frame_context,
                                  mut_ref<dyn<resource::BufferContentWriter>> buffers) const
    -> Result<empty, UniformBufferUpdateError> {
    const auto& material = *m_material;
    const auto  version  = material.customShader.value_version;
    if (m_parameter_version.is_none() || *m_parameter_version != version) {
        auto parameters = Vec<vrento::UniformParameter>::make();
        for (const auto& [name, value] : m_defaults)
            parameters.push(
                vrento::UniformParameter { rstd::cppstd::as_str(name).unwrap(), value.View() });
        for (const auto& [name, value] : material.customShader.constValues)
            parameters.push(
                vrento::UniformParameter { rstd::cppstd::as_str(name).unwrap(), value.View() });
        auto set = m_binding.SetParameters(parameters.as_slice());
        if (set.is_err()) return Err(rstd::move(set).unwrap_err_unchecked());
        m_parameter_version = Some(u64(version));
    }
    detail::ResourceSnapshot resources(frame_context, m_draw_item, m_textures.as_slice());
    detail::UpdateContext    context_impl(frame_context->Frame(), resources, m_render_view);
    auto                     context = dyn<UniformUpdateContext>::from_ref(context_impl);
    return m_binding.Update(context.as_ref(), buffers);
}

auto SharedUniformBufferBinding::Update(ref<dyn<UniformBufferFrameContext>>         frame_context,
                                        mut_ref<dyn<resource::BufferContentWriter>> buffers) const
    -> Result<empty, UniformBufferUpdateError> {
    detail::EmptyResourceView resources_impl;
    auto                      resources = dyn<UniformResourceView>::from_ref(resources_impl);
    detail::UpdateContext     context_impl(
        frame_context->Frame(), resources.as_ref(), SceneRenderViewKind::Primary);
    auto context = dyn<UniformUpdateContext>::from_ref(context_impl);
    return m_binding.Update(context.as_ref(), buffers);
}

auto MakeSharedUniformBufferBinding(ref<dyn<UniformBindingPrepareContext>>      prepare,
                                    resource::BufferUseHandle                   buffer,
                                    const resource::ShaderArtifactUniformBlock& block,
                                    ShaderMatrixConvention                      matrix_convention,
                                    ShaderMatrixAbi                             matrix_abi)
    -> Result<Box<dyn<UniformBufferUpdate>>, UniformBufferUpdateError> {
    auto definition = prepare->ResolveBlock(block.identity);
    if (definition.is_none() || (**definition).scope != UniformBlockScope::Shared) {
        return Err(UniformBufferUpdateError {
            .message = String::make("shared uniform block definition is unavailable"_str),
        });
    }
    auto layout_result = CompileUniformBufferLayout(block);
    if (layout_result.is_err()) return Err(rstd::move(layout_result).unwrap_err_unchecked());
    auto layout  = rstd::move(layout_result).unwrap_unchecked();
    auto sources = Vec<BoundUniformSource>::make();
    for (const auto& attachment : (**definition).sources) {
        auto source = prepare->ResolveSource(attachment.source);
        if (source.is_none()) {
            return Err(UniformBufferUpdateError {
                .message = String::make("shared uniform source is unavailable"_str),
            });
        }
        auto prepared = vrento::PrepareUniformSource(
            layout, rstd::move(*source), attachment.priority, matrix_convention, matrix_abi);
        if (prepared.is_err()) return Err(rstd::move(prepared).unwrap_err_unchecked());
        auto value = rstd::move(prepared).unwrap_unchecked();
        if (value.is_none()) continue;
        auto bound = rstd::move(value).unwrap_unchecked();
        sources.push(rstd::move(bound));
    }
    SharedUniformBufferBinding binding(
        buffer, rstd::move(layout), rstd::move(sources), matrix_convention, matrix_abi);
    return Ok(Box<dyn<UniformBufferUpdate>>::make(rstd::move(binding)));
}

auto MakeUniformBufferBinding(ref<dyn<UniformBindingPrepareContext>> prepare,
                              SceneDrawItemId draw_item, resource::BufferUseHandle buffer,
                              const resource::ShaderArtifactUniformBlock& block,
                              Vec<PreparedUniformTextureMetadata>         textures,
                              SceneRenderViewKind                         render_view,
                              ShaderMatrixConvention matrix_convention, ShaderMatrixAbi matrix_abi,
                              Option<ref<SceneMaterial>> material_override)
    -> Result<Box<dyn<UniformBufferUpdate>>, UniformBufferUpdateError> {
    auto draw = prepare->ResolveDraw(draw_item);
    if (draw.is_none()) {
        return Err(UniformBufferUpdateError {
            .message = String::make("uniform binding scene data is unavailable"_str),
        });
    }
    auto material = material_override.unwrap_or(draw->material);
    if (! material->customShader.shader) {
        return Err(UniformBufferUpdateError {
            .message = String::make("uniform binding shader metadata is unavailable"_str),
        });
    }

    auto layout_result = CompileUniformBufferLayout(block);
    if (layout_result.is_err()) {
        return Err(rstd::move(layout_result).unwrap_err_unchecked());
    }
    auto layout = rstd::move(layout_result).unwrap_unchecked();

    auto ranked =
        vrento::RankUniformSources(prepare->GlobalSources(), prepare->NodeSources(draw->node_id));

    auto sources = Vec<BoundUniformSource>::with_capacity(ranked.len());
    rstd::collections::HashMap<usize, usize> slot_sources;
    usize                                    source_ordinal { 0 };
    for (const auto& candidate : ranked) {
        auto source = prepare->ResolveSource(candidate.source);
        if (source.is_none()) {
            return Err(UniformBufferUpdateError {
                .message = String::make("scene uniform source is unavailable"_str),
            });
        }
        auto prepared = vrento::PrepareUniformSource(
            layout, rstd::move(*source), candidate.priority, matrix_convention, matrix_abi);
        if (prepared.is_err()) return Err(rstd::move(prepared).unwrap_err_unchecked());
        auto value = rstd::move(prepared).unwrap_unchecked();
        if (value.is_none()) continue;
        auto bound = rstd::move(value).unwrap_unchecked();
        for (const auto& output : bound.outputs) {
            auto previous = slot_sources.get(output.slot_index);
            if (previous.is_none()) {
                (void)slot_sources.insert(output.slot_index, source_ordinal);
            } else if (**previous != source_ordinal) {
                rstd_warn("uniform slot {} has multiple sources {} and {}",
                          layout.slots[output.slot_index].name.as_str(),
                          **previous,
                          source_ordinal);
            }
        }
        sources.push(rstd::move(bound));
        ++source_ordinal;
    }

    const auto&          shader = *material->customShader.shader;
    UniformBufferBinding binding(draw->draw_item,
                                 buffer,
                                 rstd::move(layout),
                                 rstd::move(sources),
                                 shader.default_uniforms,
                                 material,
                                 rstd::move(textures),
                                 render_view,
                                 matrix_convention,
                                 matrix_abi);
    return Ok(Box<dyn<UniformBufferUpdate>>::make(rstd::move(binding)));
}

} // namespace owe::vulkan
