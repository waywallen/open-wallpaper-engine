#include <rstd/test/gtest.hpp>
#include <algorithm>
#include <atomic>
#include <span>
#include <string>
#include <vector>

import rstd;
import rstd.cppstd;
import wescene.resource_registry;
import wescene.types;
import wescene.vulkan;

using namespace rstd::literals;

namespace tex = vrento::texture;

struct TextureRuntimeProbe {
    auto Pump(vrento::vulkan::TextureContext&, double)
        -> rstd::Result<rstd::empty, vrento::resource::ResourceError> {
        return rstd::Ok(rstd::empty {});
    }
};

namespace rstd
{

template<>
struct Impl<owe::resource_registry::TextureProducer, TextureRuntimeProbe>
    : ImplBase<TextureRuntimeProbe> {
    auto Pump(vrento::vulkan::TextureContext& context, double seconds)
        -> Result<empty, vrento::resource::ResourceError> {
        return this->self().Pump(context, seconds);
    }
};

} // namespace rstd

namespace
{

struct ShaderArtifactProvider {
    rstd::usize loads { 0 };

    auto LoadShader(const owe::resource::ShaderRequest& request)
        -> rstd::Result<owe::resource::ShaderArtifact, owe::resource::ResourceError> {
        ++loads;
        return rstd::Ok(owe::resource::ShaderArtifact {
            .source          = request.source,
            .content_version = request.content_version,
        });
    }
};

struct BufferContentProvider {
    rstd::usize              loads { 0 };
    rstd::vec::Vec<rstd::u8> content;
    bool                     fail {}, zeroed {};

    BufferContentProvider(): content(rstd::vec::Vec<rstd::u8>::make()) {
        content.push(rstd::u8(1));
        content.push(rstd::u8(2));
    }

    auto LoadBuffer(const owe::resource::BufferResourceRef& request)
        -> rstd::Result<owe::resource::BufferContent, owe::resource::ResourceError> {
        ++loads;
        if (fail)
            return rstd::Err(owe::resource::ResourceError {
                .kind = owe::resource::ResourceErrorKind::MissingContent });
        if (zeroed)
            return rstd::Ok(
                owe::resource::BufferContent::zeroed(request.definition().definition.size));
        return rstd::Ok(owe::resource::BufferContent::from(content.as_slice()));
    }
};

struct BufferBackend {
    rstd::usize                          allocations { 0 };
    rstd::usize                          writes { 0 };
    rstd::u64                            next_ticket { 0 };
    owe::vulkan::BufferAllocationRequest last_request;
    rstd::usize                          zeros {}, last_bytes {};
    bool                                 fail_write {};

    auto AllocateBuffer(const owe::vulkan::BufferAllocationRequest& request)
        -> rstd::Result<owe::vulkan::BufferAllocation, vrento::vulkan::BackendError> {
        ++allocations;
        last_request = request;
        if (request.size == 0)
            return rstd::Err(vrento::vulkan::BackendError {
                rstd::string::String {},
                vrento::vulkan::BackendCause::Invalid(rstd::string::String {}) });
        return rstd::Ok(owe::vulkan::BufferAllocation {});
    }

    auto QueueBufferWrite(rstd::mut_ref<owe::vulkan::BufferAllocation>, rstd::slice<rstd::u8> bytes,
                          VkDeviceSize)
        -> rstd::Result<owe::vulkan::BufferUploadTicket, vrento::vulkan::BackendError> {
        ++writes;
        last_bytes = bytes.len();
        if (fail_write)
            return rstd::Err(vrento::vulkan::BackendError { "test write"_Str,
                                                            vrento::vulkan::BackendCause::Busy() });
        ++next_ticket;
        return rstd::Ok(owe::vulkan::BufferUploadTicket { .value = next_ticket });
    }
    auto QueueBufferZero(rstd::mut_ref<owe::vulkan::BufferAllocation> allocation)
        -> rstd::Result<owe::vulkan::BufferUploadTicket, vrento::vulkan::BackendError> {
        ++zeros;
        return QueueBufferWrite(allocation, {}, 0);
    }
};

auto TextureAllocation(
    rstd::uint64_t                             generation,
    rstd::sync::Arc<rstd::dyn<rstd::any::Any>> backing =
        rstd::sync::Arc<rstd::dyn<rstd::any::Any>>::make(0),
    rstd::Option<rstd::sync::Arc<rstd::dyn<owe::resource_registry::TextureProducer>>> producer =
        rstd::None(),
    tex::TextureDesc desc         = { .extent = { 1, 1 },
                                      .usage  = tex::Usage::Sampled | tex::Usage::TransferSource |
                                                tex::Usage::TransferDestination },
    tex::SamplerDesc sampler_desc = {})
    -> rstd::sync::Arc<owe::resource_registry::TextureAllocation>;

struct TextureLoader {
    std::atomic<std::size_t>* loads;

    auto LoadTexture(rstd::ref<rstd::str> key) const
        -> rstd::Result<rstd::sync::Arc<vrento::Image>, owe::resource::ResourceError> {
        loads->fetch_add(1, std::memory_order_relaxed);
        auto image = rstd::sync::Arc<vrento::Image>::make();
        image->key = rstd::into(key);
        return rstd::Ok(rstd::move(image));
    }
};

struct TextureContentProvider {
    mutable rstd::usize              resolves { 0 };
    mutable std::atomic<std::size_t> loads { 0 };

    auto ResolveTextureContent(const owe::resource::TextureRequest& request) const
        -> rstd::Result<owe::resource::ImportedTextureContentIdentity,
                        owe::resource::ResourceError> {
        ++resolves;
        auto name = rstd::cppstd::as_string_view(request.name.as_str());
        if (name.starts_with("alias-")) {
            return rstd::Ok(owe::resource::ImportedTextureContentIdentity {
                .key = rstd::string::String::make("shared"_str),
            });
        }
        return rstd::Ok(owe::resource::ImportedTextureContentIdentity {
            .key = request.name.clone(),
        });
    }

    auto OpenTextureLoader() const
        -> rstd::Result<rstd::sync::Arc<rstd::dyn<owe::resource::TextureLoader>>,
                        owe::resource::ResourceError> {
        return rstd::Ok(
            rstd::sync::Arc<rstd::dyn<owe::resource::TextureLoader>>::make(TextureLoader {
                .loads = &loads,
            }));
    }

    auto ResolveVideoPlayback(const owe::resource::TextureRequest&) const
        -> rstd::Option<rstd::sync::Arc<rstd::dyn<vrento::VideoPlayback>>> {
        return rstd::None();
    }
};

struct ImageBackend {
    rstd::usize creates { 0 };
    rstd::usize transparent_creates { 0 };
    rstd::u64   generation { 0 };

    auto CreateImportedTexture(rstd::sync::Arc<vrento::Image> image,
                               rstd::Option<rstd::sync::Arc<rstd::dyn<vrento::VideoPlayback>>>)
        -> rstd::Result<owe::resource_registry::PreparedImageAllocation,
                        owe::resource::ResourceError> {
        ++creates;
        ++generation;
        return rstd::Ok(owe::resource_registry::PreparedImageAllocation {
            .allocation = TextureAllocation(
                generation.to_primitive(),
                rstd::sync::Arc<rstd::dyn<rstd::any::Any>>::make(rstd::move(image))),
            .upload = rstd::Some(owe::vulkan::ImageUploadTicket {
                .value = generation,
            }),
        });
    }

    auto AllocateTexture(const owe::resource::TextureDefinition& definition, bool transparent)
        -> rstd::Result<owe::resource_registry::PreparedImageAllocation,
                        owe::resource::ResourceError> {
        if (definition.width <= rstd::i32() || definition.height <= rstd::i32())
            return rstd::Err(owe::resource::ResourceError {
                .kind = owe::resource::ResourceErrorKind::BackendFailure,
            });
        tex::TextureDesc desc;
        desc.extent     = { tex::Count(definition.width.to_primitive()),
                            tex::Count(definition.height.to_primitive()) };
        desc.mip_levels = definition.mip_levels.to_primitive();
        desc.samples    = definition.samples.to_primitive();
        desc.format     = definition.format;
        desc.usage      = tex::Usage::None;
        if (owe::resource::HasTextureUsage(definition.usage, owe::resource::TextureUsage::Sampled))
            desc.usage = desc.usage | tex::Usage::Sampled;
        if (owe::resource::HasTextureUsage(definition.usage,
                                           owe::resource::TextureUsage::ColorAttachment))
            desc.usage = desc.usage | tex::Usage::ColorAttachment;
        if (owe::resource::HasTextureUsage(definition.usage,
                                           owe::resource::TextureUsage::DepthAttachment))
            desc.usage = desc.usage | tex::Usage::DepthAttachment;
        if (owe::resource::HasTextureUsage(definition.usage,
                                           owe::resource::TextureUsage::TransferSource))
            desc.usage = desc.usage | tex::Usage::TransferSource;
        if (owe::resource::HasTextureUsage(definition.usage,
                                           owe::resource::TextureUsage::TransferDestination))
            desc.usage = desc.usage | tex::Usage::TransferDestination;
        if (tex::validate(desc).is_err())
            return rstd::Err(owe::resource::ResourceError {
                .kind = owe::resource::ResourceErrorKind::BackendFailure,
            });
        ++creates;
        if (transparent) ++transparent_creates;
        ++generation;
        auto allocation = TextureAllocation(generation.to_primitive(),
                                            rstd::sync::Arc<rstd::dyn<rstd::any::Any>>::make(0),
                                            rstd::None(),
                                            desc,
                                            { definition.sample });
        if (transparent) allocation->MarkPending();
        return rstd::Ok(owe::resource_registry::PreparedImageAllocation {
            .allocation = rstd::move(allocation),
            .upload     = transparent
                              ? rstd::Some(owe::vulkan::ImageUploadTicket { .value = generation })
                              : rstd::None<owe::vulkan::ImageUploadTicket>(),
        });
    }

    auto InitializeTexture(rstd::sync::Arc<owe::resource_registry::TextureAllocation> allocation)
        -> rstd::Result<owe::resource_registry::PreparedImageAllocation,
                        owe::resource::ResourceError> {
        if (allocation->InitialReadiness().unwrap() !=
            owe::resource_registry::ImageReadiness::Allocated)
            return rstd::Ok(owe::resource_registry::PreparedImageAllocation {
                rstd::move(allocation), rstd::None() });
        ++transparent_creates;
        ++generation;
        allocation->MarkPending();
        return rstd::Ok(owe::resource_registry::PreparedImageAllocation {
            rstd::move(allocation),
            rstd::Some(owe::vulkan::ImageUploadTicket { .value = generation }) });
    }
};

auto ShaderRequest(rstd::uint64_t version) -> owe::resource::ShaderRequest {
    return owe::resource::ShaderRequest {
        .name = rstd::string::String::make("sprite"_str),
        .source =
            owe::resource::ShaderDefinitionId {
                .index      = rstd::u32(3),
                .generation = rstd::u64(1),
            },
        .content_version = rstd::u64(version),
    };
}

auto FakeNativeMetadata(rstd::slice<owe::resource_registry::TextureSlot> slots)
    -> owe::vulkan::ImageSlotsRef {
    owe::vulkan::ImageSlotsRef native;
    for (const auto& slot : slots) {
        const auto& view = slot.view.description();
        auto extent      = tex::mip_extent(slot.view.texture().description(), view.range.first_mip);
        owe::vulkan::ImageParameters image;
        image.extent       = { extent.width, extent.height, extent.depth };
        image.mipmap_level = view.range.mip_count;
        image.generation   = rstd::u64(slot.view.texture().identity().resource);
        native.slots.push(rstd::move(image));
    }
    return native;
}

auto TextureAllocation(
    rstd::uint64_t generation, rstd::sync::Arc<rstd::dyn<rstd::any::Any>> backing,
    rstd::Option<rstd::sync::Arc<rstd::dyn<owe::resource_registry::TextureProducer>>> producer,
    tex::TextureDesc desc, tex::SamplerDesc sampler_desc)
    -> rstd::sync::Arc<owe::resource_registry::TextureAllocation> {
    tex::TextureRef texture({ 1, 1, generation }, desc, backing.clone());
    auto            view_desc =
        tex::normalize(desc, { tex::Dimension::D2, { tex::format_layout(desc.format).aspect } })
            .unwrap();
    tex::TextureViewRef view(texture, view_desc, backing.clone());
    tex::SamplerRef     sampler({ 1, 1, generation }, sampler_desc, rstd::move(backing));
    rstd::vec::Vec<owe::resource_registry::TextureSlot> slots;
    slots.push({ view,
                 rstd::move(sampler),
                 tex::has(desc.usage, tex::Usage::ColorAttachment) ||
                         tex::has(desc.usage, tex::Usage::DepthAttachment)
                     ? view
                     : tex::TextureViewRef {} });
    auto native = FakeNativeMetadata(slots.as_slice());
    return rstd::sync::Arc<owe::resource_registry::TextureAllocation>::make(
        rstd::move(slots), rstd::move(native), rstd::move(producer));
}

struct TextureCommandLog {
    rstd::vec::Vec<tex::TextureUpload>   uploads;
    rstd::vec::Vec<tex::TextureAccess>   accesses;
    rstd::vec::Vec<tex::SamplerRef>      samplers;
    rstd::vec::Vec<tex::SubmissionToken> waits;
};

struct TextureCommands {
    rstd::sync::Arc<TextureCommandLog> log = rstd::sync::Arc<TextureCommandLog>::make();
    rstd::vec::Vec<rstd::boxed::Box<rstd::dyn<rstd::any::Any>>> retained;

