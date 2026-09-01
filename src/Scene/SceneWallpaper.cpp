module;

#include <rstd/enum.hpp>

module wescene.scene_wallpaper;
import wescene.types;
import wescene.utils;
import wescene.scene;

import eigen;
import owe.audio_response;
import owe.scene_audio_response;
import owe.user_property;
import rstd;
import rstd.bench;
import rstd.log;
import rstd.cppstd;
import wavsen.audio;
import wescene.fs;
import wescene.load_bench;
import wescene.timer;
import wescene.pkg.parse;
import wescene.pkg_fs;
import wescene.rgraph;
import wescene.resource;
import wescene.scene_user_property;
import wescene.script;
import wescene.vulkan_render;

using namespace owe;
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;
using rstd::sync::atomic::Atomic;

namespace owe
{

class RenderMsg final {
    RSTD_ENUM(RenderMsg,
              (Init, (Box<RenderInitInfo> info; Option<SceneLoadBenchHandle> load_bench;)),
              (SetScene, (Box<Scene> scene; Arc<UniformRuntimeInput> uniform_input;
                          Option<SceneLoadBenchHandle> load_bench; Option<u64> random_seed;)),
              (SetFillMode, (FillMode mode;)), (SetSpeed, (f32 speed;)),
              (SetUserProperty, (std::string key; Json property;)),
              (SetMediaStatus, (MediaStatus status;)),
              (SetAudioResponseDemandCallback, (AudioResponseDemandCallback callback;)),
              (SetAudioResponseEnabled, (bool enabled;)),
              (SetAudioPcmWindow, (audio::PcmWindow window;)), (EndAudioResponse),
              (Stop, (bool stop;)), (Draw), (SwapchainReady, (bool ready; u32 width; u32 height;)),
              (RequestPreparedPassDiagnostics, (RenderPassDiagnosticCallback cb;)), (Shutdown))
};

class MainMsg final {
    RSTD_ENUM(MainMsg, (LoadScene, (vulkan::DeviceCapabilities capabilities;)),
              (Configure, (SceneWallpaperConfig config;)), (SetFps, (u32 fps;)),
              (SetVolume, (f32 volume;)), (SetVolumeScale, (f32 scale; u32 fade_ms { 0 };)),
              (SetMuted, (bool muted;)),
              (SetAudioClientIdentity, (SceneAudioClientIdentity identity;)),
              (AudioDeviceEvent, (wavsen::audio::AudioDeviceEvent event;)),
              (SetFillMode, (FillMode mode;)), (SetSpeed, (f32 speed;)),
              (SetUserProperty, (std::string key; Json value;)),
              (SetFirstFrameCallback, (FirstFrameCallback cb;)),
              (SetUserPropertyDiagnosticCallback, (UserPropertyDiagnosticCallback cb;)),
              (UserPropertyDiagnostics, (Vec<SceneUserPropertyDiagnostic> diagnostics;)),
              (SceneClearColorChanged, (f32 r; f32 g; f32 b;)),
              (PreparedPassDiagnostics, (RenderPassDiagnosticCallback                cb;
                                         std::vector<vulkan::PreparedPassDiagnostic> diagnostics;)),
              (Stop, (bool stop; u32 fade_ms { 0 }; bool scale_audio { false };)),
              (PauseAudio, (u64 generation { 0 };)),
              (LoadBenchBatch,
               (Option<SceneLoadBenchHandle> context; rstd::bench::probe::ProbeBatch batch;)),
              (LoadBenchFinish, (Option<SceneLoadBenchHandle> context;)),
              (FirstFrame, (Option<SceneLoadBenchHandle>           context;
                            Option<rstd::bench::probe::ProbeBatch> batch;)),
              (Shutdown))
};

namespace
{

auto BenchContext(Option<SceneLoadBenchHandle>& handle) -> SceneLoadBenchContext& {
    return **handle;
}

auto BenchContext(const Option<SceneLoadBenchHandle>& handle) -> const SceneLoadBenchContext& {
    return **handle;
}

auto CloneAudioResponseDemandCallback(const Option<AudioResponseDemandCallback>& callback)
    -> Option<AudioResponseDemandCallback> {
    if (callback.is_none()) return None();
    return Some(callback->clone());
}

auto CloneUserPropertyDiagnostics(slice<SceneUserPropertyDiagnostic> diagnostics)
    -> Vec<SceneUserPropertyDiagnostic> {
    auto cloned = Vec<SceneUserPropertyDiagnostic>::with_capacity(diagnostics.len());
    for (usize index {}; index < diagnostics.len(); ++index)
        cloned.push(diagnostics[index].Clone());
    return cloned;
}

float LocalTimeOfDay() {
    auto local = rstd::time::OffsetDateTime::now_local();
    if (local.is_err()) return 0.0f;

    auto time      = local->time();
    auto subsecond = static_cast<double>(time.nanosecond().to_primitive()) /
                     static_cast<double>(rstd::time::NANOS_PER_SEC.to_primitive());
    auto seconds   = double(time.hour().to_primitive() * 3600 + time.minute().to_primitive() * 60 +
                            time.second().to_primitive()) +
                     subsecond;
    return static_cast<float>(seconds / 86400.0);
}

Json MakeUserPropertyDescriptor(Json value) {
    if (value.get("value"_str).is_some()) return value;
    auto object = rstd::json::Map::make();
    object.insert(::alloc::string::String::make("value"_str), rstd::move(value));
    return Json::Object(rstd::move(object));
}

Json RawUserProperty(std::string_view value) { return MakeUserPropertyWirePatch(value); }

Json InitialUserProperty(Json value) {
    if (value.is_string()) {
        auto raw = rstd::cppstd::to_string(*value.as_str());
        return RawUserProperty(raw);
    }
    return MakeUserPropertyDescriptor(rstd::move(value));
}

bool SameSceneMaterialId(SceneMaterialId lhs, SceneMaterialId rhs) {
    return lhs.index == rhs.index && lhs.generation == rhs.generation;
}

void PushUniqueMaterialId(Vec<SceneMaterialId>& materials, SceneMaterialId id) {
    for (usize index {}; index < materials.len(); ++index) {
        if (SameSceneMaterialId(materials[index], id)) return;
    }
    materials.push(rstd::move(id));
}

vulkan::PassInvalidationFlags MaterialDirtyToPassInvalidationFlags(SceneMaterialDirtyFlags flags) {
    vulkan::PassInvalidationFlags out = vulkan::PassInvalidationNone;
    if ((flags & SceneMaterialDirtyResources) != 0) {
        out |= vulkan::ToPassInvalidationFlags(vulkan::PassInvalidation::Resources);
    }
    if ((flags & SceneMaterialDirtyPipeline) != 0) {
        out |= vulkan::ToPassInvalidationFlags(vulkan::PassInvalidation::Pipeline) |
               vulkan::ToPassInvalidationFlags(vulkan::PassInvalidation::Framebuffer);
    }
    return out;
}

Json RuntimeTextureProperty(std::string value) {
    auto object = rstd::json::Map::make();
    object.insert(::alloc::string::String::make("type"_str), JsonFromStd("scenetexture"));
    object.insert(::alloc::string::String::make("value"_str), JsonFromStd(value));
    return Json::Object(rstd::move(object));
}

owe::script::MediaStatus ToScriptMediaStatus(const MediaStatus& status) {
    return owe::script::MediaStatus { .state            = status.state,
                                      .title            = status.title,
                                      .artist           = status.artist,
                                      .album            = status.album,
                                      .album_artist     = status.album_artist,
                                      .art_url          = status.art_url,
                                      .previous_art_url = status.previous_art_url };
}

void MergeProjectUserProperties(const std::filesystem::path& project_dir, rstd::json::Map& out) {
    const auto    project_path = project_dir / "project.json";
    std::ifstream is(project_path);
    if (! is) return;

    std::string source(std::istreambuf_iterator<char>(is), {});
    auto        parsed = ParseJson(source, { .allow_comments = true });
    if (parsed.is_err()) {
        rstd_warn("Can't parse {}: {}", project_path.string(), parsed.unwrap_err());
        return;
    }
    auto root    = parsed.unwrap();
    auto general = root.get("general"_str);
    if (general.is_none()) return;
    auto properties = (*general)->get("properties"_str);
    if (properties.is_none()) return;
    auto object = (*properties)->as_object();
    if (object.is_none()) return;

    (*object)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        const auto  raw_key           = rstd::cppstd::as_string_view(entry_key->as_str());
        const auto& value             = *entry_value;
        std::string key               = CanonicalSceneUserPropertyKey(raw_key);
        auto        current           = out.get(rstd::cppstd::as_str(key).unwrap());
        auto        descriptor = current.is_some() ? MergeUserPropertyDescriptor(value, **current)
                                                   : MakeUserPropertyDescriptor(value.clone());
        out.insert(::alloc::string::String::make(rstd::cppstd::as_str(key).unwrap()),
                   rstd::move(descriptor));
    });
}

