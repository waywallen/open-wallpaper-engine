#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>
#include <unistd.h>
import rstd;
import rstd.test;
import rstd.log;
import wescene.scene;
import wescene.types;
import wescene.vulkan;
import vrento;
import wescene.vulkan_render;
import wescene.scene_render_pipeline;
import wescene.pkg.parse;
import wescene.json;
import wescene.fs;
import wescene.text;
import wavsen.audio;
import vrento;

using namespace rstd::prelude;
using namespace rstd::literals;

namespace
{
namespace vk  = vrento::vulkan;
namespace tex = vrento::texture;
struct OutputCounts {
    unsigned                          acquired {}, submitted {}, aborted {};
    unsigned                          width { 64 }, height { 64 };
    bool                              reject_next {};
    vrento::FrameSurfaceAcquireStatus acquire_status {
        vrento::FrameSurfaceAcquireStatus::Acquired
    };
};
class OutputSurface : public vrento::ExSwapchain {
    vk::DeviceSession             session_;
    vvk::AllocatedImage           image_;
    vvk::ImageView                view_;
    rstd::uint64_t                serial_ {};
    bool                          defined_ {};
    unsigned                      width_ {}, height_ {};
    rstd::sync::Arc<OutputCounts> counts_;

public:
    explicit OutputSurface(vk::DeviceSession session, rstd::sync::Arc<OutputCounts> counts =
                                                          rstd::sync::Arc<OutputCounts>::make())
        : session_(session), counts_(rstd::move(counts)) {}
    void resize() {
        if (width_ == counts_->width && height_ == counts_->height) return;
        VkImageCreateInfo info { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format    = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent    = { counts_->width, counts_->height, 1 };
        info.mipLevels = info.arrayLayers = 1;
        info.samples                      = VK_SAMPLE_COUNT_1_BIT;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        auto image = session_.device().memory_allocator().create_image(info).unwrap();
        VkImageViewCreateInfo view { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image            = image.handle();
        view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        view.format           = info.format;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        rstd_assert(session_.device().handle().CreateImageView(view, view_) == VK_SUCCESS);
        image_   = rstd::move(image);
        width_   = counts_->width;
        height_  = counts_->height;
        defined_ = false;
    }
    unsigned width() const override { return counts_->width; }
    unsigned height() const override { return counts_->height; }
    VkFormat format() const override { return VK_FORMAT_R8G8B8A8_UNORM; }
    bool     ready() const override { return true; }
    void     setOnReadyChanged(Option<vrento::ExSwapchainReadyCallback>) override {}
    auto     acquireRenderTarget() -> vrento::FrameSurfaceAcquireResult override {
        if (counts_->acquire_status != vrento::FrameSurfaceAcquireStatus::Acquired)
            return { .status = counts_->acquire_status };
        resize();
        ++counts_->acquired;
        vrento::FrameSurfaceIdentity identity { u64(1), u32(), u64(++serial_) };
        vrento::FrameSurfaceLease    lease;
        lease.identity             = identity;
        lease.image.handle         = image_.handle();
        lease.image.view           = *view_;
        lease.image.extent         = { width_, height_, 1 };
        lease.format               = format();
        lease.initial_layout       = defined_ ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        lease.initial_queue_family = lease.final_queue_family =
            session_.device().graphics_queue().family_index;
        lease.final_layout = rstd::exchange(counts_->reject_next, false) ? VK_IMAGE_LAYOUT_UNDEFINED
                                                                         : VK_IMAGE_LAYOUT_GENERAL;
        return { .status     = vrento::FrameSurfaceAcquireStatus::Acquired,
                 .lease      = lease,
                 .completion = MakeCompletionCapability(identity) };
    }

private:
    auto CompleteRendered(vrento::FrameSurfaceIdentity identity, int fd)
        -> vrento::FrameSurfaceCompletionResult override {
        if (fd >= 0) ::close(fd);
        defined_ = true;
        ++counts_->submitted;
        return { .status = vrento::FrameSurfaceCompletionStatus::Submitted, .identity = identity };
    }
    auto AbortRenderTarget(vrento::FrameSurfaceIdentity identity)
        -> vrento::FrameSurfaceCompletionResult override {
        ++counts_->aborted;
        return { .status = vrento::FrameSurfaceCompletionStatus::Aborted, .identity = identity };
    }
};
auto pixels(vk::TextureContext& queue, tex::TextureViewRef view) -> Vec<u8> {
    auto size                  = view.texture().description().extent;
    auto range                 = view.description().range;
    size.width                 = rstd::cmp::max(1u, size.width >> range.first_mip);
    size.height                = rstd::cmp::max(1u, size.height >> range.first_mip);
    auto               session = queue.session();
    VkBufferCreateInfo info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size     = size.width * size.height * 4;
    info.usage    = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto readback = session.device()
                        .memory_allocator()
                        .create_buffer(info, vvk::MemoryRequest::Readback())
                        .unwrap();
    auto work     = queue.Begin().unwrap();
    work.commands()
        ->UseTexture(
            { view, tex::Access::TransferSource, tex::Stages::Transfer, false, None(), true })
        .unwrap();
    auto              command = vk::native_recording(work, session).unwrap();
    VkBufferImageCopy copy {};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, range.first_mip, range.first_layer, 1 };
    copy.imageExtent      = { size.width, size.height, 1 };
    auto& d               = session.device().handle().Dispatch();
    d.vkCmdCopyImageToBuffer(command,
                             vk::native_texture(view.texture()).unwrap(),
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             readback.handle(),
                             1,
                             &copy);
    VkBufferMemoryBarrier host { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    host.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer                                         = readback.handle();
    host.size                                           = VK_WHOLE_SIZE;
    d.vkCmdPipelineBarrier(command,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT,
                           0,
                           0,
                           nullptr,
                           1,
                           &host,
                           0,
                           nullptr);
    queue.wait(queue.Submit(rstd::move(work)).unwrap()).unwrap();
    auto memory = readback.allocation();
    auto mapped = memory.map().unwrap();
    memory.invalidate().unwrap();
    Vec<u8> result;
    for (VkDeviceSize i {}; i < info.size; ++i)
        result.push(u8(static_cast<unsigned char*>(mapped.data())[i]));
    return result;
}
TEST(RenderOutput, ClearAndResize) {
    static rstd::log::EnvLogger logger;
    rstd::log::set_logger(logger);
    rstd::log::set_max_level(logger.filter());
    owe::Scene scene;
    scene.RegisterRenderTarget(
        String::make("_rt_default"_str),
        owe::SceneRenderTarget {
            .width = i32(64), .height = i32(64), .bind = { .enable = true, .screen = true } });
    scene.RegisterRenderTarget(
        "_rt_copy"_Str,
        owe::SceneRenderTarget {
            .width = i32(64), .height = i32(64), .has_mipmap = true, .mipmap_level = 3 });
    scene.RegisterRenderTarget("_rt_seen"_Str,
                               owe::SceneRenderTarget { .width = i32(64), .height = i32(64) });
    scene.RegisterRenderTarget("_rt_uninitialized"_Str,
                               owe::SceneRenderTarget { .width = i32(64), .height = i32(64) });
    scene.SetClearColor({ 1.0f, 0.0f, 0.0f });
    owe::vulkan::VulkanRender renderer;
    owe::RenderInitInfo       info;
    info.offscreen            = true;
    info.enable_valid_layer   = true;
    info.width                = 64;
    info.height               = 64;
    info.msaa_samples         = 4;
    info.pipeline_preparation = vk::PipelinePreparationMode::Deferred;
    auto counts               = rstd::sync::Arc<OutputCounts>::make();
    auto factory =
        [&](const owe::RenderInitInfo::ExSwapchainHandles&) -> Option<vrento::ExSwapchainOwner> {
        return Some(vrento::ExSwapchain::Make<OutputSurface>(renderer.deviceSession().unwrap(),
                                                             counts.clone()));
    };
    info.ex_swapchain_factory =
        Some(Box<dyn<FnOnce<Option<vrento::ExSwapchainOwner>(
                 const owe::RenderInitInfo::ExSwapchainHandles&)>>>::make(rstd::move(factory)));
    rstd_assert(renderer.init(rstd::move(info)));
    auto session = renderer.deviceSession().unwrap();
    auto queue   = vk::TextureContext::create(session).unwrap();
    auto build   = [&](bool uninitialized = false) {
        vrento::fg::FrameGraphBuilder    graph(81);
        owe::vulkan::SceneRenderTextures textures(u64(81));
        Vec<vk::PassInput>               descriptions;
        auto copy = [&](vrento::fg::PassId pass,
                        ref<str>
                            from,
                        ref<str>
                                                            to,
                        Option<vrento::fg::ResourceVersion> written = None()) {
            auto source = textures
                              .declare(graph,
                                       { .key        = String::make(from),
                                         .definition = scene.TargetResource(from).unwrap(),
                                         .initialize_history_transparent = true })
                              .unwrap();
            if (written)
                source = *written;
            else
                graph.import_initial(source.resource).unwrap();
            auto initial = textures
                               .declare(graph,
                                        { .key        = String::make(to),
                                          .definition = scene.TargetResource(to).unwrap(),
                                          .initialize_history_transparent = true })
                               .unwrap();
            auto description =
                vk::declare_texture_copy(graph, pass, source, initial, {}, {}).unwrap();
            auto target = description.output;
            descriptions.push(rstd::move(description.description));
            (void)textures.handle(source).unwrap();
            (void)textures.handle(target).unwrap();
            return target;
        };
        copy({ 1 }, uninitialized ? "_rt_uninitialized"_str : "_rt_copy"_str, "_rt_seen"_str);
        auto result = textures
                          .declare(graph,
                                   { .key        = "_rt_default"_Str,
                                     .definition = scene.TargetResource("_rt_default"_str).unwrap(),
                                     .initialize_history_transparent = true })
                          .unwrap();
        auto clear_id = vrento::fg::PassId { 10 };
        auto color    = scene.ClearColor();
        auto clear =
            vk::declare_graphics_clear(
                graph, clear_id, result, { color[usize()], color[usize(1)], color[usize(2)], 1.0f })
                .unwrap();
        auto cleared    = result;
        cleared.version = clear.execution().graphics->color->version;
        (void)textures.handle(cleared).unwrap();
        descriptions.push(rstd::move(clear));
        auto copied = copy({ 2 }, "_rt_default"_str, "_rt_copy"_str, Some(cleared));
        graph.depends_on({ 2 }, { 1 }).unwrap();
        auto mip =
            vk::declare_texture_mips(
                graph, { 3 }, copied, scene.RenderTarget("_rt_copy"_str).unwrap()->mipmap_level, 1)
                .unwrap();
        (void)textures.handle(mip.output).unwrap();
        descriptions.push(rstd::move(mip.description));
        auto surface =
            graph.import_resource({ .id = { 20, 81 }, .spec = 20, .storage = 20 }).unwrap();
        auto output = vk::declare_surface_output(graph, { 11 }, { 12 }, cleared, surface).unwrap();
        descriptions.push(rstd::move(output.transfer));
        descriptions.push(rstd::move(output.release));
        auto source_use = textures.handle(cleared).unwrap();
        return owe::vulkan::SceneRenderStateInput { rstd::move(graph).finish().unwrap(),
                                                    rstd::move(textures),
                                                    {},
                                                    rstd::move(descriptions),
                                                    { 11 },
                                                    { 12 },
                                                    output.output,
                                                    Some(source_use) };
    };
    renderer.configureRenderTargets(scene);
    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
    auto install  = [&](bool uninitialized = false) {
        auto result = renderer.tryInstallSceneRenderState(scene, build(uninitialized), snapshot);
        for (unsigned attempt {};
             result.is_ok() && *result == owe::PrepareOutcome::Pending && attempt < 2000;
             ++attempt) {
            rstd::thread::sleep(rstd::time::Duration::from_millis(u64(1)));
            result = renderer.tryContinuePrepare(scene);
        }
        rstd_assert(result.is_ok() && *result == owe::PrepareOutcome::Ready);
    };
    auto draw = [&] {
        renderer.drawFrame(scene);
        for (unsigned attempt {}; renderer.framePending() && attempt < 2000; ++attempt) {
            rstd::thread::sleep(rstd::time::Duration::from_millis(u64(1)));
            renderer.drawFrame(scene);
        }
        rstd_assert(! renderer.framePending());
    };
    install(true);
    draw();
    rstd_assert(! renderer.framePending() && ! renderer.frameSubmitted());
    auto failure = renderer.diagnostics().frame_error;
    rstd_assert(failure && failure->stage == vk::GraphExecutionStage::Binding);
    rstd_assert(failure->resource.valid());
    rstd_assert(failure->texture && failure->texture->kind == tex::ErrorKind::Invalid);
    rstd_assert(counts->acquired == 1 && counts->aborted == 1 && counts->submitted == 0);
    install();
    rstd_assert(! renderer.renderedTarget("_rt_default"_str).unwrap().ready());
    draw();
    auto old   = renderer.renderedTarget("_rt_default"_str).unwrap().take_ready();
    auto first = pixels(queue, old);
    for (usize i {}; i < first.len(); ++i)
        rstd_assert(first[i] ==
                    u8(i.to_primitive() % 4 == 0 || i.to_primitive() % 4 == 3 ? 255 : 0));
    auto initial = pixels(queue, renderer.renderedTarget("_rt_seen"_str).unwrap().take_ready());
    for (auto byte : initial) rstd_assert(byte == u8());
    scene.SetClearColor({ 0.0f, 1.0f, 0.0f });
    install();
    draw();
    auto second = pixels(queue, renderer.renderedTarget("_rt_default"_str).unwrap().take_ready());
    for (usize i {}; i < second.len(); ++i)
        rstd_assert(second[i] ==
                    u8(i.to_primitive() % 4 == 1 || i.to_primitive() % 4 == 3 ? 255 : 0));
    auto history = pixels(queue, renderer.renderedTarget("_rt_seen"_str).unwrap().take_ready());
    for (usize i {}; i < history.len(); ++i)
        rstd_assert(history[i] ==
                    u8(i.to_primitive() % 4 == 0 || i.to_primitive() % 4 == 3 ? 255 : 0));
    auto copied = renderer.renderedTarget("_rt_copy"_str).unwrap().take_ready();
    auto levels = copied.texture().description().mip_levels;
    rstd_assert(levels > 1);
    for (unsigned mip {}; mip < levels; ++mip) {
        auto view = queue.CreateView(copied.texture(), { .range = { .first_mip = mip } }).unwrap();
        auto data = pixels(queue, view);
        for (usize i {}; i < data.len(); ++i)
            rstd_assert(data[i] ==
                            u8(i.to_primitive() % 4 == 1 || i.to_primitive() % 4 == 3 ? 255 : 0),
                        "mip {} byte {} = {}",
                        mip,
                        i,
                        u8(data[i]));
    }
    counts->width = 96;
    rstd_assert(renderer.onSwapchainReady(96, 64));
    rstd_assert(scene.ResizeRenderTarget("_rt_copy"_str, i32(96), i32(64)));
    rstd_assert(scene.ResizeRenderTarget("_rt_seen"_str, i32(96), i32(64)));
    renderer.configureRenderTargets(scene);
    install();
    draw();
    auto resized = renderer.renderedTarget("_rt_default"_str).unwrap().take_ready();
    rstd_assert(resized.texture().description().extent.width == 96);
    auto final = pixels(queue, resized);
    for (usize i {}; i < final.len(); ++i)
        rstd_assert(final[i] ==
                    u8(i.to_primitive() % 4 == 1 || i.to_primitive() % 4 == 3 ? 255 : 0));
    auto resized_history =
        pixels(queue, renderer.renderedTarget("_rt_seen"_str).unwrap().take_ready());
    for (auto byte : resized_history) rstd_assert(byte == u8());
    rstd_assert(counts->submitted == 3 && counts->aborted == 1);
    counts->reject_next = true;
    draw();
    rstd_assert(! renderer.frameSubmitted() && ! renderer.framePending());
    rstd_assert(counts->aborted == 2 && counts->submitted == 3);
    install();
    draw();
    rstd_assert(renderer.frameSubmitted() && ! renderer.framePending());
    rstd_assert(! renderer.diagnostics().frame_error);
    rstd_assert(counts->acquired == 6 && counts->submitted == 4 && counts->aborted == 2);
    renderer.destroy();
    rstd_assert(counts->aborted == 2);
    rstd_assert(! renderer.diagnostics().frame_error);
    rstd_assert(session.validation_errors() == 0);
}
TEST(RenderOutput, ShaderDirtyAndTopology) {
    auto         document = owe::wpscene::ParseSceneDocumentJson(R"({
        "camera": {},
        "general": {"orthogonalprojection": {"width": 64, "height": 64},
                    "clearcolor": "0 0 1"},
        "objects": [{"id": 1, "name": "tile", "visible": true,
                     "image": "models/util/solidlayer.json", "size": "64 64",
                     "origin": "32 32 0", "color": "1 0 0", "alpha": 1}]
    })"_str,
                                                                 owe::wpscene::kSceneVersionUnknown)
                                .unwrap();
    owe::fs::VFS vfs;
    vfs.mount("/assets"_str, owe::fs::make_physical_fs(owe::fs::Path("assets"_str)).unwrap())
        .unwrap();
    wavsen::audio::SoundManager sound;
    owe::SceneParser            parser;
    auto  parsed = parser
                       .Parse("render-output"_str,
                              ref<owe::wpscene::SceneDocument>::from_raw_parts(&document),
                              mut_ref<owe::fs::VFS>::from_raw_parts(&vfs),
                              mut_ref<wavsen::audio::SoundManager>::from_raw_parts(&sound))
                       .unwrap();
    auto& scene  = *parsed.scene;
    scene.RenderTargetMut("_rt_default"_str).unwrap()->withDepth = true;
    owe::vulkan::VulkanRender renderer;
    owe::RenderInitInfo       info;
    info.offscreen          = true;
    info.enable_valid_layer = true;
    info.width = info.height  = 64;
    info.msaa_samples         = 4;
    info.pipeline_preparation = vk::PipelinePreparationMode::Deferred;
    auto counts               = rstd::sync::Arc<OutputCounts>::make();
    auto factory =
        [&](const owe::RenderInitInfo::ExSwapchainHandles&) -> Option<vrento::ExSwapchainOwner> {
        return Some(vrento::ExSwapchain::Make<OutputSurface>(renderer.deviceSession().unwrap(),
                                                             counts.clone()));
    };
    info.ex_swapchain_factory =
        Some(Box<dyn<FnOnce<Option<vrento::ExSwapchainOwner>(
                 const owe::RenderInitInfo::ExSwapchainHandles&)>>>::make(rstd::move(factory)));
    owe::SceneRenderPipeline pipeline(scene, *parsed.runtime_input, renderer);
    unsigned                 evaluations {};
    scene.RegisterTransformUpdater(Box<dyn<FnMut<void(f64)>>>::make([&](f64) {
        ++evaluations;
    }));
    EXPECT_TRUE(pipeline.rebuild().is_err());
    auto initial_failure = pipeline.diagnostics().last_failure;
    ASSERT_TRUE(initial_failure);
    rstd_assert(renderer.init(rstd::move(info)));
    auto queue = vk::TextureContext::create(renderer.deviceSession().unwrap()).unwrap();

