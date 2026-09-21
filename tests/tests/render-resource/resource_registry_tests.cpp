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

struct TextureRuntimeProbe {
    void Pump(double) {}
};

namespace rstd
{

template<>
struct Impl<owe::vulkan::TextureAllocationRuntime, TextureRuntimeProbe>
    : ImplBase<TextureRuntimeProbe> {
    void Pump(double seconds) { this->self().Pump(seconds); }
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

    BufferContentProvider(): content(rstd::vec::Vec<rstd::u8>::make()) {
        content.push(rstd::u8(1));
        content.push(rstd::u8(2));
    }

    auto LoadBuffer(const owe::resource::BufferRequest&)
        -> rstd::Result<rstd::slice<rstd::u8>, owe::resource::ResourceError> {
        ++loads;
        return rstd::Ok(content.as_slice());
    }
};

struct BufferBackend {
    rstd::usize                          allocations { 0 };
    rstd::usize                          writes { 0 };
    rstd::u64                            next_ticket { 0 };
    owe::vulkan::BufferAllocationRequest last_request;

    auto AllocateBuffer(const owe::vulkan::BufferAllocationRequest& request)
        -> rstd::Option<owe::vulkan::BufferAllocation> {
        ++allocations;
        last_request = request;
        if (request.size == 0) return rstd::None();
        return rstd::Some(owe::vulkan::BufferAllocation {});
    }

    auto QueueBufferWrite(rstd::mut_ref<owe::vulkan::BufferAllocation>, rstd::slice<rstd::u8>,
                          VkDeviceSize) -> rstd::Option<owe::vulkan::BufferUploadTicket> {
        ++writes;
        ++next_ticket;
        return rstd::Some(owe::vulkan::BufferUploadTicket { .value = next_ticket });
    }
};

auto TextureAllocation(rstd::uint64_t generation)
    -> rstd::sync::Arc<owe::vulkan::TextureAllocation>;

struct TextureLoader {
    std::atomic<std::size_t>* loads;

