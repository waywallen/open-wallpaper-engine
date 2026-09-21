module;

#include <rstd/macro.hpp>

module wescene.pkg.scene_obj;
import wescene.core;
import rstd.log;

using namespace owe::wpscene;
using namespace rstd::prelude;
using namespace rstd::literals;

namespace
{

auto LoadAssetJsonFile(owe::fs::VFS& vfs, ref<str> path) -> Option<owe::Json> {
    auto parsed = owe::ReadAssetJsonFile(vfs, path);
    if (parsed.is_err()) {
        auto error = rstd::move(parsed).unwrap_err_unchecked();
        rstd_error("Can't load json {}: {}", path, error.message.as_str());
        return None();
    }
    return Some(rstd::move(parsed).unwrap_unchecked());
}

} // namespace

bool ParticleChild::FromJson(const owe::Json& json, fs::VFS& vfs) {
    owe::GetJsonValue(json, "name"_str, name);

    u32 raw_flags {};
    owe::GetJsonValue(json, "flags"_str, raw_flags, false);
    flags = EFlags(raw_flags.to_primitive());

    if (json.get("type"_str).is_some()) {
        owe::GetJsonValue(json, "type"_str, type);
    } else if (flags[FlagEnum::eventfollow]) {
        // Legacy child entries encode event-follow attachment in flags without a type field.
        type = "eventfollow"_Str;
    }

    if (name.is_empty()) {
        return false;
    }

    auto jParticle = LoadAssetJsonFile(vfs, name.as_str());
    if (! jParticle) return false;

    if (! obj.FromJson(*jParticle, vfs)) return false;

    owe::GetJsonValue(json, "maxcount"_str, maxcount, false);
    auto controlpoint_start = json.get("controlpointstartindex"_str);
    if (controlpoint_start.is_some() && ! (*controlpoint_start)->is_null()) {
        i32 value {};
        owe::GetJsonValue(json, "controlpointstartindex"_str, value, false);
        controlpointstartindex = Some(value);
    }
    owe::GetJsonValue(json, "probability"_str, probability, false);
    owe::GetJsonValue(json, "origin"_str, origin, false);
    owe::GetJsonValue(json, "scale"_str, scale, false);
    owe::GetJsonValue(json, "angles"_str, angles, false);
    return true;
}

ParticleChild ParticleChild::Clone() const {
    ParticleChild out;
    out.type                   = type.clone();
    out.name                   = name.clone();
    out.maxcount               = maxcount;
    out.flags                  = flags;
    out.controlpointstartindex = controlpointstartindex;
    out.probability            = probability;
    out.angles                 = angles;
    out.origin                 = origin;
    out.scale                  = scale;
    out.obj                    = obj.Clone();
    return out;
}

bool ParticleControlpoint::FromJson(const owe::Json& json) {
    owe::GetJsonValue(json, "id"_str, id);

    u32 _raw_flags { 0 };
    owe::GetJsonValue(json, "flags"_str, _raw_flags, false);
    flags = EFlags(_raw_flags.to_primitive());

    owe::GetJsonValue(json, "offset"_str, offset, false);
    return true;
};

bool ParticleRender::FromJson(const owe::Json& json) {
    owe::GetJsonValue(json, "name"_str, name);

    if (name == "ropetrail"_str) subdivision = 1.0f;
    if (name.as_str()->starts_with("rope"_str)) {
        owe::GetJsonValue(json, "subdivision"_str, subdivision, false);
    }
    if (name == "spritetrail"_str || name == "ropetrail"_str) {
        owe::GetJsonValue(json, "length"_str, length, false);
        owe::GetJsonValue(json, "maxlength"_str, maxlength, false);
        owe::GetJsonValue(json, "segments"_str, segments, false);
    }
    return true;
}

auto ParticleRender::clone() const -> ParticleRender {
    return { .name        = name.clone(),
             .length      = length,
             .maxlength   = maxlength,
             .subdivision = subdivision,
             .segments    = segments };
}

