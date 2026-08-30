module;

#include <rstd/macro.hpp>

module wescene.pkg.scene_obj;
import wescene.core;
import rstd.log;
import rstd.cppstd;

using namespace owe::wpscene;
using namespace rstd::prelude;
using namespace rstd::literals;

namespace
{

auto LoadAssetJsonFile(owe::fs::VFS& vfs, std::string_view path) -> Option<owe::Json> {
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
    owe::GetJsonValue(json, "name", name);

    u32 raw_flags {};
    owe::GetJsonValue(json, "flags", raw_flags, false);
    flags = EFlags(raw_flags.to_primitive());

    if (json.get("type"_str).is_some()) {
        owe::GetJsonValue(json, "type", type);
    } else if (flags[FlagEnum::eventfollow]) {
        // Legacy child entries encode event-follow attachment in flags without a type field.
        type = "eventfollow";
    }

    if (name.empty()) {
        return false;
    }

    auto jParticle = LoadAssetJsonFile(vfs, name);
    if (! jParticle) return false;

    if (! obj.FromJson(*jParticle, vfs)) return false;

    owe::GetJsonValue(json, "maxcount", maxcount, false);
    auto controlpoint_start = json.get("controlpointstartindex"_str);
    if (controlpoint_start.is_some() && ! (*controlpoint_start)->is_null()) {
        i32 value {};
        owe::GetJsonValue(json, "controlpointstartindex", value, false);
        controlpointstartindex = Some(value);
    }
    owe::GetJsonValue(json, "probability", probability, false);
    owe::GetJsonValue(json, "origin", origin, false);
    owe::GetJsonValue(json, "scale", scale, false);
    owe::GetJsonValue(json, "angles", angles, false);
    return true;
}

ParticleChild ParticleChild::Clone() const {
    ParticleChild out;
    out.type                   = type;
    out.name                   = name;
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
    owe::GetJsonValue(json, "id", id);

    u32 _raw_flags { 0 };
    owe::GetJsonValue(json, "flags", _raw_flags, false);
    flags = EFlags(_raw_flags.to_primitive());

    owe::GetJsonValue(json, "offset", offset, false);
    return true;
};

bool ParticleRender::FromJson(const owe::Json& json) {
    owe::GetJsonValue(json, "name", name);

    if (name == "ropetrail") subdivision = 1.0f;
    if (sstart_with(name, "rope")) {
        owe::GetJsonValue(json, "subdivision", subdivision, false);
    }
    if (name == "spritetrail" || name == "ropetrail") {
        owe::GetJsonValue(json, "length", length, false);
        owe::GetJsonValue(json, "maxlength", maxlength, false);
        owe::GetJsonValue(json, "segments", segments, false);
    }
    return true;
}

bool Emitter::FromJson(const owe::Json& json) {
    owe::GetJsonValue(json, "name", name);
    owe::GetJsonValue(json, "id", id);
    owe::GetJsonValue(json, "speedmin", speedmin, false);
    owe::GetJsonValue(json, "speedmax", speedmax, false);
    owe::GetJsonValue(json, "instantaneous", instantaneous, false);
    owe::GetJsonValue(json, "maxtoemitperperiod", max_emit_per_period, false);
    owe::GetJsonValue(json, "distancemax", distancemax, false);
    owe::GetJsonValue(json, "distancemin", distancemin, false);
    owe::GetJsonValue(json, "rate", rate, false);
    owe::GetJsonValue(json, "directions", directions, false);
    owe::GetJsonValue(json, "origin", origin, false);
    owe::GetJsonValue(json, "sign", sign, false);
    owe::GetJsonValue(json, "audioprocessingmode", audioprocessingmode, false);
    owe::GetJsonValue(json, "audioamount", audioamount, false);
    owe::GetJsonValue(json, "audioexponent", audioexponent, false);
    owe::GetJsonValue(json, "audiofrequency", audiofrequency, false);
    owe::GetJsonValue(json, "audiobounds", audiobounds, false);
    owe::GetJsonValue(json, "controlpoint", controlpoint, false);
    owe::GetJsonValue(json, "duration", duration, false);

    if (controlpoint >= i32(8)) rstd_error("wrong controlpoint {}", controlpoint);
    controlpoint = controlpoint % i32(8); // limited to 0-7

    u32 _raw_flags { 0 };
    owe::GetJsonValue(json, "flags", _raw_flags, false);
    flags = EFlags(_raw_flags.to_primitive());

    std::transform(sign.begin(), sign.end(), sign.begin(), [](i32 value) {
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
        if (string.is_some())
            bindings[rstd::cppstd::to_string(field)] = rstd::cppstd::to_string(*string);
    };

    owe::GetJsonValue(json, "alpha", alpha, false);
    bind("alpha"_str);
    owe::GetJsonValue(json, "size", size, false);
    bind("size"_str);
    owe::GetJsonValue(json, "lifetime", lifetime, false);
    bind("lifetime"_str);
    owe::GetJsonValue(json, "rate", rate, false);
    bind("rate"_str);
    owe::GetJsonValue(json, "speed", speed, false);
    bind("speed"_str);
    owe::GetJsonValue(json, "count", count, false);
    bind("count"_str);
    owe::GetJsonValue(json, "brightness", brightness, false);
    bind("brightness"_str);
    owe::GetJsonValue(json, "id", id, false);
    if (auto value = json.get("color"_str); value.is_some()) {
        owe::GetJsonValue(json, "color", color);
        overColor = true;
        bind("color"_str);
    } else if (auto value = json.get("colorn"_str); value.is_some()) {
        owe::GetJsonValue(json, "colorn", colorn);
        overColorn = true;
        bind("colorn"_str);
    }
    {
        const char* cp_keys[]  = { "controlpoint0", "controlpoint1", "controlpoint2",
                                   "controlpoint3", "controlpoint4", "controlpoint5",
                                   "controlpoint6", "controlpoint7" };
        const char* cpa_keys[] = { "controlpointangle0", "controlpointangle1", "controlpointangle2",
                                   "controlpointangle3", "controlpointangle4", "controlpointangle5",
                                   "controlpointangle6", "controlpointangle7" };
        for (int i = 0; i < 8; ++i) {
            auto value = json.get(rstd::cppstd::as_str(cp_keys[i]).unwrap());
            if (value.is_some() && ! (*value)->is_null()) {
                std::array<float, 3> point {};
                owe::GetJsonValue(json, cp_keys[i], point, false);
                controlpoint[i] = Some(point);
            }
            bind(rstd::cppstd::as_str(cp_keys[i]).unwrap());
            owe::GetJsonValue(json, cpa_keys[i], controlpointangle[i], false);
            bind(rstd::cppstd::as_str(cpa_keys[i]).unwrap());
        }
    }
    auto field_binding_state = std::make_shared<FieldBindings>();
    AbsorbAllFieldBindings(json, *field_binding_state);
    field_bindings = std::move(field_binding_state);
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
        emitters.push_back(std::move(emi));
    }
    if (auto values = json.get("renderer"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& el : **array) {
                ParticleRender pr;
                pr.FromJson(el);
                renderers.push_back(std::move(pr));
            }
        }
    }
    // add sprite if no renderers
    if (renderers.empty()) {
        ParticleRender pr;
        pr.name = "sprite";
        renderers.push_back(pr);
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
                controlpoints.push_back(std::move(pc));
            }
        }
    }

    if (auto values = json.get("children"_str); values.is_some()) {
        auto array = (*values)->as_array();
        if (array.is_some()) {
            for (const auto& el : **array) {
                ParticleChild child;
                if (child.FromJson(el, vfs)) children.push_back(std::move(child));
            }
        }
    }
    if (json.get("material"_str).is_some()) {
        std::string matPath;
        owe::GetJsonValue(json, "material", matPath);
        auto jMat = LoadAssetJsonFile(vfs, matPath);
        if (! jMat) return false;
        material.FromJson(*jMat);
    } else {
        rstd_error("particle object no material");
        return false;
    }

    owe::GetJsonValue(json, "animationmode", animationmode, false);
    owe::GetJsonValue(json, "sequencemultiplier", sequencemultiplier, false);
    owe::GetJsonValue(json, "maxcount", maxcount);
    owe::GetJsonValue(json, "starttime", starttime);

    u32 rawflags { 0 };
    owe::GetJsonValue(json, "flags", rawflags, false);
    flags = EFlags(rawflags.to_primitive());

    return true;
}