    auto Upload(tex::TextureUpload upload) -> rstd::Result<rstd::empty, tex::Error> {
        auto checked = tex::validate_upload(upload);
        if (checked.is_err()) return rstd::Err(checked.unwrap_err());
        log->uploads.push(rstd::move(upload));
        return rstd::Ok(rstd::empty {});
    }
    auto UseTexture(tex::TextureAccess access) -> rstd::Result<rstd::empty, tex::Error> {
        if (! access.view.valid() || ! access.view.permits(tex::writes(access.access)))
            return rstd::Err(tex::Error { tex::ErrorKind::Invalid });
        log->accesses.push(rstd::move(access));
        return rstd::Ok(rstd::empty {});
    }
    auto UseSampledView(tex::TextureViewRef view, tex::SamplerRef sampler, tex::Stages stages)
        -> rstd::Result<rstd::empty, tex::Error> {
        if (! sampler.valid()) return rstd::Err(tex::Error { tex::ErrorKind::Invalid });
        auto used = UseTexture({ rstd::move(view), tex::Access::Sampled, stages });
        if (used.is_ok()) log->samplers.push(rstd::move(sampler));
        return used;
    }
    void Retain(rstd::boxed::Box<rstd::dyn<rstd::any::Any>> owner) {
        retained.push(rstd::move(owner));
    }
    auto Backend() -> rstd::mut_ref<rstd::dyn<rstd::any::Any>> {
        return rstd::dyn<rstd::any::Any>::from_ref(*this).as_mut_ref();
    }
    auto Wait(tex::SubmissionToken completion, tex::Stages)
        -> rstd::Result<rstd::empty, tex::Error> {
        log->waits.push(rstd::move(completion));
        return rstd::Ok(rstd::empty {});
    }
};

struct SubmissionProgress {
    tex::Serial submitted {};
    tex::Serial completed {};
};

struct SubmissionDependency {
    rstd::sync::Arc<SubmissionProgress> progress;
    tex::Serial                         value;

    auto IdentityValue() const -> tex::Identity { return { 1, 1, 1 }; }
    auto Value() const -> tex::Serial { return value; }
    auto Poll() const -> rstd::Result<bool, tex::Error> {
        return rstd::Ok(progress->completed >= value);
    }
    auto Backend() const -> rstd::ref<rstd::dyn<rstd::any::Any>> {
        return rstd::dyn<rstd::any::Any>::from_ref(*this).as_ref();
    }
};

struct SubmissionQueue {
    rstd::sync::Arc<SubmissionProgress> progress = rstd::sync::Arc<SubmissionProgress>::make();
    rstd::vec::Vec<tex::Submission>     pending;
    bool                                fail {};

    auto Begin() -> rstd::Result<tex::Submission, tex::Error> {
        return rstd::Ok(tex::Submission(
            rstd::boxed::Box<rstd::dyn<tex::TextureCommands>>::make(TextureCommands {})));
    }
    auto Submit(tex::Submission work) -> rstd::Result<tex::SubmissionToken, tex::Error> {
        if (fail) return rstd::Err(tex::Error { tex::ErrorKind::Backend });
        tex::BackendAccess::Submitted(work);
        ++progress->submitted;
        pending.push(rstd::move(work));
        return rstd::Ok(tex::SubmissionToken(rstd::sync::Arc<rstd::dyn<tex::Dependency>>::make(
            SubmissionDependency { progress.clone(), progress->submitted })));
    }
    auto Poll() -> rstd::Result<rstd::empty, tex::Error> {
        if (progress->completed == progress->submitted) pending.clear();
        return rstd::Ok(rstd::empty {});
    }
};

auto PreparedTexture(rstd::sync::Arc<owe::resource_registry::TextureAllocation> allocation,
                     rstd::u64 generation = rstd::u64(1))
    -> owe::resource_registry::PreparedTexture {
    return owe::resource_registry::PreparedTexture {
        .use      = owe::resource::TextureUseHandle { .generation = generation },
        .resource = owe::resource::TextureHandle { .generation = rstd::u64(1) },
        .definition =
            owe::resource::TextureRequest {
                .kind = owe::resource::TextureRequestKind::Imported,
                .name = rstd::string::String::make("leased"_str),
            },
        .physical = allocation.clone(),
        .image    = allocation->View(),
    };
}

auto GpuTextures() -> owe::resource_registry::RegistryTextureContext {
    auto session = vrento::vulkan::DeviceSession::create({ .validation = true }).unwrap();
    return owe::resource_registry::RegistryTextureContext(
        vrento::vulkan::TextureContext::create(rstd::move(session)).unwrap());
}

auto GpuTexture(owe::resource_registry::RegistryTextureContext& textures,
                owe::resource::TextureDefinition definition = { .width  = rstd::i32(4),
                                                                .height = rstd::i32(4) })
    -> rstd::sync::Arc<owe::resource_registry::TextureAllocation> {
    auto prepared = textures.AllocateTexture(definition, false).unwrap();
    return rstd::move(prepared.allocation);
}

auto UploadPixels(tex::Submission& work, const owe::resource_registry::PreparedTexture& prepared,
                  rstd::u8 value) -> rstd::Result<rstd::empty, tex::Error> {
    tex::TextureUpload upload;
    upload.target     = prepared.physical->Slots()[rstd::usize()].view;
    const auto extent = upload.target.texture().description().extent;
    upload.width      = extent.width;
    upload.height     = extent.height;
    rstd::prelude::Vec<rstd::u8> bytes;
    bytes.resize(rstd::usize(extent.width) * rstd::usize(extent.height) * rstd::usize(4), value);
    upload.bytes = rstd::move(bytes);
    return work.commands()->Upload(rstd::move(upload));
}

auto CopyPixels(vrento::vulkan::TextureContext& context, tex::Submission& work,
                const owe::resource_registry::PreparedTexture& prepared) -> vvk::AllocatedBuffer {
    if (! owe::resource_registry::UsePreparedTexture(
            work, prepared, owe::resource_registry::TextureStateKind::TransferSource))
        rstd::panic("failed to record texture readback");
    const auto&        view   = prepared.physical->Slots()[rstd::usize()].view;
    const auto         extent = view.texture().description().extent;
    VkBufferCreateInfo info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size                 = extent.width * extent.height * 4;
    info.usage                = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    auto              buffer  = context.session()
                                    .device()
                                    .memory_allocator()
                                    .create_buffer(info, vvk::MemoryRequest::Readback())
                                    .unwrap();
    const auto        command = vrento::vulkan::native_recording(work, context.session()).unwrap();
    VkBufferImageCopy region {};
    region.imageSubresource = { view.description().range.aspect == tex::Aspect::Depth
                                    ? VK_IMAGE_ASPECT_DEPTH_BIT
                                    : VK_IMAGE_ASPECT_COLOR_BIT,
                                0,
                                0,
                                1 };
    region.imageExtent      = { extent.width, extent.height, 1 };
    context.session().device().handle().Dispatch().vkCmdCopyImageToBuffer(
        command,
        vrento::vulkan::native_texture(view.texture()).unwrap(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        buffer.handle(),
        1,
        &region);
    work.retain(buffer.clone());
    return buffer;
}

auto ReadPixels(const vvk::AllocatedBuffer& buffer, rstd::usize size) -> rstd::vec::Vec<rstd::u8> {
    auto memory = buffer.allocation();
    auto mapped = memory.map().unwrap();
    memory.invalidate().unwrap();
    const auto*              bytes = reinterpret_cast<const unsigned char*>(mapped.data());
    rstd::vec::Vec<rstd::u8> pixels;
    for (rstd::usize index {}; index < size; ++index)
        pixels.push(rstd::u8(bytes[index.to_primitive()]));
    return pixels;
}

} // namespace

TEST(RenderResourceSystem, FontAtlasWithoutResidentTextureDefersUpload) {
    owe::resource_registry::RenderResourceSystem resources;
    SubmissionQueue                              queue;
    auto                                         work = queue.Begin().unwrap();
    EXPECT_EQ(resources.QueueRegion(work, "_text_atlas_unused"_str, {}),
              owe::resource_registry::FontAtlasUploadStatus::NotResident);
}

TEST(TextureRegistry, OwnsLogicalEntriesBehindGenerationalHandles) {
    owe::resource::TextureRegistry registry;
    auto                           handle = registry.Register(owe::resource::TextureRequest {
        .kind       = owe::resource::TextureRequestKind::RenderTarget,
        .name       = rstd::string::String::make("frame"_str),
        .definition = rstd::Some(owe::resource::TextureDefinition {
            .width  = rstd::i32(256),
            .height = rstd::i32(128),
        }),
    });
    ASSERT_TRUE(handle.Valid());
    EXPECT_EQ(registry.Size(), rstd::usize(1));

    auto found = registry.Find(owe::resource::TextureRequestKind::RenderTarget, "frame"_str);
    ASSERT_TRUE(found.is_some());
    EXPECT_EQ(*found, handle);

    auto entry = registry.ResolveTexture(handle);
    ASSERT_TRUE(entry.is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view((**entry).request().name.as_str()), "frame");
    EXPECT_EQ((**entry).definition_version, rstd::u64(1));

    auto resized = registry.Register(owe::resource::TextureRequest {
        .kind       = owe::resource::TextureRequestKind::RenderTarget,
        .name       = rstd::string::String::make("frame"_str),
        .definition = rstd::Some(owe::resource::TextureDefinition {
            .width  = rstd::i32(512),
            .height = rstd::i32(128),
        }),
    });
    EXPECT_EQ(resized, handle);
    entry = registry.ResolveTexture(handle);
    ASSERT_TRUE(entry.is_some());
    EXPECT_EQ((**entry).definition_version, rstd::u64(2));

    auto view  = rstd::dyn<owe::resource::TextureLogicalRegistryView>::from_ref(registry);
    auto state = view->ResolveTextureState(handle);
    ASSERT_TRUE(state.is_some());
    EXPECT_EQ(state->definition_version, rstd::u64(2));
    EXPECT_EQ(state->content_version, rstd::u64(1));
}

TEST(TextureRegistry, InvalidatesOldHandlesOnReset) {
    owe::resource::TextureRegistry registry;
    auto                           handle = registry.RegisterImported(
        owe::resource::TextureRequest {
            .kind = owe::resource::TextureRequestKind::Imported,
            .name = rstd::string::String::make("asset"_str),
        },
        owe::resource::ImportedTextureContentIdentity {
            .key = rstd::string::String::make("asset"_str),
        });
    auto generation = registry.Generation();

    registry.Reset();

    EXPECT_GT(registry.Generation(), generation);
    EXPECT_TRUE(registry.ResolveTexture(handle).is_none());
    EXPECT_EQ(registry.Size(), rstd::usize());
}

TEST(TextureRegistry, KeepsImportedContentAcrossSnapshotLocators) {
    owe::resource::TextureRegistry registry;
    auto                           request = owe::resource::TextureRequest {
        .kind   = owe::resource::TextureRequestKind::Imported,
        .name   = rstd::string::String::make("asset"_str),
        .source = rstd::Some(owe::resource::TextureDefinitionId {
            .index      = rstd::u32(3),
            .generation = rstd::u64(11),
        }),
    };
    auto content = owe::resource::ImportedTextureContentIdentity {
        .key = rstd::string::String::make("asset-content"_str),
    };
    auto handle = registry.RegisterImported(request.clone(), content.clone());
    ASSERT_TRUE(registry.Publish(handle, TextureAllocation(7)).is_some());

    request.source->generation = rstd::u64(12);
    EXPECT_EQ(registry.RegisterImported(request.clone(), content.clone()), handle);
    EXPECT_TRUE(registry.ResolveCurrent(handle).is_some());
    auto logical = registry.ResolveTexture(handle);
    ASSERT_TRUE(logical.is_some());
    EXPECT_EQ((**logical).content_version, rstd::u64(1));
    EXPECT_EQ((**logical).request().source->generation, rstd::u64(12));
}

TEST(TextureRegistry, InvalidatesImportedPhysicalOnContentRevisionChange) {
    owe::resource::TextureRegistry registry;
    auto                           request = owe::resource::TextureRequest {
        .kind = owe::resource::TextureRequestKind::Imported,
        .name = rstd::string::String::make("asset"_str),
    };
    auto content = owe::resource::ImportedTextureContentIdentity {
        .key      = rstd::string::String::make("asset-content"_str),
        .revision = rstd::u64(4),
    };
    auto handle = registry.RegisterImported(request.clone(), content.clone());
    ASSERT_TRUE(registry.Publish(handle, TextureAllocation(7)).is_some());

    content.revision = rstd::u64(5);
    EXPECT_EQ(registry.RegisterImported(request.clone(), content.clone()), handle);
    EXPECT_TRUE(registry.ResolveCurrent(handle).is_none());
    auto logical = registry.ResolveTexture(handle);
    ASSERT_TRUE(logical.is_some());
    EXPECT_EQ((**logical).content_version, rstd::u64(2));
}

TEST(TextureRegistry, RestoresLogicalAndPhysicalStateAfterPrepareAbort) {
    owe::resource::TextureRegistry registry;
    auto                           request = owe::resource::TextureRequest {
        .kind = owe::resource::TextureRequestKind::Imported,
        .name = rstd::string::String::make("asset"_str),
    };
    auto first_content = owe::resource::ImportedTextureContentIdentity {
        .key      = rstd::string::String::make("first"_str),
        .revision = rstd::u64(1),
    };
    auto handle = registry.RegisterImported(request.clone(), first_content.clone());
    ASSERT_TRUE(registry.Publish(handle, TextureAllocation(7)).is_some());
    ASSERT_TRUE(registry.BeginPrepareTransaction());

    auto second_content = owe::resource::ImportedTextureContentIdentity {
        .key      = rstd::string::String::make("second"_str),
        .revision = rstd::u64(2),
    };
    EXPECT_EQ(registry.RegisterImported(request.clone(), second_content.clone()), handle);
    ASSERT_TRUE(registry.Publish(handle, TextureAllocation(8)).is_some());
    auto current = registry.ResolveCurrent(handle);
    ASSERT_TRUE(current.is_some());
    EXPECT_EQ((**current).allocation->View().getActive().generation, rstd::u64(8));

    registry.AbortPrepareTransaction();
    auto logical  = registry.ResolveTexture(handle);
    auto physical = registry.ResolveCurrent(handle);
    ASSERT_TRUE(logical.is_some());
    ASSERT_TRUE(physical.is_some());
    ASSERT_TRUE((**logical).imported_content.is_some());
    EXPECT_EQ((**logical).imported_content->key, "first"_str);
    EXPECT_EQ((**logical).content_version, rstd::u64(1));
    EXPECT_EQ((**physical).allocation->View().getActive().generation, rstd::u64(7));
}

TEST(TextureRegistry, ReleasesPhysicalResourcesOutsideTheActivePlan) {
    owe::resource::TextureRegistry registry;
    auto                           first = registry.RegisterImported(
        owe::resource::TextureRequest {
            .kind = owe::resource::TextureRequestKind::Imported,
            .name = rstd::string::String::make("first"_str),
        },
        owe::resource::ImportedTextureContentIdentity {
            .key = rstd::string::String::make("first"_str),
        });
    auto second = registry.RegisterImported(
        owe::resource::TextureRequest {
            .kind = owe::resource::TextureRequestKind::Imported,
            .name = rstd::string::String::make("second"_str),
        },
        owe::resource::ImportedTextureContentIdentity {
            .key = rstd::string::String::make("second"_str),
        });
    ASSERT_TRUE(registry.Publish(first, TextureAllocation(1)).is_some());
    ASSERT_TRUE(registry.Publish(second, TextureAllocation(2)).is_some());

    auto active = rstd::array<owe::resource::TextureHandle, 1> { first };
    registry.RetainActive(active.as_slice());
    EXPECT_TRUE(registry.Resolve(first).is_some());
    EXPECT_TRUE(registry.Resolve(second).is_none());
}

TEST(TextureAllocation, OwnsAttachedRuntimeForItsWholeLeaseLifetime) {
    auto weak = rstd::sync::Weak<rstd::dyn<owe::resource_registry::TextureProducer>>::make();
    {
        auto runtime = rstd::sync::Arc<rstd::dyn<owe::resource_registry::TextureProducer>>::make(
            TextureRuntimeProbe {});
        weak            = runtime.downgrade();
        auto allocation = TextureAllocation(
            1, rstd::sync::Arc<rstd::dyn<rstd::any::Any>>::make(0), rstd::Some(runtime.clone()));
        runtime.reset();
        EXPECT_FALSE(weak.expired());
        EXPECT_TRUE(static_cast<bool>(allocation));
    }
    EXPECT_TRUE(weak.expired());
}

TEST(TextureAllocation, SubmissionLeaseDelaysRuntimeReleaseAfterActivePlanRemoval) {
    auto weak = rstd::sync::Weak<rstd::dyn<owe::resource_registry::TextureProducer>>::make();
    SubmissionQueue      queue;
    tex::SubmissionToken completion;
    {
        auto runtime = rstd::sync::Arc<rstd::dyn<owe::resource_registry::TextureProducer>>::make(
            TextureRuntimeProbe {});
        weak            = runtime.downgrade();
        auto allocation = TextureAllocation(
            1, rstd::sync::Arc<rstd::dyn<rstd::any::Any>>::make(0), rstd::Some(runtime.clone()));

        owe::resource::TextureRegistry registry;
        auto                           handle = registry.RegisterImported(
            owe::resource::TextureRequest {
                .kind = owe::resource::TextureRequestKind::Imported,
                .name = rstd::string::String::make("video"_str),
            },
            owe::resource::ImportedTextureContentIdentity {
                .key = rstd::string::String::make("video"_str),
            });
        ASSERT_TRUE(registry.Publish(handle, allocation.clone()).is_some());

        owe::resource_registry::PreparedResourceTable table(rstd::u64(3));
        ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedTexture {
            .use      = owe::resource::TextureUseHandle { .generation = rstd::u64(3) },
            .resource = handle,
            .definition =
                owe::resource::TextureRequest {
                    .kind = owe::resource::TextureRequestKind::Imported,
                    .name = rstd::string::String::make("video"_str),
                },
            .physical = allocation.clone(),
            .image    = allocation->View(),
        }));
        auto work = queue.Begin().unwrap();
        work.retain(table.Leases());
        completion = queue.Submit(rstd::move(work)).unwrap();
        ASSERT_TRUE(completion.valid());
        EXPECT_FALSE(completion.poll().unwrap());

