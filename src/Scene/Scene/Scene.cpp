module;

#include <rstd/macro.hpp>

module wescene.scene;
import eigen;
import rstd;
import rstd.log;
import rstd.cppstd;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::collections::BTreeSet;
using rstd::collections::HashMap;
using rstd::collections::HashSet;
using rstd::cppstd::as_str;
using rstd::sync::Arc;
using rstd::sync::Mutex;
using rstd::sync::Weak;

namespace owe
{

struct AudioResponseDemand::State {
    struct Fields {
        usize            leases {};
        bool             enabled { true };
        Option<Callback> callback;
    };

    Mutex<Fields> fields { Fields {} };
};

struct AudioResponseDemand::Lease {
    Weak<State> state;

    explicit Lease(Weak<State> value): state(rstd::move(value)) {}
    Lease(const Lease&)                = delete;
    Lease& operator=(const Lease&)     = delete;
    Lease(Lease&&) noexcept            = default;
    Lease& operator=(Lease&&) noexcept = default;
    ~Lease() {
        auto owner = state.upgrade();
        if (owner) AudioResponseDemand::Update(*owner, i32(-1));
    }

    void KeepAlive() const {}
};

void AudioResponseDemand::Update(State& state, i32 delta) {
    Option<Callback> callback;
    bool             active = false;
    {
        auto       fields = state.fields.lock().unwrap_unchecked();
        const bool before = fields->enabled && fields->leases != usize();
        if (delta > i32())
            fields->leases += usize(delta.to_primitive());
        else if (fields->leases != usize())
            fields->leases -= usize((-delta).to_primitive());
        active = fields->enabled && fields->leases != usize();
        if (active != before && fields->callback.is_some())
            callback = Some((*fields->callback).clone());
    }
    if (callback.is_some()) (*callback)->operator()(active);
}

AudioResponseDemand::AudioResponseDemand(): m_state(Arc<State>::make()) {}
AudioResponseDemand::~AudioResponseDemand() = default;

auto AudioResponseDemand::Acquire() -> Box<dyn<UniformBindingLease>> {
    Update(*m_state, i32(1));
    return Box<dyn<UniformBindingLease>>::make(Lease(m_state.downgrade()));
}

void AudioResponseDemand::SetCallback(Option<Callback> callback) {
    bool             active;
    Option<Callback> notify;
    {
        auto fields      = m_state->fields.lock().unwrap_unchecked();
        fields->callback = rstd::move(callback);
        active           = fields->enabled && fields->leases != usize();
        if (fields->callback.is_some()) notify = Some((*fields->callback).clone());
    }
    if (notify.is_some()) (*notify)->operator()(active);
}

void AudioResponseDemand::SetEnabled(bool enabled) {
    Option<Callback> callback;
    bool             active;
    {
        auto       fields = m_state->fields.lock().unwrap_unchecked();
        const bool before = fields->enabled && fields->leases != usize();
        fields->enabled   = enabled;
        active            = fields->enabled && fields->leases != usize();
        if (active != before && fields->callback.is_some())
            callback = Some((*fields->callback).clone());
    }
    if (callback.is_some()) (*callback)->operator()(active);
}

bool AudioResponseDemand::Active() const {
    auto fields = m_state->fields.lock().unwrap_unchecked();
    return fields->enabled && fields->leases != usize();
}

namespace
{
u32 next_scene_resource_generation() {
    static std::atomic<rstd::uint32_t> next { 1 };
    return u32(next.fetch_add(1, std::memory_order_relaxed));
}

u64 next_render_scene_version() {
    static std::atomic<rstd::uint64_t> next { 1 };
    return u64(next.fetch_add(1, std::memory_order_relaxed));
}

template<typename T>
bool same_ids(const HashSet<T>& lhs, const HashSet<T>& rhs) {
    if (lhs.len() != rhs.len()) return false;
    auto ids = lhs.iter();
    for (auto id = ids.next(); id.is_some(); id = ids.next()) {
        if (! rhs.contains(**id)) return false;
    }
    return true;
}

u32 index_from_size(std::size_t size) { return rstd::as_cast<u32>(usize(size)); }

usize index_from_id(u32 index) { return usize(index.to_primitive()); }

template<typename Id>
bool valid_index(Id id, u32 generation, std::size_t size) {
    return id.generation == generation && id.index.to_primitive() < size;
}

template<typename Id>
bool valid_render_index(Id id, u64 generation, std::size_t size) {
    return id.generation == generation && id.index.to_primitive() < size;
}

rstd::uint64_t scene_id_key(u32 index, u32 generation) {
    return (static_cast<rstd::uint64_t>(generation.to_primitive()) << 32) |
           static_cast<rstd::uint64_t>(index.to_primitive());
}

rstd::uint64_t scene_id_key(SceneDrawItemId id) { return scene_id_key(id.index, id.generation); }

rstd::uint64_t scene_id_key(SceneNodeId id) { return scene_id_key(id.index, id.generation); }

rstd::uint64_t scene_id_key(SceneMaterialId id) { return scene_id_key(id.index, id.generation); }

rstd::uint64_t scene_id_key(SceneMeshId id) { return scene_id_key(id.index, id.generation); }

rstd::uint64_t draw_item_key(SceneNodeId node, u32 submesh_index) {
    return (static_cast<rstd::uint64_t>(node.index.to_primitive()) << 32) |
           static_cast<rstd::uint64_t>(submesh_index.to_primitive());
}

float cubic(float p0, float p1, float p2, float p3, float t) {
    float omt = 1.0f - t;
    return omt * omt * omt * p0 + 3.0f * omt * omt * t * p1 + 3.0f * omt * t * t * p2 +
           t * t * t * p3;
}

struct AnimationFrame {
    float current { 0.0f };
    i32   end {};
    bool  wraps { false };
};

// Last frame of the timeline: the authored length, stretched to cover a
// keyframe that sits past it.
i32 curve_end(const SceneAnimationCurve& curve) {
    i32  end         = curve.length;
    auto absorb_last = [&end](slice<SceneAnimationKey> keys) {
        if (! keys.is_empty()) end = std::max(end, keys[keys.len() - usize(1)].frame);
    };
    absorb_last(curve.c0.as_slice());
    absorb_last(curve.c1.as_slice());
    absorb_last(curve.c2.as_slice());
    return end;
}

bool curve_loops(const SceneAnimationCurve& curve) {
    return curve.wraploop || curve.mode == "loop"_str || curve.mode == "repeat"_str;
}

AnimationFrame animation_frame(const SceneAnimationCurve& curve, double runtime) {
    float fps   = curve.fps > 0.0f ? curve.fps : 30.0f;
    float frame = static_cast<float>(runtime) * fps;
    i32   end   = curve_end(curve);
    if (end <= i32()) return { .current = frame, .end = end };

    const float ef = rstd::as_cast<float>(end);
    if (curve.mode == "mirror"_str) {
        float period = 2.0f * ef;
        float m      = std::fmod(frame, period);
        if (m < 0.0f) m += period;
        return { .current = m <= ef ? m : (period - m), .end = end };
    }

    if (curve_loops(curve)) {
        frame = std::fmod(frame, ef);
        if (frame < 0.0f) frame += ef;
        return { .current = frame, .end = end, .wraps = curve.wraploop };
    }
    return { .current = std::clamp(frame, 0.0f, ef), .end = end };
}

float eval_segment(const SceneAnimationKey& a, const SceneAnimationKey& b, float frame) {
    float dt = rstd::as_cast<float>(b.frame - a.frame);
    if (dt <= 0.0f) return b.value;
    float linear_t = std::clamp((frame - rstd::as_cast<float>(a.frame)) / dt, 0.0f, 1.0f);
    bool  has_tan  = a.front_enabled || b.back_enabled;
    if (! has_tan) return std::lerp(a.value, b.value, linear_t);

    float p0x = rstd::as_cast<float>(a.frame);
    float p3x = rstd::as_cast<float>(b.frame);
    float p1x = a.front_enabled ? p0x + a.front_x : p0x + dt / 3.0f;
    float p2x = b.back_enabled ? p3x + b.back_x : p3x - dt / 3.0f;
    float p0y = a.value;
    float p3y = b.value;
    float p1y = a.front_enabled ? p0y + a.front_y : std::lerp(p0y, p3y, 1.0f / 3.0f);
    float p2y = b.back_enabled ? p3y + b.back_y : std::lerp(p0y, p3y, 2.0f / 3.0f);

    if (! (p0x <= p1x && p1x <= p2x && p2x <= p3x)) return std::lerp(a.value, b.value, linear_t);

    float lo = 0.0f;
    float hi = 1.0f;
    for (int i = 0; i < 16; ++i) {
        float mid = (lo + hi) * 0.5f;
        if (cubic(p0x, p1x, p2x, p3x, mid) < frame)
            lo = mid;
        else
            hi = mid;
    }
    return cubic(p0y, p1y, p2y, p3y, (lo + hi) * 0.5f);
}

float eval_axis(slice<SceneAnimationKey> keys, const AnimationFrame& frame) {
    if (keys.is_empty()) return 0.0f;
    const auto& first = keys[usize()];
    const auto& last  = keys[keys.len() - usize(1)];

    if (frame.wraps && frame.current < rstd::as_cast<float>(first.frame)) {
        auto previous = last;
        previous.frame -= frame.end;
        if (previous.frame < first.frame) return eval_segment(previous, first, frame.current);
    }
    if (frame.current <= rstd::as_cast<float>(first.frame)) return first.value;
    for (usize i(1); i < keys.len(); ++i) {
        if (frame.current <= rstd::as_cast<float>(keys[i].frame))
            return eval_segment(keys[i - usize(1)], keys[i], frame.current);
    }
    if (frame.wraps) {
        auto next = first;
        next.frame += frame.end;
        if (next.frame > last.frame) return eval_segment(last, next, frame.current);
    }
    return last.value;
}

Eigen::Vector3f lerp_vec3(const Eigen::Vector3f& a, const Eigen::Vector3f& b, float t) {
    return a + (b - a) * t;
}

SceneCameraLookAtKey eval_lookat_track(const SceneCameraLookAtTrack& track, float frame) {
    if (track.keys.is_empty()) return {};
    if (frame <= track.keys[usize()].frame) return track.keys[usize()];
    for (usize i(1); i < track.keys.len(); ++i) {
        const auto& a = track.keys[i - usize(1)];
        const auto& b = track.keys[i];
        if (frame > b.frame) continue;
        float dt = b.frame - a.frame;
        float t  = dt > 0.0f ? std::clamp((frame - a.frame) / dt, 0.0f, 1.0f) : 1.0f;
        return {
            .frame  = frame,
            .eye    = lerp_vec3(a.eye, b.eye, t),
            .center = lerp_vec3(a.center, b.center, t),
            .up     = lerp_vec3(a.up, b.up, t).normalized(),
        };
    }
    return track.keys[track.keys.len() - usize(1)];
}

Option<SceneCameraLookAtKey> eval_lookat_tracks(slice<SceneCameraLookAtTrack> tracks,
                                                double runtime, float fps) {
    float total = 0.0f;
    for (usize index {}; index < tracks.len(); ++index)
        total += std::max(tracks[index].duration, 0.0f);
    if (total <= 0.0f) return None();

    float frame = static_cast<float>(runtime) * (fps > 0.0f ? fps : 1.0f);
    frame       = std::fmod(frame, total);
    if (frame < 0.0f) frame += total;

    float offset = 0.0f;
    for (usize index {}; index < tracks.len(); ++index) {
        const auto& track    = tracks[index];
        float       duration = std::max(track.duration, 0.0f);
        if (duration <= 0.0f) continue;
        if (frame <= offset + duration) return Some(eval_lookat_track(track, frame - offset));
        offset += duration;
    }
    const auto& last = tracks[tracks.len() - usize(1)];
    return Some(eval_lookat_track(last, last.duration));
}

bool shader_values_equal(const ShaderValue& a, const ShaderValue& b) {
    if (a.size() != b.size() || a.View().layout != b.View().layout) return false;
    for (usize i {}; i < a.size(); ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

ShaderValue eval_shader_value_animation(const SceneShaderValueAnimation& animation,
                                        double                           runtime) {
    if (animation.curve.is_none() || (**animation.curve).Empty() ||
        animation.base.size() == usize())
        return animation.base;

    std::vector<float> value(animation.base.size().to_primitive());
    for (std::size_t i = 0; i < value.size(); ++i) value[i] = animation.base[usize(i)];

    if (value.size() == 1) {
        value[0] = (**animation.curve).EvaluateScalar(value[0], runtime);
        return ShaderValue(UniformValueView {
            .data   = value.data(),
            .size   = usize(value.size()),
            .layout = animation.base.View().layout,
        });
    }

    Eigen::Vector3f base { value[0],
                           value.size() > 1 ? value[1] : 0.0f,
                           value.size() > 2 ? value[2] : 0.0f };
    auto            animated = (**animation.curve).EvaluateVec3(base, runtime);
    value[0]                 = animated.x();
    if (value.size() > 1) value[1] = animated.y();
    if (value.size() > 2) value[2] = animated.z();
    return ShaderValue(UniformValueView {
        .data   = value.data(),
        .size   = usize(value.size()),
        .layout = animation.base.View().layout,
    });
}

void collect_linked_ids_from_material(SceneMaterial& material, Scene& scene, BTreeSet<i32>& out) {
    scene.ResolveMaterialTextureSources(material);
    for (const auto& source : material.texture_sources) {
        if (source.kind == SceneMaterialTextureSourceKind::LayerOutput &&
            source.wallpaper_layer >= i32()) {
            out.insert(source.wallpaper_layer);
        }
    }
}

void collect_linked_ids_from_node(SceneNode* node, Scene& scene, BTreeSet<i32>& out) {
    if (node == nullptr) return;
    if (auto* mesh = node->Mesh(); mesh != nullptr) {
        for (auto& material : mesh->MaterialSlots()) {
            if (material) collect_linked_ids_from_material(*material, scene, out);
        }
    }
    if (node->HasLayer()) {
        auto& effect_layer = node->Layer();
        for (auto& prefill : effect_layer->PrefillNodes()) {
            collect_linked_ids_from_node(prefill.sceneNode.as_ptr(), scene, out);
        }
        auto collect_effect = [&](const std::shared_ptr<SceneImageEffect>& effect) {
            if (! effect) return;
            for (auto& effect_node : effect->nodes) {
                collect_linked_ids_from_node(effect_node.sceneNode.as_ptr(), scene, out);
            }
        };
        for (usize i {}; i < effect_layer->EffectCount(); ++i) {
            collect_effect(effect_layer->GetEffect(i));
        }
        collect_effect(effect_layer->FinalResolveEffect());
        collect_effect(effect_layer->PublishedEffect());
        collect_effect(effect_layer->VisibleResolveEffect());
    }
    for (auto& child : node->GetChildren())
        collect_linked_ids_from_node(child.as_ptr(), scene, out);
}

void collect_linked_ids_from_scene(Scene& scene, BTreeSet<i32>& out) {
    collect_linked_ids_from_node(scene.RootMut().as_raw_ptr(), scene, out);
    auto post_processes = scene.PostProcesses();
    for (usize index {}; index < post_processes.len(); ++index) {
        const auto& pp = post_processes[index];
        for (auto& step : pp->steps) {
            if (step.is_Pass()) {
                collect_linked_ids_from_node(step.as_Pass().value.node.as_ptr(), scene, out);
            }
        }
    }
}

void ensure_snapshot_link_render_targets(Scene& scene, const BTreeSet<i32>& linked_ids) {
    auto ids = linked_ids.iter();
    for (auto next = ids.next(); next.is_some(); next = ids.next()) {
        auto id    = **next;
        auto layer = WallpaperLayerId { .value = id };
        auto key   = GenLinkTex(static_cast<std::ptrdiff_t>(id.to_primitive()));
        if (scene.RenderTarget(as_str(key).unwrap()).is_some()) continue;
        auto* source = scene.RegisteredLayerLinkSource(layer);
        if (source == nullptr) {
            rstd_error("linked layer {} has no registered composite producer", id);
            continue;
        }
        scene.EnsureLinkRenderTarget(layer, *source);
    }
}
} // namespace

void SceneResourceIndex::Rebuild(Scene& scene, u32 generation) {
    const bool preserve_node_ids = m_scene == &scene && m_generation == generation;
    HashMap<rstd::uint64_t, SceneDrawItemId> preserved_draw_ids;
    usize                                    preserved_draw_count {};
    if (preserve_node_ids) {
        preserved_draw_count = m_draw_items.len();
        preserved_draw_ids.reserve(m_draw_items.len());
        for (const auto& item : m_draw_items) {
            if (! item.id.Valid() || ! item.node.Valid()) continue;
            (void)preserved_draw_ids.insert(draw_item_key(item.node, item.submesh_index), item.id);
        }
    }

    m_scene      = &scene;
    m_generation = generation;

    if (! preserve_node_ids) {
        m_nodes.clear();
        m_node_ids.clear();
        m_meshes.clear();
        m_mesh_ids.clear();
        m_materials.clear();
        m_material_ids.clear();
    } else {
        for (auto& node : m_nodes) node = nullptr;
        for (auto& mesh : m_meshes) mesh = nullptr;
        for (auto& material : m_materials) material = nullptr;
    }
    m_texture_keys.clear();
    m_render_target_keys.clear();
    m_camera_keys.clear();
    m_draw_items.clear();
    m_draw_items.resize(preserved_draw_count, SceneDrawItemRecord {});
    m_texture_ids.clear();
    m_render_target_ids.clear();
    m_camera_ids.clear();

    auto register_mesh = [this, generation](SceneMesh& mesh) {
        if (auto found = m_mesh_ids.get(&mesh); found.is_some()) {
            auto id = **found;
            if (valid_index(id, generation, m_meshes.len().to_primitive())) {
                m_meshes[index_from_id(id.index)] = &mesh;
                return id;
            }
            (void)m_mesh_ids.remove(&mesh);
        }
        SceneMeshId id { .index      = index_from_size(m_meshes.len().to_primitive()),
                         .generation = generation };
        m_meshes.push(&mesh);
        (void)m_mesh_ids.insert(&mesh, id);
        return id;
    };

    auto register_material = [this, generation, &scene](SceneMaterial& material) {
        scene.ResolveMaterialTextureSources(material);
        if (auto found = m_material_ids.get(&material); found.is_some()) {
            auto id = **found;
            if (valid_index(id, generation, m_materials.len().to_primitive())) {
                m_materials[index_from_id(id.index)] = &material;
                return id;
            }
            (void)m_material_ids.remove(&material);
        }
        SceneMaterialId id { .index      = index_from_size(m_materials.len().to_primitive()),
                             .generation = generation };
        m_materials.push(&material);
        (void)m_material_ids.insert(&material, id);
        return id;
    };

    auto register_draw_items = [&](SceneNode& node, SceneNodeId node_id) {
        auto* mesh = node.Mesh();
        if (mesh == nullptr) return;

        SceneMeshId mesh_id = register_mesh(*mesh);
        const auto& slots   = mesh->MaterialSlots();
        const auto& parts   = mesh->Submeshes();
        for (std::size_t smi = 0; smi < parts.size(); ++smi) {
            const auto& submesh    = parts[smi];
            auto        slot_index = submesh.material_slot.to_primitive();
            if (slot_index >= slots.size() || ! slots[slot_index]) continue;

            SceneMaterialId material_id   = register_material(*slots[slot_index]);
            auto            submesh_index = rstd::as_cast<u32>(usize(smi));
            auto preserved = preserved_draw_ids.get(draw_item_key(node_id, submesh_index));
            SceneDrawItemId draw_id =
                preserved.is_some()
                    ? **preserved
                    : SceneDrawItemId {
                          .index      = index_from_size(m_draw_items.len().to_primitive()),
                          .generation = generation,
                      };
            SceneDrawItemRecord record { .id            = draw_id,
                                         .node          = node_id,
                                         .mesh          = mesh_id,
                                         .material      = material_id,
                                         .submesh_index = submesh_index };
            if (draw_id.index.to_primitive() < m_draw_items.len().to_primitive())
                m_draw_items[index_from_id(draw_id.index)] = record;
            else
                m_draw_items.push(rstd::move(record));
        }
    };

    auto register_node = [&](SceneNode& node) {
        auto       id       = scene.RegisterNode(node);
        const auto required = static_cast<std::size_t>(id.index.to_primitive()) + 1;
        if (m_nodes.len().to_primitive() < required) m_nodes.resize(usize(required), nullptr);
        m_nodes[index_from_id(id.index)] = &node;
        (void)m_node_ids.insert(&node, id);
        return id;
    };

    HashSet<SceneNode*> visited;
    auto                collect_node = [&](auto& self, SceneNode* node) -> void {
        if (node == nullptr) return;
        if (! visited.insert(node)) return;

        register_node(*node);

        if (node->HasLayer()) {
            auto& layer = node->Layer();
            for (auto& prefill : layer->PrefillNodes()) {
                self(self, prefill.sceneNode.as_ptr());
            }
            for (usize ei {}; ei < layer->EffectCount(); ++ei) {
                auto& effect = layer->GetEffect(ei);
                for (auto& effect_node : effect->nodes) self(self, effect_node.sceneNode.as_ptr());
            }
            auto collect_internal = [&](const std::shared_ptr<SceneImageEffect>& effect) {
                if (! effect) return;
                for (auto& effect_node : effect->nodes) self(self, effect_node.sceneNode.as_ptr());
            };
            collect_internal(layer->FinalResolveEffect());
            collect_internal(layer->PublishedEffect());
            collect_internal(layer->VisibleResolveEffect());
        }

        for (auto& child : node->GetChildren()) self(self, child.as_ptr());
    };

    collect_node(collect_node, scene.RootMut().as_raw_ptr());
    auto post_processes = scene.PostProcesses();
    for (usize index {}; index < post_processes.len(); ++index) {
        const auto& pp = post_processes[index];
        for (auto& step : pp->steps) {
            if (step.is_Pass()) {
                collect_node(collect_node, step.as_Pass().value.node.as_ptr());
            }
        }
    }

    m_node_ids.retain([this](const SceneNode* pointer, SceneNodeId& id) {
        return node(id) == pointer;
    });

    for (usize index {}; index < m_nodes.len(); ++index) {
        if (m_nodes[index] == nullptr) continue;
        register_draw_items(
            *m_nodes[index],
            SceneNodeId { .index = rstd::as_cast<u32>(index), .generation = generation });
    }

    m_mesh_ids.retain([this](const SceneMesh* pointer, SceneMeshId& id) {
        return mesh(id) == pointer;
    });
    m_material_ids.retain([this](const SceneMaterial* pointer, SceneMaterialId& id) {
        return material(id) == pointer;
    });

    auto collect_texture_ids = [generation, this]() {
        auto names = m_scene->TextureNames();
        m_texture_keys.reserve(names.len());
        for (usize index {}; index < names.len(); ++index) {
            m_texture_keys.push(names[index].clone());
        }
        rstd::slice_::sort_unstable(m_texture_keys.as_mut_slice().as_mut_ref());
        for (usize i {}; i < m_texture_keys.len(); ++i) {
            (void)m_texture_ids.insert(
                m_texture_keys[i].clone(),
                SceneTextureId { .index = rstd::as_cast<u32>(i), .generation = generation });
        }
    };

    auto collect_render_target_ids = [generation, this]() {
        auto names = m_scene->RenderTargetNames();
        m_render_target_keys.reserve(names.len());
        for (usize index {}; index < names.len(); ++index) {
            m_render_target_keys.push(names[index].clone());
        }
        rstd::slice_::sort_unstable(m_render_target_keys.as_mut_slice().as_mut_ref());
        for (usize i {}; i < m_render_target_keys.len(); ++i) {
            (void)m_render_target_ids.insert(
                m_render_target_keys[i].clone(),
                SceneRenderTargetId { .index = rstd::as_cast<u32>(i), .generation = generation });
        }
    };

    auto collect_camera_ids = [generation, this]() {
        auto names = m_scene->CameraNames();
        m_camera_keys.reserve(names.len());
        for (usize index {}; index < names.len(); ++index) {
            m_camera_keys.push(names[index].clone());
        }
        rstd::slice_::sort_unstable(m_camera_keys.as_mut_slice().as_mut_ref());
        for (usize i {}; i < m_camera_keys.len(); ++i) {
            (void)m_camera_ids.insert(
                m_camera_keys[i].clone(),
                SceneCameraId { .index = rstd::as_cast<u32>(i), .generation = generation });
        }
    };

    collect_texture_ids();
    collect_render_target_ids();
    collect_camera_ids();
}

Option<SceneNodeId> SceneResourceIndex::nodeId(const SceneNode& node) const {
    auto id = m_node_ids.get(&node);
    return id.is_some() ? Some<SceneNodeId>(**id) : None();
}

Option<SceneMeshId> SceneResourceIndex::meshId(const SceneMesh& mesh) const {
    auto id = m_mesh_ids.get(&mesh);
    return id.is_some() ? Some<SceneMeshId>(**id) : None();
}

Option<SceneMaterialId> SceneResourceIndex::materialId(const SceneMaterial& material) const {
    auto id = m_material_ids.get(&material);
    return id.is_some() ? Some<SceneMaterialId>(**id) : None();
}

Option<SceneDrawItemId> SceneResourceIndex::drawItemFor(SceneNodeId node, u32 submesh_index) const {
    if (node.generation != m_generation) return None();
    for (const auto& item : m_draw_items) {
        if (! item.id.Valid()) continue;
        if (item.node.index == node.index && item.submesh_index == submesh_index)
            return Some<SceneDrawItemId>(item.id);
    }
    return None();
}

Option<SceneTextureId> SceneResourceIndex::textureId(ref<str> url) const {
    auto id = m_texture_ids.get(url);
    return id.is_some() ? Some<SceneTextureId>(**id) : None();
}

Option<SceneRenderTargetId> SceneResourceIndex::renderTargetId(ref<str> key) const {
    auto id = m_render_target_ids.get(key);
    return id.is_some() ? Some<SceneRenderTargetId>(**id) : None();
}

Option<SceneCameraId> SceneResourceIndex::cameraId(ref<str> name) const {
    auto id = m_camera_ids.get(name);
    return id.is_some() ? Some<SceneCameraId>(**id) : None();
}

Option<DrawItemView> SceneResourceIndex::resolve(SceneDrawItemId id) const {
    if (! valid_index(id, m_generation, m_draw_items.len().to_primitive())) return None();
    const auto& item = m_draw_items[index_from_id(id.index)];
    auto*       n    = node(item.node);
    auto*       me   = mesh(item.mesh);
    auto*       ma   = material(item.material);
    if (n == nullptr || me == nullptr || ma == nullptr) return None();
    if (item.submesh_index.to_primitive() >= me->Submeshes().size()) return None();
    return Some<DrawItemView>(
        DrawItemView { .node          = n,
                       .mesh          = me,
                       .submesh       = &me->Submeshes()[item.submesh_index.to_primitive()],
                       .material      = ma,
                       .submesh_index = item.submesh_index });
}

SceneNode* SceneResourceIndex::node(SceneNodeId id) const {
    if (! valid_index(id, m_generation, m_nodes.len().to_primitive())) return nullptr;
    return m_nodes[index_from_id(id.index)];
}

SceneMesh* SceneResourceIndex::mesh(SceneMeshId id) const {
    if (! valid_index(id, m_generation, m_meshes.len().to_primitive())) return nullptr;
    return m_meshes[index_from_id(id.index)];
}

SceneMaterial* SceneResourceIndex::material(SceneMaterialId id) const {
    if (! valid_index(id, m_generation, m_materials.len().to_primitive())) return nullptr;
    return m_materials[index_from_id(id.index)];
}

const SceneTexture* SceneResourceIndex::texture(SceneTextureId id) const {
    if (m_scene == nullptr || ! valid_index(id, m_generation, m_texture_keys.len().to_primitive()))
        return nullptr;
    auto texture = m_scene->Texture(m_texture_keys[index_from_id(id.index)].as_str());
    return texture.is_some() ? (*texture).as_raw_ptr() : nullptr;
}

const SceneRenderTarget* SceneResourceIndex::renderTarget(SceneRenderTargetId id) const {
    if (m_scene == nullptr ||
        ! valid_index(id, m_generation, m_render_target_keys.len().to_primitive()))
        return nullptr;
    auto target = m_scene->RenderTarget(m_render_target_keys[index_from_id(id.index)].as_str());
    return target.is_some() ? (*target).as_raw_ptr() : nullptr;
}

SceneRenderTarget* SceneResourceIndex::mutableRenderTarget(SceneRenderTargetId id) const {
    if (m_scene == nullptr ||
        ! valid_index(id, m_generation, m_render_target_keys.len().to_primitive()))
        return nullptr;
    auto target = m_scene->RenderTargetMut(m_render_target_keys[index_from_id(id.index)].as_str());
    return target.is_some() ? (*target).as_raw_ptr() : nullptr;
}

SceneCamera* SceneResourceIndex::camera(SceneCameraId id) const {
    if (m_scene == nullptr || ! valid_index(id, m_generation, m_camera_keys.len().to_primitive()))
        return nullptr;
    auto camera = m_scene->CameraMut(m_camera_keys[index_from_id(id.index)].as_str());
    return camera.is_some() ? (*camera).as_raw_ptr() : nullptr;
}

void RenderSceneSnapshot::Rebuild(Scene& scene, RenderSceneVersion version) {
    m_version = version;

    m_render_items.clear();
    m_texture_descs.clear();
    m_render_target_descs.clear();
    m_shadow_definitions.clear();
    m_shadow_casters.clear();
    m_render_item_ids.clear();
    m_texture_desc_ids.clear();
    m_render_target_desc_ids.clear();
    m_source_layer_items.clear();
    m_material_render_items.clear();
    m_mesh_render_items.clear();
    m_link_sources.clear();
    m_link_source_ids.clear();
    m_linked_layer_ids.clear();

    collect_linked_ids_from_scene(scene, m_linked_layer_ids);
    ensure_snapshot_link_render_targets(scene, m_linked_layer_ids);
    scene.RebuildResourceIndex();

    const auto& index = scene.ResourceIndex();

    for (const auto& definition : scene.ShadowDefinitions()) {
        m_shadow_definitions.push(definition.Clone());
    }

    Vec<String> texture_keys;
    auto        texture_names = scene.TextureNames();
    texture_keys.reserve(texture_names.len());
    for (usize name_index {}; name_index < texture_names.len(); ++name_index) {
        texture_keys.push(texture_names[name_index].clone());
    }
    rstd::slice_::sort_unstable(texture_keys.as_mut_slice().as_mut_ref());

    for (usize key_index {}; key_index < texture_keys.len(); ++key_index) {
        const auto& key = texture_keys[key_index];
        auto        id  = RenderTextureDescId {
            .index      = rstd::as_cast<u32>(m_texture_descs.len()),
            .generation = version.value,
        };
        auto scene_id      = index.textureId(key.as_str()).unwrap_or(SceneTextureId {});
        auto desc          = scene.Texture(key.as_str());
        auto video_control = scene.VideoControl(key.as_str());
        (void)m_texture_desc_ids.insert(key.clone(), id);
        m_texture_descs.push(RenderTextureDescRecord {
            .id               = id,
            .scene_texture    = scene_id,
            .key              = key.clone(),
            .desc             = desc.is_some() ? **desc : SceneTexture {},
            .content_revision = scene.TextureContentRevision(key.as_str()),
            .video_control    = rstd::move(video_control),
        });
    }

    Vec<String> render_target_keys;
    auto        render_target_names = scene.RenderTargetNames();
    render_target_keys.reserve(render_target_names.len());
    for (usize name_index {}; name_index < render_target_names.len(); ++name_index) {
        render_target_keys.push(render_target_names[name_index].clone());
    }
    rstd::slice_::sort_unstable(render_target_keys.as_mut_slice().as_mut_ref());

    for (usize key_index {}; key_index < render_target_keys.len(); ++key_index) {
        const auto& key = render_target_keys[key_index];
        auto        id  = RenderTargetDescId {
            .index      = rstd::as_cast<u32>(m_render_target_descs.len()),
            .generation = version.value,
        };
        auto scene_id = index.renderTargetId(key.as_str()).unwrap_or(SceneRenderTargetId {});
        auto desc     = scene.RenderTarget(key.as_str());
        (void)m_render_target_desc_ids.insert(key.clone(), id);
        m_render_target_descs.push(RenderTargetDescRecord {
            .id                  = id,
            .scene_render_target = scene_id,
            .key                 = key.clone(),
            .desc                = desc.is_some() ? **desc : SceneRenderTarget {},
        });
    }

    auto append_render_item = [](auto& items_by_key, auto key, RenderItemId id) {
        auto items = items_by_key.get_mut(key);
        if (items.is_none()) {
            (void)items_by_key.insert(key, Vec<RenderItemId> {});
            items = items_by_key.get_mut(key);
        }
        (**items).emplace_back(id);
    };

    auto draw_items = index.DrawItems();
    for (usize item_index {}; item_index < draw_items.len(); ++item_index) {
        const auto& item = draw_items[item_index];
        if (! item.id.Valid()) continue;
        auto        id   = RenderItemId { .index      = rstd::as_cast<u32>(m_render_items.len()),
                                          .generation = version.value };
        const auto* node = index.node(item.node);
        Option<RenderTargetDescId> output_override;
        if (auto view = index.resolve(item.id)) {
            if (view->submesh != nullptr && ! view->submesh->output_override.empty()) {
                output_override = renderTargetDescId(
                    rstd::cppstd::as_str(view->submesh->output_override).unwrap());
            }
        }

        auto source_layer =
            node != nullptr
                ? scene.ResolveLayerLinkSource(*node).unwrap_or(
                      node->WallpaperIdentity().unwrap_or(WallpaperLayerId { .value = i32(-1) }))
                : WallpaperLayerId { .value = i32(-1) };
        (void)m_render_item_ids.insert(scene_id_key(item.id), id);
        append_render_item(m_source_layer_items, source_layer.value, id);
        append_render_item(m_material_render_items, scene_id_key(item.material), id);
        append_render_item(m_mesh_render_items, scene_id_key(item.mesh), id);
        m_render_items.push(RenderItemRecord { .id              = id,
                                               .scene_draw_item = item.id,
                                               .scene_node      = item.node,
                                               .scene_mesh      = item.mesh,
                                               .scene_material  = item.material,
                                               .source_layer    = source_layer,
                                               .submesh_index   = item.submesh_index,
                                               .output_override = output_override });
        auto view = index.resolve(item.id);
        if (node == nullptr || ! node->shadow.cast || view.is_none() || view->material == nullptr ||
            ! view->material->shadow_variant || m_shadow_definitions.is_empty()) {
            continue;
        }
        m_shadow_casters.push(RenderShadowCasterRecord {
            .render_item    = id,
            .material       = view->material->shadow_variant,
            .instance_count = rstd::as_cast<u32>(m_shadow_definitions[usize()].viewports.len()),
        });
    }

    auto linked_ids = m_linked_layer_ids.iter();
    for (auto next = linked_ids.next(); next.is_some(); next = linked_ids.next()) {
        auto id      = **next;
        auto key     = GenLinkTex(static_cast<std::ptrdiff_t>(id.to_primitive()));
        auto desc_id = renderTargetDescId(rstd::cppstd::as_str(key).unwrap());
        if (! desc_id) continue;

        auto record_index = rstd::as_cast<u32>(m_link_sources.len());
        (void)m_link_source_ids.insert(id, record_index);
        m_link_sources.push(RenderLinkSourceRecord {
            .source_layer      = WallpaperLayerId { .value = id },
            .scene_node        = scene.RegisteredLayerLinkSourceId(WallpaperLayerId { .value = id })
                                     .unwrap_or(SceneNodeId {}),
            .render_target_key = String::make(rstd::cppstd::as_str(key).unwrap()),
            .render_target     = *desc_id,
        });
    }
}

const RenderItemRecord* RenderSceneSnapshot::renderItem(RenderItemId id) const {
    if (! valid_render_index(id, m_version.value, m_render_items.len().to_primitive()))
        return nullptr;
    return &m_render_items[index_from_id(id.index)];
}

const RenderTextureDescRecord* RenderSceneSnapshot::textureDesc(RenderTextureDescId id) const {
    if (! valid_render_index(id, m_version.value, m_texture_descs.len().to_primitive()))
        return nullptr;
    return &m_texture_descs[index_from_id(id.index)];
}

const RenderTargetDescRecord* RenderSceneSnapshot::renderTargetDesc(RenderTargetDescId id) const {
    if (! valid_render_index(id, m_version.value, m_render_target_descs.len().to_primitive()))
        return nullptr;
    return &m_render_target_descs[index_from_id(id.index)];
}

Option<RenderItemId> RenderSceneSnapshot::renderItemFor(SceneDrawItemId id) const {
    auto render_id = m_render_item_ids.get(scene_id_key(id));
    return render_id.is_some() ? Some<RenderItemId>(**render_id) : None();
}

Option<RenderTextureDescId> RenderSceneSnapshot::textureDescId(ref<str> key) const {
    auto id = m_texture_desc_ids.get(key);
    return id.is_some() ? Some<RenderTextureDescId>(**id) : None();
}

Option<RenderTargetDescId> RenderSceneSnapshot::renderTargetDescId(ref<str> key) const {
    auto id = m_render_target_desc_ids.get(key);
    return id.is_some() ? Some<RenderTargetDescId>(**id) : None();
}

slice<RenderItemId> RenderSceneSnapshot::renderItemsFor(WallpaperLayerId id) const {
    auto items = m_source_layer_items.get(id.value);
    return items.is_some() ? (**items).as_slice() : slice<RenderItemId> {};
}

slice<RenderItemId> RenderSceneSnapshot::renderItemsFor(SceneMaterialId id) const {
    auto items = m_material_render_items.get(scene_id_key(id));
    return items.is_some() ? (**items).as_slice() : slice<RenderItemId> {};
}

slice<RenderItemId> RenderSceneSnapshot::renderItemsFor(SceneMeshId id) const {
    auto items = m_mesh_render_items.get(scene_id_key(id));
    return items.is_some() ? (**items).as_slice() : slice<RenderItemId> {};
}

const RenderLinkSourceRecord* RenderSceneSnapshot::linkSource(WallpaperLayerId id) const {
    auto index = m_link_source_ids.get(id.value);
    if (index.is_none()) return nullptr;
    return &m_link_sources[index_from_id(**index)];
}

bool RenderSceneSnapshot::HasLinkConsumer(WallpaperLayerId id) const {
    return m_linked_layer_ids.contains(id.value);
}

RenderSceneSnapshot ExtractRenderSceneSnapshot(Scene& scene) {
    RenderSceneSnapshot snapshot;
    snapshot.Rebuild(scene, RenderSceneVersion { .value = next_render_scene_version() });
    return snapshot;
}

bool SceneAnimationCurve::Empty() const { return c0.is_empty() && c1.is_empty() && c2.is_empty(); }

float SceneAnimationCurve::EvaluateScalar(float base, double runtime) const {
    if (c0.is_empty()) return base;
    float value = eval_axis(c0.as_slice(), animation_frame(*this, runtime));
    return relative ? base + value : value;
}

Eigen::Vector3f SceneAnimationCurve::EvaluateVec3(const Eigen::Vector3f& base,
                                                  double                 runtime) const {
    if (Empty()) return base;
    AnimationFrame  frame = animation_frame(*this, runtime);
    Eigen::Vector3f value = base;
    if (! c0.is_empty())
        value.x() =
            relative ? base.x() + eval_axis(c0.as_slice(), frame) : eval_axis(c0.as_slice(), frame);
    if (! c1.is_empty())
        value.y() =
            relative ? base.y() + eval_axis(c1.as_slice(), frame) : eval_axis(c1.as_slice(), frame);
    if (! c2.is_empty())
        value.z() =
            relative ? base.z() + eval_axis(c2.as_slice(), frame) : eval_axis(c2.as_slice(), frame);
    return value;
}

SceneAnimationEventCursor::SceneAnimationEventCursor(const SceneAnimationCurve& curve) {
    if (curve.events.is_empty() || curve.startpaused) return;
    m_fps    = curve.fps > 0.0f ? curve.fps : 30.0f;
    m_end    = rstd::as_cast<float>(curve_end(curve));
    m_loop   = curve_loops(curve);
    m_mirror = curve.mode == "mirror"_str;
    if (m_end <= 0.0f) {
        m_loop   = false;
        m_mirror = false;
    }
    for (usize index {}; index < curve.events.len(); ++index) {
        const auto& event = curve.events[index];
        if (m_end > 0.0f && rstd::as_cast<float>(event.frame) > m_end) continue;
        m_events.push(SceneAnimationEvent { .frame = std::max(event.frame, i32()),
                                            .name  = event.name.clone() });
    }
}

namespace
{
// First time at or after `after` at which a playhead of the given period
// reaches `at`. `None` when it never does again.
Option<double> next_pass(double at, double after, double period) {
    if (period <= 0.0) return at > after ? Some(at) : None();
    double turns = std::floor((after - at) / period) + 1.0;
    if (turns < 0.0) turns = 0.0;
    double pass = at + turns * period;
    while (pass <= after) pass += period;
    return Some(pass);
}
} // namespace

void SceneAnimationEventCursor::Advance(double runtime, Vec<ref<str>>& out) {
    if (m_events.is_empty()) return;
    const double now = runtime * static_cast<double>(m_fps);
    if (! m_primed) {
        // Start half a frame short of zero so markers sitting on frame 0
        // fire on the first tick instead of being skipped.
        m_previous = -0.5;
        m_primed   = true;
    }
    // A restarted scene rewinds the clock; pick the playhead back up there.
    if (now < m_previous) m_previous = now - 0.5;

    const double end    = static_cast<double>(m_end);
    const double period = m_mirror ? 2.0 * end : end;

    struct Pass {
        double   at { 0.0 };
        ref<str> name;
    };
    Vec<Pass> passed;
    for (usize index {}; index < m_events.len(); ++index) {
        const auto&    event = m_events[index];
        const double   frame = static_cast<double>(event.frame.to_primitive());
        Option<double> pass  = None();
        if (! m_loop && ! m_mirror) {
            pass = next_pass(frame, m_previous, 0.0);
        } else {
            pass = next_pass(frame, m_previous, period);
            if (m_mirror && frame > 0.0 && frame < end) {
                // The playhead passes an interior marker twice per period:
                // once going up, once coming back down.
                auto back = next_pass(period - frame, m_previous, period);
                if (back.is_some() && (pass.is_none() || *back < *pass)) pass = back;
            }
        }
        if (pass.is_none() || *pass > now) continue;
        passed.push(Pass { .at = *pass, .name = event.name.as_str() });
    }
    m_previous = now;
    if (passed.is_empty()) return;
    rstd::slice_::sort_unstable_by(passed.as_mut_slice().as_mut_ref(),
                                   [](const Pass& left, const Pass& right) {
                                       return left.at < right.at;
                                   });
    for (usize index {}; index < passed.len(); ++index) {
        ref<str> name = passed[index].name;
        out.push(rstd::move(name));
    }
}

void SceneNode::TickFieldAnimations(double runtime) {
    if (m_origin_curve) SetTranslate(m_origin_curve->EvaluateVec3(m_origin_base, runtime));
    if (m_scale_curve) SetScale(m_scale_curve->EvaluateVec3(m_scale_base, runtime));
    if (m_rotation_curve) SetRotation(m_rotation_curve->EvaluateVec3(m_rotation_base, runtime));
    if (m_alpha_curve) SetUserAlpha(m_alpha_curve->EvaluateScalar(m_base_alpha, runtime));
}

void SceneCameraPath::CaptureViewport() {
    if (camera.is_none()) return;
    default_width  = (**camera).Width();
    default_height = (**camera).Height();
    default_fov    = (**camera).Fov();
}

bool SceneCameraPath::ApplyDefault() {
    if (camera.is_none()) return false;
    auto& value = **camera;
    if (default_lookat) {
        value.SetLookAt(
            default_eye.cast<double>(), default_center.cast<double>(), default_up.cast<double>());
    }
    if (node) {
        node->SetTranslate(default_translate);
        node->SetRotation(default_rotation);
    }
    value.SetWidth(default_width);
    value.SetHeight(default_height);
    if (default_fov > 0.0) value.SetFov(default_fov);
    value.Update();
    return true;
}

bool SceneCameraPath::Tick(double runtime) {
    if (camera.is_none()) return false;
    auto& value = **camera;
    if (! enabled) return ApplyDefault();

    if (! lookat_tracks.is_empty()) {
        auto key = eval_lookat_tracks(lookat_tracks.as_slice(), runtime, lookat_fps);
        if (! key) return false;
        value.SetLookAt(
            key->eye.cast<double>(), key->center.cast<double>(), key->up.cast<double>());
        if (fov_base > 0.0f) value.SetFov(fov_base);
        value.Update();
        return true;
    }

    if (! node) return false;
    value.AttatchNode(node);

    node->SetTranslate(path_translate_bias + origin_curve.EvaluateVec3(origin_base, runtime));
    node->SetRotation(path_rotation_bias + rotation_curve.EvaluateVec3(rotation_base, runtime));

    if (perspective) {
        float fov = fov_curve.EvaluateScalar(fov_base, runtime);
        if (fov > 0.0f) value.SetFov(fov);
    } else {
        float zoom = zoom_curve.EvaluateScalar(zoom_base, runtime);
        zoom       = std::max(zoom, 0.001f);
        value.SetWidth(default_width / static_cast<double>(zoom));
        value.SetHeight(default_height / static_cast<double>(zoom));
    }
    value.Update();
    return true;
}

Scene::Scene()
    : m_scene_graph(Box<SceneNode>::make()),
      m_resource_generation(next_scene_resource_generation()) {
    RegisterNode(*m_scene_graph);
    m_runtime.RegisterSystem(SceneTextureAnimationRuntime { m_texture_animations });
}
Scene::~Scene() = default;

SceneNodeId Scene::RegisterNode(SceneNode& node, Option<WallpaperLayerId> wallpaper) {
    if (! node.m_identity.Valid() || node.m_identity.generation != m_resource_generation) {
        node.m_identity = SceneNodeId {
            .index      = m_next_node_index,
            .generation = m_resource_generation,
        };
        ++m_next_node_index;
    }
    if (wallpaper.is_some()) {
        if (node.m_wallpaper_identity.is_some() &&
            node.m_wallpaper_identity->value != wallpaper->value) {
            rstd_error("scene node {} already belongs to wallpaper layer {}",
                       node.m_identity.index,
                       node.m_wallpaper_identity->value);
            return node.m_identity;
        }
        auto existing = m_wallpaper_node_ids.get(wallpaper->value);
        if (existing.is_some() && **existing != node.m_identity) {
            rstd_error("wallpaper layer {} already belongs to scene node {}",
                       wallpaper->value,
                       (**existing).index);
            return node.m_identity;
        }
        node.m_wallpaper_identity = wallpaper;
        node.m_id                 = wallpaper->value;
        (void)m_wallpaper_node_ids.insert(wallpaper->value, node.m_identity);
    }
    const auto key = scene_id_key(node.m_identity);
    if (node.m_wallpaper_identity.is_none() && ! node.Visible())
        (void)m_hidden_scene_node_ids.insert(key);
    else
        (void)m_hidden_scene_node_ids.remove(key);
    return node.m_identity;
}

SceneEffectId Scene::RegisterEffect(SceneNodeId owner, SceneNodeLayer& layer,
                                    std::shared_ptr<SceneImageEffect> effect) {
    if (! effect) return {};
    if (! owner.Valid() || owner.generation != m_resource_generation) return {};
    if (! effect->id.Valid() || effect->id.generation != m_resource_generation) {
        effect->id = SceneEffectId {
            .index      = m_next_effect_index,
            .generation = m_resource_generation,
        };
        ++m_next_effect_index;
    }
    effect->owner = owner;
    (void)m_image_effects.insert(scene_id_key(effect->id.index, effect->id.generation),
                                 ImageEffectRecord {
                                     .owner  = owner,
                                     .layer  = rstd::addressof(layer),
                                     .effect = effect,
                                 });
    return effect->id;
}

String Scene::NodeResourceKey(SceneNodeId node, ref<str> role) const {
    if (! node.Valid() || node.generation != m_resource_generation) return String {};
    return rstd::format("_rt_node_{}_{}", node.index, role);
}

String Scene::EffectResourceKey(SceneEffectId effect, ref<str> local_name) const {
    if (! effect.Valid() || effect.generation != m_resource_generation) return String {};
    return rstd::format("_rt_effect_{}_{}", effect.index, local_name);
}

void Scene::RegisterTexture(String name, SceneTexture texture) {
    auto revision = m_texture_content_revisions.get_mut(name.as_str());
    if (revision.is_some()) {
        ++**revision;
    } else {
        (void)m_texture_content_revisions.insert(name.clone(), u64(1));
    }
    if (texture.isVideo) {
        auto control_key = texture.url.empty()
                               ? name.clone()
                               : String::make(rstd::cppstd::as_str(texture.url).unwrap());
        if (! m_video_controls.contains_key(control_key.as_str())) {
            (void)m_video_controls.insert(rstd::move(control_key), Arc<VideoPlaybackState>::make());
        }
    }
    if (! m_textures.contains_key(name.as_str())) m_texture_names.push(name.clone());
    (void)m_textures.insert(rstd::move(name), rstd::move(texture));
}

auto Scene::Texture(ref<str> name) const -> Option<ref<SceneTexture>> {
    return m_textures.get(name);
}

auto Scene::TextureContentRevision(ref<str> name) const -> u64 {
    auto revision = m_texture_content_revisions.get(name);
    return revision.is_some() ? **revision : u64();
}

auto Scene::VideoControl(ref<str> name) const -> Option<Arc<VideoPlaybackState>> {
    auto control = m_video_controls.get(name);
    if (control.is_some()) return Some((*control)->clone());
    auto texture = m_textures.get(name);
    if (texture.is_none() || ! (**texture).isVideo || (**texture).url.empty()) return None();
    auto key = rstd::cppstd::as_str((**texture).url).unwrap();
    control  = m_video_controls.get(key);
    return control.is_some() ? Some((*control)->clone()) : None<Arc<VideoPlaybackState>>();
}

void Scene::RegisterRenderTarget(String name, SceneRenderTarget target) {
    if (! m_render_targets.contains_key(name.as_str())) m_render_target_names.push(name.clone());
    (void)m_render_targets.insert(rstd::move(name), rstd::move(target));
}

auto Scene::RenderTarget(ref<str> name) const -> Option<ref<SceneRenderTarget>> {
    return m_render_targets.get(name);
}

auto Scene::RenderTargetMut(ref<str> name) -> Option<mut_ref<SceneRenderTarget>> {
    return m_render_targets.get_mut(name);
}

void Scene::RegisterUserTextBinding(String key, Box<dyn<FnMut<void(ref<str>)>>> setter) {
    auto setters = m_text_user_index.get_mut(key.as_str());
    if (setters.is_none()) {
        (void)m_text_user_index.insert(key.clone(), Vec<Box<dyn<FnMut<void(ref<str>)>>>> {});
        setters = m_text_user_index.get_mut(key.as_str());
    }
    (*setters)->push(rstd::move(setter));
}

bool Scene::ApplyUserTextBindings(ref<str> key, const Json& property) {
    auto setters = m_text_user_index.get_mut(key);
    if (setters.is_none()) return false;

    auto value = SceneJsonScalarString(SceneUserPropertyPayload(property));
    if (value.is_none()) return false;
    for (usize index {}; index < (*setters)->len(); ++index) {
        (**setters)[index]->operator()(value->as_str());
    }
    return true;
}

void Scene::RegisterUserPropertyBinding(String key, Box<dyn<FnMut<void(ref<Json>)>>> setter) {
    auto setters = m_user_property_index.get_mut(key.as_str());
    if (setters.is_none()) {
        (void)m_user_property_index.insert(key.clone(), Vec<Box<dyn<FnMut<void(ref<Json>)>>>> {});
        setters = m_user_property_index.get_mut(key.as_str());
    }
    (*setters)->push(rstd::move(setter));
}

bool Scene::ApplyUserPropertyBindings(ref<str> key, const Json& property) {
    auto setters = m_user_property_index.get_mut(key);
    if (setters.is_none()) return false;
    auto property_ref = ref<Json>::from_raw_parts(rstd::addressof(property));
    for (usize index {}; index < (*setters)->len(); ++index) {
        (**setters)[index]->operator()(property_ref);
    }
    return true;
}

void Scene::RegisterTransformUpdater(Box<dyn<FnMut<void(f64)>>> updater) {
    m_transform_updaters.push(rstd::move(updater));
}

void Scene::RegisterShaderUserBinding(String key, std::shared_ptr<SceneMaterial> material,
                                      String uniform) {
    auto bindings = m_shader_user_index.get_mut(key.as_str());
    if (bindings.is_none()) {
        (void)m_shader_user_index.insert(key.clone(), Vec<ShaderUserBinding> {});
        bindings = m_shader_user_index.get_mut(key.as_str());
    }
    (*bindings)->push(ShaderUserBinding {
        .material = rstd::move(material),
        .uniform  = rstd::move(uniform),
    });
}

auto Scene::ShaderUserBindings(ref<str> key) const -> slice<ShaderUserBinding> {
    auto bindings = m_shader_user_index.get(key);
    return bindings.is_some() ? (*bindings)->as_slice() : slice<ShaderUserBinding> {};
}

void Scene::RegisterShaderComboUserBinding(String key, ShaderComboUserBinding binding) {
    auto bindings = m_shader_combo_user_index.get_mut(key.as_str());
    if (bindings.is_none()) {
        (void)m_shader_combo_user_index.insert(key.clone(), Vec<ShaderComboUserBinding> {});
        bindings = m_shader_combo_user_index.get_mut(key.as_str());
    }
    (*bindings)->push(rstd::move(binding));
}

auto Scene::ShaderComboUserBindings(ref<str> key) const -> slice<ShaderComboUserBinding> {
    auto bindings = m_shader_combo_user_index.get(key);
    return bindings.is_some() ? (*bindings)->as_slice() : slice<ShaderComboUserBinding> {};
}

void Scene::RegisterMaterialTextureUserBinding(String key, MaterialTextureUserBinding binding) {
    auto bindings = m_material_texture_user_index.get_mut(key.as_str());
    if (bindings.is_none()) {
        (void)m_material_texture_user_index.insert(key.clone(), Vec<MaterialTextureUserBinding> {});
        bindings = m_material_texture_user_index.get_mut(key.as_str());
    }
    (*bindings)->push(rstd::move(binding));
}

auto Scene::MaterialTextureUserBindings(ref<str> key) const -> slice<MaterialTextureUserBinding> {
    auto bindings = m_material_texture_user_index.get(key);
    return bindings.is_some() ? (*bindings)->as_slice() : slice<MaterialTextureUserBinding> {};
}

namespace
{

auto MakeImagePropertyBinding(const Arc<SceneNode>&                 node,
                              slice<std::shared_ptr<SceneMaterial>> materials)
    -> Scene::ImagePropertyBinding {
    Scene::ImagePropertyBinding binding { .node = node.clone() };
    binding.materials.reserve(materials.len());
    for (usize index {}; index < materials.len(); ++index) {
        binding.materials.emplace_back(materials[index]);
    }
    return binding;
}

} // namespace

void Scene::RegisterImageColorUserBinding(String key, const Arc<SceneNode>& node,
                                          slice<std::shared_ptr<SceneMaterial>> materials) {
    auto bindings = m_image_color_user_index.get_mut(key.as_str());
    if (bindings.is_none()) {
        (void)m_image_color_user_index.insert(key.clone(), Vec<ImagePropertyBinding> {});
        bindings = m_image_color_user_index.get_mut(key.as_str());
    }
    (*bindings)->push(MakeImagePropertyBinding(node, materials));
}

void Scene::RegisterImageAlphaUserBinding(String key, const Arc<SceneNode>& node,
                                          slice<std::shared_ptr<SceneMaterial>> materials) {
    auto bindings = m_image_alpha_user_index.get_mut(key.as_str());
    if (bindings.is_none()) {
        (void)m_image_alpha_user_index.insert(key.clone(), Vec<ImagePropertyBinding> {});
        bindings = m_image_alpha_user_index.get_mut(key.as_str());
    }
    (*bindings)->push(MakeImagePropertyBinding(node, materials));
}

auto Scene::ImageColorUserBindings(ref<str> key) const -> slice<ImagePropertyBinding> {
    auto bindings = m_image_color_user_index.get(key);
    return bindings.is_some() ? (*bindings)->as_slice() : slice<ImagePropertyBinding> {};
}

auto Scene::ImageAlphaUserBindings(ref<str> key) const -> slice<ImagePropertyBinding> {
    auto bindings = m_image_alpha_user_index.get(key);
    return bindings.is_some() ? (*bindings)->as_slice() : slice<ImagePropertyBinding> {};
}

void Scene::RegisterCamera(String name, Arc<SceneCamera> camera) {
    if (! m_cameras.contains_key(name.as_str())) m_camera_names.push(name.clone());
    (void)m_cameras.insert(rstd::move(name), rstd::move(camera));
}

auto Scene::Camera(ref<str> name) const -> Option<ref<SceneCamera>> {
    auto camera = m_cameras.get(name);
    if (camera.is_none()) return None();
    return Some((**camera).deref());
}

auto Scene::CameraMut(ref<str> name) -> Option<mut_ref<SceneCamera>> {
    auto camera = m_cameras.get_mut(name);
    if (camera.is_none()) return None();
    return Some((**camera).deref_mut());
}

auto Scene::CameraHandle(ref<str> name) const -> Option<Arc<SceneCamera>> {
    auto camera = m_cameras.get(name);
    if (camera.is_none()) return None();
    return Some((**camera).clone());
}

bool Scene::SetActiveCamera(ref<str> name) {
    if (! m_cameras.contains_key(name)) return false;
    m_active_camera = Some(String::make(name));
    return true;
}

auto Scene::ActiveCamera() const -> Option<ref<SceneCamera>> {
    if (m_active_camera.is_none()) return None();
    return Camera((*m_active_camera).as_str());
}

auto Scene::ActiveCameraHandle() const -> Option<Arc<SceneCamera>> {
    if (m_active_camera.is_none()) return None();
    return CameraHandle((*m_active_camera).as_str());
}

auto Scene::ActiveCameraTransforms() const -> Option<SceneCameraTransforms> {
    auto camera = ActiveCamera();
    if (camera.is_none()) return None();
    return Some((**camera).Transforms());
}

bool Scene::SetActiveCameraTransforms(const SceneCameraTransforms& transforms) {
    if (m_active_camera.is_none()) return false;
    auto name = (*m_active_camera).clone();
    {
        auto camera = CameraMut(name.as_str());
        if (camera.is_none() || ! (**camera).SetTransforms(transforms)) return false;
    }
    UpdateLinkedCamera(name.as_str());
    return true;
}

bool SceneMaterial::SetShaderValueAnimation(String uniform_name, Arc<SceneAnimationCurve> curve) {
    if (uniform_name.is_empty() || curve->Empty()) return false;
    auto uniform_key = rstd::cppstd::to_string(uniform_name.as_str());

    ShaderValue base;
    if (auto it = customShader.constValues.find(uniform_key);
        it != customShader.constValues.end()) {
        base = it->second;
    } else if (customShader.shader) {
        if (auto it = customShader.shader->default_uniforms.find(uniform_key);
            it != customShader.shader->default_uniforms.end()) {
            base = ShapeShaderValue(uniform_key, it->second);
        }
    }
    if (base.size() == usize()) return false;

    (void)customShader.valueAnimations.insert(
        rstd::move(uniform_name),
        SceneShaderValueAnimation { .base = base, .curve = Some(rstd::move(curve)) });
    return true;
}

auto SceneMaterialCustomShader::Clone() const -> SceneMaterialCustomShader {
    SceneMaterialCustomShader cloned {
        .shader        = shader,
        .constValues   = constValues,
        .variant       = variant.is_some() ? Some<SceneShaderVariantDesc>(*variant) : None(),
        .value_version = value_version,
    };
    valueAnimations.iter().for_each([&](auto entry) {
        auto [name, animation] = entry;
        (void)cloned.valueAnimations.insert(name->clone(), animation->Clone());
    });
    return cloned;
}

bool SceneMaterial::TickShaderValueAnimations(double runtime) {
    bool changed = false;
    customShader.valueAnimations.iter_mut().for_each([&](auto entry) {
        auto [name, animation]   = entry;
        auto        uniform_name = rstd::cppstd::to_string(name->as_str());
        ShaderValue value        = eval_shader_value_animation(*animation, runtime);
        if (auto it = customShader.constValues.find(uniform_name);
            it != customShader.constValues.end() && shader_values_equal(it->second, value)) {
            return;
        }
        customShader.constValues[uniform_name] = std::move(value);
        changed                                = true;
    });
    if (changed) TouchShaderValues();
    return changed;
}

void SceneTextureAnimationRegistry::Rebuild(const Scene& scene) {
    auto previous = rstd::move(m_animations);
    m_animations.clear();
    m_entries.clear();
    const auto& resources  = scene.ResourceIndex();
    auto        draw_items = resources.DrawItems();
    for (usize item_index {}; item_index < draw_items.len(); ++item_index) {
        const auto& record = draw_items[item_index];
        auto        draw   = resources.resolve(record.id);
        if (draw.is_none() || draw->node == nullptr || draw->material == nullptr) continue;

        Entry entry { .node = draw->node };
        for (std::size_t index = 0; index < draw->material->textures.size(); ++index) {
            const auto& texture_key = draw->material->textures[index];
            auto        texture     = scene.Texture(rstd::cppstd::as_str(texture_key).unwrap());
            if (texture.is_none() || ! (**texture).isSprite ||
                (**texture).spriteAnim.numFrames() == usize()) {
                continue;
            }

            auto texture_name = String::make(rstd::cppstd::as_str(texture_key).unwrap());
            auto animation    = m_animations.get_mut(texture_name.as_str());
            if (animation.is_none()) {
                auto previous_animation = previous.remove(texture_name.as_str());
                auto value              = previous_animation.is_some()
                                              ? rstd::move(*previous_animation)
                                              : Animation { .sprite = (**texture).spriteAnim };
                (void)m_animations.insert(texture_name.clone(), rstd::move(value));
                animation = m_animations.get_mut(texture_name.as_str());
            }
            if (animation.is_none()) continue;
            (void)entry.bindings.insert(usize(index),
                                        Binding {
                                            .texture    = rstd::move(texture_name),
                                            .held_frame = (**animation).sprite.CurrentFrameIndex(),
                                        });
        }
        if (! entry.bindings.is_empty()) (void)m_entries.insert(Key(record.id), rstd::move(entry));
    }
}

void SceneTextureAnimationRegistry::Advance(f64 delta) {
    HashSet<String> active;
    m_entries.iter_mut().for_each([&](auto entry) {
        auto [_, state] = entry;
        if (state->node == nullptr) return;
        const auto& override = state->node->TexAnim();
        const bool  playing  = override.playing && override.current_frame < 0;
        state->bindings.iter_mut().for_each([&](auto binding_entry) {
            auto [_, binding] = binding_entry;
            auto animation    = m_animations.get_mut(binding->texture.as_str());
            if (animation.is_none()) return;
            if (! playing && binding->was_playing) {
                binding->held_frame = (**animation).sprite.CurrentFrameIndex();
            }
            binding->was_playing = playing;
            if (playing) (void)active.insert(binding->texture.clone());
        });
    });

    active.iter().for_each([&](ref<String> texture) {
        auto animation = m_animations.get_mut(texture->as_str());
        if (animation.is_none()) return;
        const auto before = (**animation).sprite.CurrentFrameIndex();
        (void)(**animation).sprite.GetAnimateFrame(delta.to_primitive());
        if (before != (**animation).sprite.CurrentFrameIndex()) {
            ++(**animation).revision;
            if ((**animation).revision == u64()) (**animation).revision = u64(1);
        }
    });

    m_entries.iter_mut().for_each([&](auto entry) {
        auto [_, state] = entry;
        if (state->node == nullptr) return;
        const auto& override = state->node->TexAnim();
        const bool  playing  = override.playing && override.current_frame < 0;
        if (! playing) return;
        state->bindings.iter_mut().for_each([&](auto binding_entry) {
            auto [_, binding] = binding_entry;
            auto animation    = m_animations.get(binding->texture.as_str());
            if (animation.is_none()) return;
            binding->held_frame = (**animation).sprite.CurrentFrameIndex();
        });
    });
}

auto SceneTextureAnimationRegistry::Frame(SceneDrawItemId draw, usize texture_index) const
    -> Option<SceneTextureFrameView> {
    auto entry = m_entries.get(Key(draw));
    if (entry.is_none() || (**entry).node == nullptr) return None();
    auto binding = (**entry).bindings.get(texture_index);
    if (binding.is_none()) return None();
    auto animation = m_animations.get((**binding).texture.as_str());
    if (animation.is_none() || (**animation).sprite.numFrames() == usize()) return None();

    const auto&        override = (**entry).node->TexAnim();
    const SpriteFrame* frame;
    if (override.current_frame >= 0) {
        const auto selected = usize(static_cast<std::size_t>(override.current_frame)) %
                              (**animation).sprite.numFrames();
        frame               = rstd::addressof((**animation).sprite.GetFrame(selected));
    } else if (! override.playing) {
        const auto selected = (**binding).held_frame % (**animation).sprite.numFrames();
        frame               = rstd::addressof((**animation).sprite.GetFrame(selected));
    } else {
        frame = rstd::addressof((**animation).sprite.GetCurFrame());
    }
    return Some(SceneTextureFrameView {
        .rotation    = { frame->xAxis[0], frame->xAxis[1], frame->yAxis[0], frame->yAxis[1] },
        .translation = { frame->x, frame->y },
        .image_slot  = usize(static_cast<std::size_t>(frame->imageId)),
        .revision    = (**animation).revision,
    });
}

void Scene::RebuildResourceIndex() {
    m_resource_index.Rebuild(*this, m_resource_generation);
    m_texture_animations.Rebuild(*this);
}

void Scene::AttachRuntimeNode(SceneNode& parent, Arc<SceneNode> node) {
    (void)RegisterNode(*node);
    parent.AppendChild(rstd::move(node));
    RebuildResourceIndex();
    m_render_graph_dirty = true;
}

auto Scene::LayerIndex(const SceneNode& node) const -> Option<usize> {
    auto* parent = node.Parent();
    return parent ? parent->ChildIndex(node) : None<usize>();
}

bool Scene::SortLayer(SceneNode& node, usize index) {
    auto* parent = node.Parent();
    if (parent == nullptr || ! parent->MoveChild(node, index)) return false;
    m_render_graph_dirty = true;
    return true;
}

bool Scene::EnsureTextureDescriptor(std::string_view key) {
    auto name = rstd::cppstd::as_str(key).unwrap();
    if (key.empty() || IsSpecTex(name)) return true;
    if (m_textures.contains_key(name)) return true;
    auto header = ParseImageHeader(rstd::cppstd::as_str(key).unwrap());
    if (header.is_err()) return false;

    SceneTexture texture;
    texture.url     = std::string(key);
    texture.sample  = header->sample;
    texture.isVideo = header->type == ImageType::VIDEO;
    if (header->isSprite) {
        texture.isSprite   = true;
        texture.spriteAnim = header->spriteAnim;
    }
    RegisterTexture(String::make(name), rstd::move(texture));
    return true;
}

auto Scene::ParseImage(ref<str> name) const -> Result<Arc<Image>, ImageParseError> {
    auto runtime = m_runtime_images.get(name);
    if (runtime.is_some()) return Ok((*runtime)->clone());
    if (m_image_parser.is_none()) {
        return Err(ImageParseError {
            .kind    = ImageParseErrorKind::MissingContent,
            .message = rstd::format("image parser unavailable for {}", name),
        });
    }
    return (*m_image_parser)->Parse(name);
}

auto Scene::ParseImages(slice<String> names) const -> Vec<Result<Arc<Image>, ImageParseError>> {
    auto delegated_names = Vec<String>::with_capacity(names.len());
    for (usize index {}; index < names.len(); ++index) {
        if (m_runtime_images.get(names[index].as_str()).is_none()) {
            delegated_names.push(names[index].clone());
        }
    }

    auto delegated = Vec<Result<Arc<Image>, ImageParseError>>::make();
    if (! delegated_names.is_empty() && m_image_parser.is_some()) {
        delegated = (*m_image_parser)->ParseMany(delegated_names.as_slice());
    }

    auto  results = Vec<Result<Arc<Image>, ImageParseError>>::with_capacity(names.len());
    usize delegated_index {};
    for (usize index {}; index < names.len(); ++index) {
        auto runtime = m_runtime_images.get(names[index].as_str());
        if (runtime.is_some()) {
            results.push(Ok((*runtime)->clone()));
            continue;
        }
        if (delegated_index < delegated.len()) {
            results.push(rstd::move(delegated[delegated_index]));
        } else {
            results.push(Err(ImageParseError {
                .kind    = ImageParseErrorKind::MissingContent,
                .message = rstd::format("image parser unavailable for {}", names[index].as_str()),
            }));
        }
        ++delegated_index;
    }
    return results;
}

auto Scene::ParseImageHeader(ref<str> name) const -> Result<ImageHeader, ImageParseError> {
    auto runtime = m_runtime_images.get(name);
    if (runtime.is_some()) return Ok((**runtime)->header);
    if (m_image_parser.is_none()) {
        return Err(ImageParseError {
            .kind    = ImageParseErrorKind::MissingContent,
            .message = rstd::format("image parser unavailable for {}", name),
        });
    }
    return (*m_image_parser)->ParseHeader(name);
}

void Scene::RegisterRuntimeImage(String name, Arc<Image> image) {
    auto revision = m_texture_content_revisions.get_mut(name.as_str());
    if (revision.is_some()) {
        ++**revision;
    } else {
        (void)m_texture_content_revisions.insert(name.clone(), u64(1));
    }
    (void)m_runtime_images.insert(rstd::move(name), rstd::move(image));
}

bool Scene::SetMaterialShaderValue(SceneMaterial& material, ref<str> uniform_name,
                                   const ShaderValue& value) {
    return material.SetShaderValue(rstd::cppstd::to_string(uniform_name), value);
}

bool Scene::SetMaterialShaderValueByKey(SceneMaterial& material, ref<str> material_key,
                                        const ShaderValue& value) {
    auto uniform_name = rstd::cppstd::to_string(material_key);
    if (material.customShader.variant.is_some()) {
        const auto& aliases = material.customShader.variant->uniform_aliases;
        if (auto alias = aliases.find(uniform_name); alias != aliases.end()) {
            uniform_name = alias->second;
        }
    }
    return material.SetShaderValue(std::move(uniform_name), value);
}

void Scene::ResolveMaterialTextureSources(SceneMaterial& material) {
    material.texture_sources.resize(usize(material.textures.size()), SceneMaterialTextureSource {});
    for (std::size_t index = 0; index < material.textures.size(); ++index) {
        const auto& texture = material.textures[index];
        auto&       source  = material.texture_sources[usize(index)];
        if (source.kind == SceneMaterialTextureSourceKind::LayerPrevious &&
            source.binding_key == rstd::cppstd::as_str(texture).unwrap()) {
            continue;
        }
        source             = {};
        source.key         = String::make(rstd::cppstd::as_str(texture).unwrap());
        source.binding_key = source.key.clone();
        if (texture.empty()) continue;

        auto name = rstd::cppstd::as_str(texture).unwrap();
        if (auto linked = ParseImageLayerCompositeId(name); linked.is_some()) {
            source.kind            = SceneMaterialTextureSourceKind::LayerOutput;
            source.wallpaper_layer = rstd::as_cast<i32>(*linked);
            auto key               = GenLinkTex(linked->to_primitive());
            source.key             = String::make(rstd::cppstd::as_str(key).unwrap());
            source.layer =
                RegisteredLayerLinkSourceId(WallpaperLayerId { .value = source.wallpaper_layer });
            continue;
        }
        if (IsSpecLinkTex(name)) {
            source.kind            = SceneMaterialTextureSourceKind::LayerOutput;
            source.wallpaper_layer = rstd::as_cast<i32>(ParseLinkTex(name));
            source.layer =
                RegisteredLayerLinkSourceId(WallpaperLayerId { .value = source.wallpaper_layer });
            continue;
        }
        if (name.starts_with(WE_MIP_MAPPED_FRAME_BUFFER)) {
            source.kind = SceneMaterialTextureSourceKind::MipMappedFramebuffer;
            continue;
        }
        if (auto rest = name.strip_prefix("_rt_effect_"_str); rest.is_some()) {
            u32   effect_index {};
            usize digit_count {};
            for (usize offset {}; offset < rest->size(); ++offset) {
                auto digit = (*rest)[offset].to_primitive();
                if (digit < '0' || digit > '9') break;
                effect_index = effect_index * u32(10) + u32(digit - '0');
                ++digit_count;
            }
            SceneEffectId effect_id {
                .index      = effect_index,
                .generation = m_resource_generation,
            };
            if (digit_count > usize() && digit_count < rest->size() &&
                (*rest)[digit_count].to_primitive() == '_' &&
                m_image_effects.contains_key(scene_id_key(effect_id.index, effect_id.generation))) {
                source.kind   = SceneMaterialTextureSourceKind::EffectLocal;
                source.effect = Some(effect_id);
                continue;
            }
        }
        if (auto rest = name.strip_prefix("_rt_node_"_str); rest.is_some()) {
            u32   node_index {};
            usize digit_count {};
            for (usize offset {}; offset < rest->size(); ++offset) {
                auto digit = (*rest)[offset].to_primitive();
                if (digit < '0' || digit > '9') break;
                node_index = node_index * u32(10) + u32(digit - '0');
                ++digit_count;
            }
            if (digit_count > usize() && digit_count < rest->size() &&
                (*rest)[digit_count].to_primitive() == '_' && node_index < m_next_node_index &&
                RenderTarget(name).is_some()) {
                source.kind  = SceneMaterialTextureSourceKind::LayerStage;
                source.layer = Some(SceneNodeId {
                    .index      = node_index,
                    .generation = m_resource_generation,
                });
                continue;
            }
        }
        if (! IsSpecTex(name)) {
            source.kind = SceneMaterialTextureSourceKind::Imported;
        } else if (RenderTarget(name).is_some()) {
            source.kind = SceneMaterialTextureSourceKind::SceneSurface;
        } else {
            source.kind = SceneMaterialTextureSourceKind::UnsupportedSpecial;
        }
    }
}

bool Scene::SetMaterialLayerPreviousSource(SceneMaterial& material, u32 slot, SceneNodeId layer,
                                           ref<str> composite_target) {
    auto index = usize(slot.to_primitive());
    if (index >= usize(material.textures.size())) return false;
    material.texture_sources.resize(usize(material.textures.size()), SceneMaterialTextureSource {});
    auto& source = material.texture_sources[index];
    source.kind  = SceneMaterialTextureSourceKind::LayerPrevious;
    source.key   = String::make(composite_target);
    source.binding_key =
        String::make(rstd::cppstd::as_str(material.textures[index.to_primitive()]).unwrap());
    source.layer = Some(layer);
    return true;
}

SceneMaterialTextureSlotMutation Scene::SetMaterialTextureSlot(SceneMaterial& material, u32 slot,
                                                               std::string_view texture) {
    if (! EnsureTextureDescriptor(texture)) return {};

    auto slot_index = static_cast<std::size_t>(slot.to_primitive());
    if (material.textures.size() <= slot_index) material.textures.resize(slot_index + 1);
    if (material.texture_metadata.size() <= slot_index) {
        material.texture_metadata.resize(slot_index + 1);
    }
    auto& current = material.textures[slot_index];
    if (current == texture) return {};

    current                               = std::string(texture);
    material.texture_metadata[slot_index] = {};
    ResolveMaterialTextureSources(material);
    material.SetTextureBindingsDirty();
    if (m_resource_index.Empty()) RebuildResourceIndex();
    m_texture_animations.Rebuild(*this);
    return SceneMaterialTextureSlotMutation {
        .changed  = true,
        .material = m_resource_index.materialId(material),
    };
}

SceneMaterialShaderVariantMutation
Scene::SetMaterialShaderVariant(SceneMaterial& material, SceneShaderVariantMutation mutation) {
    if (! material.SetShaderVariant(std::move(mutation.shader), std::move(mutation.variant)))
        return {};

    ResolveMaterialTextureSources(material);

    if (m_resource_index.Empty()) RebuildResourceIndex();
    m_texture_animations.Rebuild(*this);
    return SceneMaterialShaderVariantMutation {
        .changed  = true,
        .material = m_resource_index.materialId(material),
    };
}

void Scene::ClearUserPropertyDiagnostics(ref<str> key) {
    if (key.size() == usize()) {
        m_user_property_diagnostics.clear();
        return;
    }
    m_user_property_diagnostics.retain([&](const SceneUserPropertyDiagnostic& diagnostic) {
        return diagnostic.key != key;
    });
}

void Scene::AddUserPropertyDiagnostic(SceneUserPropertyDiagnostic diagnostic) {
    m_user_property_diagnostics.push(rstd::move(diagnostic));
}

void Scene::RebuildElidableLayerIds() {
    m_elidable_layer_ids.clear();
    m_static_elidable_layer_ids.iter().for_each([&](ref<i32> id) {
        (void)m_elidable_layer_ids.insert(*id);
    });
    m_visibility_elidable_layer_ids.iter().for_each([&](ref<i32> id) {
        (void)m_elidable_layer_ids.insert(*id);
    });
}

void Scene::MarkLayerStaticElidable(WallpaperLayerId id) {
    (void)m_static_elidable_layer_ids.insert(id.value);
    (void)m_elidable_layer_ids.insert(id.value);
}

void Scene::MarkLayerVisibilityElidable(WallpaperLayerId id) {
    (void)m_visibility_elidable_layer_ids.insert(id.value);
    (void)m_elidable_layer_ids.insert(id.value);
}

bool Scene::ConsumeRenderGraphDirty() {
    const bool elision_changed =
        ! same_ids(m_elidable_layer_ids, m_render_graph_elidable_layer_ids);
    const bool scene_node_visibility_changed =
        ! same_ids(m_hidden_scene_node_ids, m_render_graph_hidden_scene_node_ids);
    const bool dirty     = m_render_graph_dirty || elision_changed || scene_node_visibility_changed;
    m_render_graph_dirty = false;
    if (elision_changed) m_render_graph_elidable_layer_ids = m_elidable_layer_ids.clone();
    if (scene_node_visibility_changed)
        m_render_graph_hidden_scene_node_ids = m_hidden_scene_node_ids.clone();
    return dirty;
}

void Scene::RegisterLayerLinkSource(WallpaperLayerId id, SceneNode& node) {
    auto node_id  = RegisterNode(node);
    auto previous = m_layer_link_source_ids.get(id.value);
    if (previous.is_some()) (void)m_node_link_sources.remove(scene_id_key(**previous));
    (void)m_layer_link_source_ids.insert(id.value, node_id);
    (void)m_layer_link_source_nodes.insert(id.value, rstd::addressof(node));
    (void)m_layer_link_source_extents.remove(id.value);
    (void)m_node_link_sources.insert(scene_id_key(node_id), id);
}

void Scene::RegisterLayerLinkSource(WallpaperLayerId id, SceneNode& node, array<i32, 2> extent) {
    RegisterLayerLinkSource(id, node);
    extent[usize()]  = rstd::cmp::max(extent[usize()], i32(1));
    extent[usize(1)] = rstd::cmp::max(extent[usize(1)], i32(1));
    (void)m_layer_link_source_extents.insert(id.value, rstd::move(extent));
}

SceneNode* Scene::RegisteredLayerLinkSource(WallpaperLayerId id) const {
    auto source = m_layer_link_source_nodes.get(id.value);
    return source.is_some() ? **source : nullptr;
}

Option<SceneNodeId> Scene::RegisteredLayerLinkSourceId(WallpaperLayerId id) const {
    auto source = m_layer_link_source_ids.get(id.value);
    return source.is_some() ? Some<SceneNodeId>(**source) : None<SceneNodeId>();
}

Option<WallpaperLayerId> Scene::ResolveLayerLinkSource(const SceneNode& node) const {
    auto source = m_node_link_sources.get(scene_id_key(node.Identity()));
    if (source.is_some()) return Some<WallpaperLayerId>(**source);
    auto wallpaper = node.WallpaperIdentity();
    if (wallpaper.is_none()) return None();
    auto registered = m_layer_link_source_ids.get(wallpaper->value);
    if (registered.is_some() && **registered != node.Identity()) return None();
    return wallpaper;
}

bool Scene::SetNodeVisible(SceneNode& node, bool visible) {
    auto       wallpaper            = node.WallpaperIdentity();
    const i32  id                   = wallpaper.is_some() ? wallpaper->value : i32(-1);
    const bool was_elidable         = id >= i32() && m_elidable_layer_ids.contains(id);
    const bool visibility_changed   = node.Visible() != visible;
    const bool publishes_output     = node.HasLayer() && node.Layer()->PublishesOutput();
    node.m_visibility_affects_alpha = ! publishes_output;
    node.SetVisible(visible);
    if (publishes_output) {
        node.Layer()->SetVisibleOutputEnabled(visible);
    }
    if (id < i32()) {
        (void)RegisterNode(node);
        return visibility_changed;
    }

    if (! visible) {
        MarkLayerVisibilityElidable(WallpaperLayerId { .value = id });
        const bool is_elidable = m_elidable_layer_ids.contains(id);
        return was_elidable != is_elidable;
    }

    if (! m_visibility_elidable_layer_ids.remove(id)) return false;
    RebuildElidableLayerIds();
    const bool is_elidable = m_elidable_layer_ids.contains(id);
    return was_elidable != is_elidable;
}

bool Scene::ApplyUserNodeVisibilityBindings(std::string_view key, const Json& property) {
    bool requires_graph_rebuild = false;
    if (m_resource_index.Empty()) RebuildResourceIndex();
    auto nodes = m_resource_index.Nodes();
    for (usize index {}; index < nodes.len(); ++index) {
        auto* node = nodes[index];
        if (node == nullptr) continue;
        if (auto visible = ResolveSceneUserVisibilityBinding(
                node->VisibleUserBinding(), rstd::cppstd::as_str(key).unwrap(), property)) {
            requires_graph_rebuild |= SetNodeVisible(*node, *visible);
        }
    }
    return requires_graph_rebuild;
}

Option<SceneImageEffectRef> Scene::FindNodeImageEffect(const SceneNode& node,
                                                       std::string_view name) {
    if (! node.HasLayer()) return None();
    const auto& effect_layer = node.Layer();
    if (! effect_layer) return None();
    auto effect = effect_layer->FindEffect(name);
    if (! effect) return None();
    auto owner = RegisterNode(const_cast<SceneNode&>(node));
    auto id    = RegisterEffect(owner, *effect_layer, rstd::move(effect));
    return id.Valid() ? Some(SceneImageEffectRef { .id = id }) : None();
}

Option<SceneImageEffectRef> Scene::FindNodeImageEffect(const SceneNode& node, usize index) {
    if (! node.HasLayer()) return None();
    const auto& effect_layer = node.Layer();
    if (! effect_layer || index >= effect_layer->EffectCount()) return None();
    auto effect = effect_layer->GetEffect(index);
    if (! effect) return None();
    auto owner = RegisterNode(const_cast<SceneNode&>(node));
    auto id    = RegisterEffect(owner, *effect_layer, rstd::move(effect));
    return id.Valid() ? Some(SceneImageEffectRef { .id = id }) : None();
}

usize Scene::NodeImageEffectCount(const SceneNode& node) {
    if (! node.HasLayer()) return usize();
    const auto& effect_layer = node.Layer();
    return effect_layer ? effect_layer->EffectCount() : usize();
}

String Scene::ImageEffectName(const SceneImageEffectRef& ref) const {
    if (! ref.id.Valid() || ref.id.generation != m_resource_generation) return {};
    auto record = m_image_effects.get(scene_id_key(ref.id.index, ref.id.generation));
    if (record.is_none() || ! (**record).effect) return {};
    return String::make(rstd::cppstd::as_str((**record).effect->name).unwrap());
}

bool Scene::ImageEffectRuntimeVisible(const SceneImageEffectRef& ref) const {
    if (! ref.id.Valid() || ref.id.generation != m_resource_generation) return false;
    auto record = m_image_effects.get(scene_id_key(ref.id.index, ref.id.generation));
    return record.is_some() && (**record).effect && (**record).effect->runtime_visible;
}

SceneMaterial* Scene::ImageEffectMaterial(const SceneImageEffectRef& ref, usize index) {
    if (! ref.id.Valid() || ref.id.generation != m_resource_generation) return nullptr;
    auto record = m_image_effects.get(scene_id_key(ref.id.index, ref.id.generation));
    if (record.is_none() || ! (**record).effect || index >= usize((**record).effect->nodes.size()))
        return nullptr;
    auto node = (**record).effect->nodes.begin();
    std::advance(node, index.to_primitive());
    if (! node->sceneNode || ! node->sceneNode->HasMaterial()) return nullptr;
    return node->sceneNode->Mesh()->Material();
}

bool Scene::SetImageEffectRuntimeVisible(const SceneImageEffectRef& ref, bool visible) {
    if (! ref.id.Valid() || ref.id.generation != m_resource_generation) return false;
    auto record = m_image_effects.get_mut(scene_id_key(ref.id.index, ref.id.generation));
    if (record.is_none() || (**record).layer == nullptr || ! (**record).effect) return false;
    if (! (**record).layer->SetEffectRuntimeVisible(*(**record).effect, visible)) return false;
    m_render_graph_dirty = true;
    return true;
}

bool Scene::ApplyUserImageEffectVisibilityBindings(std::string_view key, const Json& property) {
    if (m_resource_index.Empty()) RebuildResourceIndex();

    bool                                  requires_graph_rebuild = false;
    std::unordered_set<SceneImageEffect*> visited;
    auto                                  nodes = m_resource_index.Nodes();
    for (usize node_index {}; node_index < nodes.len(); ++node_index) {
        auto* node = nodes[node_index];
        if (node == nullptr || ! node->HasLayer()) continue;
        auto& effect_layer = node->Layer();
        for (usize i {}; i < effect_layer->EffectCount(); ++i) {
            auto& effect = effect_layer->GetEffect(i);
            if (! effect || ! visited.insert(effect.get()).second) continue;
            auto visible = ResolveSceneUserVisibilityBinding(
                effect->visible_user_binding, rstd::cppstd::as_str(key).unwrap(), property);
            if (! visible) continue;
            auto owner = RegisterNode(*node);
            auto id    = RegisterEffect(owner, *effect_layer, effect);
            if (SetImageEffectRuntimeVisible({ .id = id }, *visible)) {
                requires_graph_rebuild = true;
            }
        }
    }
    return requires_graph_rebuild;
}

bool Scene::ApplyUserLightVisibilityBindings(std::string_view key, const Json& property) {
    bool changed = false;
    for (auto& light : m_lights) {
        auto visible = ResolveSceneUserVisibilityBinding(
            light->visibleUserBinding(), rstd::cppstd::as_str(key).unwrap(), property);
        if (! visible) continue;
        changed |= light->runtimeVisible() != *visible;
        light->setRuntimeVisible(*visible);
    }
    return changed;
}

auto Scene::RegisterLight(Box<SceneLight> light) -> mut_ref<SceneLight> {
    m_lights.push(rstd::move(light));
    return m_lights[m_lights.len() - usize(1)].deref_mut();
}

auto Scene::Lights() const -> slice<Box<SceneLight>> { return m_lights.as_slice(); }

auto Scene::RegisterPostProcess(Box<ScenePostProcess> post_process) -> mut_ref<ScenePostProcess> {
    m_post_processes.push(rstd::move(post_process));
    return m_post_processes[m_post_processes.len() - usize(1)].deref_mut();
}

auto Scene::PostProcesses() const -> slice<Box<ScenePostProcess>> {
    return m_post_processes.as_slice();
}

void Scene::RegisterShadowDefinition(SceneShadowDefinition definition) {
    m_shadow_definitions.push(rstd::move(definition));
}

auto Scene::ShadowDefinitions() const -> slice<SceneShadowDefinition> {
    return m_shadow_definitions.as_slice();
}

bool Scene::ApplyUserCameraPathVisibilityBindings(std::string_view key, const Json& property) {
    auto paths = m_camera_path_user_index.get(rstd::cppstd::as_str(key).unwrap());
    if (paths.is_none()) return false;

    bool changed = false;
    for (const auto& path : **paths) {
        auto enabled = ResolveSceneUserVisibilityBinding(
            path->visible_user_binding, rstd::cppstd::as_str(key).unwrap(), property);
        if (! enabled) continue;
        changed |= path->enabled != *enabled;
        path->SetEnabled(*enabled);
    }
    return changed;
}

void Scene::RegisterCameraPath(Arc<SceneCameraPath> path) { m_camera_paths.push(rstd::move(path)); }

void Scene::RegisterCameraPathUserBinding(String key, Arc<SceneCameraPath> path) {
    auto paths = m_camera_path_user_index.get_mut(key.as_str());
    if (paths.is_some()) {
        (**paths).push(rstd::move(path));
        return;
    }
    Vec<Arc<SceneCameraPath>> values;
    values.push(rstd::move(path));
    (void)m_camera_path_user_index.insert(rstd::move(key), rstd::move(values));
}

void Scene::RegisterLinkedCamera(String source, String linked) {
    auto cameras = m_linked_cameras.get_mut(source.as_str());
    if (cameras.is_some()) {
        (**cameras).push(rstd::move(linked));
        return;
    }
    Vec<String> values;
    values.push(rstd::move(linked));
    (void)m_linked_cameras.insert(rstd::move(source), rstd::move(values));
}

void Scene::UpdateLinkedCamera(ref<str> name) {
    auto linked = m_linked_cameras.get(name);
    if (linked.is_none()) return;
    auto source = Camera(name);
    if (source.is_none()) return;
    for (const auto& camera_name : **linked) {
        auto camera = CameraMut(camera_name.as_str());
        if (camera.is_none()) continue;
        (**camera).Clone(**source);
        (**camera).Update();
    }
}

bool Scene::ResizeRenderTarget(ref<str> name, i32 width, i32 height) {
    auto target = RenderTargetMut(name);
    if (target.is_none()) return false;
    auto& value = **target;
    if (value.width == width && value.height == height) return false;

    auto event = m_render_target_dirty_events.get_mut(name);
    if (event.is_none()) {
        auto event_name = String::make(name);
        auto event_key  = event_name.clone();
        (void)m_render_target_dirty_events.insert(rstd::move(event_key),
                                                  SceneRenderTargetDirtyEvent {
                                                      .name       = rstd::move(event_name),
                                                      .old_width  = value.width,
                                                      .old_height = value.height,
                                                      .width      = width,
                                                      .height     = height,
                                                  });
    } else {
        (**event).width  = width;
        (**event).height = height;
    }
    value.width  = width;
    value.height = height;
    return true;
}

Vec<SceneRenderTargetDirtyEvent> Scene::ConsumePreparedRenderTargetDirtyEvents() {
    auto events =
        Vec<SceneRenderTargetDirtyEvent>::with_capacity(m_render_target_dirty_events.len());
    m_render_target_dirty_events.iter_mut().for_each([&](auto entry) {
        auto [_, event] = entry;
        events.push(rstd::move(*event));
    });
    m_render_target_dirty_events.clear();
    return events;
}

Vec<SceneMeshDirtyEvent> Scene::ConsumePreparedMeshDirtyEvents() {
    if (m_resource_index.Empty()) RebuildResourceIndex();

    Vec<SceneMeshDirtyEvent> events;
    auto                     meshes = m_resource_index.Meshes();
    for (usize index {}; index < meshes.len(); ++index) {
        auto* mesh = meshes[index];
        if (mesh == nullptr) continue;
        auto flags = mesh->DirtyFlags();
        if (flags == SceneMeshDirtyNone) continue;

        SceneMeshDirtyFlags consume     = SceneMeshDirtyNone;
        SceneMeshDirtyFlags event_flags = SceneMeshDirtyNone;
        if ((flags & SceneMeshDirtyLayout) != 0) {
            consume     = SceneMeshDirtyAll;
            event_flags = SceneMeshDirtyLayout;
        } else if (! mesh->Dynamic() && (flags & SceneMeshDirtyData) != 0) {
            consume     = SceneMeshDirtyData;
            event_flags = SceneMeshDirtyData;
        }
        if (consume == SceneMeshDirtyNone) continue;

        auto consumed = mesh->ConsumeDirtyFlags(consume);
        if ((consumed & SceneMeshDirtyLayout) != 0) {
            event_flags = SceneMeshDirtyLayout;
        } else if ((consumed & SceneMeshDirtyData) == 0) {
            continue;
        }

        if (auto id = m_resource_index.meshId(*mesh)) {
            events.push(SceneMeshDirtyEvent { .mesh = *id, .flags = event_flags });
        }
    }
    return events;
}

Vec<SceneMaterialDirtyEvent> Scene::ConsumePreparedMaterialDirtyEvents() {
    if (m_resource_index.Empty()) RebuildResourceIndex();

    Vec<SceneMaterialDirtyEvent> events;
    auto                         materials = m_resource_index.Materials();
    for (usize index {}; index < materials.len(); ++index) {
        auto* material = materials[index];
        if (material == nullptr) continue;
        auto flags = material->DirtyFlags();
        if (flags == SceneMaterialDirtyNone) continue;

        SceneMaterialDirtyFlags consume     = SceneMaterialDirtyNone;
        SceneMaterialDirtyFlags event_flags = SceneMaterialDirtyNone;
        if ((flags & SceneMaterialDirtyGraph) != 0) {
            consume     = SceneMaterialDirtyAll;
            event_flags = SceneMaterialDirtyGraph;
        } else {
            consume     = flags & (SceneMaterialDirtyResources | SceneMaterialDirtyPipeline |
                                   SceneMaterialDirtyTextureBindings);
            event_flags = consume;
        }
        if (consume == SceneMaterialDirtyNone) continue;

        auto consumed = material->ConsumeDirtyFlags(consume);
        if ((consumed & SceneMaterialDirtyGraph) != 0) {
            event_flags = SceneMaterialDirtyGraph;
        } else {
            event_flags = consumed & (SceneMaterialDirtyResources | SceneMaterialDirtyPipeline |
                                      SceneMaterialDirtyTextureBindings);
        }
        if (event_flags == SceneMaterialDirtyNone) continue;

        if (auto id = m_resource_index.materialId(*material)) {
            events.push(SceneMaterialDirtyEvent { .material = *id, .flags = event_flags });
        }
    }
    return events;
}

void Scene::TickCameraPaths() {
    if (m_camera_paths.is_empty()) return;

    HashSet<String> has_enabled;
    for (const auto& path : m_camera_paths) {
        if (path->enabled) has_enabled.insert(path->camera_name.clone());
    }

    HashSet<String> touched;
    HashSet<String> reset;
    for (const auto& path : m_camera_paths) {
        if (! path->enabled) continue;
        if (path->Tick(m_runtime.Frame().elapsed.to_primitive()))
            touched.insert(path->camera_name.clone());
    }
    for (const auto& path : m_camera_paths) {
        if (path->enabled || has_enabled.contains(path->camera_name.as_str()) ||
            reset.contains(path->camera_name.as_str()))
            continue;
        if (path->ApplyDefault()) touched.insert(path->camera_name.clone());
        reset.insert(path->camera_name.clone());
    }

    touched.iter().for_each([&](ref<String> name) {
        UpdateLinkedCamera(name->as_str());
    });
}

void Scene::TickMaterialShaderAnimations() {
    if (m_resource_index.Empty()) RebuildResourceIndex();

    auto materials = m_resource_index.Materials();
    for (usize index {}; index < materials.len(); ++index) {
        auto* material = materials[index];
        if (material == nullptr) continue;
        material->TickShaderValueAnimations(m_runtime.Frame().elapsed.to_primitive());
    }
}

void Scene::TickNodeFieldAnimations() {
    auto tick_node = [runtime = m_runtime.Frame().elapsed.to_primitive()](auto&      self,
                                                                          SceneNode& node) -> void {
        node.TickFieldAnimations(runtime);
        for (const auto& child : node.GetChildren()) self(self, *child);
    };
    tick_node(tick_node, *m_scene_graph);
}

void Scene::TickTransformUpdaters() {
    for (usize index {}; index < m_transform_updaters.len(); ++index) {
        m_transform_updaters[index]->operator()(m_runtime.Frame().elapsed);
    }
}

void Scene::CaptureCameraPathViewports() {
    for (auto& path : m_camera_paths) path->CaptureViewport();
}

void Scene::EnablePlanarReflection() {
    m_planar_reflection_enabled = true;
    const auto key              = rstd::cppstd::to_string(WE_REFLECTION_PREFIX);
    if (RenderTarget(as_str(key).unwrap()).is_some()) return;

    i32  width   = m_ortho[usize()];
    i32  height  = m_ortho[usize(1)];
    auto primary = RenderTarget(SpecTex_Default);
    if (primary.is_some()) {
        width  = (**primary).width;
        height = (**primary).height;
    }
    RegisterRenderTarget(String::make(as_str(key).unwrap()),
                         SceneRenderTarget {
                             .width             = width,
                             .height            = height,
                             .withDepth         = true,
                             .bind              = { .enable = true, .screen = true },
                             .preserve_on_write = true,
                         });
}

std::string Scene::EnsureLinkRenderTarget(WallpaperLayerId source_layer,
                                          const SceneNode& source_node) {
    auto link_key = GenLinkTex(static_cast<std::ptrdiff_t>(source_layer.value.to_primitive()));
    if (RenderTarget(as_str(link_key).unwrap()).is_none()) {
        auto      sz     = source_node.Size();
        auto      extent = m_layer_link_source_extents.get(source_layer.value);
        const i32 width  = extent.is_some() ? (**extent)[usize()]
                                            : i32(sz.x() > 0 ? static_cast<std::int32_t>(sz.x())
                                                             : m_ortho[usize()].to_primitive());
        const i32 height = extent.is_some() ? (**extent)[usize(1)]
                                            : i32(sz.y() > 0 ? static_cast<std::int32_t>(sz.y())
                                                             : m_ortho[usize(1)].to_primitive());
        RegisterRenderTarget(String::make(as_str(link_key).unwrap()),
                             SceneRenderTarget {
                                 .width                  = width,
                                 .height                 = height,
                                 .allowReuse             = false,
                                 .initialize_transparent = true,
                             });
    }
    return link_key;
}

} // namespace owe