Particle Particle::Clone() const {
    Particle out;
    out.emitters      = emitters;
    out.renderers     = renderers;
    out.controlpoints = controlpoints;
    for (const auto& value : initializers) out.initializers.push(value.clone());
    for (const auto& value : operators) out.operators.push(value.clone());
    out.material = material.clone();
    out.children.reserve(children.size());
    for (const auto& child : children) out.children.push_back(child.Clone());
    out.animationmode      = animationmode;
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
    particle  = rstd::cppstd::to_string(asset);
    name      = particle;
    auto json = LoadAssetJsonFile(vfs, rstd::cppstd::as_string_view(asset));
    return json && particleObj.FromJson(*json, vfs);
}

ParticleObject ParticleObject::Clone() const {
    ParticleObject out;
    out.id               = id;
    out.name             = name;
    out.origin           = origin;
    out.scale            = scale;
    out.angles           = angles;
    out.parallax           = parallax;
    out.visible          = visible;
    out.particle         = particle;
    out.particleObj      = particleObj.Clone();
    out.instanceoverride = instanceoverride;
    out.locktransforms   = locktransforms;
    out.muteineditor     = muteineditor;
    out.nointerpolation  = nointerpolation;
    out.reflected        = reflected;
    out.parent           = parent;
    out.attachment       = attachment;
    out.dependencies     = dependencies;
    out.instance         = instance.clone();
    out.particlesrc      = particlesrc.clone();
    out.controlpoint     = controlpoint;
    out.visible_user_key = visible_user_key;

    return out;
}

