export module wescene.vulkan_render:vulkan_pass;
import rstd;
import rstd.cppstd;
import wescene.rgraph;
import wescene.vulkan;
import wescene.scene;

import :resource;
import :pipeline_layout;
import :shader_reflection_cache;
import :uniform_buffer;

using namespace rstd::prelude;

export namespace owe
{

namespace vulkan
{

using PassInvalidationFlags = u32;

enum class PassInvalidation : rstd::uint32_t
{
    Resources   = 1u << 0u,
    Pipeline    = 1u << 1u,
    Framebuffer = 1u << 2u,
};

inline constexpr PassInvalidationFlags ToPassInvalidationFlags(PassInvalidation invalidation) {
    return PassInvalidationFlags(static_cast<rstd::uint32_t>(invalidation));
}

inline constexpr PassInvalidationFlags PassInvalidationNone {};
inline constexpr PassInvalidationFlags PassInvalidationAll =
    ToPassInvalidationFlags(PassInvalidation::Resources) |
    ToPassInvalidationFlags(PassInvalidation::Pipeline) |
    ToPassInvalidationFlags(PassInvalidation::Framebuffer);

struct MaterialTextureBindingRefresh {
    PassInvalidationFlags invalidation_flags { PassInvalidationNone };
    bool                  requires_graph_rebuild { false };
};

struct PassTextureRequestDiagnostic {
    std::string                              role;
    u32                                      slot { 0 };
    std::string                              name;
    rstd::Option<resource::TextureUseHandle> use;
    rstd::Option<TextureRequest>             request;
};

struct PassResourceUses {
    rstd::vec::Vec<resource::TextureUseHandle>        textures;
    rstd::vec::Vec<resource::BufferUseHandle>         buffers;
    rstd::vec::Vec<resource::ShaderUseHandle>         shaders;
    rstd::vec::Vec<resource::PipelineUseHandle>       pipelines;
    rstd::vec::Vec<resource::RenderPassUseHandle>     render_passes;
    rstd::vec::Vec<resource::FramebufferUseHandle>    framebuffers;
    rstd::vec::Vec<resource::DescriptorBindingHandle> descriptors;
    rstd::vec::Vec<resource::ExternalUseHandle>       externals;

    friend bool operator==(const PassResourceUses&, const PassResourceUses&) = default;
};

struct GlobalDescriptorBufferUse {
    u32                       binding {};
    u64                       identity {};
    resource::BufferUseHandle buffer;
};

class PreparedPassResources {
public:
    PreparedPassResources(const resource_registry::PreparedResourceTable& prepared,
                          const PassResourceUses&                         uses)
        : m_prepared(rstd::ref<resource_registry::PreparedResourceTable>::from_raw_parts(
              rstd::addressof(prepared))),
          m_uses(rstd::ref<PassResourceUses>::from_raw_parts(rstd::addressof(uses))) {}

    auto Resolve(resource::TextureUseHandle use) const
        -> rstd::Option<rstd::ref<resource_registry::PreparedTexture>> {
        return Contains(m_uses->textures, use) ? m_prepared->Resolve(use) : rstd::None();
    }

    auto Resolve(resource::BufferUseHandle use) const
        -> rstd::Option<rstd::ref<resource_registry::PreparedBufferUse>> {
        return Contains(m_uses->buffers, use) ? m_prepared->Resolve(use) : rstd::None();
    }

    auto Resolve(resource::ShaderUseHandle use) const
        -> rstd::Option<rstd::ref<resource_registry::PreparedShaderUse>> {
        return Contains(m_uses->shaders, use) ? m_prepared->Resolve(use) : rstd::None();
    }

    auto Resolve(resource::PipelineUseHandle use) const
        -> rstd::Option<rstd::ref<resource_registry::PreparedPipeline>> {
        return Contains(m_uses->pipelines, use) ? m_prepared->Resolve(use) : rstd::None();
    }

    auto Resolve(resource::RenderPassUseHandle use) const
        -> rstd::Option<rstd::ref<resource_registry::PreparedRenderPass>> {
        return Contains(m_uses->render_passes, use) ? m_prepared->Resolve(use) : rstd::None();
    }

    auto Resolve(resource::FramebufferUseHandle use) const
        -> rstd::Option<rstd::ref<resource_registry::PreparedFramebuffer>> {
        return Contains(m_uses->framebuffers, use) ? m_prepared->Resolve(use) : rstd::None();
    }

    auto Resolve(resource::DescriptorBindingHandle handle) const
        -> rstd::Option<rstd::ref<resource_registry::PreparedDescriptorBinding>> {
        return Contains(m_uses->descriptors, handle) ? m_prepared->Resolve(handle) : rstd::None();
    }

    auto Resolve(resource::ExternalUseHandle use) const
        -> rstd::Option<rstd::ref<resource_registry::PreparedExternalUse>> {
        return Contains(m_uses->externals, use) ? m_prepared->Resolve(use) : rstd::None();
    }

private:
    template<typename Handle>
    static bool Contains(const rstd::vec::Vec<Handle>& handles, Handle value) {
        for (rstd::usize index {}; index < handles.len(); ++index) {
            if (handles[index] == value) return true;
        }
        return false;
    }

