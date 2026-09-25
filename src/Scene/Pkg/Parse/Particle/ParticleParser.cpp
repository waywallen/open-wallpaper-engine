module;

#include <rstd/enum.hpp>

module wescene.pkg.parse;
import eigen;
import wescene.core;
import rstd.log;
import wescene.utils;
import wescene.scene;
import wescene.particle;
import wescene.particle.program;

using namespace rstd::prelude;
using namespace owe;
using namespace Eigen;
using namespace rstd::literals;
using rstd::mtp::same;

namespace
{

constexpr float  kTau   = f32::consts::TAU.to_primitive();
constexpr double kTau64 = f64::consts::TAU.to_primitive();

inline Vector3d GenRandomVec3(const array<float, 3>& min, const array<float, 3>& max) {
    Vector3d result(3);
    for (int32_t i = 0; i < 3; i++) {
        result[i] = Random::get(min[usize(i)], max[usize(i)]);
    }
    return result;
}

inline float GenRandom(float min, float max, float exponent) {
    auto random = Random::get(0.0f, 1.0f);
    if (exponent != 1.0f) random = f32(random).powf(f32(exponent)).to_primitive();
    return static_cast<float>(algorism::lerp(random, min, max));
}

enum class SequenceLimitBehavior
{
    Repeat,
    Mirror,
    Clamp,
};

auto ParseSequenceLimitBehavior(const Json& json) -> SequenceLimitBehavior {
    String value { "repeat"_Str };
    owe::GetJsonValue(json, "limitbehavior"_str, value, false);
    if (value == "mirror"_str) return SequenceLimitBehavior::Mirror;
    if (value == "clamp"_str) return SequenceLimitBehavior::Clamp;
    return SequenceLimitBehavior::Repeat;
}

auto SequenceIndex(u64 sequence, u64 count, SequenceLimitBehavior behavior) -> u64 {
    if (count <= u64(1)) return u64();
    if (behavior == SequenceLimitBehavior::Clamp) return rstd::cmp::min(sequence, count - u64(1));
    if (behavior == SequenceLimitBehavior::Mirror) {
        auto period = (count - u64(1)) * u64(2);
        auto value  = sequence % period;
        return value < count ? value : period - value;
    }
    return sequence % count;
}

template<typename States>
auto SpawnSequence(const States& states, particle::ParticleSlot slot) -> u64 {
    return states[slot.index].spawn_sequence;
}

auto SequenceBasis(const Eigen::Vector3d& axis) -> Eigen::Vector3d {
    if (f64(axis.z()).abs().to_primitive() > 0.5) return Eigen::Vector3d { 0.0, 1.0, 0.0 };
    return Eigen::Vector3d { 1.0, 0.0, 0.0 };
}

struct MapSequenceAroundControlPoint {
    i32                   controlpoint {};
    float                 count { 1.0f };
    array<float, 2>       bounds { 0.0f, 1.0f };
    array<float, 3>       axis { 0.0f, 0.0f, 1.0f };
    array<float, 3>       speed_min { 0.0f, 0.0f, 0.0f };
    array<float, 3>       speed_max { 0.0f, 0.0f, 0.0f };
    SequenceLimitBehavior limit_behavior { SequenceLimitBehavior::Repeat };

    static auto ReadFromJson(const Json& json) -> MapSequenceAroundControlPoint {
        MapSequenceAroundControlPoint value;
        owe::GetJsonValue(json, "controlpoint"_str, value.controlpoint, false);
        owe::GetJsonValue(json, "count"_str, value.count, false);
        owe::GetJsonValue(json, "bounds"_str, value.bounds, false);
        owe::GetJsonValue(json, "axis"_str, value.axis, false);
        owe::GetJsonValue(json, "speedmin"_str, value.speed_min, false);
        owe::GetJsonValue(json, "speedmax"_str, value.speed_max, false);
        value.limit_behavior = ParseSequenceLimitBehavior(json);
        return value;
    }
};

struct MapSequenceAroundControlPointProgram {
    MapSequenceAroundControlPoint config;

    void Initialize(ParticleSpawnColumns& columns, particle::ParticleSpawnRequest request,
                    ref<dyn<rstd::any::Any>> frame_context) {
        auto frame         = ParticleFrameFrom(frame_context);
        auto controlpoints = frame->subsystem->Controlpoints();
        auto controlpoint  = rstd::as_cast<usize>(config.controlpoint % i32(8));
        auto center        = controlpoints[controlpoint].offset;

        Eigen::Vector3d axis { Eigen::Vector3f { config.axis.data() }.cast<double>() };
        if (axis.squaredNorm() <= 1e-12) axis = Eigen::Vector3d { 0.0, 0.0, 1.0 };
        axis.normalize();
        auto basis    = SequenceBasis(axis);
        basis         = (basis - axis * basis.dot(axis)).normalized();
        auto tangent  = basis.cross(axis).normalized();
        auto slot     = request.slot;
        auto sequence = SpawnSequence(columns.states, slot);
        auto relative = columns.positions[slot.index].cast<double>() - center;
        auto parallel = axis * relative.dot(axis);
        auto radius   = (relative - parallel).norm();
        auto angle    = kTau64 * static_cast<double>(sequence.to_primitive()) /
                        rstd::cmp::max(static_cast<double>(config.count), 1e-6);
        angle *= static_cast<double>(config.bounds[usize(1)] - config.bounds[usize(0)]);
        angle += kTau64 * static_cast<double>(config.bounds[usize(0)]);
        columns.positions[slot.index] = (center + parallel +
                                         radius * (f64(angle).cos().to_primitive() * basis +
                                                   f64(angle).sin().to_primitive() * tangent))
                                            .cast<float>();

        auto velocity = GenRandomVec3(config.speed_min, config.speed_max);
        if (velocity.squaredNorm() > 1e-12) {
            columns.velocities[slot.index] = (columns.velocities[slot.index].cast<double>() +
                                              Eigen::AngleAxisd(-angle, axis) * velocity)
                                                 .cast<float>();
        }
    }
};

struct MapSequenceBetweenControlPoints {
    i32                   controlpoint_start {};
    i32                   controlpoint_end { 1 };
    u32                   count { 2 };
    SequenceLimitBehavior limit_behavior { SequenceLimitBehavior::Repeat };

    static auto ReadFromJson(const Json& json, u32 implicit_count)
        -> MapSequenceBetweenControlPoints {
        MapSequenceBetweenControlPoints value;
        value.count = rstd::cmp::max(implicit_count, u32(2));
        owe::GetJsonValue(json, "controlpointstart"_str, value.controlpoint_start, false);
        owe::GetJsonValue(json, "controlpointend"_str, value.controlpoint_end, false);
        if (json.get("count"_str).is_some()) {
            owe::GetJsonValue(json, "count"_str, value.count, false);
            value.count = rstd::cmp::max(value.count, u32(2));
        }
        value.limit_behavior = ParseSequenceLimitBehavior(json);
        return value;
    }
};

struct MapSequenceBetweenControlPointsProgram {
    MapSequenceBetweenControlPoints config;

    void Initialize(ParticleSpawnColumns& columns, particle::ParticleSpawnRequest request,
                    ref<dyn<rstd::any::Any>> frame_context) {
        auto frame         = ParticleFrameFrom(frame_context);
        auto controlpoints = frame->subsystem->Controlpoints();
        auto start_index   = rstd::as_cast<usize>(config.controlpoint_start % i32(8));
        auto end_index     = rstd::as_cast<usize>(config.controlpoint_end % i32(8));
        auto start         = controlpoints[start_index].offset;
        auto end           = controlpoints[end_index].offset;
        auto path          = end - start;
        auto count         = rstd::as_cast<u64>(config.count);
        auto sequence      = SpawnSequence(columns.states, request.slot);
        auto index         = SequenceIndex(sequence, count, config.limit_behavior);
        auto amount        = static_cast<double>(index.to_primitive()) /
                             static_cast<double>((count - u64(1)).to_primitive());

        Eigen::Vector3d relative = columns.positions[request.slot.index].cast<double>() - start;
        if (path.squaredNorm() > 1e-12) {
            auto direction = path.normalized();
            relative -= direction * relative.dot(direction);
        }
        columns.positions[request.slot.index] = (start + amount * path + relative).cast<float>();
    }
};

inline float UiColorToLinear(float value) { return value * value; }

inline float UiScalarToLinear(float value) { return value; }

} // namespace