    auto ready = [&](Result<owe::PrepareOutcome, owe::ScenePrepareError> result) {
        for (unsigned i {}; result.is_ok() && *result == owe::PrepareOutcome::Pending && i < 2000;
             ++i) {
            ::usleep(1000);
            result = pipeline.retryPreparation();
        }
        rstd_assert(result.is_ok());
        rstd_assert(pipeline.ready());
        EXPECT_EQ(pipeline.stats().preparation_failures, 1u);
        auto retained = pipeline.diagnostics().last_failure;
        EXPECT_EQ((*retained)->attempt(), (*initial_failure)->attempt());
    };
    auto color = [&](unsigned red, unsigned green, unsigned blue) {
        auto data = pixels(queue, renderer.renderedTarget("_rt_default"_str).unwrap().take_ready());
        auto index = usize((32 * 64 + 32) * 4);
        ASSERT_EQ(data[index], u8(red));
        ASSERT_EQ(data[index + usize(1)], u8(green));
        ASSERT_EQ(data[index + usize(2)], u8(blue));
    };
    auto prepare = [&] {
        ready(pipeline.prepare({ .frametime = 1.0f / 60.0f }));
    };
    auto draw = [&] {
        renderer.drawFrame(scene);
        ASSERT_TRUE(renderer.frameSubmitted());
        ASSERT_TRUE(pipeline.acknowledgeFrameSubmitted());
        ASSERT_FALSE(pipeline.acknowledgeFrameSubmitted());
        scene.PassFrameTime(1.0 / 60.0);
    };
    ready(pipeline.rebuild());
    prepare();
    auto       waiting_uploads = renderer.diagnostics().resources.buffer_uploads;
    const auto waiting_time    = scene.Runtime().Frame().elapsed;
    for (unsigned retry {}; retry < 2000; ++retry) {
        counts->acquire_status = retry % 2 ? vrento::FrameSurfaceAcquireStatus::Busy
                                           : vrento::FrameSurfaceAcquireStatus::NotReady;
        ASSERT_EQ(pipeline.retryPreparation().unwrap(), owe::PrepareOutcome::Ready);
        ASSERT_EQ(pipeline.prepare({ .frametime = 99 }).unwrap(), owe::PrepareOutcome::Ready);
        renderer.drawFrame(scene);
        ASSERT_FALSE(renderer.frameSubmitted());
        ASSERT_TRUE(pipeline.pending().frame);
    }
    EXPECT_EQ(evaluations, 1u);
    EXPECT_EQ(pipeline.stats().prepared_frames, 1u);
    EXPECT_EQ(scene.Runtime().Frame().elapsed, waiting_time);
    for (unsigned category {}; category < 5; ++category)
        EXPECT_EQ(renderer.diagnostics().resources.buffer_uploads.queued_bytes[category],
                  waiting_uploads.queued_bytes[category]);
    counts->acquire_status = vrento::FrameSurfaceAcquireStatus::Acquired;
    draw();
    color(255, 0, 0);
    auto before = pipeline.stats();
    rstd_assert(before.graph_builds == 1);
    auto  pipeline_creations = queue.session().graphics_resources().pipelines.stats().creations;
    auto* node               = scene.RootMut()->FindByName("tile"_str);
    rstd_assert(node && node->Mesh());
    auto* material    = node->Mesh()->Material();
    auto  diagnostics = renderer.diagnostics();
    auto& vertices    = node->Mesh()->Submeshes()[usize()].vertex_arrays[usize()];
    EXPECT_FALSE(node->Mesh()->Dynamic());
    auto same_bytes =
        Vec<float>::from(slice<float>::from_raw_parts(vertices.Data(), vertices.DataSize()));
    EXPECT_TRUE(vertices.SetVertexs(usize(), same_bytes.as_slice()));
    node->Mesh()->SetDirty(owe::SceneMeshDirtyData);
    prepare();
    draw();
    color(255, 0, 0);
    EXPECT_EQ(pipeline.stats().graph_builds, before.graph_builds);
    EXPECT_EQ(pipeline.stats().graph_analysis.attempts, before.graph_analysis.attempts);
    EXPECT_EQ(renderer.diagnostics().renderer.graph_compiles, diagnostics.renderer.graph_compiles);
    EXPECT_EQ(renderer.diagnostics().pipelines.creations, diagnostics.pipelines.creations);
    EXPECT_EQ(renderer.diagnostics().resources.buffer_allocations,
              diagnostics.resources.buffer_allocations);
    auto vertex_upload = static_cast<unsigned>(vk::BufferUploadClass::Vertex);
    EXPECT_GT(renderer.diagnostics().resources.buffer_uploads.queued_copies[vertex_upload],
              diagnostics.resources.buffer_uploads.queued_copies[vertex_upload]);
    EXPECT_GT(renderer.diagnostics().renderer.execution.frame_preparations,
              diagnostics.renderer.execution.frame_preparations);
    const float green[] { 0, 1, 0 };
    rstd_assert(
        scene.SetMaterialShaderValue(*material, "g_Color"_str, owe::ShaderValue(green, usize(3))));
    prepare();
    draw();
    color(0, 255, 0);
    rstd_assert(pipeline.stats().graph_builds == before.graph_builds);
    rstd_assert(queue.session().graphics_resources().pipelines.stats().creations ==
                pipeline_creations);
    rstd::io::println("owe uniform refresh: graph builds={}, pipelines={}",
                      before.graph_builds,
                      pipeline_creations);
    node->SetUserAlpha(0);
    prepare();
    draw();
    color(0, 0, 255);
    node->SetUserAlpha(1);
    auto changed       = material->Pipeline();
    changed.depth_test = changed.depth_write = true;
    rstd_assert(material->SetPipeline(changed));
    prepare();
    draw();
    color(0, 255, 0);
    rstd_assert(pipeline.stats().graph_builds > before.graph_builds);

