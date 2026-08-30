module;

#include <rstd/macro.hpp>

module wescene.pkg.parse;

import eigen;
import rstd;
import rstd.cppstd;
import rstd.log;
import wescene.core;
import wescene.particle;
import wescene.particle.program;
import wescene.scene;
import wescene.utils;

using namespace rstd::prelude;
using namespace rstd::literals;
using namespace owe;
using rstd::sync::Arc;

namespace
{

auto ControlpointRotation(const Eigen::Vector3f& angles) -> Eigen::Matrix3d {
    return (Eigen::AngleAxisd(static_cast<double>(angles.z()), Eigen::Vector3d::UnitZ()) *
            Eigen::AngleAxisd(static_cast<double>(angles.y()), Eigen::Vector3d::UnitY()) *
            Eigen::AngleAxisd(static_cast<double>(angles.x()), Eigen::Vector3d::UnitX()))
        .toRotationMatrix();
}

template<typename Attribute, typename... Args>
auto RegisterAttribute(particle::ParticleSchemaBuilder& builder, ref<str> name, ref<str> owner,
                       Args&&... args) -> particle::ParticleAttributeKey<Attribute> {
    auto result = builder.Register<Attribute>(name, owner, rstd::forward<Args>(args)...);
    if (result.is_err()) rstd::panic { "failed to register particle attribute" };
    return result.unwrap();
}

class LifecycleProgram {
public:
    explicit LifecycleProgram(ParticleAttributes attributes): m_attributes(attributes) {}

    void Compile(particle::ParticleViewCompiler& compiler) {
        m_alpha         = compiler.Write(m_attributes.alpha);
        m_size          = compiler.Write(m_attributes.size);
        m_color         = compiler.Write(m_attributes.color);
        m_lifetime      = compiler.Write(m_attributes.lifetime);
        m_initial_alpha = compiler.Read(m_attributes.initial_alpha);
        m_initial_size  = compiler.Read(m_attributes.initial_size);
        m_initial_color = compiler.Read(m_attributes.initial_color);
    }

    void Update(particle::ParticleLifecycleContext& context) {
        auto alpha         = context.view.Write(m_alpha);
        auto size          = context.view.Write(m_size);
        auto color         = context.view.Write(m_color);
        auto lifetime      = context.view.Write(m_lifetime);
        auto initial_alpha = context.view.Read(m_initial_alpha);
        auto initial_size  = context.view.Read(m_initial_size);
        auto initial_color = context.view.Read(m_initial_color);
        for (auto slot : context.slots) {
            alpha[slot.index] = initial_alpha[slot.index];
            size[slot.index]  = initial_size[slot.index];
            color[slot.index] = initial_color[slot.index];
            lifetime[slot.index] -= context.delta.to_primitive();
            if (lifetime[slot.index] <= 0.0f) context.Kill(slot);
        }
    }

private:
    ParticleAttributes                                           m_attributes;
    particle::ParticleWriteIndex<particle::AlphaAttribute>       m_alpha;
    particle::ParticleWriteIndex<particle::SizeAttribute>        m_size;
    particle::ParticleWriteIndex<particle::ColorAttribute>       m_color;
    particle::ParticleWriteIndex<particle::LifetimeAttribute>    m_lifetime;
    particle::ParticleReadIndex<particle::InitialAlphaAttribute> m_initial_alpha;
    particle::ParticleReadIndex<particle::InitialSizeAttribute>  m_initial_size;
    particle::ParticleReadIndex<particle::InitialColorAttribute> m_initial_color;
};

class ChildEventProgram {
public:
    explicit ChildEventProgram(ParticleSubSystem& subsystem): m_subsystem(&subsystem) {}

    void Compile(particle::ParticleViewCompiler& compiler) {
        m_subsystem->CompileRuntimeView(compiler);
    }
    void Process(particle::ParticleEventContext& context) {
        m_subsystem->ProcessChildEvents(context);
    }

private:
    ParticleSubSystem* m_subsystem;
};

class TrailUpdateProgram {
public:
    TrailUpdateProgram(ParticleAttributes                                    attributes,
                       particle::ParticleAttributeKey<TrailHistoryAttribute> trail,
                       f64                                                   sample_interval)
        : m_attributes(attributes), m_trail(trail), m_sample_interval(sample_interval) {}

    void Compile(particle::ParticleViewCompiler& compiler) {
        compiler.ReadBase(m_attributes.position);
        m_trail_object = compiler.WriteObject(m_trail);
    }

    void Update(particle::ParticleUpdateContext& context) {
        auto frame     = ParticleFrameFrom(context.frame);
        auto positions = context.view.Positions();
        auto trail     = context.view.WriteObject(m_trail_object);

        auto interval     = m_sample_interval.to_primitive();
        auto delta        = context.delta.to_primitive();
        auto remainder    = frame->trail_sample_remainder.to_primitive();
        auto sample_steps = rstd::cmp::min(frame->trail_sample_steps, trail->SampleCapacity());

        for (auto slot : context.slots) {
            auto index = slot.index;
            auto state = trail->State(slot);
            if (! state.has_previous_position) {
                trail->Initialize(slot, positions[index]);
                trail->SetPreviousPosition(slot, positions[index]);
                continue;
            }
            for (usize sample {}; sample < sample_steps; ++sample) {
                auto age = remainder +
                           static_cast<double>((sample_steps - sample - usize(1)).to_primitive()) *
                               interval;
                auto amount = delta > 0.0 ? std::clamp((delta - age) / delta, 0.0, 1.0) : 1.0;
                auto position =
                    state.previous_position +
                    (positions[index] - state.previous_position) * static_cast<float>(amount);
                trail->Push(slot, position);
            }
            trail->SetPreviousPosition(slot, positions[index]);
        }
    }

private:
    ParticleAttributes                                        m_attributes;
    particle::ParticleAttributeKey<TrailHistoryAttribute>     m_trail;
    particle::ParticleWriteObjectIndex<TrailHistoryAttribute> m_trail_object;
    f64                                                       m_sample_interval {};
};

} // namespace