rstd::json::Map NormalizeUserProperties(const rstd::json::Map& input) {
    auto out = rstd::json::Map::make();
    input.iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        const auto  key               = rstd::cppstd::as_string_view(entry_key->as_str());
        const auto& value             = *entry_value;
        std::string canonical         = CanonicalSceneUserPropertyKey(key);
        if (key == canonical || out.get(rstd::cppstd::as_str(canonical).unwrap()).is_none()) {
            out.insert(::alloc::string::String::make(rstd::cppstd::as_str(canonical).unwrap()),
                       InitialUserProperty(value.clone()));
        }
    });
    return out;
}

} // namespace

using MainSender     = rstd::sync::mpmc::Sender<MainMsg>;
using MainReceiver   = rstd::sync::mpmc::Receiver<MainMsg>;
using RenderSender   = rstd::sync::mpmc::Sender<RenderMsg>;
using RenderReceiver = rstd::sync::mpmc::Receiver<RenderMsg>;

class SceneRenderController;

class SceneRuntimeController {
public:
    SceneRuntimeController();
    ~SceneRuntimeController();

    bool init();
    auto renderController() const { return m_render_controller.as_mut_ptr().as_raw_ptr(); }
    bool inited() const { return m_inited; }

    void post(MainMsg);
    void post(RenderMsg);

    void onLoadScene();
    void on(MainMsg::LoadScene_payload&&);
    void on(MainMsg::Configure_payload&&);
    void on(MainMsg::SetFps_payload&&);
    void on(MainMsg::SetVolume_payload&&);
    void on(MainMsg::SetVolumeScale_payload&&);
    void on(MainMsg::SetMuted_payload&&);
    void on(MainMsg::SetAudioClientIdentity_payload&&);
    void on(MainMsg::AudioDeviceEvent_payload&&);
    void on(MainMsg::SetFillMode_payload&&);
    void on(MainMsg::SetSpeed_payload&&);
    void on(MainMsg::SetUserProperty_payload&&);
    void on(MainMsg::SetFirstFrameCallback_payload&&);
    void on(MainMsg::SetUserPropertyDiagnosticCallback_payload&&);
    void on(MainMsg::UserPropertyDiagnostics_payload&&);
    void on(MainMsg::SceneClearColorChanged_payload&&);
    void on(MainMsg::PreparedPassDiagnostics_payload&&);
    void on(MainMsg::Stop_payload&&);
    void on(MainMsg::PauseAudio_payload&&);
    void on(MainMsg::LoadBenchBatch_payload&&);
    void on(MainMsg::LoadBenchFinish_payload&&);
    void on(MainMsg::FirstFrame_payload&&);

    bool isGenGraphviz() const { return m_config.graphviz; }

    void setOnClearColor(ClearColorCallback cb) { m_clear_color_cb = rstd::move(cb); }

private:
    MainSender sender() const;
    void       startMainLoop();
    void       stopMainLoop();
    void       loadScene();
    void       ensureLoadBench(const Option<SceneLoadBenchHandle>&);
    void       ingestLoadBenchBatch(const Option<SceneLoadBenchHandle>&,
                                    const rstd::bench::probe::ProbeBatch&);
    void       finishLoadBench();
    auto       loadBenchView() -> SceneLoadBenchRecorderView;
    auto       schemeColor() const -> Option<array<float, 3>>;
    void       publishClearColor(array<float, 3> fallback);

    bool m_inited { false };

    SceneWallpaperConfig               m_config;
    rstd::json::Map                    m_user_properties;
    Option<vulkan::DeviceCapabilities> m_render_capabilities;

    Box<wavsen::audio::SoundManager> m_sound_manager;
    FirstFrameCallback               m_first_frame_callback;
    UserPropertyDiagnosticCallback   m_user_property_diagnostic_cb;
    ClearColorCallback               m_clear_color_cb;
    u64                              m_audio_pause_generation {};
    bool                             m_audio_activated {};

    Option<SceneLoadBenchHandle>               m_load_bench;
    Option<rstd::bench::probe::ProbeCollector> m_load_bench_collector;
    Option<rstd::bench::probe::ProbeRecorder>  m_load_bench_recorder;
    Option<rstd::bench::probe::SpanGuard>      m_load_total_span;
    u64                                        m_latest_load_bench_run_id { 0 };

    Option<MainSender>                     m_main_tx;
    Option<MainReceiver>                   m_main_rx;
    Option<rstd::thread::JoinHandle<void>> m_main_thread;
    Box<SceneRenderController>             m_render_controller;
};

class SceneRenderController {
public:
    explicit SceneRenderController(SceneRuntimeController& main)
        : m_main(main), m_render(Box<vulkan::VulkanRender>::make()) {
        auto [tx, rx] = rstd::sync::mpmc::channel<RenderMsg>();
        m_tx          = Some(rstd::move(tx));
        m_rx          = Some(rstd::move(rx));
    }
    ~SceneRenderController() {
        stop();
        detachSceneAudioResponseDemandCallback();
        m_render->destroy();
        rstd_info("render handler deleted");
    }

    void start();
    void stop();
    void post(RenderMsg);
    auto sender() const -> RenderSender;

    void on(RenderMsg::Init_payload&&);
    void on(RenderMsg::SetScene_payload&&);
    void on(RenderMsg::SetFillMode_payload&&);
    void on(RenderMsg::SetSpeed_payload&&);
    void on(RenderMsg::SetUserProperty_payload&&);
    void on(RenderMsg::SetMediaStatus_payload&&);
    void on(RenderMsg::SetAudioResponseDemandCallback_payload&&);
    void on(RenderMsg::SetAudioResponseEnabled_payload&&);
    void on(RenderMsg::SetAudioPcmWindow_payload&&);
    void on(RenderMsg::Stop_payload&&);
    void onDraw();
    void on(RenderMsg::SwapchainReady_payload&&);
    void on(RenderMsg::RequestPreparedPassDiagnostics_payload&&);

    ExSwapchain* exSwapchain() const { return m_render->exSwapchain(); }
    int          takeLastFrameSyncFd() { return m_render->takeLastFrameSyncFd(); }
    bool         getDrmRenderNode(uint32_t& major, uint32_t& minor) const {
        return m_render->getDrmRenderNode(major, minor);
    }
    const vulkan::VulkanRender* render() const { return m_render.as_ptr().as_raw_ptr(); }

    bool renderInited() const { return m_render->inited(); }

    void setMousePos(double x, double y) {
        m_mouse_pos.store(array<float, 2> { static_cast<float>(x), static_cast<float>(y) });
    }

    // Edge-events for the cursor button stream. Each call from the input
    // thread sets/clears the held bit and records the edge so the next
    // TickSceneScripts can fire cursorDown/Up. fetch_or guards against
    // press-release-press coalescing between ticks (rare).
    void setMouseButton(int button, bool down) {
        if (button < 0 || button > 31) return;
        const u32 mask(1u << button);
        if (down) {
            m_buttons_down.fetch_or(mask);
            m_buttons_pressed.fetch_or(mask);
        } else {
            m_buttons_down.fetch_and(~mask);
            m_buttons_released.fetch_or(mask);
        }
    }
    void setMouseInWindow(bool in) { m_cursor_in_window.store(in); }
    u32  buttonsDown() const { return m_buttons_down.load(); }
    u32  consumePressed() { return m_buttons_pressed.exchange(u32()); }
    u32  consumeReleased() { return m_buttons_released.exchange(u32()); }
    bool cursorInWindow() const { return m_cursor_in_window.load(); }

    void setMainSender(MainSender main_tx) { m_main_tx = Some(rstd::move(main_tx)); }

    FrameTimer frame_timer { [] {
    } };
    FpsCounter fps_counter;

private:
    auto loadBenchView() -> SceneLoadBenchRecorderView;
    void rebuildRenderGraph(vulkan::RenderGraphResourceRetention retention, bool evict_meshes,
                            SceneLoadBenchRecorderView load_bench = {});
    void consumeDirtyEventsCoveredByGraphRebuild();
    void refreshPreparedRenderTargetDirtyEvents();
    void refreshPreparedMeshDirtyEvents();
    void refreshPreparedMaterialDirtyEvents();
    void detachSceneAudioResponseDemandCallback();

    SceneRuntimeController& m_main;

