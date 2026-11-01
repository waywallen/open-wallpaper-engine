module;

#include <rstd/macro.hpp>

export module wescene.vulkan_render:resource;
import wescene.core;
import rstd;
import rstd.log;
import wescene.types;
export import wescene.resource;
export import wescene.resource_registry;
import wescene.vulkan;
import wescene.scene;
import wescene.load_bench;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::collections::HashMap;
using rstd::sync::Arc;

export namespace owe::vulkan
{

class ShaderReflectionCache;

using resource::SetTextureRequestIfChanged;
using resource::TextureBindingRequest;
using resource::TextureContent;
using resource::TextureContentFlag;
using resource::TextureDefinition;
using resource::TextureDefinitionId;
using resource::TextureLifetimeClass;
using resource::TextureRequest;
using resource::TextureRequestKind;
using resource::TextureUsage;
using resource_registry::FramebufferCacheDiagnostics;
using resource_registry::FramebufferResourceResult;
using resource_registry::FramebufferResourceSystem;
using resource_registry::PipelineCacheDiagnostics;
using resource_registry::PipelineResourceEntry;
using resource_registry::PipelineResourceResult;
using resource_registry::PipelineResourceSystem;

inline bool SameTextureSample(const TextureSample& lhs, const TextureSample& rhs) {
    return lhs.wrapS == rhs.wrapS && lhs.wrapT == rhs.wrapT && lhs.magFilter == rhs.magFilter &&
           lhs.minFilter == rhs.minFilter && lhs.compare_enable == rhs.compare_enable &&
           lhs.compare_op == rhs.compare_op && lhs.border_color == rhs.border_color;
}

using resource::SameTextureBindingRequest;
using resource::SameTextureRequest;

inline void WriteTextureSampleIdentity(PipelineKeyWriter& writer, const TextureSample& sample) {
    WritePipelineScalar(writer, sample.wrapS);
    WritePipelineScalar(writer, sample.wrapT);
    WritePipelineScalar(writer, sample.magFilter);
    WritePipelineScalar(writer, sample.minFilter);
    writer.writeBool(sample.compare_enable);
    WritePipelineScalar(writer, sample.compare_op);
    WritePipelineScalar(writer, sample.border_color);
}

inline void WriteTextureDefinitionIdIdentity(PipelineKeyWriter&         writer,
                                             const TextureDefinitionId& id) {
    writer.writeU64(id.index.to_primitive());
    writer.writeU64(id.generation.to_primitive());
}

inline void WriteTextureDefinitionIdentity(PipelineKeyWriter&       writer,
                                           const TextureDefinition& definition) {
    writer.writeU32(static_cast<rstd::uint32_t>(definition.width.to_primitive()));
    writer.writeU32(static_cast<rstd::uint32_t>(definition.height.to_primitive()));
    WritePipelineScalar(writer, definition.usage);
    WritePipelineScalar(writer, definition.format);
    WriteTextureSampleIdentity(writer, definition.sample);
    writer.writeU32(definition.mip_levels.to_primitive());
    writer.writeU32(definition.samples.to_primitive());
}

inline void WriteTextureRequestIdentity(PipelineKeyWriter& writer, const TextureRequest& request) {
    WritePipelineScalar(writer, request.kind);
    writer.writeString(request.name.as_str());
    writer.writeBool(request.source.is_some());
    if (request.source.is_some()) {
        WriteTextureDefinitionIdIdentity(writer, *request.source);
    }
    writer.writeBool(request.definition.is_some());
    if (request.definition.is_some()) WriteTextureDefinitionIdentity(writer, *request.definition);
    WritePipelineScalar(writer, request.lifetime);
    writer.writeU32(request.content.to_primitive());
}

inline void WriteImageParametersIdentity(PipelineKeyWriter& writer, const ImageParameters& image) {
    writer.writeU64(static_cast<rstd::uint64_t>(reinterpret_cast<rstd::uintptr_t>(image.handle)));
    writer.writeU64(static_cast<rstd::uint64_t>(reinterpret_cast<rstd::uintptr_t>(image.view)));
    writer.writeU32(image.extent.width);
    writer.writeU32(image.extent.height);
    writer.writeU32(image.extent.depth);
    writer.writeU32(image.mipmap_level);
    writer.writeU64(image.generation.to_primitive());
}

inline FramebufferAttachmentIdentity
MakeFramebufferAttachmentIdentity(const TextureRequest& request, const ImageParameters& image) {
    PipelineKeyWriter writer;
    writer.writeString("framebuffer-attachment-v1"_str);
    WriteTextureRequestIdentity(writer, request);
    WriteImageParametersIdentity(writer, image);
    return ToFramebufferAttachmentIdentity(rstd::move(writer).finish());
}

inline FramebufferAttachmentIdentity
MakeFramebufferAttachmentIdentity(const ImageParameters& image) {
    PipelineKeyWriter writer;
    writer.writeString("framebuffer-attachment-image-v1"_str);
    WriteImageParametersIdentity(writer, image);
    return ToFramebufferAttachmentIdentity(rstd::move(writer).finish());
}

inline FramebufferAttachmentDesc MakeFramebufferAttachment(const TextureRequest&  request,
                                                           const ImageParameters& image) {
    return FramebufferAttachmentDesc {
        .view     = image.view,
        .identity = MakeFramebufferAttachmentIdentity(request, image),
    };
}

inline FramebufferAttachmentDesc MakeFramebufferAttachment(const ImageParameters& image) {
    return FramebufferAttachmentDesc {
        .view     = image.view,
        .identity = MakeFramebufferAttachmentIdentity(image),
    };
}

inline VkSampleCountFlagBits TextureSampleCount(unsigned sample_count) {
    switch (sample_count) {
    case 2: return VK_SAMPLE_COUNT_2_BIT;
    case 4: return VK_SAMPLE_COUNT_4_BIT;
    case 8: return VK_SAMPLE_COUNT_8_BIT;
    case 16: return VK_SAMPLE_COUNT_16_BIT;
    case 32: return VK_SAMPLE_COUNT_32_BIT;
    case 64: return VK_SAMPLE_COUNT_64_BIT;
    default: return VK_SAMPLE_COUNT_1_BIT;
    }
}

inline unsigned TextureSampleCountValue(VkSampleCountFlagBits sample_count) {
    return static_cast<unsigned>(sample_count);
}

inline TextureDefinition RenderTargetTextureDefinition(const owe::SceneRenderTarget& rt) {
    return *rt.request(""_str).definition;
}
inline TextureDefinition RenderTargetTextureDefinitionNoMip(const owe::SceneRenderTarget& rt) {
    auto definition       = RenderTargetTextureDefinition(rt);
    definition.mip_levels = u32(1);
    return definition;
}
inline TextureDefinition MsaaTextureDefinition(const owe::SceneRenderTarget& rt,
                                               VkSampleCountFlagBits         samples) {
    return *rt.request(""_str,
                       TextureRequestKind::RenderTargetMsaa,
                       u32(TextureSampleCountValue(samples)))
                .definition;
}
inline TextureDefinition DepthTextureDefinition(const owe::SceneRenderTarget& rt) {
    return *rt.request(""_str, TextureRequestKind::DepthAttachment, u32(rt.sample_count))
                .definition;
}
inline TextureRequest MakeImportedTextureRequest(ref<str>                    name,
                                                 Option<RenderTextureDescId> texture = None()) {
    return { .kind   = TextureRequestKind::Imported,
             .name   = String::make(name),
             .source = texture ? Some(TextureDefinitionId { texture->index, texture->generation })
                               : None() };
}
inline TextureRequest MakeRenderTargetTextureRequest(ref<str> name, const SceneRenderTarget& rt) {
    return rt.request(name);
}
inline TextureRequest MakeRenderTargetNoMipTextureRequest(ref<str>                 name,
                                                          const SceneRenderTarget& rt) {
    auto request                   = rt.request(name);
    request.definition->mip_levels = u32(1);
    return request;
}
inline TextureRequest MakeMsaaTextureRequest(ref<str> name, const SceneRenderTarget& rt,
                                             VkSampleCountFlagBits samples) {
    return rt.request(
        name, TextureRequestKind::RenderTargetMsaa, u32(TextureSampleCountValue(samples)));
}
inline TextureRequest MakeDepthTextureRequest(ref<str> name, const SceneRenderTarget& rt) {
    return rt.request(name, TextureRequestKind::DepthAttachment, u32(rt.sample_count));
}

inline Option<String> ResolveImportedTextureName(const RenderSceneSnapshot& render_scene,
                                                 const TextureRequest&      request) {
    if (request.kind != TextureRequestKind::Imported) return None();
    auto catalog  = rstd::dyn<resource::TextureCatalog>::from_ref(render_scene);
    auto resolved = rstd::None<TextureRequest>();
    if (request.source.is_some()) {
        resolved = catalog->ResolveTexture(*request.source);
        if (! resolved) return None();
    }
    if (resolved.is_none()) {
        resolved = catalog->FindTexture(request.name.as_str());
    }
    if (resolved.is_none()) return None();
    return Some(resolved->name.clone());
}

inline Arc<vrento::Image> MakeMissingTexturePlaceholder(ref<str> key) {
    constexpr rstd::int32_t size = 2;
    auto                    img  = Arc<Image>::make();
    img->content->key            = rstd::into(key);
    auto& header                 = img->header;
    header.width                 = size;
    header.height                = size;
    header.mapWidth              = size;
    header.mapHeight             = size;
    header.format                = TextureFormat::RGBA8;
    header.type                  = ImageType::PNG;
    header.count                 = 1;
    header.sample                = TextureSample { TextureWrap::CLAMP_TO_EDGE,
                                                   TextureWrap::CLAMP_TO_EDGE,
                                                   TextureFilter::LINEAR,
                                                   TextureFilter::LINEAR };
    // 2x2 is pow2, so no mipmap downsampling constraints apply.
    header.mipmap_pow2   = true;
    header.mipmap_larger = false;
    img->content->slots.push(vrento::Image::Slot {});
    auto& slot  = img->content->slots[usize(0)];
    slot.width  = size;
    slot.height = size;
    slot.mipmaps.push(ImageData {});
    auto& mipmap  = slot.mipmaps[usize(0)];
    mipmap.width  = size;
    mipmap.height = size;
    mipmap.size   = isize(static_cast<rstd::ptrdiff_t>(size * size * 4));
    // Opaque magenta (#FF00FF), WE-style missing-texture marker.
    auto* pixels = new uint8_t[static_cast<rstd::size_t>(size * size * 4)];
    for (rstd::int32_t i = 0; i < size * size; ++i) {
        pixels[i * 4 + 0] = 0xFF;
        pixels[i * 4 + 1] = 0x00;
        pixels[i * 4 + 2] = 0xFF;
        pixels[i * 4 + 3] = 0xFF;
    }
    mipmap.data = ImageDataPtr(pixels, [](uint8_t* data) {
        delete[] data;
    });
    img->FinalizeContent();
    return img->content.clone();
}

class SnapshotImportedTextureLoader {
public:
    explicit SnapshotImportedTextureLoader(Arc<SceneImageSource> source)
        : m_source(rstd::move(source)) {}

