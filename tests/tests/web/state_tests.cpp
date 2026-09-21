module;
#include <rstd/test/gtest.hpp>

module weweb;

import rstd;
import :cef;
import :cef_internal;
import :browser_host;

using namespace rstd::prelude;
using namespace rstd::literals;
using namespace weweb;

namespace
{
struct ReleaseProbe {
    int* count;
    explicit ReleaseProbe(int* value): count(value) {}
    ~ReleaseProbe() { ++*count; }
};
} // namespace

TEST(WebState, CpuFrameBorrowAndReentrantClear) {
    CefRefPtr<OsrRenderHandler> handler  = new OsrRenderHandler;
    int                         released = 0;
    int                         calls    = 0;
    rstd::array<rstd::u8, 8>    pixels {};
    auto                        owner = Box<ReleaseProbe>::make(&released);
    handler->SetCpuPaintCallback(
        Some(CpuPaintCallback::make([&, owner = rstd::move(owner)](const CpuPaintFrame& frame) {
            ++calls;
            EXPECT_EQ(frame.buffer, pixels.data());
            EXPECT_EQ(frame.width, 2);
            EXPECT_EQ(frame.height, 1);
            EXPECT_EQ(frame.row_stride, 8u);
            handler->SetCpuPaintCallback(None());
            handler->SetViewSize(32, 16);
            EXPECT_EQ(released, 0);
        })));
    handler->OnPaint(nullptr, PET_POPUP, {}, pixels.data(), 2, 1);
    handler->OnPaint(nullptr, PET_VIEW, {}, nullptr, 2, 1);
    handler->OnPaint(nullptr, PET_VIEW, {}, pixels.data(), 0, 1);
    EXPECT_EQ(calls, 0);
    handler->OnPaint(nullptr, PET_VIEW, {}, pixels.data(), 2, 1);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(released, 1);
    handler->OnPaint(nullptr, PET_VIEW, {}, pixels.data(), 2, 1);
    EXPECT_EQ(calls, 1);
    CefRect rect;
    handler->GetViewRect(nullptr, rect);
    EXPECT_EQ(rect.width, 32);
    EXPECT_EQ(rect.height, 16);
}

TEST(WebState, CallbackReplacementReleasesOutsideLock) {
    CefRefPtr<OsrRenderHandler> handler = new OsrRenderHandler;
    struct ResizeOnRelease {
        OsrRenderHandler* handler;
        explicit ResizeOnRelease(OsrRenderHandler* value): handler(value) {}
        ~ResizeOnRelease() { handler->SetViewSize(64, 48); }
    };
    handler->SetCpuPaintCallback(Some(CpuPaintCallback::make(
        [owner = Box<ResizeOnRelease>::make(handler.get())](const CpuPaintFrame&) {
        })));
    handler->SetCpuPaintCallback(Some(CpuPaintCallback::make([](const CpuPaintFrame&) {
    })));
    CefRect rect;
    handler->GetViewRect(nullptr, rect);
    EXPECT_EQ(rect.width, 64);
    EXPECT_EQ(rect.height, 48);
}

TEST(WebState, ViewStateRejectsInvalidValues) {
    CefRefPtr<OsrRenderHandler> handler = new OsrRenderHandler;
    handler->SetViewSize(40, 30);
    handler->SetViewSize(0, 20);
    handler->SetDeviceScaleFactor(2.0f);
    handler->SetDeviceScaleFactor(-1.0f);
    handler->SetDeviceScaleFactor(__builtin_nanf(""));
    CefScreenInfo info;
    EXPECT_TRUE(handler->GetScreenInfo(nullptr, info));
    EXPECT_EQ(info.rect.width, 40);
    EXPECT_EQ(info.rect.height, 30);
    EXPECT_EQ(info.device_scale_factor, 2.0f);
}

