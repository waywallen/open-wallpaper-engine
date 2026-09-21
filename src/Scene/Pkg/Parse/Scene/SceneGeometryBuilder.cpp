module;

module wescene.pkg.parse;
import :scene_context;
import eigen;
import wescene.pkg.spec_names;
import rstd;
import wescene.scene;

using namespace rstd::prelude;
using namespace owe;
using namespace Eigen;

namespace owe
{

using DirectDrawQuad = array<array<float, 2>, 4>;

void GenCardMesh(SceneMesh& mesh, const array<float, 2> size, const array<float, 2> mapRate,
                 const Vector3f& position_offset) {
    float left   = -(size[usize()] / 2.0f) + position_offset.x();
    float right  = size[usize()] / 2.0f + position_offset.x();
    float bottom = -(size[usize(1)] / 2.0f) + position_offset.y();
    float top    = size[usize(1)] / 2.0f + position_offset.y();
    float z      = 0.0f;

    float tw = mapRate[usize()], th = mapRate[usize(1)];

    // clang-format off
	const rstd::array<float, 12> pos = {
		left,  top, z,
		left, bottom, z,
		right,  top, z,
		right, bottom, z,
	};
	const rstd::array<float, 8> texCoord = {
		0.0f, 0.0f,
		0.0f, th,
		tw, 0.0f,
		tw, th,
	};
    // clang-format on

    SceneVertexArray vertex(MakeAttrSet({ VAttr::Position, VAttr::TexCoord }), usize(4));
    vertex.SetVertex(WE_IN_POSITION, pos.as_slice());
    vertex.SetVertex(WE_IN_TEXCOORD, texCoord.as_slice());
    mesh.AddVertexArray(rstd::move(vertex));
}

auto ReadDirectDrawQuad(const wpscene::Material& material) -> Option<DirectDrawQuad> {
    constexpr array<ref<str>, 4> names { "point0"_str, "point1"_str, "point2"_str, "point3"_str };
    DirectDrawQuad               points {};
    for (usize index {}; index < points.len(); ++index) {
        auto value = material.constantshadervalues.get(names[index]);
        if (value.is_none() || (*value)->len() != usize(2) ||
            ! f32((**value)[usize()]).is_finite() || ! f32((**value)[usize(1)]).is_finite()) {
            return None();
        }
        points[index] = { (**value)[usize()], (**value)[usize(1)] };
    }
    return Some(points);
}

void GenDirectDrawQuadMesh(SceneMesh& mesh, float edge, const DirectDrawQuad& points) {
    const auto position = [&](usize index) {
        return array<float, 3> { (points[index][usize()] - 0.5f) * edge,
                                 (0.5f - points[index][usize(1)]) * edge,
                                 0.0f };
    };
    const auto             p0 = position(usize());
    const auto             p1 = position(usize(1));
    const auto             p2 = position(usize(2));
    const auto             p3 = position(usize(3));
    const array<float, 12> positions {
        p0[usize()], p0[usize(1)], p0[usize(2)], p1[usize()], p1[usize(1)], p1[usize(2)],
        p2[usize()], p2[usize(1)], p2[usize(2)], p3[usize()], p3[usize(1)], p3[usize(2)],
    };
    const array<float, 8> tex_coords {
        points[usize()][usize()],   points[usize()][usize(1)],  points[usize(1)][usize()],
        points[usize(1)][usize(1)], points[usize(2)][usize()],  points[usize(2)][usize(1)],
        points[usize(3)][usize()],  points[usize(3)][usize(1)],
    };
    const array<rstd::uint32_t, 6> indices { 0u, 2u, 1u, 0u, 3u, 2u };

    SceneVertexArray vertex(MakeAttrSet({ VAttr::Position, VAttr::TexCoord }), usize(4));
    vertex.SetVertex(WE_IN_POSITION, positions.as_slice());
    vertex.SetVertex(WE_IN_TEXCOORD, tex_coords.as_slice());
    mesh.AddVertexArray(rstd::move(vertex));
    mesh.AddIndexArray(SceneIndexArray(indices.as_slice()));
}

void SetParticleMesh(SceneMesh& mesh, u32 count, bool thick_format,
                     bool geometry_shader_supported) {
    if (geometry_shader_supported) {
        auto specs =
            thick_format
                ? MakeAttrSet(
                      { VAttr::Position, VAttr::TexCoordVec4, VAttr::Color, VAttr::TexCoordVec4C1 })
                : MakeAttrSet({ VAttr::Position, VAttr::TexCoordVec4, VAttr::Color });
        mesh.SetPrimitive(MeshPrimitive::POINT);
        mesh.AddVertexArray(SceneVertexArray(rstd::move(specs), rstd::as_cast<usize>(count)));
        mesh.GetVertexArray(usize(0)).SetOption(WE_CB_THICK_FORMAT, thick_format);
        mesh.GetVertexArray(usize(0)).SetOption(WE_CB_GS_ENABLED, true);
        return;
    }
    // Geometry-less path (WE's native GS_ENABLED=0 shader variant): the CPU
    // expands each particle into a 4-vertex quad (per-corner TexCoordVec4 +
    // a_TexCoordC2), 6 indices per particle — the macOS replacement for the
    // geometry-shader billboard expansion (no Metal driver supports it).
    auto specs =
        thick_format
            ? MakeAttrSet({ VAttr::Position,
                            VAttr::TexCoordVec4,
                            VAttr::TexCoordC2,
                            VAttr::Color,
                            VAttr::TexCoordVec4C1 })
            : MakeAttrSet(
                  { VAttr::Position, VAttr::TexCoordVec4, VAttr::TexCoordC2, VAttr::Color });
    mesh.SetPrimitive(MeshPrimitive::TRIANGLE);
    mesh.AddVertexArray(
        SceneVertexArray(rstd::move(specs), rstd::as_cast<usize>(count) * usize(4)));
    auto& vertices = mesh.GetVertexArray(usize(0));
    vertices.SetOption(WE_CB_THICK_FORMAT, thick_format);
    vertices.SetOption(WE_CB_GS_ENABLED, false);
    const auto particle_count = rstd::as_cast<usize>(count);
    auto       indices        = Vec<rstd::uint32_t>::with_capacity(particle_count * usize(6));
    for (usize particle {}; particle < particle_count; ++particle) {
        const auto base = rstd::as_cast<u32>(particle * usize(4)).to_primitive();
        // 0 1 3 / 1 2 3 — matches the old port's quad triangulation.
        const array<rstd::uint32_t, 6> quad { base,      base + 1u, base + 3u,
                                              base + 1u, base + 2u, base + 3u };
        indices.extend_from_slice(quad.as_slice());
    }
    mesh.AddIndexArray(SceneIndexArray(indices.as_slice()));
}

void SetRopeParticleMesh(SceneMesh& mesh, const wpscene::Particle& particle, u32 count,
                         bool thick_format, bool trail_renderer, bool geometry_shader_supported) {
    (void)particle;
    if (geometry_shader_supported) {
        auto specs = thick_format ? MakeAttrSet({ VAttr::PositionVec4,
                                                  VAttr::TexCoordVec4,
                                                  VAttr::TexCoordVec4C1,
                                                  VAttr::TexCoordVec4C2,
                                                  VAttr::TexCoordVec4C3,
                                                  VAttr::Color })
                                  : MakeAttrSet({ VAttr::PositionVec4,
                                                  VAttr::TexCoordVec4,
                                                  VAttr::TexCoordVec4C1,
                                                  VAttr::TexCoordVec3C2,
                                                  VAttr::Color });
        mesh.SetPrimitive(MeshPrimitive::POINT);
        mesh.AddVertexArray(SceneVertexArray(rstd::move(specs), rstd::as_cast<usize>(count)));
        mesh.GetVertexArray(usize(0)).SetOption(
            trail_renderer ? WE_PRENDER_ROPE_TRAIL : WE_PRENDER_ROPE, true);
        mesh.GetVertexArray(usize(0)).SetOption(WE_CB_THICK_FORMAT, thick_format);
        mesh.GetVertexArray(usize(0)).SetOption(WE_CB_GS_ENABLED, true);
        return;
    }
    // Geometry-less rope: 4 vertices per trail point (per-corner UVs in
    // a_TexCoordC3/C4), 6 indices per segment quad — old-port CPU expansion.
    auto specs = thick_format ? MakeAttrSet({ VAttr::PositionVec4,
                                              VAttr::TexCoordVec4,
                                              VAttr::TexCoordVec4C1,
                                              VAttr::TexCoordVec4C2,
                                              VAttr::TexCoordVec4C3,
                                              VAttr::TexCoordC4,
                                              VAttr::Color })
                              : MakeAttrSet({ VAttr::PositionVec4,
                                              VAttr::TexCoordVec4,
                                              VAttr::TexCoordVec4C1,
                                              VAttr::TexCoordVec3C2,
                                              VAttr::TexCoordC3,
                                              VAttr::Color });
    mesh.SetPrimitive(MeshPrimitive::TRIANGLE);
    mesh.AddVertexArray(
        SceneVertexArray(rstd::move(specs), rstd::as_cast<usize>(count) * usize(4)));
    auto& vertices = mesh.GetVertexArray(usize(0));
    vertices.SetOption(trail_renderer ? WE_PRENDER_ROPE_TRAIL : WE_PRENDER_ROPE, true);
    vertices.SetOption(WE_CB_THICK_FORMAT, thick_format);
    vertices.SetOption(WE_CB_GS_ENABLED, false);
    const auto point_count = rstd::as_cast<usize>(count);
    auto       indices     = Vec<rstd::uint32_t>::with_capacity(point_count * usize(6));
    for (usize point {}; point < point_count; ++point) {
        const auto                     base = rstd::as_cast<u32>(point * usize(4)).to_primitive();
        const array<rstd::uint32_t, 6> quad { base,      base + 1u, base + 3u,
                                              base + 1u, base + 2u, base + 3u };
        indices.extend_from_slice(quad.as_slice());
    }
    mesh.AddIndexArray(SceneIndexArray(indices.as_slice()));
}

} // namespace owe
