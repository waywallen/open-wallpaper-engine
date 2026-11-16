#include <rstd/test/gtest.hpp>

import rstd;
import rstd.log;
import vrento;
import wescene.scene_wallpaper;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::path::PathBuf;
using rstd::sync::Arc;
using rstd::time::Duration;
using rstd::time::Instant;

namespace
{
struct Released {
    int* count;
    explicit Released(int* value): count(value) {}
    ~Released() { ++*count; }
};

class UnreadyMediaOutput : public owe::ExSwapchain {
public:
    unsigned width() const override { return 512; }
    unsigned height() const override { return 512; }
    VkFormat format() const override { return VK_FORMAT_R8G8B8A8_UNORM; }
    bool     ready() const override { return false; }
    void     setOnReadyChanged(Option<owe::ExSwapchainReadyCallback>) override {}
    auto     acquireRenderTarget() -> vrento::FrameSurfaceAcquireResult override {
        return { .status = vrento::FrameSurfaceAcquireStatus::NotReady };
    }

private:
    auto CompleteRendered(vrento::FrameSurfaceIdentity identity, int)
        -> vrento::FrameSurfaceCompletionResult override {
        return { .status = vrento::FrameSurfaceCompletionStatus::Aborted, .identity = identity };
    }
    auto AbortRenderTarget(vrento::FrameSurfaceIdentity identity)
        -> vrento::FrameSurfaceCompletionResult override {
        return { .status = vrento::FrameSurfaceCompletionStatus::Aborted, .identity = identity };
    }
};

template<class Predicate>
bool WaitFor(Predicate predicate) {
    auto deadline = Instant::now() + Duration::from_secs(u64(10));
    while (Instant::now() < deadline) {
        if (predicate()) return true;
        rstd::thread::sleep(Duration::from_millis(u64(1)));
    }
    return false;
}

auto MediaConfig(ref<rstd::path::Path> directory, ref<str> name) -> owe::SceneWallpaperConfig {
    auto project = PathBuf::from(directory).join(name);
    rstd::fs::create_dir_all(project).unwrap();
    auto document = owe::wpscene::ParseSceneDocumentJson(R"JSON({
        "camera": {},
        "general": {"orthogonalprojection": {"width":64,"height":64}},
        "objects": [{"id":1,"name":"MediaProbe","scale":{
            "value":"1 1 1",
            "script":"let playback=0, properties=0, thumbnail=0, state=0, title='', artist='', album='', albumArtist='', art='', previous=''; function save() { localStorage.set('media', {playback,properties,thumbnail,state,title,artist,album,albumArtist,art,previous}); } export function mediaPlaybackChanged(e) { state=e.state; playback++; save(); } export function mediaPropertiesChanged(e) { title=e.title; artist=e.artist; album=e.album; albumArtist=e.albumArtist; properties++; save(); } export function mediaThumbnailChanged(e) { art=e.thumbnail; previous=e.previousThumbnail; thumbnail++; save(); }"
        }}]
    })JSON"_str,
                                                         owe::wpscene::kSceneVersionUnknown);
    owe::SceneWallpaperConfig config;
    config.source_pkg_path = project.join("scene.pkg"_str);
    config.assets_dir      = PathBuf::from(directory);
    config.cache_dir       = PathBuf::from(directory).join("cache"_str);
    config.scene_document  = Some(Arc<owe::wpscene::SceneDocument>::make(document.take().unwrap()));
    return config;
}

auto ReadMedia(ref<rstd::path::Path> path) -> Option<owe::Json> {
    auto source = rstd::fs::read_to_string(path);
    if (source.is_err()) return None();
    auto parsed = rstd::json::from_str(source->as_str());
    if (parsed.is_err()) return None();
    auto media = parsed->get("media"_str);
    if (media.is_none()) return None();
    auto encoded = (*media)->as_str();
    if (encoded.is_none()) return None();
    return rstd::json::from_str(*encoded).ok();
}