TEST(WebState, AudioDemandCanClearItsOwnCallback) {
    CefRefPtr<OsrRenderHandler> render   = new OsrRenderHandler;
    CefRefPtr<ClientHandler>    client   = new ClientHandler(owe::Json::Null(), render, false);
    int                         released = 0;
    int                         calls    = 0;
    client->SetAudioDemandCallback(Some(
        AudioDemandCallback::make([&, owner = Box<ReleaseProbe>::make(&released)](bool active) {
            ++calls;
            EXPECT_FALSE(active);
            client->SetAudioDemandCallback(None());
            EXPECT_EQ(released, 0);
        })));
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(released, 1);
    int closed = 0;
    client->SetCloseCallback(Box<dyn<Fn<void()>>>::make([&] {
        ++closed;
    }));
    client->OnBeforeClose(nullptr);
    EXPECT_EQ(closed, 1);
}

TEST(WebState, HostOwnsCallbacksBeforeInitialization) {
    int released = 0;
    {
        BrowserHost host;
        host.SetCpuPaintCallback(
            [owner = Box<ReleaseProbe>::make(&released)](const CpuPaintFrame&) {
            });
        host.SetCpuPaintCallback(None());
        EXPECT_EQ(released, 1);
        EXPECT_FALSE(host.ShouldExit());
        host.RequestClose();
        EXPECT_TRUE(host.ShouldExit());
        host.Shutdown();
        host.Shutdown();
    }
    EXPECT_EQ(released, 1);
}

TEST(WebState, AudioSnippetPreservesPayload) {
    rstd::array<float, 3> data { 0.0f, 0.5f, -1.0f };
    EXPECT_EQ(
        BuildAudioResponseSnippet(data.as_slice()).as_str(),
        "(function(){if(!window.__weweb_pushAudio)return;window.__weweb_pushAudio([0.00000000e0,5.00000000e-1,-1.00000000e0]);})();"_str);
}

TEST(WebState, AudioSnippetPreservesFloatPrecisionAndSpecialValues) {
    array<float, 1> subnormal { f32::from_bits(u32(1)).to_primitive() };
    EXPECT_EQ(
        BuildAudioResponseSnippet(subnormal.as_slice()).as_str(),
        "(function(){if(!window.__weweb_pushAudio)return;window.__weweb_pushAudio([1.40129846e-45]);})();"_str);
    array<float, 7> data { 0.1f,
                           -0.0f,
                           f32::MIN_POSITIVE.to_primitive(),
                           f32::MAX.to_primitive(),
                           f32::NAN_.to_primitive(),
                           f32::INFINITY_.to_primitive(),
                           (-f32::INFINITY_).to_primitive() };
    EXPECT_EQ(
        BuildAudioResponseSnippet(data.as_slice()).as_str(),
        "(function(){if(!window.__weweb_pushAudio)return;window.__weweb_pushAudio([1.00000001e-1,-0.00000000e0,1.17549435e-38,3.40282347e38,NaN,Infinity,-Infinity]);})();"_str);
    EXPECT_EQ(
        BuildAudioResponseSnippet(slice<float>()).as_str(),
        "(function(){if(!window.__weweb_pushAudio)return;window.__weweb_pushAudio([]);})();"_str);
}

TEST(WebState, PropertySnippetEscapesKey) {
    auto snippet = BuildPropertyPatchSnippet("a\"b"_str, owe::Json::Bool(true));
    EXPECT_TRUE(snippet.as_str().contains("{\"a\\\"b\":true}"_str));
    auto empty = BuildPropertyListenerSnippet(owe::Json::Null());
    EXPECT_TRUE(empty.as_str().contains("applyUserProperties({}"_str));
}

