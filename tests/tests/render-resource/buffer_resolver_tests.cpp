#include <rstd/test/gtest.hpp>

import rstd.cppstd;
import wescene.types;
import wescene.scene;

using namespace rstd::prelude;
import wescene.vulkan_render;

using namespace rstd::literals;

namespace
{

class BufferWriter {
public:
    auto UpdateBuffer(owe::resource::BufferUseHandle, rstd::slice<rstd::u8>)
        -> rstd::Result<rstd::empty, owe::resource::ResourceError> {
        ++update_count;
        if (update_count == fail_on) return rstd::Err(owe::resource::ResourceError {});
        return rstd::Ok(rstd::empty {});
    }

    rstd::u32 update_count {};
    rstd::u32 fail_on {};
};

owe::SceneMesh::Submesh MakeSubmesh() {
    rstd::initializer_list<owe::SceneVertexArray::SceneVertexAttribute> attrs {
        { .name = "a_Position"_Str, .type = owe::VertexType::FLOAT3 },
    };

    owe::SceneVertexArray vertices(attrs, rstd::usize(2));
    std::array<float, 6>  positions { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
    (void)vertices.SetVertex(
        "a_Position"_str,
        rstd::slice<float>::from_raw_parts(positions.data(), rstd::usize(positions.size())));

    owe::SceneIndexArray    indices(rstd::usize(3));
    std::array<uint32_t, 3> tri { 0, 1, 2 };
    indices.Assign(
        rstd::usize(),
        rstd::slice<rstd::uint32_t>::from_raw_parts(tri.data(), rstd::usize(tri.size())));

    owe::SceneMesh::Submesh submesh;
    submesh.vertex_arrays.push(std::move(vertices));
    submesh.index_arrays.push(std::move(indices));
    return submesh;
}

} // namespace

namespace
{
auto MakeBuffers(owe::SceneMesh& mesh, owe::RenderItemId render_item)
    -> owe::vulkan::DrawBufferRefs {
    owe::vulkan::DrawBufferRefs buffers;
    buffers.render_item       = render_item;
    buffers.dynamic           = true;
    buffers.content_confirmed = true;
    auto keys = owe::vulkan::BuildDrawBufferKeys({ .render_item = render_item, .mesh = &mesh });
    buffers.vertex_keys.push(rstd::move(keys[rstd::usize()]));
    buffers.index_key = rstd::Some(rstd::move(keys[rstd::usize(1)]));
    buffers.vertices.push(
        owe::resource::BufferUseHandle { .index = rstd::u64(1), .generation = rstd::u64(1) });
    buffers.index = rstd::Some(
        owe::resource::BufferUseHandle { .index = rstd::u64(2), .generation = rstd::u64(1) });
    return buffers;
}
} // namespace

TEST(DynamicDrawBuffer, UploadsUnconfirmedContentBeforeSkippingUnchangedGeometry) {
    owe::SceneMesh mesh(true);
    mesh.Submeshes().push(MakeSubmesh());
    owe::RenderItemId render_item { .index = rstd::u32(1), .generation = rstd::u64(2) };
    auto              buffers = MakeBuffers(mesh, render_item);
    buffers.content_confirmed = false;
    owe::vulkan::DrawBufferRequest request { .render_item = render_item, .mesh = &mesh };
    BufferWriter                   writer;
    auto sink = rstd::dyn<owe::resource::BufferContentWriter>::from_ref(writer);
    EXPECT_TRUE(owe::vulkan::RenderBufferResolver::updateDynamicDrawBuffers(
        request, buffers, sink.as_mut_ref()));
    EXPECT_EQ(writer.update_count, rstd::u32(2));
    EXPECT_TRUE(buffers.content_confirmed);
    EXPECT_TRUE(owe::vulkan::RenderBufferResolver::updateDynamicDrawBuffers(
        request, buffers, sink.as_mut_ref()));
    EXPECT_EQ(writer.update_count, rstd::u32(2));
}

TEST(DynamicDrawBuffer, RetriesUploadFailureWithoutInvalidatingLayout) {
    owe::SceneMesh mesh(true);
    mesh.Submeshes().push(MakeSubmesh());
    owe::RenderItemId render_item { .index = rstd::u32(1), .generation = rstd::u64(2) };
    auto              buffers  = MakeBuffers(mesh, render_item);
    auto              original = buffers.vertex_keys[rstd::usize()].data_generation;
    auto&             submesh  = mesh.Submeshes()[usize(0)];
    submesh.vertex_arrays[usize(0)].ResetSize();
    const rstd::uint32_t changed[] { 1, 0, 1 };
    submesh.index_arrays[usize(0)].Assign(
        rstd::usize(), rstd::slice<rstd::uint32_t>::from_raw_parts(changed, rstd::usize(3)));
    mesh.SetDirty();
    owe::vulkan::DrawBufferRequest request { .render_item = render_item, .mesh = &mesh };
    BufferWriter                   writer;
    writer.fail_on = rstd::u32(2);
    auto sink      = rstd::dyn<owe::resource::BufferContentWriter>::from_ref(writer);
    EXPECT_FALSE(owe::vulkan::RenderBufferResolver::updateDynamicDrawBuffers(
        request, buffers, sink.as_mut_ref()));
    EXPECT_EQ(buffers.vertex_keys[rstd::usize()].data_generation, original);
    EXPECT_EQ(mesh.DirtyFlags() & owe::SceneMeshDirtyLayout, owe::SceneMeshDirtyNone);
    writer.fail_on = rstd::u32();
    EXPECT_TRUE(owe::vulkan::RenderBufferResolver::updateDynamicDrawBuffers(
        request, buffers, sink.as_mut_ref()));
    EXPECT_EQ(writer.update_count, rstd::u32(4));
    EXPECT_TRUE(owe::vulkan::RenderBufferResolver::updateDynamicDrawBuffers(
        request, buffers, sink.as_mut_ref()));
    EXPECT_EQ(writer.update_count, rstd::u32(4));
}

TEST(DynamicDrawBuffer, DetectsSharedDataChangesWithoutInstanceDirtyFlag) {
    owe::SceneMesh mesh(true);
    mesh.Submeshes().push(MakeSubmesh());
    auto              clone = mesh.CloneInstance();
    owe::RenderItemId render_item { .index = rstd::u32(1), .generation = rstd::u64(2) };
    auto              buffers = MakeBuffers(*clone, render_item);
    mesh.Submeshes()[usize(0)].vertex_arrays[usize(0)].ResetSize();
    mesh.SetDirty();
    EXPECT_EQ(clone->DirtyFlags(), owe::SceneMeshDirtyNone);
    owe::vulkan::DrawBufferRequest request { .render_item = render_item,
                                             .mesh        = clone.as_ptr().as_raw_ptr() };
    BufferWriter                   writer;
    auto sink = rstd::dyn<owe::resource::BufferContentWriter>::from_ref(writer);
    EXPECT_TRUE(owe::vulkan::RenderBufferResolver::updateDynamicDrawBuffers(
        request, buffers, sink.as_mut_ref()));
    EXPECT_EQ(writer.update_count, rstd::u32(1));
    mesh.Submeshes()[usize(0)].vertex_arrays[usize(0)] =
        owe::SceneVertexArray({ { "a_Position"_Str, owe::VertexType::FLOAT3 } }, rstd::usize(2));
    EXPECT_FALSE(owe::vulkan::RenderBufferResolver::updateDynamicDrawBuffers(
        request, buffers, sink.as_mut_ref()));
    EXPECT_NE(clone->DirtyFlags() & owe::SceneMeshDirtyLayout, owe::SceneMeshDirtyNone);
    mesh.Submeshes().clear();
    EXPECT_FALSE(owe::vulkan::RenderBufferResolver::updateDynamicDrawBuffers(
        request, buffers, sink.as_mut_ref()));
}

TEST(DrawBufferResourceName, UsesStableSceneDrawIdentity) {
    owe::SceneDrawItemId draw { .index = rstd::u32(7), .generation = rstd::u32(11) };

    auto vertex0 = owe::vulkan::BuildDrawBufferResourceName(
        draw, owe::vulkan::DrawBufferRole::Vertex, rstd::u32());
    auto vertex1 = owe::vulkan::BuildDrawBufferResourceName(
        draw, owe::vulkan::DrawBufferRole::Vertex, rstd::u32(1));
    auto index = owe::vulkan::BuildDrawBufferResourceName(draw, owe::vulkan::DrawBufferRole::Index);
    auto uniform =
        owe::vulkan::BuildDrawBufferResourceName(draw, owe::vulkan::DrawBufferRole::Uniform);

    EXPECT_EQ(rstd::cppstd::as_string_view(vertex0.as_str()), "draw:11:7:vertex:0");
    EXPECT_EQ(rstd::cppstd::as_string_view(vertex1.as_str()), "draw:11:7:vertex:1");
    EXPECT_EQ(rstd::cppstd::as_string_view(index.as_str()), "draw:11:7:index:0");
    EXPECT_EQ(rstd::cppstd::as_string_view(uniform.as_str()), "draw:11:7:uniform:0");
    EXPECT_TRUE(owe::vulkan::BuildDrawBufferResourceName({}, owe::vulkan::DrawBufferRole::Vertex)
                    .is_empty());
}

TEST(DrawBufferKey, BuildsStaticKeysFromRenderItemAndGeometryGeneration) {
    owe::SceneMesh mesh;
    mesh.Submeshes().push(MakeSubmesh());

    owe::RenderItemId render_item { .index = rstd::u32(7), .generation = rstd::u64(11) };
    owe::vulkan::DrawBufferRequest request { .render_item   = render_item,
                                             .mesh          = &mesh,
                                             .submesh_index = rstd::u32() };

    auto keys = owe::vulkan::BuildDrawBufferKeys(request, rstd::u64(99));
    ASSERT_EQ(keys.len(), rstd::usize(2));

    const auto& vertex = mesh.Submeshes()[usize(0)].vertex_arrays[usize(0)];
    EXPECT_EQ(keys[rstd::usize()].render_item.index, render_item.index);
    EXPECT_EQ(keys[rstd::usize()].render_item.generation, render_item.generation);
    EXPECT_EQ(keys[rstd::usize()].role, owe::vulkan::DrawBufferRole::Vertex);
    EXPECT_EQ(keys[rstd::usize()].submesh_index, rstd::u32());
    EXPECT_EQ(keys[rstd::usize()].stream_index, rstd::u32());
    EXPECT_EQ(keys[rstd::usize()].data_generation, vertex.DataGeneration());
    EXPECT_EQ(keys[rstd::usize()].allocation_generation, rstd::u64());

    const auto& index = mesh.Submeshes()[usize(0)].index_arrays[usize(0)];
    EXPECT_EQ(keys[rstd::usize(1)].role, owe::vulkan::DrawBufferRole::Index);
    EXPECT_EQ(keys[rstd::usize(1)].data_generation, index.DataGeneration());
    EXPECT_EQ(keys[rstd::usize(1)].allocation_generation, rstd::u64());
}

TEST(DrawBufferKey, KeepsDynamicAllocationGenerationSeparateFromDataGeneration) {
    owe::SceneMesh mesh(true);
    mesh.Submeshes().push(MakeSubmesh());

    owe::RenderItemId render_item { .index = rstd::u32(3), .generation = rstd::u64(5) };
    owe::vulkan::DrawBufferRequest request { .render_item   = render_item,
                                             .mesh          = &mesh,
                                             .submesh_index = rstd::u32() };

    auto keys = owe::vulkan::BuildDrawBufferKeys(request, rstd::u64(77));
    ASSERT_EQ(keys.len(), rstd::usize(2));
    EXPECT_EQ(keys[rstd::usize()].allocation_generation, rstd::u64(77));
    EXPECT_EQ(keys[rstd::usize(1)].allocation_generation, rstd::u64(77));
    EXPECT_EQ(keys[rstd::usize()].data_generation,
              mesh.Submeshes()[usize(0)].vertex_arrays[usize(0)].DataGeneration());
    EXPECT_EQ(keys[rstd::usize(1)].data_generation,
              mesh.Submeshes()[usize(0)].index_arrays[usize(0)].DataGeneration());
}

TEST(DrawBufferKey, ObservesCompletedVertexRewriteGeneration) {
    owe::SceneMesh mesh(true);
    mesh.Submeshes().push(MakeSubmesh());
    auto& vertices   = mesh.Submeshes()[usize(0)].vertex_arrays[usize(0)];
    auto  generation = vertices.DataGeneration();

    auto rewrite = vertices.RewriteVertices([](owe::SceneVertexWriter& writer) {
        auto vertex = writer.AppendZeroedVertex();
        ASSERT_TRUE(vertex.is_some());
        (*vertex)[rstd::usize()] = 3.0f;
    });
    ASSERT_FALSE(rewrite.overflowed);

    owe::vulkan::DrawBufferRequest request {
        .render_item   = { .index = rstd::u32(4), .generation = rstd::u64(6) },
        .mesh          = &mesh,
        .submesh_index = rstd::u32(),
    };
    auto keys = owe::vulkan::BuildDrawBufferKeys(request, rstd::u64(9));
    ASSERT_FALSE(keys.is_empty());
    EXPECT_EQ(vertices.DataGeneration(), generation + rstd::u64(1));
    EXPECT_EQ(keys[rstd::usize()].data_generation, vertices.DataGeneration());
    EXPECT_EQ(vertices.VertexCount(), rstd::usize(1));
}

TEST(DrawBufferKey, ReturnsEmptyForInvalidRequest) {
    owe::SceneMesh mesh;
    EXPECT_TRUE(owe::vulkan::BuildDrawBufferKeys({ .mesh = nullptr }, rstd::u64(1)).is_empty());
    EXPECT_TRUE(owe::vulkan::BuildDrawBufferKeys({ .mesh = &mesh, .submesh_index = rstd::u32(1) },
                                                 rstd::u64(1))
                    .is_empty());
}

TEST(DynamicDrawBuffer, UpdatesEveryViewBeforeDirtyDataIsConsumed) {
    owe::SceneMesh mesh(true);
    mesh.Submeshes().push(MakeSubmesh());
    mesh.Submeshes()[usize(0)].index_arrays[usize(0)].SetRenderDataCount(rstd::usize(2));
    mesh.SetDirty();

    auto make_buffers = [&mesh] {
        owe::vulkan::DrawBufferRefs buffers;
        buffers.dynamic     = true;
        buffers.render_item = { .index = rstd::u32(3), .generation = rstd::u64(5) };
        auto keys =
            owe::vulkan::BuildDrawBufferKeys({ .render_item = buffers.render_item, .mesh = &mesh });
        keys[rstd::usize()].data_generation  = rstd::u64();
        keys[rstd::usize(1)].data_generation = rstd::u64();
        buffers.vertex_keys.push(rstd::move(keys[rstd::usize()]));
        buffers.index_key = rstd::Some(rstd::move(keys[rstd::usize(1)]));
        buffers.vertices.push(
            owe::resource::BufferUseHandle { .index = rstd::u64(1), .generation = rstd::u64(1) });
        buffers.index = rstd::Some(
            owe::resource::BufferUseHandle { .index = rstd::u64(2), .generation = rstd::u64(1) });
        return buffers;
    };
    auto reflection_buffers = make_buffers();
    auto primary_buffers    = make_buffers();

    owe::vulkan::DrawBufferRequest request {
        .render_item   = { .index = rstd::u32(3), .generation = rstd::u64(5) },
        .mesh          = &mesh,
        .submesh_index = rstd::u32(),
    };
    BufferWriter writer;
    auto         writer_trait = rstd::dyn<owe::resource::BufferContentWriter>::from_ref(writer);

    EXPECT_TRUE(owe::vulkan::RenderBufferResolver::updateDynamicDrawBuffers(
        request, reflection_buffers, writer_trait.as_mut_ref()));
    EXPECT_TRUE(owe::vulkan::RenderBufferResolver::updateDynamicDrawBuffers(
        request, primary_buffers, writer_trait.as_mut_ref()));
    EXPECT_EQ(reflection_buffers.draw_count, rstd::u32(2));
    EXPECT_EQ(primary_buffers.draw_count, rstd::u32(2));
    EXPECT_EQ(writer.update_count, rstd::u32(4));
    EXPECT_NE(mesh.DirtyFlags() & owe::SceneMeshDirtyData, owe::SceneMeshDirtyNone);
}