auto ParticleAttributes::Register(particle::ParticleSchemaBuilder& builder) -> ParticleAttributes {
    const Eigen::Vector3f zero = Eigen::Vector3f::Zero();
    const Eigen::Vector3f one  = Eigen::Vector3f::Ones();
    return {
        .position = builder.PositionKey(),
        .velocity =
            RegisterAttribute<particle::VelocityAttribute>(builder, "velocity"_str, "we"_str, zero),
        .acceleration = RegisterAttribute<particle::AccelerationAttribute>(
            builder, "acceleration"_str, "we"_str, zero),
        .rotation =
            RegisterAttribute<particle::RotationAttribute>(builder, "rotation"_str, "we"_str, zero),
        .angular_velocity = RegisterAttribute<particle::AngularVelocityAttribute>(
            builder, "angular_velocity"_str, "we"_str, zero),
        .angular_acceleration = RegisterAttribute<particle::AngularAccelerationAttribute>(
            builder, "angular_acceleration"_str, "we"_str, zero),
        .color = RegisterAttribute<particle::ColorAttribute>(builder, "color"_str, "we"_str, one),
        .alpha = RegisterAttribute<particle::AlphaAttribute>(builder, "alpha"_str, "we"_str, 1.0f),
        .size  = RegisterAttribute<particle::SizeAttribute>(builder, "size"_str, "we"_str, 20.0f),
        .lifetime =
            RegisterAttribute<particle::LifetimeAttribute>(builder, "lifetime"_str, "we"_str, 1.0f),
        .random =
            RegisterAttribute<particle::RandomAttribute>(builder, "random"_str, "we"_str, 0.0f),
        .initial_color = RegisterAttribute<particle::InitialColorAttribute>(
            builder, "initial_color"_str, "we"_str, one),
        .initial_alpha = RegisterAttribute<particle::InitialAlphaAttribute>(
            builder, "initial_alpha"_str, "we"_str, 1.0f),
        .initial_size = RegisterAttribute<particle::InitialSizeAttribute>(
            builder, "initial_size"_str, "we"_str, 20.0f),
        .initial_lifetime = RegisterAttribute<particle::InitialLifetimeAttribute>(
            builder, "initial_lifetime"_str, "we"_str, 1.0f),
    };
}

TrailHistoryAttribute::TrailHistoryAttribute(particle::ParticleAttributeDescriptor descriptor,
                                             usize                                 sample_capacity)
    : m_descriptor(rstd::move(descriptor)), m_sample_capacity(sample_capacity) {}

auto TrailHistoryAttribute::Descriptor() const -> ref<particle::ParticleAttributeDescriptor> {
    return ref<particle::ParticleAttributeDescriptor>::from_raw_parts(
        rstd::addressof(m_descriptor));
}

auto TrailHistoryAttribute::ConcreteType() const noexcept -> rstd::any::TypeId {
    return m_descriptor.concrete_type;
}

auto TrailHistoryAttribute::ValueType() const noexcept -> rstd::any::TypeId {
    return m_descriptor.value_type;
}

auto TrailHistoryAttribute::Len() const noexcept -> usize { return m_states.len(); }
auto TrailHistoryAttribute::Capacity() const noexcept -> usize { return m_states.capacity(); }

void TrailHistoryAttribute::Reserve(usize total_slots) {
    if (total_slots <= m_states.capacity()) return;
    if (m_sample_capacity != usize() && total_slots > usize::MAX / m_sample_capacity) {
        rstd::panic { "particle trail capacity overflow" };
    }
    m_states.reserve(total_slots - m_states.len());
    auto total_samples = total_slots * m_sample_capacity;
    if (total_samples > m_positions.capacity()) {
        m_positions.reserve(total_samples - m_positions.len());
    }
}

void TrailHistoryAttribute::AppendDefaults(usize count) {
    for (usize slot {}; slot < count; ++slot) {
        m_states.emplace_back();
        for (usize index {}; index < m_sample_capacity; ++index) {
            m_positions.emplace_back(Eigen::Vector3f::Zero());
        }
    }
}

void TrailHistoryAttribute::ResetSlots(slice<particle::ParticleSlot> slots) {
    for (auto slot : slots) {
        m_states[slot.index] = {};
        auto begin           = slot.index * m_sample_capacity;
        for (usize index {}; index < m_sample_capacity; ++index) {
            m_positions[begin + index] = Eigen::Vector3f::Zero();
        }
    }
}

void TrailHistoryAttribute::Clear() {
    m_states.clear();
    m_positions.clear();
}

