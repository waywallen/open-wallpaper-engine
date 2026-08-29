module wescene.pkg.parse;
import :scene_context;
import rstd;

using namespace rstd::prelude;

void owe::SetUniformConfig(SceneParseContext& context, const Arc<SceneNode>& node,
                           UniformNodeConfigDraft config) {
    config.configured = true;
    context.uniform_state->RegisterNodeParallaxContract(
        *node, config.object_id, config.parallax_depth);
    for (auto& entry : context.uniform_configs) {
        if (entry.node.as_ptr() != node.as_ptr()) continue;
        entry.config = rstd::move(config);
        return;
    }
    context.uniform_configs.push(SceneUniformConfigDraft {
        .node   = node.clone(),
        .config = rstd::move(config),
    });
}

auto owe::FindUniformConfig(const SceneParseContext& context, const SceneNode& node)
    -> const UniformNodeConfigDraft* {
    for (const auto& entry : context.uniform_configs) {
        if (entry.node.as_ptr() == &node) return &entry.config;
    }
    return nullptr;
}

auto owe::SceneParseContext::NextSyntheticObjectId() -> i32 {
    auto id                  = next_synthetic_object_id;
    next_synthetic_object_id = next_synthetic_object_id.checked_sub(i32(1)).unwrap();
    return id;
}

void owe::RegisterNodeRef(SceneParseContext& context, i32 id, SceneParseContext::NodeRef node) {
    if (node.node.is_some()) {
        context.scene->RegisterNode(**node.node,
                                    id >= i32() ? Some(WallpaperLayerId { .value = id })
                                                : None<WallpaperLayerId>());
    }
    (void)context.node_id_map.insert(id, rstd::move(node));
}
