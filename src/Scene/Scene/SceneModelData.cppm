export module wescene.model_data;

import rstd;
import wescene.scene;
import wescene.types;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;
using rstd::sync::Weak;

export namespace owe
{

struct SceneModelShape {
    Vec<SceneVertexArray::SceneVertexAttribute> attributes;
    slice<float>                                vertices;
    Option<slice<rstd::uint32_t>>               indices;
    String                                      material;
    bool                                        dynamic_vertices {};
    bool                                        dynamic_indices {};
};

struct SceneModelUpdate {
    Option<slice<float>>          vertices;
    Option<slice<rstd::uint32_t>> indices;
};

class SceneModelData {
public:
    struct Shape {
        String material;
        bool   dynamic_vertices {};
        bool   dynamic_indices {};
    };

    static auto Create(Vec<SceneModelShape> shapes) -> Result<Arc<SceneModelData>, String> {
        if (shapes.is_empty()) return Err("model requires at least one shape"_Str);
        bool dynamic {};
        for (const auto& shape : shapes)
            dynamic = dynamic || shape.dynamic_vertices || shape.dynamic_indices;
        auto model = Arc<SceneModelData>::make(dynamic);
        for (auto& shape : shapes) {
            usize stride {};
            for (const auto& attr : shape.attributes)
                stride += SceneVertexArray::RealAttributeSize(attr);
            if (stride == usize() || shape.vertices.len() % stride != usize())
                return Err("vertex buffer does not match vertex format"_Str);
            const auto count = shape.vertices.len() / stride;
            if (shape.indices) {
                if (shape.indices->len() % usize(3) != usize())
                    return Err("index buffer must contain triangles"_Str);
                for (auto index : *shape.indices)
                    if (rstd::as_cast<usize>(index) >= count)
                        return Err("model index exceeds vertex count"_Str);
            } else if (count % usize(3) != usize()) {
                return Err("non-indexed model must contain triangles"_Str);
            }
            SceneMesh::Submesh submesh;
            submesh.material_slot = rstd::as_cast<u32>(model->m_shapes.len());
            SceneVertexArray vertices(rstd::move(shape.attributes), count);
            if (! vertices.SetVertexs(usize(), shape.vertices))
                return Err("cannot initialize model vertices"_Str);
            submesh.vertex_arrays.push(rstd::move(vertices));
            if (shape.indices) submesh.index_arrays.push(SceneIndexArray(*shape.indices));
            model->m_mesh->Submeshes().push(rstd::move(submesh));
            model->m_shapes.push(
                { rstd::move(shape.material), shape.dynamic_vertices, shape.dynamic_indices });
        }
        return Ok(rstd::move(model));
    }

    explicit SceneModelData(bool dynamic): m_mesh(Arc<SceneMesh>::make(dynamic)) {
        m_mesh->SetPrimitive(MeshPrimitive::TRIANGLE);
    }

    auto Shapes() const -> slice<Shape> { return m_shapes.as_slice(); }

    auto Instantiate() -> Arc<SceneMesh> {
        auto mesh = m_mesh->CloneInstance();
        m_instances.push(mesh.downgrade());
        return mesh;
    }

    auto ApplyData(slice<SceneModelUpdate> updates) -> Result<empty, String> {
        if (updates.len() != m_shapes.len())
            return Err("model update shape count must not change"_Str);
        for (usize i {}; i < updates.len(); ++i) {
            const auto& update   = updates[i];
            const auto& shape    = m_shapes[i];
            const auto& submesh  = m_mesh->Submeshes()[i];
            const auto& vertices = submesh.vertex_arrays[usize()];
            if (update.vertices &&
                (! shape.dynamic_vertices || update.vertices->len() != vertices.DataSize()))
                return Err("vertex update requires a dynamic buffer of unchanged length"_Str);
            if (update.indices) {
                if (! shape.dynamic_indices || submesh.index_arrays.is_empty() ||
                    update.indices->len() != submesh.index_arrays[usize()].DataCount())
                    return Err("index update requires a dynamic buffer of unchanged length"_Str);
                for (auto index : *update.indices)
                    if (rstd::as_cast<usize>(index) >= vertices.VertexCount())
                        return Err("model index exceeds vertex count"_Str);
            }
        }
        bool changed {};
        for (usize i {}; i < updates.len(); ++i) {
            const auto& update  = updates[i];
            auto&       submesh = m_mesh->Submeshes()[i];
            if (update.vertices) {
                (void)submesh.vertex_arrays[usize()].SetVertexs(usize(), *update.vertices);
                changed = true;
            }
            if (update.indices) {
                submesh.index_arrays[usize()].Assign(usize(), *update.indices);
                changed = true;
            }
        }
        if (changed) {
            m_instances.retain([](const Weak<SceneMesh>& instance) {
                auto mesh = instance.upgrade();
                if (! mesh) return false;
                mesh->SetDirty();
                return true;
            });
        }
        return Ok(empty {});
    }

private:
    Arc<SceneMesh>       m_mesh;
    Vec<Shape>           m_shapes;
    Vec<Weak<SceneMesh>> m_instances;
};

} // namespace owe
