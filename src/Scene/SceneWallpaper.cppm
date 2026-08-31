module;
#include <vulkan/vulkan.h>

export module wescene.scene_wallpaper;
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
using rstd::sync::Arc;

export namespace owe
{

using FirstFrameCallback             = std::function<void()>;
using AudioResponseDemandCallback    = Arc<dyn<rstd::Fn<void(bool)>>>;
using UserPropertyDiagnosticCallback = std::function<void(Vec<SceneUserPropertyDiagnostic>)>;
using RenderPassDiagnosticCallback =
    std::function<void(std::vector<vulkan::PreparedPassDiagnostic>)>;

// Publishes the effective wallpaper background color. The project scheme color
// takes precedence over `general.clearcolor` and runtime changes are emitted.
// Components are 0..=1 sRGB. Alpha is fixed at 1.0 by the host.
using ClearColorCallback = std::function<void(float r, float g, float b)>;

struct MediaStatus {
    uint32_t    state { 0 };
    std::string title;
    std::string artist;
    std::string album;
    std::string album_artist;
    std::string art_url;
    std::string previous_art_url;
};

struct SceneAudioClientIdentity {
    std::string application_name;
    std::string application_id;
    std::string stream_prefix;
    std::string component;
    std::string media_name;
    std::string media_role { "music" };
};

struct SceneWallpaperConfig {
    std::string                             source_pkg_path;
    std::string                             assets_dir;
    std::string                             cache_dir;
    std::shared_ptr<wpscene::SceneDocument> scene_document;
    Option<SceneLoadBenchHandle>            load_bench;
    rstd::json::Map                         user_properties;
    uint32_t                                fps { 30 };
    float                                   volume { 1.0f };
    float                                   volume_scale { 1.0f };
    bool                                    muted { false };
    FillMode                                fill_mode { FillMode::ASPECTCROP };
    float                                   speed { 1.0f };
    bool                                    graphviz { false };
    Option<u64>                             random_seed;
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
    void setUserPropertyRaw(std::string_view, std::string);
    void setUserPropertyJson(std::string_view, Json);
    void setOnFirstFrame(FirstFrameCallback);
    void setOnUserPropertyDiagnostics(UserPropertyDiagnosticCallback);
    void requestPreparedPassDiagnostics(RenderPassDiagnosticCallback);

    // Install a callback for the effective wallpaper background color.
    // Set once before initVulkan.
    void setOnClearColor(ClearColorCallback);

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

    bool                                    m_offscreen { false };
    Option<SceneLoadBenchHandle>            m_load_bench;
    std::unique_ptr<SceneRuntimeController> m_runtime;
};

} // namespace owe
