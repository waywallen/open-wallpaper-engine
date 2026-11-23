export module wescene.vulkan_render:scene_draw_preparation;
import rstd;
import wescene.core;
import wescene.vulkan;
import wescene.scene;
import wescene.resource;
import :pipeline_layout;

using namespace rstd::prelude;
using rstd::sync::Arc;

export namespace owe::vulkan
{

struct DrawVertexAttribute {
    String name;
    usize  offset {};

    bool operator==(const DrawVertexAttribute&) const = default;
    auto clone() const -> DrawVertexAttribute { return { name.clone(), offset }; }
    void clone_from(const DrawVertexAttribute& other) { *this = other.clone(); }
};

struct DrawVertexLayout {
    usize                    stride {};
    Vec<DrawVertexAttribute> attributes;

    bool operator==(const DrawVertexLayout&) const = default;
    auto clone() const -> DrawVertexLayout { return { stride, attributes.clone() }; }
    void clone_from(const DrawVertexLayout& other) { *this = other.clone(); }
};

struct DrawSamplerLocation {
    i32                     binding { -1 };
    u32                     set {};
    vrento::texture::Stages stages { vrento::texture::Stages::None };
};

struct ScenePipelineStateKey {
    vrento::MaterialPipelineDesc material;
    VkPrimitiveTopology          topology;
    VkSampleCountFlagBits        samples;
    u32                          viewports;
    u32                          scissors;
    bool                         depth;
    bool                         writes_alpha;

    bool operator==(const ScenePipelineStateKey&) const = default;
};

class SceneDrawPreparation {
public:
    SceneDrawPreparation(Vec<DrawSamplerLocation>, Vec<VkVertexInputBindingDescription>,
                         Vec<VkVertexInputAttributeDescription>, PipelineLayoutRequirement);

    const Vec<DrawSamplerLocation>               samplers;
    const Vec<VkVertexInputBindingDescription>   vertex_bindings;
    const Vec<VkVertexInputAttributeDescription> vertex_attributes;

    auto pipelineLayout(resource::PipelineUseHandle) const -> PipelineLayoutRequirement;
    auto findPipelineState(const Arc<resource::ShaderArtifact>&, const ScenePipelineStateKey&) const
        -> Option<Arc<vrento::vulkan::PipelineStateRecipe>>;
    auto rememberPipelineState(const Arc<resource::ShaderArtifact>&, ScenePipelineStateKey,
                               vrento::vulkan::PipelineResourceRequest) const
        -> Arc<vrento::vulkan::PipelineStateRecipe>;
    void prunePipelineStates() const;

private:
    struct PipelineStateEntry {
        Arc<resource::ShaderArtifact>            shader;
        ScenePipelineStateKey                    key;
        Arc<vrento::vulkan::PipelineStateRecipe> state;
    };
    mutable Vec<PipelineStateEntry> pipeline_states_;
    const PipelineLayoutRequirement layout_template_;
};

struct SceneDrawPreparationStats {
    u64   hits {};
    u64   misses {};
    usize entries {};
};

class SceneDrawPreparationCache : NoCopy, NoMove {
public:
    SceneDrawPreparationCache();
    ~SceneDrawPreparationCache();

    auto prepare(const resource::ShaderArtifact&, slice<String> sampler_members,
                 slice<DrawVertexLayout>)
        -> Result<Arc<SceneDrawPreparation>, resource::ResourceError>;
    void prune();
    void clear();
    auto stats() const -> SceneDrawPreparationStats;

private:
    struct State;
    Box<State> state_;
};

auto PrepareSceneDraw(const resource::ShaderArtifact&, slice<String> sampler_members,
                      slice<DrawVertexLayout>)
    -> Result<Arc<SceneDrawPreparation>, resource::ResourceError>;

} // namespace owe::vulkan
