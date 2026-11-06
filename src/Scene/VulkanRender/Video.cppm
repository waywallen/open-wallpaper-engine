module;
#include <rstd/macro.hpp>
#include <unistd.h>
#if __is_target_os(macos)
#    include <vulkan/vulkan.h>
#    include <vulkan/vulkan_metal.h>
#endif

export module wescene.video;
import vrento;
export import wescene.video_error;
import rstd;
import rstd.log;
import wavsen.video;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::ffi::CString;
using rstd::sync::Arc;
using rstd::sync::Weak;

using namespace vrento::vulkan;
using owe::video::MediaError;
using owe::video::MediaOperation;
using vrento::Image;
using vrento::ImageKind;
using vrento::VideoPlayback;
using vrento::VideoPlaybackSnapshot;
namespace tex = vrento::texture;

export namespace owe::video
{

struct MediaDecodeOptions {
    String hwdec { "auto"_Str };
    String render_node;
    auto   clone() const -> MediaDecodeOptions { return { hwdec.clone(), render_node.clone() }; }
};

enum class MediaSourceKind
{
    Unknown,
    Software,
    VulkanShared,
    VaapiDrm,
    VideoToolbox,
};

struct MediaTextureProducer {
    using Trait                  = MediaTextureProducer;
    static constexpr bool direct = false;
    template<typename Self, typename = void>
    struct Api {
        using Trait = MediaTextureProducer;
        auto Pump(TextureContext& context, double seconds) -> Result<bool, MediaError> {
            return rstd::trait_call<0>(this, context, seconds);
        }
        auto ContentRevision() const -> u64 { return rstd::trait_call<1>(this); }
        auto SourceKind() const -> MediaSourceKind { return rstd::trait_call<2>(this); }
    };
    template<typename T>
    using Funcs = rstd::TraitFuncs<&T::Pump, &T::ContentRevision, &T::SourceKind>;
};

struct PreparedMediaTexture {
    tex::TextureViewRef            view;
    tex::SamplerRef                sampler;
    Vec<tex::TextureUpload>        uploads;
    Arc<dyn<MediaTextureProducer>> producer;
};

class MediaRegistry {
public:
    struct State;
    explicit MediaRegistry(DeviceSession session, MediaDecodeOptions options = {});
    auto Prepare(TextureContext& context, const Image& image,
                 Option<Arc<dyn<VideoPlayback>>> playback = None())
        -> Result<PreparedMediaTexture, MediaError>;
    auto Pump(TextureContext& context, double seconds) -> Result<usize, MediaError>;
    auto Drain(u64 timeout_ns) -> Result<empty, MediaError>;
    void SetOptions(MediaDecodeOptions options);
    void Clear();

private:
    Arc<State> state_;
};

} // namespace owe::video

namespace owe::video
{

namespace
{

class RangeInputStream {
public:
    explicit RangeInputStream(rstd::io::ReadRange source)
        : m_length(static_cast<rstd::int64_t>(source.len().to_primitive())),
          m_reader(rstd::move(source).into_reader()) {}

    int read(rstd::uint8_t* buf, int size) {
        if (size <= 0) return 0;
        auto bytes = rstd::mut_ref<rstd::byte[]>::from_raw_parts(
            reinterpret_cast<rstd::byte*>(buf), usize(static_cast<rstd::size_t>(size)));
        auto result = m_reader.read(rstd::as_u8_slice_mut(bytes));
        if (result.is_err()) return -1;
        return static_cast<int>(rstd::move(result).unwrap_unchecked().to_primitive());
    }