    prepare();
    const auto resize_evaluations = evaluations;
    const auto resize_frames      = pipeline.stats().prepared_frames;
    const auto resize_time        = scene.Runtime().Frame().elapsed;
    counts->acquire_status        = vrento::FrameSurfaceAcquireStatus::Busy;
    renderer.drawFrame(scene);
    ASSERT_FALSE(renderer.frameSubmitted());
    counts->width = 96;
    (void)pipeline.resize(96, 64);
    ready(pipeline.retryPreparation());
    ASSERT_TRUE(pipeline.pending().frame);
    EXPECT_EQ(evaluations, resize_evaluations);
    EXPECT_EQ(pipeline.stats().prepared_frames, resize_frames);
    EXPECT_EQ(scene.Runtime().Frame().elapsed, resize_time);
    counts->acquire_status = vrento::FrameSurfaceAcquireStatus::Acquired;
    draw();
    auto resized = pixels(queue, renderer.renderedTarget("_rt_default"_str).unwrap().take_ready());
    EXPECT_EQ(resized.len(), usize(96 * 64 * 4));
    EXPECT_EQ(resized[usize((32 * 96 + 48) * 4 + 1)], u8(255));
    renderer.destroy();
    rstd_assert(queue.session().validation_errors() == 0);
}
TEST(RenderOutput, IgnoresInactiveSamplerContentsAndFeedback) {
    auto         document = owe::wpscene::ParseSceneDocumentJson(R"({
        "camera": {},
        "general": {"orthogonalprojection": {"width": 64, "height": 64},
                    "clearcolor": "0 0 1"},
        "objects": [{"id": 1, "name": "tile", "visible": true,
                     "image": "models/util/solidlayer.json", "size": "64 64",
                     "origin": "32 32 0", "color": "1 0 0", "alpha": 1}]
    })"_str,
                                                                 owe::wpscene::kSceneVersionUnknown)
                                .unwrap();
    owe::fs::VFS vfs;
    vfs.mount("/assets"_str, owe::fs::make_physical_fs(owe::fs::Path("assets"_str)).unwrap())
        .unwrap();
    wavsen::audio::SoundManager sound;
    auto  parsed = owe::SceneParser()
                       .Parse("inactive-sampler"_str,
                              ref<owe::wpscene::SceneDocument>::from_raw_parts(&document),
                              mut_ref<owe::fs::VFS>::from_raw_parts(&vfs),
                              mut_ref<wavsen::audio::SoundManager>::from_raw_parts(&sound))
                       .unwrap();
    auto& scene  = *parsed.scene;
    scene.RegisterRenderTarget("_rt_undefined"_Str,
                               owe::SceneRenderTarget { .width = i32(64), .height = i32(64) });
    auto* tile = scene.RootMut()->FindByName("tile"_str);
    ASSERT_TRUE(tile && tile->Mesh());
    auto* material = tile->Mesh()->Material();
    ASSERT_TRUE(material && material->customShader.shader);
    ASSERT_TRUE((**material->customShader.shader).SamplerMember(0).is_empty());
    auto                        backend = owe::vulkan::MakeShaderBackend();
    owe::vulkan::ShaderCompUnit fragment {
        .stage       = owe::ShaderType::FRAGMENT,
        .src         = R"(#version 450
            layout(set=2,binding=0,std140) uniform TextureSize { vec4 g_Texture0Resolution; };
            layout(location=0) out vec4 color;
            void main() { color = vec4(dot(g_Texture0Resolution, vec4(1)) / 256.0, 0, 0, 1); }
        )"_Str,
        .entry_point = "main"_Str,
    };
    Vec<owe::vulkan::Uni_ShaderSpv> compiled;
    ASSERT_TRUE(backend->CompileAndLinkShaderUnits(
        slice<owe::vulkan::ShaderCompUnit>::from_raw_parts(&fragment, usize(1)), {}, compiled));
    auto                            shader   = rstd::sync::Arc<owe::SceneShader>::make();
    const auto&                     original = **material->customShader.shader;
    Vec<owe::vulkan::Uni_ShaderSpv> stages;
    owe::vulkan::ShaderReflected    reflected;
    ASSERT_TRUE(backend->GenReflect(original.codes.as_slice(), stages, reflected));
    for (const auto& stage : stages)
        if (stage->stage != owe::ShaderType::FRAGMENT) shader->codes.push(stage->spirv.clone());
    shader->codes.push(rstd::move(compiled[usize()]->spirv));
    shader->attrs                 = original.attrs.clone();
    shader->uniform_blocks        = original.uniform_blocks.clone();
    shader->matrix_convention     = original.matrix_convention;
    shader->matrix_abi            = original.matrix_abi;
    material->customShader.shader = Some(rstd::move(shader));
    material->textures.clear();
    material->textures.push("_rt_undefined"_Str);
    material->textures.push("_rt_default"_Str);
    material->SetBlendMode(owe::BlendMode::Translucent);
    auto graph = owe::BuildSceneRenderState(scene).unwrap();
    for (const auto& desc : graph.descriptions) {
        EXPECT_NE(desc.execution().kind, vk::texture_copy_kind);
        if (auto draw = desc.get<vk::GraphicsDrawDesc>())
            EXPECT_TRUE((*draw)->resources.textures.is_empty());
    }
    EXPECT_EQ(graph.textures.find("_rt_undefined"_str), nullptr);
    ASSERT_EQ(graph.draws.len(), usize(1));
    const auto& slots = graph.draws[usize()].source->parameters().texture_bindings;
    ASSERT_EQ(slots.len(), usize(2));
    EXPECT_EQ(slots[usize()].name, "_rt_undefined"_str);
    EXPECT_FALSE(slots[usize()].use);
    ASSERT_TRUE(slots[usize()].request && slots[usize()].request->definition);
    EXPECT_EQ(slots[usize()].request->definition->width, i32(64));

    owe::vulkan::VulkanRender renderer;
    owe::RenderInitInfo       info;
    info.offscreen = info.enable_valid_layer = true;
    info.width = info.height = 64;
    auto counts              = rstd::sync::Arc<OutputCounts>::make();
    auto factory =
        [&](const owe::RenderInitInfo::ExSwapchainHandles&) -> Option<vrento::ExSwapchainOwner> {
        return Some(vrento::ExSwapchain::Make<OutputSurface>(renderer.deviceSession().unwrap(),
                                                             counts.clone()));
    };
    info.ex_swapchain_factory =
        Some(Box<dyn<FnOnce<Option<vrento::ExSwapchainOwner>(
                 const owe::RenderInitInfo::ExSwapchainHandles&)>>>::make(rstd::move(factory)));
    ASSERT_TRUE(renderer.init(rstd::move(info)));
    auto queue = vk::TextureContext::create(renderer.deviceSession().unwrap()).unwrap();
    owe::SceneRenderPipeline pipeline(scene, *parsed.runtime_input, renderer);
    ASSERT_TRUE(pipeline.rebuild().is_ok());
    for (unsigned frame {}; frame < 3; ++frame) {
        ASSERT_TRUE(pipeline.prepare({ .frametime = 1.0f / 60.0f }).is_ok());
        ASSERT_TRUE(pipeline.ready());
        renderer.drawFrame(scene);
        ASSERT_FALSE(renderer.diagnostics().frame_error);
        ASSERT_TRUE(renderer.frameSubmitted());
        ASSERT_TRUE(pipeline.acknowledgeFrameSubmitted());
        scene.PassFrameTime(1.0 / 60.0);
        auto data = pixels(queue, renderer.renderedTarget("_rt_default"_str).unwrap().take_ready());
        auto center = usize((32 * 64 + 32) * 4);
        EXPECT_EQ(data[center], u8(255));
        EXPECT_EQ(data[center + usize(1)], u8());
        EXPECT_EQ(data[center + usize(2)], u8());
    }
    EXPECT_EQ(counts->submitted, 3u);
    EXPECT_EQ(counts->aborted, 0u);
    renderer.destroy();
    EXPECT_EQ(queue.session().validation_errors(), 0u);
}

