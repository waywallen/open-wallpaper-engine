module;

#include <rstd/macro.hpp>

module wescene.pkg.parse;
import :scene_context;
import eigen;
import wescene.pkg.spec_names;
import wescene.load_bench;
import wescene.core;
import wescene.types;
import rstd;
import rstd.log;
import rstd.cppstd;
import wescene.utils;
import wescene.scene;
import wescene.text;
import wescene.script;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;
using namespace owe;
using namespace Eigen;

namespace owe
{

float ParticleTextureRatio(const SceneMaterial& material) {
    auto it = material.customShader.constValues.get(WE_GLTEX_RESOLUTION_NAMES[usize()]);
    if (it.is_none()) return 1.0f;
    const auto& r = **it;
    if (r.size() < usize(2) || r[usize(0)] == 0.0f) return 1.0f;
    return r[usize(1)] / r[usize(0)];
}

struct ParticleRenderDesc {
    bool rope { false };
    bool rope_trail { false };
    bool trail { false };
};

ParticleRenderDesc DescribeParticleRender(const wpscene::ParticleRender& render) {
    ParticleRenderDesc desc;
    desc.rope       = render.name == "rope"_str;
    desc.rope_trail = render.name == "ropetrail"_str;
    desc.trail      = render.name.as_str()->ends_with("trail"_str);
    return desc;
}

bool ShaderComboEnabled(const wpscene::Material& material, const ShaderInfo& info, ref<str> name) {
    auto material_combo = material.combos.get(name);
    if (material_combo.is_some()) return **material_combo != i32();

    auto combo = info.combos.get(name);
    return combo.is_some() && (**combo).as_str() != "0"_str;
}

i32 LimitRopeSubdivision(i32 requested, const ParticleObjectParseServices& services,
                         const wpscene::Material& material, const ShaderInfo& info) {
    if (requested <= i32()) return i32();

    const bool lighting     = ShaderComboEnabled(material, info, "LIGHTING"_str);
    const bool refract      = ShaderComboEnabled(material, info, "REFRACT"_str);
    const bool fog          = ShaderComboEnabled(material, info, "FOG"_str);
    const bool fog_distance = ShaderComboEnabled(material, info, "FOG_DIST"_str) ||
                              (fog && services.shader_environment.fog_distance);
    const bool fog_height   = ShaderComboEnabled(material, info, "FOG_HEIGHT"_str) ||
                              (fog && services.shader_environment.fog_height);

    // genericropeparticle always emits position (4), UV (2), and color (4).
    u32 output_components { 10 };
    if (fog_distance || fog_height || lighting) output_components += u32(4);
    if (lighting) output_components += u32(6);
    if (refract) output_components += u32(7);

    const auto& limits                 = services.geometry_shader_limits;
    const auto  vertices_by_components = limits.max_total_output_components / output_components;
    const auto  max_vertices = rstd::cmp::min(vertices_by_components, limits.max_output_vertices);
    const auto  max_subdivision = max_vertices > u32(4) ? (max_vertices - u32(4)) / u32(2) : u32();
    const auto  limited =
        rstd::as_cast<i32>(rstd::cmp::min(max_subdivision, rstd::as_cast<u32>(requested)));
    if (limited != requested) {
        rstd_warn("rope subdivision reduced from {} to {} for geometry shader limits",
                  requested,
                  limited);
    }
    return limited;
}

ParticleAnimationMode ToAnimMode(ref<str> str) {
    if (str == "randomframe"_str)
        return ParticleAnimationMode::RANDOMONE;
    else if (str == "sequence"_str)
        return ParticleAnimationMode::SEQUENCE;
    else {
        return ParticleAnimationMode::SEQUENCE;
    }
}

void ApplyParticleOverride(wpscene::ParticleInstanceoverride& state, ref<str> field,
                           slice<float> values) {
    auto write_scalar = [&](float& destination) {
        if (values.len() >= usize(1)) destination = values[usize()];
    };
    auto write_vec3 = [&](array<float, 3>& destination, float scale) -> bool {
        if (values.len() < usize(3)) return false;
        destination = { values[usize()] * scale,
                        values[usize(1)] * scale,
                        values[usize(2)] * scale };
        return true;
    };

    auto parse_index = [&](ref<str> prefix) -> Option<usize> {
        auto suffix = field.strip_prefix(prefix);
        if (suffix.is_none()) return None();
        auto parsed = rstd::from_str<usize>(*suffix);
        if (parsed.is_err()) return None();
        auto index = rstd::move(parsed).unwrap_unchecked();
        return index < usize(8) ? Some(index) : None();
    };

    if (field == "alpha"_str)
        write_scalar(state.alpha);
    else if (field == "size"_str)
        write_scalar(state.size);
    else if (field == "lifetime"_str)
        write_scalar(state.lifetime);
    else if (field == "rate"_str)
        write_scalar(state.rate);
    else if (field == "speed"_str)
        write_scalar(state.speed);
    else if (field == "count"_str)
        write_scalar(state.count);
    else if (field == "brightness"_str)
        write_scalar(state.brightness);
    else if (field == "color"_str) {
        write_vec3(state.color, 255.0f);
        state.overColor = true;
    } else if (field == "colorn"_str) {
        write_vec3(state.colorn, 1.0f);
        state.overColorn = true;
    } else if (auto index = parse_index("controlpointangle"_str); index.is_some()) {
        write_vec3(state.controlpointangle[*index], 1.0f);
    } else if (auto index = parse_index("controlpoint"_str); index.is_some()) {
        array<float, 3> point {};
        if (write_vec3(point, 1.0f)) state.controlpoint[*index] = Some(point);
    }
}

Vec<float> ReadParticleOverride(const wpscene::ParticleInstanceoverride& state, ref<str> field) {
    auto scalar = [](float value) {
        Vec<float> out;
        out.push(float(value));
        return out;
    };
    auto vec3 = [](const array<float, 3>& value, float scale) {
        Vec<float> out;
        out.push(value[usize(0)] * scale);
        out.push(value[usize(1)] * scale);
        out.push(value[usize(2)] * scale);
        return out;
    };
    if (field == "alpha"_str) return scalar(state.alpha);
    if (field == "size"_str) return scalar(state.size);
    if (field == "lifetime"_str) return scalar(state.lifetime);
    if (field == "rate"_str) return scalar(state.rate);
    if (field == "speed"_str) return scalar(state.speed);
    if (field == "count"_str) return scalar(state.count);
    if (field == "brightness"_str) return scalar(state.brightness);
    if (field == "color"_str) return vec3(state.color, 1.0f / 255.0f);
    if (field == "colorn"_str) return vec3(state.colorn, 1.0f);
    return {};
}

struct ParticleOverrideControl {
    Arc<wpscene::ParticleInstanceoverride> state;
    String                                 field;