        table       = owe::resource_registry::PreparedResourceTable {};
        auto active = rstd::vec::Vec<owe::resource::TextureHandle>::make();
        registry.RetainActive(active.as_slice());
        allocation.reset();
        runtime.reset();
        EXPECT_FALSE(weak.expired());
    }

    ASSERT_TRUE(queue.Poll().is_ok());
    EXPECT_FALSE(weak.expired());
    queue.progress->completed = completion.value();
    ASSERT_TRUE(completion.poll().unwrap());
    EXPECT_FALSE(weak.expired());
    ASSERT_TRUE(queue.Poll().is_ok());
    EXPECT_TRUE(weak.expired());
}

TEST(TextureAllocation, TypedReferencesRetainBackingBeyondNativeMetadata) {
    auto backing    = rstd::sync::Arc<rstd::dyn<rstd::any::Any>>::make(7);
    auto weak       = backing.downgrade();
    auto allocation = TextureAllocation(7, backing.clone());
    auto native     = allocation->View();
    ASSERT_EQ(allocation->Slots().len(), rstd::usize(1));
    auto view    = allocation->Slots()[rstd::usize()].view;
    auto sampler = allocation->Slots()[rstd::usize()].sampler;
    ASSERT_TRUE(view.valid());
    ASSERT_TRUE(sampler.valid());
    EXPECT_EQ(view.texture().identity().resource, rstd::uint64_t(7));
    EXPECT_EQ(native.getActive().generation, rstd::u64(view.texture().identity().resource));
    EXPECT_EQ(native.getActive().extent.width, view.texture().description().extent.width);

    allocation.reset();
    backing.reset();
    EXPECT_FALSE(weak.expired());
    view = {};
    EXPECT_FALSE(weak.expired());
    sampler = {};
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(native.slots.len(), rstd::usize(1));
}

TEST(TextureRegistry, PublishesVersionedPhysicalGenerations) {
    owe::resource::TextureRegistry registry;
    auto                           handle = registry.Register(owe::resource::TextureRequest {
        .kind       = owe::resource::TextureRequestKind::RenderTarget,
        .name       = rstd::string::String::make("frame"_str),
        .definition = rstd::Some(owe::resource::TextureDefinition {
            .width  = rstd::i32(256),
            .height = rstd::i32(128),
        }),
    });

    auto allocation = TextureAllocation(7);
    auto first      = registry.Publish(handle, allocation.clone());
    ASSERT_TRUE(first.is_some());
    EXPECT_EQ(*first, rstd::u64(1));

    auto same = registry.Publish(handle, allocation.clone());
    ASSERT_TRUE(same.is_some());
    EXPECT_EQ(*same, rstd::u64(1));

    auto replaced = registry.Publish(handle, TextureAllocation(8));
    ASSERT_TRUE(replaced.is_some());
    EXPECT_EQ(*replaced, rstd::u64(2));

    auto physical = registry.Resolve(handle);
    ASSERT_TRUE(physical.is_some());
    EXPECT_EQ((**physical).generation, rstd::u64(2));
    EXPECT_FALSE((**physical).completion.valid());
}

TEST(TextureRegistry, PublishesUploadedPhysicalOnlyAfterSubmission) {
    owe::resource::TextureRegistry registry;
    auto                           handle = registry.RegisterImported(
        owe::resource::TextureRequest {
            .kind = owe::resource::TextureRequestKind::Imported,
            .name = rstd::string::String::make("asset"_str),
        },
        owe::resource::ImportedTextureContentIdentity {
            .key = rstd::string::String::make("asset"_str),
        });
    owe::vulkan::ImageUploadTicket ticket { .value = rstd::u64(7) };

    auto published = registry.Publish(handle, TextureAllocation(9), rstd::Some(ticket));
    ASSERT_TRUE(published.is_some());
    EXPECT_TRUE(registry.ResolveCurrent(handle).is_none());

    SubmissionQueue queue;
    auto            completion = queue.Submit(queue.Begin().unwrap()).unwrap();
    registry.MarkUploadsSubmitted(
        rstd::slice<owe::vulkan::ImageUploadTicket>::from_raw_parts(&ticket, rstd::usize(1)),
        completion);
    auto physical = registry.ResolveCurrent(handle);
    ASSERT_TRUE(physical.is_some());
    EXPECT_EQ((**physical).generation, rstd::u64(1));
    EXPECT_EQ((**physical).completion, completion);
}

TEST(TextureRegistry, PublishesCompletionBeforeGpuRetiresTheUpload) {
    owe::resource::TextureRegistry registry;
    auto                           handle = registry.RegisterImported(
        owe::resource::TextureRequest {
            .kind = owe::resource::TextureRequestKind::Imported,
            .name = rstd::string::String::make("asset"_str),
        },
        owe::resource::ImportedTextureContentIdentity {
            .key = rstd::string::String::make("asset"_str),
        });
    auto                           allocation = TextureAllocation(9);
    owe::vulkan::ImageUploadTicket ticket { .value = rstd::u64(7) };
    ASSERT_TRUE(registry.Publish(handle, allocation.clone(), rstd::Some(ticket)).is_some());
    EXPECT_TRUE(registry.ResolveCurrent(handle).is_none());
    EXPECT_TRUE(registry.ResolvePending(handle).is_some());
    registry.PublishSubmitted();
    EXPECT_TRUE(registry.ResolveCurrent(handle).is_none());

    SubmissionQueue queue;
    auto            work = queue.Begin().unwrap();
    work.retain(allocation.clone());
    auto receipt    = work.receipt();
    auto completion = queue.Submit(rstd::move(work)).unwrap();
    ASSERT_TRUE(receipt.submitted());
    ASSERT_FALSE(completion.poll().unwrap());
    allocation->MarkSubmitted(completion);
    registry.ReservePublications();
    registry.PublishSubmitted();
    auto current = registry.ResolveCurrent(handle);
    ASSERT_TRUE(current.is_some());
    EXPECT_TRUE(registry.ResolvePending(handle).is_none());
    EXPECT_EQ((**current).completion, completion);
    EXPECT_EQ((**current).allocation->Completion(), completion);
    EXPECT_FALSE((**current).completion.poll().unwrap());
    queue.progress->completed = completion.value();
    ASSERT_TRUE(queue.Poll().is_ok());
    EXPECT_TRUE((**current).completion.poll().unwrap());
}

TEST(TextureRegistry, SubmittedReplacementSurvivesPrepareAbortUntilCompletion) {
    owe::resource::TextureRegistry registry;
    auto                           request = owe::resource::TextureRequest {
        .kind = owe::resource::TextureRequestKind::Imported,
        .name = rstd::string::String::make("asset"_str),
    };
    auto content = owe::resource::ImportedTextureContentIdentity {
        .key      = rstd::string::String::make("asset"_str),
        .revision = rstd::u64(1),
    };
    auto handle = registry.RegisterImported(request.clone(), content.clone());
    ASSERT_TRUE(registry.Publish(handle, TextureAllocation(7)).is_some());
    ASSERT_TRUE(registry.BeginPrepareTransaction());
    content.revision = rstd::u64(2);
    EXPECT_EQ(registry.RegisterImported(request.clone(), content.clone()), handle);
    auto                           backing = rstd::sync::Arc<rstd::dyn<rstd::any::Any>>::make(8);
    auto                           weak    = backing.downgrade();
    auto                           replacement = TextureAllocation(8, rstd::move(backing));
    owe::vulkan::ImageUploadTicket ticket { .value = rstd::u64(8) };
    ASSERT_TRUE(registry.Publish(handle, replacement.clone(), rstd::Some(ticket)).is_some());
    EXPECT_TRUE(registry.ResolveCurrent(handle).is_none());

    SubmissionQueue queue;
    auto            work = queue.Begin().unwrap();
    work.retain(replacement.clone());
    auto completion = queue.Submit(rstd::move(work)).unwrap();
    replacement->MarkSubmitted(completion);
    registry.ReservePublications();
    registry.PublishSubmitted();
    ASSERT_TRUE(registry.ResolveCurrent(handle).is_some());
    registry.AbortPrepareTransaction();
    auto restored = registry.ResolveCurrent(handle);
    ASSERT_TRUE(restored.is_some());
    EXPECT_EQ((**restored).allocation->Slots()[rstd::usize()].view.texture().identity().resource,
              rstd::uint64_t(7));
    EXPECT_EQ((**restored).content_version, rstd::u64(1));
    EXPECT_TRUE(registry.ResolvePending(handle).is_none());
    replacement.reset();
    EXPECT_FALSE(weak.expired());
    ASSERT_FALSE(completion.poll().unwrap());
    queue.progress->completed = completion.value();
    ASSERT_TRUE(queue.Poll().is_ok());
    EXPECT_TRUE(weak.expired());
}

TEST(TextureRegistry, RejectsSubmittedUploadsFromAnObsoleteRevision) {
    owe::resource::TextureRegistry registry;
    auto                           request = owe::resource::TextureRequest {
        .kind = owe::resource::TextureRequestKind::Imported,
        .name = rstd::string::String::make("asset"_str),
    };
    auto content = owe::resource::ImportedTextureContentIdentity {
        .key      = rstd::string::String::make("asset"_str),
        .revision = rstd::u64(1),
    };
    auto handle = registry.RegisterImported(request.clone(), content.clone());
    owe::vulkan::ImageUploadTicket ticket { .value = rstd::u64(1) };
    auto                           allocation = TextureAllocation(1);
    ASSERT_TRUE(registry.Publish(handle, allocation.clone(), rstd::Some(ticket)).is_some());
    SubmissionQueue queue;
    auto            work = queue.Begin().unwrap();
    work.retain(allocation.clone());
    auto completion = queue.Submit(rstd::move(work)).unwrap();
    allocation->MarkSubmitted(completion);
    content.revision = rstd::u64(2);
    EXPECT_EQ(registry.RegisterImported(request.clone(), content.clone()), handle);
    registry.ReservePublications();
    registry.PublishSubmitted();
    EXPECT_TRUE(registry.ResolveCurrent(handle).is_none());
    EXPECT_TRUE(registry.ResolvePending(handle).is_none());
    EXPECT_EQ((**registry.ResolveTexture(handle)).content_version, rstd::u64(2));
}

