module wescene.pkg.parse;

import eigen;
import rstd;
import rstd.cppstd;
import wescene.core;
import wescene.particle;
import wescene.particle.program;
import wescene.utils;

using namespace rstd::prelude;
using namespace owe;

namespace
{

auto EmitCount(f64 timer, float speed) -> u32 {
    if (speed <= 0.0f) return u32();
    if (! f32(speed).is_finite()) return u32::MAX;

    const double elapsed  = timer.to_primitive();
    const double duration = static_cast<double>(1.0f / speed);
    if (elapsed < duration) return u32();

    const double count = f64(elapsed / duration).floor().to_primitive();
    if (count >= static_cast<double>(u32::MAX.to_primitive())) return u32::MAX;
    return u32(static_cast<rstd::uint32_t>(count));
}

auto ResolveEmitCount(f64 timer, float speed, u32 instantaneous, bool one_per_frame, bool empty)
    -> u32 {
    if (instantaneous > u32() && empty) return instantaneous;
    if (speed <= 0.0f) return u32();
    auto count = EmitCount(timer, speed);
    return one_per_frame && count > u32(1) ? u32(1) : count;
}

void CommitEmitCount(f64& timer, float speed, u32 requested, u32 emitted, bool one_per_frame) {
    if (requested == u32() || speed <= 0.0f) return;
    if (! f32(speed).is_finite()) {
        timer = f64();
        return;
    }

    const double duration  = static_cast<double>(1.0f / speed);
    double       remaining = rstd::cmp::max(
        timer.to_primitive() - duration * static_cast<double>(emitted.to_primitive()), 0.0);
    if (emitted < requested) {
        remaining = rstd::cmp::min(duration, remaining);
    } else if (one_per_frame) {
        remaining = (f64(remaining) % f64(duration)).to_primitive();
    }
    timer = f64(remaining);
}

auto EmitDuration(float speed) noexcept -> f64 {
    return speed > 0.0f ? f64(static_cast<double>(1.0f / speed)) : f64();
}

auto AudioResponseScale(slice<float> audio, const ParticleAudioResponse& response) -> float {
    if (! response.enable || audio.is_empty()) return 1.0f;

    auto clamp_index = [audio](float value) {
        auto index = static_cast<rstd::int32_t>(f32(value).round().to_primitive());
        index      = rstd::cmp::max<rstd::int32_t>(
            rstd::cmp::min(static_cast<rstd::int32_t>(audio.len().to_primitive()) - 1, index), 0);
        return usize(static_cast<rstd::size_t>(index));
    };
    auto first = clamp_index(response.frequency[usize()]);
    auto last  = clamp_index(response.frequency[usize(1)]);
    if (last < first) rstd::swap(first, last);

    float sum = 0.0f;
    for (auto index = first; index <= last; ++index) sum += rstd::cmp::max(audio[index], 0.0f);
    float level = sum / static_cast<float>((last - first + usize(1)).to_primitive());
    float low   = rstd::cmp::min(response.bounds[usize(1)], response.bounds[usize()]);
    float high  = rstd::cmp::max(response.bounds[usize(1)], response.bounds[usize()]);
    if (high > low) level = (level - low) / (high - low);
    level = rstd::cmp::min(1.0f, rstd::cmp::max(0.0f, level));
    level = f32(level).powf(f32(rstd::cmp::max(response.exponent, 0.001f))).to_primitive();
    return rstd::cmp::max(1.0f + level * response.amount, 0.0f);
}

auto ResolveEmitterOrigin(slice<ParticleControlpoint> controlpoints, i32 controlpoint,
                          const rstd::array<float, 3>& authored) -> Eigen::Vector3d {
    Eigen::Vector3d origin {
        static_cast<double>(authored[usize()]),
        static_cast<double>(authored[usize(1)]),
        static_cast<double>(authored[usize(2)]),
    };
    if (controlpoint >= i32() && rstd::as_cast<usize>(controlpoint) < controlpoints.len()) {
        origin += controlpoints[rstd::as_cast<usize>(controlpoint)].offset;
    }
    return origin;
}

void ApplySign(Eigen::Vector3d& value, i32 x, i32 y, i32 z) noexcept {
    if (x != i32())
        value.x() = f64(value.x()).abs().to_primitive() * static_cast<double>(x.to_primitive());
    if (y != i32())
        value.y() = f64(value.y()).abs().to_primitive() * static_cast<double>(y.to_primitive());
    if (z != i32())
        value.z() = f64(value.z()).abs().to_primitive() * static_cast<double>(z.to_primitive());
}

auto ActiveAxisCount(const Eigen::Vector3d& directions) noexcept -> u32 {
    u32 count {};
    for (usize index {}; index < usize(3); ++index) {
        if (f64(directions[index.to_primitive()]).abs().to_primitive() > 1e-6) ++count;
    }
    return rstd::cmp::max(u32(1), count);
}

auto RandomRadius(double min_distance, double max_distance, u32 dimensions) -> double {
    min_distance = rstd::cmp::max(min_distance, 0.0);
    max_distance = rstd::cmp::max(max_distance, min_distance);
    if (dimensions <= u32(1)) {
        return algorism::lerp(Random::get(0.0, 1.0), min_distance, max_distance);
    }

    double dimension = static_cast<double>(dimensions.to_primitive());
    double low       = f64(min_distance).powf(f64(dimension)).to_primitive();
    double high      = f64(max_distance).powf(f64(dimension)).to_primitive();
    return f64(algorism::lerp(Random::get(0.0, 1.0), low, high))
        .powf(f64(1.0 / dimension))
        .to_primitive();
}

auto RandomDirectedUnit(const Eigen::Vector3d& directions) -> Eigen::Vector3d {
    Eigen::Vector3d unit { 0.0, 0.0, 0.0 };
    for (usize retry {}; retry < usize(8); ++retry) {
        for (usize index {}; index < usize(3); ++index) {
            auto component  = index.to_primitive();
            unit[component] = f64(directions[component]).abs().to_primitive() > 1e-6
                                  ? Random::get<std::normal_distribution<>>(0.0, 1.0)
                                  : 0.0;
        }
        double norm = unit.norm();
        if (norm > 1e-6) return unit / norm;
    }
    return { 1.0, 0.0, 0.0 };
}

bool InstanceCanEmit(ref<ParticleFrame> frame) {
    return ! frame->subsystem->InstanceState(frame->instance_index).death;
}

} // namespace

