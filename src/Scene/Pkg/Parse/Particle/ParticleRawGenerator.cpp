module;

#include <rstd/macro.hpp>

module wescene.pkg.parse;

import eigen;
import rstd;
import rstd.log;
import wescene.core;
import wescene.particle;
import wescene.particle.program;
import wescene.scene;
import wescene.pkg.spec_names;

using namespace rstd::prelude;
using namespace owe;

namespace
{

struct GOption {
    bool thick_format { false };
    bool expand_corners { false }; // GS_ENABLED=0: CPU 4 vertex expansion per particle/segment
};

struct AttrSlot {
    usize offset {};
    bool  enabled { false };
};

struct PointVertexLayout {
    AttrSlot position;
    AttrSlot texcoord;
    AttrSlot color;
    AttrSlot velocity;
    AttrSlot texcoord_c2; // No geometry shader variant: Rotate x, y
};

struct RopeVertexLayout {
    AttrSlot position;
    AttrSlot endpoint;
    AttrSlot previous_point;
    AttrSlot next_point;
    AttrSlot color_end;
    AttrSlot color;
    AttrSlot corner_uv; // Geometry-shader-free variant: segment corner UVs
};

struct ExtractParticle {
    particle::ParticleSlot             slot;
    const particle::ParticleSlotState& state;
    const Eigen::Vector3f&             position;
    const Eigen::Vector3f&             velocity;
    const Eigen::Vector3f&             rotation;
    const Eigen::Vector3f&             color;
    const float&                       alpha;
    const float&                       size;
    const float&                       lifetime;
    const float&                       random;
    const float&                       initial_lifetime;
};

struct ExtractInstance {
    slice<particle::ParticleSlotState> states;
    slice<Eigen::Vector3f>             positions;
    slice<Eigen::Vector3f>             velocities;
    slice<Eigen::Vector3f>             rotations;
    slice<Eigen::Vector3f>             colors;
    slice<float>                       alphas;
    slice<float>                       sizes;
    slice<float>                       lifetimes;
    slice<float>                       randoms;
    slice<float>                       initial_lifetimes;
    slice<particle::ParticleSlot>      slots;
    Option<ref<TrailHistoryAttribute>> trail;
    usize                              instance_index {};

