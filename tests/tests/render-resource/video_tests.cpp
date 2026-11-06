#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>
import rstd;
import rstd.test;
import vrento;
import wescene.video;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;
namespace tex   = vrento::texture;
namespace vk    = vrento::vulkan;
namespace media = owe::video;
namespace owe_test::video
{
struct Playback {
    struct State {
        vrento::VideoPlaybackSnapshot snapshot;
        f64                           current;
        Option<f64>                   duration;
    };
    mutable rstd::sync::Mutex<State> state;
    auto                             Snapshot() const -> vrento::VideoPlaybackSnapshot {
        return state.lock().unwrap()->snapshot;
    }
    void PublishTime(f64 time, Option<f64> length) const {
        auto locked      = state.lock().unwrap();
        locked->current  = time;
        locked->duration = length;
    }
};
} // namespace owe_test::video
namespace rstd
{
template<>
struct Impl<vrento::VideoPlayback, Arc<owe_test::video::Playback>>
    : ImplBase<Arc<owe_test::video::Playback>> {
    auto Snapshot() const -> vrento::VideoPlaybackSnapshot { return self()->Snapshot(); }
    void PublishTime(f64 time, Option<f64> duration) const { self()->PublishTime(time, duration); }
};
} // namespace rstd

namespace owe_test::video
{
TEST(MediaRegistry, ContinuousPlaybackKeepsStableTexture) {
    auto path = rstd::env::var("VR_TEST_VIDEO"_str);
    if (path.is_err()) GTEST_SKIP();
    auto                     width  = rstd::env::var("VR_TEST_VIDEO_WIDTH"_str).unwrap();
    auto                     height = rstd::env::var("VR_TEST_VIDEO_HEIGHT"_str).unwrap();
    auto                     hwdec  = rstd::env::var("VR_TEST_VIDEO_HWDEC"_str).unwrap();
    array<vk::Extension, 13> extensions {
        vk::Extension { true, "VK_KHR_external_memory_fd"_str },
        vk::Extension { true, "VK_KHR_external_semaphore_fd"_str },
        vk::Extension { false, "VK_KHR_synchronization2"_str },
        vk::Extension { false, "VK_EXT_external_memory_dma_buf"_str },
        vk::Extension { false, "VK_KHR_image_format_list"_str },
        vk::Extension { false, "VK_EXT_image_drm_format_modifier"_str },
        vk::Extension { false, "VK_EXT_queue_family_foreign"_str },
        vk::Extension { false, "VK_KHR_video_queue"_str },
        vk::Extension { false, "VK_KHR_video_decode_queue"_str },
        vk::Extension { false, "VK_KHR_video_decode_h264"_str },
        vk::Extension { false, "VK_KHR_video_decode_h265"_str },
        vk::Extension { false, "VK_EXT_descriptor_buffer"_str },
        vk::Extension { false, "VK_EXT_shader_object"_str },
    };
    auto session = vk::DeviceSession::create({ .device_extensions = extensions.as_slice(),
                                               .api_version       = VK_API_VERSION_1_3,
                                               .validation        = true })
                       .unwrap();
    auto context = vk::TextureContext::create(session).unwrap();
    media::MediaRegistry media(session, { hwdec.clone(), {} });
    auto                 physical_path = rstd::path::PathBuf::from(path->as_str());
    auto                 file          = rstd::fs::File::open(physical_path.as_path()).unwrap();
    auto                 size          = file.metadata().unwrap().len();
    auto                 source        = rstd::io::SharedReadAt::make(rstd::move(file));
    auto                 owned_image   = Arc<vrento::Image>::make();
    auto&                image         = *owned_image;
    image.header.kind                  = vrento::ImageKind::Video;
    image.key                          = "playback"_Str;
    vrento::Image::Slot slot;
    slot.width  = rstd::from_str<i32>(width.as_str()).unwrap().to_primitive();
    slot.height = rstd::from_str<i32>(height.as_str()).unwrap().to_primitive();
    vrento::ImageData data;
    data.width        = slot.width;
    data.height       = slot.height;
    data.video_source = Some(rstd::io::ReadRange::make(rstd::move(source), u64(), size).unwrap());
    slot.mipmaps.push(rstd::move(data));
    image.slots.push(rstd::move(slot));
    auto control     = Arc<Playback>::make();
    auto playback    = Arc<dyn<vrento::VideoPlayback>>::make(control.clone());
    auto preparation = media.Prepare(context, image, Some(playback.clone()));
    if (preparation.is_err())
        rstd::io::eprintln("media preparation failed: {}", preparation.as_ref().unwrap_err());
    auto prepared = rstd::move(preparation).unwrap();
    auto identity = prepared.view.texture().identity();
    auto initial  = context.Begin().unwrap();
    for (auto& upload : prepared.uploads) initial.commands()->Upload(rstd::move(upload)).unwrap();
    initial.commands()
        ->UseSampledView(prepared.view, prepared.sampler, tex::Stages::Fragment)
        .unwrap();
    context.wait(context.Submit(rstd::move(initial)).unwrap()).unwrap();
    auto expected = hwdec.as_str() == "none"_str     ? media::MediaSourceKind::Software
                    : hwdec.as_str() == "vulkan"_str ? media::MediaSourceKind::VulkanShared
                                                     : media::MediaSourceKind::VaapiDrm;
    rstd_assert(prepared.producer->SourceKind() == expected);
    auto tick = [&] {
        prepared.producer->Pump(context, 1.0 / 30.0).unwrap();
        vrento::fg::FrameGraphBuilder builder(1);
        auto                          input = builder.import_resource({ .id = { 1, 1 } }).unwrap();
        auto                          id    = builder.add_pass({ 1 }, { 1 }).unwrap();
        auto                  use = builder.read(id, input, Some(vk::GraphRange {})).unwrap();
        vk::PassExecutionInfo pass { .id = id, .kind = vk::texture_sampled_kind, .queue = { 1 } };
        pass.uses.push({ input.resource,
                         input.version,
                         vk::GraphAccess::Read,
                         vk::PassUsage::Sampled,
                         vk::PassStages::Fragment,
                         {},
                         use });
        Vec<vk::PassExecutionInfo> passes;
        passes.push(rstd::move(pass));
        auto set =
            vk::VulkanPassSet::associate(rstd::move(builder).finish().unwrap(), rstd::move(passes))
                .unwrap();
        vk::GraphBindings bindings(1);
        bindings
            .bind(input.resource,
                  Arc<dyn<vk::GraphBinding>>::make(vk::GraphTextureBinding(prepared.view)))
            .unwrap();
        vk::VulkanPassBackends backends;
        vk::register_texture_passes(backends).unwrap();
        auto graph = vk::PreparedGraphProgram::prepare(session, rstd::move(set), bindings, backends)
                         .unwrap();
        auto sampled = context.Begin().unwrap();
        graph.record(sampled).unwrap();
        context.wait(context.Submit(rstd::move(sampled)).unwrap()).unwrap();
        context.Poll().unwrap();
        rstd_assert(prepared.view.texture().identity() == identity);
    };
    for (unsigned frame {}; frame < 12; ++frame) tick();
    rstd_assert(prepared.producer->ContentRevision() != u64());
    control->state.lock().unwrap()->snapshot.playing = false;
    auto paused                                      = prepared.producer->ContentRevision();
    for (unsigned frame {}; frame < 4; ++frame) tick();
    rstd_assert(prepared.producer->ContentRevision() == paused);
    {
        auto state              = control->state.lock().unwrap();
        state->snapshot.playing = true;
        ++state->snapshot.seek_sequence;
        state->snapshot.seek_seconds = *state->duration - f64(0.2);
    }
    bool looped {};
    for (unsigned frame {}; frame < 30; ++frame) {
        tick();
        if (control->state.lock().unwrap()->current < f64(1.0)) looped = true;
    }
    rstd_assert(looped);
    media.Drain(u64(5'000'000'000)).unwrap();
    media.Clear();
    vrento::resource_registry::ResourceRegistries registries;
    registries.TryInitialize(session).unwrap();
    auto& textures = registries.Textures();
    textures
        .SetProducerFactory(Arc<dyn<vrento::resource_registry::TextureProducerFactory>>::make(
            owe::video::WavsenTextureFactory(session, { hwdec.clone(), {} })))
        .unwrap();
    auto imported = textures.CreateImportedTexture(owned_image.clone(), None()).unwrap();
    textures.Flush().unwrap();
    textures.WaitForUploads(5'000'000'000).unwrap();
    for (unsigned frame {}; frame < 4; ++frame)
        imported.allocation->Pump(textures.Context(), 1.0 / 30.0).unwrap();
    textures.ClearProducers();
    imported.allocation->Pump(textures.Context(), 1.0 / 30.0).unwrap();
    textures.DrainProducers(u64(5'000'000'000)).unwrap();
    registries.Reset();
    imported.allocation.reset();
    context.Poll().unwrap();
    rstd_assert(session.validation_errors() == 0);
}
TEST(MediaRegistry, OpenFailureOwnsDecoderDiagnostic) {
    array<vk::Extension, 2> extensions { vk::Extension { true, "VK_KHR_external_memory_fd"_str },
                                         vk::Extension { true,
                                                         "VK_KHR_external_semaphore_fd"_str } };
    auto session = vk::DeviceSession::create({ .device_extensions = extensions.as_slice(),
                                               .api_version       = VK_API_VERSION_1_3,
                                               .validation        = true })
                       .unwrap();
    auto context = vk::TextureContext::create(session).unwrap();
    media::MediaRegistry media(session, { "none"_Str, {} });
    auto                 dir = rstd::fs::TempDir::make("vrento-invalid-video"_str).unwrap();
    auto                 path =
        rstd::path::PathBuf::from(dir.path().as_os_str().to_os_string()).join("invalid.bin"_str);
    rstd::fs::write(path.as_path(), "not a video"_str.as_bytes()).unwrap();
    auto          file = rstd::fs::File::open(path.as_path()).unwrap();
    auto          size = file.metadata().unwrap().len();
    vrento::Image input;
    input.header.kind = vrento::ImageKind::Video;
    input.key         = "invalid-video"_Str;
    vrento::Image::Slot slot;
    slot.width  = 16;
    slot.height = 16;
    vrento::ImageData data;
    data.width  = 16;
    data.height = 16;
    data.video_source =
        Some(rstd::io::ReadRange::make(rstd::io::SharedReadAt::make(rstd::move(file)), u64(), size)
                 .unwrap());
    slot.mipmaps.push(rstd::move(data));
    input.slots.push(rstd::move(slot));
    auto failed = media.Prepare(context, input);
    ASSERT_TRUE(failed.is_err());
    auto error = failed.unwrap_err();
    EXPECT_EQ(error.operation, media::MediaOperation::Open);
    ASSERT_TRUE(error.cause.is_Decoder());
    EXPECT_FALSE(error.cause.as_Decoder().message.is_empty());
    auto failure = media::ResourceFailure(rstd::move(error));
    media.Clear();
    auto source = failure.source->as_External().source.deref();
    auto saved  = rstd::error::downcast_ref<media::MediaError>(source);
    ASSERT_TRUE(saved.is_some());
    EXPECT_FALSE((*saved)->cause.as_Decoder().message.is_empty());
    EXPECT_EQ(session.validation_errors(), 0u);
}

} // namespace owe_test::video