    auto LoadTexture(rstd::ref<rstd::str> key) const
        -> rstd::Result<rstd::sync::Arc<vrento::Image>, owe::resource::ResourceError> {
        loads->fetch_add(1, std::memory_order_relaxed);
        auto image = rstd::sync::Arc<vrento::Image>::make();
        image->key = rstd::cppstd::to_string(key);
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

    auto CreateImportedTexture(rstd::ref<vrento::Image>,
                               rstd::Option<rstd::sync::Arc<rstd::dyn<vrento::VideoPlayback>>>)
        -> rstd::Option<owe::vulkan::PreparedImageAllocation> {
        ++creates;
        ++generation;
        return rstd::Some(owe::vulkan::PreparedImageAllocation {
            .allocation = TextureAllocation(generation.to_primitive()),
            .upload     = rstd::Some(owe::vulkan::ImageUploadTicket {
                .value = generation,
            }),
        });
    }

    auto AllocateTexture(owe::vulkan::TextureKey)
        -> rstd::Option<rstd::sync::Arc<owe::vulkan::TextureAllocation>> {
        ++creates;
        ++generation;
        return rstd::Some(TextureAllocation(generation.to_primitive()));
    }

    auto AllocateTransparentTexture(owe::vulkan::TextureKey)
        -> rstd::Option<owe::vulkan::PreparedImageAllocation> {
        ++creates;
        ++transparent_creates;
        ++generation;
        return rstd::Some(owe::vulkan::PreparedImageAllocation {
            .allocation = TextureAllocation(generation.to_primitive()),
        });
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

auto TextureAllocation(rstd::uint64_t generation)
    -> rstd::sync::Arc<owe::vulkan::TextureAllocation> {
    owe::vulkan::ImageSlots slots;
    slots.slots.resize(1);
    slots.slots[0].generation = rstd::u64(generation);
    return rstd::sync::Arc<owe::vulkan::TextureAllocation>::make(rstd::move(slots));
}

} // namespace

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
    EXPECT_EQ(rstd::cppstd::as_string_view((**entry).request.name.as_str()), "frame");
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
    ASSERT_TRUE(
        registry.Publish(handle, TextureAllocation(7), owe::resource::ReadyToken {}).is_some());

    request.source->generation = rstd::u64(12);
    EXPECT_EQ(registry.RegisterImported(request.clone(), content.clone()), handle);
    EXPECT_TRUE(registry.ResolveCurrent(handle).is_some());
    auto logical = registry.ResolveTexture(handle);
    ASSERT_TRUE(logical.is_some());
    EXPECT_EQ((**logical).content_version, rstd::u64(1));
    EXPECT_EQ((**logical).request.source->generation, rstd::u64(12));
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
    ASSERT_TRUE(
        registry.Publish(handle, TextureAllocation(7), owe::resource::ReadyToken {}).is_some());

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
    ASSERT_TRUE(
        registry.Publish(handle, TextureAllocation(7), owe::resource::ReadyToken {}).is_some());
    ASSERT_TRUE(registry.BeginPrepareTransaction());

    auto second_content = owe::resource::ImportedTextureContentIdentity {
        .key      = rstd::string::String::make("second"_str),
        .revision = rstd::u64(2),
    };
    EXPECT_EQ(registry.RegisterImported(request.clone(), second_content.clone()), handle);
    ASSERT_TRUE(
        registry.Publish(handle, TextureAllocation(8), owe::resource::ReadyToken {}).is_some());
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
    ASSERT_TRUE(
        registry.Publish(first, TextureAllocation(1), owe::resource::ReadyToken {}).is_some());
    ASSERT_TRUE(
        registry.Publish(second, TextureAllocation(2), owe::resource::ReadyToken {}).is_some());

    auto active = rstd::array<owe::resource::TextureHandle, 1> { first };
    registry.RetainActive(active.as_slice());
    EXPECT_TRUE(registry.Resolve(first).is_some());
    EXPECT_TRUE(registry.Resolve(second).is_none());
}

TEST(TextureAllocation, OwnsAttachedRuntimeForItsWholeLeaseLifetime) {
    auto weak = rstd::sync::Weak<rstd::dyn<owe::vulkan::TextureAllocationRuntime>>::make();
    {
        auto runtime = rstd::sync::Arc<rstd::dyn<owe::vulkan::TextureAllocationRuntime>>::make(
            TextureRuntimeProbe {});
        weak = runtime.downgrade();
        owe::vulkan::ImageSlots slots;
        auto                    allocation = rstd::sync::Arc<owe::vulkan::TextureAllocation>::make(
            rstd::move(slots), rstd::Some(runtime.clone()));
        runtime.reset();
        EXPECT_FALSE(weak.expired());
        EXPECT_TRUE(static_cast<bool>(allocation));
    }
    EXPECT_TRUE(weak.expired());
}

TEST(TextureAllocation, SubmissionLeaseDelaysRuntimeReleaseAfterActivePlanRemoval) {
    auto weak = rstd::sync::Weak<rstd::dyn<owe::vulkan::TextureAllocationRuntime>>::make();
    owe::resource_registry::SubmissionTracker submissions;
    owe::resource::CompletionToken            completion;
    {
        auto runtime = rstd::sync::Arc<rstd::dyn<owe::vulkan::TextureAllocationRuntime>>::make(
            TextureRuntimeProbe {});
        weak = runtime.downgrade();
        owe::vulkan::ImageSlots slots;
        slots.slots.resize(1);
        auto allocation = rstd::sync::Arc<owe::vulkan::TextureAllocation>::make(
            rstd::move(slots), rstd::Some(runtime.clone()));

        owe::resource::TextureRegistry registry;
        auto                           handle = registry.RegisterImported(
            owe::resource::TextureRequest {
                .kind = owe::resource::TextureRequestKind::Imported,
                .name = rstd::string::String::make("video"_str),
            },
            owe::resource::ImportedTextureContentIdentity {
                .key = rstd::string::String::make("video"_str),
            });
        ASSERT_TRUE(
            registry.Publish(handle, allocation.clone(), owe::resource::ReadyToken {}).is_some());

        owe::resource_registry::PreparedResourceTable table(rstd::u64(3));
        ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedTexture {
            .use      = owe::resource::TextureUseHandle { .generation = rstd::u64(3) },
            .resource = handle,
            .request =
                owe::resource::TextureRequest {
                    .kind = owe::resource::TextureRequestKind::Imported,
                    .name = rstd::string::String::make("video"_str),
                },
            .physical = allocation.clone(),
            .image    = allocation->View(),
        }));
        completion = submissions.Begin(table);
        ASSERT_TRUE(completion.Valid());