    rstd::ref<resource_registry::PreparedResourceTable> m_prepared;
    rstd::ref<PassResourceUses>                         m_uses;
};

struct PassRecordContext {
    rstd::mut_ref<vvk::CommandBuffer>                              command;
    rstd::ref<PreparedPassResources>                               resources;
    rstd::mut_ref<resource_registry::DescriptorBindingRecordState> descriptor_state;
    Option<ref<resource_registry::PreparedDescriptorBinding>>      global_descriptor;
};

struct PassUpdateContext {
    rstd::mut_ref<rstd::dyn<resource::BufferContentWriter>>               buffers;
    rstd::ref<PreparedPassResources>                                      resources;
    rstd::mut_ref<rstd::dyn<resource_registry::GraphicsResourcePreparer>> graphics;
    rstd::ref<rstd::dyn<SceneTextureAnimationView>>                       textures;
};

struct PassPrepareContext {
    rstd::ref<rstd::dyn<ShaderBackend>>                                   shader_backend;
    rstd::ref<resource_registry::PreparedResourceTable>                   resources;
    rstd::mut_ref<rstd::dyn<resource_registry::GraphicsResourcePreparer>> graphics;
    rstd::ref<PipelineLayoutAssignments>                                  pipeline_layouts;
};

class ResourceDeclarationContext {
public:
    explicit ResourceDeclarationContext(resource::ResourcePlan& plan,
                                        ShaderReflectionCache&  shader_cache)
        : m_plan(plan),
          m_shader_cache(
              rstd::mut_ref<ShaderReflectionCache>::from_raw_parts(rstd::addressof(shader_cache))) {
    }

    auto AddBuffer(resource::BufferRequest request, rstd::slice<rstd::u8> content)
        -> resource::BufferUseHandle {
        auto bytes = rstd::vec::Vec<rstd::u8>::from(content);
        return AddBuffer(rstd::move(request), rstd::move(bytes));
    }

    auto AddBuffer(resource::BufferRequest request) -> resource::BufferUseHandle {
        auto bytes = rstd::vec::Vec<rstd::u8>::with_capacity(request.definition.size);
        bytes.resize(request.definition.size, rstd::u8(0));
        return AddBuffer(rstd::move(request), rstd::move(bytes));
    }

    auto AddSharedBuffer(resource::BufferRequest request) -> resource::BufferUseHandle {
        auto existing = m_shared_buffers.get(request.name);
        if (existing.is_some()) return **existing;
        auto name   = request.name.clone();
        auto handle = AddBuffer(rstd::move(request));
        if (handle.Valid()) (void)m_shared_buffers.insert(rstd::move(name), handle);
        return handle;
    }

    auto AddTexture(resource::TextureRequest request, resource::ResourceAccess access)
        -> resource::TextureUseHandle {
        return m_plan.DeclareTexture(rstd::move(request), access);
    }

    auto AddShader(resource::ShaderRequest request, const SceneShader& shader)
        -> resource::ShaderUseHandle {
        auto handle = resource::ShaderUseHandle {
            .index      = m_next_shader++,
            .generation = m_plan.generation,
        };
        SceneShaderArtifactProvider provider(*m_shader_cache, shader);
        auto                        artifact = provider.LoadShader(request);
        if (artifact.is_err()) return {};
        auto artifact_key = request.clone();
        (void)m_shaders.insert(rstd::move(artifact_key), rstd::move(artifact).unwrap_unchecked());
        m_plan.shaders.push(resource::ShaderPlanEntry {
            .handle  = handle,
            .request = rstd::move(request),
        });
        return handle;
    }

    auto ReservePipeline() -> resource::PipelineUseHandle {
        return resource::PipelineUseHandle {
            .index      = m_next_pipeline++,
            .generation = m_plan.generation,
        };
    }

    auto ReserveRenderPass() -> resource::RenderPassUseHandle {
        return resource::RenderPassUseHandle {
            .index      = m_next_render_pass++,
            .generation = m_plan.generation,
        };
    }

    auto ReserveFramebuffer() -> resource::FramebufferUseHandle {
        return resource::FramebufferUseHandle {
            .index      = m_next_framebuffer++,
            .generation = m_plan.generation,
        };
    }

    auto ReserveExternal() -> resource::ExternalUseHandle {
        return resource::ExternalUseHandle {
            .index      = m_next_external++,
            .generation = m_plan.generation,
        };
    }

    auto LoadBuffer(const resource::BufferRequest& request)
        -> rstd::Result<rstd::slice<rstd::u8>, resource::ResourceError> {
        auto content = m_buffers.get(request.name);
        if (content.is_none()) {
            return rstd::Err(resource::ResourceError {
                .kind    = resource::ResourceErrorKind::MissingContent,
                .message = rstd::format("buffer content {} unavailable", request.name),
            });
        }
        return rstd::Ok((**content).as_slice());
    }