struct SingleRandom {
    float       min { 0.0f };
    float       max { 0.0f };
    float       exponent { 1.0f };
    static void ReadFromJson(const Json& j, SingleRandom& r) {
        owe::GetJsonValue(j, "min"_str, r.min, false);
        owe::GetJsonValue(j, "max"_str, r.max, false);
        owe::GetJsonValue(j, "exponent"_str, r.exponent, false);
    };
};
struct VecRandom {
    array<float, 3> min { 0.0f, 0.0f, 0.0f };
    array<float, 3> max { 0.0f, 0.0f, 0.0f };
    float           exponent { 1.0f };

    static void ReadFromJson(const Json& j, VecRandom& r) {
        owe::GetJsonValue(j, "min"_str, r.min, false);
        owe::GetJsonValue(j, "max"_str, r.max, false);
        owe::GetJsonValue(j, "exponent"_str, r.exponent, false);
    };
};
struct TurbulentRandom {
    float  scale { 1.0f };
    double timescale { 1.0f };
    float  offset { 0.0f };
    float  speedmin { 100.0f };
    float  speedmax { 250.0f };
    float  phasemin { 0.0f };
    float  phasemax { 0.1f };

    array<float, 3> forward { 0.0f, 1.0f, 0.0f };
    array<float, 3> normal { 0.0f, 0.0f, 1.0f };

    static void ReadFromJson(const Json& j, TurbulentRandom& r) {
        owe::GetJsonValue(j, "scale"_str, r.scale, false);
        owe::GetJsonValue(j, "timescale"_str, r.timescale, false);
        owe::GetJsonValue(j, "offset"_str, r.offset, false);
        owe::GetJsonValue(j, "speedmin"_str, r.speedmin, false);
        owe::GetJsonValue(j, "speedmax"_str, r.speedmax, false);
        owe::GetJsonValue(j, "phasemin"_str, r.phasemin, false);
        owe::GetJsonValue(j, "phasemax"_str, r.phasemax, false);
        owe::GetJsonValue(j, "forward"_str, r.forward, false);
        owe::GetJsonValue(j, "right"_str, r.normal, false);
        owe::GetJsonValue(j, "normal"_str, r.normal, false);
    };
};
struct NoopSpawnProgram {
    void Initialize(ParticleSpawnColumns&, particle::ParticleSpawnRequest,
                    ref<dyn<rstd::any::Any>>) {}
};

struct ColorRandomProgram {
    array<float, 3> min;
    array<float, 3> max;

    void Initialize(ParticleSpawnColumns& columns, particle::ParticleSpawnRequest request,
                    ref<dyn<rstd::any::Any>>) {
        auto            random = Random::get(0.0, 1.0);
        Eigen::Vector3f value;
        for (usize component {}; component < usize(3); ++component) {
            auto raw   = component.to_primitive();
            value[raw] = static_cast<float>(algorism::lerp(random, min[component], max[component]));
        }
        columns.colors[request.slot.index]         = value;
        columns.initial_colors[request.slot.index] = value;
    }
};

struct LifetimeRandomProgram {
    SingleRandom config;

    void Initialize(ParticleSpawnColumns& columns, particle::ParticleSpawnRequest request,
                    ref<dyn<rstd::any::Any>>) {
        auto value                            = GenRandom(config.min, config.max, config.exponent);
        columns.lifetimes[request.slot.index] = value;
        columns.initial_lifetimes[request.slot.index] = value;
    }
};

struct SizeRandomProgram {
    SingleRandom config;

    void Initialize(ParticleSpawnColumns& columns, particle::ParticleSpawnRequest request,
                    ref<dyn<rstd::any::Any>>) {
        auto value                        = GenRandom(config.min, config.max, config.exponent);
        columns.sizes[request.slot.index] = value;
        columns.initial_sizes[request.slot.index] = value;
    }
};

struct AlphaRandomProgram {
    SingleRandom config;

    void Initialize(ParticleSpawnColumns& columns, particle::ParticleSpawnRequest request,
                    ref<dyn<rstd::any::Any>>) {
        auto value                         = GenRandom(config.min, config.max, config.exponent);
        columns.alphas[request.slot.index] = value;
        columns.initial_alphas[request.slot.index] = value;
    }
};

struct VectorRandomProgram {
    enum class Target
    {
        Velocity,
        Rotation,
        AngularVelocity,
    };

    VecRandom config;
    Target    target { Target::Velocity };

    void Initialize(ParticleSpawnColumns& columns, particle::ParticleSpawnRequest request,
                    ref<dyn<rstd::any::Any>>) {
        Eigen::Vector3f value;
        for (usize component {}; component < usize(3); ++component) {
            auto raw   = component.to_primitive();
            value[raw] = GenRandom(config.min[component], config.max[component], config.exponent);
        }
        if (target == Target::Velocity) {
            columns.velocities[request.slot.index] += value;
        } else if (target == Target::Rotation) {
            columns.rotations[request.slot.index] += value;
        } else {
            columns.angular_velocities[request.slot.index] += value;
        }
    }
};

struct TurbulentVelocityRandomProgram {
    TurbulentRandom config;
    Eigen::Vector3f normal;
    Eigen::Vector3f forward;
    Eigen::Vector3f position;

    void Initialize(ParticleSpawnColumns& columns, particle::ParticleSpawnRequest request,
                    ref<dyn<rstd::any::Any>>) {
        auto  duration = request.emitter_duration;
        float speed    = Random::get(config.speedmin, config.speedmax);
        float phase    = Random::get(config.phasemin, config.phasemax);
        if (duration > f64(10.0)) {
            position[0] += speed;
            duration = f64();
        }
        Eigen::Vector3f result;
        do {
            result = algorism::CurlNoise((position + normal * phase).cast<double>()).cast<float>();
            result -= normal * result.dot(normal);
            if (result.squaredNorm() <= 1e-8f) result = forward;
            result.normalize();
            position += result * 0.005f * static_cast<float>(config.timescale);
            duration -= f64(0.01);
        } while (duration > f64(0.01));

        auto cosine = rstd::cmp::min(1.0f, rstd::cmp::max(-1.0f, result.dot(forward)));
        auto angle  = static_cast<float>(
            f32(normal.dot(forward.cross(result))).atan2(f32(cosine)).to_primitive());
        auto scale = rstd::cmp::max(config.scale * 0.5f, 0.0f);
        result     = Eigen::AngleAxisf(angle * scale + config.offset, normal) * forward;
        columns.velocities[request.slot.index] += result * speed;
    }
};

struct OverrideSpawnProgram {
    ParticleInstanceModifiers modifiers;

    void Initialize(ParticleSpawnColumns& columns, particle::ParticleSpawnRequest request,
                    ref<dyn<rstd::any::Any>>) {
        auto index = request.slot.index;
        columns.lifetimes[index] *= modifiers.Lifetime();
        columns.initial_lifetimes[index] = columns.lifetimes[index];
        columns.alphas[index] *= UiScalarToLinear(modifiers.Alpha());
        columns.initial_alphas[index] = columns.alphas[index];
        columns.sizes[index] *= modifiers.Size();
        columns.initial_sizes[index] = columns.sizes[index];
        columns.velocities[index] *= modifiers.Speed();
        columns.angular_velocities[index] *= modifiers.Speed();
        if (modifiers.HasColorOverride()) {
            auto            color = modifiers.Color();
            Eigen::Vector3f value { color[usize(0)], color[usize(1)], color[usize(2)] };
            if (modifiers.UsesLegacyColor()) value /= 255.0f;
            value                         = { UiColorToLinear(value[0]),
                                              UiColorToLinear(value[1]),
                                              UiColorToLinear(value[2]) };
            columns.colors[index]         = value;
            columns.initial_colors[index] = value;
        }
    }
};

class ParticleSpawnInstructionValue {
    RSTD_ENUM(ParticleSpawnInstructionValue, (Noop, (NoopSpawnProgram value;)),
              (ColorRandom, (ColorRandomProgram value;)),
              (LifetimeRandom, (LifetimeRandomProgram value;)),
              (SizeRandom, (SizeRandomProgram value;)), (AlphaRandom, (AlphaRandomProgram value;)),
              (VectorRandom, (VectorRandomProgram value;)),
              (TurbulentVelocityRandom, (TurbulentVelocityRandomProgram value;)),
              (MapSequenceAroundControlPoint, (MapSequenceAroundControlPointProgram value;)),
              (MapSequenceBetweenControlPoints, (MapSequenceBetweenControlPointsProgram value;)),
              (Override, (OverrideSpawnProgram value;)))
};

