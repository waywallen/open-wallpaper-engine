export module wescene.resource_registry;
export import wescene.resource;
export import vrento.resource_registry;
export namespace owe
{
namespace resource_registry = vrento::resource_registry;
namespace vulkan
{
using vrento::vulkan::CanonicalCacheKeyData;
using vrento::vulkan::CanonicalCacheKeyStdHash;
using vrento::vulkan::FramebufferAttachmentDesc;
using vrento::vulkan::FramebufferAttachmentIdentity;
using vrento::vulkan::FramebufferCacheKey;
using vrento::vulkan::FramebufferCacheKeyEqual;
using vrento::vulkan::FramebufferResourceDesc;
using vrento::vulkan::FramebufferResourceRequest;
using vrento::vulkan::MakeFramebufferCacheKey;
using vrento::vulkan::MakeFramebufferResourceDesc;
using vrento::vulkan::MakePipelineCacheKey;
using vrento::vulkan::MakePipelineResourceDesc;
using vrento::vulkan::MakeRenderPassCacheKey;
using vrento::vulkan::MakeRenderPassResourceDesc;
using vrento::vulkan::PipelineCacheKey;
using vrento::vulkan::PipelineCacheKeyEqual;
using vrento::vulkan::PipelineCacheProbe;
using vrento::vulkan::PipelineKeyWriter;
using vrento::vulkan::PipelineResourceDesc;
using vrento::vulkan::PipelineResourceRequest;
using vrento::vulkan::RenderPassCacheKey;
using vrento::vulkan::RenderPassCacheKeyEqual;
using vrento::vulkan::RenderPassResourceDesc;
using vrento::vulkan::SameFramebufferCacheKey;
using vrento::vulkan::SamePipelineCacheKey;
using vrento::vulkan::SameRenderPassCacheKey;
} // namespace vulkan
} // namespace owe