    auto LoadTexture(ref<str> key) const -> Result<Arc<vrento::Image>, resource::ResourceError> {
        auto parsed = m_source->Parse(key);
        if (parsed.is_ok()) return Ok(parsed.unwrap_unchecked()->content.clone());
        auto error = rstd::move(parsed).unwrap_err_unchecked();
        if (error.kind == ImageParseErrorKind::MissingContent) {
            rstd_warn("texture {} not found, using placeholder", key);
            return Ok(MakeMissingTexturePlaceholder(key));
        }
        return Err(resource::ResourceError {
            .kind    = resource::ResourceErrorKind::BackendFailure,
            .message = rstd::move(error.message),
        });
    }

private:
    Arc<SceneImageSource> m_source;
};

class SnapshotImportedTextureProvider {
public:
    SnapshotImportedTextureProvider(const RenderSceneSnapshot& render_scene,
                                    Arc<SceneImageSource>      source)
        : m_render_scene(render_scene), m_source(rstd::move(source)) {}

    auto Declare(const TextureRequest& request) const
        -> Result<vrento::texture::TextureDesc, resource::ResourceError> {
        auto identity = ResolveTextureContent(request);
        if (identity.is_err()) return Err(identity.unwrap_err());
        auto described = m_source->Describe(identity->key.as_str());
        if (described.is_err()) {
            auto error = rstd::move(described).unwrap_err();
            if (error.kind == ImageParseErrorKind::MissingContent) {
                auto fallback     = MakeMissingTexturePlaceholder(identity->key.as_str());
                auto descriptions = vrento::texture::describe_image(*fallback).unwrap();
                return Ok(descriptions[usize()]);
            }
            return Err(
                resource::ResourceError { .kind    = resource::ResourceErrorKind::BackendFailure,
                                          .message = rstd::move(error.message) });
        }
        const auto& description  = *described;
        auto        descriptions = vrento::texture::describe_image(
            vrento::ImageHeader { .kind   = description.header.type == ImageType::VIDEO
                                                ? vrento::ImageKind::Video
                                                : vrento::ImageKind::Pixels,
                                  .format = description.header.format,
                                  .sample = description.header.sample },
            description.slots.as_slice());
        if (descriptions.is_err())
            return Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingDefinition,
                .message = rstd::format("invalid texture metadata {}", request.name) });
        auto shape = (*descriptions)[usize()];
        for (const auto& slot : *descriptions) {
            if (slot.layers != shape.layers)
                return Err(resource::ResourceError {
                    .kind    = resource::ResourceErrorKind::MissingDefinition,
                    .message = rstd::format("animated texture {} has incompatible subresources",
                                            request.name) });
            // Each slot is uploaded as its own texture, so slots may carry different mip
            // chains; declare the shortest one, which every slot can satisfy.
            if (slot.mip_levels < shape.mip_levels) shape.mip_levels = slot.mip_levels;
        }
        return Ok(rstd::move(shape));
    }

    auto ResolveTextureContent(const TextureRequest& request) const
        -> Result<resource::ImportedTextureContentIdentity, resource::ResourceError> {
        auto record = ResolveRecord(request);
        if (record == nullptr) {
            if (request.source)
                return Err(resource::ResourceError {
                    resource::ResourceErrorKind::MissingDefinition,
                    rstd::format("stale texture source {}", request.name) });
            return Ok(resource::ImportedTextureContentIdentity {
                .key = request.name.clone(),
            });
        }
        auto key = record->desc.url.is_empty() ? record->key.clone()
                                               : String::make(record->desc.url.as_str());
        return Ok(resource::ImportedTextureContentIdentity {
            .key      = rstd::move(key),
            .revision = record->content_revision,
            .scope    = rstd::as_cast<u64>(record->scene_texture.generation),
        });
    }

    auto OpenTextureLoader() const
        -> Result<Arc<dyn<resource::TextureLoader>>, resource::ResourceError> {
        return Ok(Arc<dyn<resource::TextureLoader>>::make(
            SnapshotImportedTextureLoader(m_source.clone())));
    }

    auto ResolveVideoPlayback(const TextureRequest& request) const
        -> Option<Arc<dyn<vrento::VideoPlayback>>> {
        auto record = ResolveRecord(request);
        return record != nullptr && record->video_control.is_some()
                   ? Some(Arc<dyn<vrento::VideoPlayback>>::make(
                         SharedVideoPlayback { record->video_control->clone() }))
                   : None<Arc<dyn<vrento::VideoPlayback>>>();
    }