        table       = owe::resource_registry::PreparedResourceTable {};
        auto active = rstd::vec::Vec<owe::resource::TextureHandle>::make();
        registry.RetainActive(active.as_slice());
        allocation.reset();
        runtime.reset();
        EXPECT_FALSE(weak.expired());
    }

    auto completed = submissions.Complete(completion);
    ASSERT_TRUE(completed.is_some());
    EXPECT_FALSE(weak.expired());
    completed = rstd::None();
    EXPECT_TRUE(weak.expired());
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
    auto first      = registry.Publish(
        handle, allocation.clone(), owe::resource::ReadyToken { .value = rstd::u64(3) });
    ASSERT_TRUE(first.is_some());
    EXPECT_EQ(*first, rstd::u64(1));

    auto same = registry.Publish(
        handle, allocation.clone(), owe::resource::ReadyToken { .value = rstd::u64(4) });
    ASSERT_TRUE(same.is_some());
    EXPECT_EQ(*same, rstd::u64(1));

    auto replaced = registry.Publish(
        handle, TextureAllocation(8), owe::resource::ReadyToken { .value = rstd::u64(5) });
    ASSERT_TRUE(replaced.is_some());
    EXPECT_EQ(*replaced, rstd::u64(2));

    auto physical = registry.Resolve(handle);
    ASSERT_TRUE(physical.is_some());
    EXPECT_EQ((**physical).generation, rstd::u64(2));
    EXPECT_EQ((**physical).ready.value, rstd::u64(5));
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

    auto published = registry.Publish(
        handle, TextureAllocation(9), owe::resource::ReadyToken {}, rstd::Some(ticket));
    ASSERT_TRUE(published.is_some());
    EXPECT_TRUE(registry.ResolveCurrent(handle).is_none());

