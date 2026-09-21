export module wescene.vulkan_render:buffer_resolver;
export import vrento.draw_buffer;
import wescene.scene;
import wescene.resource_registry;
import rstd;

using namespace rstd::prelude;

export namespace owe::vulkan
{
using vrento::BuildDrawBufferResourceName;
using vrento::DrawBufferKey;
using vrento::DrawBufferRefs;
using vrento::DrawBufferRole;

struct DrawBufferRequest {
    RenderItemId                     render_item;
    SceneMesh*                       mesh { nullptr };
    u32                              submesh_index {};
    u64                              dynamic_allocation_generation {};
    slice<resource::BufferUseHandle> buffer_uses;
};

auto BuildDrawBufferKeys(const DrawBufferRequest&, u64 allocation_generation = u64())
    -> Vec<DrawBufferKey>;

class RenderBufferResolver {
public:
    explicit RenderBufferResolver(const resource_registry::PreparedResourceTable& resources)
        : m_resolver(resources) {}
    auto        prepareDrawBuffers(const DrawBufferRequest&) -> Option<DrawBufferRefs>;
    static bool updateDynamicDrawBuffers(const DrawBufferRequest&, DrawBufferRefs&,
                                         mut_ref<dyn<resource::BufferContentWriter>>);

private:
    vrento::RenderBufferResolver m_resolver;
};
} // namespace owe::vulkan