auto TrailHistoryAttribute::CloneEmpty() const -> TrailHistoryAttribute {
    return TrailHistoryAttribute(m_descriptor.Clone(), m_sample_capacity);
}

void TrailHistoryAttribute::Push(particle::ParticleSlot slot, const Eigen::Vector3f& position) {
    if (m_sample_capacity == usize()) return;
    auto& state        = m_states[slot.index];
    state.head         = (state.head + usize(1)) % m_sample_capacity;
    state.sample_count = rstd::cmp::min(state.sample_count + usize(1), m_sample_capacity);
    m_positions[slot.index * m_sample_capacity + state.head] = position;
    if (state.len < m_sample_capacity) ++state.len;
}

void TrailHistoryAttribute::Initialize(particle::ParticleSlot slot,
                                       const Eigen::Vector3f& position) {
    if (m_sample_capacity == usize()) return;
    auto& state        = m_states[slot.index];
    state              = {};
    state.len          = m_sample_capacity;
    state.sample_count = usize(1);
    const auto begin   = slot.index * m_sample_capacity;
    for (usize index {}; index < m_sample_capacity; ++index) {
        m_positions[begin + index] = position;
    }
}

auto TrailHistoryAttribute::At(particle::ParticleSlot slot, usize logical_index) const
    -> Eigen::Vector3f {
    const auto state = m_states[slot.index];
    if (logical_index >= state.len) rstd::panic { "particle trail index out of bounds" };
    auto index = (state.head + m_sample_capacity - (state.len - usize(1) - logical_index)) %
                 m_sample_capacity;
    return m_positions[slot.index * m_sample_capacity + index];
}

auto TrailHistoryAttribute::State(particle::ParticleSlot slot) const -> TrailSlotState {
    return m_states[slot.index];
}

void TrailHistoryAttribute::SetPreviousPosition(particle::ParticleSlot slot,
                                                const Eigen::Vector3f& position) {
    auto& state                 = m_states[slot.index];
    state.previous_position     = position;
    state.has_previous_position = true;
}

auto owe::ParticleFrameFrom(ref<dyn<rstd::any::Any>> frame) -> ref<ParticleFrame> {
    auto value = rstd::any::downcast_ref<ParticleFrame>(frame);
    if (value.is_none()) rstd::panic { "unexpected particle frame context" };
    return *value;
}

void ParticleSpawnPipeline::Compile(particle::ParticleViewCompiler& compiler) {
    if (m_compiled) return;
    compiler.WriteBase(m_attributes.position);
    m_velocity         = compiler.Write(m_attributes.velocity);
    m_rotation         = compiler.Write(m_attributes.rotation);
    m_angular_velocity = compiler.Write(m_attributes.angular_velocity);
    m_color            = compiler.Write(m_attributes.color);
    m_alpha            = compiler.Write(m_attributes.alpha);
    m_size             = compiler.Write(m_attributes.size);
    m_lifetime         = compiler.Write(m_attributes.lifetime);
    m_random           = compiler.Write(m_attributes.random);
    m_initial_color    = compiler.Write(m_attributes.initial_color);
    m_initial_alpha    = compiler.Write(m_attributes.initial_alpha);
    m_initial_size     = compiler.Write(m_attributes.initial_size);
    m_initial_lifetime = compiler.Write(m_attributes.initial_lifetime);
    m_compiled         = true;
}

auto ParticleSpawnPipeline::Bind(particle::ParticleWriteView view) -> ParticleSpawnColumns {
    if (! m_compiled) rstd::panic { "particle spawn pipeline is not compiled" };
    return {
        .states             = view.StatesMut(),
        .positions          = view.PositionsMut(),
        .velocities         = view.Write(m_velocity),
        .rotations          = view.Write(m_rotation),
        .angular_velocities = view.Write(m_angular_velocity),
        .colors             = view.Write(m_color),
        .alphas             = view.Write(m_alpha),
        .sizes              = view.Write(m_size),
        .lifetimes          = view.Write(m_lifetime),
        .randoms            = view.Write(m_random),
        .initial_colors     = view.Write(m_initial_color),
        .initial_alphas     = view.Write(m_initial_alpha),
        .initial_sizes      = view.Write(m_initial_size),
        .initial_lifetimes  = view.Write(m_initial_lifetime),
    };
}

void ParticleSpawnPipeline::Initialize(ParticleSpawnColumns&          columns,
                                       particle::ParticleSpawnRequest request,
                                       ref<dyn<rstd::any::Any>>       frame) {
    columns.randoms[request.slot.index] = Random::get(0.0f, 1.0f);
    for (auto& instruction : m_instructions) instruction.Initialize(columns, request, frame);
    if (! m_world_space) return;
    auto            wp_frame = ParticleFrameFrom(frame);
    auto            index    = request.slot.index;
    Eigen::Vector3f local_position =
        columns.positions[index] +
        wp_frame->subsystem->InstanceState(wp_frame->instance_index).bounded.position;
    Eigen::Vector4d world_position =
        wp_frame->world_from_spawn_space *
        Eigen::Vector4d(local_position.x(), local_position.y(), local_position.z(), 1.0);
    columns.positions[index]  = world_position.head<3>().cast<float>();
    columns.velocities[index] = (wp_frame->world_from_spawn_space.block<3, 3>(0, 0) *
                                 columns.velocities[index].cast<double>())
                                    .cast<float>();
}