    auto Particle(particle::ParticleSlot slot) const -> ExtractParticle {
        auto index = slot.index;
        return {
            .slot             = slot,
            .state            = states[index],
            .position         = positions[index],
            .velocity         = velocities[index],
            .rotation         = rotations[index],
            .color            = colors[index],
            .alpha            = alphas[index],
            .size             = sizes[index],
            .lifetime         = lifetimes[index],
            .random           = randoms[index],
            .initial_lifetime = initial_lifetimes[index],
        };
    }
};

auto FindAttrSlot(const SceneVertexArray& vertices, ref<str> name) noexcept -> AttrSlot {
    auto found = vertices.AttributeOffset(name);
    if (found.is_none()) return {};
    return {
        .offset  = *found / usize(sizeof(float)),
        .enabled = true,
    };
}

auto ResolvePointVertexLayout(const SceneVertexArray& vertices) -> PointVertexLayout {
    const auto& attributes = vertices;
    return {
        .position    = FindAttrSlot(attributes, WE_IN_POSITION),
        .texcoord    = FindAttrSlot(attributes, WE_IN_TEXCOORDVEC4),
        .color       = FindAttrSlot(attributes, WE_IN_COLOR),
        .velocity    = FindAttrSlot(attributes, WE_IN_TEXCOORDVEC4C1),
        .texcoord_c2 = FindAttrSlot(attributes, WE_IN_TEXCOORDC2),
    };
}

auto ResolveRopeVertexLayout(const SceneVertexArray& vertices, GOption option) -> RopeVertexLayout {
    const auto& attributes = vertices;
    return {
        .position       = FindAttrSlot(attributes, WE_IN_POSITIONVEC4),
        .endpoint       = FindAttrSlot(attributes, WE_IN_TEXCOORDVEC4),
        .previous_point = FindAttrSlot(attributes, WE_IN_TEXCOORDVEC4C1),
        .next_point     = FindAttrSlot(
            attributes, option.thick_format ? WE_IN_TEXCOORDVEC4C2 : WE_IN_TEXCOORDVEC3C2),
        .color_end = FindAttrSlot(attributes, WE_IN_TEXCOORDVEC4C3),
        .color     = FindAttrSlot(attributes, WE_IN_COLOR),
        .corner_uv =
            FindAttrSlot(attributes, option.thick_format ? WE_IN_TEXCOORDC4 : WE_IN_TEXCOORDC3),
    };
}

void Write2(mut_ref<float[]> data, AttrSlot slot, float x, float y) noexcept {
    if (! slot.enabled) return;
    data[slot.offset]            = x;
    data[slot.offset + usize(1)] = y;
}

void Write3(mut_ref<float[]> data, AttrSlot slot, float x, float y, float z) noexcept {
    if (! slot.enabled) return;
    data[slot.offset]            = x;
    data[slot.offset + usize(1)] = y;
    data[slot.offset + usize(2)] = z;
}

void Write3(mut_ref<float[]> data, AttrSlot slot, const Eigen::Vector3f& source) noexcept {
    Write3(data, slot, source[0], source[1], source[2]);
}

void Write4(mut_ref<float[]> data, AttrSlot slot, float x, float y, float z, float w) noexcept {
    if (! slot.enabled) return;
    Write3(data, slot, x, y, z);
    data[slot.offset + usize(3)] = w;
}

void Write4(mut_ref<float[]> data, AttrSlot slot, const Eigen::Vector3f& source, float w) noexcept {
    Write4(data, slot, source[0], source[1], source[2], w);
}

// Four corner UVs without geometry shader variant (in order of index array (0,1,3)(1,2,3)).
const array<array<float, 2>, 4> g_corners { array<float, 2> { 0.0f, 1.0f },
                                            array<float, 2> { 1.0f, 1.0f },
                                            array<float, 2> { 1.0f, 0.0f },
                                            array<float, 2> { 0.0f, 0.0f } };

void WriteColor(mut_ref<float[]> data, AttrSlot slot, const ExtractParticle& value) noexcept {
    Write4(data, slot, value.color[0], value.color[1], value.color[2], value.alpha);
}

auto AnimationLifetime(const ExtractParticle& value, ParticleAnimationSpec animation) noexcept
    -> float {
    if (value.lifetime <= 0.0f) return 0.0f;
    switch (animation.mode) {
    case ParticleAnimationMode::RANDOMONE:
        return rstd::cmp::min(f32(1.0f).next_down().to_primitive(),
                              rstd::cmp::max(0.0f, value.random));
    case ParticleAnimationMode::SEQUENCE:
        if (value.initial_lifetime == 0.0f) return 0.0f;
        return (1.0f - (value.lifetime / value.initial_lifetime)) * animation.sequence_multiplier;
    }
    return 0.0f;
}

void GenParticlePointData(slice<ExtractInstance> instances, const ParticleSubSystem& subsystem,
                          GOption option, const PointVertexLayout& layout,
                          SceneVertexWriter& writer) noexcept {
    // GS_ENABLED=0 (no geometry shader) variant: CPU expands each particle into a 4-vertex quad.
    // a_TexCoordVec4 = (corner_u, corner_v, rz, size)，a_TexCoordC2 = (rx, ry)。
    static const array<array<float, 2>, 4> corners { array<float, 2> { 0.0f, 1.0f },
                                                     array<float, 2> { 1.0f, 1.0f },
                                                     array<float, 2> { 1.0f, 0.0f },
                                                     array<float, 2> { 0.0f, 0.0f } };
    for (const auto& instance : instances) {
        if (subsystem.InstanceState(instance.instance_index).no_live_particle) continue;
        for (auto slot : instance.slots) {
            auto value = instance.Particle(slot);
            if (value.lifetime <= 0.0f) continue;

            auto render_position =
                subsystem.RenderPosition(instance.instance_index, value.position);
            auto lifetime = AnimationLifetime(value, subsystem.AnimationSpec());
            if (! option.expand_corners) {
                auto destination = writer.AppendZeroedVertex();
                if (destination.is_none()) return;
                auto data = *destination;
                Write3(data,
                       layout.position,
                       render_position[0],
                       render_position[1],
                       render_position[2]);
                Write4(data,
                       layout.texcoord,
                       value.rotation[0],
                       value.rotation[1],
                       value.rotation[2],
                       value.size * 0.5f);
                Write4(data,
                       layout.color,
                       value.color[0],
                       value.color[1],
                       value.color[2],
                       value.alpha);
                if (option.thick_format) {
                    Write4(data,
                           layout.velocity,
                           value.velocity[0],
                           value.velocity[1],
                           value.velocity[2],
                           lifetime);
                }
                continue;
            }
            for (const auto& corner : corners) {
                auto destination = writer.AppendZeroedVertex();
                if (destination.is_none()) return;
                auto data = *destination;
                Write3(data,
                       layout.position,
                       render_position[0],
                       render_position[1],
                       render_position[2]);
                Write4(data,
                       layout.texcoord,
                       corner[usize(0)],
                       corner[usize(1)],
                       value.rotation[2],
                       value.size * 0.5f);
                Write2(data, layout.texcoord_c2, value.rotation[0], value.rotation[1]);
                Write4(data,
                       layout.color,
                       value.color[0],
                       value.color[1],
                       value.color[2],
                       value.alpha);
                if (option.thick_format) {
                    Write4(data,
                           layout.velocity,
                           value.velocity[0],
                           value.velocity[1],
                           value.velocity[2],
                           lifetime);
                }
            }
        }
    }
}

void GenRopeParticleData(slice<ExtractInstance> instances, const ParticleSubSystem& subsystem,
                         GOption option, const RopeVertexLayout& layout,
                         SceneVertexWriter& writer) {
    for (const auto& instance : instances) {
        if (subsystem.InstanceState(instance.instance_index).no_live_particle) continue;
        Vec<usize> slots;
        slots.reserve(instance.slots.len());
        for (auto slot : instance.slots) {
            if (instance.lifetimes[slot.index] > 0.0f) slots.push(usize(slot.index));
        }
        rstd::slice_::sort_unstable_by(
            slots.as_mut_slice().as_mut_ref(), [&](usize lhs, usize rhs) {
                return instance.states[lhs].spawn_sequence < instance.states[rhs].spawn_sequence;
            });
        if (slots.len().to_primitive() < 2) continue;

        auto particle = [&](rstd::size_t index) {
            return instance.Particle(particle::ParticleSlot { slots[usize(index)] });
        };
        auto render_position = [&](const ExtractParticle& value) {
            return subsystem.RenderPosition(instance.instance_index, value.position);
        };

        auto emit_group = [&](rstd::size_t begin, rstd::size_t end) -> bool {
            if (end - begin < 2) return true;

            auto  newest      = particle(end - 1);
            auto  before      = particle(end - 2);
            auto  newest_age  = newest.initial_lifetime - newest.lifetime;
            auto  before_age  = before.initial_lifetime - before.lifetime;
            auto  emit_period = before_age - newest_age;
            float sequence_offset {};
            if (emit_period > 1e-6f)
                sequence_offset =
                    -rstd::cmp::min(1.0f, rstd::cmp::max(0.0f, newest_age / emit_period));

            auto segment_count = end - begin - 1;
            for (rstd::size_t index = begin; index < end - 1; ++index) {
                auto previous_index = index == begin ? begin : index - 1;
                auto next_index     = index + 1;
                auto after_index    = rstd::cmp::min(end - 1, index + 2);
                auto current        = particle(index);
                auto previous       = particle(previous_index);
                auto next           = particle(next_index);
                auto after          = particle(after_index);

                if (option.expand_corners) {
                    // GS_ENABLED=0: Each segment is expanded into a 4-vertex quad.
                    // a_PositionVec4=(sp,size)，a_TexCoordVec4=(ep,trail_len)，
                    // a_TexCoordVec4C1=(scp,trail_pos)，C2=(ecp,size_end)，
                    // corner UVs in a_TexCoordC3/C4。
                    const auto  sp        = render_position(current);
                    const auto  ep        = render_position(next);
                    const float trail_pos = static_cast<float>(index - begin) + sequence_offset;
                    // Control point offset: perpendicular to segment, based on end rotation.
                    Eigen::Vector3f cp_vec  = Eigen::AngleAxisf(next.rotation[2] + 1.57079632679f,
                                                                Eigen::Vector3f::UnitZ()) *
                                              Eigen::Vector3f { 0.0f, next.size * 0.25f, 0.0f };
                    Eigen::Vector3f pos_vec = ep - sp;
                    cp_vec = pos_vec.normalized().dot(cp_vec) > 0.0f ? cp_vec : -cp_vec;
                    const Eigen::Vector3f scp        = sp + cp_vec;
                    const Eigen::Vector3f ecp        = ep - cp_vec;
                    const float           size_start = current.size * 0.5f;
                    const float           size_end   = next.size * 0.5f;
                    for (const auto& corner : g_corners) {
                        auto destination = writer.AppendZeroedVertex();
                        if (destination.is_none()) return false;
                        auto data = *destination;
                        Write4(data, layout.position, sp, size_start);
                        Write4(data, layout.endpoint, ep, static_cast<float>(segment_count));
                        Write4(data, layout.previous_point, scp, trail_pos);
                        if (option.thick_format)
                            Write4(data, layout.next_point, ecp, size_end);
                        else
                            Write3(data, layout.next_point, ecp);
                        Write2(data, layout.corner_uv, corner[usize(0)], corner[usize(1)]);
                        WriteColor(data, layout.color_end, next);
                        WriteColor(data, layout.color, current);
                    }
                    continue;
                }

                auto destination = writer.AppendZeroedVertex();
                if (destination.is_none()) return false;
                auto data = *destination;
                Write4(data, layout.position, render_position(current), current.size * 0.5f);
                Write4(data,
                       layout.endpoint,
                       render_position(next),
                       static_cast<float>(segment_count));
                Write4(data,
                       layout.previous_point,
                       render_position(previous),
                       static_cast<float>(index - begin) + sequence_offset);
                if (option.thick_format)
                    Write4(data, layout.next_point, render_position(after), next.size * 0.5f);
                else
                    Write3(data, layout.next_point, render_position(after));
                WriteColor(data, layout.color_end, next);
                WriteColor(data, layout.color, current);
            }
            return true;
        };

        auto sequence_count = subsystem.RopeSequenceCount();
        if (sequence_count.is_none()) {
            if (! emit_group(0, slots.len().to_primitive())) return;
            continue;
        }

        auto count = rstd::as_cast<u64>(rstd::cmp::max(*sequence_count, u32(2)));
        auto group = [&](rstd::size_t index) {
            return instance.states[slots[usize(index)]].spawn_sequence / count;
        };
        rstd::size_t begin {};
        while (begin < slots.len().to_primitive()) {
            auto         sequence_group = group(begin);
            rstd::size_t end            = begin + 1;
            while (end < slots.len().to_primitive() && group(end) == sequence_group) ++end;
            if (! emit_group(begin, end)) return;
            begin = end;
        }
    }
}

void GenRopeTrailSegments(const ExtractParticle& value, const ParticleSubSystem& subsystem,
                          usize instance_index, const TrailHistoryAttribute& trails, GOption option,
                          const RopeVertexLayout& layout, SceneVertexWriter& writer) {
    auto state = trails.State(value.slot);
    if (state.len == usize()) return;

    auto size         = value.size * 0.5f;
    auto trail_length = static_cast<float>(state.sample_count.to_primitive());

    auto point = [&](usize point_index) -> Eigen::Vector3f {
        if (point_index == usize()) return subsystem.RenderPosition(instance_index, value.position);
        return subsystem.RenderPosition(instance_index,
                                        trails.At(value.slot, state.len - point_index));
    };

    for (usize sample_index {}; sample_index < state.len; ++sample_index) {
        auto previous       = point(sample_index);
        auto current        = point(sample_index + usize(1));
        auto start_control  = point(sample_index == usize() ? usize() : sample_index - usize(1));
        auto end_control    = point(rstd::cmp::min(sample_index + usize(2), state.len));
        auto trail_position = static_cast<float>(sample_index.to_primitive());
        if (option.expand_corners) {
            // GS_ENABLED=0: Each sample point is expanded into a 4-vertex quadrilateral.
            for (const auto& corner : g_corners) {
                auto destination = writer.AppendZeroedVertex();
                if (destination.is_none()) return;
                auto data = *destination;
                Write4(data, layout.position, previous, size);
                Write4(data, layout.endpoint, current, trail_length);
                Write4(data, layout.previous_point, start_control, trail_position);
                if (option.thick_format)
                    Write4(data, layout.next_point, end_control, size);
                else
                    Write3(data, layout.next_point, end_control);
                Write2(data, layout.corner_uv, corner[usize(0)], corner[usize(1)]);
                WriteColor(data, layout.color_end, value);
                WriteColor(data, layout.color, value);
            }
            continue;
        }
        auto destination = writer.AppendZeroedVertex();
        if (destination.is_none()) return;
        auto data = *destination;
        Write4(data, layout.position, previous, size);
        Write4(data, layout.endpoint, current, trail_length);
        Write4(data, layout.previous_point, start_control, trail_position);
        if (option.thick_format)
            Write4(data, layout.next_point, end_control, size);
        else
            Write3(data, layout.next_point, end_control);
        WriteColor(data, layout.color_end, value);
        WriteColor(data, layout.color, value);
    }
}

void GenRopeTrailData(slice<ExtractInstance> instances, const ParticleSubSystem& subsystem,
                      GOption option, const RopeVertexLayout& layout, SceneVertexWriter& writer) {
    for (const auto& instance : instances) {
        if (subsystem.InstanceState(instance.instance_index).no_live_particle) continue;
        if (instance.trail.is_none()) continue;
        auto trails = *instance.trail;
        for (auto slot : instance.slots) {
            auto value = instance.Particle(slot);
            if (value.lifetime <= 0.0f) continue;
            GenRopeTrailSegments(
                value, subsystem, instance.instance_index, *trails, option, layout, writer);
            if (writer.Overflowed()) return;
        }
    }
}

} // namespace