    Box<vulkan::VulkanRender>        m_render;
    Option<Box<Scene>>               m_scene_owner;
    Scene*                           m_scene { nullptr };
    Option<Arc<UniformRuntimeInput>> m_uniform_input_owner;
    UniformRuntimeInput*             m_uniform_input { nullptr };
    // Identity snapshot owned by the compiled render graph.
    RenderSceneSnapshot                 m_render_scene;
    std::string                         m_last_art_url;
    std::string                         m_last_prev_art_url;
    bool                                m_has_media { false };
    Option<Box<rg::RenderGraph>>        m_rg;
    f32                                 m_speed { 1.0f };
    FillMode                            m_fillmode { FillMode::ASPECTCROP };
    bool                                m_stopped { false };
    Option<AudioResponseDemandCallback> m_audio_response_demand_callback;
    bool                                m_audio_response_enabled { true };
    audio::ResponseEngine               m_audio_response_engine;
    scene_audio::ResponseProcessor      m_scene_audio_response;
    bool                                m_first_frame_ok { false };

    Atomic<array<float, 2>> m_mouse_pos { array<float, 2> { 0.5f, 0.5f } };
    Atomic<u32>             m_buttons_down {};
    Atomic<u32>             m_buttons_pressed {};
    Atomic<u32>             m_buttons_released {};
    Atomic<bool>            m_cursor_in_window { false };

    Option<RenderSender>                   m_tx;
    Option<RenderReceiver>                 m_rx;
    Option<rstd::thread::JoinHandle<void>> m_thread;
    Option<MainSender>                     m_main_tx;

    Option<SceneLoadBenchHandle>              m_load_bench;
    Option<rstd::bench::probe::ProbeRecorder> m_load_bench_recorder;

    // Strong ref kept here, weak copy captured by the swapchain callback.
    std::shared_ptr<RenderSender> m_swapchain_tx;
};

auto SceneRenderController::sender() const -> RenderSender {
    if (! m_tx) rstd::panic { "render mailbox is stopped" };
    return *m_tx;
}

void SceneRenderController::post(RenderMsg msg) {
    if (m_tx) (void)m_tx->send(rstd::move(msg));
}

auto SceneRenderController::loadBenchView() -> SceneLoadBenchRecorderView {
    return {
        .recorder = m_load_bench_recorder ? &*m_load_bench_recorder : nullptr,
        .ids      = m_load_bench ? &BenchContext(m_load_bench).ids() : nullptr,
    };
}

void SceneRenderController::detachSceneAudioResponseDemandCallback() {
    if (! m_scene) return;
    m_scene->AudioDemandMut()->SetCallback(None<AudioResponseDemandCallback>());
}

void SceneRenderController::start() {
    if (m_thread) return;
    if (! m_rx) rstd::panic { "render mailbox cannot be restarted" };

    auto rx     = rstd::move(m_rx.take()).unwrap_unchecked();
    auto thread = rstd::thread::spawn([this, rx = rstd::move(rx)]() mutable {
        rstd_info("render loop started");
        while (true) {
            auto received = rx.recv();
            if (received.is_err()) break;

            auto message  = rstd::move(received).unwrap();
            bool shutdown = false;
            RSTD_MATCH(rstd::move(message)) {
                RSTD_CASE_PAYLOAD(Init, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetScene, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetFillMode, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetSpeed, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetUserProperty, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetMediaStatus, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetAudioResponseDemandCallback, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetAudioResponseEnabled, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetAudioPcmWindow, value) { on(rstd::move(value)); }
                RSTD_CASE(EndAudioResponse) {
                    m_audio_response_engine.end();
                    m_scene_audio_response.end();
                }
                RSTD_CASE_PAYLOAD(Stop, value) { on(rstd::move(value)); }
                RSTD_CASE(Draw) { onDraw(); }
                RSTD_CASE_PAYLOAD(SwapchainReady, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(RequestPreparedPassDiagnostics, value) { on(rstd::move(value)); }
                RSTD_CASE(Shutdown) {
                    frame_timer.Stop();
                    frame_timer.SetCallback([] {
                    });
                    m_swapchain_tx.reset();
                    shutdown = true;
                }
            }
            if (shutdown) break;
        }
        rstd_info("render loop stopped");
    });
    m_thread    = Some(rstd::move(thread).unwrap());
}

void SceneRenderController::stop() {
    if (! m_thread) {
        frame_timer.Stop();
        frame_timer.SetCallback([] {
        });
        m_swapchain_tx.reset();
        m_tx      = None();
        m_rx      = None();
        m_main_tx = None();
        return;
    }
    if (rstd::thread::current_id() == m_thread->thread().id()) {
        rstd::panic { "SceneRenderController destroyed from render thread" };
    }

    post(RenderMsg::Shutdown());
    auto thread = rstd::move(m_thread.take()).unwrap_unchecked();
    rstd::move(thread).join().unwrap();
    m_tx      = None();
    m_main_tx = None();
}

// ---- SceneRenderController message handlers ---------------------------------

void SceneRenderController::on(RenderMsg::Stop_payload&& m) {
    m_stopped = m.stop;
    if (m.stop)
        frame_timer.Stop();
    else
        frame_timer.Run();
}

void SceneRenderController::onDraw() {
    frame_timer.FrameBegin();
    if (m_rg.is_some()) {
        const bool first_draw = ! m_first_frame_ok;
        auto       load_bench = loadBenchView();
        auto       first_frame_span =
            first_draw ? SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_first_frame)
                       : rstd::bench::probe::SpanGuard {};
        auto first_frame_prepare_span =
            first_draw ? SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_first_frame_prepare)
                       : rstd::bench::probe::SpanGuard {};
        {
            auto pos = m_mouse_pos.load();
            m_scene->SetPointerPosition(array<float, 2> { pos[usize()], pos[usize(1)] });
            if (m_uniform_input) m_uniform_input->SetPointerInput(pos[usize()], pos[usize(1)]);
        }
        // Drive any per-Scene scenescripts before particle emission.
        // Scripts mutate SceneNode transforms (scale/origin/angles) so
        // they need to run before the matrix-derivation in the
        // uniform evaluation runs inside drawFrame after scripts have updated Scene state.
        // The runtime is a no-op when no ScriptScene is installed.
        {
            owe::script::FrameInputs fi;
            fi.frametime   = static_cast<float>(m_scene->Runtime().Frame().delta.to_primitive() *
                                                m_speed.to_primitive());
            fi.runtime     = static_cast<float>(m_scene->Runtime().Frame().elapsed.to_primitive());
            fi.time_of_day = LocalTimeOfDay();
            auto ortho     = m_scene->Ortho();
            fi.canvas_w    = static_cast<float>(ortho[usize()].to_primitive());
            fi.canvas_h    = static_cast<float>(ortho[usize(1)].to_primitive());
            fi.screen_w    = fi.canvas_w;
            fi.screen_h    = fi.canvas_h;
            {
                auto pos    = m_mouse_pos.load();
                fi.cursor_x = pos[usize()];
                fi.cursor_y = pos[usize(1)];
            }
            fi.cursor_in_window       = cursorInWindow();
            fi.mouse_buttons_down     = buttonsDown().to_primitive();
            fi.mouse_buttons_pressed  = consumePressed().to_primitive();
            fi.mouse_buttons_released = consumeReleased().to_primitive();
            (void)m_scene_audio_response.advance(fi.frametime, fi.audio);
            if (m_uniform_input) m_uniform_input->SetAudioSpectrum(fi.audio);
            m_scene->TickNodeFieldAnimations();
            owe::script::TickSceneScripts(*m_scene, fi);
            m_scene->TickCameraPaths();
            m_scene->TickMaterialShaderAnimations();
            m_scene->TickTransformUpdaters();
            if (m_scene->ConsumeRenderGraphDirty()) {
                rebuildRenderGraph(
                    vulkan::RenderGraphResourceRetention::KeepSceneTextures, false, load_bench);
            }
        }
        m_scene->Runtime().BeforeRender();
        refreshPreparedRenderTargetDirtyEvents();
        refreshPreparedMeshDirtyEvents();
        refreshPreparedMaterialDirtyEvents();

        /* Advance video textures (no-op if none) before drawFrame so
         * the new RGBA frame is sampled by the same render pass. */
        m_render->pumpVideoTextures(frame_timer.TargetFrameTime() * m_speed.to_primitive());

        /* Upload any glyph rects the actuators added this tick. Runs after
         * TickSceneScripts (which calls FontFace::Populate) and before
         * drawFrame so newly-rasterised glyphs are visible the same frame. */
        m_render->pumpFontAtlases(*m_scene);

        (void)first_frame_prepare_span.finish();
        auto first_draw_span =
            first_draw ? SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_first_draw)
                       : rstd::bench::probe::SpanGuard {};
        m_render->drawFrame(*m_scene);
        (void)first_draw_span.finish();
        (void)first_frame_span.finish();

        m_scene->PassFrameTime(frame_timer.TargetFrameTime() * m_speed.to_primitive());

        if (first_draw) {
            m_first_frame_ok = true;
            Option<rstd::bench::probe::ProbeBatch> batch;
            if (m_load_bench_recorder) {
                auto drained = m_load_bench_recorder->drain();
                if (drained.is_ok()) {
                    batch = Some(rstd::move(drained).unwrap_unchecked());
                } else {
                    rstd_warn("render probe drain failed");
                }
            }
            if (m_main_tx) {
                (void)m_main_tx->send(MainMsg::FirstFrame(m_load_bench.clone(), rstd::move(batch)));
            }
            m_load_bench_recorder = None();
            m_load_bench          = None();
        }
    }
    frame_timer.FrameEnd();
}