auto Emitter::clone() const -> Emitter {
    Emitter out;
    out.name                = name.clone();
    out.directions          = directions;
    out.distancemax         = distancemax;
    out.distancemin         = distancemin;
    out.origin              = origin;
    out.sign                = sign;
    out.instantaneous       = instantaneous;
    out.max_emit_per_period = max_emit_per_period;
    out.speedmin            = speedmin;
    out.speedmax            = speedmax;
    out.audioprocessingmode = audioprocessingmode;
    out.audioamount         = audioamount;
    out.audioexponent       = audioexponent;
    out.audiofrequency      = audiofrequency;
    out.audiobounds         = audiobounds;
    out.controlpoint        = controlpoint;
    out.id                  = id;
    out.flags               = flags;
    out.rate                = rate;
    out.duration            = duration;
    return out;
}

bool Emitter::FromJson(const owe::Json& json) {
    owe::GetJsonValue(json, "name"_str, name);
    owe::GetJsonValue(json, "id"_str, id);
    owe::GetJsonValue(json, "speedmin"_str, speedmin, false);
    owe::GetJsonValue(json, "speedmax"_str, speedmax, false);
    owe::GetJsonValue(json, "instantaneous"_str, instantaneous, false);
    owe::GetJsonValue(json, "maxtoemitperperiod"_str, max_emit_per_period, false);
    owe::GetJsonValue(json, "distancemax"_str, distancemax, false);
    owe::GetJsonValue(json, "distancemin"_str, distancemin, false);
    owe::GetJsonValue(json, "rate"_str, rate, false);
    owe::GetJsonValue(json, "directions"_str, directions, false);
    owe::GetJsonValue(json, "origin"_str, origin, false);
    owe::GetJsonValue(json, "sign"_str, sign, false);
    owe::GetJsonValue(json, "audioprocessingmode"_str, audioprocessingmode, false);
    owe::GetJsonValue(json, "audioamount"_str, audioamount, false);
    owe::GetJsonValue(json, "audioexponent"_str, audioexponent, false);
    owe::GetJsonValue(json, "audiofrequency"_str, audiofrequency, false);
    owe::GetJsonValue(json, "audiobounds"_str, audiobounds, false);
    owe::GetJsonValue(json, "controlpoint"_str, controlpoint, false);
    owe::GetJsonValue(json, "duration"_str, duration, false);

    if (controlpoint >= i32(8)) rstd_error("wrong controlpoint {}", controlpoint);
    controlpoint = controlpoint % i32(8); // limited to 0-7

    u32 _raw_flags { 0 };
    owe::GetJsonValue(json, "flags"_str, _raw_flags, false);
    flags = EFlags(_raw_flags.to_primitive());

    sign = rstd::move(sign).map([](i32 value) {
        if (value > i32()) return i32(1);
        if (value < i32()) return i32(-1);
        return i32();
    });
    return true;
}

bool ParticleInstanceoverride::FromJosn(const owe::Json& json) {
    enabled = true;

    // {"user":"<key>","value":...} indirection -> record the key for the
    // live user-property pipeline. The value still parses normally via
    // GetJsonValue (which already looks through the `value` wrapper).
    auto bind = [&](ref<str> field) {
        auto sub = json.get(field);
        if (sub.is_none() || ! (*sub)->is_object()) return;
        auto user = (*sub)->get("user"_str);
        if (user.is_none()) return;
        auto string = (*user)->as_str();
        if (string.is_some()) (void)bindings.insert(rstd::into(field), rstd::into(*string));
    };

    owe::GetJsonValue(json, "alpha"_str, alpha, false);
    bind("alpha"_str);
    owe::GetJsonValue(json, "size"_str, size, false);
    bind("size"_str);
    owe::GetJsonValue(json, "lifetime"_str, lifetime, false);
    bind("lifetime"_str);
    owe::GetJsonValue(json, "rate"_str, rate, false);
    bind("rate"_str);
    owe::GetJsonValue(json, "speed"_str, speed, false);
    bind("speed"_str);
    owe::GetJsonValue(json, "count"_str, count, false);
    bind("count"_str);
    owe::GetJsonValue(json, "brightness"_str, brightness, false);
    bind("brightness"_str);
    owe::GetJsonValue(json, "id"_str, id, false);
    if (auto value = json.get("color"_str); value.is_some()) {
        owe::GetJsonValue(json, "color"_str, color);
        overColor = true;
        bind("color"_str);
    } else if (auto value = json.get("colorn"_str); value.is_some()) {
        owe::GetJsonValue(json, "colorn"_str, colorn);
        overColorn = true;
        bind("colorn"_str);
    }
    {
        const array<ref<str>, 8> cp_keys  = { "controlpoint0"_str, "controlpoint1"_str,
                                              "controlpoint2"_str, "controlpoint3"_str,
                                              "controlpoint4"_str, "controlpoint5"_str,
                                              "controlpoint6"_str, "controlpoint7"_str };
        const array<ref<str>, 8> cpa_keys = { "controlpointangle0"_str, "controlpointangle1"_str,
                                              "controlpointangle2"_str, "controlpointangle3"_str,
                                              "controlpointangle4"_str, "controlpointangle5"_str,
                                              "controlpointangle6"_str, "controlpointangle7"_str };
        for (usize i {}; i < cp_keys.len(); ++i) {
            auto value = json.get(cp_keys[i]);
            if (value.is_some() && ! (*value)->is_null()) {
                array<float, 3> point {};
                owe::GetJsonValue(json, cp_keys[i], point, false);
                controlpoint[i] = Some(point);
            }
            bind(cp_keys[i]);
            owe::GetJsonValue(json, cpa_keys[i], controlpointangle[i], false);
            bind(cpa_keys[i]);
        }
    }
    FieldBindings field_binding_state;
    AbsorbAllFieldBindings(json, field_binding_state);
    m_field_bindings = Some(Arc<FieldBindings>::make(rstd::move(field_binding_state)));
    return true;
};