    void Apply(slice<float> values) { ApplyParticleOverride(*state, field.as_str(), values); }
};

struct ParticleNodeControl {
    Arc<wpscene::ParticleInstanceoverride> state;
    Arc<ParticlePlaybackState>             playback;

    Vec<float> Get(ref<str> field) const { return ReadParticleOverride(*state, field); }
    void       Apply(ref<str> field, slice<float> values) {
        ApplyParticleOverride(*state, field, values);
    }
    void Play() {
        playback->playing.store(true, rstd::sync::atomic::Ordering::Release);
        playback->reset_sequence.fetch_add(u32(1), rstd::sync::atomic::Ordering::AcqRel);
    }
    void Stop() {
        playback->playing.store(false, rstd::sync::atomic::Ordering::Release);
        playback->reset_sequence.fetch_add(u32(1), rstd::sync::atomic::Ordering::AcqRel);
    }
    void Pause() { playback->playing.store(false, rstd::sync::atomic::Ordering::Release); }
    bool IsPlaying() const { return playback->playing.load(rstd::sync::atomic::Ordering::Acquire); }
    void Emit(u32 count) const {
        auto pending = playback->pending_emit_count.load(rstd::sync::atomic::Ordering::Relaxed);
        while (! playback->pending_emit_count.compare_exchange_weak(
            pending,
            pending.saturating_add(count),
            rstd::sync::atomic::Ordering::Release,
            rstd::sync::atomic::Ordering::Relaxed)) {
        }
    }
};

void LoadControlPoint(SceneParseContext& context, ParticleSubSystem& system,
                      const wpscene::Particle& particle, ParticleInstanceModifiers modifiers) {
    auto points = system.ControlpointsMut();
    auto count  = rstd::cmp::min(points.len(), particle.controlpoints.len());
    for (usize index {}; index < count; ++index) {
        auto source_index         = index;
        points[index].base_offset = Eigen::Vector3d { particle.controlpoints[source_index]
                                                          .offset.clone()
                                                          .map([](float value) {
                                                              return static_cast<double>(value);
                                                          })
                                                          .data() };
        points[index].offset     = points[index].base_offset;
        points[index].link_mouse = particle.controlpoints[source_index]
                                       .flags[wpscene::ParticleControlpoint::FlagEnum::link_mouse];
        points[index].worldspace = particle.controlpoints[source_index]
                                       .flags[wpscene::ParticleControlpoint::FlagEnum::worldspace];
    }
    system.SetInstanceModifiers(modifiers.Clone());
    if (! modifiers.ControlpointsEnabled()) return;
    const auto& field_bindings = modifiers.ControlpointFieldBindings();
    if (! field_bindings) return;
    for (usize index {}; index < points.len(); ++index) {
        auto field   = rstd::format("controlpointangle{}", index);
        auto binding = field_bindings->Get(field.as_str());
        if (binding.is_some() && (**binding).animation.is_some())
            system.SetControlpointAngleTrack(index, ResolveAnimationTrack(context, **binding));
    }
}
void LoadInitializer(ParticleSubSystem& system, const wpscene::Particle& particle,
                     ParticleInstanceModifiers modifiers) {
    u32 implicit_sequence_count { 2 };
    for (const auto& emitter : particle.emitters) {
        if (emitter.max_emit_per_period > u32()) {
            implicit_sequence_count = emitter.max_emit_per_period;
            break;
        }
    }
    for (const auto& initializer : particle.initializers) {
        auto instruction = ParticleParser::GenInitializer(initializer, implicit_sequence_count);
        auto count       = instruction.SequenceCount();
        if (count.is_some()) system.SetRopeSequenceCount(*count);
        system.AddInitializer(rstd::move(instruction));
    }
    if (modifiers.Enabled()) {
        system.AddInitializer(ParticleParser::GenOverride(rstd::move(modifiers)));
    }
}
void LoadOperator(ParticleSubSystem& system, const wpscene::Particle& particle,
                  ParticleInstanceModifiers modifiers) {
    usize index {};
    for (const auto& operation : particle.operators) {
        system.AddOperator(
            ParticleParser::GenOperator(operation, modifiers.Clone(), system, index++));
    }
}
void LoadEmitter(ParticleSubSystem& system, const wpscene::Particle& particle,
                 const ParticleInstanceModifiers& modifiers) {
    usize emitter_index {};
    for (const auto& em : particle.emitters) {
        auto newEm = em.clone();
        newEm.rate *= modifiers.Count();
        system.AddEmitter(ParticleParser::GenEmitter(newEm, system, emitter_index++));
    }
}

ParticleSubSystem::SpawnType ParseSpawnType(ref<str> str) {
    using ST = ParticleSubSystem::SpawnType;
    ST type { ST::STATIC };
    if (str == "eventfollow"_str) {
        type = ST::EVENT_FOLLOW;
    } else if (str == "eventspawn"_str) {
        type = ST::EVENT_SPAWN;
    } else if (str == "eventdeath"_str) {
        type = ST::EVENT_DEATH;
    }
    return type;
};

struct ParticleChildPtr {
    wpscene::ParticleChild*                        child { nullptr };
    SceneNode*                                     node_parent { nullptr };
    ParticleSubSystem*                             particle_parent { nullptr };
    Option<Arc<ParticlePlaybackState>>             playback;
    Option<Arc<wpscene::ParticleInstanceoverride>> instance_override;