template<typename T>
auto MakeParticleSpawnInstructionValue(T value) -> ParticleSpawnInstructionValue {
    if constexpr (same<T, NoopSpawnProgram>)
        return ParticleSpawnInstructionValue::Noop(rstd::move(value));
    else if constexpr (same<T, ColorRandomProgram>)
        return ParticleSpawnInstructionValue::ColorRandom(rstd::move(value));
    else if constexpr (same<T, LifetimeRandomProgram>)
        return ParticleSpawnInstructionValue::LifetimeRandom(rstd::move(value));
    else if constexpr (same<T, SizeRandomProgram>)
        return ParticleSpawnInstructionValue::SizeRandom(rstd::move(value));
    else if constexpr (same<T, AlphaRandomProgram>)
        return ParticleSpawnInstructionValue::AlphaRandom(rstd::move(value));
    else if constexpr (same<T, VectorRandomProgram>)
        return ParticleSpawnInstructionValue::VectorRandom(rstd::move(value));
    else if constexpr (same<T, TurbulentVelocityRandomProgram>)
        return ParticleSpawnInstructionValue::TurbulentVelocityRandom(rstd::move(value));
    else if constexpr (same<T, MapSequenceAroundControlPointProgram>)
        return ParticleSpawnInstructionValue::MapSequenceAroundControlPoint(rstd::move(value));
    else if constexpr (same<T, MapSequenceBetweenControlPointsProgram>)
        return ParticleSpawnInstructionValue::MapSequenceBetweenControlPoints(rstd::move(value));
    else {
        static_assert(same<T, OverrideSpawnProgram>);
        return ParticleSpawnInstructionValue::Override(rstd::move(value));
    }
}

struct ParticleSpawnInstruction::Impl {
    template<typename T>
    explicit Impl(T value): value(MakeParticleSpawnInstructionValue(rstd::move(value))) {}

    ParticleSpawnInstructionValue value;
};

ParticleSpawnInstruction::ParticleSpawnInstruction(Box<Impl> impl): m_impl(rstd::move(impl)) {}
ParticleSpawnInstruction::ParticleSpawnInstruction(ParticleSpawnInstruction&&) noexcept = default;
auto ParticleSpawnInstruction::operator=(ParticleSpawnInstruction&&) noexcept
    -> ParticleSpawnInstruction&                      = default;
ParticleSpawnInstruction::~ParticleSpawnInstruction() = default;

auto ParticleSpawnInstruction::SequenceCount() const -> Option<u32> {
    RSTD_MATCH(m_impl->value) {
        RSTD_CASE(Noop) { return None(); }
        RSTD_CASE(ColorRandom) { return None(); }
        RSTD_CASE(LifetimeRandom) { return None(); }
        RSTD_CASE(SizeRandom) { return None(); }
        RSTD_CASE(AlphaRandom) { return None(); }
        RSTD_CASE(VectorRandom) { return None(); }
        RSTD_CASE(TurbulentVelocityRandom) { return None(); }
        RSTD_CASE(MapSequenceAroundControlPoint) { return None(); }
        RSTD_CASE(MapSequenceBetweenControlPoints, instruction) {
            return Some(rstd::as_cast<u32>(instruction.config.count));
        }
        RSTD_CASE(Override) { return None(); }
    }
    rstd::unreachable();
}

template<typename T>
auto ParticleSpawnInstruction::Make(T value) -> ParticleSpawnInstruction {
    return ParticleSpawnInstruction(Box<Impl>::make(rstd::move(value)));
}

void ParticleSpawnInstruction::Initialize(ParticleSpawnColumns&          columns,
                                          particle::ParticleSpawnRequest request,
                                          ref<dyn<rstd::any::Any>>       frame) {
    RSTD_MATCH(m_impl->value) {
        RSTD_CASE(Noop, instruction) { instruction.Initialize(columns, request, frame); }
        RSTD_CASE(ColorRandom, instruction) { instruction.Initialize(columns, request, frame); }
        RSTD_CASE(LifetimeRandom, instruction) { instruction.Initialize(columns, request, frame); }
        RSTD_CASE(SizeRandom, instruction) { instruction.Initialize(columns, request, frame); }
        RSTD_CASE(AlphaRandom, instruction) { instruction.Initialize(columns, request, frame); }
        RSTD_CASE(VectorRandom, instruction) { instruction.Initialize(columns, request, frame); }
        RSTD_CASE(TurbulentVelocityRandom, instruction) {
            instruction.Initialize(columns, request, frame);
        }
        RSTD_CASE(MapSequenceAroundControlPoint, instruction) {
            instruction.Initialize(columns, request, frame);
        }
        RSTD_CASE(MapSequenceBetweenControlPoints, instruction) {
            instruction.Initialize(columns, request, frame);
        }
        RSTD_CASE(Override, instruction) { instruction.Initialize(columns, request, frame); }
    }
}

ParticleSpawnInstruction ParticleParser::GenInitializer(const Json& wpj,
                                                        u32         implicit_sequence_count) {
    do {
        if (wpj.get("name"_str).is_none()) break;
        String name;
        owe::GetJsonValue(wpj, "name"_str, name);

        if (name == "colorrandom"_str) {
            VecRandom r;
            r.min = { 0.0f, 0.0f, 0.0f };
            r.max = { 255.0f, 255.0f, 255.0f };
            VecRandom::ReadFromJson(wpj, r);
            return ParticleSpawnInstruction::Make(ColorRandomProgram {
                .min = rstd::move(r.min).map([](float value) {
                    return value / 255.0f;
                }),
                .max = rstd::move(r.max).map([](float value) {
                    return value / 255.0f;
                }),
            });
        } else if (name == "lifetimerandom"_str) {
            SingleRandom r = { 0.0f, 1.0f };
            SingleRandom::ReadFromJson(wpj, r);
            return ParticleSpawnInstruction::Make(LifetimeRandomProgram { .config = r });
        } else if (name == "sizerandom"_str) {
            SingleRandom r = { 0.0f, 20.0f };
            SingleRandom::ReadFromJson(wpj, r);
            return ParticleSpawnInstruction::Make(SizeRandomProgram { .config = r });
        } else if (name == "alpharandom"_str) {
            SingleRandom r = { 0.05f, 1.0f };
            SingleRandom::ReadFromJson(wpj, r);
            return ParticleSpawnInstruction::Make(AlphaRandomProgram { .config = r });
        } else if (name == "velocityrandom"_str) {
            VecRandom r;
            r.min[usize(0)] = r.min[usize(1)] = -32.0f;
            r.max[usize(0)] = r.max[usize(1)] = 32.0f;
            VecRandom::ReadFromJson(wpj, r);
            return ParticleSpawnInstruction::Make(VectorRandomProgram {
                .config = r,
                .target = VectorRandomProgram::Target::Velocity,
            });
        } else if (name == "rotationrandom"_str) {
            VecRandom r;
            r.max[usize(2)] = kTau;
            VecRandom::ReadFromJson(wpj, r);
            return ParticleSpawnInstruction::Make(VectorRandomProgram {
                .config = r,
                .target = VectorRandomProgram::Target::Rotation,
            });
        } else if (name == "angularvelocityrandom"_str) {
            VecRandom r;
            r.min[usize(2)] = -5.0f;
            r.max[usize(2)] = 5.0f;
            VecRandom::ReadFromJson(wpj, r);
            return ParticleSpawnInstruction::Make(VectorRandomProgram {
                .config = r,
                .target = VectorRandomProgram::Target::AngularVelocity,
            });
        } else if (name == "turbulentvelocityrandom"_str) {
            TurbulentRandom r;
            TurbulentRandom::ReadFromJson(wpj, r);
            Vector3f normal(r.normal.data());
            if (normal.squaredNorm() <= 1e-8f) normal = Vector3f::UnitZ();
            normal.normalize();

            Vector3f forward(r.forward.data());
            forward -= normal * forward.dot(normal);
            if (forward.squaredNorm() <= 1e-8f) forward = normal.unitOrthogonal();
            forward.normalize();

            return ParticleSpawnInstruction::Make(TurbulentVelocityRandomProgram {
                .config  = r,
                .normal  = normal,
                .forward = forward,
                .position =
                    GenRandomVec3({ 0.0f, 0.0f, 0.0f }, { 10.0f, 10.0f, 10.0f }).cast<float>(),
            });
        } else if (name == "mapsequencearoundcontrolpoint"_str) {
            return ParticleSpawnInstruction::Make(MapSequenceAroundControlPointProgram {
                .config = MapSequenceAroundControlPoint::ReadFromJson(wpj),
            });
        } else if (name == "mapsequencebetweencontrolpoints"_str) {
            return ParticleSpawnInstruction::Make(MapSequenceBetweenControlPointsProgram {
                .config =
                    MapSequenceBetweenControlPoints::ReadFromJson(wpj, implicit_sequence_count),
            });
        }
    } while (false);
    return ParticleSpawnInstruction::Make(NoopSpawnProgram {});
}