bool MediaMatches(ref<rstd::path::Path> path, const owe::MediaStatus& status, int events) {
    auto value = ReadMedia(path);
    if (value.is_none()) return false;
    auto expected =
        rstd::json::from_str(
            rstd::format(R"({{"playback":{},"properties":{},"thumbnail":{},"state":{}}})",
                         events,
                         events,
                         events,
                         status.state)
                .as_str())
            .unwrap();
    for (const auto& [key, count] : expected.as_object().unwrap()->iter()) {
        auto actual = value->get(key->as_str());
        if (actual.is_none() || **actual != *count) return false;
    }
    struct TextField {
        ref<str> key, value;
    };
    for (auto [key, text] : array<TextField, 6> {
             TextField { "title"_str, status.title.as_str() },
             TextField { "artist"_str, status.artist.as_str() },
             TextField { "album"_str, status.album.as_str() },
             TextField { "albumArtist"_str, status.album_artist.as_str() },
             TextField { "art"_str, status.art_url.as_str() },
             TextField { "previous"_str, status.previous_art_url.as_str() },
         }) {
        auto actual = value->get(key);
        if (actual.is_none() || (*actual)->as_str().unwrap_or("missing"_str) != text) return false;
    }
    return true;
}
} // namespace

TEST(HostCallbacks, ReleasesQueuedMoveOnlyCallbacksWithoutInitialization) {
    int released = 0;
    {
        owe::SceneWallpaper wallpaper;
        wallpaper.setOnClearColor([owner = Box<Released>::make(&released)](float, float, float) {
        });
        wallpaper.setOnClearColor(None());
        EXPECT_EQ(released, 1);
        wallpaper.setOnFirstFrame([owner = Box<Released>::make(&released)] {
        });
        wallpaper.setOnUserPropertyDiagnostics(
            [owner = Box<Released>::make(&released)](Vec<owe::SceneUserPropertyDiagnostic>) {
            });
        EXPECT_EQ(released, 1);
    }
    EXPECT_EQ(released, 3);
}

TEST(HostCallbacks, PreservesMutableCallbackState) {
    int  observed = 0;
    auto callback = Box<dyn<FnMut<void()>>>::make([value = 0, &observed]() mutable {
        observed = ++value;
    });
    callback->operator()();
    callback->operator()();
    EXPECT_EQ(observed, 2);
}

TEST(HostCallbacks, KeepsPreloadedDocumentAliveUntilQueuedConfigurationIsReleased) {
    auto document = Arc<owe::wpscene::SceneDocument>::make();
    auto weak     = document.downgrade();
    {
        owe::SceneWallpaper       wallpaper;
        owe::SceneWallpaperConfig config;
        config.source_pkg_path = rstd::path::PathBuf::from("scene.pkg"_str);
        config.assets_dir      = rstd::path::PathBuf::from("assets"_str);
        config.scene_document  = Some(document.clone());
        wallpaper.configure(rstd::move(config));
        document.reset();
        EXPECT_FALSE(weak.expired());
    }
    EXPECT_TRUE(weak.expired());
}

TEST(HostCallbacks, ShutsDownWithoutWaitingForAudioFadeDeadline) {
    auto started = rstd::time::Instant::now();
    {
        owe::SceneWallpaper wallpaper;
        ASSERT_TRUE(wallpaper.init());
        wallpaper.pause(60000);
        wallpaper.play();
        wallpaper.pause(60000);
    }
    EXPECT_LT((rstd::time::Instant::now() - started).as_secs(), u64(10));
}

TEST(HostCallbacks, OwnsSurfaceCallbackAndDeviceIdentity) {
    int                 released = 0;
    owe::RenderInitInfo info;
    {
        rstd::array<rstd::uint8_t, 16> uuid {};
        uuid[usize()]                     = 42;
        info.uuid                         = Some(uuid);
        uuid[usize()]                     = 7;
        info.surface_info.createSurfaceOp = Some(owe::CreateSurfaceCallback::make(
            [owner = Box<Released>::make(&released)](VkInstance, VkSurfaceKHR*) {
                return VK_SUCCESS;
            }));
    }
    EXPECT_EQ((*info.uuid)[usize()], 42);
    EXPECT_EQ(released, 0);
    {
        auto callback = info.surface_info.createSurfaceOp.take().unwrap();
        EXPECT_EQ(callback->call_once(VkInstance {}, nullptr), VK_SUCCESS);
    }
    EXPECT_EQ(released, 1);
}

TEST(HostCallbacks, PreparationDiagnosticsReturnOwnedResultThroughMailboxes) {
    auto                observed = Arc<rstd::sync::atomic::Atomic<rstd::u32>>::make(rstd::u32());
    owe::SceneWallpaper wallpaper;
    ASSERT_TRUE(wallpaper.init());
    wallpaper.requestPreparationDiagnostics(owe::ScenePreparationDiagnosticCallback::make(
        [observed = observed.clone()](Option<owe::ScenePreparationDiagnostics> diagnostics) {
            observed->store(diagnostics.is_none() ? rstd::u32(1) : rstd::u32(2));
        }));
    auto started = rstd::time::Instant::now();
    while (observed->load() == rstd::u32() && started.elapsed().as_secs() < rstd::u64(5))
        rstd::thread::sleep(rstd::time::Duration::from_millis(rstd::u64(1)));
    EXPECT_EQ(observed->load(), rstd::u32(1));
}