ParticleSubSystem::ParticleSubSystem(Scene& scene, std::shared_ptr<SceneMesh> mesh, u32 max_count,
                                     f64 rate, u32 max_instance_count, f64 probability,
                                     SpawnType spawn_type, ParticleAnimationSpec animation_spec,
                                     ParticleFollowAnchor follow_anchor, u32 trail_length,
                                     f64 trail_duration, f64 start_time, bool world_space,
                                     Option<Arc<ParticleTrailUniformState>> trail_uniform_state)
    : m_scene(scene),
      m_mesh(rstd::move(mesh)),
      m_attributes(ParticleAttributes::Register(m_schema_builder)),
      m_spawn_pipeline(m_attributes),
      m_animation_spec(animation_spec),
      m_follow_anchor(follow_anchor),
      m_max_count(max_count),
      m_rate(rate),
      m_start_time(start_time),
      m_world_space(world_space),
      m_max_instance_count(EffectiveInstanceCapacity(max_instance_count, spawn_type)),
      m_probability(probability),
      m_spawn_type(spawn_type),
      m_trail_length(trail_length),
      m_trail_sample_interval(trail_length == u32()
                                  ? f64()
                                  : f64(trail_duration.to_primitive() /
                                        static_cast<double>(trail_length.to_primitive()))),
      m_trail_uniform_state(rstd::move(trail_uniform_state)) {
    if (m_trail_length != u32()) {
        m_trail_key =
            Some(RegisterAttribute<TrailHistoryAttribute>(m_schema_builder,
                                                          "trail_history"_str,
                                                          "we.rope"_str,
                                                          rstd::as_cast<usize>(m_trail_length)));
    }
}

ParticleSubSystem::~ParticleSubSystem() = default;

void ParticleSubSystem::Finalize() {
    if (m_system.is_some()) return;
    if (m_world_space) m_spawn_pipeline.EnableWorldSpace();
    m_program.AddLifecycle(
        Box<dyn<particle::ParticleLifecycleProgram>>::make(LifecycleProgram(m_attributes)));
    m_program.AddEvent(Box<dyn<particle::ParticleEventProgram>>::make(ChildEventProgram(*this)));
    if (m_trail_key.is_some()) {
        m_program.AddPostUpdate(Box<dyn<particle::ParticleUpdateProgram>>::make(
            TrailUpdateProgram(m_attributes, *m_trail_key, m_trail_sample_interval)));
    }
    m_program.AddExtractor(
        Box<dyn<particle::ParticleExtractProgram>>::make(ParticleRawGenerator(*this)));

    auto definition = particle::ParticleDefinition::Prepare(rstd::move(m_schema_builder).Build(),
                                                            rstd::move(m_program),
                                                            rstd::as_cast<usize>(m_max_count));
    if (definition.is_err()) {
        auto error = rstd::move(definition).unwrap_err();
        rstd_error("particle definition prepare failed: {}", error.message.as_str());
        rstd::panic { "particle definition prepare failed" };
    }
    m_system = Some(Box<particle::ParticleSystem>::make(rstd::move(definition).unwrap()));
}

void ParticleSubSystem::AddEmitter(Box<dyn<particle::ParticleEmitterProgram>> emitter) {
    m_program.AddEmitter(rstd::move(emitter));
}

void ParticleSubSystem::AddInitializer(ParticleSpawnInstruction initializer) {
    m_spawn_pipeline.Add(rstd::move(initializer));
}

void ParticleSubSystem::AddOperator(Box<dyn<particle::ParticleUpdateProgram>> update) {
    m_program.AddUpdate(rstd::move(update));
}

void ParticleSubSystem::AddChild(Box<ParticleSubSystem> child) {
    m_children.push(rstd::move(child));
}

auto ParticleSubSystem::System() noexcept -> particle::ParticleSystem& {
    if (m_system.is_none()) rstd::panic { "particle subsystem not finalized" };
    return *m_system->get();
}

auto ParticleSubSystem::QueryNewInstance() -> Option<ParticleInstanceRef> {
    if (Random::get(0.0, 1.0) > m_probability.to_primitive()) return None();

    auto& system = System();
    for (usize index {}; index < system.InstanceCount(); ++index) {
        auto& state = m_instance_states[index];
        if (! state.death || ! state.no_live_particle) continue;
        system.Instance(index).Reset();
        state.Reset();
        return Some(ParticleInstanceRef {
            .instance = rstd::addressof(system.Instance(index)),
            .state    = rstd::addressof(state),
            .index    = index,
        });
    }

    if (system.InstanceCount() >= rstd::as_cast<usize>(m_max_instance_count)) return None();
    auto  index    = system.InstanceCount();
    auto& instance = system.CreateInstance();
    m_instance_states.emplace_back();
    return Some(ParticleInstanceRef {
        .instance = rstd::addressof(instance),
        .state    = rstd::addressof(m_instance_states[index]),
        .index    = index,
    });
}

void ParticleSubSystem::CompileRuntimeView(particle::ParticleViewCompiler& compiler) {
    compiler.ReadBase(m_attributes.position);
    m_follow_velocity = compiler.Read(m_attributes.velocity);
    m_follow_size     = compiler.Read(m_attributes.size);
    m_follow_lifetime = compiler.Read(m_attributes.lifetime);
}