void SceneRenderController::on(RenderMsg::SetFillMode_payload&& m) {
    m_fillmode = m.mode;
    if (m_scene && renderInited()) {
        m_render->UpdateCameraFillMode(*m_scene, m_fillmode);
    }
}

void SceneRenderController::rebuildRenderGraph(vulkan::RenderGraphResourceRetention retention,
                                               bool                                 evict_meshes,
                                               SceneLoadBenchRecorderView           load_bench) {
    if (! m_scene || ! renderInited()) return;
    if (m_rg.is_some()) m_render->clearLastRenderGraph(retention);
    if (evict_meshes) m_render->evictUnusedMeshes();
    m_render->configureRenderTargets(*m_scene);
    {
        auto snapshot_span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_snapshot);
        m_render_scene     = ExtractRenderSceneSnapshot(*m_scene);
    }
    {
        auto graph_span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_graph_build);
        m_rg            = Some(sceneToRenderGraph(*m_scene, m_render_scene));
    }

    if (m_main.isGenGraphviz()) (*m_rg)->ToGraphviz("graph.dot"_str);
    m_render->compileRenderGraph(*m_scene, **m_rg, m_render_scene, load_bench);
    m_render->UpdateCameraFillMode(*m_scene, m_fillmode);
    consumeDirtyEventsCoveredByGraphRebuild();
    (void)m_scene->ConsumeRenderGraphDirty();

    // Media textures can change while a build is in flight; the drain above
    // just ate those dirty events even though the build may have read the
    // older material state. Re-apply the last media status: if the built
    // graph already matches, this is a no-op, otherwise it re-marks the
    // materials and refreshes (bounded: the follow-up rebuild re-applies
    // again and finds nothing changed).
    if (m_has_media) {
        auto changed = SceneUserPropertyApplier::ApplyTexture(
            *m_scene, "$mediaThumbnail", RuntimeTextureProperty(m_last_art_url));
        auto changed_prev = SceneUserPropertyApplier::ApplyTexture(
            *m_scene, "$mediaPreviousThumbnail", RuntimeTextureProperty(m_last_prev_art_url));
        if (! changed.is_empty() || ! changed_prev.is_empty()) {
            refreshPreparedMaterialDirtyEvents();
        }
    }
}

void SceneRenderController::consumeDirtyEventsCoveredByGraphRebuild() {
    if (! m_scene) return;
    (void)m_scene->ConsumePreparedMaterialDirtyEvents();
    (void)m_scene->ConsumePreparedMeshDirtyEvents();
    (void)m_scene->ConsumePreparedRenderTargetDirtyEvents();
}

void SceneRenderController::refreshPreparedRenderTargetDirtyEvents() {
    if (! m_scene || ! renderInited() || m_rg.is_none()) return;
    auto events = m_scene->ConsumePreparedRenderTargetDirtyEvents();
    if (events.is_empty()) return;

    m_render->refreshPreparedTextures(*m_scene, m_render_scene);
}

void SceneRenderController::refreshPreparedMeshDirtyEvents() {
    if (! m_scene || ! renderInited() || m_rg.is_none()) return;
    auto events = m_scene->ConsumePreparedMeshDirtyEvents();
    if (events.is_empty()) return;

    bool requires_graph_rebuild = events.iter().any([](auto event) {
        return (event->flags & SceneMeshDirtyLayout) != 0;
    });
    if (requires_graph_rebuild) {
        rebuildRenderGraph(vulkan::RenderGraphResourceRetention::KeepSceneTextures, false);
        return;
    }

    for (const auto& event : events) {
        if ((event.flags & SceneMeshDirtyData) == 0) continue;
        m_render->refreshPreparedMesh(
            *m_scene,
            m_render_scene,
            event.mesh,
            vulkan::ToPassInvalidationFlags(vulkan::PassInvalidation::Resources));
    }
}

void SceneRenderController::refreshPreparedMaterialDirtyEvents() {
    if (! m_scene || ! renderInited() || m_rg.is_none()) return;
    auto events = m_scene->ConsumePreparedMaterialDirtyEvents();
    if (events.is_empty()) return;

    bool requires_graph_rebuild = events.iter().any([](auto event) {
        return (event->flags & SceneMaterialDirtyGraph) != 0;
    });
    if (requires_graph_rebuild) {
        rebuildRenderGraph(vulkan::RenderGraphResourceRetention::KeepSceneTextures, false);
        return;
    }

    Vec<SceneMaterialId> texture_materials;
    for (const auto& event : events) {
        if ((event.flags & SceneMaterialDirtyTextureBindings) != 0) {
            PushUniqueMaterialId(texture_materials, event.material);
        }
    }
    if (! texture_materials.is_empty() &&
        ! m_render->refreshPreparedMaterialTextures(
            *m_scene, m_render_scene, texture_materials.as_slice())) {
        rebuildRenderGraph(vulkan::RenderGraphResourceRetention::KeepSceneTextures, false);
        return;
    }
    for (const auto& event : events) {
        auto flags = MaterialDirtyToPassInvalidationFlags(event.flags);
        if (flags == vulkan::PassInvalidationNone) continue;
        m_render->refreshPreparedMaterial(*m_scene, m_render_scene, event.material, flags);
    }
}

void SceneRenderController::on(RenderMsg::SetScene_payload&& m) {
    m_load_bench          = rstd::move(m.load_bench);
    m_load_bench_recorder = None();
    if (m_load_bench) {
        rstd::bench::probe::RecorderConfig config;
        config.sample_capacity = usize(32768);
        m_load_bench_recorder  = Some(BenchContext(m_load_bench).session().recorder(config));
    }
    auto load_bench = loadBenchView();
    auto load_span  = SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_load);
    if (m.random_seed.is_some()) {
        using Seed = decltype(Random::max());
        Random::seed(static_cast<Seed>(m.random_seed->to_primitive()));
    }
    detachSceneAudioResponseDemandCallback();
    m_scene               = nullptr;
    m_uniform_input       = nullptr;
    m_scene_owner         = Some(rstd::move(m.scene));
    m_uniform_input_owner = Some(rstd::move(m.uniform_input));
    m_scene               = m_scene_owner->get();
    m_uniform_input       = m_uniform_input_owner->as_ptr().as_raw_ptr();
    m_scene_audio_response.end();
    m_first_frame_ok = false;
    if (m_scene) {
        m_scene->AudioDemandMut()->SetEnabled(m_audio_response_enabled);
        m_scene->AudioDemandMut()->SetCallback(
            CloneAudioResponseDemandCallback(m_audio_response_demand_callback));
    }
    rebuildRenderGraph(
        vulkan::RenderGraphResourceRetention::ReleaseSceneTextures, true, load_bench);
}

void SceneRenderController::on(RenderMsg::SetSpeed_payload&& m) { m_speed = m.speed; }

void SceneRenderController::on(RenderMsg::SetUserProperty_payload&& m) {
    if (! m_scene) return;
    auto load_bench    = loadBenchView();
    auto property_span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_user_property);

    SceneUserPropertyMutation mutation;
    {
        auto apply_span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_user_property_apply);
        mutation        = SceneUserPropertyApplier::Apply(*m_scene, m.key, m.property);
    }

    if (mutation.diagnostics_changed && m_main_tx) {
        auto diagnostics = CollectSceneUserPropertyDiagnostics(*m_scene, m.key);
        (void)m_main_tx->send(MainMsg::UserPropertyDiagnostics(rstd::move(diagnostics)));
    }
    if (mutation.clear_color.is_some() && m_main_tx) {
        const auto color = *mutation.clear_color;
        (void)m_main_tx->send(MainMsg::SceneClearColorChanged(
            f32(color[usize()]), f32(color[usize(1)]), f32(color[usize(2)])));
    }
    if (mutation.graph_changed) {
        auto rebuild_span =
            SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_user_property_graph_rebuild);
        rebuildRenderGraph(
            vulkan::RenderGraphResourceRetention::KeepSceneTextures, false, load_bench);
        return;
    }
    if (renderInited() && m_rg.is_some()) {
        auto refresh_span =
            SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_user_property_refresh);
        refreshPreparedMaterialDirtyEvents();
    }
}