    // Effective world scale at node_parent. Particle child origins are
    // pre-divided by this so the shader's MVP scale recovers the authored
    // parent-relative world-pixel offset.
    Eigen::Vector3f world_scale { 1.f, 1.f, 1.f };
};

void SetParticleUniformConfig(ParticleObjectParseOutput& output, const Arc<SceneNode>& node,
                              UniformNodeConfigDraft config) {
    config.configured = true;
    for (auto& entry : output.uniform_configs) {
        if (entry.node.as_ptr() != node.as_ptr()) continue;
        entry.config = rstd::move(config);
        return;
    }
    output.uniform_configs.push(SceneUniformConfigDraft {
        .node   = node.clone(),
        .config = rstd::move(config),
    });
}

void BuildParticleObjectNode(ParticleObjectParseServices& services,
                             ParticleObjectParseOutput& output, wpscene::ParticleObject& wppartobj,
                             ParticleChildPtr child_ptr = {}) {
    struct ChildData {
        ChildData() = default;
        ChildData(const wpscene::ParticleChild& o)
            : type(o.type.as_str()),
              maxcount(o.maxcount),
              controlpointstartindex(o.controlpointstartindex),
              probability(o.probability) {}
        ref<str>    type { "static"_str };
        i32         maxcount { 20 };
        Option<i32> controlpointstartindex;
        float       probability { 1.0f };
    };

    wpscene::Particle*     p_particle_obj { nullptr };
    Option<Arc<SceneNode>> spNodeOpt;
    ChildData              child_data;

    bool is_child = child_ptr.child != nullptr;
    if (is_child) {
        p_particle_obj = &(child_ptr.child->obj);
        // ParticleChild::origin is a WE world-pixel offset from the parent
        // particle. SceneNode hierarchy composes T(local) * S(parent) so
        // the local translation gets multiplied by parent scale at render
        // time; pre-divide so the world translation matches the JSON.
        Vector3f corigin(child_ptr.child->origin.data());
        for (int i = 0; i < 3; ++i) {
            float s = child_ptr.world_scale[i];
            if (f32(s).abs().to_primitive() > 1e-6f) corigin[i] /= s;
        }
        spNodeOpt  = Some(Arc<SceneNode>::make(corigin,
                                               Vector3f(child_ptr.child->scale.data()),
                                               Vector3f(child_ptr.child->angles.data()),
                                               child_ptr.child->name.as_str()));
        child_data = ChildData(*child_ptr.child);

    } else {
        p_particle_obj = &wppartobj.particleObj;
        spNodeOpt      = Some(Arc<SceneNode>::make(Vector3f(wppartobj.origin.data()),
                                                   Vector3f(wppartobj.scale.data()),
                                                   Vector3f(wppartobj.angles.data()),
                                                   wppartobj.name.as_str()));
        auto& spNode   = *spNodeOpt;
        spNode->ID()   = wppartobj.id;
        if (! wppartobj.visible) {
            spNode->SetVisible(false);
            services.scene->MarkLayerVisibilityElidable(WallpaperLayerId { .value = wppartobj.id });
        }
        if (! wppartobj.visible_user.empty())
            spNode->SetVisibleUserBinding(ToSceneUserVisibilityBinding(wppartobj.visible_user));
    }
    auto& spNode = *spNodeOpt;
    spNode->SetReflected(wppartobj.reflected);

    // Effective world scale at this SceneNode: parent's world scale times
    // this node's local scale. Propagated to child particle nodes.
    Eigen::Vector3f node_world_scale = child_ptr.world_scale.cwiseProduct(spNode->Scale());

    auto playback_state = is_child && child_ptr.playback.is_some()
                              ? (*child_ptr.playback).clone()
                              : Arc<ParticlePlaybackState>::make();

    auto& particle_obj = *p_particle_obj;
    auto& vfs          = *services.vfs;
    if (is_child && child_ptr.instance_override.is_none()) {
        rstd_error("particle child '{}' has no instance override state", child_ptr.child->name);
        return;
    }
    auto override_state =
        is_child ? (*child_ptr.instance_override).clone()
                 : Arc<wpscene::ParticleInstanceoverride>::make(wppartobj.instanceoverride.clone());
    auto modifiers =
        ParticleInstanceModifiers(override_state.clone(), particle_obj.flags, ! is_child);
    const auto& override = *override_state;

    const auto& wppartRenderer    = particle_obj.renderers[usize()];
    auto        render_desc       = DescribeParticleRender(wppartRenderer);
    bool        render_rope       = render_desc.rope;
    bool        render_rope_trail = render_desc.rope_trail;
    bool        rope_shader       = render_rope || render_rope_trail;
    bool        hastrail          = render_desc.trail;

    if (rope_shader) particle_obj.material.shader = "genericropeparticle"_Str;

    // wppartobj.origin[1] = context.ortho_h - wppartobj.origin[1];

    if (particle_obj.flags[wpscene::Particle::FlagEnum::perspective]) {
        spNode->SetCamera("global_perspective"_str);
    }

    SceneMaterial          material;
    UniformNodeConfigDraft svData;

    if (! is_child) {
        svData.SetParallaxContract(wppartobj.parallax, wppartobj.id);
    }
    svData.use_camera_eye_position = particle_obj.flags[wpscene::Particle::FlagEnum::perspective];
    svData.vertices_in_world_space = particle_obj.flags[wpscene::Particle::FlagEnum::wordspace];

    ShaderInfo shaderInfo;
    shaderInfo.baseConstSvs = services.global_base_uniforms.clone();
    (void)shaderInfo.baseConstSvs.insert(rstd::into(G_ORIENTATIONUP),
                                         ShaderValue(array<float, 3> { 0.0f, 1.0f, 0.0f }));
    (void)shaderInfo.baseConstSvs.insert(rstd::into(G_ORIENTATIONRIGHT),
                                         ShaderValue(array<float, 3> { 1.0f, 0.0f, 0.0f }));
    (void)shaderInfo.baseConstSvs.insert(rstd::into(G_ORIENTATIONFORWARD),
                                         ShaderValue(array<float, 3> { 0.0f, 0.0f, 1.0f }));
    (void)shaderInfo.baseConstSvs.insert(rstd::into(G_VIEWUP),
                                         ShaderValue(array<float, 3> { 0.0f, 1.0f, 0.0f }));
    (void)shaderInfo.baseConstSvs.insert(rstd::into(G_VIEWRIGHT),
                                         ShaderValue(array<float, 3> { 1.0f, 0.0f, 0.0f }));
    (void)shaderInfo.baseConstSvs.insert(rstd::into(G_EYEPOSITION),
                                         ShaderValue(array<float, 3> {
                                             rstd::as_cast<float>(services.ortho_w) / 2.0f,
                                             rstd::as_cast<float>(services.ortho_h) / 2.0f,
                                             1000.0f,
                                         }));

    u32 maxcount = rstd::cmp::min(u32(20000), particle_obj.maxcount);

    Option<Arc<ParticleTrailUniformState>> trail_uniform_state;
    if (hastrail) {
        double          in_SegmentUVTimeOffset = 0.0;
        double          in_SegmentMaxCount     = rstd::as_cast<double>(maxcount) - 1.0;
        array<float, 4> render_var {
            (float)wppartRenderer.length,
            (float)wppartRenderer.maxlength,
            (float)in_SegmentUVTimeOffset,
            (float)in_SegmentMaxCount,
        };
        (void)shaderInfo.baseConstSvs.insert(rstd::into(G_RENDERVAR0), ShaderValue(render_var));
        if (render_rope_trail) {
            trail_uniform_state = Some(Arc<ParticleTrailUniformState>::make(
                ParticleTrailUniformState { .render_var = render_var }));
        }
        (void)shaderInfo.combos.insert(rstd::into(WE_CB_TRAILRENDERER), "1"_Str);
        if (! render_rope_trail)
            (void)shaderInfo.combos.insert(rstd::into(WE_CB_THICK_FORMAT), "1"_Str);
    }
    if (rope_shader) {
        i32 subdiv = rstd::as_cast<i32>(f32(wppartRenderer.subdivision).round().to_primitive());
        subdiv     = LimitRopeSubdivision(subdiv, services, particle_obj.material, shaderInfo);
        (void)shaderInfo.combos.insert("TRAILSUBDIVISION"_Str, rstd::format("{}", subdiv));
    }

    auto animationmode = ToAnimMode(particle_obj.animationmode.as_str());
    if (animationmode == ParticleAnimationMode::SEQUENCE &&
        ! particle_obj.flags[wpscene::Particle::FlagEnum::spritenoframeblending]) {
        (void)shaderInfo.combos.insert("SPRITESHEETBLEND"_Str, "1"_Str);
    }

    bool mat_ok = false;
    try {
        auto material_result =
            BuildMaterial(vfs,
                          *services.shader_cache,
                          services.shader_environment,
                          particle_obj.material,
                          *services.scene,
                          rstd::move(shaderInfo),
                          services.geometry_shader_supported ? GeometryStageRequirement::Required
                                                             : GeometryStageRequirement::Disabled);
        if (material_result.is_ok()) {
            auto material_build = rstd::move(material_result).unwrap_unchecked();
            material            = rstd::move(material_build.material);
            shaderInfo          = rstd::move(material_build.shader_info);
            mat_ok              = true;
        }
    } catch (const std::exception& e) {
        rstd_error("load particleobj '{}' material exception: {}", wppartobj.name, e.what());
    }
    if (! mat_ok) {
        rstd_error("load particleobj '{}' material faild", wppartobj.name);
        return;
    }
    LoadConstvalue(*services.construction_context, material, particle_obj.material, shaderInfo);
    auto  spMesh             = Arc<SceneMesh>::make(true);
    auto& mesh               = *spMesh;
    auto  sequencemultiplier = particle_obj.sequencemultiplier;
    bool  hasSprite          = material.hasSprite;
    (void)hasSprite;

    bool           thick_format = material.hasSprite || (hastrail && ! render_rope_trail);
    rstd::uint32_t trail_length = 0;
    if (render_rope_trail) {
        rstd::int32_t segments = wppartRenderer.segments.to_primitive();
        segments               = rstd::cmp::min(256, rstd::cmp::max(1, segments));
        trail_length           = static_cast<rstd::uint32_t>(segments);
    }
    ParticleFollowAnchor follow_anchor;
    if (hastrail && ! render_rope_trail) {
        follow_anchor.trail_renderer = true;
        follow_anchor.length         = wppartRenderer.length;
        follow_anchor.max_length     = wppartRenderer.maxlength;
        follow_anchor.texture_ratio  = ParticleTextureRatio(material);
    }

    auto spawn_type = ParseSpawnType(child_data.type);
    if (is_child && spawn_type == ParticleSubSystem::SpawnType::STATIC &&
        child_data.controlpointstartindex.is_some()) {
        spawn_type = ParticleSubSystem::SpawnType::STATIC_CONTROLPOINT;
    }
    auto max_instance_count = u32(static_cast<rstd::uint32_t>(
        rstd::cmp::max(rstd::int32_t(0), child_data.maxcount.to_primitive())));
    auto particleSub        = Box<ParticleSubSystem>::make(
        *services.scene,
        spMesh.clone(),
        maxcount,
        f64(modifiers.Rate()),
        max_instance_count,
        f64(child_data.probability),
        spawn_type,
        ParticleAnimationSpec {
            .mode                = animationmode,
            .sequence_multiplier = sequencemultiplier,
        },
        follow_anchor,
        u32(trail_length),
        f64(render_rope_trail ? static_cast<double>(wppartRenderer.length) : 0.0),
        f64(static_cast<double>(particle_obj.starttime)),
        particle_obj.flags[wpscene::Particle::FlagEnum::wordspace],
        trail_uniform_state.is_some() ? Some((*trail_uniform_state).clone()) : None());

    {
        auto mesh_capacity = particleSub->MaxParticleCapacity();
        if (mesh_capacity.is_none()) {
            rstd_error("particle mesh capacity overflow for '{}'", spNode->Name());
            return;
        }
        auto mesh_maxcount = *mesh_capacity;
        if (rope_shader) {
            if (render_rope_trail) {
                auto capacity = mesh_capacity->checked_mul(u32(trail_length));
                if (capacity.is_none()) {
                    rstd_error("particle rope capacity overflow for '{}'", spNode->Name());
                    return;
                }
                mesh_maxcount = *capacity;
            }
            SetRopeParticleMesh(mesh,
                                particle_obj,
                                mesh_maxcount,
                                thick_format,
                                render_rope_trail,
                                services.geometry_shader_supported);
        } else {
            SetParticleMesh(mesh, mesh_maxcount, thick_format, services.geometry_shader_supported);
        }
    }

    particleSub->SetOwnerNode(spNode.as_ptr());
    particleSub->SetPlaybackState(playback_state.clone());
    if (child_data.controlpointstartindex.is_some())
        particleSub->SetParentControlpointStartIndex(*child_data.controlpointstartindex);
    LoadEmitter(*particleSub, particle_obj, modifiers);
    LoadInitializer(*particleSub, particle_obj, modifiers.Clone());
    LoadOperator(*particleSub, particle_obj, modifiers.Clone());
    LoadControlPoint(*services.construction_context, *particleSub, particle_obj, modifiers.Clone());
    particleSub->Finalize();

    // Register every {user:"<key>", value:...} binding on instanceoverride
    // so RenderSetUserProperty can mutate the shared state at runtime.
    if (! is_child) {
        override.bindings.iter().for_each([&](auto entry) {
            auto [field, key] = entry;
            services.scene->RegisterParticleOverrideBinding(
                key->clone(),
                Arc<dyn<SceneParticleOverrideControl>>::make(ParticleOverrideControl {
                    .state = override_state.clone(),
                    .field = field->clone(),
                }));
        });
    }

    mesh.AddMaterial(rstd::move(material));
    RegisterMaterialBindings(
        *services.scene, mesh.MaterialSlots()[usize()], particle_obj.material, shaderInfo);
    if (services.construction_context != nullptr) {
        WireMaterialShaderValueScripts(*services.construction_context,
                                       spNode,
                                       mesh.MaterialSlots()[mesh.MaterialSlots().len() - usize(1)],
                                       particle_obj.material,
                                       shaderInfo);
    }
    spNode->AddMesh(spMesh.clone());
    SetParticleUniformConfig(output, spNode, rstd::move(svData));
    if (trail_uniform_state.is_some()) {
        output.trail_uniform_configs.push(ParticleTrailUniformConfigDraft {
            .node          = spNode.clone(),
            .uniform_state = rstd::move(*trail_uniform_state),
        });
    }

    for (auto& child : particle_obj.children) {
        BuildParticleObjectNode(services,
                                output,
                                wppartobj,
                                {
                                    .child             = &child,
                                    .node_parent       = spNode.as_ptr(),
                                    .particle_parent   = particleSub.get(),
                                    .playback          = Some(playback_state.clone()),
                                    .instance_override = Some(override_state.clone()),
                                    .world_scale       = node_world_scale,
                                });
    }

    if (is_child)
        child_ptr.particle_parent->AddChild(rstd::move(particleSub));
    else
        services.particle_runtime->Add(rstd::move(particleSub));

    if (! is_child) {
        spNode->SetParticleControl(Arc<dyn<SceneParticleControl>>::make(ParticleNodeControl {
            .state    = override_state.clone(),
            .playback = playback_state.clone(),
        }));
        AssignNodeFieldAnimations(
            *services.construction_context, *spNode.as_ptr(), wppartobj.field_bindings);
    }
    if (services.construction_context != nullptr)
        WireFieldScripts(*services.construction_context, spNode, wppartobj.field_bindings);
    if (is_child)
        child_ptr.node_parent->AppendChild(spNode.clone());
    else
        output.root = Some(spNode.clone());
}

auto BuildParticleObjectImpl(ParticleObjectParseServices& services,
                             wpscene::ParticleObject&     particle) -> ParticleObjectParseOutput {
    ParticleObjectParseOutput output;
    BuildParticleObjectNode(services, output, particle);
    return output;
}

void ParseParticleObjImpl(SceneParseContext& context, wpscene::ParticleObject& particle) {
    if (context.particle_runtime.is_none()) return;
    if (! particle.particle.is_empty() &&
        ! context.dynamic_particle_prototypes.contains_key(particle.particle.as_str())) {
        (void)context.dynamic_particle_prototypes.insert(particle.particle.clone(),
                                                         particle.Clone());
    }

    ParticleObjectParseServices services {
        .scene                     = context.scene.get(),
        .vfs                       = context.vfs,
        .shader_cache              = context.shader_cache.clone(),
        .shader_environment        = context.shader_environment,
        .geometry_shader_limits    = context.geometry_shader_limits,
        .geometry_shader_supported = context.geometry_shader_supported,
        .global_base_uniforms      = context.global_base_uniforms.clone(),
        .particle_runtime          = (*context.particle_runtime).clone(),
        .ortho_w                   = context.ortho_w,
        .ortho_h                   = context.ortho_h,
        .construction_context      = &context,
    };
    auto output = BuildParticleObject(services, particle);
    if (output.root.is_none()) return;
    for (auto& draft : output.uniform_configs) context.uniform_configs.push(rstd::move(draft));
    for (auto& draft : output.trail_uniform_configs)
        context.particle_trail_uniform_configs.push(rstd::move(draft));
    RegisterNodeRef(context,
                    particle.id,
                    SceneParseContext::NodeRef {
                        particle.parent,
                        Some(rstd::move(*output.root)),
                        None(),
                        particle.attachment.clone(),
                    });
}

void ParseParticleObj(SceneParseContext& context, wpscene::ParticleObject& particle) {
    PrepareAnimationBindings(context, particle);
    ParseParticleObjImpl(context, particle);
}

auto BuildParticleObject(ParticleObjectParseServices& services, wpscene::ParticleObject& particle)
    -> ParticleObjectParseOutput {
    return BuildParticleObjectImpl(services, particle);
}

} // namespace owe