auto ParticleSubSystem::FollowWorldPosition(particle::ParticleInstance& instance,
                                            usize                       parent_instance_index,
                                            particle::ParticleSlot slot) const -> Eigen::Vector3f {
    auto       view       = instance.Binding().Read();
    const auto positions  = view.Positions();
    const auto velocities = view.Read(m_follow_velocity);
    const auto sizes      = view.Read(m_follow_size);
    auto       pos        = RenderPosition(parent_instance_index, positions[slot.index]);
    if (! m_follow_anchor.trail_renderer) return m_world_space ? pos : OwnerLocalToWorld(pos);

    float speed = velocities[slot.index].norm();
    if (speed <= 1e-6f) return m_world_space ? pos : OwnerLocalToWorld(pos);
    float trail_length =
        std::max(0.0f, std::min(speed * m_follow_anchor.length, m_follow_anchor.max_length));
    if (trail_length <= 0.0f) return m_world_space ? pos : OwnerLocalToWorld(pos);
    float visual_half_length =
        (sizes[slot.index] * 0.5f) * m_follow_anchor.texture_ratio * trail_length * 0.5f;
    pos += velocities[slot.index].normalized() * visual_half_length;
    return m_world_space ? pos : OwnerLocalToWorld(pos);
}

bool ParticleSubSystem::LifetimeAlive(particle::ParticleInstance& instance,
                                      particle::ParticleSlot      slot) const {
    return instance.Binding().Read().Read(m_follow_lifetime)[slot.index] > 0.0f;
}

auto ParticleSubSystem::RenderPosition(usize instance_index, const Eigen::Vector3f& position) const
    -> Eigen::Vector3f {
    if (m_world_space) return position;
    return m_instance_states[instance_index].bounded.position + position;
}

auto ParticleSubSystem::SimulationControlpoint(usize index) const
    -> ParticleSimulationControlpoint {
    const auto& controlpoint = m_controlpoints[index];
    if (! m_world_space) {
        return {
            .center = controlpoint.offset,
            .basis  = controlpoint.rotation,
        };
    }

    const auto&     world_from_local = m_frame.world_from_spawn_space;
    Eigen::Vector4d center           = world_from_local * Eigen::Vector4d(controlpoint.offset.x(),
                                                                          controlpoint.offset.y(),
                                                                          controlpoint.offset.z(),
                                                                          1.0);
    return {
        .center = center.head<3>(),
        .basis  = world_from_local.block<3, 3>(0, 0) * controlpoint.rotation,
    };
}

auto ParticleSubSystem::OwnerLocalToWorld(const Eigen::Vector3f& position) const
    -> Eigen::Vector3f {
    if (m_owner_node == nullptr) return position;
    m_owner_node->UpdateTrans();
    Eigen::Vector4d world =
        m_owner_node->ModelTrans() * Eigen::Vector4d(position.x(), position.y(), position.z(), 1.0);
    return world.head<3>().cast<float>();
}

auto ParticleSubSystem::OwnerWorldToLocal(const Eigen::Vector3f& position) const
    -> Eigen::Vector3f {
    if (m_owner_node == nullptr) return position;
    m_owner_node->UpdateTrans();
    Eigen::Vector4d local = m_owner_node->ModelTrans().inverse() *
                            Eigen::Vector4d(position.x(), position.y(), position.z(), 1.0);
    return local.head<3>().cast<float>();
}

void ParticleSubSystem::UpdateFrameInput(f64 frame_time) {
    const auto            pointer = m_scene.PointerPosition();
    const auto            ortho   = m_scene.Ortho();
    const Eigen::Vector3d mouse_world {
        static_cast<double>(pointer[usize()]) * static_cast<double>(ortho[usize()].to_primitive()),
        (1.0 - static_cast<double>(pointer[usize(1)])) *
            static_cast<double>(ortho[usize(1)].to_primitive()),
        0.0,
    };
    Eigen::Vector3d mouse_local            = mouse_world;
    Eigen::Matrix3d world_from_local_dir   = Eigen::Matrix3d::Identity();
    Eigen::Matrix3d local_from_world_dir   = Eigen::Matrix3d::Identity();
    Eigen::Matrix4d local_from_world       = Eigen::Matrix4d::Identity();
    Eigen::Matrix4d world_from_spawn_space = Eigen::Matrix4d::Identity();
    if (m_owner_node != nullptr) {
        m_owner_node->UpdateTrans();
        const auto model       = m_owner_node->ModelTrans();
        world_from_spawn_space = model;
        local_from_world       = model.inverse();
        Eigen::Vector4d value =
            local_from_world * Eigen::Vector4d(mouse_world.x(), mouse_world.y(), 0.0, 1.0);
        mouse_local = value.head<3>();
        if (! m_world_space) {
            world_from_local_dir = model.block<3, 3>(0, 0);
            if (std::abs(world_from_local_dir.determinant()) > 1e-9) {
                local_from_world_dir = world_from_local_dir.inverse();
            }
        }
    }
    m_frame.subsystem              = this;
    m_frame.mouse_local            = mouse_local;
    m_frame.world_from_local_dir   = world_from_local_dir;
    m_frame.local_from_world_dir   = local_from_world_dir;
    m_frame.world_from_spawn_space = world_from_spawn_space;
    m_frame.local_from_world       = local_from_world;
    m_frame.world_space            = m_world_space;
    m_frame.time                   = m_time;
    m_frame.delta                  = frame_time;
    m_frame.emitter_delta          = frame_time;
    m_frame.trail_sample_steps     = usize();
    m_frame.trail_sample_remainder = f64();
    if (m_trail_key.is_some()) {
        const auto interval = m_trail_sample_interval.to_primitive();
        if (interval > 0.0) {
            auto elapsed = m_trail_sample_accumulator.to_primitive() + frame_time.to_primitive();
            const auto total_steps =
                u64(static_cast<rstd::uint64_t>(std::floor(elapsed / interval)));
            elapsed -= static_cast<double>(total_steps.to_primitive()) * interval;
            m_trail_sample_accumulator     = f64(elapsed);
            m_frame.trail_sample_steps     = rstd::as_cast<usize>(total_steps);
            m_frame.trail_sample_remainder = f64(elapsed);
            if (m_trail_uniform_state.is_some()) {
                (*m_trail_uniform_state)->render_var[usize(2)] =
                    static_cast<float>(std::clamp(elapsed / interval, 0.0, 1.0));
            }
        } else {
            m_frame.trail_sample_steps = usize(1);
            if (m_trail_uniform_state.is_some()) {
                (*m_trail_uniform_state)->render_var[usize(2)] = 1.0f;
            }
        }
    }
    for (usize index {}; index < m_frame.audio_average.len(); ++index) {
        m_frame.audio_average[index] = m_scene.AudioAverage(index).to_primitive();
    }
}