TEST(FrameSubmissionLease, MixedResourcesRemainOwnedUntilCompletion) {
    auto buffer_owner = rstd::sync::Arc<owe::resource_registry::RegistryBufferOwner>::make(
        vrento::vulkan::DeviceSession::create().unwrap());
    auto owner_weak      = buffer_owner.downgrade();
    auto texture_backing = rstd::sync::Arc<rstd::dyn<rstd::any::Any>>::make(1);
    auto texture_weak    = texture_backing.downgrade();
    auto allocation      = TextureAllocation(1, rstd::move(texture_backing));
    auto buffer          = rstd::sync::Arc<owe::resource_registry::BufferPhysical>::make(
        owe::vulkan::BufferAllocation {}, rstd::u64(1), rstd::u64(1));
    auto                                          buffer_weak = buffer.downgrade();
    owe::resource_registry::PreparedResourceTable table(rstd::u64(1));
    ASSERT_TRUE(table.Insert(PreparedTexture(allocation.clone())));
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedBufferUse {
        .use = owe::resource::BufferUseHandle { .generation = rstd::u64(1) },
        .buffer =
            owe::resource_registry::PreparedBuffer {
                .resource = owe::resource::BufferHandle { .generation = rstd::u64(1) },
                .physical = buffer.clone(),
            },
    }));
    SubmissionQueue queue;
    auto            work    = queue.Begin().unwrap();
    auto            receipt = work.receipt();
    auto            frame   = rstd::sync::Arc<owe::resource_registry::FrameSubmissionLease>::make(
        rstd::move(buffer_owner), table.Leases(), receipt);
    EXPECT_EQ(frame->resources.textures.len(), rstd::usize(1));
    EXPECT_EQ(frame->resources.buffers.len(), rstd::usize(1));
    work.retain(frame.clone());
    auto completion = queue.Submit(rstd::move(work)).unwrap();
    ASSERT_TRUE(receipt.submitted());
    frame.reset();
    table = owe::resource_registry::PreparedResourceTable {};
    allocation.reset();
    buffer.reset();
    EXPECT_FALSE(texture_weak.expired());
    EXPECT_FALSE(buffer_weak.expired());
    EXPECT_FALSE(owner_weak.expired());
    ASSERT_TRUE(queue.Poll().is_ok());
    EXPECT_FALSE(texture_weak.expired());
    EXPECT_FALSE(buffer_weak.expired());
    EXPECT_FALSE(owner_weak.expired());
    queue.progress->completed = completion.value();
    ASSERT_TRUE(queue.Poll().is_ok());
    EXPECT_TRUE(texture_weak.expired());
    EXPECT_TRUE(buffer_weak.expired());
    EXPECT_TRUE(owner_weak.expired());
}

TEST(FrameSubmissionLease, CancelledMixedRecordingReleasesItsOwners) {
    auto buffer_owner = rstd::sync::Arc<owe::resource_registry::RegistryBufferOwner>::make(
        vrento::vulkan::DeviceSession::create().unwrap());
    auto owner_weak      = buffer_owner.downgrade();
    auto texture_backing = rstd::sync::Arc<rstd::dyn<rstd::any::Any>>::make(1);
    auto texture_weak    = texture_backing.downgrade();
    auto allocation      = TextureAllocation(1, rstd::move(texture_backing));
    auto buffer          = rstd::sync::Arc<owe::resource_registry::BufferPhysical>::make(
        owe::vulkan::BufferAllocation {}, rstd::u64(1), rstd::u64(1));
    auto                                          buffer_weak = buffer.downgrade();
    owe::resource_registry::PreparedResourceTable table(rstd::u64(1));
    ASSERT_TRUE(table.Insert(PreparedTexture(allocation.clone())));
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedBufferUse {
        .use = owe::resource::BufferUseHandle { .generation = rstd::u64(1) },
        .buffer =
            owe::resource_registry::PreparedBuffer {
                .resource = owe::resource::BufferHandle { .generation = rstd::u64(1) },
                .physical = buffer.clone(),
            },
    }));
    SubmissionQueue queue;
    queue.fail   = true;
    auto work    = queue.Begin().unwrap();
    auto receipt = work.receipt();
    auto frame   = rstd::sync::Arc<owe::resource_registry::FrameSubmissionLease>::make(
        rstd::move(buffer_owner), table.Leases(), receipt);
    work.retain(frame.clone());
    frame.reset();
    table = owe::resource_registry::PreparedResourceTable {};
    allocation.reset();
    buffer.reset();
    EXPECT_FALSE(texture_weak.expired());
    EXPECT_FALSE(buffer_weak.expired());
    EXPECT_FALSE(owner_weak.expired());
    EXPECT_TRUE(queue.Submit(rstd::move(work)).is_err());
    EXPECT_TRUE(receipt.cancelled());
    EXPECT_FALSE(receipt.submitted());
    EXPECT_TRUE(texture_weak.expired());
    EXPECT_TRUE(buffer_weak.expired());
    EXPECT_TRUE(owner_weak.expired());
}

TEST(ShaderRegistry, OwnsArtifactsByRequestIdentity) {
    owe::resource_registry::ShaderRegistry registry;
    ShaderArtifactProvider                 provider;
    auto object = rstd::dyn<owe::resource::ShaderArtifactProvider>::from_ref(provider);

    auto first = registry.Ensure(ShaderRequest(4), object);
    ASSERT_TRUE(first.is_ok());
    auto handle = rstd::move(first).unwrap_unchecked();
    EXPECT_EQ(provider.loads, rstd::usize(1));

    auto same = registry.Ensure(ShaderRequest(4), object);
    ASSERT_TRUE(same.is_ok());
    EXPECT_EQ(rstd::move(same).unwrap_unchecked(), handle);
    EXPECT_EQ(provider.loads, rstd::usize(1));

    auto changed = registry.Ensure(ShaderRequest(5), object);
    ASSERT_TRUE(changed.is_ok());
    auto changed_handle = rstd::move(changed).unwrap_unchecked();
    EXPECT_NE(changed_handle, handle);
    EXPECT_EQ(provider.loads, rstd::usize(2));
    EXPECT_EQ(registry.Size(), rstd::usize(2));

    auto first_entry = registry.Resolve(handle);
    ASSERT_TRUE(first_entry.is_some());
    EXPECT_EQ((**first_entry).physical->artifact.content_version, rstd::u64(4));
    auto changed_entry = registry.Resolve(changed_handle);
    ASSERT_TRUE(changed_entry.is_some());
    EXPECT_EQ((**changed_entry).physical->artifact.content_version, rstd::u64(5));

    registry.Reset();
    EXPECT_TRUE(registry.Resolve(handle).is_none());
    EXPECT_TRUE(registry.Resolve(changed_handle).is_none());
}

TEST(BufferRegistry, OwnsLogicalDefinitionsBehindStableHandles) {
    owe::resource_registry::BufferRegistry registry;
    auto                                   first = registry.Declare(owe::resource::BufferRequest {
        .name       = rstd::string::String::make("vertices"_str),
        .definition = { .size = rstd::usize(128), .usage = owe::resource::BufferUsage::Vertex },
        .content_version = rstd::u64(3),
    });
    ASSERT_TRUE(first.Valid());

    auto changed = registry.Declare(owe::resource::BufferRequest {
        .name       = rstd::string::String::make("vertices"_str),
        .definition = { .size = rstd::usize(256), .usage = owe::resource::BufferUsage::Vertex },
        .content_version = rstd::u64(4),
    });
    EXPECT_EQ(changed, first);
    auto entry = registry.ResolveBuffer(first);
    ASSERT_TRUE(entry.is_some());
    EXPECT_EQ((**entry).definition_version, rstd::u64(2));
    EXPECT_EQ((**entry).content_version, rstd::u64(2));

    registry.Reset();
    EXPECT_TRUE(registry.ResolveBuffer(first).is_none());
}

TEST(BufferRegistry, UpdatesDynamicContentWithoutReplacingItsPlannedCapacity) {
    owe::resource_registry::BufferRegistry registry;
    BufferBackend                          upload_backend;
    auto upload  = rstd::dyn<owe::vulkan::BufferBackend>::from_ref(upload_backend);
    auto content = rstd::vec::Vec<rstd::u8>::make();
    content.push(rstd::u8(1));
    content.push(rstd::u8(2));
    auto request = owe::resource::BufferRequest {
        .name       = rstd::string::String::make("dynamic-vertices"_str),
        .definition = { .size      = rstd::usize(128),
                        .usage     = owe::resource::BufferUsage::Vertex,
                        .alignment = rstd::usize(16) },
        .lifetime   = owe::resource::BufferLifetimeClass::Dynamic,
    };

    auto prepared = registry.Ensure(request.clone(), content.as_slice(), upload.as_mut_ref());
    ASSERT_TRUE(prepared.is_ok());
    auto buffer = rstd::move(prepared).unwrap_unchecked();
    EXPECT_EQ(upload_backend.last_request.size, 128u);
    EXPECT_EQ(upload_backend.last_request.alignment, 16u);
    EXPECT_EQ(upload_backend.last_request.usage, owe::vulkan::BufferUploadClass::Vertex);

    content.push(rstd::u8(3));
    auto updated = registry.Update(buffer.resource, content.as_slice(), upload.as_mut_ref());
    ASSERT_TRUE(updated.is_ok());
    EXPECT_EQ(upload_backend.allocations, rstd::usize(1));
    EXPECT_EQ(upload_backend.writes, rstd::usize(2));
    auto physical = registry.Resolve(buffer.resource);
    ASSERT_TRUE(physical.is_some());
    EXPECT_EQ((**physical).source_generation, rstd::u64(2));

    request.content_version = rstd::u64(7);
    content[rstd::usize()]  = rstd::u8(4);
    auto prepared_again = registry.Ensure(request.clone(), content.as_slice(), upload.as_mut_ref());
    ASSERT_TRUE(prepared_again.is_ok());
    EXPECT_EQ(upload_backend.allocations, rstd::usize(1));
    EXPECT_EQ(upload_backend.writes, rstd::usize(2));

    updated = registry.Update(buffer.resource, content.as_slice(), upload.as_mut_ref());
    ASSERT_TRUE(updated.is_ok());
    EXPECT_EQ(upload_backend.writes, rstd::usize(3));
    physical = registry.Resolve(buffer.resource);
    ASSERT_TRUE(physical.is_some());
    EXPECT_EQ((**physical).source_generation, rstd::u64(3));
}

TEST(BufferRegistry, ReusesPhysicalAllocationAcrossPreparedContentVersions) {
    owe::resource_registry::BufferRegistry registry;
    BufferBackend                          backend;
    auto buffer_backend = rstd::dyn<owe::vulkan::BufferBackend>::from_ref(backend);
    auto content        = rstd::vec::Vec<rstd::u8>::make();
    content.push(rstd::u8(1));
    auto request = owe::resource::BufferRequest {
        .name            = rstd::string::String::make("retained-vertices"_str),
        .definition      = { .size = rstd::usize(64), .usage = owe::resource::BufferUsage::Vertex },
        .content_version = rstd::u64(1),
    };

    auto first = registry.Ensure(request.clone(), content.as_slice(), buffer_backend.as_mut_ref());
    ASSERT_TRUE(first.is_ok());
    auto first_physical = rstd::move(first).unwrap_unchecked().physical;

    request.content_version = rstd::u64(2);
    content[rstd::usize()]  = rstd::u8(2);
    auto second = registry.Ensure(request.clone(), content.as_slice(), buffer_backend.as_mut_ref());
    ASSERT_TRUE(second.is_ok());
    auto second_physical = rstd::move(second).unwrap_unchecked().physical;
    EXPECT_EQ(first_physical.as_ptr().as_raw_ptr(), second_physical.as_ptr().as_raw_ptr());
    EXPECT_EQ(backend.allocations, rstd::usize(1));
    EXPECT_EQ(backend.writes, rstd::usize(2));

    request.definition.size = rstd::usize(128);
    request.content_version = rstd::u64(3);
    auto replaced =
        registry.Ensure(rstd::move(request), content.as_slice(), buffer_backend.as_mut_ref());
    ASSERT_TRUE(replaced.is_ok());
    auto replacement = rstd::move(replaced).unwrap_unchecked().physical;
    EXPECT_NE(first_physical.as_ptr().as_raw_ptr(), replacement.as_ptr().as_raw_ptr());
    EXPECT_EQ(replacement->generation, rstd::u64(2));
    EXPECT_EQ(backend.allocations, rstd::usize(2));
}

TEST(BufferRegistry, UploadsReplacementSourceWithMatchingVersionAndCapacity) {
    owe::resource_registry::BufferRegistry registry;
    BufferBackend                          backend;
    auto buffer_backend = rstd::dyn<owe::vulkan::BufferBackend>::from_ref(backend);
    auto content        = rstd::vec::Vec<rstd::u8>::make();
    content.push(rstd::u8(1));
    auto request = owe::resource::BufferRequest {
        .name            = "effect-vertices"_Str,
        .definition      = { .size = rstd::usize(64), .usage = owe::resource::BufferUsage::Vertex },
        .content_version = rstd::u64(3),
        .content_identity = rstd::u64(10),
    };
    auto first = registry.Ensure(request.clone(), content.as_slice(), buffer_backend.as_mut_ref());
    ASSERT_TRUE(first.is_ok());
    auto prepared = rstd::move(first).unwrap_unchecked();
    for (auto identity : { 20, 10, 20, 10 }) {
        request.content_identity = rstd::u64(identity);
        auto changed =
            registry.Ensure(request.clone(), content.as_slice(), buffer_backend.as_mut_ref());
        ASSERT_TRUE(changed.is_ok());
        EXPECT_EQ(changed->resource, prepared.resource);
        EXPECT_EQ(changed->physical.as_ptr().as_raw_ptr(), prepared.physical.as_ptr().as_raw_ptr());
        EXPECT_EQ(changed->physical->source_identity, request.content_identity);
        auto writes = backend.writes;
        ASSERT_TRUE(
            registry.Ensure(request.clone(), content.as_slice(), buffer_backend.as_mut_ref())
                .is_ok());
        EXPECT_EQ(backend.writes, writes);
    }
    EXPECT_EQ(backend.allocations, rstd::usize(1));
    EXPECT_EQ(backend.writes, rstd::usize(5));
    auto entry = registry.ResolveBuffer(prepared.resource);
    ASSERT_TRUE(entry.is_some());
    EXPECT_EQ((**entry).content_version, rstd::u64(5));
}

