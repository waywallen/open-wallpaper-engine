export module wescene.vulkan_render:scene_render_state_input;
import rstd;
import vrento;
import eigen;
import wescene.scene;
import :scene_render_resources;
import :scene_draw;
export import :scene_render_textures;

using namespace rstd::prelude;

export namespace owe::vulkan
{
struct SceneRenderDrawSource {
    fg::PassId            id;
    Box<SceneDraw>        source;
    PassInvalidationFlags invalidation { PassInvalidationNone };
};
struct SceneDepthDrawOrder {
    Vec<SceneNodeId> nodes;

    static auto Depth(SceneNode& node, const Eigen::Matrix4d& view) -> f64 {
        node.UpdateTrans();
        const Eigen::Vector4d center = view * node.ModelTrans() * node.GeometryTransform() *
                                       Eigen::Vector4d(0.0, 0.0, 0.0, 1.0);
        const f64             depth(center.z());
        return depth.is_finite() ? depth : f64();
    }
    bool Matches(Scene& scene) const {
        auto camera = scene.ActiveCameraHandle();
        if (! camera || ! (*camera)->IsPerspective()) return false;
        const auto  view = (*camera)->GetViewMatrix();
        Option<f64> previous;
        for (auto id : nodes) {
            auto* node = scene.ResourceIndex().node(id);
            if (! node) return false;
            auto depth = Depth(*node, view);
            if (previous && *previous > depth) return false;
            previous = Some(depth);
        }
        return true;
    }
};
struct SceneRenderStateInput {
    fg::FrameGraph                     graph;
    SceneRenderTextures                textures;
    Vec<SceneRenderDrawSource>         draws;
    Vec<vrento::vulkan::PassInput>     descriptions;
    fg::PassId                         output_pass, release_pass;
    fg::ResourceVersion                surface;
    Option<resource::TextureUseHandle> result_use;
    resource::ResourcePlan             resource_plan;
    ResourceDeclarationData            resource_data;
    Option<Arc<SceneImageSource>>      image_source;
    Vec<SceneDepthDrawOrder>           depth_orders;

    auto draw(fg::PassId id) const -> Option<ref<SceneDraw>> {
        for (const auto& entry : draws)
            if (entry.id == id) return Some(entry.source.as_ref());
        return None();
    }
};
} // namespace owe::vulkan
