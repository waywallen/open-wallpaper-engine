module;
export module wescene.load_bench;

import rstd;
import rstd.bench;

using rstd::sync::Arc;

using namespace rstd::prelude;
using namespace rstd::literals;

export namespace owe
{

struct SceneLoadProbeIds {
    rstd::bench::probe::ProbeId preload_scene_document;
    rstd::bench::probe::ProbeId vulkan_init;
    rstd::bench::probe::ProbeId vulkan_instance;
    rstd::bench::probe::ProbeId vulkan_device;
    rstd::bench::probe::ProbeId vulkan_swapchain;
    rstd::bench::probe::ProbeId vulkan_resources;
    rstd::bench::probe::ProbeId load_total;
    rstd::bench::probe::ProbeId load_audio;
    rstd::bench::probe::ProbeId load_vfs_assets;
    rstd::bench::probe::ProbeId load_project_properties;
    rstd::bench::probe::ProbeId load_package;
    rstd::bench::probe::ProbeId load_scene_document;
    rstd::bench::probe::ProbeId parse_total;
    rstd::bench::probe::ProbeId parse_expand_objects;
    rstd::bench::probe::ProbeId parse_context;
    rstd::bench::probe::ProbeId parse_objects;
    rstd::bench::probe::ProbeId parse_object_image;
    rstd::bench::probe::ProbeId parse_object_particle;
    rstd::bench::probe::ProbeId parse_object_sound;
    rstd::bench::probe::ProbeId parse_object_light;
    rstd::bench::probe::ProbeId parse_object_text;
    rstd::bench::probe::ProbeId parse_object_model;
    rstd::bench::probe::ProbeId parse_object_camera;
    rstd::bench::probe::ProbeId parse_post_process;
    rstd::bench::probe::ProbeId parse_finalize;
    rstd::bench::probe::ProbeId load_runtime_setup;
    rstd::bench::probe::ProbeId load_initial_properties;
    rstd::bench::probe::ProbeId render_load;
    rstd::bench::probe::ProbeId render_snapshot;
    rstd::bench::probe::ProbeId render_graph_build;
    rstd::bench::probe::ProbeId render_graph_compile;
    rstd::bench::probe::ProbeId render_program_build;
    rstd::bench::probe::ProbeId render_requests;
    rstd::bench::probe::ProbeId render_resources_prepare;
    rstd::bench::probe::ProbeId render_texture_plan;
    rstd::bench::probe::ProbeId render_texture_decode;
    rstd::bench::probe::ProbeId render_texture_upload_prepare;
    rstd::bench::probe::ProbeId render_texture_upload_wait;
    rstd::bench::probe::ProbeId render_scopes;
    rstd::bench::probe::ProbeId render_upload_submit;
    rstd::bench::probe::ProbeId render_user_property;
    rstd::bench::probe::ProbeId render_user_property_apply;
    rstd::bench::probe::ProbeId render_user_property_refresh;
    rstd::bench::probe::ProbeId render_user_property_graph_rebuild;
    rstd::bench::probe::ProbeId render_first_frame;
    rstd::bench::probe::ProbeId render_first_frame_prepare;
    rstd::bench::probe::ProbeId render_first_draw;
};

struct SceneLoadBenchRecorderView {
    rstd::bench::probe::ProbeRecorder* recorder { nullptr };
    const SceneLoadProbeIds*           ids { nullptr };

    explicit operator bool() const noexcept { return recorder != nullptr && ids != nullptr; }
};

using SceneLoadSpanGuard = rstd::bench::probe::SpanGuard;

auto SceneLoadSpan(SceneLoadBenchRecorderView  view,
                   rstd::bench::probe::ProbeId SceneLoadProbeIds::* probe) noexcept
    -> rstd::bench::probe::SpanGuard {
    return view ? view.recorder->span(view.ids->*probe) : rstd::bench::probe::SpanGuard {};
}

class SceneLoadBenchContext;
using SceneLoadBenchHandle = Arc<SceneLoadBenchContext>;

class SceneLoadBenchContext {
public:
    SceneLoadBenchContext(const SceneLoadBenchContext&)                    = delete;
    auto operator=(const SceneLoadBenchContext&) -> SceneLoadBenchContext& = delete;

    auto run_id() const noexcept -> u64 { return m_run_id; }
    auto ids() const noexcept -> const SceneLoadProbeIds& { return m_ids; }
    auto session() const noexcept -> const rstd::bench::probe::ProbeSession& { return m_session; }
    auto schema_owner() const noexcept -> Arc<rstd::bench::probe::ProbeSchema> {
        return m_schema.clone();
    }
    auto output_path() const noexcept -> ref<rstd::path::Path> { return m_output_path.as_path(); }

    void add_preload_batch(rstd::bench::probe::ProbeBatch batch) {
        m_preload_batches.push(rstd::move(batch));
    }

    auto take_preload_batches() -> Vec<rstd::bench::probe::ProbeBatch> {
        auto batches      = rstd::move(m_preload_batches);
        m_preload_batches = Vec<rstd::bench::probe::ProbeBatch>::make();
        return batches;
    }

private:
    struct FactoryToken {};
    friend auto CreateSceneLoadBench(ref<str>) -> Option<SceneLoadBenchHandle>;

public:
    SceneLoadBenchContext(FactoryToken, rstd::path::PathBuf output_path, SceneLoadProbeIds ids,
                          Arc<rstd::bench::probe::ProbeSchema> schema, u64 run_id)
        : m_output_path(rstd::move(output_path)),
          m_ids(ids),
          m_schema(schema.clone()),
          m_session(rstd::bench::probe::ProbeSession::new_(rstd::move(schema))),
          m_run_id(run_id),
          m_preload_batches(Vec<rstd::bench::probe::ProbeBatch>::make()) {}

