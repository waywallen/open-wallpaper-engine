module wescene.vulkan_render;
import rstd;
import wescene.scene;
import vrento.draw_buffer;

using namespace rstd::prelude;

namespace owe::vulkan
{
namespace
{
auto AdaptRequest(const DrawBufferRequest& request, const vrento::GeometryView& geometry)
    -> vrento::DrawBufferRequest {
    return { .render_item                   = request.render_item,
             .geometry                      = geometry,
             .submesh_index                 = request.submesh_index,
             .dynamic_allocation_generation = request.dynamic_allocation_generation,
             .buffer_uses                   = request.buffer_uses };
}
} // namespace

auto BuildDrawBufferKeys(const DrawBufferRequest& request, u64 allocation_generation)
    -> Vec<DrawBufferKey> {
    if (! request.mesh) return {};
    auto geometry = request.mesh->BufferView(request.submesh_index);
    if (geometry.is_none()) return {};
    return vrento::BuildDrawBufferKeys(AdaptRequest(request, *geometry), allocation_generation);
}

auto RenderBufferResolver::prepareDrawBuffers(const DrawBufferRequest& request)
    -> Option<DrawBufferRefs> {
    if (! request.mesh) return None();
    auto geometry = request.mesh->BufferView(request.submesh_index);
    if (geometry.is_none()) return None();
    return m_resolver.prepareDrawBuffers(AdaptRequest(request, *geometry));
}

bool RenderBufferResolver::updateDynamicDrawBuffers(
    const DrawBufferRequest& request, DrawBufferRefs& buffers,
    mut_ref<dyn<resource::BufferContentWriter>> writer) {
    if (! request.mesh) return false;
    auto geometry = request.mesh->BufferView(request.submesh_index);
    if (geometry.is_none()) {
        request.mesh->SetLayoutDirty();
        return false;
    }
    auto updated = vrento::RenderBufferResolver::updateDynamicDrawBuffers(
        AdaptRequest(request, *geometry), buffers, writer);
    if (updated.is_ok()) return true;
    if (updated.unwrap_err_unchecked().kind == vrento::DrawBufferUpdateErrorKind::NeedsReprepare)
        request.mesh->SetLayoutDirty();
    return false;
}
} // namespace owe::vulkan