void BoxEmitterProgram::Compile(particle::ParticleViewCompiler& compiler) {
    m_pipeline->Compile(compiler);
}

void BoxEmitterProgram::Emit(particle::ParticleEmitterContext& context) {
    auto frame = ParticleFrameFrom(context.Frame());
    if (! InstanceCanEmit(frame)) return;

    auto pending_count = m_index == usize() ? frame->subsystem->TakePendingEmitCount() : u32();

    auto& emitter = frame->subsystem->InstanceStateMut(frame->instance_index).Emitter(m_index);
    emitter.elapsed += frame->emitter_delta;
    if (m_args.duration > 0.0f && emitter.elapsed > f64(m_args.duration) && pending_count == u32())
        return;
    emitter.timer += frame->emitter_delta;

    auto  controlpoints = frame->subsystem->Controlpoints();
    auto  origin        = ResolveEmitterOrigin(controlpoints, m_args.controlpoint, m_args.origin);
    float emit_speed = m_args.emit_speed *
                       AudioResponseScale(frame->audio_average.as_slice(), m_args.audio_response);
    auto  timed_emit_count = ResolveEmitCount(
        emitter.timer, emit_speed, m_args.instantaneous, m_args.one_per_frame, context.Empty());
    auto emit_count = timed_emit_count.saturating_add(pending_count);
    auto requests   = context.Acquire(rstd::as_cast<usize>(emit_count), EmitDuration(emit_speed));
    auto columns    = m_pipeline->Bind(context.View());
    for (auto request : requests) {
        Eigen::Vector3d position;
        for (usize component {}; component < usize(3); ++component) {
            position[component.to_primitive()] = algorism::lerp(Random::get(-1.0, 1.0),
                                                                m_args.min_distance[component],
                                                                m_args.max_distance[component]);
        }
        position =
            position.cwiseProduct(Eigen::Vector3f { m_args.directions.data() }.cast<double>());
        columns.positions[request.slot.index] = position.cast<float>();
        columns.positions[request.slot.index] =
            (columns.positions[request.slot.index].cast<double>() + origin).cast<float>();
        double speed = Random::get(m_args.min_speed, m_args.max_speed);
        if (speed != 0.0 && position.squaredNorm() > 1e-12) {
            columns.velocities[request.slot.index] =
                (columns.velocities[request.slot.index].cast<double>() +
                 speed * position.normalized())
                    .cast<float>();
        }
        m_pipeline->Initialize(columns, request, context.Frame());
    }
    auto emitted       = rstd::as_cast<u32>(requests.len());
    auto timed_emitted = emitted.saturating_sub(pending_count);
    CommitEmitCount(
        emitter.timer, emit_speed, timed_emit_count, timed_emitted, m_args.one_per_frame);
}