void ParticleSubSystem::UpdateControlpoints(ParticleInstanceRef current) {
    for (usize index {}; index < m_controlpoints.len(); ++index) {
        auto&           controlpoint = m_controlpoints[index];
        Eigen::Vector3f angles { Eigen::Vector3f::Zero() };
        controlpoint.offset = controlpoint.base_offset;
        if (m_instance_modifiers.is_some() && (*m_instance_modifiers).ControlpointsEnabled()) {
            const auto& position_override = (*m_instance_modifiers).Controlpoint(index);
            if (position_override.is_some()) {
                Eigen::Vector3d position =
                    Eigen::Vector3f { position_override->data() }.cast<double>();
                if (controlpoint.worldspace) {
                    Eigen::Vector4d local =
                        m_frame.local_from_world *
                        Eigen::Vector4d(position.x(), position.y(), position.z(), 1.0);
                    controlpoint.offset = local.head<3>();
                } else {
                    controlpoint.offset += position;
                }
            }
            angles = Eigen::Vector3f { (*m_instance_modifiers).ControlpointAngle(index).data() };
        }
        if (controlpoint.link_mouse) controlpoint.offset += m_frame.mouse_local;
        if (controlpoint.angle_track) {
            angles = controlpoint.angle_track->EvaluateVec3(angles);
        }
        controlpoint.rotation = ControlpointRotation(angles);
    }

    if (m_spawn_type != SpawnType::STATIC_CONTROLPOINT ||
        m_parent_controlpoint_start_index.is_none()) {
        return;
    }
    auto& bounded = current.state->bounded;
    if (bounded.parent == nullptr || bounded.parent_subsystem == nullptr) return;

    auto               view   = bounded.parent->Binding().Read();
    auto               states = view.States();
    std::vector<usize> slots;
    slots.reserve(states.len().to_primitive());
    for (usize index {}; index < states.len(); ++index) {
        if (states[index].active) slots.push_back(index);
    }
    std::sort(slots.begin(), slots.end(), [&](usize lhs, usize rhs) {
        return states[lhs].spawn_sequence < states[rhs].spawn_sequence;
    });

    auto controlpoint_index =
        rstd::as_cast<usize>(rstd::cmp::max(*m_parent_controlpoint_start_index, i32()));
    for (auto slot_index : slots) {
        if (controlpoint_index >= m_controlpoints.len()) break;
        particle::ParticleSlot slot { .index = slot_index };
        auto position = OwnerWorldToLocal(bounded.parent_subsystem->FollowWorldPosition(
            *bounded.parent, bounded.parent_instance_index, slot));
        m_controlpoints[controlpoint_index].offset =
            (position - current.state->bounded.position).cast<double>();
        ++controlpoint_index;
    }
}

void ParticleSubSystem::UpdateBoundedState(ParticleInstanceRef current) {
    auto& bounded = current.state->bounded;
    if (bounded.parent == nullptr || bounded.parent_subsystem == nullptr) return;

    bool type_has_death = m_spawn_type == SpawnType::EVENT_SPAWN ||
                          m_spawn_type == SpawnType::EVENT_FOLLOW ||
                          m_spawn_type == SpawnType::STATIC_CONTROLPOINT;

    auto& parent_storage = bounded.parent->Storage();
    if (bounded.particle_index >= isize() &&
        rstd::as_cast<usize>(bounded.particle_index) < parent_storage.Len()) {
        particle::ParticleSlot slot { .index = rstd::as_cast<usize>(bounded.particle_index) };
        if (m_spawn_type != SpawnType::STATIC_CONTROLPOINT) {
            bounded.position = OwnerWorldToLocal(bounded.parent_subsystem->FollowWorldPosition(
                *bounded.parent, bounded.parent_instance_index, slot));
        }
        if (m_spawn_type == SpawnType::EVENT_DEATH) bounded.particle_index = isize(-1);

        if (! current.state->death && type_has_death) {
            bool lifetime_ok     = bounded.parent_subsystem->LifetimeAlive(*bounded.parent, slot);
            current.state->death = ! lifetime_ok && bounded.previous_lifetime_ok;
            bounded.previous_lifetime_ok = lifetime_ok;
        }
    }

    if (! current.state->death && type_has_death) {
        current.state->death =
            bounded.parent_subsystem->InstanceState(bounded.parent_instance_index).death;
    }
}

