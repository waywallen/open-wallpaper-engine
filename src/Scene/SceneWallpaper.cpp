module;

#include <rstd/enum.hpp>

module wescene.scene_wallpaper;
import wescene.types;
import vrento;
import wescene.utils;
import wescene.scene;

import eigen;
import owe.audio_response;
import owe.scene_audio_response;
import owe.user_property;
import rstd;
import rstd.bench;
import rstd.log;
import wavsen.audio;
import wescene.fs;
import wescene.load_bench;
import wescene.timer;
import wescene.pkg.parse;
import wescene.pkg_fs;
import wescene.scene_loader;
import wescene.resource;
import wescene.scene_user_property;
import wescene.script;
import wescene.vulkan_render;

using namespace owe;
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::ffi::OsStr;
using rstd::path::Path;
using rstd::path::PathBuf;
using rstd::sync::Arc;
using rstd::sync::atomic::Atomic;
using rstd::sync::mpmc::RecvTimeoutError;
using rstd::time::Duration;
using rstd::time::Instant;

namespace owe
{

struct SceneLoadState {
    Atomic<bool> active { true };
};

class RenderMsg final {
    RSTD_ENUM(RenderMsg,
              (Init, (Box<RenderInitInfo> info; Option<SceneLoadBenchHandle> load_bench;)),
              (BeginSceneLoad, (Arc<SceneLoadState> load;)),
              (SetScene,
               (Arc<SceneLoadState> load; Box<Scene> scene; Arc<UniformRuntimeInput> uniform_input;
                Option<SceneLoadBenchHandle> load_bench; Option<u64> random_seed;)),
              (SetFillMode, (FillMode mode;)), (SetSpeed, (f32 speed;)),
              (SetMouseParallax, (bool enabled;)), (SetUserProperty, (String key; Json property;)),
              (SetMediaStatus, (MediaStatus status;)),
              (SetAudioResponseDemandCallback, (AudioResponseDemandCallback callback;)),
              (SetAudioResponseEnabled, (bool enabled;)),
              (SetAudioPcmWindow, (audio::PcmWindow window;)), (EndAudioResponse),
              (Stop, (bool stop;)), (Draw), (SwapchainReady, (bool ready; u32 width; u32 height;)),
              (RequestPreparedPassDiagnostics, (RenderPassDiagnosticCallback cb;)),
              (RequestPreparationDiagnostics, (ScenePreparationDiagnosticCallback cb;)), (Shutdown))
};

class MainMsg final {
    RSTD_ENUM(MainMsg, (LoadScene, (vulkan::DeviceCapabilities capabilities;)),
              (Configure, (SceneWallpaperConfig config;)), (SetFps, (u32 fps;)),
              (SetVolume, (f32 volume;)), (SetVolumeScale, (f32 scale; u32 fade_ms { 0 };)),
              (SetMuted, (bool muted;)),
              (SetAudioClientIdentity, (SceneAudioClientIdentity identity;)),
              (AudioDeviceEvent, (wavsen::audio::AudioDeviceEvent event;)),
              (UnmountSound, (wavsen::audio::SoundStreamId id;)),
              (MountSound,
               (Arc<SceneLoadState> load; Box<dyn<wavsen::audio::SoundStreamObject>> stream;
                Arc<SceneSoundMountState>                                            mount;)),
              (SetFillMode, (FillMode mode;)), (SetSpeed, (f32 speed;)),
              (SetMouseParallax, (bool enabled;)), (SetUserProperty, (String key; Json value;)),
              (SetFirstFrameCallback, (FirstFrameCallback cb;)),
              (SetUserPropertyDiagnosticCallback, (UserPropertyDiagnosticCallback cb;)),
              (UserPropertyDiagnostics, (Vec<SceneUserPropertyDiagnostic> diagnostics;)),
              (SceneClearColorChanged, (f32 r; f32 g; f32 b;)),
              (PreparationDiagnostics, (ScenePreparationDiagnosticCallback  cb;
                                        Option<ScenePreparationDiagnostics> diagnostics;)),
              (PreparedPassDiagnostics,
               (RenderPassDiagnosticCallback cb; Vec<vulkan::PreparedPassDiagnostic> diagnostics;)),
              (Stop, (bool stop; u32 fade_ms { 0 }; bool scale_audio { false };)),
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

void ReportScenePreparationError(const ScenePrepareError& error) {
    rstd_error("{}", error);
    auto source = as<rstd::error::Error>(error).source();
    for (int depth = 0; source && depth < 32; ++depth) {
        rstd_error("  caused by: {}", *source);
        source = (*source)->source();
    }
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

Json RawUserProperty(ref<str> value) { return MakeUserPropertyWirePatch(value); }

Json RuntimeTextureProperty(ref<str> value) {
    auto object = rstd::json::Map::make();
    object.insert("type"_Str, rstd::into<owe::Json>("scenetexture"_Str));
    object.insert("value"_Str, rstd::into<owe::Json>(String::make(value)));
    return Json::Object(rstd::move(object));
}

owe::script::MediaStatus ToScriptMediaStatus(const MediaStatus& status) {
    return owe::script::MediaStatus { .state            = status.state,
                                      .title            = status.title.clone(),
                                      .artist           = status.artist.clone(),
                                      .album            = status.album.clone(),
                                      .album_artist     = status.album_artist.clone(),
                                      .art_url          = status.art_url.clone(),
                                      .previous_art_url = status.previous_art_url.clone() };
}

} // namespace

using MainSender     = rstd::sync::mpmc::Sender<MainMsg>;
using MainReceiver   = rstd::sync::mpmc::Receiver<MainMsg>;
using RenderSender   = rstd::sync::mpmc::Sender<RenderMsg>;
using RenderReceiver = rstd::sync::mpmc::Receiver<RenderMsg>;

class SceneRenderController;

class SceneRuntimeController {
public:
    explicit SceneRuntimeController(TextureMemoryOptions);
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
    void on(MainMsg::UnmountSound_payload&&);
    void on(MainMsg::MountSound_payload&&);
    void on(MainMsg::SetFillMode_payload&&);
    void on(MainMsg::SetSpeed_payload&&);
    void on(MainMsg::SetMouseParallax_payload&&);
    void on(MainMsg::SetUserProperty_payload&&);
    void on(MainMsg::SetFirstFrameCallback_payload&&);
    void on(MainMsg::SetUserPropertyDiagnosticCallback_payload&&);
    void on(MainMsg::UserPropertyDiagnostics_payload&&);
    void on(MainMsg::SceneClearColorChanged_payload&&);
    void on(MainMsg::PreparedPassDiagnostics_payload&&);
    void on(MainMsg::PreparationDiagnostics_payload&&);
    void on(MainMsg::Stop_payload&&);
    void on(MainMsg::LoadBenchBatch_payload&&);
    void on(MainMsg::LoadBenchFinish_payload&&);
    void on(MainMsg::FirstFrame_payload&&);

    bool isGenGraphviz() const { return m_config.graphviz; }

    void setOnClearColor(ClearColorCallback cb) { m_clear_color_cb = rstd::move(cb); }

private:
    TextureMemoryOptions m_texture_memory;
    MainSender           sender() const;
    void                 startMainLoop();
    void                 stopMainLoop();
    void                 loadScene();
    void                 ensureLoadBench(const Option<SceneLoadBenchHandle>&);
    void ingestLoadBenchBatch(const Option<SceneLoadBenchHandle>&, rstd::bench::probe::ProbeBatch);
    void finishLoadBench();
    auto loadBenchView() -> SceneLoadBenchRecorderView;
    auto schemeColor() const -> Option<array<float, 3>>;
    void publishClearColor(array<float, 3> fallback);

    bool m_inited { false };

    SceneWallpaperConfig               m_config;
    rstd::json::Map                    m_user_properties;
    Option<vulkan::DeviceCapabilities> m_render_capabilities;

    Box<wavsen::audio::SoundManager> m_sound_manager;
    FirstFrameCallback               m_first_frame_callback;
    UserPropertyDiagnosticCallback   m_user_property_diagnostic_cb;
    ClearColorCallback               m_clear_color_cb;
    Option<Instant>                  m_audio_pause_deadline;
    bool                             m_audio_activated {};

    Option<SceneLoadBenchHandle>               m_load_bench;
    Option<rstd::bench::probe::ProbeCollector> m_load_bench_collector;
    Vec<rstd::bench::probe::ProbeBatch>        m_load_bench_batches;
    Option<rstd::bench::probe::ProbeRecorder>  m_load_bench_recorder;
    Option<rstd::bench::probe::SpanGuard>      m_load_total_span;
    u64                                        m_latest_load_bench_run_id { 0 };

    Option<Arc<SceneLoadState>>            m_loading_scene;
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
    void on(RenderMsg::SetMouseParallax_payload&&);
    void on(RenderMsg::SetUserProperty_payload&&);
    void on(RenderMsg::SetMediaStatus_payload&&);
    void on(RenderMsg::SetAudioResponseDemandCallback_payload&&);
    void on(RenderMsg::SetAudioResponseEnabled_payload&&);
    void on(RenderMsg::SetAudioPcmWindow_payload&&);
    void on(RenderMsg::Stop_payload&&);
    void onDraw();
    void on(RenderMsg::SwapchainReady_payload&&);
    void on(RenderMsg::RequestPreparedPassDiagnostics_payload&&);
    void on(RenderMsg::RequestPreparationDiagnostics_payload&&);

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

    FrameTimer frame_timer;
    FpsCounter fps_counter;

private:
    auto loadBenchView() -> SceneLoadBenchRecorderView;
    void rebuildRenderGraph(vulkan::RenderGraphResourceRetention retention, bool evict_meshes,
                            SceneLoadBenchRecorderView load_bench = {});
    void detachSceneAudioResponseDemandCallback();
    void maintainResources();
    bool frameBusy() const {
        return m_render->framePending() || (m_pipeline && (*m_pipeline)->pending().frame);
    }
    void applyPendingChanges();

    SceneRuntimeController&     m_main;
    Option<Arc<SceneLoadState>> m_scene_load;

    Box<vulkan::VulkanRender>                 m_render;
    Option<Box<Scene>>                        m_scene_owner;
    Option<RenderMsg::SetScene_payload>       m_pending_scene;
    Scene*                                    m_scene { nullptr };
    Option<Arc<UniformRuntimeInput>>          m_uniform_input_owner;
    UniformRuntimeInput*                      m_uniform_input { nullptr };
    Option<Box<SceneRenderPipeline>>          m_pipeline;
    f32                                       m_speed { 1.0f };
    bool                                      m_mouse_parallax { true };
    FillMode                                  m_fillmode { FillMode::ASPECTCROP };
    bool                                      m_stopped { false };
    Option<AudioResponseDemandCallback>       m_audio_response_demand_callback;
    bool                                      m_audio_response_enabled { true };
    audio::ResponseEngine                     m_audio_response_engine;
    scene_audio::ResponseProcessor            m_scene_audio_response;
    bool                                      m_first_frame_ok { false };
    double                                    m_frame_delta {};
    Option<Instant>                           m_maintenance_deadline;
    Vec<RenderMsg::SetUserProperty_payload>   m_pending_properties;
    Option<RenderMsg::SetMediaStatus_payload> m_pending_media;
    bool                                      m_pending_view_config {};
    Option<RenderMsg::SwapchainReady_payload> m_pending_swapchain;

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
    Option<Arc<RenderSender>> m_swapchain_tx;
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
            if (m_maintenance_deadline && Instant::now() >= *m_maintenance_deadline)
                maintainResources();
            auto received = [&]() -> Result<RenderMsg, RecvTimeoutError> {
                if (m_maintenance_deadline) return rx.recv_deadline(*m_maintenance_deadline);
                auto message = rx.recv();
                if (message.is_err()) return Err(RecvTimeoutError::Disconnected);
                return Ok(rstd::move(message).unwrap());
            }();
            if (received.is_err()) {
                if (received.unwrap_err() == RecvTimeoutError::Timeout) continue;
                break;
            }

            auto message  = rstd::move(received).unwrap();
            bool shutdown = false;
            RSTD_MATCH(rstd::move(message)) {
                RSTD_CASE_PAYLOAD(Init, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(BeginSceneLoad, value) {
                    if (m_scene_load) (*m_scene_load)->active.store(false);
                    m_scene_load = Some(rstd::move(value.load));
                }
                RSTD_CASE_PAYLOAD(SetScene, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetFillMode, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetSpeed, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetMouseParallax, value) { on(rstd::move(value)); }
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
                RSTD_CASE(Draw) {
                    onDraw();
                    maintainResources();
                }
                RSTD_CASE_PAYLOAD(SwapchainReady, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(RequestPreparedPassDiagnostics, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(RequestPreparationDiagnostics, value) { on(rstd::move(value)); }
                RSTD_CASE(Shutdown) {
                    if (m_scene_load) (*m_scene_load)->active.store(false);
                    frame_timer.Stop();
                    frame_timer.SetCallback(None());
                    m_swapchain_tx = None();
                    shutdown       = true;
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
        frame_timer.SetCallback(None());
        m_swapchain_tx = None();
        m_tx           = None();
        m_rx           = None();
        m_main_tx      = None();
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
    maintainResources();
}

void SceneRenderController::maintainResources() {
    m_maintenance_deadline =
        m_render->maintainResources() ? Some(Instant::now() + Duration::from_secs(u64(1))) : None();
}

void SceneRenderController::applyPendingChanges() {
    if (frameBusy()) return;
    if (rstd::exchange(m_pending_view_config, false)) {
        if (m_pipeline) (*m_pipeline)->setFillMode(m_fillmode);
        if (m_uniform_input) m_uniform_input->SetMouseParallax(m_mouse_parallax);
    }
    auto properties = rstd::move(m_pending_properties);
    for (auto& property : properties) on(rstd::move(property));
    if (m_pending_media) on(m_pending_media.take().unwrap());
}

void SceneRenderController::onDraw() {
    if (m_pending_swapchain && ! m_render->framePending()) on(m_pending_swapchain.take().unwrap());
    if (m_pending_scene && ! m_render->framePending()) on(m_pending_scene.take().unwrap());
    applyPendingChanges();
    frame_timer.FrameBegin();
    if (m_pipeline.is_some() && m_scene && renderInited()) {
        const bool first_draw = ! m_first_frame_ok;
        auto       load_bench = loadBenchView();
        auto       first_frame_span =
            first_draw ? SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_first_frame)
                       : rstd::bench::probe::SpanGuard {};
        auto first_frame_prepare_span =
            first_draw ? SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_first_frame_prepare)
                       : rstd::bench::probe::SpanGuard {};
        auto prepared = [&]() -> Result<PrepareOutcome, ScenePrepareError> {
            // Existing frames still need execution while new preparation is blocked.
            if (m_render->framePending()) return Ok(PrepareOutcome::Ready);
            if ((*m_pipeline)->pending().frame) return (*m_pipeline)->retryPreparation(load_bench);
            if (! (*m_pipeline)->ready()) {
                auto retried = (*m_pipeline)->retryPreparation(load_bench);
                if (retried.is_err() || *retried != PrepareOutcome::Ready) return retried;
            }
            m_frame_delta =
                m_stopped ? 0.0 : frame_timer.TargetFrameTime() * m_speed.to_primitive();
            owe::script::FrameInputs fi;
            fi.frametime   = static_cast<float>(m_frame_delta);
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
            return (*m_pipeline)->prepare(fi, load_bench);
        }();
        if (prepared.is_err() || *prepared != PrepareOutcome::Ready) {
            if (prepared.is_err()) ReportScenePreparationError(prepared.unwrap_err());
            frame_timer.FrameEnd();
            return;
        }

        (void)first_frame_prepare_span.finish();
        auto first_draw_span =
            first_draw ? SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_first_draw)
                       : rstd::bench::probe::SpanGuard {};
        m_render->drawFrame(*m_scene);
        (void)first_draw_span.finish();
        (void)first_frame_span.finish();

        if (! m_render->frameSubmitted()) {
            frame_timer.FrameEnd();
            return;
        }
        if ((*m_pipeline)->acknowledgeFrameSubmitted()) m_scene->PassFrameTime(m_frame_delta);

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
    if (frameBusy()) {
        m_pending_view_config = true;
        return;
    }
    if (m_scene && renderInited()) {
        if (m_pipeline) (*m_pipeline)->setFillMode(m_fillmode);
    }
}

void SceneRenderController::rebuildRenderGraph(vulkan::RenderGraphResourceRetention retention,
                                               bool                                 evict_meshes,
                                               SceneLoadBenchRecorderView           load_bench) {
    if (m_pipeline) {
        auto result = (*m_pipeline)->rebuild(retention, evict_meshes, load_bench);
        if (result.is_err()) ReportScenePreparationError(result.unwrap_err());
    }
}

void SceneRenderController::on(RenderMsg::SetScene_payload&& m) {
    if (! m_scene_load || ! m.load->active.load() ||
        ! Arc<SceneLoadState>::ptr_eq(*m_scene_load, m.load))
        return;
    if (m_render->framePending()) {
        m_pending_scene = Some(rstd::move(m));
        return;
    }
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
    m_pipeline = None();
    m_pending_properties.clear();
    m_pending_media       = None();
    m_scene               = nullptr;
    m_uniform_input       = nullptr;
    m_scene_owner         = Some(rstd::move(m.scene));
    m_uniform_input_owner = Some(rstd::move(m.uniform_input));
    m_scene               = m_scene_owner->get();
    m_uniform_input       = m_uniform_input_owner->as_ptr().as_raw_ptr();
    m_uniform_input->SetMouseParallax(m_mouse_parallax);
    m_pipeline = Some(Box<SceneRenderPipeline>::make(
        *m_scene,
        *m_uniform_input,
        *m_render,
        SceneRenderPipelineOptions { m_fillmode, m_main.isGenGraphviz() }));
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

void SceneRenderController::on(RenderMsg::SetMouseParallax_payload&& m) {
    m_mouse_parallax = m.enabled;
    if (frameBusy()) {
        m_pending_view_config = true;
        return;
    }
    if (m_uniform_input) m_uniform_input->SetMouseParallax(m_mouse_parallax);
}

void SceneRenderController::on(RenderMsg::SetUserProperty_payload&& m) {
    if (! m_scene) return;
    if (frameBusy()) {
        for (auto& pending : m_pending_properties) {
            if (pending.key == m.key) {
                pending = rstd::move(m);
                return;
            }
        }
        m_pending_properties.push(rstd::move(m));
        return;
    }
    auto load_bench    = loadBenchView();
    auto property_span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_user_property);

    SceneUserPropertyMutation mutation;
    {
        auto apply_span = SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_user_property_apply);
        mutation        = SceneUserPropertyApplier::Apply(*m_scene, m.key.as_str(), m.property);
    }

    if (mutation.diagnostics_changed && m_main_tx) {
        auto diagnostics = CollectSceneUserPropertyDiagnostics(*m_scene, m.key.as_str());
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
    if (renderInited() && (m_pipeline.is_some() && (*m_pipeline)->hasGraph())) {
        auto refresh_span =
            SceneLoadSpan(load_bench, &SceneLoadProbeIds::render_user_property_refresh);
        auto result = (*m_pipeline)->refreshMaterials();
        if (result.is_err()) ReportScenePreparationError(result.unwrap_err());
    }
}

void SceneRenderController::on(RenderMsg::SetMediaStatus_payload&& m) {
    if (! m_scene) return;
    if (frameBusy()) {
        m_pending_media = Some(rstd::move(m));
        return;
    }

    owe::script::SetSceneMediaStatus(*m_scene, ToScriptMediaStatus(m.status));

    (void)SceneUserPropertyApplier::ApplyTexture(
        *m_scene, "$mediaThumbnail"_str, RuntimeTextureProperty(m.status.art_url.as_str()));
    (void)SceneUserPropertyApplier::ApplyTexture(
        *m_scene,
        "$mediaPreviousThumbnail"_str,
        RuntimeTextureProperty(m.status.previous_art_url.as_str()));
    if (renderInited() && (m_pipeline.is_some() && (*m_pipeline)->hasGraph())) {
        auto result = (*m_pipeline)->refreshMaterials();
        if (result.is_err()) ReportScenePreparationError(result.unwrap_err());
    }
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
            m_swapchain_tx = Some(Arc<RenderSender>::make(*m_tx));
            auto weak      = (*m_swapchain_tx).downgrade();
            sw->setOnReadyChanged(Some(ExSwapchainReadyCallback::make(
                [weak = rstd::move(weak)](const ExSwapchainReadyEvent& e) {
                    if (auto tx = weak.upgrade()) {
                        (void)tx->send(
                            RenderMsg::SwapchainReady(e.ready, u32(e.width), u32(e.height)));
                    }
                })));
        }
    }

    // inited, callback to load scene
    if (m_main_tx) {
        (void)m_main_tx->send(MainMsg::LoadScene(m_render->deviceCapabilities()));
    }
}

void SceneRenderController::on(RenderMsg::SwapchainReady_payload&& m) {
    if (m_render->framePending()) {
        if (m.ready && ! m_stopped) frame_timer.Run();
        m_pending_swapchain = Some(rstd::move(m));
        return;
    }
    if (! m.ready) {
        frame_timer.Stop();
        return;
    }
    bool extent_changed =
        m_render->onSwapchainReady(m.width.to_primitive(), m.height.to_primitive());
    if (extent_changed && m_scene && (m_pipeline.is_some() && (*m_pipeline)->hasGraph())) {
        rebuildRenderGraph(vulkan::RenderGraphResourceRetention::KeepSceneTextures, false);
    }
    if (m_stopped) {
        frame_timer.Stop();
        if (extent_changed && m_tx) (void)m_tx->send(RenderMsg::Draw());
    } else
        frame_timer.Run();
}

void SceneRenderController::on(RenderMsg::RequestPreparationDiagnostics_payload&& m) {
    if (! m_main_tx) return;
    auto diagnostics =
        m_pipeline ? Some((*m_pipeline)->diagnostics()) : None<ScenePreparationDiagnostics>();
    (void)m_main_tx->send(
        MainMsg::PreparationDiagnostics(rstd::move(m.cb), rstd::move(diagnostics)));
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
    (*m_clear_color_cb)->operator()(value[usize()], value[usize(1)], value[usize(2)]);
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
    m_load_bench_batches.clear();
    auto batches = BenchContext(m_load_bench).take_preload_batches();
    for (usize index; index < batches.len(); ++index) {
        ingestLoadBenchBatch(context, rstd::move(batches[index]));
    }
}

void SceneRuntimeController::ingestLoadBenchBatch(const Option<SceneLoadBenchHandle>& context,
                                                  rstd::bench::probe::ProbeBatch      batch) {
    if (! context) return;
    if (! m_load_bench) ensureLoadBench(context);
    if (! m_load_bench || ! context ||
        BenchContext(m_load_bench).run_id() != BenchContext(context).run_id() ||
        ! m_load_bench_collector)
        return;
    auto ingested = m_load_bench_collector->ingest(batch);
    if (ingested.is_err()) {
        rstd_warn("scene load probe batch schema mismatch");
        return;
    }
    m_load_bench_batches.push(rstd::move(batch));
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
            ingestLoadBenchBatch(m_load_bench, rstd::move(batch).unwrap_unchecked());
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
            auto written = WriteSceneLoadTimeline(output, m_load_bench_batches.as_slice());
            if (written.is_ok()) written = rstd::bench::probe::write_text(output, report);
            if (written.is_err()) {
                auto error = rstd::move(written).unwrap_err_unchecked();
                rstd_warn("cannot write scene load probe report {}: {}",
                          BenchContext(m_load_bench).output_path(),
                          error);
            }
        }
        m_load_bench_collector = None();
    }
    m_load_bench_batches.clear();
    m_load_bench = None();
}

void SceneRuntimeController::startMainLoop() {
    if (m_main_thread) return;
    if (! m_main_rx) rstd::panic { "main mailbox cannot be restarted" };

    auto rx       = rstd::move(m_main_rx.take()).unwrap_unchecked();
    auto thread   = rstd::thread::spawn([this, rx = rstd::move(rx)]() mutable {
        rstd_info("main loop started");
        while (true) {
            if (m_audio_pause_deadline && Instant::now() >= *m_audio_pause_deadline) {
                m_audio_pause_deadline = None();
                m_sound_manager->pause();
            }
            auto received = [&]() -> Result<MainMsg, RecvTimeoutError> {
                if (m_audio_pause_deadline) return rx.recv_deadline(*m_audio_pause_deadline);
                auto message = rx.recv();
                if (message.is_err()) return Err(RecvTimeoutError::Disconnected);
                return Ok(rstd::move(message).unwrap());
            }();
            if (received.is_err()) {
                if (received.unwrap_err() == RecvTimeoutError::Timeout) continue;
                break;
            }

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
                RSTD_CASE_PAYLOAD(UnmountSound, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(MountSound, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetFillMode, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetSpeed, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetMouseParallax, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetUserProperty, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetFirstFrameCallback, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SetUserPropertyDiagnosticCallback, value) {
                    on(rstd::move(value));
                }
                RSTD_CASE_PAYLOAD(UserPropertyDiagnostics, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(SceneClearColorChanged, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(PreparedPassDiagnostics, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(PreparationDiagnostics, value) { on(rstd::move(value)); }
                RSTD_CASE_PAYLOAD(Stop, value) { on(rstd::move(value)); }
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
            m_sound_manager->activate(wavsen::audio::AudioDeviceEventSink::make(
                [tx = rstd::move(tx)](wavsen::audio::AudioDeviceEvent event) {
                    (void)tx.send(MainMsg::AudioDeviceEvent(rstd::move(event)));
                }));
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
    m_user_properties = NormalizeSceneUserProperties(m_config.user_properties);
    on(MainMsg::SetFps_payload { u32(m_config.fps) });
    on(MainMsg::SetVolume_payload { f32(m_config.volume) });
    on(MainMsg::SetVolumeScale_payload { f32(m_config.volume_scale) });
    on(MainMsg::SetMuted_payload { m_config.muted });
    on(MainMsg::SetFillMode_payload { m_config.fill_mode });
    on(MainMsg::SetSpeed_payload { f32(m_config.speed) });
    on(MainMsg::SetMouseParallax_payload { m_config.mouse_parallax });
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
        .application_name = rstd::move(m.identity.application_name),
        .application_id   = rstd::move(m.identity.application_id),
        .stream_prefix    = rstd::move(m.identity.stream_prefix),
        .component        = rstd::move(m.identity.component),
        .media_name       = rstd::move(m.identity.media_name),
        .media_role       = rstd::move(m.identity.media_role),
    };
    if (! m_sound_manager->set_identity(rstd::move(identity))) {
        rstd_warn("audio identity cannot change after audio shutdown");
    }
}

void SceneRuntimeController::on(MainMsg::AudioDeviceEvent_payload&& m) {
    m_sound_manager->on_device_event(rstd::move(m.event));
}

void SceneRuntimeController::on(MainMsg::UnmountSound_payload&& m) {
    (void)m_sound_manager->unmount(m.id);
}

void SceneRuntimeController::on(MainMsg::MountSound_payload&& m) {
    if (! m.load->active.load() || ! m_loading_scene ||
        ! Arc<SceneLoadState>::ptr_eq(*m_loading_scene, m.load) || m.mount->IsCanceled())
        return;
    auto id = m_sound_manager->mount(rstd::move(m.stream));
    if (id.is_none()) {
        rstd_warn("failed to mount scene sound");
        return;
    }
    if (! m.load->active.load() || ! m.mount->Attach(*id)) (void)m_sound_manager->unmount(*id);
}

void SceneRuntimeController::on(MainMsg::SetFillMode_payload&& m) {
    m_config.fill_mode = m.mode;
    m_render_controller->post(RenderMsg::SetFillMode(m.mode));
}

void SceneRuntimeController::on(MainMsg::SetMouseParallax_payload&& m) {
    m_config.mouse_parallax = m.enabled;
    m_render_controller->post(RenderMsg::SetMouseParallax(m.enabled));
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
    auto property = CanonicalSceneUserPropertyKey(m.key.as_str());
    auto current  = m_user_properties.get(property);
    Json prop     = current.is_some() ? MergeUserPropertyDescriptor(**current, m.value)
                                      : MakeUserPropertyDescriptor(rstd::move(m.value));
    m_config.user_properties.insert(::alloc::string::String::make(property), prop.clone());
    m_user_properties.insert(::alloc::string::String::make(property), prop.clone());
    if (property == "schemecolor"_str) {
        auto color = ResolveSceneUserPropertyColor(prop);
        if (color.is_some() && m_clear_color_cb) {
            const auto value = *color;
            (*m_clear_color_cb)->operator()(value[usize()], value[usize(1)], value[usize(2)]);
        }
    }
    m_render_controller->post(RenderMsg::SetUserProperty(rstd::into(property), rstd::move(prop)));
}

void SceneRuntimeController::on(MainMsg::SetFirstFrameCallback_payload&& m) {
    m_first_frame_callback = rstd::move(m.cb);
}

void SceneRuntimeController::on(MainMsg::SetUserPropertyDiagnosticCallback_payload&& m) {
    m_user_property_diagnostic_cb = rstd::move(m.cb);
}

void SceneRuntimeController::on(MainMsg::UserPropertyDiagnostics_payload&& m) {
    if (m_user_property_diagnostic_cb)
        (*m_user_property_diagnostic_cb)->operator()(rstd::move(m.diagnostics));
}

void SceneRuntimeController::on(MainMsg::SceneClearColorChanged_payload&& m) {
    if (schemeColor().is_none() && m_clear_color_cb) {
        (*m_clear_color_cb)->operator()(m.r.to_primitive(), m.g.to_primitive(), m.b.to_primitive());
    }
}

void SceneRuntimeController::on(MainMsg::PreparationDiagnostics_payload&& m) {
    m.cb->call_once(rstd::move(m.diagnostics));
}

void SceneRuntimeController::on(MainMsg::PreparedPassDiagnostics_payload&& m) {
    m.cb->call_once(rstd::move(m.diagnostics));
}

void SceneRuntimeController::on(MainMsg::Stop_payload&& m) {
    m_audio_pause_deadline = None();
    if (m.stop) {
        if (m.scale_audio) m_sound_manager->set_volume_scale(f32(), m.fade_ms);
        if (m.fade_ms == u32() || ! m.scale_audio) {
            m_sound_manager->pause();
        } else {
            m_audio_pause_deadline =
                Some(Instant::now() + Duration::from_millis(rstd::into<u64>(m.fade_ms)));
        }
    } else {
        m_sound_manager->play();
        if (m.scale_audio) m_sound_manager->set_volume_scale(f32(1.0f), m.fade_ms);
    }
    m_render_controller->post(RenderMsg::Stop(m.stop));
}

void SceneRuntimeController::on(MainMsg::LoadBenchBatch_payload&& m) {
    ingestLoadBenchBatch(m.context, rstd::move(m.batch));
}

void SceneRuntimeController::on(MainMsg::LoadBenchFinish_payload&& m) {
    if (m_load_bench && m.context &&
        BenchContext(m_load_bench).run_id() == BenchContext(m.context).run_id()) {
        finishLoadBench();
    }
}

void SceneRuntimeController::on(MainMsg::FirstFrame_payload&& m) {
    if (m.batch) ingestLoadBenchBatch(m.context, rstd::move(*m.batch));
    if (m_load_bench && m.context &&
        BenchContext(m_load_bench).run_id() == BenchContext(m.context).run_id()) {
        finishLoadBench();
    }
    if (m_first_frame_callback) (*m_first_frame_callback)->operator()();
}

void SceneRuntimeController::loadScene() {
    ensureLoadBench(m_config.load_bench);
    if (m_config.source_pkg_path.is_empty() || m_config.assets_dir.is_empty()) {
        finishLoadBench();
        return;
    }
    if (m_load_bench && ! m_load_bench_recorder) {
        rstd::bench::probe::RecorderConfig config;
        config.sample_capacity = usize(32768);
        m_load_bench_recorder  = Some(BenchContext(m_load_bench).session().recorder(config));
        m_load_total_span = Some(SceneLoadSpan(loadBenchView(), &SceneLoadProbeIds::load_total));
    }
    if (m_loading_scene) (*m_loading_scene)->active.store(false);
    auto scene_load = Arc<SceneLoadState>::make();
    m_loading_scene = Some(scene_load.clone());
    if (m_render_controller->sender()
            .send(RenderMsg::BeginSceneLoad(scene_load.clone()))
            .is_err()) {
        scene_load->active.store(false);
        finishLoadBench();
        return;
    }
    auto abort_load = [this, scene_load = scene_load.clone()] {
        scene_load->active.store(false);
        finishLoadBench();
    };

    if (m_config.random_seed.is_some()) {
        using Seed = decltype(Random::max());
        Random::seed(static_cast<Seed>(m_config.random_seed->to_primitive()));
    }

    rstd_info("loading scene: {}", m_config.source_pkg_path.as_path().to_string_lossy());

    {
        auto span = SceneLoadSpan(loadBenchView(), &SceneLoadProbeIds::load_audio);
        m_sound_manager->unmount_all();
    }

    auto loaded = LoadScene(SceneLoadRequest {
        .source_pkg_path = m_config.source_pkg_path.clone(),
        .assets_dir = m_config.assets_dir.clone(),
        .cache_dir = m_config.cache_dir.clone(),
        .document = m_config.scene_document.clone(),
        .user_properties = m_user_properties.clone(),
        .capabilities = {
            .directional_shadow = m_render_capabilities->directional_shadow(),
            .geometry_shader_supported = m_render_capabilities->geometry_shader,
            .max_geometry_output_vertices = u32(m_render_capabilities->max_geometry_output_vertices),
            .max_geometry_total_output_components = u32(m_render_capabilities->max_geometry_total_output_components),
        },
        .load_bench = loadBenchView(),
        .sound_unmount = Some(SceneSoundUnmountCallback::make(
            [tx = sender()](wavsen::audio::SoundStreamId id) {
                (void)tx.send(MainMsg::UnmountSound(id));
            })),
        .sound_mount = Some(SceneSoundMountCallback::make(
            [tx = sender(), load = scene_load.clone()](
                Box<dyn<wavsen::audio::SoundStreamObject>> stream,
                Arc<SceneSoundMountState> mount) {
                return tx.send(MainMsg::MountSound(load.clone(), rstd::move(stream),
                                                   rstd::move(mount))).is_ok();
            })),
        .texture_memory = m_texture_memory.clone(),
    }, mut_ref<wavsen::audio::SoundManager>::from_raw_parts(m_sound_manager.get()));
    if (loaded.is_err()) {
        rstd_error("scene load failed: {}", loaded.unwrap_err().message);
        abort_load();
        return;
    }
    auto result       = loaded.unwrap();
    m_user_properties = rstd::move(result.user_properties);
    if (result.initial_mutation.diagnostics_changed && m_user_property_diagnostic_cb)
        (*m_user_property_diagnostic_cb)
            ->operator()(
                CloneUserPropertyDiagnostics(result.parsed.scene->UserPropertyDiagnostics()));
    publishClearColor(result.parsed.scene->ClearColor());
    auto parsed = rstd::move(result.parsed);
    auto rtx    = m_render_controller->sender();
    if (rtx.send(RenderMsg::SetScene(scene_load.clone(),
                                     rstd::move(parsed.scene),
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
        frameTimer.SetCallback(Some(FrameTimer::Callback::make([rtx]() mutable {
            (void)rtx.send(RenderMsg::Draw());
        })));
        frameTimer.SetRequiredFps(u16(15));
        frameTimer.Run();
    }

    m_inited = true;
    return true;
}

SceneRuntimeController::SceneRuntimeController(TextureMemoryOptions memory)
    : m_texture_memory(rstd::move(memory)),
      m_sound_manager(Box<wavsen::audio::SoundManager>::make()),
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

SceneWallpaper::SceneWallpaper(): SceneWallpaper(TextureMemoryOptions {}) {}

SceneWallpaper::SceneWallpaper(TextureMemoryOptions memory)
    : m_runtime(Box<SceneRuntimeController>::make(rstd::move(memory))) {}

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

void SceneWallpaper::setMouseParallax(bool enabled) {
    m_runtime->post(MainMsg::SetMouseParallax(enabled));
}

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

void SceneWallpaper::setUserPropertyRaw(ref<str> name, ref<str> value) {
    m_runtime->post(MainMsg::SetUserProperty(String::make(name), RawUserProperty(value)));
}

void SceneWallpaper::setUserPropertyJson(ref<str> name, Json value) {
    m_runtime->post(MainMsg::SetUserProperty(String::make(name), rstd::move(value)));
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

void SceneWallpaper::requestPreparationDiagnostics(ScenePreparationDiagnosticCallback cb) {
    m_runtime->post(RenderMsg::RequestPreparationDiagnostics(rstd::move(cb)));
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
    auto deadline = Instant::now() + Duration::from_millis(u64(timeout_ms));
    auto rh       = m_runtime->renderController();
    while (Instant::now() < deadline) {
        if (rh->renderInited()) return true;
        rstd::thread::sleep(Duration::from_millis(u64(2)));
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