    registry.MarkUploadsSubmitted(
        std::span<const owe::vulkan::ImageUploadTicket>(&ticket, std::size_t(1)),
        rstd::Some(owe::resource::ReadyToken { .value = rstd::u64(11) }));
    auto physical = registry.ResolveCurrent(handle);
    ASSERT_TRUE(physical.is_some());
    EXPECT_EQ((**physical).generation, rstd::u64(1));
    EXPECT_EQ((**physical).ready.value, rstd::u64(11));
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
        .request =
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
        rstd::None<rstd::mut_ref<rstd::dyn<owe::vulkan::ImagePrepareBackend>>>(),
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

TEST(ResourcePrepareService, InitializesRetainedHistoryTextureOnce) {
    owe::resource::TextureRegistry         textures;
    owe::resource_registry::BufferRegistry buffers;
    owe::resource_registry::ShaderRegistry shaders;
    BufferBackend                          buffer_backend;
    ImageBackend                           image_backend;
    auto buffer = rstd::dyn<owe::vulkan::BufferBackend>::from_ref(buffer_backend);
    auto image  = rstd::dyn<owe::vulkan::ImagePrepareBackend>::from_ref(image_backend);

    owe::resource::ResourcePlan plan { .generation = rstd::u64(20) };
    plan.textures.push(owe::resource::TexturePlanEntry {
        .handle =
            owe::resource::TextureUseHandle {
                .index      = rstd::u64(1),
                .generation = rstd::u64(20),
            },
        .request =
            owe::resource::TextureRequest {
                .kind       = owe::resource::TextureRequestKind::RenderTarget,
                .name       = rstd::string::String::make("history"_str),
                .definition = rstd::Some(owe::resource::TextureDefinition {
                    .width  = rstd::i32(64),
                    .height = rstd::i32(32),
                }),
                .lifetime   = owe::resource::TextureLifetimeClass::Retained,
                .content    = owe::resource::TextureContentFlag(
                    owe::resource::TextureContent::InitializeTransparent),
            },
        .access = owe::resource::ResourceAccess::ReadWrite,
    });

    owe::resource_registry::ResourcePrepareService service(
        textures, rstd::Some(image.as_mut_ref()), buffers, buffer.as_mut_ref(), shaders);
    auto first = service.Prepare(plan);
    ASSERT_TRUE(first.is_ok());
    EXPECT_EQ(first->TextureCount(), rstd::usize(1));
    EXPECT_EQ(image_backend.transparent_creates, rstd::usize(1));

    auto second = service.Prepare(plan);
    ASSERT_TRUE(second.is_ok());
    EXPECT_EQ(second->TextureCount(), rstd::usize(1));
    EXPECT_EQ(image_backend.transparent_creates, rstd::usize(1));
}

TEST(ResourcePrepareService, MapsLogicalTextureUsesToPlannedPhysicalSlots) {
    owe::resource::TextureRegistry         textures;
    owe::resource_registry::BufferRegistry buffers;
    owe::resource_registry::ShaderRegistry shaders;
    BufferBackend                          buffer_backend;
    ImageBackend                           image_backend;
    auto buffer = rstd::dyn<owe::vulkan::BufferBackend>::from_ref(buffer_backend);
    auto image  = rstd::dyn<owe::vulkan::ImagePrepareBackend>::from_ref(image_backend);

    owe::resource::ResourcePlan plan { .generation = rstd::u64(22) };
    for (rstd::uint64_t index = 0; index < 3; ++index) {
        plan.textures.push(owe::resource::TexturePlanEntry {
            .handle =
                owe::resource::TextureUseHandle {
                    .index      = rstd::u64(index),
                    .generation = rstd::u64(22),
                },
            .request =
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
    EXPECT_EQ((**first).resource, (**second).resource);
    EXPECT_NE((**first).resource, (**third).resource);
    EXPECT_EQ((**first).request.name, "composite-v0"_str);
    EXPECT_EQ((**second).request.name, "composite-v1"_str);
}

TEST(ResourcePrepareService, BatchesDeduplicatesAndCachesImportedTextures) {
    owe::resource::TextureRegistry         textures;
    owe::resource_registry::BufferRegistry buffers;
    owe::resource_registry::ShaderRegistry shaders;
    BufferBackend                          buffer_backend;
    ImageBackend                           image_backend;
    TextureContentProvider                 content_provider;
    auto buffer  = rstd::dyn<owe::vulkan::BufferBackend>::from_ref(buffer_backend);
    auto image   = rstd::dyn<owe::vulkan::ImagePrepareBackend>::from_ref(image_backend);
    auto content = rstd::dyn<owe::resource::TextureContentProvider>::from_ref(content_provider);

    owe::resource::ResourcePlan plan { .generation = rstd::u64(21) };
    for (std::uint64_t index = 0; index < 10; ++index) {
        auto name = index < 2 ? std::string("alias-") + std::to_string(index)
                              : std::string("texture-") + std::to_string(index);
        plan.textures.push(owe::resource::TexturePlanEntry {
            .handle = owe::resource::TextureUseHandle { .index      = rstd::u64(index),
                                                        .generation = rstd::u64(21) },
            .request =
                owe::resource::TextureRequest {
                    .kind = owe::resource::TextureRequestKind::Imported,
                    .name = rstd::string::String::make(rstd::cppstd::as_str(name).unwrap()),
                },
        });
    }

    owe::resource_registry::ResourcePrepareService service(
        textures, rstd::Some(image.as_mut_ref()), buffers, buffer.as_mut_ref(), shaders);
    auto started = service.Begin(plan,
                                 owe::resource_registry::ResourceContentProviders {
                                     .texture = rstd::Some(content.as_mut_ref()),
                                 });
    ASSERT_TRUE(started.is_ok());
    auto        session = rstd::move(started).unwrap_unchecked();
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
        .request =
            owe::resource::TextureRequest {
                .kind = owe::resource::TextureRequestKind::Imported,
                .name = rstd::string::String::make("prepared"_str),
            },
        .physical            = allocation.clone(),
        .image               = allocation->View(),
        .physical_generation = rstd::u64(3),
        .ready               = owe::resource::ReadyToken { .value = rstd::u64(8) },
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
        .request =
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
        .request =
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
        .request =
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

    owe::resource_registry::SubmissionTracker submissions;
    auto                                      completion = submissions.Begin(table);
    ASSERT_TRUE(completion.Valid());
    auto submitted = submissions.Complete(completion);
    ASSERT_TRUE(submitted.is_some());
    EXPECT_EQ(submitted->textures.len(), rstd::usize(1));
    EXPECT_EQ(submitted->buffers.len(), rstd::usize(1));
    EXPECT_EQ(submitted->shaders.len(), rstd::usize(1));
    EXPECT_EQ(submitted->pipelines.len(), rstd::usize(1));
    EXPECT_EQ(submitted->render_passes.len(), rstd::usize(1));
    EXPECT_EQ(submitted->framebuffers.len(), rstd::usize(1));
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

    owe::resource_registry::SubmissionTracker submissions;
    auto                                      completion = submissions.Begin(table);
    EXPECT_TRUE(completion.Valid());
    EXPECT_EQ(submissions.InFlight(), rstd::usize(1));
    auto completed = submissions.Complete(completion);
    ASSERT_TRUE(completed.is_some());
    EXPECT_EQ(completed->program_generation, rstd::u64());
    EXPECT_EQ(completed->descriptors.len(), rstd::usize(1));
    EXPECT_EQ(submissions.InFlight(), rstd::usize());
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

TEST(UploadScheduler, TracksTimelineReadiness) {
    owe::resource_registry::UploadScheduler uploads;
    auto                                    first  = uploads.Reserve();
    auto                                    second = uploads.Reserve();
    EXPECT_LT(first.value, second.value);
    EXPECT_TRUE(uploads.MarkSubmitted(first, owe::vulkan::BufferUploadBatchLease {}));
    EXPECT_TRUE(uploads.MarkSubmitted(second, owe::vulkan::BufferUploadBatchLease {}));
    ASSERT_TRUE(uploads.Pending().is_some());
    EXPECT_EQ(uploads.Pending()->value, second.value);
    EXPECT_EQ(uploads.InFlight(), rstd::usize(2));

    uploads.CompleteThrough(first.value);
    EXPECT_EQ(uploads.InFlight(), rstd::usize(1));
    EXPECT_EQ(uploads.Pending()->value, second.value);
    uploads.CompleteThrough(second.value);
    EXPECT_EQ(uploads.InFlight(), rstd::usize());
    EXPECT_TRUE(uploads.Pending().is_none());
}

TEST(ResourceStateTracker, CompilesTypedUsesIntoBarrierPackets) {
    auto use =
        owe::resource::TextureUseHandle { .index = rstd::u64(6), .generation = rstd::u64(3) };
    auto request = owe::resource::TextureRequest {
        .kind = owe::resource::TextureRequestKind::Imported,
        .name = rstd::string::String::make("sampled"_str),
    };
    owe::resource::ResourcePlan plan { .generation = rstd::u64(3) };
    plan.textures.push(owe::resource::TexturePlanEntry {
        .handle  = use,
        .request = request.clone(),
        .access  = owe::resource::ResourceAccess::Read,
    });

    auto                                          allocation = TextureAllocation(1);
    owe::resource_registry::PreparedResourceTable table(rstd::u64(3));
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedTexture {
        .use = use,
        .resource =
            owe::resource::TextureHandle { .index = rstd::u64(8), .generation = rstd::u64(1) },
        .request             = rstd::move(request),
        .physical            = allocation.clone(),
        .image               = allocation->View(),
        .physical_generation = rstd::u64(2),
        .ready               = owe::resource::ReadyToken { .value = rstd::u64(4) },
    }));

    owe::resource_registry::ResourceStateTracker states;
    ASSERT_TRUE(states.Compile(plan, table));
    auto sampled = states.Prepare(use, owe::resource_registry::TextureStateKind::Sampled);
    ASSERT_TRUE(sampled.is_some());
    EXPECT_EQ(sampled->barrier.oldLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_EQ(sampled->barrier.newLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    auto transfer = states.Prepare(use, owe::resource_registry::TextureStateKind::TransferSource);
    ASSERT_TRUE(transfer.is_some());
    EXPECT_EQ(transfer->src_stage, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    EXPECT_EQ(transfer->dst_stage, VK_PIPELINE_STAGE_TRANSFER_BIT);
    EXPECT_EQ(transfer->barrier.oldLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_EQ(transfer->barrier.newLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    auto discard = states.Prepare(
        use, owe::resource_registry::TextureStateKind::TransferDestination, {}, true);
    ASSERT_TRUE(discard.is_some());
    EXPECT_EQ(discard->barrier.oldLayout, VK_IMAGE_LAYOUT_UNDEFINED);
    EXPECT_EQ(discard->barrier.newLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    auto sampled_after_clear =
        states.Prepare(use, owe::resource_registry::TextureStateKind::Sampled);
    ASSERT_TRUE(sampled_after_clear.is_some());
    EXPECT_EQ(sampled_after_clear->barrier.oldLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    EXPECT_EQ(sampled_after_clear->barrier.newLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

TEST(ResourceStateTracker, TransitionsDepthAttachmentToDepthSampled) {
    auto use =
        owe::resource::TextureUseHandle { .index = rstd::u64(9), .generation = rstd::u64(3) };
    auto request = owe::resource::TextureRequest {
        .kind = owe::resource::TextureRequestKind::RenderTarget,
        .name = rstd::string::String::make("shadow"_str),
    };
    owe::resource::ResourcePlan plan { .generation = rstd::u64(3) };
    plan.textures.push(owe::resource::TexturePlanEntry {
        .handle  = use,
        .request = request.clone(),
        .access  = owe::resource::ResourceAccess::ReadWrite,
    });

    auto                                          allocation = TextureAllocation(1);
    owe::resource_registry::PreparedResourceTable table(rstd::u64(3));
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedTexture {
        .use = use,
        .resource =
            owe::resource::TextureHandle { .index = rstd::u64(10), .generation = rstd::u64(1) },
        .request  = rstd::move(request),
        .physical = allocation.clone(),
        .image    = allocation->View(),
    }));

    owe::resource_registry::ResourceStateTracker states;
    ASSERT_TRUE(states.Compile(plan, table));
    ASSERT_TRUE(states.Set(use, owe::resource_registry::TextureStateKind::DepthAttachment));
    auto barrier = states.Prepare(
        use,
        owe::resource_registry::TextureStateKind::DepthSampled,
        owe::resource_registry::TextureSubresourceRange { .aspect = VK_IMAGE_ASPECT_DEPTH_BIT });
    ASSERT_TRUE(barrier.is_some());
    EXPECT_EQ(barrier->src_stage,
              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
    EXPECT_EQ(barrier->dst_stage, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    EXPECT_EQ(barrier->barrier.oldLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(barrier->barrier.newLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    EXPECT_EQ(barrier->barrier.subresourceRange.aspectMask, VK_IMAGE_ASPECT_DEPTH_BIT);
}

TEST(ResourceStateTracker, SharesStateAcrossUsesOfOneTexture) {
    auto first_use =
        owe::resource::TextureUseHandle { .index = rstd::u64(1), .generation = rstd::u64(3) };
    auto second_use =
        owe::resource::TextureUseHandle { .index = rstd::u64(2), .generation = rstd::u64(3) };
    auto resource =
        owe::resource::TextureHandle { .index = rstd::u64(8), .generation = rstd::u64(1) };
    auto request = owe::resource::TextureRequest {
        .kind = owe::resource::TextureRequestKind::RenderTarget,
        .name = rstd::string::String::make("frame"_str),
    };
    owe::resource::ResourcePlan plan { .generation = rstd::u64(3) };
    plan.textures.push(owe::resource::TexturePlanEntry {
        .handle  = first_use,
        .request = request.clone(),
        .access  = owe::resource::ResourceAccess::Write,
    });
    plan.textures.push(owe::resource::TexturePlanEntry {
        .handle  = second_use,
        .request = request.clone(),
        .access  = owe::resource::ResourceAccess::ReadWrite,
    });

    auto                                          allocation = TextureAllocation(1);
    owe::resource_registry::PreparedResourceTable table(rstd::u64(3));
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedTexture {
        .use      = first_use,
        .resource = resource,
        .request  = request.clone(),
        .physical = allocation.clone(),
        .image    = allocation->View(),
    }));
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedTexture {
        .use      = second_use,
        .resource = resource,
        .request  = rstd::move(request),
        .physical = allocation.clone(),
        .image    = allocation->View(),
    }));

    owe::resource_registry::ResourceStateTracker states;
    ASSERT_TRUE(states.Compile(plan, table));
    ASSERT_TRUE(states.Set(first_use, owe::resource_registry::TextureStateKind::Sampled));
    auto barrier =
        states.Prepare(second_use, owe::resource_registry::TextureStateKind::ColorAttachment);
    ASSERT_TRUE(barrier.is_some());
    EXPECT_EQ(barrier->barrier.oldLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_EQ(barrier->barrier.newLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

TEST(ResourceStateTracker, PreservesFrameBoundaryContentBeforeItsFirstRead) {
    auto use =
        owe::resource::TextureUseHandle { .index = rstd::u64(1), .generation = rstd::u64(3) };
    auto request = owe::resource::TextureRequest {
        .kind = owe::resource::TextureRequestKind::RenderTarget,
        .name = rstd::string::String::make("history"_str),
        .content =
            owe::resource::TextureContentFlag(owe::resource::TextureContent::PreserveAcrossFrames),
    };
    owe::resource::ResourcePlan plan { .generation = rstd::u64(3) };
    plan.textures.push(owe::resource::TexturePlanEntry {
        .handle  = use,
        .request = request.clone(),
        .access  = owe::resource::ResourceAccess::ReadWrite,
    });

    auto                                          allocation = TextureAllocation(1);
    owe::resource_registry::PreparedResourceTable table(rstd::u64(3));
    ASSERT_TRUE(table.Insert(owe::resource_registry::PreparedTexture {
        .use = use,
        .resource =
            owe::resource::TextureHandle { .index = rstd::u64(8), .generation = rstd::u64(1) },
        .request  = rstd::move(request),
        .physical = allocation.clone(),
        .image    = allocation->View(),
    }));

    owe::resource_registry::ResourceStateTracker states;
    ASSERT_TRUE(states.Compile(plan, table));
    auto sampled = states.Prepare(use, owe::resource_registry::TextureStateKind::Sampled);
    ASSERT_TRUE(sampled.is_some());
    EXPECT_EQ(sampled->barrier.oldLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EXPECT_EQ(sampled->barrier.newLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