TEST(ResourcePrepareService, RetainsPublishedShaderArtifactAcrossPlanGenerations) {
    owe::resource::TextureRegistry         textures;
    owe::resource_registry::BufferRegistry buffers;
    owe::resource_registry::ShaderRegistry shaders;
    BufferBackend                          backend;
    auto upload = rstd::dyn<owe::vulkan::BufferBackend>::from_ref(backend);
    owe::resource_registry::ResourcePrepareService service(
        textures, rstd::None(), buffers, upload.as_mut_ref(), shaders);
    auto request = ShaderRequest(5);
    auto artifact =
        rstd::sync::Arc<owe::resource::ShaderArtifact>::make(owe::resource::ShaderArtifact {
            .source = request.source, .content_version = request.content_version });
    owe::resource::ResourcePlan plan { .generation = rstd::u64(12) };
    plan.shaders.push({
        .handle   = { rstd::u64(0), plan.generation },
        .request  = request.clone(),
        .artifact = rstd::Some(artifact.clone()),
    });
    auto first = service.Prepare(plan, {}, owe::resource::ResourcePlanShaders).unwrap();
    auto old   = first.Resolve(plan.shaders[rstd::usize()].handle).unwrap();
    EXPECT_EQ(&old->shader.physical->artifact, &*artifact);

    auto next               = plan.clone();
    next.generation         = rstd::u64(13);
    auto& entry             = next.shaders[rstd::usize()];
    entry.handle.generation = next.generation;
    auto prepared = service.Prepare(next, {}, owe::resource::ResourcePlanShaders).unwrap();
    auto current  = prepared.Resolve(entry.handle).unwrap();
    EXPECT_EQ(&*current->shader.physical, &*old->shader.physical);
    EXPECT_EQ(&current->shader.physical->artifact, &*artifact);
    auto replacement = rstd::sync::Arc<owe::resource::ShaderArtifact>::make(artifact->clone());
    entry.artifact   = rstd::Some(replacement.clone());
    auto reused      = service.Prepare(next, {}, owe::resource::ResourcePlanShaders).unwrap();
    EXPECT_EQ(&reused.Resolve(entry.handle).unwrap()->shader.physical->artifact, &*artifact);
    entry.request.content_version += rstd::u64(1);
    auto rejected = service.Prepare(next, {}, owe::resource::ResourcePlanShaders);
    ASSERT_TRUE(rejected.is_err());
    EXPECT_EQ(rejected.unwrap_err().kind, owe::resource::ResourceErrorKind::MissingDefinition);
}

TEST(ResourcePrepareService, VisitsBufferAndShaderPlansThroughTypedProviders) {
    owe::resource::TextureRegistry         textures;
    owe::resource_registry::BufferRegistry buffers;
    owe::resource_registry::ShaderRegistry shaders;
    BufferBackend                          upload_backend;
    BufferContentProvider                  buffer_provider;
    ShaderArtifactProvider                 shader_provider;
    auto upload = rstd::dyn<owe::vulkan::BufferBackend>::from_ref(upload_backend);
    auto buffer = rstd::dyn<owe::resource::BufferContentProvider>::from_ref(buffer_provider);
    auto shader = rstd::dyn<owe::resource::ShaderArtifactProvider>::from_ref(shader_provider);

    owe::resource::ResourcePlan plan { .generation = rstd::u64(12) };
    plan.buffers.push(owe::resource::BufferPlanEntry {
        .handle =
            owe::resource::BufferUseHandle { .index = rstd::u64(3), .generation = rstd::u64(12) },
        .definition =
            owe::resource::BufferRequest {
                .name            = rstd::string::String::make("vertices"_str),
                .definition      = { .size  = rstd::usize(2),
                                     .usage = owe::resource::BufferUsage::Vertex },
                .content_version = rstd::u64(4),
            },
    });
    plan.shaders.push(owe::resource::ShaderPlanEntry {
        .handle =
            owe::resource::ShaderUseHandle { .index = rstd::u64(7), .generation = rstd::u64(12) },
        .request = ShaderRequest(5),
    });

    owe::resource_registry::ResourcePrepareService service(
        textures,
        rstd::None<rstd::mut_ref<rstd::dyn<owe::resource_registry::TexturePrepareBackend>>>(),
        buffers,
        upload.as_mut_ref(),
        shaders);
    auto prepared = service.Prepare(plan,
                                    owe::resource_registry::ResourceContentProviders {
                                        .buffer = rstd::Some(buffer.as_mut_ref()),
                                        .shader = rstd::Some(shader.as_mut_ref()),
                                    });

    ASSERT_TRUE(prepared.is_ok());
    auto table = rstd::move(prepared).unwrap_unchecked();
    EXPECT_EQ(table.Generation(), rstd::u64(12));
    EXPECT_EQ(table.BufferCount(), rstd::usize(1));
    EXPECT_EQ(table.ShaderCount(), rstd::usize(1));
    EXPECT_EQ(buffer_provider.loads, rstd::usize(1));
    EXPECT_EQ(upload_backend.allocations, rstd::usize(1));
    EXPECT_EQ(shader_provider.loads, rstd::usize(1));

    auto texture_only = service.Prepare(plan,
                                        owe::resource_registry::ResourceContentProviders {
                                            .buffer = rstd::Some(buffer.as_mut_ref()),
                                            .shader = rstd::Some(shader.as_mut_ref()),
                                        },
                                        owe::resource::ResourcePlanTextures);
    ASSERT_TRUE(texture_only.is_ok());
    auto texture_table = rstd::move(texture_only).unwrap_unchecked();
    EXPECT_EQ(texture_table.BufferCount(), rstd::usize());
    EXPECT_EQ(texture_table.ShaderCount(), rstd::usize());
    EXPECT_EQ(buffer_provider.loads, rstd::usize(1));
    EXPECT_EQ(upload_backend.allocations, rstd::usize(1));
    EXPECT_EQ(shader_provider.loads, rstd::usize(1));
}

TEST(BufferRegistry, LoadsOnlyMissingVersionsAndRetriesFailedUploads) {
    owe::resource_registry::BufferRegistry registry;
    BufferBackend                          backend;
    BufferContentProvider                  provider;
    auto upload     = rstd::dyn<owe::vulkan::BufferBackend>::from_ref(backend);
    auto source     = rstd::dyn<owe::resource::BufferContentProvider>::from_ref(provider);
    auto definition = owe::resource::BufferResourceRef(owe::resource::BufferRequest {
        .name       = "lazy-vertices"_Str,
        .definition = { .size = rstd::usize(4096), .usage = owe::resource::BufferUsage::Vertex },
        .content_version  = rstd::u64(3),
        .content_identity = rstd::u64(5),
    });
    EXPECT_TRUE(registry.Ensure(definition.clone(), rstd::None(), upload.as_mut_ref()).is_err());
    EXPECT_EQ(backend.allocations, rstd::usize());
    auto prepare = [&](const owe::resource::BufferResourceRef& value) {
        return registry.Ensure(value.clone(), rstd::Some(source.as_mut_ref()), upload.as_mut_ref());
    };
    provider.fail = true;
    EXPECT_TRUE(prepare(definition).is_err());
    EXPECT_EQ(backend.allocations, rstd::usize());
    provider.fail = false;
    auto first    = prepare(definition).unwrap();
    EXPECT_EQ(backend.last_bytes, rstd::usize(2));
    EXPECT_EQ(provider.loads, rstd::usize(2));
    EXPECT_TRUE(registry.Ensure(definition.clone(), rstd::None(), upload.as_mut_ref()).is_ok());
    EXPECT_EQ(provider.loads, rstd::usize(2));
    auto changed             = definition.definition().clone();
    changed.content_identity = rstd::u64(6);
    definition               = definition.with_definition(rstd::move(changed));
    backend.fail_write       = true;
    EXPECT_TRUE(prepare(definition).is_err());
    EXPECT_EQ(first.physical->source_identity, rstd::u64(5));
    backend.fail_write = false;
    EXPECT_TRUE(prepare(definition).is_ok());
    EXPECT_EQ(first.physical->source_identity, rstd::u64(6));
    EXPECT_EQ(backend.allocations, rstd::usize(1));
    EXPECT_EQ(provider.loads, rstd::usize(4));
    provider.fail = true;
    EXPECT_TRUE(prepare(definition).is_ok());
    EXPECT_EQ(provider.loads, rstd::usize(4));
}

TEST(BufferRegistry, DynamicReuseDoesNotRequireOrReapplyInitialContent) {
    owe::resource_registry::BufferRegistry registry;
    BufferBackend                          backend;
    BufferContentProvider                  provider;
    auto upload     = rstd::dyn<owe::vulkan::BufferBackend>::from_ref(backend);
    auto source     = rstd::dyn<owe::resource::BufferContentProvider>::from_ref(provider);
    auto definition = owe::resource::BufferResourceRef(owe::resource::BufferRequest {
        .name       = "dynamic-lazy"_Str,
        .definition = { .size = rstd::usize(64) },
        .lifetime   = owe::resource::BufferLifetimeClass::Dynamic,
    });
    auto first =
        registry.Ensure(definition.clone(), rstd::Some(source.as_mut_ref()), upload.as_mut_ref())
            .unwrap();
    EXPECT_TRUE(
        registry.Update(first.resource, provider.content.as_slice(), upload.as_mut_ref()).is_ok());
    auto request            = definition.definition().clone();
    request.content_version = rstd::u64(100);
    definition              = definition.with_definition(rstd::move(request));
    EXPECT_TRUE(registry.Ensure(definition.clone(), rstd::None(), upload.as_mut_ref()).is_ok());
    EXPECT_EQ(provider.loads, rstd::usize(1));
    EXPECT_EQ(backend.writes, rstd::usize(2));
    registry.Reset();
    EXPECT_TRUE(registry.Resolve(first.resource).is_none());
    EXPECT_TRUE(registry.Ensure(definition.clone(), rstd::None(), upload.as_mut_ref()).is_err());
    EXPECT_TRUE(
        registry.Ensure(definition.clone(), rstd::Some(source.as_mut_ref()), upload.as_mut_ref())
            .is_ok());
    EXPECT_EQ(provider.loads, rstd::usize(2));
}

TEST(BufferRegistry, ReloadsRegisteredSourceAfterPhysicalEviction) {
    owe::resource_registry::BufferRegistry registry;
    BufferBackend                          backend;
    BufferContentProvider                  provider;
    auto upload     = rstd::dyn<owe::vulkan::BufferBackend>::from_ref(backend);
    auto source     = rstd::dyn<owe::resource::BufferContentProvider>::from_ref(provider);
    auto definition = owe::resource::BufferResourceRef(owe::resource::BufferRequest {
        .name       = "evicted-source"_Str,
        .definition = { .size = rstd::usize(64) },
    });
    owe::resource::BufferHandle handle;
    {
        auto prepared =
            registry
                .Ensure(definition.clone(), rstd::Some(source.as_mut_ref()), upload.as_mut_ref())
                .unwrap();
        handle = prepared.resource;
        const owe::vulkan::BufferUploadTicket ticket { .value = backend.next_ticket };
        registry.MarkUploadsSubmitted(
            rstd::slice<owe::vulkan::BufferUploadTicket>::from_raw_parts(&ticket, rstd::usize(1)),
            rstd::None());
    }
    registry.EvictUnused();
    EXPECT_TRUE(registry.Resolve(handle).is_none());
    auto prepared =
        registry.Ensure(definition.clone(), rstd::Some(source.as_mut_ref()), upload.as_mut_ref())
            .unwrap();
    EXPECT_EQ(prepared.resource, handle);
    EXPECT_EQ(provider.loads, rstd::usize(2));
    EXPECT_EQ(backend.allocations, rstd::usize(2));
    provider.content.resize(rstd::usize(65), rstd::u8());
    auto request = definition.definition().clone();
    ++request.content_version;
    EXPECT_TRUE(registry
                    .Ensure(definition.with_definition(rstd::move(request)),
                            rstd::Some(source.as_mut_ref()),
                            upload.as_mut_ref())
                    .is_err());
    EXPECT_EQ(backend.allocations, rstd::usize(2));
}

TEST(ResourcePrepareService, CachedBuffersSkipUnavailableProviderAndZeroInitialization) {
    owe::resource::TextureRegistry         textures;
    owe::resource_registry::BufferRegistry buffers;
    owe::resource_registry::ShaderRegistry shaders;
    BufferBackend                          backend;
    BufferContentProvider                  provider;
    provider.zeroed = true;
    auto upload     = rstd::dyn<owe::vulkan::BufferBackend>::from_ref(backend);
    auto source     = rstd::dyn<owe::resource::BufferContentProvider>::from_ref(provider);
    owe::resource_registry::ResourcePrepareService service(
        textures, rstd::None(), buffers, upload.as_mut_ref(), shaders);
    owe::resource::ResourcePlan plan { .generation = rstd::u64(2) };
    plan.buffers.push(owe::resource::BufferPlanEntry {
        .handle = { .index = rstd::u64(1), .generation = plan.generation },
        .definition =
            owe::resource::BufferRequest {
                .name       = "zero-uniform"_Str,
                .definition = { .size  = rstd::usize(1024),
                                .usage = owe::resource::BufferUsage::Uniform },
            },
    });
    EXPECT_TRUE(service.Prepare(plan, { .buffer = rstd::Some(source.as_mut_ref()) }).is_ok());
    EXPECT_EQ(backend.zeros, rstd::usize(1));
    EXPECT_EQ(backend.last_bytes, rstd::usize());
    EXPECT_TRUE(service.Prepare(plan, {}).is_ok());
    EXPECT_EQ(provider.loads, rstd::usize(1));
    EXPECT_EQ(backend.zeros, rstd::usize(1));
}