ParticleSpawnInstruction ParticleParser::GenOverride(ParticleInstanceModifiers modifiers) {
    return ParticleSpawnInstruction::Make(
        OverrideSpawnProgram { .modifiers = rstd::move(modifiers) });
}
double FadeValueChange(float life, float start, float end, float startValue,
                       float endValue) noexcept {
    if (life <= start)
        return startValue;
    else if (life > end)
        return endValue;
    else {
        double pass = (life - start) / (end - start);
        return algorism::lerp(pass, startValue, endValue);
    }
}

struct ValueChange {
    float starttime { 0 };
    float endtime { 1.0f };
    float startvalue { 1.0f };
    float endvalue { 0.0f };

    static auto ReadFromJson(const Json& j) {
        ValueChange v;
        owe::GetJsonValue(j, "starttime"_str, v.starttime, false);
        owe::GetJsonValue(j, "endtime"_str, v.endtime, false);
        owe::GetJsonValue(j, "startvalue"_str, v.startvalue, false);
        owe::GetJsonValue(j, "endvalue"_str, v.endvalue, false);
        return v;
    }
};
double FadeValueChange(float life, const ValueChange& v) noexcept {
    return FadeValueChange(life, v.starttime, v.endtime, v.startvalue, v.endvalue);
}

struct VecChange {
    float           starttime { 0 };
    float           endtime { 1.0f };
    array<float, 3> startvalue { 1.0f, 1.0f, 1.0f };
    array<float, 3> endvalue { 0.0f, 0.0f, 0.0f };

    static auto ReadFromJson(const Json& j) {
        VecChange v;
        owe::GetJsonValue(j, "starttime"_str, v.starttime, false);
        owe::GetJsonValue(j, "endtime"_str, v.endtime, false);
        owe::GetJsonValue(j, "startvalue"_str, v.startvalue, false);
        owe::GetJsonValue(j, "endvalue"_str, v.endvalue, false);
        return v;
    }
};

struct FrequencyValue {
    array<float, 3> mask { 1.0f, 1.0f, 0.0f };

    float frequencymin { 0.0f };
    float frequencymax { 10.0f };
    float scalemin { 0.0f };
    float scalemax { 1.0f };
    float phasemin { 0.0f };
    float phasemax { kTau };

    static auto ReadFromJson(const Json& j, ref<str> name) {
        FrequencyValue v;
        if (name == "oscillatesize"_str) {
            v.scalemin = 0.8f;
            v.scalemax = 1.2f;
        } else if (name == "oscillateposition"_str) {
            v.frequencymax = 5.0f;
        }
        owe::GetJsonValue(j, "frequencymin"_str, v.frequencymin, false);
        owe::GetJsonValue(j, "frequencymax"_str, v.frequencymax, false);
        if (v.frequencymax == 0.0f) v.frequencymax = v.frequencymin;
        owe::GetJsonValue(j, "scalemin"_str, v.scalemin, false);
        owe::GetJsonValue(j, "scalemax"_str, v.scalemax, false);
        owe::GetJsonValue(j, "phasemin"_str, v.phasemin, false);
        owe::GetJsonValue(j, "phasemax"_str, v.phasemax, false);
        owe::GetJsonValue(j, "mask"_str, v.mask, false);
        return v;
    };
    inline void GenFrequency(bool lifetime_ok, OscillationStateRef st) {
        if (! lifetime_ok) st.reset = true;
        if (st.reset) {
            st.frequency = Random::get(frequencymin, frequencymax);
            st.scale     = Random::get(scalemin, scalemax);
            st.phase =
                static_cast<float>(Random::get(static_cast<double>(phasemin), phasemax + kTau64));
            st.reset = false;
        }
    }
    inline double GetScale(OscillationStateRef st, double time) {
        double f = st.frequency / kTau;
        double w = kTau * f;
        return algorism::lerp(
            (f64(w * time + st.phase).cos().to_primitive() + 1.0f) * 0.5f, scalemin, scalemax);
    }
    inline double GetMove(OscillationStateRef st, double time, f64 time_pass) {
        double f = st.frequency / kTau;
        double w = kTau * f;
        return -1.0f * st.scale * w * f64(w * time + st.phase).sin().to_primitive() *
               time_pass.to_primitive();
    }
};

struct Turbulence {
    // the minimum time offset of the noise field for a particle.
    float phasemin { 0 };
    // the maximum time offset of the noise field for a particle.
    float phasemax { 0 };
    // the minimum velocity applied to particles.
    float speedmin { 500.0f };
    // the maximum velocity applied to particles.
    float speedmax { 1000.0f };
    // how fast the noise field changes shape.
    float timescale { 20.0f };

    float scale { 0.01f };

    array<float, 3> mask { 1.0f, 1.0f, 0.0f };

    static auto ReadFromJson(const Json& j) {
        Turbulence v;
        owe::GetJsonValue(j, "phasemin"_str, v.phasemin, false);
        owe::GetJsonValue(j, "phasemax"_str, v.phasemax, false);
        owe::GetJsonValue(j, "speedmin"_str, v.speedmin, false);
        owe::GetJsonValue(j, "speedmax"_str, v.speedmax, false);
        owe::GetJsonValue(j, "timescale"_str, v.timescale, false);
        owe::GetJsonValue(j, "mask"_str, v.mask, false);
        owe::GetJsonValue(j, "scale"_str, v.scale, false);
        return v;
    };
};

struct Vortex {
    enum class FlagEnum
    {
        infinite_axis               = 0, // 1
        maintain_distance_to_center = 1, // 2
    };
    using EFlags = BitFlags<FlagEnum>;

    i32 controlpoint { 0 };

    // anything below this distance receives force multiplied with speed inner.
    float distanceinner { 500.0f };
    // anything above this distance receives force multiplied with speed outer.
    float distanceouter { 650.0f };
    // amount of force applied to inner ring.
    float speedinner { 2500.0f };
    // amount of force applied to outer ring.
    float speedouter { 0 };

    EFlags flags { 0 };

    // positional offset from the center of the control point.
    array<float, 3> offset { 0.0f, 0.0f, 0.0f };

    // the axis to rotate around.
    array<float, 3> axis { 0.0f, 0.0f, 1.0f };

    float ringradius {};
    float ringwidth {};
    float ringpulldistance {};

    static auto ReadFromJson(const Json& j) {
        Vortex v;
        owe::GetJsonValue(j, "controlpoint"_str, v.controlpoint, false);
        if (v.controlpoint >= i32(8)) rstd_error("wrong contropoint index {}", v.controlpoint);
        v.controlpoint %= i32(8);

        owe::GetJsonValue(j, "distanceinner"_str, v.distanceinner, false);
        owe::GetJsonValue(j, "distanceouter"_str, v.distanceouter, false);
        owe::GetJsonValue(j, "speedinner"_str, v.speedinner, false);
        owe::GetJsonValue(j, "speedouter"_str, v.speedouter, false);

        i32 _flags { 0 };
        owe::GetJsonValue(j, "flags"_str, _flags, false);
        v.flags = EFlags(static_cast<rstd::uint32_t>(_flags.to_primitive()));

        owe::GetJsonValue(j, "offset"_str, v.offset, false);
        owe::GetJsonValue(j, "axis"_str, v.axis, false);
        owe::GetJsonValue(j, "ringradius"_str, v.ringradius, false);
        owe::GetJsonValue(j, "ringwidth"_str, v.ringwidth, false);
        owe::GetJsonValue(j, "ringpulldistance"_str, v.ringpulldistance, false);

        return v;
    };
};

struct VortexFrame {
    Vector3d center { Vector3d::Zero() };
    Vector3d axis { Vector3d::UnitZ() };
};

auto ResolveVortexFrame(const Vortex& vortex, const ParticleSubSystem& subsystem) -> VortexFrame {
    auto controlpoint = subsystem.SimulationControlpoint(rstd::as_cast<usize>(vortex.controlpoint));
    Vector3d local_offset = Vector3f { vortex.offset.data() }.cast<double>();
    Vector3d axis         = Vector3f { vortex.axis.data() }.cast<double>();
    local_offset          = controlpoint.basis * local_offset;
    axis                  = controlpoint.basis * axis;
    if (axis.squaredNorm() <= 1e-12) axis = Vector3d::UnitZ();
    return {
        .center = controlpoint.center + local_offset,
        .axis   = axis.normalized(),
    };
}

auto VortexSpeedAtDistance(const Vortex& vortex, double distance) -> double {
    auto distance_range = static_cast<double>(vortex.distanceouter - vortex.distanceinner);
    if (distance_range < 0.0 || distance < vortex.distanceinner) return vortex.speedinner;
    if (distance > vortex.distanceouter) return vortex.speedouter;
    auto amount = (distance - vortex.distanceinner) / (distance_range + 0.1);
    return algorism::lerp(amount, vortex.speedinner, vortex.speedouter);
}

