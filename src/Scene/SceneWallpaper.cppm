export module wescene.scene_wallpaper;
import vvk;
import rstd;

export import wescene.core;
export import wescene.json;
export import rstd.cppstd;
export import wescene.load_bench;
export import wescene.scene;
export import wescene.vulkan_render;
export import wescene.vulkan;
export import wescene.types;
export import wescene.utils;
export import wescene.pkg.parse;
export import owe.audio_response;

using namespace rstd::prelude;
using rstd::path::PathBuf;
using rstd::sync::Arc;
using namespace rstd::literals;

export namespace owe
{

using FirstFrameCallback          = Option<Box<dyn<FnMut<void()>>>>;
using AudioResponseDemandCallback = Arc<dyn<rstd::Fn<void(bool)>>>;
using UserPropertyDiagnosticCallback =
    Option<Box<dyn<FnMut<void(Vec<SceneUserPropertyDiagnostic>)>>>>;
using RenderPassDiagnosticCallback =
    Box<dyn<rstd::FnOnce<void(Vec<vulkan::PreparedPassDiagnostic>)>>>;

// Publishes the effective wallpaper background color. The project scheme color
// takes precedence over `general.clearcolor` and runtime changes are emitted.
// Components are 0..=1 sRGB. Alpha is fixed at 1.0 by the host.
using ClearColorCallback = Option<Box<dyn<FnMut<void(float, float, float)>>>>;

struct MediaStatus {
    uint32_t state { 0 };
    String   title;
    String   artist;
    String   album;
    String   album_artist;
    String   art_url;
    String   previous_art_url;
};

struct SceneAudioClientIdentity {
    String application_name;
    String application_id;
    String stream_prefix;
    String component;
    String media_name;
    String media_role { "music"_Str };
};

struct SceneWallpaperConfig {
    PathBuf                             source_pkg_path;
    PathBuf                             assets_dir;
    PathBuf                             cache_dir;
    Option<Arc<wpscene::SceneDocument>> scene_document;
    Option<SceneLoadBenchHandle>        load_bench;
    rstd::json::Map                     user_properties;
    uint32_t                            fps { 30 };
    float                               volume { 1.0f };
    float                               volume_scale { 1.0f };
    bool                                muted { false };
    FillMode                            fill_mode { FillMode::ASPECTCROP };
    float                               speed { 1.0f };
    bool                                graphviz { false };
    Option<u64>                         random_seed;
};

class SceneRuntimeController;

class SceneWallpaper : NoCopy {
public:
    SceneWallpaper();
    ~SceneWallpaper();
    bool init();
    bool inited() const;

    void initVulkan(RenderInitInfo);

    void play();
    void play(uint32_t fade_ms);
    void pause();
    void pause(uint32_t fade_ms);
    void requestFrame();
    void mouseInput(double x, double y);
    // button: 0=left, 1=right, 2=middle (GLFW numbering). down=true on
    // press, false on release.
    void mouseButton(int button, bool down);
    void mouseEnter(bool in_window);

    void configure(SceneWallpaperConfig);
    void setFps(uint32_t);
    void setVolume(float);
    void setVolumeScale(float);
    void setVolumeScale(float, uint32_t fade_ms);
    void setMuted(bool);
    void setFillMode(FillMode);
    void setSpeed(float);
    void setMediaStatus(MediaStatus);
    void setAudioClientIdentity(SceneAudioClientIdentity);
    void setAudioResponseDemandCallback(AudioResponseDemandCallback);
    template<typename Callback>
    void setAudioResponseDemandCallback(Callback callback) {
        setAudioResponseDemandCallback(AudioResponseDemandCallback::make(rstd::move(callback)));
    }
    void setAudioResponseEnabled(bool);
    void setAudioPcmWindow(audio::PcmWindow window);
    void endAudioResponse();
    void setUserPropertyRaw(ref<str>, ref<str>);
    void setUserPropertyJson(ref<str>, Json);
    void setOnFirstFrame(FirstFrameCallback);
    template<typename Callback>
        requires requires(Callback cb) { cb(); }
    void setOnFirstFrame(Callback cb) {
        setOnFirstFrame(Some(Box<dyn<FnMut<void()>>>::make(rstd::move(cb))));
    }
    void setOnUserPropertyDiagnostics(UserPropertyDiagnosticCallback);
    template<typename Callback>
        requires requires(Callback cb, Vec<SceneUserPropertyDiagnostic> diagnostics) {
            cb(rstd::move(diagnostics));
        }
    void setOnUserPropertyDiagnostics(Callback cb) {
        setOnUserPropertyDiagnostics(
            Some(Box<dyn<FnMut<void(Vec<SceneUserPropertyDiagnostic>)>>>::make(rstd::move(cb))));
    }
    void requestPreparedPassDiagnostics(RenderPassDiagnosticCallback);

    // Install a callback for the effective wallpaper background color.
    // Set once before initVulkan.
    void setOnClearColor(ClearColorCallback);
    template<typename Callback>
        requires requires(Callback cb) { cb(0.0f, 0.0f, 0.0f); }
    void setOnClearColor(Callback cb) {
        setOnClearColor(Some(Box<dyn<FnMut<void(float, float, float)>>>::make(rstd::move(cb))));
    }

    ExSwapchain* exSwapchain() const;

    int takeLastFrameSyncFd();

    bool getDrmRenderNode(uint32_t& out_major, uint32_t& out_minor) const;

    bool waitVulkanInited(uint32_t timeout_ms);

    VkInstance       vkInstance() const;
    VkPhysicalDevice vkPhysicalDevice() const;
    VkDevice         vkDevice() const;
    VkQueue          vkGraphicsQueue() const;
    uint32_t         vkGraphicsQueueFamily() const;

    void deviceUuid(uint8_t out[16]) const;
    void driverUuid(uint8_t out[16]) const;

private:
    bool m_inited { false };

private:
    friend class SceneRuntimeController;

    bool                         m_offscreen { false };
    Option<SceneLoadBenchHandle> m_load_bench;
    Box<SceneRuntimeController>  m_runtime;
};

} // namespace owe
