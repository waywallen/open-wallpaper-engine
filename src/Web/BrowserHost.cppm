export module weweb:browser_host;

import rstd;

import wescene.json;

import :frame;
import :manifest;

using namespace rstd::prelude;
using rstd::path::Path;
using rstd::path::PathBuf;

export namespace weweb
{

class OsrRenderHandler;
class ClientHandler;

class BrowserHost {
public:
    struct InitOptions {
        PathBuf resources_dir;
        PathBuf locales_dir;
        PathBuf cache_dir;
        bool    enable_remote_debugging { false };
        int     remote_debugging_port { 0 };
        bool    enable_audio { true };
        bool    shared_texture_enabled { true };
        String  render_node_override;
    };

    struct OpenOptions {
        bool  shared_texture_enabled { true };
        bool  initially_muted { false };
        int   frame_rate { 60 };
        float device_scale_factor { 1.0f };
    };

    BrowserHost();
    ~BrowserHost();

    BrowserHost(const BrowserHost&)            = delete;
    BrowserHost& operator=(const BrowserHost&) = delete;

    int  RunOrExitIfHelper(int argc, char** argv);
    bool Init(const InitOptions& opts);

    void SetAcceleratedPaintCallback(Option<AcceleratedPaintCallback> cb);
    void SetCpuPaintCallback(Option<CpuPaintCallback> cb);
    void SetAudioResponseDemandCallback(Option<AudioDemandCallback> cb);
    template<typename Callback>
        requires requires(Callback cb, const DmaBufFrame& frame) { cb(frame); }
    void SetAcceleratedPaintCallback(Callback cb) {
        SetAcceleratedPaintCallback(Some(AcceleratedPaintCallback::make(rstd::move(cb))));
    }
    template<typename Callback>
        requires requires(Callback cb, const CpuPaintFrame& frame) { cb(frame); }
    void SetCpuPaintCallback(Callback cb) {
        SetCpuPaintCallback(Some(CpuPaintCallback::make(rstd::move(cb))));
    }
    template<typename Callback>
        requires requires(Callback cb) { cb(false); }
    void SetAudioResponseDemandCallback(Callback cb) {
        SetAudioResponseDemandCallback(Some(AudioDemandCallback::make(rstd::move(cb))));
    }

    bool OpenWallpaper(const WebManifest& manifest, ref<Path> workshop_dir, int width, int height);
    bool OpenWallpaper(const WebManifest& manifest, ref<Path> workshop_dir, int width, int height,
                       OpenOptions opts);

    void OnResize(int width, int height);
    void OnResize(int width, int height, float device_scale_factor);
    void Invalidate();

    void OnMouseMove(int x, int y, bool left_down);
    void OnMouseButton(int x, int y, int cef_button, bool down, int click_count);
    void OnMouseWheel(int x, int y, int delta_x, int delta_y);
    void OnKey(int cef_key_event_type, int native_key_code, int windows_key_code, int modifiers,
               unsigned int unicode_char);
    void OnFocus(bool gained);

    void Pump();

    void ApplyVolume(float volume);
    void SetFrameRate(int fps);
    void SetPaused(bool paused);
    void ApplyUserProperty(ref<str> key, const owe::Json& value);
    void PushAudioData(slice<float> data);

    bool ShouldExit() const;
    void RequestClose();
    void Shutdown();

private:
    struct Impl;
    Box<Impl> impl_;
};

} // namespace weweb