    rstd::int64_t seek(rstd::int64_t offset, int whence) {
        constexpr int AVSEEK_SIZE = 0x10000;
        if (whence == AVSEEK_SIZE) return m_length;
        rstd::io::SeekFrom from;
        switch (whence) {
        case 0:
            if (offset < 0) return -1;
            from = rstd::io::SeekFrom::from_start(u64(static_cast<rstd::uint64_t>(offset)));
            break;
        case 1: from = rstd::io::SeekFrom::from_current(i64(offset)); break;
        case 2: from = rstd::io::SeekFrom::from_end(i64(offset)); break;
        default: return -1;
        }
        auto result = m_reader.seek(from);
        return result.is_ok() ? static_cast<rstd::int64_t>(
                                    rstd::move(result).unwrap_unchecked().to_primitive())
                              : -1;
    }

private:
    rstd::int64_t         m_length { 0 };
    rstd::io::RangeReader m_reader;
};

wavsen::video::HwAccel ParseHwdec(ref<str> value) {
    if (value == "vulkan"_str) return wavsen::video::HwAccel::Vulkan;
    if (value == "vaapi"_str) return wavsen::video::HwAccel::Vaapi;
#if __is_target_os(macos)
    if (value == "videotoolbox"_str) return wavsen::video::HwAccel::VideoToolbox;
#endif
    if (value == "none"_str) return wavsen::video::HwAccel::None;
    return wavsen::video::HwAccel::Auto;
}

const char* HwdecLabel(wavsen::video::HwAccel h) {
    switch (h) {
    case wavsen::video::HwAccel::Auto: return "auto";
    case wavsen::video::HwAccel::Vulkan: return "vulkan";
    case wavsen::video::HwAccel::Vaapi: return "vaapi";
    case wavsen::video::HwAccel::VideoToolbox: return "videotoolbox";
    case wavsen::video::HwAccel::None: return "none";
    }
    return "?";
}

const char* FrameKindLabel(wavsen::video::FrameKind k) {
    switch (k) {
    case wavsen::video::FrameKind::Sw: return "sw";
    case wavsen::video::FrameKind::VulkanShared: return "vulkan-shared";
    case wavsen::video::FrameKind::VaapiDrm: return "vaapi-drm";
    case wavsen::video::FrameKind::VideoToolbox: return "videotoolbox";
    }
    return "?";
}

Vec<const char*> ExtensionPtrs(slice<CString> names) {
    auto out = Vec<const char*>::with_capacity(names.len());
    for (const auto& name : names) out.push(name.as_ptr());
    return out;
}

Vec<wavsen::video::QueueFamily> QueueFamiliesForFfmpeg(const Device& device) {
    auto props = device.gpu().GetQueueFamilyProperties();
    auto out   = Vec<wavsen::video::QueueFamily>::with_capacity(props.len());
    for (rstd::uint32_t i = 0; i < props.len().to_primitive(); ++i) {
        out.push(wavsen::video::QueueFamily {
            .index      = u32(i),
            .flags      = props[usize(i)].queueFlags,
            .video_caps = u32(),
        });
    }
    return out;
}

wavsen::video::Producer::ExternalDeviceInfo
MakeExternalProducerInfo(const Device& device, rstd::uint32_t width, rstd::uint32_t height) {
    return wavsen::video::Producer::ExternalDeviceInfo {
        .instance                    = device.instance_handle(),
        .physical_device             = *device.gpu(),
        .device                      = *device.handle(),
        .queue                       = *device.graphics_queue().handle,
        .queue_family_index          = u32(device.graphics_queue().family_index),
        .queue_families              = QueueFamiliesForFfmpeg(device),
        .enabled_instance_extensions = ExtensionPtrs(device.enabled_instance_extensions()),
        .enabled_device_extensions   = ExtensionPtrs(device.enabled_device_extensions()),
        .api_version                 = u32(device.instance_api_version()),
        .width                       = u32(width),
        .height                      = u32(height),
        .instance_dispatch           = &device.instance_dispatch(),
        .device_dispatch             = &device.handle().Dispatch(),
    };
}

void CloseSyncFd(int fd) {
    if (fd >= 0) ::close(fd);
}

#if __is_target_os(macos)
void* ExportMetalDeviceHandle(const Device& device) {
    auto export_metal_objects = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(
        device.handle().Dispatch().vkGetDeviceProcAddr(*device.handle(),
                                                       "vkExportMetalObjectsEXT"));
    if (export_metal_objects == nullptr) return nullptr;

    VkExportMetalDeviceInfoEXT device_info {
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT,
        .pNext = nullptr,
    };
    VkExportMetalObjectsInfoEXT export_info {
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
        .pNext = &device_info,
    };
    export_metal_objects(*device.handle(), &export_info);
    return device_info.mtlDevice;
}

Option<ExImageParameters> CreateImportedMetalTextureImage(
    const Device& device, void* metal_texture, rstd::uint32_t width, rstd::uint32_t height,
    VkFormat           format = VK_FORMAT_B8G8R8A8_UNORM,
    VkImageUsageFlags  usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    if (metal_texture == nullptr || width == 0 || height == 0) return None();

    ExImageParameters           image;
    VkImportMetalTextureInfoEXT import_info {
        .sType      = VK_STRUCTURE_TYPE_IMPORT_METAL_TEXTURE_INFO_EXT,
        .pNext      = nullptr,
        .plane      = static_cast<VkImageAspectFlagBits>(aspect),
        .mtlTexture = reinterpret_cast<MTLTexture_id>(metal_texture),
    };
    VkImageCreateInfo image_info {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext         = &import_info,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = format,
        .extent        = VkExtent3D { width, height, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = usage,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (device.handle().CreateImage(image_info, image.handle) != VK_SUCCESS) return None();
    image.extent       = image_info.extent;
    image.mipmap_level = 1;

    VkImageViewCreateInfo view_info {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext    = nullptr,
        .image    = *image.handle,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = format,
        .subresourceRange =
            VkImageSubresourceRange {
                .aspectMask     = aspect,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
    };
    if (device.handle().CreateImageView(view_info, image.view) != VK_SUCCESS) {
        image.handle.reset();
        return None();
    }
    return Some(rstd::move(image));
}

#endif

} // anonymous namespace

struct MediaRegistry::State {
    State(DeviceSession value, MediaDecodeOptions decode_options)
        : options(rstd::move(decode_options)), session(rstd::move(value)) {}
    auto               device() const -> const Device& { return session.device(); }
    MediaDecodeOptions options;
    DeviceSession      session;
    Option<Box<wavsen::video::Producer>>  producer;
    Option<Box<wavsen::video::YuvToRgba>> yuv;
    Option<vvk::SubmissionToken>          yuv_completion;
    rstd::uint32_t                        yuv_max_width { 0 };
    rstd::uint32_t                        yuv_max_height { 0 };
#if __is_target_os(macos)
    Option<Arc<vvk::TimelineSemaphoreGeneration>> apple_timeline;
    u64                                           apple_completion {};
    bool                                          ensureAppleTimeline() {
        if (apple_timeline.is_some()) return true;
        static Atomic<u64> next_generation { u64(1) };
        auto               generation = next_generation.fetch_add(u64(1), Ordering::Relaxed);
        const auto&        queue      = device().graphics_queue();
        auto               domain     = vvk::MakeQueueDomain(
            *device().handle(), *queue.handle, u32(queue.family_index), u32(), generation);
        auto created = vvk::TimelineSemaphoreGeneration::Create(
            *device().handle(),
            domain,
            generation,
            vvk::TimelineSemaphoreDeviceDispatch::FromDispatch(device().handle().Dispatch()));
        if (! created.created()) return false;
        apple_timeline = rstd::move(created.generation);
        return true;
    }
#endif

    struct Runtime {
        Runtime(Arc<State> state, tex::TextureViewRef view)
            : shared_state(rstd::move(state)), typed_target(rstd::move(view)) {}
        Runtime(const Runtime&)                = delete;
        Runtime& operator=(const Runtime&)     = delete;
        Runtime(Runtime&&) noexcept            = default;
        Runtime& operator=(Runtime&&) noexcept = default;
        ~Runtime();

        Arc<State>                               shared_state;
        Weak<Runtime>                            self { Weak<Runtime>::make() };
        tex::TextureViewRef                      typed_target;
        Option<MediaError>                       error;
        bool                                     updated { false };
        u64                                      content_revision {};
        String                                   key;
        rstd::uint32_t                           width { 0 };
        rstd::uint32_t                           height { 0 };
        Option<Arc<rstd::dyn<VideoPlayback>>>    playback;
        Option<Box<wavsen::video::VideoDecoder>> decoder;
        wavsen::video::Nv12Frame                 nv12_scratch;
#if __is_target_os(macos)
        struct AppleUploadSlot {
            vvk::CommandBuffers       command_storage;
            vvk::CommandBuffer        command;
            vvk::Fence                fence;
            Option<ExImageParameters> imported;
            void*                     metal_texture { nullptr };
            bool                      pending { false };
        };

        rstd::array<AppleUploadSlot, 3> apple_upload_slots;
        rstd::size_t                    next_apple_upload_slot {};
        void*                           metal_device { nullptr };
        bool                            metal_device_queried { false };
        bool                            prepare_apple_upload_slot(AppleUploadSlot&);
        bool                            ensure_apple_upload_command(AppleUploadSlot&);
        void                            retire_apple_upload_slot(AppleUploadSlot&);
        bool upload_apple_frame(const wavsen::video::AppleFrameLease&, const ImageParameters&,
                                Option<wavsen::video::ConversionSubmission>&, bool&);
#endif
        f64  pts_acc {};
        f64  last_pts { -1.0 };
        bool have_frame { false };
        u64  applied_seek_sequence {};
        u64  attempted_seek_sequence {};

        auto device() const -> const Device& { return shared_state->session.device(); }
        void Pump(TextureContext& context, double dt_seconds);
    };
    Vec<Weak<Runtime>> runtimes;
    Vec<Weak<Runtime>> retiring;

    const wavsen::video::Producer* ensureProducer(rstd::uint32_t width, rstd::uint32_t height) {
        const auto& device = session.device();
        if (producer.is_some()) return producer->get();
        auto r =
            wavsen::video::Producer::from_external(MakeExternalProducerInfo(device, width, height));
        if (r.is_err()) {
            rstd_warn("MediaRegistry::Prepare: shared-device producer unavailable; falling back to "
                      "sw decode: {}",
                      rstd::move(r).unwrap_err().message);
            return nullptr;
        }
        producer = rstd::Some(rstd::move(r).unwrap());
        return producer->get();
    }

    auto ensureYuv(rstd::uint32_t width, rstd::uint32_t height) -> Result<empty, MediaError> {
        const auto& device = session.device();
        if (yuv.is_some() && width <= yuv_max_width && height <= yuv_max_height)
            return Ok(empty {});
        auto next_w = rstd::cmp::max(yuv_max_width, width);
        auto next_h = rstd::cmp::max(yuv_max_height, height);
        auto r      = wavsen::video::YuvToRgba::create(device.instance_dispatch(),
                                                       *device.gpu(),
                                                       device.handle().Dispatch(),
                                                       u32(device.graphics_queue().family_index),
                                                       *device.graphics_queue().handle,
                                                       u32(next_w),
                                                       u32(next_h));
        if (r.is_err())
            return Err(
                MediaError::decoder(MediaOperation::Prepare, rstd::move(r).unwrap_err().message));
        yuv = rstd::Some(rstd::move(r).unwrap());
        (void)yuv_completion.take();
        yuv_max_width  = next_w;
        yuv_max_height = next_h;
        return Ok(empty {});
    }
};

MediaRegistry::State::Runtime::~Runtime() {
#if __is_target_os(macos)
    for (auto& slot : apple_upload_slots) {
        retire_apple_upload_slot(slot);
        if (slot.metal_texture != nullptr) {
            wavsen::video::release_apple_video_metal_texture(slot.metal_texture);
            slot.metal_texture = nullptr;
        }
    }
#endif
}

#if __is_target_os(macos)
bool MediaRegistry::State::Runtime::prepare_apple_upload_slot(AppleUploadSlot& slot) {
    retire_apple_upload_slot(slot);
    return true;
}

bool MediaRegistry::State::Runtime::ensure_apple_upload_command(AppleUploadSlot& slot) {
    if (! slot.command) {
        if (device().cmd_pool().Allocate(
                usize(1), VK_COMMAND_BUFFER_LEVEL_PRIMARY, slot.command_storage) != VK_SUCCESS) {
            return false;
        }
        slot.command =
            vvk::CommandBuffer(slot.command_storage[usize()], device().handle().Dispatch());

        VkFenceCreateInfo fence_info {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        };
        if (device().handle().CreateFence(fence_info, slot.fence) != VK_SUCCESS) return false;
    }
    return true;
}

void MediaRegistry::State::Runtime::retire_apple_upload_slot(AppleUploadSlot& slot) {
    if (slot.pending) {
        VkResult result = slot.fence ? slot.fence.Wait() : VK_SUCCESS;
        if (result != VK_SUCCESS) {
            rstd_warn("PumpVideoTextures[{}]: video upload fence wait failed ({}), draining device",
                      key.as_str(),
                      static_cast<int>(result));
            (void)device().handle().WaitIdle();
        }
        if (slot.fence) (void)slot.fence.Reset();
        slot.pending = false;
    }
    (void)slot.imported.take();
}

bool MediaRegistry::State::Runtime::upload_apple_frame(
    const wavsen::video::AppleFrameLease& frame, const ImageParameters& target,
    Option<wavsen::video::ConversionSubmission>& result, bool& submit_attempted) {
    if (! frame.valid()) return false;
    if (! shared_state->ensureAppleTimeline()) return false;
    if (metal_device == nullptr && ! metal_device_queried) {
        metal_device         = ExportMetalDeviceHandle(device());
        metal_device_queried = true;
    }
    if (metal_device == nullptr) {
        rstd_error("PumpVideoTextures[{}]: VK_EXT_metal_objects did not expose an MTLDevice",
                   key.as_str());
        return false;
    }

    auto& slot = apple_upload_slots[usize(next_apple_upload_slot++ %
                                          apple_upload_slots.len().to_primitive())];
    if (! prepare_apple_upload_slot(slot)) {
        rstd_error("PumpVideoTextures[{}]: failed to prepare asynchronous video upload slot",
                   key.as_str());
        return false;
    }

    if (! ensure_apple_upload_command(slot)) {
        rstd_error("PumpVideoTextures[{}]: failed to allocate BGRA upload command", key.as_str());
        return false;
    }

    void* previous_metal_texture = slot.metal_texture;
    auto  texture_result         = wavsen::video::create_apple_video_metal_texture(
        frame, metal_device, previous_metal_texture);
    if (texture_result.is_err()) {
        rstd_error("PumpVideoTextures[{}]: VideoToolbox Metal texture: {}",
                   key.as_str(),
                   texture_result.unwrap_err().message);
        return false;
    }
    void* metal_texture = rstd::move(texture_result).unwrap();
    if (previous_metal_texture != nullptr && previous_metal_texture != metal_texture) {
        wavsen::video::release_apple_video_metal_texture(previous_metal_texture);
    }
    slot.metal_texture = metal_texture;
    auto imported      = CreateImportedMetalTextureImage(device(),
                                                         metal_texture,
                                                         frame.view().width.to_primitive(),
                                                         frame.view().height.to_primitive());
    if (imported.is_none()) {
        retire_apple_upload_slot(slot);
        rstd_error("PumpVideoTextures[{}]: failed to import Metal video texture into Vulkan",
                   key.as_str());
        return false;
    }
    slot.imported = Some(rstd::move(imported).unwrap());
    auto& cmd     = slot.command;
    if (cmd.Begin(VkCommandBufferBeginInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        }) != VK_SUCCESS) {
        retire_apple_upload_slot(slot);
        return false;
    }

    VkImageSubresourceRange range {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };
    VkImageMemoryBarrier imported_to_src {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = 0,
        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = *slot.imported->handle,
        .subresourceRange    = range,
    };
    VkImageMemoryBarrier target_to_dst {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = target.handle,
        .subresourceRange    = range,
    };
    cmd.PipelineBarrier(
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, imported_to_src);
    cmd.PipelineBarrier(
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, target_to_dst);

    VkImageBlit blit {
        .srcSubresource =
            VkImageSubresourceLayers {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel       = 0,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        .srcOffsets = { VkOffset3D { 0, 0, 0 },
                        VkOffset3D { static_cast<int>(frame.view().width.to_primitive()),
                                     static_cast<int>(frame.view().height.to_primitive()),
                                     1 } },
        .dstSubresource =
            VkImageSubresourceLayers {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel       = 0,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        .dstOffsets = { VkOffset3D { 0, 0, 0 },
                        VkOffset3D { static_cast<int>(width), static_cast<int>(height), 1 } },
    };
    cmd.BlitImage(*slot.imported->handle,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  target.handle,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  blit,
                  VK_FILTER_LINEAR);
    VkImageMemoryBarrier target_to_shader {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = target.handle,
        .subresourceRange    = range,
    };
    cmd.PipelineBarrier(
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, target_to_shader);
    if (cmd.End() != VK_SUCCESS) {
        retire_apple_upload_slot(slot);
        return false;
    }
    auto value = shared_state->apple_completion + u64(1);
    if (value == u64()) return false;
    auto                          semaphore    = (*shared_state->apple_timeline)->handle();
    auto                          native_value = value.to_primitive();
    VkTimelineSemaphoreSubmitInfo timeline {
        .sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues    = &native_value,
    };
    VkSubmitInfo submit {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext                = &timeline,
        .commandBufferCount   = 1,
        .pCommandBuffers      = cmd.address(),
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = &semaphore,
    };
    submit_attempted = true;
    if (device().graphics_queue().handle.Submit(submit, *slot.fence) != VK_SUCCESS) {
        // The producer reservation retains the source until uncertain work is drained.
        return false;
    }
    slot.pending                   = true;
    shared_state->apple_completion = value;
    vvk::SubmissionToken readiness { (*shared_state->apple_timeline)->source(), value };
    result = Some(wavsen::video::ConversionSubmission {
        .target               = target.handle,
        .width                = u32(width),
        .height               = u32(height),
        .target_kind          = wavsen::video::ConvertTarget::SampledLocal,
        .final_layout         = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .final_queue_family   = device().graphics_queue().family_index,
        .content_revision     = value,
        .readiness            = readiness,
        .execution_dependency = { Some((*shared_state->apple_timeline).clone()), readiness },
    });
    return true;
}
#endif

struct MediaProducerLease {
    Arc<MediaRegistry::State::Runtime> runtime;
    auto Pump(TextureContext& context, double seconds) -> Result<bool, MediaError> {
        runtime->Pump(context, seconds);
        if (runtime->error.is_some()) return Err(runtime->error.take().unwrap());
        return Ok(runtime->updated);
    }
    auto ContentRevision() const -> u64 { return runtime->content_revision; }
    auto SourceKind() const -> MediaSourceKind {
        if (runtime->decoder.is_none()) return MediaSourceKind::Unknown;
        switch ((*runtime->decoder)->kind()) {
        case wavsen::video::FrameKind::Sw: return MediaSourceKind::Software;
        case wavsen::video::FrameKind::VulkanShared: return MediaSourceKind::VulkanShared;
        case wavsen::video::FrameKind::VaapiDrm: return MediaSourceKind::VaapiDrm;
        case wavsen::video::FrameKind::VideoToolbox: return MediaSourceKind::VideoToolbox;
        }
        return MediaSourceKind::Unknown;
    }
};

} // namespace owe::video

namespace rstd
{
template<>
struct Impl<owe::video::MediaTextureProducer, owe::video::MediaProducerLease>
    : ImplBase<owe::video::MediaProducerLease> {
    auto Pump(vrento::vulkan::TextureContext& context, double seconds)
        -> Result<bool, owe::video::MediaError> {
        return self().Pump(context, seconds);
    }
    auto ContentRevision() const -> u64 { return self().ContentRevision(); }
    auto SourceKind() const -> owe::video::MediaSourceKind { return self().SourceKind(); }
};
} // namespace rstd

namespace owe::video
{
void MediaRegistry::State::Runtime::Pump(TextureContext& context, double dt_seconds) {
    updated = false;
    error   = None();
    if (decoder.is_none()) return;
    if (context.session().epoch() != shared_state->session.epoch()) {
        error = Some(MediaError::texture(tex::Error { tex::ErrorKind::DeviceMismatch }));
        return;
    }
    auto& s            = *this;
    auto  publish_time = [&] {
        if (s.playback.is_some()) {
            (*s.playback)->PublishTime(s.pts_acc, (*s.decoder)->duration());
        }
    };
    if (s.playback.is_some()) {
        auto state = (*s.playback)->Snapshot();
        if (state.seek_sequence != s.attempted_seek_sequence) {
            auto seeked               = (*s.decoder)->seek(state.seek_seconds);
            s.attempted_seek_sequence = state.seek_sequence;
            if (seeked.is_err()) {
                error = Some(MediaError::decoder(MediaOperation::Seek,
                                                 rstd::move(seeked).unwrap_err().message));
                publish_time();
                return;
            } else {
                s.applied_seek_sequence = state.seek_sequence;
                s.pts_acc               = state.seek_seconds;
                s.last_pts              = f64(-1.0);
                s.have_frame            = false;
            }
        }
        if (! state.playing) {
            publish_time();
            return;
        }
        dt_seconds *= state.rate.to_primitive();
    }
    s.pts_acc += f64(dt_seconds);

    const auto                fkind = (*s.decoder)->kind();
    wavsen::video::YuvToRgba* yuv   = nullptr;
    if (fkind != wavsen::video::FrameKind::VideoToolbox) {
        auto converted = shared_state->ensureYuv(s.width, s.height);
        if (converted.is_err()) {
            error = Some(converted.unwrap_err());
            publish_time();
            return;
        }
        yuv = shared_state->yuv->get();
    }
    if (! yuv && fkind != wavsen::video::FrameKind::VideoToolbox) {
        publish_time();
        return;
    }
    Option<wavsen::video::VkFrameLease>    vulkan_frame;
    Option<wavsen::video::VaapiFrameLease> vaapi_frame;
    Option<wavsen::video::AppleFrameLease> apple_frame;

    /* Drain decoded frames until we catch up to wall time. Cap to
     * 4 frames per tick to avoid spiral-of-death on heavy stalls. */
    bool got_new = false;
    for (int i = 0; i < 4; ++i) {
        if (s.last_pts >= f64() && s.last_pts > s.pts_acc) break;

        rstd::Result<wavsen::video::NextFrame, wavsen::video::Error> r =
            rstd::Ok(wavsen::video::NextFrame::Ok);
        switch (fkind) {
        case wavsen::video::FrameKind::VulkanShared: {
            auto pulled = (*s.decoder)->next_vk_frame();
            if (pulled.is_err()) {
                r = Err(rstd::move(pulled).unwrap_err());
            } else {
                auto value = rstd::move(pulled).unwrap();
                r          = Ok(value.status);
                if (value.frame.is_some()) vulkan_frame = rstd::move(value.frame);
            }
            break;
        }
        case wavsen::video::FrameKind::VaapiDrm: {
            auto pulled = (*s.decoder)->next_vaapi_frame();
            if (pulled.is_err()) {
                r = Err(rstd::move(pulled).unwrap_err());
            } else {
                auto value  = rstd::move(pulled).unwrap();
                r           = Ok(value.status);
                vaapi_frame = rstd::move(value.frame);
            }
            break;
        }
        case wavsen::video::FrameKind::VideoToolbox: {
            auto pulled = (*s.decoder)->next_apple_frame();
            if (pulled.is_err()) {
                r = Err(rstd::move(pulled).unwrap_err());
            } else {
                auto value = rstd::move(pulled).unwrap();
                r          = Ok(value.status);
                if (value.frame.is_some()) apple_frame = rstd::move(value.frame);
            }
            break;
        }
        case wavsen::video::FrameKind::Sw: r = (*s.decoder)->next_frame(s.nv12_scratch); break;
        }
        if (r.is_err()) {
            error = Some(
                MediaError::decoder(MediaOperation::Decode, rstd::move(r).unwrap_err().message));
            publish_time();
            return;
        }
        auto kind = r.unwrap();
        if (kind == wavsen::video::NextFrame::Eof) {
            s.pts_acc  = f64();
            s.last_pts = f64(-1.0);
            break;
        }
        const bool decoder_looped = kind == wavsen::video::NextFrame::Looped;
        f64        frame_pts { -1.0 };
        switch (fkind) {
        case wavsen::video::FrameKind::VulkanShared:
            if (vulkan_frame.is_none()) {
                error = Some(
                    MediaError { MediaOperation::Decode,
                                 MediaCause::Invalid("Vulkan decoder returned no lease"_Str) });
                publish_time();
                return;
            }
            frame_pts = vulkan_frame->info().pts_seconds;
            break;
        case wavsen::video::FrameKind::VaapiDrm:
            if (vaapi_frame.is_none()) {
                error =
                    Some(MediaError { MediaOperation::Decode,
                                      MediaCause::Invalid("VAAPI decoder returned no lease"_Str) });
                publish_time();
                return;
            }
            frame_pts = vaapi_frame->view().pts_seconds;
            break;
        case wavsen::video::FrameKind::VideoToolbox:
            if (apple_frame.is_none()) {
                error = Some(MediaError {
                    MediaOperation::Decode,
                    MediaCause::Invalid("VideoToolbox decoder returned no lease"_Str) });
                publish_time();
                return;
            }
            frame_pts = apple_frame->view().pts_seconds;
            break;
        case wavsen::video::FrameKind::Sw: frame_pts = s.nv12_scratch.pts_seconds; break;
        }
        if (decoder_looped) s.pts_acc = frame_pts.max(f64());
        s.last_pts = frame_pts;
        got_new    = true;
        if (decoder_looped) break;
    }
    if (! got_new && s.have_frame) {
        publish_time();
        return;
    }
    if (! got_new) {
        publish_time();
        return;
    }

    u32 cs_id;
    u32 cr_id;
    switch (fkind) {
    case wavsen::video::FrameKind::VulkanShared:
        cs_id = vulkan_frame->info().colorspace;
        cr_id = vulkan_frame->info().color_range;
        break;
    case wavsen::video::FrameKind::VaapiDrm:
        cs_id = vaapi_frame->view().colorspace;
        cr_id = vaapi_frame->view().color_range;
        break;
    case wavsen::video::FrameKind::VideoToolbox:
        cs_id = apple_frame->view().colorspace;
        cr_id = apple_frame->view().color_range;
        break;
    case wavsen::video::FrameKind::Sw:
        cs_id = s.nv12_scratch.colorspace;
        cr_id = s.nv12_scratch.color_range;
        break;
    }
    const auto color_matrix = wavsen::video::make_color_matrix(
        static_cast<wavsen::video::ColorSpace>(cs_id.to_primitive()),
        static_cast<wavsen::video::ColorRange>(cr_id.to_primitive()));

    auto runtime_owner = self.upgrade();
    if (! runtime_owner) {
        error = Some(MediaError::texture(tex::Error { tex::ErrorKind::Closed }));
        return;
    }
    auto final         = native_access({ tex::Access::Sampled, tex::Stages::Fragment });
    final.queue_family = device().graphics_queue().family_index;
    auto reserved      = context.BeginProducerWrite({
        .view         = typed_target,
        .final        = { tex::Access::Sampled, tex::Stages::Fragment },
        .native_final = Some(final),
        .owner        = Arc<dyn<rstd::any::Any>>::make(rstd::move(runtime_owner)),
    });
    if (reserved.is_err()) {
        auto failure = reserved.unwrap_err();
        if (failure.kind != tex::ErrorKind::Busy)
            error = Some(MediaError::texture(rstd::move(failure)));
        return;
    }
    auto        write        = rstd::move(reserved).unwrap();
    const auto& target_state = write.target();
    if (target_state.initial.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
        (target_state.initial.queue_family != VK_QUEUE_FAMILY_IGNORED &&
         target_state.initial.queue_family != device().graphics_queue().family_index)) {
        error = Some(MediaError::texture(tex::Error { tex::ErrorKind::Invalid }));
        return;
    }
    // wavsen cannot consume target waits; don't overwrite an unfinished renderer read.
    auto ready = target_state.dependency.poll();
    if (ready.is_err()) {
        error = Some(MediaError::texture(ready.unwrap_err()));
        return;
    }
    if (! ready.unwrap()) return;
    const auto ip = target_state.image;

    Option<wavsen::video::ConversionSubmission> converted_frame;
    bool                                        submit_attempted  = false;
    rstd::Result<int, wavsen::video::Error>     cv                = rstd::Ok(-1);
    auto                                        failure_operation = MediaOperation::Convert;
    switch (fkind) {
    case wavsen::video::FrameKind::VulkanShared: {
        auto reserved = yuv->reserve({
            .target = {
                .image  = ip.handle,
                .view   = ip.view,
                .width  = u32(s.width),
                .height = u32(s.height),
                .kind   = wavsen::video::ConvertTarget::SampledLocal,
            },
        });
        if (reserved.is_err()) {
            cv = Err(rstd::move(reserved).unwrap_err());
            break;
        }
        auto reservation = rstd::move(reserved).unwrap();
        if (reservation.is_none()) {
            publish_time();
            return;
        }
        submit_attempted = true;
        auto submitted   = yuv->submit_av_vk_frame(
            rstd::move(*reservation), rstd::move(*vulkan_frame), color_matrix);
        if (submitted.is_err()) {
            cv = Err(rstd::move(submitted).unwrap_err());
        } else {
            converted_frame = Some(rstd::move(submitted).unwrap());
            cv              = Ok(converted_frame->sync_fd);
        }
        break;
    }
    case wavsen::video::FrameKind::VaapiDrm: {
        auto reserved = yuv->reserve({
            .target = {
                .image  = ip.handle,
                .view   = ip.view,
                .width  = u32(s.width),
                .height = u32(s.height),
                .kind   = wavsen::video::ConvertTarget::SampledLocal,
            },
        });
        if (reserved.is_err()) {
            cv = Err(rstd::move(reserved).unwrap_err());
            break;
        }
        auto reservation = rstd::move(reserved).unwrap();
        if (reservation.is_none()) {
            publish_time();
            return;
        }
        failure_operation = MediaOperation::Import;
        auto mapped       = rstd::move(*vaapi_frame).into_drm();
        if (mapped.is_err()) {
            cv = Err(rstd::move(mapped).unwrap_err());
            break;
        }
        failure_operation = MediaOperation::Convert;
        submit_attempted  = true;
        auto submitted    = yuv->submit_drm_prime(
            rstd::move(*reservation), rstd::move(mapped).unwrap(), color_matrix);
        if (submitted.is_err()) {
            cv = Err(rstd::move(submitted).unwrap_err());
            break;
        }
        auto value = rstd::move(submitted).unwrap();
        if (value.is_none()) {
            publish_time();
            return;
        }
        converted_frame = Some(rstd::move(*value));
        cv              = Ok(converted_frame->sync_fd);
        break;
    }
    case wavsen::video::FrameKind::VideoToolbox:
#if __is_target_os(macos)
        if (apple_frame.is_none() ||
            ! s.upload_apple_frame(*apple_frame, ip, converted_frame, submit_attempted)) {
            cv = Err(wavsen::video::Error(rstd::format("VideoToolbox Metal→Vulkan copy failed")));
        } else {
            cv = Ok(-1);
        }
#else
        cv = Err(wavsen::video::Error(
            rstd::format("VideoToolbox is only available on Apple platforms")));
#endif
        break;
    case wavsen::video::FrameKind::Sw: {
        submit_attempted = true;
        auto submitted   = yuv->submit_nv12(ip.handle,
                                            u32(s.width),
                                            u32(s.height),
                                            s.nv12_scratch.data.data(),
                                            s.nv12_scratch.data.len(),
                                            color_matrix,
                                            wavsen::video::ConvertTarget::SampledLocal);
        if (submitted.is_err()) {
            cv = Err(rstd::move(submitted).unwrap_err());
        } else {
            converted_frame = Some(rstd::move(submitted).unwrap());
            cv              = Ok(converted_frame->sync_fd);
        }
    } break;
    }
    if (cv.is_err()) {
        if (submit_attempted) write.uncertain();
        error = Some(MediaError::decoder(failure_operation, rstd::move(cv).unwrap_err().message));
        publish_time();
        return;
    }
    CloseSyncFd(rstd::move(cv).unwrap());
    if (converted_frame.is_some() && fkind != wavsen::video::FrameKind::VideoToolbox)
        shared_state->yuv_completion = Some(converted_frame->readiness);
    if (converted_frame.is_none() || ! converted_frame->submitted() ||
        converted_frame->target_kind != wavsen::video::ConvertTarget::SampledLocal ||
        converted_frame->target != ip.handle || converted_frame->width != u32(s.width) ||
        converted_frame->height != u32(s.height) ||
        converted_frame->final_layout != write.target().final.layout ||
        converted_frame->final_queue_family != write.target().final.queue_family ||
        converted_frame->execution_dependency.signal_queue().device !=
            vvk::OpaqueHandleIdentity(*device().handle())) {
        write.uncertain();
        error = Some(MediaError::texture(tex::Error { tex::ErrorKind::Invalid }));
        publish_time();
        return;
    }
    auto completion_owner =
        Arc<dyn<rstd::any::Any>>::make(converted_frame->execution_dependency.clone());
    auto dependency =
        context.ImportDependency(converted_frame->execution_dependency.wait_semaphore(),
                                 converted_frame->execution_dependency.wait_value().to_primitive(),
                                 rstd::move(completion_owner));
    if (dependency.is_err()) {
        write.uncertain();
        error = Some(MediaError::texture(dependency.unwrap_err()));
        publish_time();
        return;
    }
    auto committed = context.CommitProducerWrite(write, rstd::move(dependency).unwrap());
    if (committed.is_err()) {
        write.uncertain();
        error = Some(MediaError::texture(committed.unwrap_err()));
        publish_time();
        return;
    }
    content_revision = converted_frame->content_revision;
    updated          = true;
    s.have_frame     = true;
    publish_time();
}

MediaRegistry::MediaRegistry(DeviceSession session, MediaDecodeOptions options)
    : state_(Arc<State>::make(rstd::move(session), rstd::move(options))) {}

auto OpenMediaRuntime(Arc<MediaRegistry::State> state, const Image& image, tex::TextureViewRef view,
                      Option<Arc<dyn<VideoPlayback>>> playback)
    -> Result<Arc<MediaRegistry::State::Runtime>, MediaError> {
    if (image.header.kind != ImageKind::Video || image.slots.is_empty() ||
        image.slots[usize()].mipmaps.is_empty())
        return Err(
            MediaError { MediaOperation::Open, MediaCause::Invalid("invalid video input"_Str) });
    const auto& mip = image.slots[usize()].mipmaps[usize()];
    if (mip.video_source.is_none() || mip.width <= 0 || mip.height <= 0)
        return Err(
            MediaError { MediaOperation::Open, MediaCause::Invalid("invalid video input"_Str) });
    auto                          video_source = mip.video_source->clone();
    auto*                         registry     = rstd::addressof(*state);
    MediaRegistry::State::Runtime runtime(state.clone(), rstd::move(view));
    runtime.key      = image.key.clone();
    auto extent      = tex::mip_extent(runtime.typed_target.texture().description(),
                                       runtime.typed_target.description().range.first_mip);
    runtime.width    = extent.width;
    runtime.height   = extent.height;
    runtime.playback = rstd::move(playback);
#if ! __is_target_os(macos)
    auto converted = registry->ensureYuv(runtime.width, runtime.height);
    if (converted.is_err()) return Err(converted.unwrap_err());
#endif
    auto factory = Box<dyn<FnMut<Box<dyn<wavsen::video::InputStream>>()>>>::make(
        [source = rstd::move(video_source)]() -> Box<dyn<wavsen::video::InputStream>> {
            return Box<dyn<wavsen::video::InputStream>>::make(RangeInputStream(source.clone()));
        });
    const auto              requested_hwdec = ParseHwdec(registry->options.hwdec.as_str());
    wavsen::video::OpenOpts opts {
        requested_hwdec,
        registry->options.render_node.clone(),
    };
    const wavsen::video::Producer* producer = nullptr;
#if __is_target_os(macos)
    const bool needs_shared_vulkan = requested_hwdec == wavsen::video::HwAccel::Vulkan;
#else
    const bool needs_shared_vulkan = requested_hwdec != wavsen::video::HwAccel::None;
#endif
    if (needs_shared_vulkan) {
        producer = registry->ensureProducer(runtime.width, runtime.height);
        if (! producer) opts.hwaccel = wavsen::video::HwAccel::None;
    }
    auto dec_r = wavsen::video::VideoDecoder::open_from_stream(rstd::move(factory),
                                                               u32(runtime.width),
                                                               u32(runtime.height),
                                                               /*loop=*/true,
                                                               producer,
                                                               opts);
    if (dec_r.is_err())
        return Err(
            MediaError::decoder(MediaOperation::Open, rstd::move(dec_r).unwrap_err().message));
    runtime.decoder = rstd::Some(rstd::move(dec_r).unwrap());
#if __is_target_os(macos)
    if ((*runtime.decoder)->kind() != wavsen::video::FrameKind::VideoToolbox) {
        auto converted = registry->ensureYuv(runtime.width, runtime.height);
        if (converted.is_err()) return Err(converted.unwrap_err());
    }
#endif
    if (runtime.playback.is_some()) {
        (*runtime.playback)->PublishTime(f64(), (*runtime.decoder)->duration());
    }
    rstd_info("MediaRegistry::Prepare: {} hwdec={} decoder kind={}",
              image.key,
              HwdecLabel(requested_hwdec),
              FrameKindLabel((*runtime.decoder)->kind()));

    auto owned  = Arc<MediaRegistry::State::Runtime>::make(rstd::move(runtime));
    owned->self = owned.downgrade();
    state->runtimes.push(owned.downgrade());
    return Ok(rstd::move(owned));
}

auto MediaRegistry::Prepare(TextureContext& context, const Image& image,
                            Option<Arc<dyn<VideoPlayback>>> playback)
    -> Result<PreparedMediaTexture, MediaError> {
    if (context.session().epoch() != state_->session.epoch())
        return Err(MediaError::texture(tex::Error { tex::ErrorKind::DeviceMismatch }));
    if (image.header.kind != ImageKind::Video || image.slots.is_empty() ||
        image.slots[usize()].mipmaps.is_empty())
        return Err(MediaError::texture(tex::Error { tex::ErrorKind::Invalid }));
    const auto& mip = image.slots[usize()].mipmaps[usize()];
    if (mip.video_source.is_none() || mip.width <= 0 || mip.height <= 0)
        return Err(MediaError::texture(tex::Error { tex::ErrorKind::Invalid }));
    tex::TextureDesc desc;
    desc.extent = { (tex::Count(mip.width) + 1u) & ~1u, (tex::Count(mip.height) + 1u) & ~1u };
    desc.format = vrento::TextureFormat::RGBA8;
    desc.usage  = tex::Usage::Sampled | tex::Usage::Storage | tex::Usage::TransferDestination;
    auto pixels = rstd::uint64_t(desc.extent.width) * rstd::uint64_t(desc.extent.height);
    if (pixels > usize::MAX.to_primitive() / 4u)
        return Err(MediaError::texture(tex::Error { tex::ErrorKind::Invalid }));
    auto texture = context.CreateTexture(desc);
    if (texture.is_err()) return Err(MediaError::texture(texture.unwrap_err()));
    auto view = context.CreateView(texture.unwrap(), {});
    if (view.is_err()) return Err(MediaError::texture(view.unwrap_err()));
    auto target  = rstd::move(view).unwrap();
    auto sampler = context.CreateSampler({ image.header.sample });
    if (sampler.is_err()) return Err(MediaError::texture(sampler.unwrap_err()));
    auto runtime = OpenMediaRuntime(state_.clone(), image, target, rstd::move(playback));
    if (runtime.is_err()) return Err(runtime.unwrap_err());
    tex::TextureUpload upload;
    upload.target = target;
    upload.width  = desc.extent.width;
    upload.height = desc.extent.height;
    Vec<u8> bytes;
    bytes.resize(usize(pixels * 4u), u8());
    for (usize pixel {}; pixel < usize(pixels); ++pixel)
        bytes[pixel * usize(4) + usize(3)] = u8(255);
    upload.bytes = rstd::move(bytes);
    Vec<tex::TextureUpload> uploads;
    uploads.push(rstd::move(upload));
    return Ok(PreparedMediaTexture {
        rstd::move(target),
        sampler.unwrap(),
        rstd::move(uploads),
        Arc<dyn<MediaTextureProducer>>::make(MediaProducerLease { rstd::move(*runtime) }),
    });
}

auto MediaRegistry::Pump(TextureContext& context, double seconds) -> Result<usize, MediaError> {
    if (context.session().epoch() != state_->session.epoch())
        return Err(MediaError::texture(tex::Error { tex::ErrorKind::DeviceMismatch }));
    state_->runtimes.retain([](const auto& weak) {
        return ! weak.expired();
    });
    usize updated {};
    for (const auto& weak : state_->runtimes) {
        auto runtime = weak.upgrade();
        if (! runtime) continue;
        runtime->Pump(context, seconds);
        if (runtime->error.is_some()) return Err(runtime->error.take().unwrap());
        if (runtime->updated) ++updated;
    }
    return Ok(updated);
}
auto MediaRegistry::Drain(u64 timeout_ns) -> Result<empty, MediaError> {
    auto started   = rstd::time::Instant::now();
    auto remaining = [&] {
        auto elapsed = rstd::try_from<u64>((rstd::time::Instant::now() - started).as_nanos())
                           .unwrap_or(u64(u64::MAX.to_primitive()));
        return elapsed >= timeout_ns ? u64() : timeout_ns - elapsed;
    };
#if __is_target_os(macos)
    auto drain_slots = [&](const Vec<Weak<State::Runtime>>& runtimes) -> Result<empty, tex::Error> {
        for (const auto& weak : runtimes) {
            auto runtime = weak.upgrade();
            if (! runtime) continue;
            for (auto& slot : runtime->apple_upload_slots) {
                if (slot.pending && slot.fence) {
                    auto result = slot.fence.Wait(remaining().to_primitive());
                    if (result != VK_SUCCESS) return Err(texture_error(result));
                }
                runtime->retire_apple_upload_slot(slot);
            }
        }
        return Ok(empty {});
    };
    auto active = drain_slots(state_->runtimes);
    if (active.is_err()) return Err(MediaError::texture(active.unwrap_err()));
    auto retired = drain_slots(state_->retiring);
    if (retired.is_err()) return Err(MediaError::texture(retired.unwrap_err()));
#endif
    if (state_->yuv.is_none()) return Ok(empty {});
    if (state_->yuv_completion.is_some()) {
        const auto observed = (*state_->yuv)->wait_completion(*state_->yuv_completion, remaining());
        if (! observed.observed() || ! observed.completed.valid() ||
            ! vvk::CompletionCovers(observed.completed, *state_->yuv_completion))
            return Err(MediaError::texture(texture_error(
                observed.api_result == VK_SUCCESS ? VK_TIMEOUT : observed.api_result)));
    }
    auto drained = (*state_->yuv)->drain_submissions(remaining());
    if (drained.is_err()) {
        return Err(MediaError::decoder(MediaOperation::Convert, drained.unwrap_err().message));
    }
    (void)state_->yuv_completion.take();
    state_->retiring.retain([](const auto& weak) {
        return ! weak.expired();
    });
    return Ok(empty {});
}
void MediaRegistry::SetOptions(MediaDecodeOptions options) {
    state_->options = rstd::move(options);
    state_->runtimes.retain([](const auto& weak) {
        return ! weak.expired();
    });
    state_->retiring.retain([](const auto& weak) {
        return ! weak.expired();
    });
    if (state_->runtimes.is_empty() && state_->retiring.is_empty()) (void)state_->producer.take();
}
void MediaRegistry::Clear() {
    state_->retiring.retain([](const auto& weak) {
        return ! weak.expired();
    });
    for (auto& weak : state_->runtimes) state_->retiring.push(rstd::move(weak));
    state_->runtimes.clear();
}

} // namespace owe::video

export namespace owe::video
{
inline auto ResourceFailure(MediaError error) -> vrento::resource::ResourceError {
    return vrento::resource::ResourceError::external(
        rstd::format("video operation={}", int(error.operation)),
        Box<dyn<rstd::error::Error>>::make(rstd::move(error)));
}
struct WavsenTextureFactory {
    MediaRegistry media;
    explicit WavsenTextureFactory(DeviceSession session, MediaDecodeOptions options = {})
        : media(rstd::move(session), rstd::move(options)) {}
};
} // namespace owe::video

export namespace rstd
{
template<>
struct Impl<vrento::resource_registry::TextureProducer,
            sync::Arc<dyn<owe::video::MediaTextureProducer>>>
    : ImplBase<sync::Arc<dyn<owe::video::MediaTextureProducer>>> {
    auto Pump(vrento::vulkan::TextureContext& context, double seconds)
        -> Result<empty, vrento::resource::ResourceError> {
        auto result = self()->Pump(context, seconds);
        if (result.is_err()) return Err(owe::video::ResourceFailure(result.unwrap_err()));
        return Ok(empty {});
    }
};
template<>
struct Impl<vrento::resource_registry::TextureProducerFactory, owe::video::WavsenTextureFactory>
    : ImplBase<owe::video::WavsenTextureFactory> {
    auto Create(vrento::vulkan::TextureContext& context, sync::Arc<vrento::Image> image,
                Option<sync::Arc<dyn<vrento::VideoPlayback>>> playback)
        -> Result<vrento::resource_registry::ProducedTexture, vrento::resource::ResourceError> {
        auto result = self().media.Prepare(context, *image, rstd::move(playback));
        if (result.is_err()) return Err(owe::video::ResourceFailure(result.unwrap_err()));
        auto                                       media = rstd::move(result).unwrap();
        vrento::resource_registry::ProducedTexture produced;
        produced.slots.push({ rstd::move(media.view), rstd::move(media.sampler) });
        produced.uploads  = rstd::move(media.uploads);
        produced.producer = Some(sync::Arc<dyn<vrento::resource_registry::TextureProducer>>::make(
            rstd::move(media.producer)));
        return Ok(rstd::move(produced));
    }
    void Clear() { self().media.Clear(); }
    auto Drain(u64 timeout) -> Result<empty, vrento::resource::ResourceError> {
        auto result = self().media.Drain(timeout);
        if (result.is_err()) return Err(owe::video::ResourceFailure(result.unwrap_err()));
        return Ok(empty {});
    }
};
} // namespace rstd