TEST(ResourcePrepareService, InitializesRetainedHistoryTextureOnce) {
    owe::resource::TextureRegistry         textures;
    owe::resource_registry::BufferRegistry buffers;
    owe::resource_registry::ShaderRegistry shaders;
    BufferBackend                          buffer_backend;
    ImageBackend                           image_backend;
    auto buffer = rstd::dyn<owe::vulkan::BufferBackend>::from_ref(buffer_backend);
    auto image  = rstd::dyn<owe::resource_registry::TexturePrepareBackend>::from_ref(image_backend);

    owe::resource::ResourcePlan plan { .generation = rstd::u64(20) };
    plan.textures.push(owe::resource::TexturePlanEntry {
        .handle =
            owe::resource::TextureUseHandle {
                .index      = rstd::u64(1),
                .generation = rstd::u64(20),
            },
        .definition =
            owe::resource::TextureRequest {
                .kind       = owe::resource::TextureRequestKind::RenderTarget,
                .name       = rstd::string::String::make("history"_str),
                .definition = rstd::Some(owe::resource::TextureDefinition {
                    .width  = rstd::i32(64),
                    .height = rstd::i32(32),
                }),
                .lifetime   = owe::resource::TextureLifetimeClass::FrameLocal,
            },
        .access                 = owe::resource::ResourceAccess::ReadWrite,
        .preserve_contents      = true,
        .initialize_transparent = true,
    });

    owe::resource_registry::ResourcePrepareService service(
        textures, rstd::Some(image.as_mut_ref()), buffers, buffer.as_mut_ref(), shaders);
    auto first = service.Prepare(plan);
    ASSERT_TRUE(first.is_ok());
    EXPECT_EQ(first->TextureCount(), rstd::usize(1));
    EXPECT_EQ(image_backend.transparent_creates, rstd::usize(1));
    auto first_texture = first->Resolve(plan.textures[rstd::usize()].handle);
    ASSERT_TRUE(first_texture.is_some());
    auto first_slots = (**first_texture).physical->Slots();
    ASSERT_EQ(first_slots.len(), rstd::usize(1));
    EXPECT_EQ(first_slots[rstd::usize()].view.texture().description().extent.width, 64u);
    EXPECT_EQ(first_slots[rstd::usize()].view.texture().description().extent.height, 32u);
    EXPECT_TRUE(first_slots[rstd::usize()].sampler.valid());
    EXPECT_TRUE(first_slots[rstd::usize()].attachment_view.valid());
    EXPECT_TRUE(textures.ResolveCurrent((**first_texture).resource).is_none());
    EXPECT_TRUE(textures.ResolvePending((**first_texture).resource).is_some());

    auto rebuilt                                      = plan.clone();
    rebuilt.generation                                = rstd::u64(21);
    rebuilt.textures[rstd::usize()].handle.generation = rebuilt.generation;
    auto second                                       = service.Prepare(rebuilt);
    ASSERT_TRUE(second.is_ok());
    EXPECT_EQ(second->TextureCount(), rstd::usize(1));
    EXPECT_EQ(image_backend.transparent_creates, rstd::usize(1));
    auto second_texture = second->Resolve(rebuilt.textures[rstd::usize()].handle);
    ASSERT_TRUE(second_texture.is_some());
    EXPECT_EQ((**second_texture).physical->Slots()[rstd::usize()].view.texture().identity(),
              first_slots[rstd::usize()].view.texture().identity());
}

TEST(ResourcePrepareService, MapsLogicalTextureUsesToPlannedPhysicalSlots) {
    owe::resource::TextureRegistry         textures;
    owe::resource_registry::BufferRegistry buffers;
    owe::resource_registry::ShaderRegistry shaders;
    BufferBackend                          buffer_backend;
    ImageBackend                           image_backend;
    auto buffer = rstd::dyn<owe::vulkan::BufferBackend>::from_ref(buffer_backend);
    auto image  = rstd::dyn<owe::resource_registry::TexturePrepareBackend>::from_ref(image_backend);

    owe::resource::ResourcePlan plan { .generation = rstd::u64(22) };
    for (rstd::uint64_t index = 0; index < 3; ++index) {
        plan.textures.push(owe::resource::TexturePlanEntry {
            .handle =
                owe::resource::TextureUseHandle {
                    .index      = rstd::u64(index),
                    .generation = rstd::u64(22),
                },
            .definition =
                owe::resource::TextureRequest {
                    .kind       = owe::resource::TextureRequestKind::RenderTarget,
                    .name       = rstd::format("composite-v{}", index),
                    .definition = rstd::Some(owe::resource::TextureDefinition {
                        .width  = rstd::i32(64),
                        .height = rstd::i32(32),
                    }),
                    .lifetime   = owe::resource::TextureLifetimeClass::FrameLocal,
                },
            .allocation_key = index < 2 ? rstd::string::String::make("composite::slot_0"_str)
                                        : rstd::string::String::make("composite::slot_1"_str),
        });
    }

    owe::resource_registry::ResourcePrepareService service(
        textures, rstd::Some(image.as_mut_ref()), buffers, buffer.as_mut_ref(), shaders);
    auto prepared = service.Prepare(plan);
    ASSERT_TRUE(prepared.is_ok());
    ASSERT_EQ(prepared->TextureCount(), rstd::usize(3));
    EXPECT_EQ(image_backend.creates, rstd::usize(2));

    auto first  = prepared->Resolve(plan.textures[rstd::usize()].handle);
    auto second = prepared->Resolve(plan.textures[rstd::usize(1)].handle);
    auto third  = prepared->Resolve(plan.textures[rstd::usize(2)].handle);
    ASSERT_TRUE(first.is_some());
    ASSERT_TRUE(second.is_some());
    ASSERT_TRUE(third.is_some());
    EXPECT_NE((**first).resource, (**second).resource);
    EXPECT_NE((**first).resource, (**third).resource);
    EXPECT_EQ((**first).physical->Slots()[rstd::usize()].view.texture().identity(),
              (**second).physical->Slots()[rstd::usize()].view.texture().identity());
    EXPECT_NE((**first).physical->Slots()[rstd::usize()].view.texture().identity(),
              (**third).physical->Slots()[rstd::usize()].view.texture().identity());
    EXPECT_EQ((**first).request().name, "composite-v0"_str);
    EXPECT_EQ((**second).request().name, "composite-v1"_str);
}

TEST(ResourcePrepareService, BatchesDeduplicatesAndCachesImportedTextures) {
    owe::resource::TextureRegistry         textures;
    owe::resource_registry::BufferRegistry buffers;
    owe::resource_registry::ShaderRegistry shaders;
    BufferBackend                          buffer_backend;
    ImageBackend                           image_backend;
    TextureContentProvider                 content_provider;
    auto buffer = rstd::dyn<owe::vulkan::BufferBackend>::from_ref(buffer_backend);
    auto image  = rstd::dyn<owe::resource_registry::TexturePrepareBackend>::from_ref(image_backend);
    auto content = rstd::dyn<owe::resource::TextureContentProvider>::from_ref(content_provider);

    owe::resource::ResourcePlan plan { .generation = rstd::u64(21) };
    for (std::uint64_t index = 0; index < 10; ++index) {
        auto name = index < 2 ? std::string("alias-") + std::to_string(index)
                              : std::string("texture-") + std::to_string(index);
        plan.textures.push(owe::resource::TexturePlanEntry {
            .handle = owe::resource::TextureUseHandle { .index      = rstd::u64(index),
                                                        .generation = rstd::u64(21) },
            .definition =
                owe::resource::TextureRequest {
                    .kind = owe::resource::TextureRequestKind::Imported,
                    .name = rstd::string::String::make(rstd::cppstd::as_str(name).unwrap()),
                },
        });
    }

    owe::resource_registry::ResourcePrepareService service(
        textures, rstd::Some(image.as_mut_ref()), buffers, buffer.as_mut_ref(), shaders);
    auto started = [&] {
        auto published = plan.clone();
        return service.Begin(published,
                             owe::resource_registry::ResourceContentProviders {
                                 .texture = rstd::Some(content.as_mut_ref()),
                             });
    }();
    ASSERT_TRUE(started.is_ok());
    auto session = rstd::move(started).unwrap_unchecked();
    EXPECT_EQ(session.Plan().textures.len(), plan.textures.len());
    EXPECT_EQ(session.Plan().generation, plan.generation);
    rstd::usize batches {};
    while (true) {
        auto progress = service.Continue(session);
        ASSERT_TRUE(progress.is_ok());
        if (progress.unwrap_unchecked() ==
            owe::resource_registry::ResourcePrepareProgress::Complete) {
            break;
        }
        ++batches;
    }
    auto first = rstd::move(session).TakeTable();
    EXPECT_EQ(first.TextureCount(), rstd::usize(10));
    EXPECT_EQ(batches, rstd::usize(3));
    EXPECT_EQ(content_provider.resolves, rstd::usize(10));
    EXPECT_EQ(content_provider.loads.load(std::memory_order_relaxed), std::size_t(9));
    EXPECT_EQ(image_backend.creates, rstd::usize(9));

    auto second = service.Prepare(plan,
                                  owe::resource_registry::ResourceContentProviders {
                                      .texture = rstd::Some(content.as_mut_ref()),
                                  });
    ASSERT_TRUE(second.is_ok());
    EXPECT_EQ(second->TextureCount(), rstd::usize(10));
    EXPECT_EQ(content_provider.resolves, rstd::usize(20));
    EXPECT_EQ(content_provider.loads.load(std::memory_order_relaxed), std::size_t(9));
    EXPECT_EQ(image_backend.creates, rstd::usize(9));
}

TEST(PreparedResourceTable, ResolvesTypedUsesWithoutRegistryLookup) {
    owe::resource_registry::PreparedResourceTable table(rstd::u64(9));
    auto                                          use =
        owe::resource::TextureUseHandle { .index = rstd::u64(4), .generation = rstd::u64(9) };
    auto allocation = TextureAllocation(11);

    EXPECT_TRUE(table.Insert(owe::resource_registry::PreparedTexture {
        .use = use,
        .resource =
            owe::resource::TextureHandle { .index = rstd::u64(2), .generation = rstd::u64(1) },
        .definition =
            owe::resource::TextureRequest {
                .kind = owe::resource::TextureRequestKind::Imported,
                .name = rstd::string::String::make("prepared"_str),
            },
        .physical            = allocation.clone(),
        .image               = allocation->View(),
        .physical_generation = rstd::u64(3),
    }));

    auto prepared = table.Resolve(use);
    ASSERT_TRUE(prepared.is_some());
    EXPECT_EQ(table.Generation(), rstd::u64(9));
    EXPECT_EQ(table.TextureCount(), rstd::usize(1));
    EXPECT_EQ((**prepared).image.getActive().generation, rstd::u64(11));
    EXPECT_EQ((**prepared).physical_generation, rstd::u64(3));
    auto leases = table.Leases();
    ASSERT_EQ(leases.textures.len(), rstd::usize(1));
    EXPECT_EQ(leases.textures[rstd::usize()].resource.index, rstd::u64(2));
    EXPECT_EQ(leases.textures[rstd::usize()].physical_generation, rstd::u64(3));
}

TEST(PreparedResourceTable, RestoresReplacedSectionsAfterPrepareFailure) {
    owe::resource_registry::PreparedResourceTable prepared(rstd::u64(5));
    auto                                          old_use =
        owe::resource::TextureUseHandle { .index = rstd::u64(1), .generation = rstd::u64(5) };
    auto old_allocation = TextureAllocation(3);
    ASSERT_TRUE(prepared.Insert(owe::resource_registry::PreparedTexture {
        .use = old_use,
        .resource =
            owe::resource::TextureHandle { .index = rstd::u64(2), .generation = rstd::u64(1) },
        .definition =
            owe::resource::TextureRequest {
                .kind = owe::resource::TextureRequestKind::Imported,
                .name = rstd::string::String::make("old"_str),
            },
        .physical = old_allocation.clone(),
        .image    = old_allocation->View(),
    }));
    auto descriptor = owe::resource::DescriptorBindingHandle {
        .index      = rstd::u64(8),
        .generation = rstd::u64(5),
    };
    ASSERT_TRUE(prepared.Insert(owe::resource_registry::PreparedDescriptorBinding {
        .handle = descriptor,
        .images = rstd::vec::Vec<owe::resource_registry::DescriptorImageBinding>::make(),
    }));

    auto rollback = prepared.clone();

    owe::resource_registry::PreparedResourceTable replacement(rstd::u64(6));
    auto                                          new_use =
        owe::resource::TextureUseHandle { .index = rstd::u64(4), .generation = rstd::u64(6) };
    auto new_allocation = TextureAllocation(7);
    ASSERT_TRUE(replacement.Insert(owe::resource_registry::PreparedTexture {
        .use = new_use,
        .resource =
            owe::resource::TextureHandle { .index = rstd::u64(5), .generation = rstd::u64(1) },
        .definition =
            owe::resource::TextureRequest {
                .kind = owe::resource::TextureRequestKind::Imported,
                .name = rstd::string::String::make("new"_str),
            },
        .physical = new_allocation.clone(),
        .image    = new_allocation->View(),
    }));
    replacement.CarryForward(rstd::move(prepared), owe::resource::ResourcePlanTextures);
    replacement.Remove(descriptor);

    replacement = rstd::move(rollback);
    EXPECT_EQ(replacement.Generation(), rstd::u64(5));
    EXPECT_EQ(replacement.TextureCount(), rstd::usize(1));
    EXPECT_TRUE(replacement.Resolve(old_use).is_some());
    EXPECT_TRUE(replacement.Resolve(new_use).is_none());
    EXPECT_TRUE(replacement.Resolve(descriptor).is_some());
}