void SceneRenderController::on(RenderMsg::SetMediaStatus_payload&& m) {
    if (! m_scene) return;

    owe::script::SetSceneMediaStatus(*m_scene, ToScriptMediaStatus(m.status));

    m_last_art_url      = m.status.art_url;
    m_last_prev_art_url = m.status.previous_art_url;
    m_has_media         = true;
    (void)SceneUserPropertyApplier::ApplyTexture(
        *m_scene, "$mediaThumbnail", RuntimeTextureProperty(m.status.art_url));
    (void)SceneUserPropertyApplier::ApplyTexture(
        *m_scene, "$mediaPreviousThumbnail", RuntimeTextureProperty(m.status.previous_art_url));
    if (renderInited() && m_rg.is_some()) refreshPreparedMaterialDirtyEvents();
}

void SceneRenderController::on(RenderMsg::SetAudioResponseDemandCallback_payload&& m) {
    m_audio_response_demand_callback = Some(rstd::move(m.callback));
    if (m_scene) {
        m_scene->AudioDemandMut()->SetCallback(
            CloneAudioResponseDemandCallback(m_audio_response_demand_callback));
    }
}

void SceneRenderController::on(RenderMsg::SetAudioResponseEnabled_payload&& m) {
    m_audio_response_enabled = m.enabled;
    if (m_scene) m_scene->AudioDemandMut()->SetEnabled(m.enabled);
    if (! m.enabled) {
        m_audio_response_engine.end();
        m_scene_audio_response.end();
    }
}

void SceneRenderController::on(RenderMsg::SetAudioPcmWindow_payload&& m) {
    audio::ResponseFrame response {};
    if (! m_audio_response_engine.analyze(m.window, response)) return;
    m_scene_audio_response.submit(rstd::move(response));
}

void SceneRenderController::on(RenderMsg::Init_payload&& m) {
    auto                                      context = rstd::move(m.load_bench);
    Option<rstd::bench::probe::ProbeRecorder> recorder;
    if (context) recorder = Some(BenchContext(context).session().recorder());
    auto load_bench = SceneLoadBenchRecorderView {
        .recorder = recorder ? &*recorder : nullptr,
        .ids      = context ? &BenchContext(context).ids() : nullptr,
    };
    bool initialized = false;
    {
        auto init_span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::vulkan_init);
        initialized    = m_render->init(rstd::move(*m.info), load_bench);
    }
    if (recorder && m_main_tx) {
        auto batch = recorder->drain();
        if (batch.is_ok()) {
            (void)m_main_tx->send(
                MainMsg::LoadBenchBatch(context.clone(), rstd::move(batch).unwrap_unchecked()));
        } else {
            rstd_warn("vulkan init probe drain failed");
        }
    }

    if (! initialized) {
        if (context && m_main_tx) {
            (void)m_main_tx->send(MainMsg::LoadBenchFinish(context.clone()));
        }
        return;
    }

    // Subscribe to ExSwapchain ready/extent/format changes. The
    // callback runs on the render thread (sync for Local, from
    // drainPendingDirective for Bridge); we just relay it as a
    // RenderSwapchainReady message back to ourselves so the actual
    // handling happens through the normal loop path. Format reaches
    // VulkanRender via ExSwapchain::format() directly; no need to
    // round-trip it through this message.
    if (auto* sw = m_render->exSwapchain()) {
        if (m_tx) {
            m_swapchain_tx                   = std::make_shared<RenderSender>(*m_tx);
            std::weak_ptr<RenderSender> weak = m_swapchain_tx;
            sw->setOnReadyChanged([weak](const ExSwapchainReadyEvent& e) {
                if (auto tx = weak.lock()) {
                    (void)tx->send(RenderMsg::SwapchainReady(e.ready, u32(e.width), u32(e.height)));
                }
            });
        }
    }

    // inited, callback to load scene
    if (m_main_tx) {
        (void)m_main_tx->send(MainMsg::LoadScene(m_render->deviceCapabilities()));
    }
}

void SceneRenderController::on(RenderMsg::SwapchainReady_payload&& m) {
    if (! m.ready) {
        frame_timer.Stop();
        return;
    }
    bool extent_changed =
        m_render->onSwapchainReady(m.width.to_primitive(), m.height.to_primitive());
    if (extent_changed && m_scene && m_rg.is_some()) {
        rebuildRenderGraph(vulkan::RenderGraphResourceRetention::KeepSceneTextures, false);
    }
    if (m_stopped)
        frame_timer.Stop();
    else
        frame_timer.Run();
}

void SceneRenderController::on(RenderMsg::RequestPreparedPassDiagnostics_payload&& m) {
    if (! m_main_tx) return;
    auto diagnostics = m_render->preparedPassDiagnostics();
    (void)m_main_tx->send(
        MainMsg::PreparedPassDiagnostics(rstd::move(m.cb), rstd::move(diagnostics)));
}

auto SceneRuntimeController::sender() const -> MainSender {
    if (! m_main_tx) rstd::panic { "main mailbox is stopped" };
    return *m_main_tx;
}

void SceneRuntimeController::post(MainMsg msg) {
    if (m_main_tx) (void)m_main_tx->send(rstd::move(msg));
}

void SceneRuntimeController::post(RenderMsg msg) { m_render_controller->post(rstd::move(msg)); }

auto SceneRuntimeController::loadBenchView() -> SceneLoadBenchRecorderView {
    return {
        .recorder = m_load_bench_recorder ? &*m_load_bench_recorder : nullptr,
        .ids      = m_load_bench ? &BenchContext(m_load_bench).ids() : nullptr,
    };
}

auto SceneRuntimeController::schemeColor() const -> Option<array<float, 3>> {
    auto property = m_user_properties.get("schemecolor"_str);
    return property.is_some() ? ResolveSceneUserPropertyColor(**property) : None();
}

void SceneRuntimeController::publishClearColor(array<float, 3> fallback) {
    if (! m_clear_color_cb) return;
    auto color = schemeColor();
    auto value = color.is_some() ? *color : fallback;
    m_clear_color_cb(value[usize()], value[usize(1)], value[usize(2)]);
}

void SceneRuntimeController::ensureLoadBench(const Option<SceneLoadBenchHandle>& context) {
    if (! context) {
        finishLoadBench();
        return;
    }
    if (m_load_bench && BenchContext(m_load_bench).run_id() == BenchContext(context).run_id())
        return;
    if (BenchContext(context).run_id() <= m_latest_load_bench_run_id) return;

    finishLoadBench();
    m_latest_load_bench_run_id = BenchContext(context).run_id();
    m_load_bench               = context.clone();
    m_load_bench_collector =
        Some(rstd::bench::probe::ProbeCollector::new_(BenchContext(context).schema_owner()));
    auto batches = BenchContext(m_load_bench).take_preload_batches();
    for (usize index; index < batches.len(); ++index) {
        auto ingested = m_load_bench_collector->ingest(batches[index]);
        if (ingested.is_err()) rstd_warn("preload probe batch schema mismatch");
    }
}

void SceneRuntimeController::ingestLoadBenchBatch(const Option<SceneLoadBenchHandle>&   context,
                                                  const rstd::bench::probe::ProbeBatch& batch) {
    if (! context) return;
    if (! m_load_bench) ensureLoadBench(context);
    if (! m_load_bench || ! context ||
        BenchContext(m_load_bench).run_id() != BenchContext(context).run_id() ||
        ! m_load_bench_collector)
        return;
    auto ingested = m_load_bench_collector->ingest(batch);
    if (ingested.is_err()) rstd_warn("scene load probe batch schema mismatch");
}

void SceneRuntimeController::finishLoadBench() {
    if (! m_load_bench) return;

    if (m_load_total_span) {
        (void)m_load_total_span->finish();
        m_load_total_span = None();
    }
    if (m_load_bench_recorder) {
        auto batch = m_load_bench_recorder->drain();
        if (batch.is_ok() && m_load_bench_collector) {
            auto ingested = m_load_bench_collector->ingest(*batch);
            if (ingested.is_err()) rstd_warn("main probe batch schema mismatch");
        } else if (batch.is_err()) {
            rstd_warn("main probe drain failed");
        }
        m_load_bench_recorder = None();
    }

    if (m_load_bench_collector) {
        auto report = rstd::move(*m_load_bench_collector).finish();
        auto file   = rstd::fs::File::create(BenchContext(m_load_bench).output_path());
        if (file.is_err()) {
            auto error = rstd::move(file).unwrap_err_unchecked();
            rstd_warn("cannot create scene load probe report {}: {}",
                      BenchContext(m_load_bench).output_path(),
                      error);
        } else {
            auto output  = rstd::move(file).unwrap_unchecked();
            auto written = rstd::bench::probe::write_text(output, report);
            if (written.is_err()) {
                auto error = rstd::move(written).unwrap_err_unchecked();
                rstd_warn("cannot write scene load probe report {}: {}",
                          BenchContext(m_load_bench).output_path(),
                          error);
            }
        }
        m_load_bench_collector = None();
    }
    m_load_bench = None();
}