bool Particle::FromJson(const owe::Json& json, fs::VFS& vfs) {
    auto emitter_values = json.get("emitter"_str);
    if (emitter_values.is_none()) {
        rstd_error("particle no emitter");
        return false;
    }
    auto emitter_array = (*emitter_values)->as_array();
    if (emitter_array.is_none()) {
        rstd_error("particle emitter is not an array");
        return false;
    }
    for (const auto& el : **emitter_array) {
        Emitter emi;
        emi.FromJson(el);
        emitters.push(rstd::move(emi));
    }
    if (auto values = json.get("renderer"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& el : **array) {
                ParticleRender pr;
                pr.FromJson(el);
                renderers.push(rstd::move(pr));
            }
        }
    }
    // add sprite if no renderers
    if (renderers.is_empty()) {
        ParticleRender pr;
        pr.name = "sprite"_Str;
        renderers.push(rstd::move(pr));
    }
    if (auto values = json.get("initializer"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some())
            for (const auto& el : **array) initializers.push(el.clone());
    }
    if (auto values = json.get("operator"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some())
            for (const auto& el : **array) operators.push(el.clone());
    }
    if (auto values = json.get("controlpoint"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& el : **array) {
                ParticleControlpoint pc;
                pc.FromJson(el);
                controlpoints.push(rstd::move(pc));
            }
        }
    }

    if (auto values = json.get("children"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& el : **array) {
                ParticleChild child;
                if (child.FromJson(el, vfs)) children.push(rstd::move(child));
            }
        }
    }
    if (json.get("material"_str).is_some()) {
        String matPath;
        owe::GetJsonValue(json, "material"_str, matPath);
        auto jMat = LoadAssetJsonFile(vfs, matPath.as_str());
        if (! jMat) return false;
        material.FromJson(*jMat);
    } else {
        rstd_error("particle object no material");
        return false;
    }

    owe::GetJsonValue(json, "animationmode"_str, animationmode, false);
    owe::GetJsonValue(json, "sequencemultiplier"_str, sequencemultiplier, false);
    owe::GetJsonValue(json, "maxcount"_str, maxcount);
    owe::GetJsonValue(json, "starttime"_str, starttime);

    u32 rawflags { 0 };
    owe::GetJsonValue(json, "flags"_str, rawflags, false);
    flags = EFlags(rawflags.to_primitive());

    return true;
}

Particle Particle::Clone() const {
    Particle out;
    out.emitters      = emitters.clone();
    out.renderers     = renderers.clone();
    out.controlpoints = controlpoints.clone();
    for (const auto& value : initializers) out.initializers.push(value.clone());
    for (const auto& value : operators) out.operators.push(value.clone());
    out.material = material.clone();
    out.children.reserve(children.len());
    for (const auto& child : children) out.children.push(child.Clone());
    out.animationmode      = animationmode.clone();
    out.sequencemultiplier = sequencemultiplier;
    out.maxcount           = maxcount;
    out.starttime          = starttime;
    out.flags              = flags;
    return out;
}