private:
    const RenderTextureDescRecord* ResolveRecord(const TextureRequest& request) const {
        if (request.source.is_some()) {
            auto record = m_render_scene.textureDesc(RenderTextureDescId {
                .index      = request.source->index,
                .generation = request.source->generation,
            });
            return record;
        }
        auto id = m_render_scene.textureDescId(request.name.as_str());
        return id.is_some() ? m_render_scene.textureDesc(*id) : nullptr;
    }

    const RenderSceneSnapshot& m_render_scene;
    Arc<SceneImageSource>      m_source;
};

class SnapshotTexturePrepareObserver {
public:
    explicit SnapshotTexturePrepareObserver(SceneLoadBenchRecorderView load_bench)
        : m_load_bench(load_bench) {}

    void BeginTexturePlan() {
        m_plan = Some(SceneLoadSpan(m_load_bench, &SceneLoadProbeIds::render_texture_plan));
    }
    void EndTexturePlan() { (void)m_plan.take(); }
    void BeginTextureDecode() {
        m_decode = Some(SceneLoadSpan(m_load_bench, &SceneLoadProbeIds::render_texture_decode));
    }
    void EndTextureDecode() { (void)m_decode.take(); }
    void BeginTextureUpload() {
        m_upload =
            Some(SceneLoadSpan(m_load_bench, &SceneLoadProbeIds::render_texture_upload_prepare));
    }
    void EndTextureUpload() { (void)m_upload.take(); }

private:
    SceneLoadBenchRecorderView m_load_bench;
    Option<SceneLoadSpanGuard> m_plan;
    Option<SceneLoadSpanGuard> m_decode;
    Option<SceneLoadSpanGuard> m_upload;
};

