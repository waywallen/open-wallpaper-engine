#include <rstd/test/gtest.hpp>

import rstd;
import wescene.scene_wallpaper;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

namespace
{
struct Released {
    int* count;
    explicit Released(int* value): count(value) {}
    ~Released() { ++*count; }
};
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
