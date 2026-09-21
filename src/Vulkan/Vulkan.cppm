export module wescene.vulkan;
export import vrento.vulkan;
export import wescene.shader_compile;

export namespace owe
{
using vrento::ExHandle;
using vrento::ExSwapchain;
using vrento::ExSwapchainOwner;
using vrento::ExSwapchainReadyCallback;
using vrento::ExSwapchainReadyEvent;
using vrento::FrameSurfaceAcquireDependency;
using vrento::FrameSurfaceAcquireKind;
using vrento::FrameSurfaceAcquireResult;
using vrento::FrameSurfaceAcquireStatus;
using vrento::FrameSurfaceCompletionCapability;
using vrento::FrameSurfaceCompletionResult;
using vrento::FrameSurfaceCompletionStatus;
using vrento::FrameSurfaceIdentity;
using vrento::FrameSurfaceLease;
using vrento::FrameSurfaceReuseKind;
using vrento::FrameSurfaceReuseProof;
using vrento::TexTiling;
using vrento::TripleSwapchain;
namespace vulkan
{
using vrento::vulkan::AllocatedBufferParameters;
using vrento::vulkan::AllocatedImageParameters;
using vrento::vulkan::BufferAllocation;
using vrento::vulkan::BufferAllocationRequest;
using vrento::vulkan::BufferBackend;
using vrento::vulkan::BufferManager;
using vrento::vulkan::BufferParameters;
using vrento::vulkan::BufferUploadBatchLease;
using vrento::vulkan::BufferUploadClass;
using vrento::vulkan::BufferUploadTicket;
using vrento::vulkan::CheckGpuOp;
using vrento::vulkan::CreateLocalExSwapchain;
using vrento::vulkan::CreateStagingBuffer;
using vrento::vulkan::DescriptorSetInfo;
using vrento::vulkan::Device;
using vrento::vulkan::DeviceCapabilities;
using vrento::vulkan::ExImageParameters;
using vrento::vulkan::Extension;
using vrento::vulkan::GraphicsPipeline;
using vrento::vulkan::ImageParameters;
using vrento::vulkan::ImagePrepareBackend;
using vrento::vulkan::ImagePrepareContext;
using vrento::vulkan::ImageSlots;
using vrento::vulkan::ImageSlotsRef;
using vrento::vulkan::ImageUploadBatchLease;
using vrento::vulkan::ImageUploadManager;
using vrento::vulkan::ImageUploadTicket;
using vrento::vulkan::Instance;
using vrento::vulkan::InstanceLayer;
using vrento::vulkan::LocalExHandle;
using vrento::vulkan::LocalExSwapchain;
using vrento::vulkan::MemoryBudgetSnapshot;
using vrento::vulkan::MemoryBudgetSource;
using vrento::vulkan::PipelineParameters;
using vrento::vulkan::PreparedImageAllocation;
using vrento::vulkan::QueueParameters;
using vrento::vulkan::RecordedBufferUploads;
using vrento::vulkan::RecordedImageUploads;
using vrento::vulkan::RecordGenerateMipmaps;
using vrento::vulkan::Swapchain;
using vrento::vulkan::TexHash;
using vrento::vulkan::TextureAllocation;
using vrento::vulkan::TextureAllocationRuntime;
using vrento::vulkan::TextureCache;
using vrento::vulkan::TextureKey;
using vrento::vulkan::ToImageParameters;
using vrento::vulkan::ToVkType;
using vrento::vulkan::VALIDATION_LAYER_NAME;
using vrento::vulkan::VertexInputState;
using vrento::vulkan::WP_APPLICATION_NAME;
using vrento::vulkan::WP_VULKAN_VERSION;
} // namespace vulkan
} // namespace owe