void ParticleRawGenerator::Compile(particle::ParticleViewCompiler& compiler) {
    auto attributes = m_subsystem->Attributes();
    compiler.ReadBase(attributes.position);
    m_velocity         = compiler.Read(attributes.velocity);
    m_rotation         = compiler.Read(attributes.rotation);
    m_color            = compiler.Read(attributes.color);
    m_alpha            = compiler.Read(attributes.alpha);
    m_size             = compiler.Read(attributes.size);
    m_lifetime         = compiler.Read(attributes.lifetime);
    m_random           = compiler.Read(attributes.random);
    m_initial_lifetime = compiler.Read(attributes.initial_lifetime);
    auto trail_key     = m_subsystem->TrailKey();
    if (trail_key.is_some()) m_trail = compiler.ReadObject(*trail_key);
}

void ParticleRawGenerator::Extract(particle::ParticleExtractContext& context) {
    auto instances = Vec<ExtractInstance>::with_capacity(context.instances.len());
    for (const auto& instance : context.instances) {
        Option<ref<TrailHistoryAttribute>> trail = None();
        if (m_trail.Valid()) trail = Some(instance.view.ReadObject(m_trail));
        instances.emplace_back(ExtractInstance {
            .states            = instance.view.States(),
            .positions         = instance.view.Positions(),
            .velocities        = instance.view.Read(m_velocity),
            .rotations         = instance.view.Read(m_rotation),
            .colors            = instance.view.Read(m_color),
            .alphas            = instance.view.Read(m_alpha),
            .sizes             = instance.view.Read(m_size),
            .lifetimes         = instance.view.Read(m_lifetime),
            .randoms           = instance.view.Read(m_random),
            .initial_lifetimes = instance.view.Read(m_initial_lifetime),
            .slots             = instance.slots,
            .trail             = trail,
            .instance_index    = instance.instance_index,
        });
    }

    auto&   mesh     = m_subsystem->Mesh();
    auto&   vertices = mesh.GetVertexArray(usize());
    GOption option {
        .thick_format   = vertices.GetOption(WE_CB_THICK_FORMAT),
        .expand_corners = ! vertices.GetOption(WE_CB_GS_ENABLED),
    };

    auto rope       = vertices.GetOption(WE_PRENDER_ROPE);
    auto rope_trail = vertices.GetOption(WE_PRENDER_ROPE_TRAIL);
    auto result     = vertices.RewriteVertices([&](SceneVertexWriter& writer) {
        if (rope || rope_trail) {
            auto layout = ResolveRopeVertexLayout(vertices, option);
            if (rope) {
                GenRopeParticleData(instances.as_slice(), *m_subsystem, option, layout, writer);
            } else {
                GenRopeTrailData(instances.as_slice(), *m_subsystem, option, layout, writer);
            }
            return;
        }

        auto layout = ResolvePointVertexLayout(vertices);
        GenParticlePointData(instances.as_slice(), *m_subsystem, option, layout, writer);
    });
    if (result.overflowed) {
        rstd_error("particle vertex capacity exceeded: written={}, capacity={}",
                   result.vertex_count,
                   result.capacity);
    }
}
