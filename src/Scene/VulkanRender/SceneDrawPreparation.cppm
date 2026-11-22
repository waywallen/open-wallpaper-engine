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

class SceneDrawPreparation {
public:
    SceneDrawPreparation(Vec<DrawSamplerLocation>, Vec<VkVertexInputBindingDescription>,
                         Vec<VkVertexInputAttributeDescription>, PipelineLayoutRequirement);

    const Vec<DrawSamplerLocation>               samplers;
    const Vec<VkVertexInputBindingDescription>   vertex_bindings;
    const Vec<VkVertexInputAttributeDescription> vertex_attributes;

    auto pipelineLayout(resource::PipelineUseHandle) const -> PipelineLayoutRequirement;

private:
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