struct ControlPointForce {
    i32 controlpoint { 0 };
    u32 flags { 2 };

    // how strongly the control point attracts or repels.
    float scale { 512.0f };
    // the maximum distance between particle and control point where the force takes effect.
    float threshold { 512.0f };

    // positional offset from the center of the control point.
    array<float, 3> origin { 0.0f, 0.0f, 0.0f };

    static auto ReadFromJson(const Json& j) {
        ControlPointForce v;
        owe::GetJsonValue(j, "controlpoint"_str, v.controlpoint, false);
        if (v.controlpoint >= i32(8)) rstd_error("wrong contropoint index {}", v.controlpoint);
        v.controlpoint %= i32(8);

        owe::GetJsonValue(j, "scale"_str, v.scale, false);
        owe::GetJsonValue(j, "threshold"_str, v.threshold, false);
        owe::GetJsonValue(j, "flags"_str, v.flags, false);

        owe::GetJsonValue(j, "origin"_str, v.origin, false);
        return v;
    };
};

struct MaintainDistanceState {
    float distance {};
    bool  initialized { false };
};

class MaintainDistanceAttribute {
public:
    using Value = MaintainDistanceState;

    MaintainDistanceAttribute(particle::ParticleAttributeDescriptor descriptor, Value default_value)
        : m_storage(rstd::move(descriptor), rstd::move(default_value)) {}

    auto Descriptor() const -> ref<particle::ParticleAttributeDescriptor> {
        return m_storage.Descriptor();
    }
    auto ConcreteType() const noexcept -> rstd::any::TypeId { return m_storage.ConcreteType(); }
    auto ValueType() const noexcept -> rstd::any::TypeId { return m_storage.ValueTypeId(); }
    auto Len() const noexcept -> usize { return m_storage.Len(); }
    auto Capacity() const noexcept -> usize { return m_storage.Capacity(); }
    void Reserve(usize total_slots) { m_storage.Reserve(total_slots); }
    void AppendDefaults(usize count) { m_storage.AppendDefaults(count); }
    void ResetSlots(slice<particle::ParticleSlot> slots) { m_storage.ResetSlots(slots); }
    void Clear() { m_storage.Clear(); }
    auto Values() const noexcept -> slice<Value> { return m_storage.Values(); }
    auto ValuesMut() noexcept -> mut_ref<Value[]> { return m_storage.ValuesMut(); }
    auto CloneEmpty() const -> MaintainDistanceAttribute {
        return MaintainDistanceAttribute(m_storage.CloneDescriptor(), m_storage.DefaultValue());
    }

private:
    particle::ParticleValueAttributeStorage<Value> m_storage;
};

struct MaintainDistance {
    i32   controlpoint {};
    float variable_strength { 5.0f };

    static auto ReadFromJson(const Json& json) -> MaintainDistance {
        MaintainDistance value;
        owe::GetJsonValue(json, "controlpoint"_str, value.controlpoint, false);
        owe::GetJsonValue(json, "variablestrength"_str, value.variable_strength, false);
        value.controlpoint %= i32(8);
        return value;
    }
};

auto RegisterMaintainDistanceAttribute(ParticleSubSystem& subsystem, usize operator_index)
    -> particle::ParticleAttributeKey<MaintainDistanceAttribute> {
    auto name   = rstd::format("maintain_distance_{}", operator_index);
    auto result = subsystem.SchemaBuilder().Register<MaintainDistanceAttribute>(
        name.as_str(), "we.operator.maintain_distance"_str, MaintainDistanceState {});
    if (result.is_err()) rstd::panic { "failed to register maintain distance attribute" };
    return result.unwrap();
}

template<typename Attribute, typename Value>
auto RegisterOscillationAttribute(particle::ParticleSchemaBuilder& builder, ref<str> name,
                                  Value default_value)
    -> particle::ParticleAttributeKey<Attribute> {
    auto result = builder.Register<Attribute>(name,
                                              "we.operator.oscillation"_str,
                                              particle::ParticleAttributeResetPolicy::Custom,
                                              default_value);
    if (result.is_err()) rstd::panic { "failed to register oscillation attribute" };
    return result.unwrap();
}

auto RegisterOscillationAttributes(ParticleSubSystem& subsystem, usize operator_index,
                                   ref<str> suffix) -> OscillationAttributes {
    auto                  prefix  = rstd::format("oscillation_{}_{}", operator_index, suffix);
    auto&                 builder = subsystem.SchemaBuilder();
    OscillationAttributes attributes {
        .reset = RegisterOscillationAttribute<OscillationResetAttribute>(
            builder, rstd::format("{}_reset", prefix).as_str(), true),
        .frequency = RegisterOscillationAttribute<OscillationFrequencyAttribute>(
            builder, rstd::format("{}_frequency", prefix).as_str(), 0.0f),
        .scale = RegisterOscillationAttribute<OscillationScaleAttribute>(
            builder, rstd::format("{}_scale", prefix).as_str(), 1.0f),
        .phase = RegisterOscillationAttribute<OscillationPhaseAttribute>(
            builder, rstd::format("{}_phase", prefix).as_str(), 0.0f),
    };
    return attributes;
}

auto LifetimePosition(float lifetime, float initial_lifetime) noexcept -> double {
    if (lifetime < 0.0f) return 1.0;
    return 1.0 - static_cast<double>(lifetime / initial_lifetime);
}

auto LifetimePassed(float lifetime, float initial_lifetime) noexcept -> double {
    return static_cast<double>(initial_lifetime - lifetime);
}

struct MovementOperator {
    ParticleAttributes                                        attributes;
    float                                                     drag {};
    Eigen::Vector3d                                           gravity;
    ParticleInstanceModifiers                                 modifiers;
    particle::ParticleWriteIndex<particle::VelocityAttribute> velocity;

    void Compile(particle::ParticleViewCompiler& compiler) {
        compiler.WriteBase(attributes.position);
        velocity = compiler.Write(attributes.velocity);
    }

    void Update(particle::ParticleUpdateContext& context) {
        auto frame      = ParticleFrameFrom(context.frame);
        auto positions  = context.view.PositionsMut();
        auto velocities = context.view.Write(velocity);
        auto delta      = context.delta.to_primitive();
        auto speed      = modifiers.Speed();
        for (auto slot : context.slots) {
            auto world_velocity =
                frame->world_from_local_dir * velocities[slot.index].cast<double>();
            Eigen::Vector3d acceleration =
                frame->local_from_world_dir * algorism::DragForce(world_velocity, drag);
            acceleration +=
                speed * (frame->world_space ? frame->local_from_world_dir * gravity : gravity);
            velocities[slot.index] =
                (velocities[slot.index].cast<double>() + acceleration * delta).cast<float>();
            positions[slot.index] = (positions[slot.index].cast<double>() +
                                     velocities[slot.index].cast<double>() * delta)
                                        .cast<float>();
        }
    }
};

struct AngularMovementOperator {
    ParticleAttributes                                               attributes;
    float                                                            drag {};
    Eigen::Vector3d                                                  force;
    particle::ParticleWriteIndex<particle::RotationAttribute>        rotation;
    particle::ParticleWriteIndex<particle::AngularVelocityAttribute> angular_velocity;

    void Compile(particle::ParticleViewCompiler& compiler) {
        rotation         = compiler.Write(attributes.rotation);
        angular_velocity = compiler.Write(attributes.angular_velocity);
    }

    void Update(particle::ParticleUpdateContext& context) {
        auto rotations  = context.view.Write(rotation);
        auto velocities = context.view.Write(angular_velocity);
        auto delta      = context.delta.to_primitive();
        for (auto slot : context.slots) {
            Eigen::Vector3d acceleration =
                algorism::DragForce(rotations[slot.index].cast<double>(), drag) + force;
            velocities[slot.index] =
                (velocities[slot.index].cast<double>() + acceleration * delta).cast<float>();
            rotations[slot.index] = (rotations[slot.index].cast<double>() +
                                     velocities[slot.index].cast<double>() * delta)
                                        .cast<float>();
        }
    }
};

struct ScalarChangeOperator {
    enum class Target
    {
        Size,
        Alpha,
    };

    ParticleAttributes                                              attributes;
    ValueChange                                                     change;
    Target                                                          target { Target::Alpha };
    ParticleInstanceModifiers                                       modifiers;
    particle::ParticleWriteIndex<particle::SizeAttribute>           size;
    particle::ParticleWriteIndex<particle::AlphaAttribute>          alpha;
    particle::ParticleReadIndex<particle::LifetimeAttribute>        lifetime;
    particle::ParticleReadIndex<particle::InitialLifetimeAttribute> initial_lifetime;