void SceneRuntimeController::startMainLoop() {
    if (m_main_thread) return;
    if (! m_main_rx) rstd::panic { "main mailbox cannot be restarted" };

    auto rx       = rstd::move(m_main_rx.take()).unwrap_unchecked();
    auto thread   = rstd::thread::spawn([this, rx = rstd::move(rx)]() mutable {
        rstd_info("main loop started");
        while (true) {
            auto received = rx.recv();
            if (received.is_err()) break;

            auto message  = rstd::move(received).unwrap();
            bool shutdown = false;
            RSTD_MATCH(rstd::move(message)) {
                RSTD_CASE_PAYLOAD(LoadScene, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(Configure, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetFps, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetVolume, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetVolumeScale, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetMuted, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetAudioClientIdentity, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(AudioDeviceEvent, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetFillMode, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetSpeed, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetUserProperty, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetFirstFrameCallback, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetUserPropertyDiagnosticCallback, value) {
                    on(rstd::move(value));
                }
                RSTD_CASE_PAYLOAD(UserPropertyDiagnostics, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SceneClearColorChanged, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(PreparedPassDiagnostics, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(Stop, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(PauseAudio, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(LoadBenchBatch, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(LoadBenchFinish, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(FirstFrame, value) { on(rstd::move(value)); }
                RSTD_CASE(Shutdown) {
                    m_sound_manager->shutdown();
                    finishLoadBench();
                    shutdown = true;
                }
            }
            if (shutdown) break;
        }
        rstd_info("main loop stopped");
    });
    m_main_thread = Some(rstd::move(thread).unwrap());
}

void SceneRuntimeController::stopMainLoop() {
    if (! m_main_thread) {
        m_main_tx = None();
        m_main_rx = None();
        return;
    }
    if (rstd::thread::current_id() == m_main_thread->thread().id()) {
        rstd::panic { "SceneRuntimeController destroyed from main thread" };
    }

    post(MainMsg::Shutdown());
    auto thread = rstd::move(m_main_thread.take()).unwrap_unchecked();
    rstd::move(thread).join().unwrap();
    m_main_tx = None();
}

// ---- SceneRuntimeController message handlers --------------------------------

void SceneRuntimeController::onLoadScene() {
    if (m_render_capabilities.is_some() && m_render_controller->renderInited()) {
        if (! m_audio_activated) {
            auto tx = sender();
            m_sound_manager->activate(
                [tx = rstd::move(tx)](wavsen::audio::AudioDeviceEvent event) mutable {
                    (void)tx.send(MainMsg::AudioDeviceEvent(rstd::move(event)));
                });
            m_audio_activated = true;
        }
        loadScene();
    }
}

void SceneRuntimeController::on(MainMsg::LoadScene_payload&& m) {
    m_render_capabilities = Some(m.capabilities);
    onLoadScene();
}

void SceneRuntimeController::on(MainMsg::Configure_payload&& m) {
    m_config = rstd::move(m.config);
    ensureLoadBench(m_config.load_bench);
    m_user_properties = NormalizeUserProperties(m_config.user_properties);
    on(MainMsg::SetFps_payload { u32(m_config.fps) });
    on(MainMsg::SetVolume_payload { f32(m_config.volume) });
    on(MainMsg::SetVolumeScale_payload { f32(m_config.volume_scale) });
    on(MainMsg::SetMuted_payload { m_config.muted });
    on(MainMsg::SetFillMode_payload { m_config.fill_mode });
    on(MainMsg::SetSpeed_payload { f32(m_config.speed) });
    onLoadScene();
}

void SceneRuntimeController::on(MainMsg::SetFps_payload&& m) {
    m_config.fps = m.fps.to_primitive();
    if (m.fps >= u32(5)) {
        m_render_controller->frame_timer.SetRequiredFps(u16(m.fps.to_primitive()));
    }
}

void SceneRuntimeController::on(MainMsg::SetVolume_payload&& m) {
    m_config.volume = m.volume.to_primitive();
    m_sound_manager->set_volume(m.volume);
}

void SceneRuntimeController::on(MainMsg::SetVolumeScale_payload&& m) {
    m_sound_manager->set_volume_scale(m.scale, m.fade_ms);
}

void SceneRuntimeController::on(MainMsg::SetMuted_payload&& m) {
    m_config.muted = m.muted;
    m_sound_manager->set_muted(m.muted);
}

void SceneRuntimeController::on(MainMsg::SetAudioClientIdentity_payload&& m) {
    auto identity = wavsen::audio::AudioClientIdentity {
        .application_name =
            String::make(rstd::cppstd::as_str(m.identity.application_name).unwrap()),
        .application_id = String::make(rstd::cppstd::as_str(m.identity.application_id).unwrap()),
        .stream_prefix  = String::make(rstd::cppstd::as_str(m.identity.stream_prefix).unwrap()),
        .component      = String::make(rstd::cppstd::as_str(m.identity.component).unwrap()),
        .media_name     = String::make(rstd::cppstd::as_str(m.identity.media_name).unwrap()),
        .media_role     = String::make(rstd::cppstd::as_str(m.identity.media_role).unwrap()),
    };
    if (! m_sound_manager->set_identity(rstd::move(identity))) {
        rstd_warn("audio identity cannot change after audio shutdown");
    }
}

void SceneRuntimeController::on(MainMsg::AudioDeviceEvent_payload&& m) {
    m_sound_manager->on_device_event(rstd::move(m.event));
}

void SceneRuntimeController::on(MainMsg::SetFillMode_payload&& m) {
    m_config.fill_mode = m.mode;
    m_render_controller->post(RenderMsg::SetFillMode(m.mode));
}

void SceneRuntimeController::on(MainMsg::SetSpeed_payload&& m) {
    if (! m.speed.is_finite() || m.speed <= f32()) {
        rstd_warn("SceneWallpaper: invalid playback speed {}; ignoring", m.speed);
        return;
    }
    m_config.speed = m.speed.to_primitive();
    m_render_controller->post(RenderMsg::SetSpeed(m.speed));
}

void SceneRuntimeController::on(MainMsg::SetUserProperty_payload&& m) {
    const std::string property = CanonicalSceneUserPropertyKey(m.key);
    auto              current  = m_user_properties.get(rstd::cppstd::as_str(property).unwrap());
    Json              prop = current.is_some() ? MergeUserPropertyDescriptor(**current, m.value)
                                               : MakeUserPropertyDescriptor(rstd::move(m.value));
    m_config.user_properties.insert(
        ::alloc::string::String::make(rstd::cppstd::as_str(property).unwrap()), prop.clone());
    m_user_properties.insert(::alloc::string::String::make(rstd::cppstd::as_str(property).unwrap()),
                             prop.clone());
    if (property == "schemecolor") {
        auto color = ResolveSceneUserPropertyColor(prop);
        if (color.is_some() && m_clear_color_cb) {
            const auto value = *color;
            m_clear_color_cb(value[usize()], value[usize(1)], value[usize(2)]);
        }
    }
    m_render_controller->post(RenderMsg::SetUserProperty(property, rstd::move(prop)));
}

void SceneRuntimeController::on(MainMsg::SetFirstFrameCallback_payload&& m) {
    m_first_frame_callback = rstd::move(m.cb);
}

void SceneRuntimeController::on(MainMsg::SetUserPropertyDiagnosticCallback_payload&& m) {
    m_user_property_diagnostic_cb = rstd::move(m.cb);
}

void SceneRuntimeController::on(MainMsg::UserPropertyDiagnostics_payload&& m) {
    if (m_user_property_diagnostic_cb) m_user_property_diagnostic_cb(rstd::move(m.diagnostics));
}

void SceneRuntimeController::on(MainMsg::SceneClearColorChanged_payload&& m) {
    if (schemeColor().is_none() && m_clear_color_cb) {
        m_clear_color_cb(m.r.to_primitive(), m.g.to_primitive(), m.b.to_primitive());
    }
}

void SceneRuntimeController::on(MainMsg::PreparedPassDiagnostics_payload&& m) {
    if (m.cb) m.cb(rstd::move(m.diagnostics));
}

void SceneRuntimeController::on(MainMsg::Stop_payload&& m) {
    const u64 generation = ++m_audio_pause_generation;
    if (m.stop) {
        if (m.scale_audio) m_sound_manager->set_volume_scale(f32(), m.fade_ms);
        if (m.fade_ms == u32() || ! m.scale_audio) {
            m_sound_manager->pause();
        } else {
            auto tx    = sender();
            auto delay = m.fade_ms.to_primitive();
            std::thread([tx = rstd::move(tx), generation, delay]() mutable {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                (void)tx.send(MainMsg::PauseAudio(u64(generation)));
            }).detach();
        }
    } else {
        m_sound_manager->play();
        if (m.scale_audio) m_sound_manager->set_volume_scale(f32(1.0f), m.fade_ms);
    }
    m_render_controller->post(RenderMsg::Stop(m.stop));
}

void SceneRuntimeController::on(MainMsg::PauseAudio_payload&& m) {
    if (m.generation == m_audio_pause_generation) m_sound_manager->pause();
}

void SceneRuntimeController::on(MainMsg::LoadBenchBatch_payload&& m) {
    ingestLoadBenchBatch(m.context, m.batch);
}

void SceneRuntimeController::on(MainMsg::LoadBenchFinish_payload&& m) {
    if (m_load_bench && m.context &&
        BenchContext(m_load_bench).run_id() == BenchContext(m.context).run_id()) {
        finishLoadBench();
    }
}

void SceneRuntimeController::on(MainMsg::FirstFrame_payload&& m) {
    if (m.batch) ingestLoadBenchBatch(m.context, *m.batch);
    if (m_load_bench && m.context &&
        BenchContext(m_load_bench).run_id() == BenchContext(m.context).run_id()) {
        finishLoadBench();
    }
    if (m_first_frame_callback) m_first_frame_callback();
}

void SceneRuntimeController::loadScene() {
    ensureLoadBench(m_config.load_bench);
    if (m_config.source_pkg_path.empty() || m_config.assets_dir.empty()) {
        finishLoadBench();
        return;
    }
    if (m_load_bench && ! m_load_bench_recorder) {
        rstd::bench::probe::RecorderConfig config;
        config.sample_capacity = usize(32768);
        m_load_bench_recorder  = Some(BenchContext(m_load_bench).session().recorder(config));
        m_load_total_span = Some(SceneLoadSpan(loadBenchView(), &SceneLoadProbeIds::load_total));
    }
    auto abort_load = [this] {
        finishLoadBench();
    };

    if (m_config.random_seed.is_some()) {
        using Seed = decltype(Random::max());
        Random::seed(static_cast<Seed>(m_config.random_seed->to_primitive()));
    }

    rstd_info("loading scene: {}", m_config.source_pkg_path);

    {
        auto span = SceneLoadSpan(loadBenchView(), &SceneLoadProbeIds::load_audio);
        m_sound_manager->unmount_all();
    }

    Option<ParsedScene> parsed_scene;

    // mount assets dir
    Box<fs::VFS> pVfs = Box<fs::VFS>::make();
    auto&        vfs  = *pVfs;
    {
        auto span = SceneLoadSpan(loadBenchView(), &SceneLoadProbeIds::load_vfs_assets);
        if (! vfs.is_mounted("assets"_str)) {
            auto assets = fs::make_physical_fs(fs::ToPath(m_config.assets_dir));
            if (assets.is_err() ||
                vfs.mount("/assets"_str, rstd::move(assets).unwrap_unchecked(), "assets"_str)
                    .is_err()) {
                rstd_error("Mount assets dir failed");
                abort_load();
                return;
            }
        }
    }
    std::filesystem::path pkgPath_fs { m_config.source_pkg_path };
    pkgPath_fs.replace_extension("pkg");
    std::string pkgPath  = pkgPath_fs.native();
    std::string pkgEntry = pkgPath_fs.filename().replace_extension("json").native();
    std::string pkgDir   = pkgPath_fs.parent_path().native();
    std::string scene_id = pkgPath_fs.parent_path().filename().native();
    {
        auto span = SceneLoadSpan(loadBenchView(), &SceneLoadProbeIds::load_project_properties);
        MergeProjectUserProperties(pkgPath_fs.parent_path(), m_user_properties);
    }

    // load pkgfile. Read pkg version stamp before move-mounting so we can
    // pass it to the scene parser; on fallback (loose dir) we have no
    // version info and use kSceneVersionUnknown.
    wpscene::SceneVersion pkg_v = wpscene::kSceneVersionUnknown;
    {
        auto span        = SceneLoadSpan(loadBenchView(), &SceneLoadProbeIds::load_package);
        auto wfs         = fs::WPPkgFs::open(fs::ToPath(pkgPath));
        bool pkg_mounted = false;
        if (wfs.is_ok()) {
            auto stamp  = wfs->pkg_version_stamp();
            pkg_v       = wpscene::ParsePkgVersionStamp(std::string_view(
                reinterpret_cast<const char*>(stamp.data()), stamp.size().to_primitive()));
            pkg_mounted = vfs.mount("/assets"_str, wfs->mount_handle()).is_ok();
        }
        if (! pkg_mounted) {
            rstd_info("load pkg file {} failed, fallback to use dir", pkgPath);
            pkg_v      = wpscene::kSceneVersionUnknown;
            auto loose = fs::make_physical_fs(fs::ToPath(pkgDir));
            if (loose.is_err() ||
                vfs.mount("/assets"_str, rstd::move(loose).unwrap_unchecked()).is_err()) {
                rstd_error("can't load pkg directory: {}", pkgDir);
                abort_load();
                return;
            }
        }
    }
    {
        const std::string base { "/assets/" };
        auto              scene_doc = m_config.scene_document;
        if (! scene_doc) {
            auto span   = SceneLoadSpan(loadBenchView(), &SceneLoadProbeIds::load_scene_document);
            auto loaded = wpscene::LoadSceneDocumentFromVfs(vfs, base + pkgEntry, pkg_v);
            if (loaded) scene_doc = std::make_shared<wpscene::SceneDocument>(rstd::move(*loaded));
        }
        if (! scene_doc) {
            rstd_error("Not supported scene type");
            abort_load();
            return;
        }
        Option<rstd::path::PathBuf> shader_cache_dir;
        if (! m_config.cache_dir.empty()) {
            shader_cache_dir =
                Some(rstd::path::PathBuf::from(rstd::cppstd::as_str(m_config.cache_dir).unwrap()));
            rstd_info("shader cache folder: {}", m_config.cache_dir);
        }
        SceneParser parser;
        auto        parsed = parser.Parse(
            rstd::cppstd::as_str(scene_id).unwrap(),
            rstd::ref<wpscene::SceneDocument>::from_raw_parts(scene_doc.get()),
            rstd::mut_ref<fs::VFS>::from_raw_parts(&vfs),
            rstd::mut_ref<wavsen::audio::SoundManager>::from_raw_parts(m_sound_manager.get()),
            SceneParseOptions {
                .load_bench      = loadBenchView(),
                .user_properties = Some(
                    rstd::ref<rstd::json::Map>::from_raw_parts(rstd::addressof(m_user_properties))),
                .shader_cache_dir = rstd::move(shader_cache_dir),
                .capabilities =
                    SceneParseCapabilities {
                        .directional_shadow = m_render_capabilities->directional_shadow(),
                        .max_geometry_output_vertices =
                            u32(m_render_capabilities->max_geometry_output_vertices),
                        .max_geometry_total_output_components =
                            u32(m_render_capabilities->max_geometry_total_output_components),
                    },
            });
        if (parsed.is_err()) {
            rstd_error("scene parse failed: {}", parsed.unwrap_err().message.as_str());
            abort_load();
            return;
        }
        parsed_scene       = Some(rstd::move(parsed).unwrap());
        auto& scene        = parsed_scene->scene;
        auto  runtime_span = SceneLoadSpan(loadBenchView(), &SceneLoadProbeIds::load_runtime_setup);
        scene->InstallExtension(rstd::move(pVfs));
        SceneUserPropertyMutation initial_mutation;
        {
            auto property_span =
                SceneLoadSpan(loadBenchView(), &SceneLoadProbeIds::load_initial_properties);
            initial_mutation = SceneUserPropertyApplier::ApplyAll(*scene, m_user_properties);
        }
        if (initial_mutation.diagnostics_changed && m_user_property_diagnostic_cb) {
            m_user_property_diagnostic_cb(
                CloneUserPropertyDiagnostics(scene->UserPropertyDiagnostics()));
        }
        if (! m_config.cache_dir.empty()) {
            std::filesystem::path ls_dir =
                std::filesystem::path(m_config.cache_dir) / "script_localstorage";
            std::error_code ec;
            std::filesystem::create_directories(ls_dir, ec);
            std::string ls_file = (ls_dir / (scene_id + ".json")).native();
            owe::script::SetScenePersistence(*scene, rstd::move(ls_file));
        }

        publishClearColor(scene->ClearColor());
    }

    auto parsed = rstd::move(parsed_scene).unwrap();
    auto rtx    = m_render_controller->sender();
    if (rtx.send(RenderMsg::SetScene(rstd::move(parsed.scene),
                                     rstd::move(parsed.runtime_input),
                                     m_config.load_bench.clone(),
                                     m_config.random_seed))
            .is_err()) {
        abort_load();
        return;
    }
    if (rtx.send(RenderMsg::Draw()).is_err()) abort_load();
}

bool SceneRuntimeController::init() {
    if (m_inited) return true;

    // Wire render handler senders before starting the loops; otherwise an
    // early RenderInit could fire before they're set.
    m_render_controller->setMainSender(sender());

    startMainLoop();
    m_render_controller->start();

    {
        auto& frameTimer = m_render_controller->frame_timer;
        auto  rtx        = m_render_controller->sender();
        frameTimer.SetCallback([rtx]() mutable {
            (void)rtx.send(RenderMsg::Draw());
        });
        frameTimer.SetRequiredFps(u16(15));
        frameTimer.Run();
    }

    m_inited = true;
    return true;
}

SceneRuntimeController::SceneRuntimeController()
    : m_sound_manager(Box<wavsen::audio::SoundManager>::make()),
      m_render_controller(Box<SceneRenderController>::make(*this)) {
    auto [tx, rx] = rstd::sync::mpmc::channel<MainMsg>();
    m_main_tx     = Some(rstd::move(tx));
    m_main_rx     = Some(rstd::move(rx));
}

SceneRuntimeController::~SceneRuntimeController() {
    // Stop main before render so no main handler can enqueue more render work.
    m_render_controller->frame_timer.Stop();
    stopMainLoop();
    m_render_controller->stop();
}

} // namespace owe

SceneWallpaper::SceneWallpaper(): m_runtime(std::make_unique<SceneRuntimeController>()) {}

SceneWallpaper::~SceneWallpaper() = default;

bool SceneWallpaper::inited() const { return m_runtime->inited(); }

bool SceneWallpaper::init() { return m_runtime->init(); }

void SceneWallpaper::initVulkan(RenderInitInfo info) {
    m_offscreen = info.offscreen;
    auto boxed  = Box<RenderInitInfo>::make(rstd::move(info));
    m_runtime->post(RenderMsg::Init(rstd::move(boxed), m_load_bench.clone()));
}

void SceneWallpaper::play() { m_runtime->post(MainMsg::Stop(false)); }
void SceneWallpaper::play(uint32_t fade_ms) {
    m_runtime->post(MainMsg::Stop(false, u32(fade_ms), true));
}
void SceneWallpaper::pause() { m_runtime->post(MainMsg::Stop(true)); }
void SceneWallpaper::pause(uint32_t fade_ms) {
    m_runtime->post(MainMsg::Stop(true, u32(fade_ms), true));
}
void SceneWallpaper::requestFrame() { m_runtime->post(RenderMsg::Draw()); }

void SceneWallpaper::mouseInput(double x, double y) {
    m_runtime->renderController()->setMousePos(x, y);
}

void SceneWallpaper::mouseButton(int button, bool down) {
    m_runtime->renderController()->setMouseButton(button, down);
}

void SceneWallpaper::mouseEnter(bool in_window) {
    m_runtime->renderController()->setMouseInWindow(in_window);
}

void SceneWallpaper::configure(SceneWallpaperConfig config) {
    m_load_bench = config.load_bench.clone();
    m_runtime->post(MainMsg::Configure(rstd::move(config)));
}

void SceneWallpaper::setFps(uint32_t fps) { m_runtime->post(MainMsg::SetFps(u32(fps))); }

void SceneWallpaper::setVolume(float volume) { m_runtime->post(MainMsg::SetVolume(f32(volume))); }

void SceneWallpaper::setVolumeScale(float scale) { setVolumeScale(scale, 0); }

void SceneWallpaper::setVolumeScale(float scale, uint32_t fade_ms) {
    m_runtime->post(MainMsg::SetVolumeScale(f32(scale), u32(fade_ms)));
}

void SceneWallpaper::setMuted(bool muted) { m_runtime->post(MainMsg::SetMuted(muted)); }

void SceneWallpaper::setFillMode(FillMode mode) { m_runtime->post(MainMsg::SetFillMode(mode)); }

void SceneWallpaper::setSpeed(float speed) { m_runtime->post(MainMsg::SetSpeed(f32(speed))); }

void SceneWallpaper::setMediaStatus(MediaStatus status) {
    m_runtime->post(RenderMsg::SetMediaStatus(rstd::move(status)));
}

void SceneWallpaper::setAudioClientIdentity(SceneAudioClientIdentity identity) {
    m_runtime->post(MainMsg::SetAudioClientIdentity(rstd::move(identity)));
}

void SceneWallpaper::setAudioResponseDemandCallback(AudioResponseDemandCallback callback) {
    m_runtime->post(RenderMsg::SetAudioResponseDemandCallback(rstd::move(callback)));
}

void SceneWallpaper::setAudioResponseEnabled(bool enabled) {
    m_runtime->post(RenderMsg::SetAudioResponseEnabled(enabled));
}

void SceneWallpaper::setAudioPcmWindow(audio::PcmWindow window) {
    m_runtime->post(RenderMsg::SetAudioPcmWindow(rstd::move(window)));
}

void SceneWallpaper::endAudioResponse() { m_runtime->post(RenderMsg::EndAudioResponse()); }

void SceneWallpaper::setUserPropertyRaw(std::string_view name, std::string value) {
    m_runtime->post(MainMsg::SetUserProperty(std::string(name), RawUserProperty(value)));
}

void SceneWallpaper::setUserPropertyJson(std::string_view name, Json value) {
    m_runtime->post(MainMsg::SetUserProperty(std::string(name), rstd::move(value)));
}

void SceneWallpaper::setOnClearColor(ClearColorCallback cb) {
    m_runtime->setOnClearColor(rstd::move(cb));
}

void SceneWallpaper::setOnFirstFrame(FirstFrameCallback cb) {
    m_runtime->post(MainMsg::SetFirstFrameCallback(rstd::move(cb)));
}

void SceneWallpaper::setOnUserPropertyDiagnostics(UserPropertyDiagnosticCallback cb) {
    m_runtime->post(MainMsg::SetUserPropertyDiagnosticCallback(rstd::move(cb)));
}

void SceneWallpaper::requestPreparedPassDiagnostics(RenderPassDiagnosticCallback cb) {
    m_runtime->post(RenderMsg::RequestPreparedPassDiagnostics(rstd::move(cb)));
}

int SceneWallpaper::takeLastFrameSyncFd() {
    return m_runtime->renderController()->takeLastFrameSyncFd();
}

ExSwapchain* SceneWallpaper::exSwapchain() const {
    return m_runtime->renderController()->exSwapchain();
}

bool SceneWallpaper::getDrmRenderNode(uint32_t& out_major, uint32_t& out_minor) const {
    return m_runtime->renderController()->getDrmRenderNode(out_major, out_minor);
}

bool SceneWallpaper::waitVulkanInited(uint32_t timeout_ms) {
    auto deadline = rstd::time::Instant::now() + rstd::time::Duration::from_millis(u64(timeout_ms));
    auto rh       = m_runtime->renderController();
    while (rstd::time::Instant::now() < deadline) {
        if (rh->renderInited()) return true;
        rstd::thread::sleep(rstd::time::Duration::from_millis(u64(2)));
    }
    return rh->renderInited();
}

VkInstance SceneWallpaper::vkInstance() const {
    return m_runtime->renderController()->render()->vkInstance();
}
VkPhysicalDevice SceneWallpaper::vkPhysicalDevice() const {
    return m_runtime->renderController()->render()->vkPhysicalDevice();
}
VkDevice SceneWallpaper::vkDevice() const {
    return m_runtime->renderController()->render()->vkDevice();
}
VkQueue SceneWallpaper::vkGraphicsQueue() const {
    return m_runtime->renderController()->render()->vkGraphicsQueue();
}
uint32_t SceneWallpaper::vkGraphicsQueueFamily() const {
    return m_runtime->renderController()->render()->vkGraphicsQueueFamily();
}
void SceneWallpaper::deviceUuid(uint8_t out[16]) const {
    m_runtime->renderController()->render()->deviceUuid(out);
}
void SceneWallpaper::driverUuid(uint8_t out[16]) const {
    m_runtime->renderController()->render()->driverUuid(out);
}
