module wescene.scene_render_pipeline;
import rstd.bench;

using namespace rstd::prelude;
using namespace rstd::literals;

namespace owe
{
namespace
{
auto ScenePreparation(Result<PrepareOutcome, vulkan::RenderPrepareError> result)
    -> Result<PrepareOutcome, ScenePrepareError> {
    if (result.is_err()) return Err(ScenePrepareError::Renderer(rstd::move(result).unwrap_err()));
    return Ok(*result);
}
auto InvalidPreparation(vulkan::PrepareStateError reason = vulkan::PrepareStateError::NoSceneState)
    -> ScenePrepareError {
    return ScenePrepareError::Renderer(vulkan::RenderPrepareError::State(reason));
}
template<class Id>
bool SameId(Id lhs, Id rhs) {
    return lhs.index == rhs.index && lhs.generation == rhs.generation;
}

vulkan::PassInvalidationFlags MaterialInvalidation(SceneMaterialDirtyFlags flags) {
    vulkan::PassInvalidationFlags out = vulkan::PassInvalidationNone;
    if ((flags & SceneMaterialDirtyResources) != 0)
        out |= vulkan::ToPassInvalidationFlags(vulkan::PassInvalidation::Resources);
    if ((flags & SceneMaterialDirtyPipeline) != 0)
        out |= vulkan::ToPassInvalidationFlags(vulkan::PassInvalidation::Pipeline) |
               vulkan::ToPassInvalidationFlags(vulkan::PassInvalidation::Framebuffer);
    return out;
}
} // namespace

SceneRenderPipeline::SceneRenderPipeline(Scene& scene, UniformRuntimeInput& input,
                                         vulkan::VulkanRender&      renderer,
                                         SceneRenderPipelineOptions options)
    : m_scene(scene), m_input(input), m_render(renderer), m_options(options) {}

bool SceneRenderPipeline::hasGraph() const { return m_render.hasSceneRenderState(); }

auto SceneRenderPipeline::pending() const -> PendingScenePreparation {
    return { m_graph_dirty,  m_textures_dirty,  m_frame_work != FrameWork::Idle,
             m_meshes.len(), m_materials.len(), m_gpu_preparation.is_some() };
}

auto SceneRenderPipeline::failed(ScenePrepareError error)
    -> Result<PrepareOutcome, ScenePrepareError> {
    m_ready          = false;
    m_attempt_active = false;
    ++m_stats.preparation_failures;
    m_last_failure = Some(rstd::sync::Arc<ScenePrepareFailure>::make(m_attempt, error));
    return Err(rstd::move(error));
}
auto SceneRenderPipeline::finishAttempt(Result<PrepareOutcome, ScenePrepareError> result)
    -> Result<PrepareOutcome, ScenePrepareError> {
    if (result.is_err()) return failed(rstd::move(result).unwrap_err());
    if (*result == PrepareOutcome::Ready) m_attempt_active = false;
    m_ready = *result == PrepareOutcome::Ready &&
              (m_frame_work == FrameWork::Idle || m_frame_work == FrameWork::Prepared);
    return result;
}
auto SceneRenderPipeline::takeGpuPreparation(bool graph) -> GpuPreparation {
    GpuPreparation captured {
        graph, m_textures_dirty, rstd::move(m_meshes), rstd::move(m_materials)
    };
    m_textures_dirty = false;
    if (graph) {
        m_graph_dirty  = false;
        m_evict_meshes = false;
        m_retention    = vulkan::RenderGraphResourceRetention::KeepSceneTextures;
        ++m_stats.graph_builds;
    }
    return captured;
}
void SceneRenderPipeline::finishGpuPreparation(GpuPreparation captured) {
    if (captured.graph)
        m_render.UpdateCameraFillMode(m_scene, m_options.fill_mode);
    else {
        if (captured.textures) ++m_stats.texture_refreshes;
        m_stats.mesh_refreshes += captured.meshes.len().to_primitive();
        for (const auto& event : captured.materials)
            if (MaterialInvalidation(event.flags) != vulkan::PassInvalidationNone)
                ++m_stats.material_refreshes;
    }
}

void SceneRenderPipeline::setFillMode(FillMode mode) {
    m_options.fill_mode = mode;
    if (m_render.inited()) m_render.UpdateCameraFillMode(m_scene, mode);
}

bool SceneRenderPipeline::resize(unsigned width, unsigned height) {
    if (! m_render.onSwapchainReady(width, height)) return false;
    if (hasGraph()) {
        auto result = rebuild();
        return result.is_ok() && *result == PrepareOutcome::Ready;
    }
    return true;
}

void SceneRenderPipeline::captureChanges() {
    m_graph_dirty |= m_scene.ConsumeRenderGraphDirty();
    auto targets = m_scene.ConsumePreparedRenderTargetDirtyEvents();
    m_textures_dirty |= ! targets.is_empty();
    m_graph_dirty |= ! targets.is_empty();
    for (auto event : m_scene.ConsumePreparedMeshDirtyEvents()) {
        bool merged {};
        for (auto& pending : m_meshes) {
            if (! SameId(pending.mesh, event.mesh)) continue;
            pending.flags |= event.flags;
            merged = true;
            break;
        }
        if (! merged) m_meshes.push(SceneMeshDirtyEvent(event));
        m_graph_dirty |= (event.flags & ~SceneMeshDirtyData) != 0;
    }
    for (auto event : m_scene.ConsumePreparedMaterialDirtyEvents()) {
        bool merged {};
        for (auto& pending : m_materials) {
            if (! SameId(pending.material, event.material)) continue;
            pending.flags |= event.flags;
            merged = true;
            break;
        }
        if (! merged) m_materials.push(SceneMaterialDirtyEvent(event));
        m_graph_dirty |=
            (event.flags & (SceneMaterialDirtyGraph | SceneMaterialDirtyResources |
                            SceneMaterialDirtyPipeline | SceneMaterialDirtyTextureBindings)) != 0;
    }
}

auto SceneRenderPipeline::flushChanges(SceneLoadBenchRecorderView bench)
    -> Result<PrepareOutcome, ScenePrepareError> {
    auto flush_span = SceneLoadSpan(bench, &SceneLoadProbeIds::render_changes_flush);
    if (! m_render.inited())
        return Err(InvalidPreparation(vulkan::PrepareStateError::NotInitialized));
    if (! hasGraph() && ! m_graph_dirty) return Err(InvalidPreparation());
    if (m_gpu_preparation) {
        auto continued = ScenePreparation(m_render.tryContinuePrepare(m_scene));
        if (continued.is_err()) {
            auto previous = m_gpu_preparation.take().unwrap();
            m_graph_dirty = true;
            m_textures_dirty |= previous.textures;
            for (auto event : previous.meshes) m_meshes.push(SceneMeshDirtyEvent(event));
            for (auto event : previous.materials) m_materials.push(SceneMaterialDirtyEvent(event));
            return continued;
        }
        if (*continued == PrepareOutcome::Pending) return continued;
        finishGpuPreparation(m_gpu_preparation.take().unwrap());
    }
    if (m_graph_dirty) {
        [[maybe_unused]] auto reconciliation = m_scene.AudioDemandMut()->BeginReconciliation();
        auto cleared = ScenePreparation(m_render.tryClearLastRenderGraph(m_retention));
        if (cleared.is_err() || *cleared != PrepareOutcome::Ready) return cleared;
        if (m_evict_meshes) m_render.evictUnusedMeshes();
        m_render.configureRenderTargets(m_scene);
        {
            auto span  = SceneLoadSpan(bench, &SceneLoadProbeIds::render_snapshot);
            m_snapshot = ExtractRenderSceneSnapshot(m_scene);
        }
        auto input = [&] {
            auto build_span = SceneLoadSpan(bench, &SceneLoadProbeIds::render_graph_build);
            return BuildSceneRenderState(m_scene, m_snapshot, &m_stats.graph_analysis);
        }();
        if (input.is_err()) return Err(ScenePrepareError::GraphBuild(input.unwrap_err()));
        if (m_options.graphviz) {
            auto dot = vrento::fg::graphviz(input->graph);
            (void)rstd::fs::write(ref<rstd::path::Path>("graph.dot"_str), dot->as_bytes());
        }
        auto compiled = ScenePreparation(m_render.tryInstallSceneRenderState(
            m_scene, rstd::move(input).unwrap(), m_snapshot, bench));
        if (compiled.is_err()) return compiled;
        (void)m_scene.ConsumePreparedMaterialDirtyEvents();
        (void)m_scene.ConsumePreparedMeshDirtyEvents();
        (void)m_scene.ConsumePreparedRenderTargetDirtyEvents();
        (void)m_scene.ConsumeRenderGraphDirty();
        auto captured = takeGpuPreparation(true);
        if (*compiled == PrepareOutcome::Pending) {
            m_gpu_preparation = Some(rstd::move(captured));
            return compiled;
        }
        finishGpuPreparation(rstd::move(captured));
    } else if (m_textures_dirty || ! m_meshes.is_empty() || ! m_materials.is_empty()) {
        Vec<vulkan::RenderItemPreparation> items;
        Vec<SceneMaterialId>               textures;
        auto invalidate = [&](slice<RenderItemId> ids, vulkan::PassInvalidationFlags flags) {
            for (auto id : ids) {
                bool merged {};
                for (auto& item : items) {
                    if (! SameId(item.item, id)) continue;
                    item.flags |= flags;
                    merged = true;
                    break;
                }
                if (! merged) items.push({ id, flags });
            }
        };
        for (const auto& event : m_meshes)
            invalidate(m_snapshot.renderItemsFor(event.mesh),
                       vulkan::ToPassInvalidationFlags(vulkan::PassInvalidation::Content));
        for (const auto& event : m_materials) {
            if ((event.flags & SceneMaterialDirtyTextureBindings) != 0)
                textures.push(SceneMaterialId(event.material));
            auto flags = MaterialInvalidation(event.flags);
            if (flags != vulkan::PassInvalidationNone)
                invalidate(m_snapshot.renderItemsFor(event.material), flags);
        }
        auto prepared = ScenePreparation(
            m_render.tryPrepareResources(m_scene,
                                         m_snapshot,
                                         { items.as_slice(),
                                           textures.as_slice(),
                                           m_meshes.is_empty() && m_materials.is_empty() }));
        if (prepared.is_err()) return prepared;
        if (*prepared == PrepareOutcome::Pending) {
            m_gpu_preparation = Some(takeGpuPreparation(false));
            return prepared;
        }
        if (*prepared == PrepareOutcome::RebuildRequired) {
            m_graph_dirty = true;
            return flushChanges(bench);
        }
        finishGpuPreparation(takeGpuPreparation(false));
    }
    m_meshes.clear();
    m_materials.clear();
    m_textures_dirty = false;
    return Ok(PrepareOutcome::Ready);
}

auto SceneRenderPipeline::rebuild(vulkan::RenderGraphResourceRetention retention, bool evict_meshes,
                                  SceneLoadBenchRecorderView bench)
    -> Result<PrepareOutcome, ScenePrepareError> {
    beginAttempt();
    m_ready       = false;
    m_graph_dirty = true;
    m_evict_meshes |= evict_meshes;
    if (retention == vulkan::RenderGraphResourceRetention::ReleaseSceneTextures)
        m_retention = retention;
    captureChanges();
    auto result = flushChanges(bench);
    return finishAttempt(rstd::move(result));
}

auto SceneRenderPipeline::prepare(const script::FrameInputs& inputs,
                                  SceneLoadBenchRecorderView bench)
    -> Result<PrepareOutcome, ScenePrepareError> {
    if (m_render.framePending()) return Ok(PrepareOutcome::Pending);
    if (m_gpu_preparation || m_frame_work != FrameWork::Idle) return retryPreparation(bench);
    beginAttempt();
    if (! m_render.inited())
        return failed(InvalidPreparation(vulkan::PrepareStateError::NotInitialized));
    if (! hasGraph()) return failed(InvalidPreparation());
    m_ready      = false;
    m_frame_time = inputs.frametime;
    m_scene.SetPointerPosition({ inputs.cursor_x, inputs.cursor_y });
    m_scene.SetPointerInWindow(inputs.cursor_in_window);
    m_input.SetPointerInput(inputs.cursor_x, inputs.cursor_y);
    m_input.SetAudioSpectrum(inputs.audio);
    auto animations_span = SceneLoadSpan(bench, &SceneLoadProbeIds::render_frame_animations);
    m_scene.TickNodeFieldAnimations();
    (void)animations_span.finish();
    {
        auto span = SceneLoadSpan(bench, &SceneLoadProbeIds::render_frame_scripts);
        script::TickSceneScripts(m_scene, inputs);
    }
    animations_span = SceneLoadSpan(bench, &SceneLoadProbeIds::render_frame_animations);
    m_scene.TickCameraPaths();
    m_scene.TickMaterialShaderAnimations();
    m_scene.TickTransformUpdaters();
    (void)animations_span.finish();
    m_frame_work = FrameWork::BeforeRuntime;
    return retryPreparation(bench);
}

auto SceneRenderPipeline::retryPreparation(SceneLoadBenchRecorderView bench)
    -> Result<PrepareOutcome, ScenePrepareError> {
    if (m_render.framePending()) return Ok(PrepareOutcome::Pending);
    if (m_frame_work == FrameWork::Prepared && ready()) return Ok(PrepareOutcome::Ready);
    beginAttempt();
    m_ready = false;
    if (! m_render.inited())
        return failed(InvalidPreparation(vulkan::PrepareStateError::NotInitialized));
    if (! hasGraph() && ! m_graph_dirty) return failed(InvalidPreparation());
    captureChanges();
    if (m_frame_work == FrameWork::BeforeRuntime) {
        if (m_graph_dirty || m_gpu_preparation) {
            auto rebuilt = flushChanges(bench);
            if (rebuilt.is_err() || *rebuilt != PrepareOutcome::Ready)
                return finishAttempt(rstd::move(rebuilt));
        }
        {
            auto span = SceneLoadSpan(bench, &SceneLoadProbeIds::render_frame_runtime);
            m_scene.Runtime().BeforeRender();
        }
        m_frame_work = FrameWork::Resources;
        captureChanges();
    }
    // New mutations during a failed frame are merged without repeating scripts or runtime work.
    auto flushed = flushChanges(bench);
    if (flushed.is_err() || *flushed != PrepareOutcome::Ready)
        return finishAttempt(rstd::move(flushed));
    if (m_frame_work == FrameWork::Resources) m_frame_work = FrameWork::Video;
    if (m_frame_work == FrameWork::Video) {
        auto video = [&] {
            auto span = SceneLoadSpan(bench, &SceneLoadProbeIds::render_frame_video);
            return ScenePreparation(m_render.tryPumpVideoTextures(m_frame_time));
        }();
        if (video.is_err() || *video != PrepareOutcome::Ready)
            return finishAttempt(rstd::move(video));
        m_frame_work = FrameWork::Fonts;
    }
    auto fonts = [&] {
        auto span = SceneLoadSpan(bench, &SceneLoadProbeIds::render_frame_fonts);
        return ScenePreparation(m_render.tryPumpFontAtlases(m_scene));
    }();
    if (fonts.is_err() || *fonts != PrepareOutcome::Ready) return finishAttempt(rstd::move(fonts));
    if (m_frame_work != FrameWork::Idle && m_frame_work != FrameWork::Prepared) {
        ++m_stats.prepared_frames;
        m_frame_work = FrameWork::Prepared;
    }
    m_ready = true;
    return finishAttempt(Ok(PrepareOutcome::Ready));
}

bool SceneRenderPipeline::acknowledgeFrameSubmitted() {
    if (m_frame_work != FrameWork::Prepared) return false;
    m_frame_work = FrameWork::Idle;
    return true;
}

auto SceneRenderPipeline::refreshMaterials() -> Result<PrepareOutcome, ScenePrepareError> {
    beginAttempt();
    m_ready = false;
    captureChanges();
    auto result = flushChanges({});
    return finishAttempt(rstd::move(result));
}

} // namespace owe