TEST(WebManifest, LoadsOwnedStringsAndDefaults) {
    auto temporary = rstd::fs::TempDir::make("owe-web-manifest"_str).unwrap();
    auto root      = rstd::path::PathBuf::from(temporary.path());
    auto path      = root.as_path();
    auto content =
        R"({/*comment*/"type":"WeB","title":"\u4f60\u597d","file":"page.html","preview":"preview.jpg","general":{"properties":{"enabled":{"value":true}}}})"_str;
    ASSERT_TRUE(
        rstd::fs::write(root.join("project.json"_str).as_path(), content.as_bytes()).is_ok());
    auto manifest = LoadWebManifest(path);
    ASSERT_TRUE(manifest.is_some());
    EXPECT_EQ(manifest->title.as_str(), "\xe4\xbd\xa0\xe5\xa5\xbd"_str);
    EXPECT_EQ(manifest->entry_html.as_str(), "page.html"_str);
    ASSERT_TRUE(manifest->preview.is_some());
    EXPECT_EQ(manifest->preview->as_str(), "preview.jpg"_str);
    EXPECT_TRUE(manifest->user_props.get("enabled"_str).is_some());
    ASSERT_TRUE(
        rstd::fs::write(root.join("project.json"_str).as_path(),
                        R"({"type":"web","title":0,"file":null,"preview":false})"_str.as_bytes())
            .is_ok());
    auto defaults = LoadWebManifest(path);
    ASSERT_TRUE(defaults.is_some());
    EXPECT_EQ(defaults->title.as_str(), "Wallpaper"_str);
    EXPECT_EQ(defaults->entry_html.as_str(), "index.html"_str);
    EXPECT_TRUE(defaults->preview.is_none());
    EXPECT_EQ(manifest->entry_html.as_str(), "page.html"_str);
}

TEST(WebManifest, RejectsInvalidDocuments) {
    auto temporary = rstd::fs::TempDir::make("owe-web-invalid"_str).unwrap();
    auto root      = rstd::path::PathBuf::from(temporary.path());
    auto path      = root.as_path();
    EXPECT_TRUE(LoadWebManifest(path).is_none());
    for (auto content : rstd::array<ref<str>, 6> { "{"_str,
                                                   "{}"_str,
                                                   R"({"type":1})"_str,
                                                   R"({"type":"scene"})"_str,
                                                   R"({"type":"\u4f60"})"_str,
                                                   R"({"type":"web\u0000"})"_str }) {
        ASSERT_TRUE(
            rstd::fs::write(root.join("project.json"_str).as_path(), content.as_bytes()).is_ok());
        EXPECT_TRUE(LoadWebManifest(path).is_none());
    }
}

TEST(WebManifest, ReadsNativePathBytes) {
    auto temporary = rstd::fs::TempDir::make("owe-web-path"_str).unwrap();
    auto root      = rstd::path::PathBuf::from(temporary.path());
    auto name      = Vec<u8>::from("web-"_str.as_bytes());
    name.push(u8(0xff));
    auto native    = rstd::ffi::OsString::from_encoded_bytes_unchecked(rstd::move(name));
    auto directory = root.join(rstd::ref<rstd::path::Path>(native.as_os_str()));
    ASSERT_TRUE(rstd::fs::create_dir(directory.as_path()).is_ok());
    ASSERT_TRUE(rstd::fs::write(directory.join("project.json"_str).as_path(),
                                R"({"type":"web"})"_str.as_bytes())
                    .is_ok());
    EXPECT_TRUE(LoadWebManifest(directory.as_path()).is_some());
}

#if ! __is_target_os(macos)
TEST(WebState, AcceleratedFrameBorrowsDescriptors) {
    CefRefPtr<OsrRenderHandler> handler = new OsrRenderHandler;
    int                         calls   = 0;
    handler->SetAcceleratedPaintCallback(
        Some(AcceleratedPaintCallback::make([&](const DmaBufFrame& frame) {
            ++calls;
            EXPECT_EQ(frame.plane_count, 1);
            EXPECT_EQ(frame.planes[0].fd, -1);
            EXPECT_EQ(frame.planes[0].stride, 64u);
            EXPECT_EQ(frame.coded_width, 16);
            EXPECT_EQ(frame.visible_width, 12);
            handler->SetAcceleratedPaintCallback(None());
        })));
    CefAcceleratedPaintInfo info {};
    info.plane_count              = 1;
    info.planes[0].fd             = -1;
    info.planes[0].stride         = 64;
    info.extra.coded_size.width   = 16;
    info.extra.visible_rect.width = 12;
    handler->OnAcceleratedPaint(nullptr, PET_POPUP, {}, info);
    EXPECT_EQ(calls, 0);
    handler->OnAcceleratedPaint(nullptr, PET_VIEW, {}, info);
    EXPECT_EQ(calls, 1);
    handler->OnAcceleratedPaint(nullptr, PET_VIEW, {}, info);
    EXPECT_EQ(calls, 1);
}
#endif