    rstd::path::PathBuf                  m_output_path;
    SceneLoadProbeIds                    m_ids;
    Arc<rstd::bench::probe::ProbeSchema> m_schema;
    rstd::bench::probe::ProbeSession     m_session;
    u64                                  m_run_id;
    Vec<rstd::bench::probe::ProbeBatch>  m_preload_batches;
};

auto CreateSceneLoadBench(ref<str> output_path) -> Option<SceneLoadBenchHandle>;

} // namespace owe

namespace
{

auto RegisterProbe(rstd::bench::probe::ProbeRegistry& registry, ref<str> label)
    -> rstd::bench::probe::ProbeId {
    auto registered = registry.register_probe(label);
    if (registered.is_err()) rstd::panic { "scene load probe catalog exhausted" };
    return rstd::move(registered).unwrap_unchecked();
}

} // namespace

namespace owe
{

auto CreateSceneLoadBench(ref<str> output_path) -> Option<SceneLoadBenchHandle> {
    if (output_path.is_empty()) return None();

    auto              registry = rstd::bench::probe::ProbeRegistry::new_();
    SceneLoadProbeIds ids {
        .preload_scene_document   = RegisterProbe(registry, "preload.scene_document"_str),
        .vulkan_init              = RegisterProbe(registry, "vulkan.init"_str),
        .vulkan_instance          = RegisterProbe(registry, "vulkan.instance"_str),
        .vulkan_device            = RegisterProbe(registry, "vulkan.device"_str),
        .vulkan_swapchain         = RegisterProbe(registry, "vulkan.swapchain"_str),
        .vulkan_resources         = RegisterProbe(registry, "vulkan.resources"_str),
        .load_total               = RegisterProbe(registry, "load.total"_str),
        .load_audio               = RegisterProbe(registry, "load.audio"_str),
        .load_vfs_assets          = RegisterProbe(registry, "load.vfs.assets"_str),
        .load_project_properties  = RegisterProbe(registry, "load.project_properties"_str),
        .load_package             = RegisterProbe(registry, "load.package"_str),
        .load_scene_document      = RegisterProbe(registry, "load.scene_document"_str),
        .parse_total              = RegisterProbe(registry, "parse.total"_str),
        .parse_expand_objects     = RegisterProbe(registry, "parse.expand_objects"_str),
        .parse_context            = RegisterProbe(registry, "parse.context"_str),
        .parse_objects            = RegisterProbe(registry, "parse.objects"_str),
        .parse_object_image       = RegisterProbe(registry, "parse.object.image"_str),
        .parse_object_particle    = RegisterProbe(registry, "parse.object.particle"_str),
        .parse_object_sound       = RegisterProbe(registry, "parse.object.sound"_str),
        .parse_object_light       = RegisterProbe(registry, "parse.object.light"_str),
        .parse_object_text        = RegisterProbe(registry, "parse.object.text"_str),
        .parse_object_model       = RegisterProbe(registry, "parse.object.model"_str),
        .parse_object_camera      = RegisterProbe(registry, "parse.object.camera"_str),
        .parse_post_process       = RegisterProbe(registry, "parse.post_process"_str),
        .parse_finalize           = RegisterProbe(registry, "parse.finalize"_str),
        .load_runtime_setup       = RegisterProbe(registry, "load.runtime_setup"_str),
        .load_initial_properties  = RegisterProbe(registry, "load.initial_properties"_str),
        .render_load              = RegisterProbe(registry, "render.load"_str),
        .render_snapshot          = RegisterProbe(registry, "render.snapshot"_str),
        .render_graph_build       = RegisterProbe(registry, "render.graph.build"_str),
        .render_graph_compile     = RegisterProbe(registry, "render.graph.compile"_str),
        .render_program_build     = RegisterProbe(registry, "render.program.build"_str),
        .render_requests          = RegisterProbe(registry, "render.requests"_str),
        .render_resources_prepare = RegisterProbe(registry, "render.resources.prepare"_str),
        .render_texture_plan      = RegisterProbe(registry, "render.resources.texture.plan"_str),
        .render_texture_decode    = RegisterProbe(registry, "render.resources.texture.decode"_str),
        .render_texture_upload_prepare =
            RegisterProbe(registry, "render.resources.texture.upload.prepare"_str),
        .render_texture_upload_wait =
            RegisterProbe(registry, "render.resources.texture.upload.wait"_str),
        .render_scopes                = RegisterProbe(registry, "render.scopes"_str),
        .render_upload_submit         = RegisterProbe(registry, "render.upload.submit"_str),
        .render_user_property         = RegisterProbe(registry, "render.user_property"_str),
        .render_user_property_apply   = RegisterProbe(registry, "render.user_property.apply"_str),
        .render_user_property_refresh = RegisterProbe(registry, "render.user_property.refresh"_str),
        .render_user_property_graph_rebuild =
            RegisterProbe(registry, "render.user_property.graph_rebuild"_str),
        .render_first_frame         = RegisterProbe(registry, "render.first_frame"_str),
        .render_first_frame_prepare = RegisterProbe(registry, "render.first_frame.prepare"_str),
        .render_first_draw          = RegisterProbe(registry, "render.first_draw"_str),
    };
    auto schema = rstd::move(registry).freeze();
    auto path   = rstd::path::PathBuf::from(output_path);

    static rstd::sync::atomic::Atomic<u64> next_run_id { u64(1) };
    auto run_id = next_run_id.fetch_add(u64(1), rstd::sync::atomic::Ordering::Relaxed);
    return Some(SceneLoadBenchHandle::make(
        SceneLoadBenchContext::FactoryToken {}, rstd::move(path), ids, rstd::move(schema), run_id));
}

} // namespace owe