TEST(PreparedResourceTable, PinsEveryPreparedGenerationInOneLeaseSet) {
    owe::resource_registry::PreparedResourceTable table(rstd::u64(4));

    auto texture_physical = TextureAllocation(2);
    auto texture_use =
        owe::resource::TextureUseHandle { .index = rstd::u64(), .generation = rstd::u64(4) };
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedTexture {
        .use = texture_use,
        .resource =
            owe::resource::TextureHandle { .index = rstd::u64(10), .generation = rstd::u64(2) },
        .definition =
            owe::resource::TextureRequest {
                .kind = owe::resource::TextureRequestKind::Imported,
                .name = rstd::string::String::make("leased"_str),
            },
        .physical = texture_physical.clone(),
        .image    = texture_physical->View(),
    }));

    auto buffer_physical = rstd::sync::Arc<owe::resource_registry::BufferPhysical>::make(
        owe::vulkan::BufferAllocation {}, rstd::u64(3), rstd::u64(7));
    auto buffer_use =
        owe::resource::BufferUseHandle { .index = rstd::u64(1), .generation = rstd::u64(4) };
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedBufferUse {
        .use = buffer_use,
        .buffer =
            owe::resource_registry::PreparedBuffer {
                .resource = owe::resource::BufferHandle { .index      = rstd::u64(11),
                                                          .generation = rstd::u64(2) },
                .physical = buffer_physical.clone(),
            },
    }));

    auto shader_physical = rstd::sync::Arc<owe::resource_registry::ShaderPhysical>::make(
        owe::resource::ShaderArtifact {}, rstd::u64(5));
    auto shader_use =
        owe::resource::ShaderUseHandle { .index = rstd::u64(2), .generation = rstd::u64(4) };
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedShaderUse {
        .use = shader_use,
        .shader =
            owe::resource_registry::PreparedShader {
                .resource = owe::resource::ShaderHandle { .index      = rstd::u64(12),
                                                          .generation = rstd::u64(2) },
                .physical = shader_physical.clone(),
            },
    }));

    auto pipeline_layout =
        rstd::sync::Arc<owe::resource_registry::PipelineLayoutResourceEntry>::make();
    auto pipeline = rstd::sync::Arc<owe::resource_registry::PipelineResourceEntry>::make(
        owe::resource_registry::PipelineResourceEntry {
            .layout = pipeline_layout.clone(),
        });
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedPipeline {
        .use =
            owe::resource::PipelineUseHandle { .index = rstd::u64(3), .generation = rstd::u64(4) },
        .resource =
            owe::resource::PipelineHandle { .index = rstd::u64(13), .generation = rstd::u64(2) },
        .physical = pipeline.clone(),
    }));
    auto render_pass = rstd::sync::Arc<vvk::RenderPass>::make();
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedRenderPass {
        .use = owe::resource::RenderPassUseHandle { .index      = rstd::u64(4),
                                                    .generation = rstd::u64(4) },
        .resource =
            owe::resource::RenderPassHandle { .index = rstd::u64(14), .generation = rstd::u64(2) },
        .physical = render_pass.clone(),
    }));
    auto framebuffer = rstd::sync::Arc<vvk::Framebuffer>::make();
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedFramebuffer {
        .use = owe::resource::FramebufferUseHandle { .index      = rstd::u64(5),
                                                     .generation = rstd::u64(4) },
        .resource =
            owe::resource::FramebufferHandle { .index = rstd::u64(15), .generation = rstd::u64(2) },
        .physical = framebuffer.clone(),
    }));
    auto external_use =
        owe::resource::ExternalUseHandle { .index = rstd::u64(6), .generation = rstd::u64(4) };
    table.Insert(owe::resource_registry::PreparedExternalUse {
        .use   = external_use,
        .frame = owe::resource_registry::PreparedExternalFrame {},
    });

    ASSERT_TRUE(table.Resolve(texture_use).is_some());
    ASSERT_TRUE(table.Resolve(buffer_use).is_some());
    ASSERT_TRUE(table.Resolve(shader_use).is_some());
    ASSERT_TRUE(table.Resolve(external_use).is_some());
    auto leases = table.Leases();
    EXPECT_EQ(leases.textures.len(), rstd::usize(1));
    EXPECT_EQ(leases.buffers.len(), rstd::usize(1));
    EXPECT_EQ(leases.shaders.len(), rstd::usize(1));
    EXPECT_EQ(leases.pipelines.len(), rstd::usize(1));
    EXPECT_EQ(leases.render_passes.len(), rstd::usize(1));
    EXPECT_EQ(leases.framebuffers.len(), rstd::usize(1));
    EXPECT_EQ(leases.externals.len(), rstd::usize(1));
    EXPECT_EQ(texture_physical.strong_count(), rstd::usize(3));
    EXPECT_EQ(buffer_physical.strong_count(), rstd::usize(3));
    EXPECT_EQ(shader_physical.strong_count(), rstd::usize(3));
    EXPECT_EQ(pipeline.strong_count(), rstd::usize(3));
    EXPECT_EQ(render_pass.strong_count(), rstd::usize(3));
    EXPECT_EQ(framebuffer.strong_count(), rstd::usize(3));

    SubmissionQueue queue;
    auto            work    = queue.Begin().unwrap();
    auto            receipt = work.receipt();
    work.retain(rstd::move(leases));
    auto completion = queue.Submit(rstd::move(work)).unwrap();
    ASSERT_TRUE(receipt.submitted());
    ASSERT_TRUE(completion.valid());
    EXPECT_FALSE(completion.poll().unwrap());
    table = owe::resource_registry::PreparedResourceTable {};
    EXPECT_EQ(texture_physical.strong_count(), rstd::usize(2));
    EXPECT_EQ(buffer_physical.strong_count(), rstd::usize(2));
    EXPECT_EQ(shader_physical.strong_count(), rstd::usize(2));
    EXPECT_EQ(pipeline.strong_count(), rstd::usize(2));
    EXPECT_EQ(render_pass.strong_count(), rstd::usize(2));
    EXPECT_EQ(framebuffer.strong_count(), rstd::usize(2));
    queue.progress->completed = completion.value();
    ASSERT_TRUE(completion.poll().unwrap());
    ASSERT_TRUE(queue.Poll().is_ok());
    EXPECT_EQ(texture_physical.strong_count(), rstd::usize(1));
    EXPECT_EQ(buffer_physical.strong_count(), rstd::usize(1));
    EXPECT_EQ(shader_physical.strong_count(), rstd::usize(1));
    EXPECT_EQ(pipeline.strong_count(), rstd::usize(1));
    EXPECT_EQ(render_pass.strong_count(), rstd::usize(1));
    EXPECT_EQ(framebuffer.strong_count(), rstd::usize(1));
}

TEST(DescriptorLayoutRegistry, CanonicalizesBindingOrder) {
    auto bindings = rstd::vec::Vec<VkDescriptorSetLayoutBinding>::make();
    bindings.push(VkDescriptorSetLayoutBinding {
        .binding         = 3,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
    });
    bindings.push(VkDescriptorSetLayoutBinding {
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_VERTEX_BIT,
    });
    owe::vulkan::DescriptorSetInfo first {
        .push_descriptor = true,
        .bindings        = rstd::move(bindings),
    };
    auto second = first.clone();
    rstd::swap(second.bindings[rstd::usize()], second.bindings[rstd::usize(1)]);

    auto first_schema  = owe::resource_registry::DescriptorLayoutRegistry::CanonicalSchema(first);
    auto second_schema = owe::resource_registry::DescriptorLayoutRegistry::CanonicalSchema(second);
    ASSERT_TRUE(first_schema.is_ok());
    ASSERT_TRUE(second_schema.is_ok());
    EXPECT_TRUE(rstd::move(first_schema).unwrap_unchecked() ==
                rstd::move(second_schema).unwrap_unchecked());
}

TEST(DescriptorLayoutRegistry, RejectsDuplicateBindings) {
    auto bindings = rstd::vec::Vec<VkDescriptorSetLayoutBinding>::make();
    bindings.push(VkDescriptorSetLayoutBinding {
        .binding         = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_VERTEX_BIT,
    });
    bindings.push(VkDescriptorSetLayoutBinding {
        .binding         = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
    });
    owe::vulkan::DescriptorSetInfo info {
        .bindings = rstd::move(bindings),
    };

    EXPECT_TRUE(owe::resource_registry::DescriptorLayoutRegistry::CanonicalSchema(info).is_err());
}