TEST(SceneMediaStatus, KeepsLatestStateBeforeSceneLoadAndReplaysAfterReload) {
    static rstd::log::EnvLogger logger;
    rstd::log::set_logger(logger);
    rstd::log::set_max_level(logger.filter());
    auto                directory = rstd::fs::TempDir::make("owe-media-replay"_str).unwrap();
    owe::SceneWallpaper wallpaper;
    wallpaper.setMediaStatus(owe::MediaStatus { .state = 1, .title = "Superseded"_Str });
    wallpaper.setMediaStatus(owe::MediaStatus {
        .state            = 2,
        .title            = "Current"_Str,
        .artist           = "Artist"_Str,
        .album            = "Album"_Str,
        .album_artist     = "Album Artist"_Str,
        .art_url          = "/tmp/current.png"_Str,
        .previous_art_url = "/tmp/previous.png"_Str,
    });
    ASSERT_TRUE(wallpaper.init());
    auto drained = Arc<rstd::sync::atomic::Atomic<bool>>::make(false);
    wallpaper.requestPreparationDiagnostics(owe::ScenePreparationDiagnosticCallback::make(
        [drained = drained.clone()](Option<owe::ScenePreparationDiagnostics>) {
            drained->store(true);
        }));
    ASSERT_TRUE(WaitFor([&] {
        return drained->load();
    }));

    auto expected = owe::MediaStatus {
        .state            = 2,
        .title            = "Current"_Str,
        .artist           = "Artist"_Str,
        .album            = "Album"_Str,
        .album_artist     = "Album Artist"_Str,
        .art_url          = "/tmp/current.png"_Str,
        .previous_art_url = "/tmp/previous.png"_Str,
    };
    auto storage = PathBuf::from(directory.path()).join("cache/script_localstorage"_str);
    wallpaper.configure(MediaConfig(directory.path(), "first"_str));
    owe::RenderInitInfo init;
    init.offscreen = true;
    init.width = init.height = 512;
    init.video_hwdec         = "none"_Str;
    init.ex_swapchain_factory =
        Some(Box<dyn<FnOnce<Option<owe::ExSwapchainOwner>(
                 const owe::RenderInitInfo::ExSwapchainHandles&)>>>::
                 make([](const owe::RenderInitInfo::ExSwapchainHandles&) {
                     return Some(owe::ExSwapchain::Make<UnreadyMediaOutput>());
                 }));
    wallpaper.initVulkan(rstd::move(init));
    ASSERT_TRUE(wallpaper.waitVulkanInited(10000));
    ASSERT_TRUE(WaitFor([&] {
        return MediaMatches(storage.join("first.json"_str), expected, 1);
    }));

    wallpaper.configure(MediaConfig(directory.path(), "second"_str));
    ASSERT_TRUE(WaitFor([&] {
        return MediaMatches(storage.join("second.json"_str), expected, 1);
    }));
    wallpaper.requestFrame();
    drained->store(false);
    wallpaper.requestPreparationDiagnostics(owe::ScenePreparationDiagnosticCallback::make(
        [drained = drained.clone()](Option<owe::ScenePreparationDiagnostics>) {
            drained->store(true);
        }));
    ASSERT_TRUE(WaitFor([&] {
        return drained->load();
    }));
    EXPECT_TRUE(MediaMatches(storage.join("second.json"_str), expected, 1));

    wallpaper.setMediaStatus(owe::MediaStatus {});
    owe::MediaStatus stopped;
    drained->store(false);
    wallpaper.requestPreparationDiagnostics(owe::ScenePreparationDiagnosticCallback::make(
        [drained = drained.clone()](Option<owe::ScenePreparationDiagnostics>) {
            drained->store(true);
        }));
    ASSERT_TRUE(WaitFor([&] {
        return drained->load();
    }));
    EXPECT_TRUE(MediaMatches(storage.join("second.json"_str), expected, 1));
    wallpaper.configure(MediaConfig(directory.path(), "third"_str));
    ASSERT_TRUE(WaitFor([&] {
        return MediaMatches(storage.join("third.json"_str), stopped, 1);
    }));
}