void ParticleSubSystem::Advance(f64 frame_time, f64 child_frame_time, bool update_mesh) {
    m_time += frame_time;
    UpdateFrameInput(frame_time);
    if (m_spawn_type == SpawnType::STATIC && System().InstanceCount() == usize()) {
        (void)System().CreateInstance();
        m_instance_states.emplace_back();
    }

    auto frame_ref = rstd::dyn<rstd::any::Any>::from_ref(m_frame).as_ref();
    for (usize index {}; index < System().InstanceCount(); ++index) {
        ParticleInstanceRef current {
            .instance = rstd::addressof(System().Instance(index)),
            .state    = rstd::addressof(m_instance_states[index]),
            .index    = index,
        };
        UpdateBoundedState(current);
        if (current.state->death && (m_spawn_type == SpawnType::EVENT_FOLLOW ||
                                     m_spawn_type == SpawnType::STATIC_CONTROLPOINT)) {
            current.instance->Storage().Clear();
        }

        m_frame.instance_index = index;
        UpdateControlpoints(current);
        Warmup(current, frame_ref);
        m_pending_child_deaths.clear();
        System().Advance(*current.instance, frame_ref, frame_time, m_time);
        current.state->no_live_particle = true;
        auto states                     = current.instance->Binding().Read().States();
        for (const auto& state : states) {
            if (! state.active) continue;
            current.state->no_live_particle = false;
            break;
        }
        if (m_spawn_type == SpawnType::EVENT_DEATH) current.state->death = true;
    }

    if (update_mesh) {
        m_frame.instance_index = usize();
        System().Extract(frame_ref);
        m_mesh->SetDirty();
    }
    for (auto& child : m_children) {
        child->Tick(child_frame_time, update_mesh);
    }
}

void ParticleSubSystem::Warmup(ParticleInstanceRef current, ref<dyn<rstd::any::Any>> frame_ref) {
    if (! current.state->warmup_pending) return;
    current.state->warmup_pending = false;
    if (m_start_time <= f64()) return;

    constexpr double kFrameTime  = 1.0 / 60.0;
    constexpr auto   kMaxFrames  = u32(240);
    auto             frame_count = u32(static_cast<rstd::uint32_t>(
        std::max(1.0, std::ceil(m_start_time.to_primitive() / kFrameTime))));
    frame_count                  = rstd::cmp::min(frame_count, kMaxFrames);
    auto frame_time =
        f64(m_start_time.to_primitive() / static_cast<double>(frame_count.to_primitive()));

    auto saved_time          = m_frame.time;
    auto saved_delta         = m_frame.delta;
    auto saved_emitter_delta = m_frame.emitter_delta;
    auto warmup_start        = std::max(0.0, m_time.to_primitive() - m_start_time.to_primitive());
    for (u32 index {}; index < frame_count; ++index) {
        m_frame.time = f64(warmup_start + frame_time.to_primitive() *
                                              static_cast<double>((index + u32(1)).to_primitive()));
        m_frame.delta         = frame_time;
        m_frame.emitter_delta = frame_time;
        m_pending_child_deaths.clear();
        System().Advance(*current.instance, frame_ref, frame_time, m_frame.time);
    }
    m_frame.time          = saved_time;
    m_frame.delta         = saved_delta;
    m_frame.emitter_delta = saved_emitter_delta;
}

bool ParticleSubSystem::SyncPlayback() {
    if (m_playback_state.is_none()) return false;
    const auto sequence =
        (*m_playback_state)->reset_sequence.load(rstd::sync::atomic::Ordering::Acquire);
    if (sequence == m_seen_reset_sequence) return false;

    m_seen_reset_sequence      = sequence;
    m_time                     = f64();
    m_trail_sample_accumulator = f64();
    for (usize index {}; index < System().InstanceCount(); ++index) {
        System().Instance(index).Reset();
        m_instance_states[index].Reset();
    }
    return true;
}

void ParticleSubSystem::ExtractCurrentMesh() {
    UpdateFrameInput(f64());
    auto frame_ref = rstd::dyn<rstd::any::Any>::from_ref(m_frame).as_ref();
    System().Extract(frame_ref);
    m_mesh->SetDirty();
}