    auto ShaderArtifact(const resource::ShaderRequest& request) const
        -> rstd::Option<rstd::ref<resource::ShaderArtifact>> {
        return m_shaders.get(request);
    }

    auto ScopeResourceName(String local_name) const -> String {
        return rstd::format("plan:{}:{}", m_plan.generation, local_name);
    }

private:
    auto AddBuffer(resource::BufferRequest request, rstd::vec::Vec<rstd::u8> content)
        -> resource::BufferUseHandle {
        auto handle = resource::BufferUseHandle {
            .index      = m_next_buffer++,
            .generation = m_plan.generation,
        };
        auto name = request.name.clone();
        (void)m_buffers.insert(rstd::move(name), rstd::move(content));
        m_plan.buffers.push(resource::BufferPlanEntry {
            .handle  = handle,
            .request = rstd::move(request),
        });
        return handle;
    }
    resource::ResourcePlan&                                       m_plan;
    rstd::mut_ref<ShaderReflectionCache>                          m_shader_cache;
    rstd::u64                                                     m_next_buffer { 0 };
    rstd::u64                                                     m_next_shader { 0 };
    rstd::u64                                                     m_next_pipeline { 0 };
    rstd::u64                                                     m_next_render_pass { 0 };
    rstd::u64                                                     m_next_framebuffer { 0 };
    rstd::u64                                                     m_next_external { 0 };
    rstd::collections::HashMap<String, rstd::vec::Vec<rstd::u8>>  m_buffers;
    rstd::collections::HashMap<String, resource::BufferUseHandle> m_shared_buffers;
    rstd::collections::HashMap<resource::ShaderRequest, resource::ShaderArtifact> m_shaders;
};

class VulkanPass : public rg::Pass {
public:
    VulkanPass()                                                                      = default;
    virtual ~VulkanPass()                                                             = default;
    virtual void                  prepare(Scene&, const Device&, PassPrepareContext&) = 0;
    virtual bool                  update(PassUpdateContext&) { return true; }
    virtual void                  completeUpdate() {}
    virtual void                  record(PassRecordContext&) = 0;
    virtual void                  destory(const Device&)     = 0;
    virtual PassInvalidationFlags finalizeResourceRequests(Scene&) { return PassInvalidationNone; }
    virtual void                  declareResources(ResourceDeclarationContext&) {}
    virtual PassResourceUses      resourceUses() const { return {}; }
    virtual auto                  pipelineLayoutRequirement(const PreparedPassResources&) const
        -> Result<Option<PipelineLayoutRequirement>, resource::ResourceError> {
        return Ok(None());
    }
    virtual auto globalDescriptorBufferUses(const PreparedPassResources&) const
        -> Result<Vec<GlobalDescriptorBufferUse>, resource::ResourceError> {
        return Ok(Vec<GlobalDescriptorBufferUse>::make());
    }
    virtual auto createUniformBufferUpdates(ref<dyn<UniformBindingPrepareContext>>,
                                            const PreparedPassResources&)
        -> Result<Vec<Box<dyn<UniformBufferUpdate>>>, UniformBufferUpdateError> {
        return Ok(Vec<Box<dyn<UniformBufferUpdate>>>::make());
    }
    virtual bool
    prepareResourceStates(rstd::mut_ref<rstd::dyn<resource_registry::TextureStatePreparer>>) {
        return true;
    }
    virtual Option<RenderItemId>        renderItemId() const { return None(); }
    virtual Option<PipelineCacheKey>    pipelineCacheKey() const { return None(); }
    virtual bool                        pipelineCacheHit() const { return false; }
    virtual u64                         pipelineCacheObservedCount() const { return u64(); }
    virtual Option<RenderPassCacheKey>  renderPassCacheKey() const { return None(); }
    virtual bool                        renderPassCacheHit() const { return false; }
    virtual u64                         renderPassCacheObservedCount() const { return u64(); }
    virtual Option<FramebufferCacheKey> framebufferCacheKey() const { return None(); }
    virtual bool                        framebufferCacheHit() const { return false; }
    virtual u64                         framebufferCacheObservedCount() const { return u64(); }
    virtual std::vector<PassTextureRequestDiagnostic> textureRequestDiagnostics() const {
        return {};
    }
    virtual MaterialTextureBindingRefresh
    refreshMaterialTextureBindings(const RenderSceneSnapshot&) {
        return {};
    }
    virtual bool setTextureBinding(u32, TextureBindingRequest) { return false; }
    virtual bool supportsRenderScope() const { return false; }
    virtual bool canJoinRenderScopeAfter(const VulkanPass&) const { return false; }
    virtual void prepareRenderScopeDraw(PassRecordContext&) {}
    virtual void beginRenderScope(PassRecordContext&) {}
    virtual void recordRenderScopeDraw(PassRecordContext&) {}
    virtual void endRenderScope(PassRecordContext&) {}

    bool prepared() const { return m_prepared; }
    void resetPrepared() { setPrepared(false); }

protected:
    void setPrepared(bool v = true) { m_prepared = v; }

private:
    bool m_prepared { false };
};

} // namespace vulkan
} // namespace owe