TEST(DescriptorSystem, PreparesOwnedPushBindingPacket) {
    owe::resource_registry::DescriptorSystem descriptors;
    auto images = rstd::vec::Vec<owe::resource_registry::DescriptorImageBinding>::make();
    owe::vulkan::ImageParameters image;
    image.generation = rstd::u64(17);
    images.push(owe::resource_registry::DescriptorImageBinding {
        .binding = 2,
        .image   = image,
    });
    auto buffers = rstd::vec::Vec<owe::resource_registry::DescriptorBufferBinding>::make();
    buffers.push(owe::resource_registry::DescriptorBufferBinding {
        .binding = 2,
        .offset  = 256,
        .size    = 512,
    });
    buffers.push(owe::resource_registry::DescriptorBufferBinding {
        .binding = 0,
        .offset  = 64,
        .size    = 128,
    });
    auto prepared = descriptors.PreparePush(3, images.as_slice(), buffers.as_slice());
    EXPECT_EQ(prepared.set_index, 3u);
    auto handle = prepared.handle;

    EXPECT_TRUE(handle.Valid());
    EXPECT_EQ(prepared.images.len(), rstd::usize(1));
    EXPECT_EQ(prepared.images[rstd::usize()].binding, 2u);
    EXPECT_EQ(prepared.images[rstd::usize()].layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_EQ(prepared.images[rstd::usize()].image.generation, rstd::u64(17));
    ASSERT_EQ(prepared.buffers.len(), rstd::usize(2));
    EXPECT_EQ(prepared.buffers[rstd::usize()].binding, 0u);
    EXPECT_EQ(prepared.buffers[rstd::usize()].offset, 64u);
    EXPECT_EQ(prepared.buffers[rstd::usize()].size, 128u);
    EXPECT_EQ(prepared.buffers[rstd::usize(1)].binding, 2u);
    EXPECT_EQ(prepared.buffers[rstd::usize(1)].offset, 256u);
    EXPECT_EQ(prepared.buffers[rstd::usize(1)].size, 512u);

    owe::resource_registry::PreparedResourceTable table;
    EXPECT_TRUE(table.Insert(rstd::move(prepared)));
    auto resolved = table.Resolve(handle);
    ASSERT_TRUE(resolved.is_some());
    EXPECT_EQ(table.DescriptorCount(), rstd::usize(1));
    EXPECT_EQ((**resolved).images[rstd::usize()].image.generation, rstd::u64(17));

    auto leases = table.Leases();
    ASSERT_EQ(leases.descriptors.len(), rstd::usize(1));
    EXPECT_EQ(leases.descriptors[rstd::usize()].handle, handle);
    SubmissionQueue queue;
    auto            work    = queue.Begin().unwrap();
    auto            receipt = work.receipt();
    work.retain(rstd::move(leases));
    auto completion = queue.Submit(rstd::move(work)).unwrap();
    EXPECT_TRUE(receipt.submitted());
    EXPECT_TRUE(completion.valid());
    EXPECT_FALSE(completion.poll().unwrap());
    EXPECT_EQ(queue.pending.len(), rstd::usize(1));
    table                     = owe::resource_registry::PreparedResourceTable {};
    queue.progress->completed = completion.value();
    EXPECT_TRUE(completion.poll().unwrap());
    ASSERT_TRUE(queue.Poll().is_ok());
    EXPECT_EQ(queue.pending.len(), rstd::usize());
}

TEST(DescriptorSystem, PreservesDepthSampledLayoutAcrossCloneAndUpdate) {
    owe::resource_registry::DescriptorSystem descriptors;
    auto images = rstd::vec::Vec<owe::resource_registry::DescriptorImageBinding>::make();
    images.push(owe::resource_registry::DescriptorImageBinding {
        .binding = 1,
        .layout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
    });
    auto prepared = descriptors.PreparePush(
        2, images.as_slice(), rstd::slice<owe::resource_registry::DescriptorBufferBinding>());
    auto cloned = prepared.clone();
    ASSERT_EQ(cloned.images.len(), rstd::usize(1));
    EXPECT_EQ(cloned.images[rstd::usize()].layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

    auto color_images                  = images.clone();
    color_images[rstd::usize()].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ASSERT_TRUE(prepared.UpdateImages(color_images.as_slice()).is_ok());
    EXPECT_EQ(prepared.images[rstd::usize()].layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

TEST(DescriptorSystem, RejectsActiveWritesOutsideThePipelineLayout) {
    auto bindings = rstd::vec::Vec<owe::resource_registry::DescriptorBindingSchema>::make();
    bindings.push(owe::resource_registry::DescriptorBindingSchema {
        .binding          = 0,
        .descriptor_type  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptor_count = 1,
        .stage_flags      = VK_SHADER_STAGE_VERTEX_BIT,
    });
    owe::resource_registry::DescriptorLayoutEntry layout {
        .handle =
            owe::resource::DescriptorLayoutHandle {
                .index      = rstd::u64(1),
                .generation = rstd::u64(1),
            },
        .schema =
            owe::resource_registry::DescriptorSetSchema {
                .push_descriptor = true,
                .bindings        = rstd::move(bindings),
            },
    };
    auto images = rstd::vec::Vec<owe::resource_registry::DescriptorImageBinding>::make();
    images.push(owe::resource_registry::DescriptorImageBinding {
        .binding = 2,
    });

    owe::resource_registry::DescriptorSystem descriptors;
    owe::vulkan::Device                      device;
    auto                                     rejected =
        descriptors.Prepare(device,
                            1,
                            layout,
                            images.as_slice(),
                            rstd::slice<owe::resource_registry::DescriptorBufferBinding>(),
                            owe::resource_registry::DescriptorBindingReuse::Exclusive);
    EXPECT_TRUE(rejected.is_err());
}

TEST(DescriptorSystem, RejectsDuplicateBufferWrites) {
    auto bindings = rstd::vec::Vec<owe::resource_registry::DescriptorBindingSchema>::make();
    bindings.push(owe::resource_registry::DescriptorBindingSchema {
        .binding          = 1,
        .descriptor_type  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptor_count = 1,
        .stage_flags      = VK_SHADER_STAGE_VERTEX_BIT,
    });
    owe::resource_registry::DescriptorLayoutEntry layout {
        .handle =
            owe::resource::DescriptorLayoutHandle {
                .index      = rstd::u64(2),
                .generation = rstd::u64(1),
            },
        .schema =
            owe::resource_registry::DescriptorSetSchema {
                .push_descriptor = true,
                .bindings        = rstd::move(bindings),
            },
    };
    auto buffers = rstd::vec::Vec<owe::resource_registry::DescriptorBufferBinding>::make();
    buffers.push(owe::resource_registry::DescriptorBufferBinding { .binding = 1, .size = 16 });
    buffers.push(owe::resource_registry::DescriptorBufferBinding { .binding = 1, .size = 32 });
    auto images = rstd::vec::Vec<owe::resource_registry::DescriptorImageBinding>::make();

    owe::resource_registry::DescriptorSystem descriptors;
    owe::vulkan::Device                      device;
    auto rejected = descriptors.Prepare(device,
                                        0,
                                        layout,
                                        images.as_slice(),
                                        buffers.as_slice(),
                                        owe::resource_registry::DescriptorBindingReuse::Exclusive);
    EXPECT_TRUE(rejected.is_err());
}

TEST(DescriptorSystem, RejectsDuplicateImageWrites) {
    auto bindings = rstd::vec::Vec<owe::resource_registry::DescriptorBindingSchema>::make();
    bindings.push(owe::resource_registry::DescriptorBindingSchema {
        .binding          = 2,
        .descriptor_type  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptor_count = 1,
        .stage_flags      = VK_SHADER_STAGE_FRAGMENT_BIT,
    });
    owe::resource_registry::DescriptorLayoutEntry layout {
        .handle =
            owe::resource::DescriptorLayoutHandle {
                .index      = rstd::u64(3),
                .generation = rstd::u64(1),
            },
        .schema =
            owe::resource_registry::DescriptorSetSchema {
                .push_descriptor = true,
                .bindings        = rstd::move(bindings),
            },
    };
    auto images = rstd::vec::Vec<owe::resource_registry::DescriptorImageBinding>::make();
    images.push(owe::resource_registry::DescriptorImageBinding { .binding = 2 });
    images.push(owe::resource_registry::DescriptorImageBinding { .binding = 2 });
    auto buffers = rstd::vec::Vec<owe::resource_registry::DescriptorBufferBinding>::make();

    owe::resource_registry::DescriptorSystem descriptors;
    owe::vulkan::Device                      device;
    auto rejected = descriptors.Prepare(device,
                                        0,
                                        layout,
                                        images.as_slice(),
                                        buffers.as_slice(),
                                        owe::resource_registry::DescriptorBindingReuse::Exclusive);
    EXPECT_TRUE(rejected.is_err());
}

TEST(DescriptorSystem, TracksBoundSetsAtArbitraryIndices) {
    owe::resource_registry::DescriptorBindingRecordState state;
    auto layout = owe::resource::DescriptorLayoutHandle {
        .index      = rstd::u64(12),
        .generation = rstd::u64(3),
    };

    EXPECT_TRUE(state.BindRequired(4, layout, VK_NULL_HANDLE));
    EXPECT_FALSE(state.BindRequired(4, layout, VK_NULL_HANDLE));

    state.Push(7);
    EXPECT_FALSE(state.BindRequired(4, layout, VK_NULL_HANDLE));

    state.Push(4);
    EXPECT_TRUE(state.BindRequired(4, layout, VK_NULL_HANDLE));
}

TEST(DescriptorSystem, InvalidatesBindingsFromTheFirstIncompatiblePipelineSet) {
    auto layout = [](rstd::u64 index) {
        return owe::resource::DescriptorLayoutHandle {
            .index      = index,
            .generation = rstd::u64(1),
        };
    };
    auto pipeline = [](rstd::u64 index) {
        return owe::resource::PipelineLayoutHandle {
            .index      = index,
            .generation = rstd::u64(1),
        };
    };

    auto first = rstd::vec::Vec<owe::resource::DescriptorLayoutHandle>::make();
    first.push(layout(rstd::u64(10)));
    first.push(layout(rstd::u64(11)));
    first.push(layout(rstd::u64(12)));
    auto second = rstd::vec::Vec<owe::resource::DescriptorLayoutHandle>::make();
    second.push(layout(rstd::u64(10)));
    second.push(layout(rstd::u64(21)));
    second.push(layout(rstd::u64(12)));

    owe::resource_registry::DescriptorBindingRecordState state;
    state.UsePipeline(pipeline(rstd::u64(1)), first.as_slice(), 7u);
    EXPECT_TRUE(state.BindRequired(0, first[rstd::usize()], VK_NULL_HANDLE));
    EXPECT_TRUE(state.BindRequired(2, first[rstd::usize(2)], VK_NULL_HANDLE));

    state.UsePipeline(pipeline(rstd::u64(2)), second.as_slice(), 7u);
    EXPECT_FALSE(state.BindRequired(0, second[rstd::usize()], VK_NULL_HANDLE));
    EXPECT_TRUE(state.BindRequired(2, second[rstd::usize(2)], VK_NULL_HANDLE));

    state.UsePipeline(pipeline(rstd::u64(3)), second.as_slice(), 8u);
    EXPECT_TRUE(state.BindRequired(0, second[rstd::usize()], VK_NULL_HANDLE));
}

TEST(TextureRecording, TracksTypedUsesInSubmission) {
    auto  textures   = GpuTextures();
    auto  allocation = GpuTexture(textures);
    auto  prepared   = PreparedTexture(allocation.clone());
    auto& context    = textures.Context();
    auto  work       = context.Begin().unwrap();
    ASSERT_TRUE(UploadPixels(work, prepared, rstd::u8(17)).is_ok());
    ASSERT_TRUE(owe::resource_registry::UsePreparedTexture(
        work, prepared, owe::resource_registry::TextureStateKind::Sampled));
    ASSERT_TRUE(owe::resource_registry::UsePreparedTexture(
        work, prepared, owe::resource_registry::TextureStateKind::TransferSource));
    ASSERT_TRUE(owe::resource_registry::UsePreparedTexture(
        work, prepared, owe::resource_registry::TextureStateKind::TransferDestination, {}, true));
    ASSERT_TRUE(UploadPixels(work, prepared, rstd::u8(77)).is_ok());
    ASSERT_TRUE(owe::resource_registry::UsePreparedTexture(
        work, prepared, owe::resource_registry::TextureStateKind::Sampled));
    EXPECT_FALSE(owe::resource_registry::UsePreparedTexture(
        work,
        prepared,
        owe::resource_registry::TextureStateKind::TransferSource,
        owe::resource_registry::TextureSubresourceRange { .base_mip_level = rstd::u32(1) }));
    auto buffer    = CopyPixels(context, work, prepared);
    auto submitted = context.Submit(rstd::move(work)).unwrap();
    ASSERT_TRUE(context.wait(submitted).is_ok());
    for (auto byte : ReadPixels(buffer, rstd::usize(64))) EXPECT_EQ(byte, rstd::u8(77));
    EXPECT_EQ(context.session().validation_errors(), 0u);
}

TEST(TextureRecording, TransitionsDepthAttachmentToDepthSampled) {
    auto  textures = GpuTextures();
    auto& context  = textures.Context();
    if (! context.session().device().capabilities().sampled_depth_d32)
        GTEST_SKIP() << "sampled D32F is unavailable";
    auto allocation = GpuTexture(textures,
                                 owe::resource::TextureDefinition {
                                     .width  = rstd::i32(4),
                                     .height = rstd::i32(4),
                                     .usage  = owe::resource::TextureUsage::DepthAttachment |
                                               owe::resource::TextureUsage::Sampled |
                                               owe::resource::TextureUsage::TransferSource |
                                               owe::resource::TextureUsage::TransferDestination,
                                     .format = vrento::TextureFormat::D32F,
                                 });
    auto prepared   = PreparedTexture(allocation.clone());
    auto work       = context.Begin().unwrap();
    ASSERT_TRUE(owe::resource_registry::UsePreparedTexture(
        work, prepared, owe::resource_registry::TextureStateKind::TransferDestination));
    auto        command = vrento::vulkan::native_recording(work, context.session()).unwrap();
    const auto& view    = allocation->Slots()[rstd::usize()].view;
    VkImageSubresourceRange  range { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
    VkClearDepthStencilValue clear { 0.25f, 0 };
    const auto&              device  = context.session().device().handle();
    auto                     resolve = device.Dispatch().vkGetDeviceProcAddr;
    ASSERT_NE(resolve, nullptr);
    auto clear_depth = reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(
        resolve(*device, "vkCmdClearDepthStencilImage"));
    ASSERT_NE(clear_depth, nullptr);
    clear_depth(command,
                vrento::vulkan::native_texture(view.texture()).unwrap(),
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                &clear,
                1,
                &range);
    ASSERT_TRUE(owe::resource_registry::UsePreparedTexture(
        work, prepared, owe::resource_registry::TextureStateKind::DepthAttachment));
    ASSERT_TRUE(owe::resource_registry::UsePreparedTexture(
        work,
        prepared,
        owe::resource_registry::TextureStateKind::DepthSampled,
        owe::resource_registry::TextureSubresourceRange { .aspect = VK_IMAGE_ASPECT_DEPTH_BIT }));
    EXPECT_FALSE(owe::resource_registry::UsePreparedTexture(
        work,
        prepared,
        owe::resource_registry::TextureStateKind::DepthSampled,
        owe::resource_registry::TextureSubresourceRange { .aspect = VK_IMAGE_ASPECT_STENCIL_BIT }));
    auto buffer    = CopyPixels(context, work, prepared);
    auto submitted = context.Submit(rstd::move(work)).unwrap();
    ASSERT_TRUE(context.wait(submitted).is_ok());
    auto memory = buffer.allocation();
    auto mapped = memory.map().unwrap();
    memory.invalidate().unwrap();
    const auto* depths = reinterpret_cast<const float*>(mapped.data());
    for (unsigned index = 0; index < 16; ++index) EXPECT_FLOAT_EQ(depths[index], 0.25f);
    EXPECT_EQ(context.session().validation_errors(), 0u);
}

TEST(TextureRecording, SharesStateAcrossUsesOfOneTexture) {
    auto                                          textures   = GpuTextures();
    auto                                          allocation = GpuTexture(textures);
    owe::resource_registry::PreparedResourceTable table(rstd::u64(3));
    auto first_prepared       = PreparedTexture(allocation.clone(), rstd::u64(3));
    auto second_prepared      = PreparedTexture(allocation.clone(), rstd::u64(3));
    second_prepared.use.index = rstd::u64(1);
    const auto first_use      = first_prepared.use;
    const auto second_use     = second_prepared.use;
    ASSERT_TRUE(table.Insert(rstd::move(first_prepared)));
    ASSERT_TRUE(table.Insert(rstd::move(second_prepared)));
    auto first  = table.Resolve(first_use);
    auto second = table.Resolve(second_use);
    ASSERT_TRUE(first.is_some());
    ASSERT_TRUE(second.is_some());
    EXPECT_EQ((**first).physical->Slots()[rstd::usize()].view.texture().identity(),
              (**second).physical->Slots()[rstd::usize()].view.texture().identity());
    auto& context = textures.Context();
    auto  work    = context.Begin().unwrap();
    ASSERT_TRUE(UploadPixels(work, **first, rstd::u8(23)).is_ok());
    ASSERT_TRUE(owe::resource_registry::UsePreparedTexture(
        work, **first, owe::resource_registry::TextureStateKind::Sampled));
    ASSERT_TRUE(owe::resource_registry::UsePreparedTexture(
        work, **second, owe::resource_registry::TextureStateKind::ColorAttachment));
    auto buffer    = CopyPixels(context, work, **first);
    auto submitted = context.Submit(rstd::move(work)).unwrap();
    ASSERT_TRUE(context.wait(submitted).is_ok());
    for (auto byte : ReadPixels(buffer, rstd::usize(64))) EXPECT_EQ(byte, rstd::u8(23));
    EXPECT_EQ(context.session().validation_errors(), 0u);
}

TEST(TextureRecording, PreservesFrameBoundaryContentBeforeItsFirstRead) {
    auto                          textures   = GpuTextures();
    auto                          allocation = GpuTexture(textures);
    auto                          prepared   = PreparedTexture(allocation.clone());
    owe::resource::TextureRequest prepared_request;
    prepared_request.kind = owe::resource::TextureRequestKind::RenderTarget;
    prepared_request.content =
        owe::resource::TextureContentFlag(owe::resource::TextureContent::PreserveAcrossFrames);
    prepared.definition = rstd::move(prepared_request);
    auto& context       = textures.Context();
    auto  first_frame   = context.Begin().unwrap();
    ASSERT_TRUE(UploadPixels(first_frame, prepared, rstd::u8(31)).is_ok());
    ASSERT_TRUE(owe::resource_registry::UsePreparedTexture(
        first_frame, prepared, owe::resource_registry::TextureStateKind::Sampled));
    auto first_completion = context.Submit(rstd::move(first_frame)).unwrap();
    allocation->MarkSubmitted(first_completion);
    ASSERT_TRUE(context.wait(first_completion).is_ok());
    {
        auto cancelled = context.Begin().unwrap();
        ASSERT_TRUE(owe::resource_registry::UsePreparedTexture(
            cancelled,
            prepared,
            owe::resource_registry::TextureStateKind::TransferDestination,
            {},
            true));
        ASSERT_TRUE(UploadPixels(cancelled, prepared, rstd::u8(99)).is_ok());
    }
    auto second_frame = context.Begin().unwrap();
    ASSERT_TRUE(owe::resource_registry::UsePreparedTexture(
        second_frame, prepared, owe::resource_registry::TextureStateKind::Sampled));
    auto buffer            = CopyPixels(context, second_frame, prepared);
    auto second_completion = context.Submit(rstd::move(second_frame)).unwrap();
    ASSERT_TRUE(context.wait(second_completion).is_ok());
    for (auto byte : ReadPixels(buffer, rstd::usize(64))) EXPECT_EQ(byte, rstd::u8(31));
    EXPECT_EQ(context.session().validation_errors(), 0u);
}

TEST(MemoryBudgetPolicy, ClassifiesBudgetPressure) {
    owe::resource_registry::MemoryBudgetPolicy memory;
    memory.Refresh(owe::vulkan::MemoryBudgetSnapshot {
        .usage  = 70,
        .budget = 100,
    });
    EXPECT_EQ(memory.Pressure(), owe::resource_registry::MemoryPressure::Normal);
    EXPECT_FALSE(memory.ShouldEvictTransient());

    memory.Refresh(owe::vulkan::MemoryBudgetSnapshot {
        .usage  = 85,
        .budget = 100,
    });
    EXPECT_EQ(memory.Pressure(), owe::resource_registry::MemoryPressure::Elevated);

    memory.Refresh(owe::vulkan::MemoryBudgetSnapshot {
        .usage  = 96,
        .budget = 100,
    });
    EXPECT_EQ(memory.Pressure(), owe::resource_registry::MemoryPressure::Critical);
}

TEST(ExternalResourceBridge, PreparesLayoutAndQueueOwnershipContract) {
    owe::FrameSurfaceLease lease {
        .identity =
            owe::FrameSurfaceIdentity {
                .owner_generation = rstd::u64(2),
                .image_index      = rstd::u32(1),
                .acquire_serial   = rstd::u64(4),
            },
        .reuse =
            owe::FrameSurfaceReuseProof {
                .kind = owe::FrameSurfaceReuseKind::QueueOrdered,
            },
        .format               = VK_FORMAT_R8G8B8A8_UNORM,
        .initial_layout       = VK_IMAGE_LAYOUT_UNDEFINED,
        .initial_queue_family = 3,
        .acquire =
            owe::FrameSurfaceAcquireDependency {
                .kind = owe::FrameSurfaceAcquireKind::QueueOrdered,
            },
        .final_layout       = VK_IMAGE_LAYOUT_GENERAL,
        .final_queue_family = 3,
        .discard_content    = true,
    };
    lease.image.handle = reinterpret_cast<VkImage>(2);
    lease.image.extent = VkExtent3D { 64, 32, 1 };

    owe::resource_registry::ExternalResourceBridge bridge;
    auto prepared = bridge.Prepare(owe::vulkan::DeviceCapabilities {}, lease, 3);
    ASSERT_TRUE(prepared.is_ok());
    auto contract = rstd::move(prepared).unwrap_unchecked();
    EXPECT_EQ(contract.before_copy.Size(), rstd::usize(1));
    EXPECT_EQ(contract.after_copy.Size(), rstd::usize(1));
    EXPECT_EQ(contract.lease.identity.acquire_serial, rstd::u64(4));
}
