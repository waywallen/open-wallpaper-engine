module wescene.pkg.parse;
import :scene_context;
import rstd;

using namespace rstd::prelude;

auto owe::ResolveTextureSize(Scene& scene, ref<str> texture) -> Option<array<float, 2>> {
    auto parsed = scene.ParseImageHeader(texture);
    if (parsed.is_err()) return None();
    auto  header = rstd::move(parsed).unwrap_unchecked();
    float width {};
    float height {};
    if (header.isSprite && header.spriteAnim.numFrames() > usize()) {
        const auto& frame = header.spriteAnim.GetCurFrame();
        width             = frame.width;
        height            = frame.height;
    } else {
        width  = static_cast<float>(header.width > 0 ? header.width : header.mapWidth);
        height = static_cast<float>(header.height > 0 ? header.height : header.mapHeight);
    }
    if (width <= 0.0f || height <= 0.0f) return None();
    return Some(array<float, 2> { width, height });
}

void owe::SetUniformConfig(SceneLayerBuildContext& context, const Arc<SceneNode>& node,
                           UniformNodeConfigDraft config) {
    config.configured = true;
    context.layer_resources->uniform_state->RegisterNodeParallaxContract(
        *node, config.object_id, config.parallax);
    for (auto& entry : context.layer_build.uniform_configs) {
        if (entry.node.as_ptr() != node.as_ptr()) continue;
        entry.config = rstd::move(config);
        return;
    }
    context.layer_build.uniform_configs.push(SceneUniformConfigDraft {
        .node   = node.clone(),
        .config = rstd::move(config),
    });
}

void owe::ApplyParallaxUniformConfig(SceneLayerBuildContext& context, const Arc<SceneNode>& node,
                                     const wpscene::ParallaxDepthBinding& parallax, i32 object_id,
                                     bool propagate_to_children) {
    if (! parallax.authored && wpscene::IsZeroParallaxDepth(parallax.depth)) return;
    UniformNodeConfigDraft uniform_config;
    uniform_config.SetParallaxContract(parallax, object_id, propagate_to_children);
    SetUniformConfig(context, node, rstd::move(uniform_config));
}

auto owe::SceneParseContext::NextSyntheticObjectId() -> i32 {
    auto id                  = next_synthetic_object_id;
    next_synthetic_object_id = next_synthetic_object_id.checked_sub(i32(1)).unwrap();
    return id;
}

void owe::RegisterNodeRef(SceneLayerBuildContext& context, i32 id, SceneLayerNodeDraft node) {
    if (node.node.is_some()) {
        context.scene->RegisterNode(**node.node,
                                    id >= i32() ? Some(WallpaperLayerId { .value = id })
                                                : None<WallpaperLayerId>());
    }
    (void)context.layer_build.node_id_map.insert(id, rstd::move(node));
}