bool ParticleObject::FromJson(const owe::Json& json, fs::VFS& vfs, SceneVersion /*v*/) {
    owe::GetJsonValue(json, "particle", particle);
    ReadVisibleProperty(json, visible, visible_user);
    visible_user_key = visible_user.name;

    owe::GetJsonValue(json, "name", name, false);
    owe::GetJsonValue(json, "id", id, false);
    owe::GetJsonValue(json, "origin", origin);
    owe::GetJsonValue(json, "angles", angles);
    owe::GetJsonValue(json, "scale", scale);
    ReadParallaxDepth(json, parallax);

    if (auto value = json.get("instanceoverride"_str); value.is_some() && ! (*value)->is_null()) {
        instanceoverride.FromJosn(**value);
    }

    owe::GetJsonValue(json, "locktransforms", locktransforms, false);
    owe::GetJsonValue(json, "muteineditor", muteineditor, false);
    owe::GetJsonValue(json, "nointerpolation", nointerpolation, false);
    owe::GetJsonValue(json, "reflected", reflected, false);
    owe::GetJsonValue(json, "parent", parent, false);
    owe::GetJsonValue(json, "attachment", attachment, false);
    owe::GetJsonValue(json, "dependencies", dependencies, false);
    owe::GetJsonValue(json, "controlpoint", controlpoint, false);
    if (auto value = json.get("instance"_str); value.is_some()) instance = (*value)->clone();
    if (auto value = json.get("particlesrc"_str); value.is_some()) particlesrc = (*value)->clone();

    AbsorbAllFieldBindings(json, field_bindings);

    auto jParticle = LoadAssetJsonFile(vfs, particle);
    if (! jParticle) return false;
    if (! particleObj.FromJson(*jParticle, vfs)) return false;
    return true;
}