void ParticleSubSystem::Tick(f64 frame_time, bool update_mesh) {
    // Conditional particle layers stay parsed so user properties can reactivate them.
    // Their simulation must remain paused while the render graph elides the owner node.
    if (m_owner_node != nullptr && ! m_owner_node->Visible()) return;

    const bool reset = SyncPlayback();
    if (m_playback_state.is_some() &&
        ! (*m_playback_state)->playing.load(rstd::sync::atomic::Ordering::Acquire)) {
        if (reset && update_mesh) ExtractCurrentMesh();
        for (auto& child : m_children) child->Tick(f64(), update_mesh);
        return;
    }
    auto rate =
        m_instance_modifiers.is_some() ? (*m_instance_modifiers).Rate() : m_rate.to_primitive();
    Advance(frame_time * f64(std::max(rate, 0.0)), frame_time, update_mesh);
}

void ParticleSubSystem::SpawnChild(ParticleInstanceRef parent, ParticleSubSystem& child,
                                   particle::ParticleSlot slot, Eigen::Vector3f position,
                                   bool fixed) {
    if (child.Type() == SpawnType::STATIC_CONTROLPOINT)
        position = OwnerLocalToWorld(parent.state->bounded.position);
    else if (! fixed)
        position = FollowWorldPosition(*parent.instance, parent.index, slot);
    position      = child.OwnerWorldToLocal(position);
    auto instance = child.QueryNewInstance();
    if (instance.is_none()) return;
    instance->state->bounded = {
        .parent                = parent.instance,
        .parent_subsystem      = this,
        .parent_instance_index = parent.index,
        .particle_index =
            fixed ? isize(-1) : isize(static_cast<rstd::intptr_t>(slot.index.to_primitive())),
        .previous_lifetime_ok = true,
        .position             = position,
    };
}

auto ParticleSubSystem::HasBoundInstance(particle::ParticleInstance* parent,
                                         usize                       parent_instance_index,
                                         particle::ParticleSlot      slot) const -> bool {
    for (const auto& state : m_instance_states) {
        const auto& bounded = state.bounded;
        if (! state.death && bounded.parent == parent &&
            bounded.parent_instance_index == parent_instance_index &&
            bounded.particle_index ==
                isize(static_cast<rstd::intptr_t>(slot.index.to_primitive()))) {
            return true;
        }
    }
    return false;
}

void ParticleSubSystem::ReleaseBoundInstances(particle::ParticleInstance* parent,
                                              usize                       parent_instance_index,
                                              particle::ParticleSlot      slot) {
    for (usize index {}; index < System().InstanceCount(); ++index) {
        auto& state   = m_instance_states[index];
        auto& bounded = state.bounded;
        if (bounded.parent != parent || bounded.parent_instance_index != parent_instance_index ||
            bounded.particle_index !=
                isize(static_cast<rstd::intptr_t>(slot.index.to_primitive()))) {
            continue;
        }
        state.death = true;
        if (m_spawn_type == SpawnType::EVENT_FOLLOW ||
            m_spawn_type == SpawnType::STATIC_CONTROLPOINT) {
            System().Instance(index).Storage().Clear();
            state.no_live_particle = true;
        }
    }
}

void ParticleSubSystem::ProcessChildEvents(particle::ParticleEventContext& context) {
    auto                frame = ParticleFrameFrom(context.frame);
    ParticleInstanceRef parent {
        .instance = rstd::addressof(System().Instance(frame->instance_index)),
        .state    = rstd::addressof(m_instance_states[frame->instance_index]),
        .index    = frame->instance_index,
    };

    if (context.phase == particle::ParticleEventPhase::BeforeEmit) {
        for (const auto& transition : context.events->transitions) {
            if (! transition.died) continue;
            m_pending_child_deaths.emplace_back(transition.slot);
            for (auto& child : m_children) {
                if (child->Type() != SpawnType::EVENT_DEATH) continue;
                SpawnChild(parent,
                           *child,
                           transition.slot,
                           FollowWorldPosition(*parent.instance, parent.index, transition.slot),
                           true);
            }
        }
        return;
    }

    auto spawned = [&](particle::ParticleSlot slot) {
        for (const auto& value : context.events->spawned) {
            if (value == slot) return true;
        }
        return false;
    };
    for (const auto& slot : m_pending_child_deaths) {
        bool replaced = spawned(slot);
        for (auto& child : m_children) {
            if (child->Type() == SpawnType::EVENT_FOLLOW && replaced) continue;
            if (child->Type() != SpawnType::EVENT_FOLLOW &&
                child->Type() != SpawnType::EVENT_SPAWN &&
                child->Type() != SpawnType::STATIC_CONTROLPOINT) {
                continue;
            }
            child->ReleaseBoundInstances(parent.instance, parent.index, slot);
        }
    }
    for (const auto& slot : context.events->spawned) {
        for (auto& child : m_children) {
            if (child->Type() == SpawnType::EVENT_FOLLOW &&
                child->HasBoundInstance(parent.instance, parent.index, slot)) {
                continue;
            }
            if (child->Type() != SpawnType::EVENT_FOLLOW &&
                child->Type() != SpawnType::EVENT_SPAWN &&
                child->Type() != SpawnType::STATIC_CONTROLPOINT) {
                continue;
            }
            SpawnChild(parent, *child, slot);
        }
    }
    m_pending_child_deaths.clear();
}

void ParticleRuntime::Update(ref<SceneFrame> frame) { Tick(frame->delta); }

void ParticleRuntime::Tick(f64 delta) {
    for (auto& subsystem : m_subsystems) {
        subsystem->Tick(delta);
    }
}