void SphereEmitterProgram::Compile(particle::ParticleViewCompiler& compiler) {
    m_pipeline->Compile(compiler);
}

void SphereEmitterProgram::Emit(particle::ParticleEmitterContext& context) {
    auto frame = ParticleFrameFrom(context.Frame());
    if (! InstanceCanEmit(frame)) return;

    auto pending_count = m_index == usize() ? frame->subsystem->TakePendingEmitCount() : u32();

    auto& emitter = frame->subsystem->InstanceStateMut(frame->instance_index).Emitter(m_index);
    emitter.elapsed += frame->emitter_delta;
    if (m_args.duration > 0.0f && emitter.elapsed > f64(m_args.duration) && pending_count == u32())
        return;
    emitter.timer += frame->emitter_delta;

    auto controlpoints = frame->subsystem->Controlpoints();
    auto origin        = ResolveEmitterOrigin(controlpoints, m_args.controlpoint, m_args.origin);
    Eigen::Vector3d directions = Eigen::Vector3f { m_args.directions.data() }.cast<double>();
    auto            dimensions = ActiveAxisCount(directions);
    float emit_speed = m_args.emit_speed *
                       AudioResponseScale(frame->audio_average.as_slice(), m_args.audio_response);
    auto  timed_emit_count = ResolveEmitCount(
        emitter.timer, emit_speed, m_args.instantaneous, m_args.one_per_frame, context.Empty());
    auto emit_count = timed_emit_count.saturating_add(pending_count);
    auto requests   = context.Acquire(rstd::as_cast<usize>(emit_count), EmitDuration(emit_speed));
    auto columns    = m_pipeline->Bind(context.View());
    for (auto request : requests) {
        double          radius = RandomRadius(m_args.min_distance, m_args.max_distance, dimensions);
        Eigen::Vector3d unit   = RandomDirectedUnit(directions);
        Eigen::Vector3d position = radius * unit.cwiseProduct(directions.cwiseAbs());
        ApplySign(position, m_args.sign[usize()], m_args.sign[usize(1)], m_args.sign[usize(2)]);

        columns.positions[request.slot.index] = position.cast<float>();
        columns.positions[request.slot.index] =
            (columns.positions[request.slot.index].cast<double>() + origin).cast<float>();
        double speed = Random::get(m_args.min_speed, m_args.max_speed);
        if (speed != 0.0 && position.squaredNorm() > 1e-12) {
            columns.velocities[request.slot.index] =
                (columns.velocities[request.slot.index].cast<double>() +
                 speed * position.normalized())
                    .cast<float>();
        }
        m_pipeline->Initialize(columns, request, context.Frame());
    }
    auto emitted       = rstd::as_cast<u32>(requests.len());
    auto timed_emitted = emitted.saturating_sub(pending_count);
    CommitEmitCount(
        emitter.timer, emit_speed, timed_emit_count, timed_emitted, m_args.one_per_frame);
}