    void Compile(particle::ParticleViewCompiler& compiler) {
        if (target == Target::Size)
            size = compiler.Write(attributes.size);
        else
            alpha = compiler.Write(attributes.alpha);
        lifetime         = compiler.Read(attributes.lifetime);
        initial_lifetime = compiler.Read(attributes.initial_lifetime);
    }

    void Update(particle::ParticleUpdateContext& context) {
        auto lifetimes = context.view.Read(lifetime);
        auto initial   = context.view.Read(initial_lifetime);
        if (target == Target::Size) {
            auto values = context.view.Write(size);
            auto scale  = modifiers.Size();
            for (auto slot : context.slots) {
                values[slot.index] *=
                    scale *
                    FadeValueChange(LifetimePosition(lifetimes[slot.index], initial[slot.index]),
                                    change);
            }
        } else {
            auto values = context.view.Write(alpha);
            for (auto slot : context.slots) {
                values[slot.index] *= FadeValueChange(
                    LifetimePosition(lifetimes[slot.index], initial[slot.index]), change);
            }
        }
    }
};

struct AlphaFadeOperator {
    ParticleAttributes                                              attributes;
    float                                                           fade_in { 0.5f };
    float                                                           fade_out { 0.5f };
    particle::ParticleWriteIndex<particle::AlphaAttribute>          alpha;
    particle::ParticleReadIndex<particle::LifetimeAttribute>        lifetime;
    particle::ParticleReadIndex<particle::InitialLifetimeAttribute> initial_lifetime;

    void Compile(particle::ParticleViewCompiler& compiler) {
        alpha            = compiler.Write(attributes.alpha);
        lifetime         = compiler.Read(attributes.lifetime);
        initial_lifetime = compiler.Read(attributes.initial_lifetime);
    }

    void Update(particle::ParticleUpdateContext& context) {
        auto values    = context.view.Write(alpha);
        auto lifetimes = context.view.Read(lifetime);
        auto initial   = context.view.Read(initial_lifetime);
        for (auto slot : context.slots) {
            auto life = LifetimePosition(lifetimes[slot.index], initial[slot.index]);
            if (life <= fade_in) {
                values[slot.index] *= FadeValueChange(life, 0, fade_in, 0, 1.0f);
            } else if (life > fade_out) {
                values[slot.index] *= 1.0f - FadeValueChange(life, fade_out, 1.0f, 0, 1.0f);
            }
        }
    }
};

struct ColorChangeOperator {
    ParticleAttributes                                              attributes;
    VecChange                                                       change;
    particle::ParticleWriteIndex<particle::ColorAttribute>          color;
    particle::ParticleReadIndex<particle::LifetimeAttribute>        lifetime;
    particle::ParticleReadIndex<particle::InitialLifetimeAttribute> initial_lifetime;

    void Compile(particle::ParticleViewCompiler& compiler) {
        color            = compiler.Write(attributes.color);
        lifetime         = compiler.Read(attributes.lifetime);
        initial_lifetime = compiler.Read(attributes.initial_lifetime);
    }

    void Update(particle::ParticleUpdateContext& context) {
        auto colors    = context.view.Write(color);
        auto lifetimes = context.view.Read(lifetime);
        auto initial   = context.view.Read(initial_lifetime);
        for (auto slot : context.slots) {
            auto            life = LifetimePosition(lifetimes[slot.index], initial[slot.index]);
            Eigen::Vector3f factor;
            for (usize component {}; component < usize(3); ++component) {
                auto raw    = component.to_primitive();
                factor[raw] = static_cast<float>(FadeValueChange(life,
                                                                 change.starttime,
                                                                 change.endtime,
                                                                 change.startvalue[component],
                                                                 change.endvalue[component]));
            }
            colors[slot.index] = colors[slot.index].cwiseProduct(factor);
        }
    }
};

struct OscillationIndices {
    particle::ParticleWriteIndex<OscillationResetAttribute>     reset;
    particle::ParticleWriteIndex<OscillationFrequencyAttribute> frequency;
    particle::ParticleWriteIndex<OscillationScaleAttribute>     scale;
    particle::ParticleWriteIndex<OscillationPhaseAttribute>     phase;

    void Compile(particle::ParticleViewCompiler& compiler,
                 const OscillationAttributes&    attributes) {
        reset     = compiler.Write(attributes.reset);
        frequency = compiler.Write(attributes.frequency);
        scale     = compiler.Write(attributes.scale);
        phase     = compiler.Write(attributes.phase);
    }

    auto Bind(particle::ParticleWriteView view) const -> OscillationValues {
        return {
            .reset     = view.Write(reset),
            .frequency = view.Write(frequency),
            .scale     = view.Write(scale),
            .phase     = view.Write(phase),
        };
    }
};

struct OscillateScalarOperator {
    enum class Target
    {
        Alpha,
        Size,
    };

    ParticleAttributes                                              attributes;
    OscillationAttributes                                           state_attributes;
    FrequencyValue                                                  frequency;
    Target                                                          target { Target::Alpha };
    OscillationIndices                                              state;
    particle::ParticleWriteIndex<particle::AlphaAttribute>          alpha;
    particle::ParticleWriteIndex<particle::SizeAttribute>           size;
    particle::ParticleReadIndex<particle::LifetimeAttribute>        lifetime;
    particle::ParticleReadIndex<particle::InitialLifetimeAttribute> initial_lifetime;

    void Compile(particle::ParticleViewCompiler& compiler) {
        if (target == Target::Alpha)
            alpha = compiler.Write(attributes.alpha);
        else
            size = compiler.Write(attributes.size);
        lifetime         = compiler.Read(attributes.lifetime);
        initial_lifetime = compiler.Read(attributes.initial_lifetime);
        state.Compile(compiler, state_attributes);
    }

    template<typename Values>
    void Apply(Values values, particle::ParticleUpdateContext& context) {
        auto lifetimes = context.view.Read(lifetime);
        auto initial   = context.view.Read(initial_lifetime);
        auto states    = state.Bind(context.view);
        for (auto slot : context.slots) {
            auto oscillator = states.At(slot.index);
            frequency.GenFrequency(lifetimes[slot.index] > 0.0f, oscillator);
            values[slot.index] *= frequency.GetScale(
                oscillator, LifetimePassed(lifetimes[slot.index], initial[slot.index]));
        }
    }

    void Update(particle::ParticleUpdateContext& context) {
        if (target == Target::Alpha)
            Apply(context.view.Write(alpha), context);
        else
            Apply(context.view.Write(size), context);
    }
};

struct OscillatePositionOperator {
    ParticleAttributes                                              attributes;
    array<FrequencyValue, 3>                                        frequencies;
    array<OscillationAttributes, 3>                                 state_attributes;
    array<OscillationIndices, 3>                                    states;
    particle::ParticleReadIndex<particle::LifetimeAttribute>        lifetime;
    particle::ParticleReadIndex<particle::InitialLifetimeAttribute> initial_lifetime;

    void Compile(particle::ParticleViewCompiler& compiler) {
        compiler.WriteBase(attributes.position);
        lifetime         = compiler.Read(attributes.lifetime);
        initial_lifetime = compiler.Read(attributes.initial_lifetime);
        for (usize index {}; index < usize(3); ++index) {
            states[index].Compile(compiler, state_attributes[index]);
        }
    }

    void Update(particle::ParticleUpdateContext& context) {
        auto                        positions = context.view.PositionsMut();
        auto                        lifetimes = context.view.Read(lifetime);
        auto                        initial   = context.view.Read(initial_lifetime);
        array<OscillationValues, 3> values {
            states[usize()].Bind(context.view),
            states[usize(1)].Bind(context.view),
            states[usize(2)].Bind(context.view),
        };
        for (auto slot : context.slots) {
            Eigen::Vector3d offset { Eigen::Vector3d::Zero() };
            auto            time = LifetimePassed(lifetimes[slot.index], initial[slot.index]);
            for (usize component {}; component < usize(3); ++component) {
                auto raw = component.to_primitive();
                if (frequencies[usize()].mask[component] < 0.01f) continue;
                auto oscillator = values[component].At(slot.index);
                frequencies[component].GenFrequency(lifetimes[slot.index] > 0.0f, oscillator);
                offset[raw] = frequencies[component].GetMove(oscillator, time, context.delta);
            }
            positions[slot.index] = (positions[slot.index].cast<double>() + offset).cast<float>();
        }
    }
};