struct RenderingResources {
    Arc<vrento::vulkan::GraphicsScopeCache> scopes {
        Arc<vrento::vulkan::GraphicsScopeCache>::make()
    };

    rstd::Option<rstd::mut_ref<ShaderReflectionCache>> shader_reflection_cache;
    Arc<resource_registry::RenderResourceSystem>       resource_owner {
        Arc<resource_registry::RenderResourceSystem>::make()
    };
    resource_registry::RenderResourceSystem& resources { *resource_owner };
};

} // namespace owe::vulkan

export namespace rstd
{

template<>
struct Impl<owe::resource::TextureCatalog, owe::RenderSceneSnapshot>
    : ImplBase<owe::RenderSceneSnapshot> {
    auto ResolveTexture(owe::resource::TextureDefinitionId id) const
        -> Option<owe::resource::TextureRequest> {
        auto record = this->self().textureDesc(
            owe::RenderTextureDescId { .index = id.index, .generation = id.generation });
        if (record == nullptr) return None();
        auto name = record->desc.url.is_empty() ? record->key.as_str() : record->desc.url.as_str();
        return Some(owe::vulkan::MakeImportedTextureRequest(
            name,
            Some(owe::RenderTextureDescId { .index = id.index, .generation = id.generation })));
    }

    auto FindTexture(ref<str> name) const -> Option<owe::resource::TextureRequest> {
        auto id = this->self().textureDescId(name);
        if (id.is_none()) return None();
        return ResolveTexture(owe::resource::TextureDefinitionId { .index      = id->index,
                                                                   .generation = id->generation });
    }
};

template<>
struct Impl<owe::resource::TextureLoader, owe::vulkan::SnapshotImportedTextureLoader>
    : ImplBase<owe::vulkan::SnapshotImportedTextureLoader> {
    auto LoadTexture(ref<str> key) const
        -> Result<Arc<vrento::Image>, owe::resource::ResourceError> {
        return this->self().LoadTexture(key);
    }
};

template<>
struct Impl<owe::resource::TextureContentProvider, owe::vulkan::SnapshotImportedTextureProvider>
    : ImplBase<owe::vulkan::SnapshotImportedTextureProvider> {
    auto ResolveTextureContent(const owe::resource::TextureRequest& request) const
        -> Result<owe::resource::ImportedTextureContentIdentity, owe::resource::ResourceError> {
        return this->self().ResolveTextureContent(request);
    }

    auto OpenTextureLoader() const
        -> Result<Arc<dyn<owe::resource::TextureLoader>>, owe::resource::ResourceError> {
        return this->self().OpenTextureLoader();
    }

    auto ResolveVideoPlayback(const owe::resource::TextureRequest& request) const
        -> Option<Arc<dyn<vrento::VideoPlayback>>> {
        return this->self().ResolveVideoPlayback(request);
    }
};

template<>
struct Impl<owe::resource::TexturePrepareObserver, owe::vulkan::SnapshotTexturePrepareObserver>
    : ImplBase<owe::vulkan::SnapshotTexturePrepareObserver> {
    void BeginTexturePlan() { this->self().BeginTexturePlan(); }
    void EndTexturePlan() { this->self().EndTexturePlan(); }
    void BeginTextureDecode() { this->self().BeginTextureDecode(); }
    void EndTextureDecode() { this->self().EndTextureDecode(); }
    void BeginTextureUpload() { this->self().BeginTextureUpload(); }
    void EndTextureUpload() { this->self().EndTextureUpload(); }
};

} // namespace rstd
