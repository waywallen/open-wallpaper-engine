export module wescene.scene_render_pipeline;
import rstd;
import wescene.core;
export import wescene.scene;
export import wescene.vulkan_render;
export import wescene.scene_prepare_error;
export import wescene.script;
export import wescene.pkg.parse;
import wescene.types;
import wescene.load_bench;

using namespace rstd::prelude;

export namespace owe
{

struct SceneRenderPipelineOptions {
    FillMode fill_mode { FillMode::ASPECTCROP };
    bool     graphviz { false };
};

struct SceneRenderPipelineStats {
    vrento::fg::AnalysisStatistics    graph_analysis;
    vulkan::SceneDrawPreparationStats draw_preparations;
    vulkan::ShaderArtifactCacheStats  shader_artifacts;
    rstd::uint64_t                    graph_builds {};
    rstd::uint64_t                    prepared_frames {};
    rstd::uint64_t                    mesh_refreshes {};
    rstd::uint64_t                    material_refreshes {};
    rstd::uint64_t                    texture_refreshes {};
    rstd::uint64_t                    preparation_failures {};
};

struct PendingScenePreparation {
    bool  graph {}, textures {}, frame {};
    usize meshes {}, materials {};
    bool  gpu {};
};

struct ScenePreparationDiagnostics {
    u64                                          attempt {};
    Option<rstd::sync::Arc<ScenePrepareFailure>> last_failure;
    vulkan::RenderDiagnostics                    renderer;
};

// The borrowed scene, input and renderer must outlive this pipeline; calls are serialized.
class SceneRenderPipeline : NoCopy, NoMove {
public:
    SceneRenderPipeline(Scene&, UniformRuntimeInput&, vulkan::VulkanRender&,
                        SceneRenderPipelineOptions = {});
    auto rebuild(vulkan::RenderGraphResourceRetention =
                     vulkan::RenderGraphResourceRetention::KeepSceneTextures,
                 bool evict_meshes = false, SceneLoadBenchRecorderView = {})
        -> Result<PrepareOutcome, ScenePrepareError>;
    // Updates scene state and GPU preparation; the host owns drawing and time advancement.
    auto prepare(const script::FrameInputs&, SceneLoadBenchRecorderView = {})
        -> Result<PrepareOutcome, ScenePrepareError>;
    auto retryPreparation(SceneLoadBenchRecorderView = {})
        -> Result<PrepareOutcome, ScenePrepareError>;
    bool acknowledgeFrameSubmitted();
    auto refreshMaterials() -> Result<PrepareOutcome, ScenePrepareError>;
    void setFillMode(FillMode);
    bool resize(unsigned width, unsigned height);
    bool hasGraph() const;
    bool ready() const { return m_ready && hasGraph() && m_render.inited(); }
    auto pending() const -> PendingScenePreparation;
    auto snapshot() const -> const RenderSceneSnapshot& { return m_snapshot; }
    auto stats() const -> SceneRenderPipelineStats { return m_stats; }
    auto diagnostics() const -> ScenePreparationDiagnostics {
        return { m_attempt, m_last_failure.clone(), m_render.diagnostics() };
    }

private:
    u64                                          m_attempt {};
    bool                                         m_attempt_active {};
    Option<rstd::sync::Arc<ScenePrepareFailure>> m_last_failure;
    void                                         beginAttempt() {
        if (! m_attempt_active) {
            ++m_attempt;
            m_attempt_active = true;
        }
    }
    auto finishAttempt(Result<PrepareOutcome, ScenePrepareError>)
        -> Result<PrepareOutcome, ScenePrepareError>;
    struct GpuPreparation {
        bool                         graph {}, textures {};
        Vec<SceneMeshDirtyEvent>     meshes;
        Vec<SceneMaterialDirtyEvent> materials;
    };
    auto                   takeGpuPreparation(bool graph) -> GpuPreparation;
    void                   finishGpuPreparation(GpuPreparation);
    Option<GpuPreparation> m_gpu_preparation;
    void                   captureChanges();
    auto flushChanges(SceneLoadBenchRecorderView) -> Result<PrepareOutcome, ScenePrepareError>;
    auto failed(ScenePrepareError) -> Result<PrepareOutcome, ScenePrepareError>;
    enum class FrameWork
    {
        Idle,
        BeforeRuntime,
        Resources,
        Video,
        Fonts,
        Prepared
    };
    FrameWork m_frame_work { FrameWork::Idle };
    double    m_frame_time {};
    bool      m_ready {}, m_graph_dirty {}, m_textures_dirty {}, m_evict_meshes {};
    vulkan::RenderGraphResourceRetention m_retention {
        vulkan::RenderGraphResourceRetention::KeepSceneTextures
    };
    Vec<SceneMeshDirtyEvent>          m_meshes;
    Vec<SceneMaterialDirtyEvent>      m_materials;
    Scene&                            m_scene;
    UniformRuntimeInput&              m_input;
    vulkan::VulkanRender&             m_render;
    SceneRenderPipelineOptions        m_options;
    RenderSceneSnapshot               m_snapshot;
    Vec<vulkan::SceneDepthDrawOrder>  m_depth_orders;
    vulkan::SceneDrawPreparationCache m_draw_preparations;
    vulkan::ShaderArtifactCache       m_shader_artifacts;
    SceneRenderPipelineStats          m_stats;
};

} // namespace owe