struct TurbulenceOperator {
    ParticleAttributes                                        attributes;
    Turbulence                                                config;
    ParticleInstanceModifiers                                 modifiers;
    double                                                    phase {};
    double                                                    speed {};
    particle::ParticleWriteIndex<particle::VelocityAttribute> velocity;

    void Compile(particle::ParticleViewCompiler& compiler) {
        compiler.ReadBase(attributes.position);
        velocity = compiler.Write(attributes.velocity);
    }

    void Update(particle::ParticleUpdateContext& context) {
        auto frame      = ParticleFrameFrom(context.frame);
        auto positions  = context.view.Positions();
        auto velocities = context.view.Write(velocity);
        auto delta      = context.delta.to_primitive();
        for (auto slot : context.slots) {
            Eigen::Vector3d position = positions[slot.index].cast<double>();
            position.x() += phase + config.timescale * frame->time.to_primitive();
            Eigen::Vector3d result = speed * modifiers.Speed() *
                                     algorism::CurlNoise(position * config.scale * 2).normalized();
            for (usize component {}; component < usize(3); ++component) {
                result[component.to_primitive()] *= config.mask[component];
            }
            velocities[slot.index] =
                (velocities[slot.index].cast<double>() + result * delta).cast<float>();
        }
    }
};

struct VortexOperator {
    ParticleAttributes                                        attributes;
    Vortex                                                    config;
    bool                                                      extended { false };
    particle::ParticleWriteIndex<particle::VelocityAttribute> velocity;

    void Compile(particle::ParticleViewCompiler& compiler) {
        compiler.ReadBase(attributes.position);
        velocity = compiler.Write(attributes.velocity);
    }

    void Update(particle::ParticleUpdateContext& context) {
        auto particle_frame = ParticleFrameFrom(context.frame);
        auto frame          = ResolveVortexFrame(config, *particle_frame->subsystem);
        auto positions      = context.view.Positions();
        auto velocities     = context.view.Write(velocity);
        auto delta          = context.delta.to_primitive();
        for (auto slot : context.slots) {
            auto relative        = positions[slot.index].cast<double>() - frame.center;
            auto radial          = relative - frame.axis * relative.dot(frame.axis);
            auto radial_distance = radial.norm();
            if (radial_distance <= 1e-9) continue;

            if (! extended) {
                Eigen::Vector3d direction = -frame.axis.cross(radial).normalized();
                velocities[slot.index] =
                    (velocities[slot.index].cast<double>() +
                     direction * VortexSpeedAtDistance(config, radial_distance) * 0.5 * delta)
                        .cast<float>();
                continue;
            }

            auto distance =
                config.flags[Vortex::FlagEnum::infinite_axis] ? radial_distance : relative.norm();
            bool ring_shape = config.ringradius > 0.0f && config.ringwidth > 0.0f &&
                              config.ringpulldistance > 0.0f;
            if (ring_shape) {
                Eigen::Vector3d ring_position =
                    radial.normalized() * static_cast<double>(config.ringradius);
                auto ring_delta    = ring_position - relative;
                auto ring_distance = ring_delta.norm();
                if (ring_distance >= static_cast<double>(config.ringpulldistance)) continue;

                auto ring_width = static_cast<double>(config.ringwidth);
                auto pull_range =
                    rstd::cmp::max(1e-9, static_cast<double>(config.ringpulldistance) - ring_width);
                double ring_influence {};
                double pull_influence {};
                double ring_speed {};
                if (ring_distance <= ring_width) {
                    auto amount    = ring_distance / ring_width;
                    ring_speed     = algorism::lerp(amount, config.speedinner, config.speedouter);
                    ring_influence = 1.0;
                    pull_influence = amount;
                } else {
                    pull_influence = (ring_distance - ring_width) / pull_range;
                    ring_influence = f64(1.0 - pull_influence).sqrt().to_primitive();
                    ring_speed     = config.speedouter;
                }

                auto            ring_strength = ring_speed * ring_influence * 0.5;
                Eigen::Vector3d tangent       = -frame.axis.cross(radial).normalized();
                Eigen::Vector3d current       = velocities[slot.index].cast<double>();
                if (! config.flags[Vortex::FlagEnum::maintain_distance_to_center]) {
                    current += tangent * ring_strength * delta;
                } else {
                    auto axis_velocity = frame.axis * current.dot(frame.axis);
                    auto tangent_speed = current.dot(tangent) + ring_strength * delta;
                    current            = axis_velocity + tangent * tangent_speed;
                }
                auto pull_strength =
                    f64(VortexSpeedAtDistance(config, distance)).abs().to_primitive() * 0.5;
                current += ring_delta.normalized() * pull_strength * pull_influence * delta;
                velocities[slot.index] = current.cast<float>();
                continue;
            }

            auto strength = VortexSpeedAtDistance(config, distance) * 0.5;
            if (f64(strength).abs().to_primitive() <= 1e-9) continue;
            Eigen::Vector3d tangent = -frame.axis.cross(radial).normalized();
            Eigen::Vector3d current = velocities[slot.index].cast<double>();
            if (! config.flags[Vortex::FlagEnum::maintain_distance_to_center]) {
                current += tangent * strength * delta;
            } else {
                auto axis_velocity = frame.axis * current.dot(frame.axis);
                auto tangent_speed = current.dot(tangent) + strength * delta;
                current            = axis_velocity + tangent * tangent_speed;
            }
            velocities[slot.index] = current.cast<float>();
        }
    }
};

struct MaintainDistanceOperator {
    ParticleAttributes                                        attributes;
    MaintainDistance                                          config;
    particle::ParticleAttributeKey<MaintainDistanceAttribute> state_key;
    particle::ParticleWriteIndex<particle::VelocityAttribute> velocity;
    particle::ParticleWriteIndex<MaintainDistanceAttribute>   state;

    void Compile(particle::ParticleViewCompiler& compiler) {
        compiler.ReadBase(attributes.position);
        velocity = compiler.Write(attributes.velocity);
        state    = compiler.Write(state_key);
    }

    void Update(particle::ParticleUpdateContext& context) {
        auto frame      = ParticleFrameFrom(context.frame);
        auto positions  = context.view.Positions();
        auto velocities = context.view.Write(velocity);
        auto states     = context.view.Write(state);
        auto center =
            frame->subsystem->SimulationControlpoint(rstd::as_cast<usize>(config.controlpoint))
                .center;
        for (auto slot : context.slots) {
            auto relative = positions[slot.index].cast<double>() - center;
            auto distance = relative.norm();
            if (distance <= 1e-9) continue;
            if (! states[slot.index].initialized) {
                states[slot.index].distance    = static_cast<float>(distance);
                states[slot.index].initialized = true;
            }
            auto direction       = relative / distance;
            auto current         = velocities[slot.index].cast<double>();
            auto radial_velocity = current.dot(direction);
            auto target_velocity = (static_cast<double>(states[slot.index].distance) - distance) *
                                   static_cast<double>(config.variable_strength);
            velocities[slot.index] =
                (current + direction * (target_velocity - radial_velocity)).cast<float>();
        }
    }
};

struct ControlPointAttractOperator {
    ParticleAttributes                                        attributes;
    ControlPointForce                                         config;
    particle::ParticleWriteIndex<particle::VelocityAttribute> velocity;

    void Compile(particle::ParticleViewCompiler& compiler) {
        compiler.ReadBase(attributes.position);
        velocity = compiler.Write(attributes.velocity);
    }

    void Update(particle::ParticleUpdateContext& context) {
        auto frame      = ParticleFrameFrom(context.frame);
        auto positions  = context.view.Positions();
        auto velocities = context.view.Write(velocity);
        auto controlpoint =
            frame->subsystem->SimulationControlpoint(rstd::as_cast<usize>(config.controlpoint));
        Eigen::Vector3d offset =
            controlpoint.center +
            controlpoint.basis * Eigen::Vector3f { config.origin.data() }.cast<double>();
        auto delta = context.delta.to_primitive();
        for (auto slot : context.slots) {
            Eigen::Vector3d difference = offset - positions[slot.index].cast<double>();
            auto            distance   = difference.norm();
            if (distance <= 0.0 || distance >= config.threshold) continue;
            auto impulse = config.scale * delta * (1.0 - distance / config.threshold);
            // WE limits the velocity increment near the control point by default.
            if ((config.flags & u32(2)) != u32()) impulse = rstd::cmp::min(distance, impulse);
            velocities[slot.index] =
                (velocities[slot.index].cast<double>() + difference * (impulse / distance))
                    .cast<float>();
        }
    }
};

struct NoopUpdateOperator {
    void Compile(particle::ParticleViewCompiler&) {}
    void Update(particle::ParticleUpdateContext&) {}
};