TEST(RenderOutput, DynamicFontUploads) {
    auto         document = owe::wpscene::ParseSceneDocumentJson(R"({
        "camera": {},
        "general": {"orthogonalprojection": {"width": 64, "height": 64}, "clearcolor": "0 0 1"},
        "objects": [{"id": 1, "name": "label", "visible": true, "origin": "32 32 0",
            "text": {"value": "A", "script": "export function update() { return engine.runtime < 0.02 ? 'A' : 'B'; }"},
            "font": "systemfont_monospace", "pointsize": 16, "color": "1 1 1"}]
    })"_str,
                                                                 owe::wpscene::kSceneVersionUnknown)
                                .unwrap();
    owe::fs::VFS vfs;
    vfs.mount("/assets"_str, owe::fs::make_physical_fs(owe::fs::Path("assets"_str)).unwrap())
        .unwrap();
    wavsen::audio::SoundManager sound;
    owe::SceneParser            parser;
    auto  parsed = parser
                       .Parse("render-output"_str,
                              ref<owe::wpscene::SceneDocument>::from_raw_parts(&document),
                              mut_ref<owe::fs::VFS>::from_raw_parts(&vfs),
                              mut_ref<wavsen::audio::SoundManager>::from_raw_parts(&sound))
                       .unwrap();
    auto& scene  = *parsed.scene;
    owe::vulkan::VulkanRender renderer;
    owe::RenderInitInfo       info;
    info.offscreen          = true;
    info.enable_valid_layer = true;
    info.width = info.height  = 64;
    info.msaa_samples         = 4;
    info.pipeline_preparation = vk::PipelinePreparationMode::Deferred;
    auto factory =
        [&](const owe::RenderInitInfo::ExSwapchainHandles&) -> Option<vrento::ExSwapchainOwner> {
        return Some(vrento::ExSwapchain::Make<OutputSurface>(renderer.deviceSession().unwrap()));
    };
    info.ex_swapchain_factory =
        Some(Box<dyn<FnOnce<Option<vrento::ExSwapchainOwner>(
                 const owe::RenderInitInfo::ExSwapchainHandles&)>>>::make(rstd::move(factory)));
    rstd_assert(renderer.init(rstd::move(info)));
    auto queue = vk::TextureContext::create(renderer.deviceSession().unwrap()).unwrap();
    owe::SceneRenderPipeline pipeline(scene, *parsed.runtime_input, renderer);
    auto ready = [&](Result<owe::PrepareOutcome, owe::ScenePrepareError> result) {
        for (unsigned i {}; result.is_ok() && *result == owe::PrepareOutcome::Pending && i < 2000;
             ++i) {
            ::usleep(1000);
            result = pipeline.retryPreparation();
        }
        rstd_assert(result.is_ok());
        rstd_assert(pipeline.ready());
    };
    auto* fonts = owe::text::SceneTextResources(scene);
    rstd_assert(fonts);
    auto dirty = [&] {
        usize count;
        for (const auto& page : fonts->Pages()) count += page->SnapshotDirtyUpdates().rects.len();
        return count;
    };
    ready(pipeline.rebuild());
    scene.PassFrameTime(1.0 / 60.0);
    ready(pipeline.prepare({ .frametime = 1.0f / 60.0f, .runtime = 0.01f }));
    renderer.drawFrame(scene);
    ASSERT_TRUE(pipeline.acknowledgeFrameSubmitted());
    auto first = pixels(queue, renderer.renderedTarget("_rt_default"_str).unwrap().take_ready());
    rstd_assert(dirty() == usize());
    usize ink;
    for (usize i {}; i < first.len(); i += usize(4))
        if (first[i] != u8()) ++ink;
    rstd_assert(ink > usize());
    scene.PassFrameTime(1.0 / 60.0);
    ready(pipeline.prepare({ .frametime = 1.0f / 60.0f, .runtime = 0.03f }));
    rstd_assert(dirty() > usize());
    renderer.drawFrame(scene);
    ASSERT_TRUE(pipeline.acknowledgeFrameSubmitted());
    auto second = pixels(queue, renderer.renderedTarget("_rt_default"_str).unwrap().take_ready());
    rstd_assert(first != second && dirty() == usize());
    renderer.destroy();
    rstd_assert(queue.session().validation_errors() == 0);
}
} // namespace