bool ParticleObject::FromJson(const owe::Json& json, fs::VFS& vfs) {
    return FromJson(json, vfs, kSceneVersionUnknown);
}

bool ParticleObject::FromAsset(ref<str> asset, fs::VFS& vfs) {
    particle  = rstd::into(asset);
    name      = rstd::into(asset);
    auto json = LoadAssetJsonFile(vfs, asset);
    return json && particleObj.FromJson(*json, vfs);
}

ParticleObject ParticleObject::Clone() const {
    ParticleObject out;
    out.id               = id;
    out.name             = name.clone();
    out.origin           = origin;
    out.scale            = scale;
    out.angles           = angles;
    out.parallax         = parallax;
    out.visible          = visible;
    out.particle         = particle.clone();
    out.particleObj      = particleObj.Clone();
    out.instanceoverride = instanceoverride.clone();
    out.locktransforms   = locktransforms;
    out.muteineditor     = muteineditor;
    out.nointerpolation  = nointerpolation;
    out.reflected        = reflected;
    out.parent           = parent;
    out.attachment       = attachment.clone();
    out.dependencies     = dependencies.clone();
    out.instance         = instance.clone();
    out.particlesrc      = particlesrc.clone();
    out.controlpoint     = controlpoint;
    out.visible_user_key = visible_user_key.clone();

    return out;
}

bool ParticleObject::FromJson(const owe::Json& json, fs::VFS& vfs, SceneVersion /*v*/) {
    owe::GetJsonValue(json, "particle"_str, particle);
    ReadVisibleProperty(json, visible, visible_user);
    visible_user_key = visible_user.name.clone();

    owe::GetJsonValue(json, "name"_str, name, false);
    owe::GetJsonValue(json, "id"_str, id, false);
    owe::GetJsonValue(json, "origin"_str, origin);
    owe::GetJsonValue(json, "angles"_str, angles);
    owe::GetJsonValue(json, "scale"_str, scale);
    ReadParallaxDepth(json, parallax);

    if (auto value = json.get("instanceoverride"_str); value.is_some() && ! (*value)->is_null()) {
        instanceoverride.FromJosn(**value);
    }

    owe::GetJsonValue(json, "locktransforms"_str, locktransforms, false);
    owe::GetJsonValue(json, "muteineditor"_str, muteineditor, false);
    owe::GetJsonValue(json, "nointerpolation"_str, nointerpolation, false);
    owe::GetJsonValue(json, "reflected"_str, reflected, false);
    owe::GetJsonValue(json, "parent"_str, parent, false);
    owe::GetJsonValue(json, "attachment"_str, attachment, false);
    owe::GetJsonValue(json, "dependencies"_str, dependencies, false);
    owe::GetJsonValue(json, "controlpoint"_str, controlpoint, false);
    if (auto value = json.get("instance"_str); value.is_some()) instance = (*value)->clone();
    if (auto value = json.get("particlesrc"_str); value.is_some()) particlesrc = (*value)->clone();

    AbsorbAllFieldBindings(json, field_bindings);

    auto jParticle = LoadAssetJsonFile(vfs, particle.as_str());
    if (! jParticle) return false;
    if (! particleObj.FromJson(*jParticle, vfs)) return false;
    return true;
}

auto owe::wpscene::ParticleInstanceoverride::clone() const -> ParticleInstanceoverride {
    ParticleInstanceoverride out;
    out.enabled           = enabled;
    out.overColor         = overColor;
    out.overColorn        = overColorn;
    out.alpha             = alpha;
    out.count             = count;
    out.lifetime          = lifetime;
    out.rate              = rate;
    out.speed             = speed;
    out.size              = size;
    out.brightness        = brightness;
    out.id                = id;
    out.color             = color;
    out.colorn            = colorn;
    out.controlpoint      = controlpoint;
    out.controlpointangle = controlpointangle;
    out.bindings          = bindings.clone();
    out.m_field_bindings  = m_field_bindings.clone();
    return out;
}