Box<dyn<particle::ParticleUpdateProgram>>
ParticleParser::GenOperator(const Json& wpj, ParticleInstanceModifiers modifiers,
                            ParticleSubSystem& subsystem, usize operator_index) {
    auto attributes = subsystem.Attributes();
    do {
        if (wpj.get("name"_str).is_none()) break;
        String name;
        owe::GetJsonValue(wpj, "name"_str, name);
        if (name == "movement"_str) {
            float           drag { 0.0f };
            array<float, 3> gravity { 0.0f, 0.0f, 0.0f };
            owe::GetJsonValue(wpj, "drag"_str, drag, false);
            owe::GetJsonValue(wpj, "gravity"_str, gravity, false);
            return Box<dyn<particle::ParticleUpdateProgram>>::make(MovementOperator {
                .attributes = attributes,
                .drag       = drag,
                .gravity    = Vector3f(gravity.data()).cast<double>(),
                .modifiers  = modifiers.Clone(),
            });
        } else if (name == "angularmovement"_str) {
            float           drag { 0.0f };
            array<float, 3> force { 0.0f, 0.0f, 0.0f };
            owe::GetJsonValue(wpj, "drag"_str, drag, false);
            owe::GetJsonValue(wpj, "force"_str, force, false);
            return Box<dyn<particle::ParticleUpdateProgram>>::make(AngularMovementOperator {
                .attributes = attributes,
                .drag       = drag,
                .force      = Vector3f(force.data()).cast<double>(),
            });
        } else if (name == "sizechange"_str) {
            return Box<dyn<particle::ParticleUpdateProgram>>::make(ScalarChangeOperator {
                .attributes = attributes,
                .change     = ValueChange::ReadFromJson(wpj),
                .target     = ScalarChangeOperator::Target::Size,
                .modifiers  = modifiers.Clone(),
            });
        } else if (name == "alphafade"_str) {
            float fadeintime { 0.5f }, fadeouttime { 0.5f };
            owe::GetJsonValue(wpj, "fadeintime"_str, fadeintime, false);
            owe::GetJsonValue(wpj, "fadeouttime"_str, fadeouttime, false);
            return Box<dyn<particle::ParticleUpdateProgram>>::make(AlphaFadeOperator {
                .attributes = attributes,
                .fade_in    = fadeintime,
                .fade_out   = fadeouttime,
            });
        } else if (name == "alphachange"_str) {
            return Box<dyn<particle::ParticleUpdateProgram>>::make(ScalarChangeOperator {
                .attributes = attributes,
                .change     = ValueChange::ReadFromJson(wpj),
                .target     = ScalarChangeOperator::Target::Alpha,
                .modifiers  = modifiers.Clone(),
            });
        } else if (name == "colorchange"_str) {
            return Box<dyn<particle::ParticleUpdateProgram>>::make(ColorChangeOperator {
                .attributes = attributes,
                .change     = VecChange::ReadFromJson(wpj),
            });
        } else if (name == "oscillatealpha"_str) {
            return Box<dyn<particle::ParticleUpdateProgram>>::make(OscillateScalarOperator {
                .attributes = attributes,
                .state_attributes =
                    RegisterOscillationAttributes(subsystem, operator_index, "alpha"_str),
                .frequency = FrequencyValue::ReadFromJson(wpj, name.as_str()),
                .target    = OscillateScalarOperator::Target::Alpha,
            });
        } else if (name == "oscillatesize"_str) {
            return Box<dyn<particle::ParticleUpdateProgram>>::make(OscillateScalarOperator {
                .attributes = attributes,
                .state_attributes =
                    RegisterOscillationAttributes(subsystem, operator_index, "size"_str),
                .frequency = FrequencyValue::ReadFromJson(wpj, name.as_str()),
                .target    = OscillateScalarOperator::Target::Size,
            });
        } else if (name == "oscillateposition"_str) {
            auto frequency = FrequencyValue::ReadFromJson(wpj, name.as_str());
            return Box<dyn<particle::ParticleUpdateProgram>>::make(
                OscillatePositionOperator {
                    .attributes  = attributes,
                    .frequencies = { frequency, frequency, frequency },
                    .state_attributes = {
                        RegisterOscillationAttributes(subsystem, operator_index, "position_x"_str),
                        RegisterOscillationAttributes(subsystem, operator_index, "position_y"_str),
                        RegisterOscillationAttributes(subsystem, operator_index, "position_z"_str),
                    },
                });
        } else if (name == "turbulence"_str) {
            auto config = Turbulence::ReadFromJson(wpj);
            return Box<dyn<particle::ParticleUpdateProgram>>::make(TurbulenceOperator {
                .attributes = attributes,
                .config     = config,
                .modifiers  = modifiers.Clone(),
                .phase      = Random::get(config.phasemin, config.phasemax),
                .speed      = Random::get(config.speedmin, config.speedmax),
            });
        } else if (name == "vortex"_str) {
            return Box<dyn<particle::ParticleUpdateProgram>>::make(VortexOperator {
                .attributes = attributes,
                .config     = Vortex::ReadFromJson(wpj),
            });
        } else if (name == "vortex_v2"_str) {
            return Box<dyn<particle::ParticleUpdateProgram>>::make(VortexOperator {
                .attributes = attributes,
                .config     = Vortex::ReadFromJson(wpj),
                .extended   = true,
            });
        } else if (name == "maintaindistancetocontrolpoint"_str) {
            return Box<dyn<particle::ParticleUpdateProgram>>::make(MaintainDistanceOperator {
                .attributes = attributes,
                .config     = MaintainDistance::ReadFromJson(wpj),
                .state_key  = RegisterMaintainDistanceAttribute(subsystem, operator_index),
            });
        } else if (name == "controlpointattract"_str) {
            return Box<dyn<particle::ParticleUpdateProgram>>::make(ControlPointAttractOperator {
                .attributes = attributes,
                .config     = ControlPointForce::ReadFromJson(wpj),
            });
        }
    } while (false);
    return Box<dyn<particle::ParticleUpdateProgram>>::make(NoopUpdateOperator {});
}

Box<dyn<particle::ParticleEmitterProgram>> ParticleParser::GenEmitter(const wpscene::Emitter& wpe,
                                                                      ParticleSubSystem& subsystem,
                                                                      usize emitter_index) {
    ParticleAudioResponse audio_response {
        .enable    = wpe.audioprocessingmode != u32(),
        .amount    = wpe.audioamount,
        .exponent  = wpe.audioexponent,
        .frequency = wpe.audiofrequency,
        .bounds    = wpe.audiobounds,
    };
    if (wpe.name == "boxrandom"_str) {
        ParticleBoxEmitterArgs box;
        box.emit_speed     = wpe.rate;
        box.min_distance   = wpe.distancemin;
        box.max_distance   = wpe.distancemax;
        box.directions     = wpe.directions;
        box.origin         = wpe.origin;
        box.one_per_frame  = wpe.flags[wpscene::Emitter::FlagEnum::one_per_frame];
        box.instantaneous  = wpe.instantaneous;
        box.min_speed      = wpe.speedmin;
        box.max_speed      = wpe.speedmax;
        box.duration       = wpe.duration;
        box.controlpoint   = wpe.controlpoint;
        box.audio_response = audio_response;
        return Box<dyn<particle::ParticleEmitterProgram>>::make(
            BoxEmitterProgram(subsystem.SpawnPipeline(), rstd::move(box), emitter_index));
    } else if (wpe.name == "sphererandom"_str) {
        ParticleSphereEmitterArgs sphere;
        sphere.emit_speed     = wpe.rate;
        sphere.min_distance   = wpe.distancemin[usize(0)];
        sphere.max_distance   = wpe.distancemax[usize(0)];
        sphere.directions     = wpe.directions;
        sphere.origin         = wpe.origin;
        sphere.sign           = wpe.sign;
        sphere.one_per_frame  = wpe.flags[wpscene::Emitter::FlagEnum::one_per_frame];
        sphere.instantaneous  = wpe.instantaneous;
        sphere.min_speed      = wpe.speedmin;
        sphere.max_speed      = wpe.speedmax;
        sphere.duration       = wpe.duration;
        sphere.controlpoint   = wpe.controlpoint;
        sphere.audio_response = audio_response;
        return Box<dyn<particle::ParticleEmitterProgram>>::make(
            SphereEmitterProgram(subsystem.SpawnPipeline(), rstd::move(sphere), emitter_index));
    }

    struct NoopEmitter {
        void Compile(particle::ParticleViewCompiler&) {}
        void Emit(particle::ParticleEmitterContext&) {}
    };
    return Box<dyn<particle::ParticleEmitterProgram>>::make(NoopEmitter {});
}
