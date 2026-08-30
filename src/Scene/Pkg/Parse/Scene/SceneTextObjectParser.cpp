module;

#include <rstd/macro.hpp>

module wescene.pkg.parse;
import :scene_context;
import eigen;
import wescene.pkg.spec_names;
import wescene.load_bench;
import wescene.core;
import wescene.types;
import rstd;
import rstd.log;
import rstd.cppstd;
import wescene.utils;
import wescene.scene;
import wescene.text;
import wescene.script;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::cppstd::as_str;
using rstd::cppstd::as_string_view;
using rstd::sync::Arc;
using namespace owe;
using namespace Eigen;

namespace owe
{

struct SceneNodeArcHold {
    Arc<SceneNode> node;

    explicit SceneNodeArcHold(Arc<SceneNode> n): node(rstd::move(n)) {}
    SceneNodeArcHold(const SceneNodeArcHold& other): node(other.node.clone()) {}
    SceneNodeArcHold(SceneNodeArcHold&&) noexcept            = default;
    SceneNodeArcHold& operator=(SceneNodeArcHold&&) noexcept = default;
    SceneNodeArcHold& operator=(const SceneNodeArcHold&)     = delete;

    SceneNode* get() const { return node.as_ptr(); }
};

array<i32, 2> TextLayerExtent(const text::TextGeometry& geometry) {
    return {
        std::max(i32(1), rstd::as_cast<i32>(std::ceil(geometry.rt_width))),
        std::max(i32(1), rstd::as_cast<i32>(std::ceil(geometry.rt_height))),
    };
}

std::uint32_t TextPointSizeToPx(float point_size) {
    constexpr float kPointsizeToPx = 4.0f;
    if (! std::isfinite(point_size) || point_size <= 0.0f) return 1;
    auto px = static_cast<std::uint32_t>(std::round(point_size * kPointsizeToPx));
    return std::clamp<std::uint32_t>(px, 1, 1024);
}

array<i32, 2> TextEffectFboExtent(const text::TextGeometry& geometry, u32 scale, u32 fit) {
    if (fit > u32()) {
        const float max_size = std::max(geometry.effect_frame_width, geometry.effect_frame_height);
        if (max_size > 0.0f) {
            const float fit_scale = rstd::as_cast<float>(fit) / max_size;
            return {
                std::max(i32(1),
                         rstd::as_cast<i32>(std::round(geometry.effect_frame_width * fit_scale))),
                std::max(i32(1),
                         rstd::as_cast<i32>(std::round(geometry.effect_frame_height * fit_scale))),
            };
        }
    }
    const float fbo_scale = std::max(1.0f, rstd::as_cast<float>(scale));
    return {
        std::max(i32(1), rstd::as_cast<i32>(std::round(geometry.effect_frame_width / fbo_scale))),
        std::max(i32(1), rstd::as_cast<i32>(std::round(geometry.effect_frame_height / fbo_scale))),
    };
}

struct TextRuntimeFbo {
    std::string name;
    u32         scale { 1 };
    u32         fit {};
};

struct TextRuntimeEffectNode {
    SceneNode*                                       node { nullptr };
    std::shared_ptr<text::TextEffectProjectionState> text_projection;
};

struct TextRuntimeTargets {
    TextRuntimeTargets(Scene& scene_owner, mut_ref<UniformSceneState> state)
        : scene(&scene_owner), uniform_state(state) {}

    Scene*                     scene;
    mut_ref<UniformSceneState> uniform_state;
    std::string                camera_key;
    std::string                composite;
    std::string                effect_final;
    bool                       has_effect { false };
    i32                        layer_w { 1 };
    i32                        layer_h { 1 };
    Vec<TextRuntimeFbo>        fbos;
    Vec<TextRuntimeEffectNode> effect_nodes;

    bool Apply(const text::TextGeometry& geometry) {
        if (scene == nullptr) return false;

        bool changed          = false;
        auto [next_w, next_h] = TextLayerExtent(geometry);
        changed |=
            scene->ResizeRenderTarget(rstd::cppstd::as_str(composite).unwrap(), next_w, next_h);
        if (has_effect) {
            changed |= scene->ResizeRenderTarget(
                rstd::cppstd::as_str(effect_final).unwrap(), next_w, next_h);
        }

        auto camera = scene->CameraMut(rstd::cppstd::as_str(camera_key).unwrap());
        if (camera.is_some()) {
            auto& value = **camera;
            if (value.Width() != rstd::as_cast<double>(next_w) ||
                value.Height() != rstd::as_cast<double>(next_h)) {
                value.SetWidth(rstd::as_cast<double>(next_w));
                value.SetHeight(rstd::as_cast<double>(next_h));
                value.Update();
                changed = true;
            }
        }

        for (const auto& fbo : fbos) {
            auto [w, h] = TextEffectFboExtent(geometry, fbo.scale, fbo.fit);
            changed |= scene->ResizeRenderTarget(rstd::cppstd::as_str(fbo.name).unwrap(), w, h);
        }

        const array<float, 2> effect_size {
            geometry.effect_frame_width,
            geometry.effect_frame_height,
        };
        for (auto& item : effect_nodes) {
            if (item.text_projection) {
                item.text_projection->size = effect_size;
                continue;
            }
            if (item.node == nullptr) continue;
            auto node = scene->ResourceIndex().nodeId(*item.node);
            if (node.is_some()) {
                (void)uniform_state->SetEffectProjectionSize(*node, effect_size);
            }
        }

        layer_w = next_w;
        layer_h = next_h;
        return changed;
    }
};

bool EnsureTextAtlas(Scene& scene, text::FontFace& face) {
    const std::string& atlas_url = face.AtlasUrl();
    if (scene.Texture(rstd::cppstd::as_str(atlas_url).unwrap()).is_some()) return true;
    auto atlas_image = text::BuildAtlasImage(face, rstd::cppstd::as_str(atlas_url).unwrap());
    if (atlas_image.is_none()) return false;
    auto         image = rstd::move(atlas_image).unwrap_unchecked();
    SceneTexture stex;
    stex.url    = atlas_url;
    stex.sample = image->header.sample;
    scene.RegisterTexture(String::make(rstd::cppstd::as_str(atlas_url).unwrap()), rstd::move(stex));
    scene.RegisterRuntimeImage(String::make(rstd::cppstd::as_str(atlas_url).unwrap()),
                               rstd::move(image));
    face.ClearDirtyRects();
    return true;
}

auto UserPropertyValue(Option<ref<rstd::json::Map>> user_props, std::string_view key)
    -> Option<ref<Json>> {
    if (key.empty()) return None();
    auto        props   = rstd_try(user_props);
    auto        value   = rstd_try(props->get(rstd::cppstd::as_str(key).unwrap()));
    const auto& payload = SceneUserPropertyPayload(*value);
    return Some(ref<Json>::from_raw_parts(rstd::addressof(payload)));
}

void ParseTextObjImpl(SceneParseContext& context, wpscene::TextObject& obj) {
    if (! obj.visible) {
        context.scene->MarkLayerVisibilityElidable(WallpaperLayerId { .value = obj.id });
    }
    MarkHiddenLinkSource(context, obj.id);

    // --- determine initial text + whether a runtime binding will rewrite it
    auto text_binding_it      = obj.field_bindings.scripts.find("text");
    bool has_text_script      = (text_binding_it != obj.field_bindings.scripts.end());
    auto pointsize_binding_it = obj.field_bindings.scripts.find("pointsize");
    bool has_pointsize_script = (pointsize_binding_it != obj.field_bindings.scripts.end());
    // Scripts can also drive `text` indirectly: a script attached to any
    // other field (commonly `visible`) writes `thisLayer.text = "..."` from
    // its update() side-effect (e.g. workshop 2283810443's clock). Transform
    // scripts alone should not force large dynamic text RTs.
    bool has_indirect_text_script = false;
    if (! has_text_script) {
        for (const auto& [_, sb] : obj.field_bindings.scripts) {
            if (sb.source.find(".text") != std::string::npos ||
                sb.source.find("[\"text\"]") != std::string::npos ||
                sb.source.find("['text']") != std::string::npos) {
                has_indirect_text_script = true;
                break;
            }
        }
    }
    const bool has_text_user = ! obj.text_user.empty();
    bool wants_dynamic_text = has_text_script || has_indirect_text_script || has_pointsize_script ||
                              has_text_user || context.scene_layer_text_writes;
    bool has_text_effect    = false;
    for (const auto& effect : obj.effects) {
        if (effect.visible || ! effect.visible_user.empty()) {
            has_text_effect = true;
            break;
        }
    }
    const bool linked_source        = context.IsLinkedSource(obj.id);
    const auto text_render_mode     = ResolveTextRenderMode(TextSurfaceRequirements {
        .has_effect        = has_text_effect,
        .copy_background   = obj.copybackground,
        .opaque_background = obj.opaquebackground,
        .linked_source     = linked_source,
    });
    const bool direct_text          = text_render_mode == TextRenderMode::Direct;
    const bool copy_background_seed = has_text_effect || obj.copybackground || linked_source;

    std::string s_text;
    if (obj.text.is_string()) {
        s_text = rstd::cppstd::to_string(*obj.text.as_str());
    } else if (obj.text.is_object()) {
        auto value = obj.text.get("value"_str);
        if (value.is_none()) value = obj.text.get("text"_str);
        if (value.is_some()) {
            auto string = (*value)->as_str();
            if (string.is_some()) s_text = rstd::cppstd::to_string(*string);
        }
    }
    if (has_text_user) {
        auto value = UserPropertyValue(context.user_properties, obj.text_user.name);
        if (value.is_some()) {
            auto text = SceneJsonScalarString(**value);
            if (text.is_some()) s_text = rstd::cppstd::to_string(text->as_str());
        }
    }
    if (s_text.empty() && ! wants_dynamic_text) {
        // Empty text layers can still be transform parents for authored child layers.
        auto node  = Arc<SceneNode>::make(Vector3f(obj.origin.data()),
                                          Vector3f(obj.scale.data()),
                                          Vector3f(obj.angles.data()),
                                          obj.name);
        node->ID() = obj.id;
        node->SetSize({ obj.size[0], obj.size[1] });
        node->SetReflected(obj.reflected);
        AssignNodeFieldAnimations(*node.as_ptr(), obj.field_bindings);
        WireFieldScripts(context, node, obj.field_bindings);
        if (! obj.visible) node->SetVisible(false);
        if (! obj.visible_user.empty())
            node->SetVisibleUserBinding(ToSceneUserVisibilityBinding(obj.visible_user));

        ApplyParallaxUniformConfig(context, node, obj.parallax, obj.id);
        RegisterNodeRef(context,
                        obj.id,
                        SceneParseContext::NodeRef {
                            obj.parent,
                            Some(node.clone()),
                            None(),
                            String::make(rstd::cppstd::as_str(obj.attachment).unwrap()),
                        });
        return;
    }

    // --- font resolution: VFS first (WE shared /assets + pkg overlay),
    //     then host system font dirs.
    std::string font_name;
    if (obj.font.is_string()) {
        font_name = rstd::cppstd::to_string(*obj.font.as_str());
    } else if (obj.font.is_object()) {
        if (auto value = obj.font.get("value"_str); value.is_some()) {
            auto string = (*value)->as_str();
            if (string.is_some()) font_name = rstd::cppstd::to_string(*string);
        }
    }

    text::FontCache::ResolvedBlob resolved;
    // `systemfont_<family>` is WE's alias for a host system font — never exists
    // in the pkg, so skip the VFS round-trip and let fontconfig resolve it.
    // Some scenes write it with a leading dir (e.g. `fonts/systemfont_arial`),
    // so match on the basename.
    const bool is_systemfont =
        std::filesystem::path(font_name).filename().native().starts_with("systemfont_");
    std::string font_source_key;
    if (! font_name.empty() && ! is_systemfont) {
        // scene.json's `font` is a pkg-relative path, e.g. `fonts/2.ttf` or
        // `fonts/workshop/<id>/X.otf`. The pkg mounts at /assets so the full
        // VFS path is /assets/<font_name>.
        std::string vfs_path =
            (std::filesystem::path("/assets") / font_name).lexically_normal().native();
        font_source_key = vfs_path;
        auto key        = rstd::cppstd::as_str(font_source_key).unwrap();
        if (auto cached = context.font_sources.get(key); cached.is_some()) {
            resolved = **cached;
        } else {
            auto blob = fs::ReadFileContent(*context.vfs, vfs_path);
            if (blob.is_ok() && ! blob->empty()) {
                auto blob_str = rstd::move(blob).unwrap_unchecked();
                auto bytes    = std::make_shared<std::vector<std::byte>>(blob_str.size());
                std::memcpy(bytes->data(), blob_str.data(), blob_str.size());
                resolved.bytes  = std::move(bytes);
                resolved.source = vfs_path;
            }
        }
    }
    if (! resolved.bytes) {
        font_source_key = "system:" + font_name;
        auto key        = rstd::cppstd::as_str(font_source_key).unwrap();
        if (auto cached = context.font_sources.get(key); cached.is_some()) {
            resolved = **cached;
        } else {
            resolved = text::FontCache::ResolveSystemFont(font_name, /*fallback_to_any=*/true);
        }
    }
    if (! resolved.bytes) {
        rstd_error("text '{}': could not resolve font '{}'", obj.name, font_name);
        return;
    }

    if (font_source_key.empty()) font_source_key = resolved.source;
    auto source_key = rstd::cppstd::as_str(font_source_key).unwrap();
    if (! context.font_sources.contains_key(source_key)) {
        (void)context.font_sources.insert(String::make(source_key), resolved);
    }
    const auto& font_source = **context.font_sources.get(source_key);

    std::uint32_t px = TextPointSizeToPx(obj.pointsize);

    auto& font_cache = text::EnsureSceneFontCache(*context.scene);
    auto* face       = font_cache.GetFace(font_source, px);
    if (face == nullptr) {
        rstd_error("text '{}': FreeType failed to open '{}'", obj.name, resolved.source);
        return;
    }

    auto shader = text::GetTextSceneShader();
    if (! shader) {
        rstd_error("text '{}': text shader compile failed", obj.name);
        return;
    }
    auto copy_background_shader =
        copy_background_seed ? text::GetTextCopyBackgroundSceneShader() : nullptr;
    if (copy_background_seed && ! copy_background_shader) {
        rstd_error("text '{}': copy-background shader compile failed", obj.name);
        return;
    }

    // Populate the seed text's glyphs up front so the first SetText has the
    // initial layout's bbox. Runtime SetText calls (from the script actuator)
    // do their own Populate of the latest string each tick.
    {
        auto seed = text::DecodeUtf8(s_text);
        face->Populate(seed);
    }

    // --- atlas-texture registration. We snapshot the per-face CPU atlas
    // (seed glyphs + the white cell) and register it with the Scene.
    // TextureCache::CreateTex will pick this up on first material bind.
    // Subsequent glyph adds emit dirty rects which the renderer re-uploads
    // each frame via TextureCache::PumpFontAtlases.
    const std::string& atlas_url = face->AtlasUrl();
    if (! EnsureTextAtlas(*context.scene, *face)) {
        rstd_error("text '{}': atlas snapshot failed", obj.name);
        return;
    }

    // --- mesh capacity. Static text exactly fits its initial layout;
    //     dynamic text reserves headroom so SetText can grow
    //     the string at runtime without reallocating GPU buffers.
    std::size_t initial_codepoints = text::DecodeUtf8(s_text).size();
    bool        has_bg             = obj.opaquebackground;
    std::size_t peak_quads;
    if (wants_dynamic_text) {
        // The glyph mesh renders into the layer RT (sized below to the same
        // ceiling), so the only quads that can ever be visible are those that
        // fit the RT grid. Budget to that cap — terminal/log scripts (e.g.
        // 2268178377) append unbounded text but the layouter clips everything
        // past the RT anyway. Conservative narrow-glyph advance avoids
        // undercounting columns for tight fonts.
        const auto& fm  = face->Metrics();
        const float adv = std::max(1.0f, static_cast<float>(fm.pixel_size) * 0.25f);
        const float lh = fm.line_height > 1.0f ? fm.line_height : static_cast<float>(fm.pixel_size);
        const float obj_w        = obj.size[0] > 0.0f ? obj.size[0] : 1024.0f;
        const float obj_h        = obj.size[1] > 0.0f ? obj.size[1] : 256.0f;
        const float rt_w         = std::max(1024.0f, obj_w * 3.0f);
        const float rt_h         = std::max(256.0f, obj_h * 2.0f);
        const std::size_t cols   = static_cast<std::size_t>(std::ceil(rt_w / adv));
        const std::size_t rows   = static_cast<std::size_t>(std::ceil(rt_h / std::max(1.0f, lh)));
        const std::size_t rt_cap = std::clamp<std::size_t>(cols * rows, 64, 16384);
        peak_quads               = std::max<std::size_t>(initial_codepoints * 4, rt_cap);
        if (has_bg) ++peak_quads;
    } else {
        peak_quads = initial_codepoints + (has_bg ? 1u : 0u);
        if (peak_quads == 0) return;
    }

    const bool supports_runtime_text_write = wants_dynamic_text || context.scene_has_scripts;
    auto       sp_mesh = std::make_shared<SceneMesh>(/*dynamic=*/supports_runtime_text_write);
    {
        SceneVertexArray vertex(MakeAttrSet({ VAttr::Position, VAttr::TexCoord, VAttr::Color }),
                                usize(peak_quads * 4));
        sp_mesh->AddVertexArray(std::move(vertex));
        sp_mesh->AddIndexArray(SceneIndexArray(usize(peak_quads * 6)));
    }
    {
        SceneMaterial material;
        material.name     = "text";
        material.textures = { atlas_url };
        material.defines  = { "g_Texture0" };
        material.blenmode =
            direct_text || copy_background_seed ? BlendMode::Translucent : BlendMode::Normal;
        material.customShader.shader = shader;
        sp_mesh->AddMaterial(std::move(material));
    }

    // --- layouter owns the cache (FontFace lifetime) + mesh ref + style.
    text::TextLayoutStyle style;
    style.color                 = { obj.color[0], obj.color[1], obj.color[2] };
    style.alpha                 = obj.alpha;
    style.brightness            = obj.brightness;
    style.opaquebackground      = has_bg;
    style.background_color      = { obj.backgroundcolor[0],
                                    obj.backgroundcolor[1],
                                    obj.backgroundcolor[2] };
    style.background_brightness = obj.backgroundbrightness;
    style.halign                = obj.horizontalalign.empty() ? obj.alignment : obj.horizontalalign;
    style.padding               = rstd::as_cast<float>(obj.padding);

    auto align_or_default = [](std::string      value,
                               std::string_view fallback,
                               std::string_view negative,
                               std::string_view positive) {
        if (! value.empty()) return value;
        if (fallback.find(negative) != std::string::npos) return std::string(negative);
        if (fallback.find(positive) != std::string::npos) return std::string(positive);
        return std::string("center");
    };
    const std::string initial_halign =
        align_or_default(obj.horizontalalign, obj.alignment, "left", "right");
    const std::string initial_valign =
        align_or_default(obj.verticalalign, obj.alignment, "top", "bottom");
    style.halign = initial_halign;

    auto layouter = std::make_shared<text::TextLayouter>(face, sp_mesh, style, peak_quads);
    layouter->SetText(s_text);
    auto current_text       = std::make_shared<std::string>(s_text);
    auto current_point_size = std::make_shared<double>(obj.pointsize);

    auto  initial_metrics = layouter->Metrics();
    float text_w          = initial_metrics.text_width;
    float text_h          = initial_metrics.text_height;
    float text_source_w   = initial_metrics.source_width;
    float text_source_h   = initial_metrics.source_height;
    if (text_w <= 0.0f || text_h <= 0.0f) {
        // Empty seed (scripted-only text). Fake a 1×1 bbox so SceneNode /
        // parallax setup still works; the runtime actuator scales the
        // compose node to actual text dims each tick.
        initial_metrics.text_width  = 1.0f;
        initial_metrics.text_height = 1.0f;
        text_w                      = initial_metrics.text_width;
        text_h                      = initial_metrics.text_height;
    }
    if (text_source_w <= 0.0f) initial_metrics.source_width = text_w;
    if (text_source_h <= 0.0f) initial_metrics.source_height = text_h;
    text_source_w = initial_metrics.source_width;
    text_source_h = initial_metrics.source_height;

    auto        sp_node     = Arc<SceneNode>::make(Vector3f(obj.origin.data()),
                                                   Vector3f(obj.scale.data()),
                                                   Vector3f(obj.angles.data()),
                                                   direct_text ? obj.name : std::string {});
    const float text_bbox_w = text_w + 2.0f * style.padding;
    const float text_bbox_h = text_h + 2.0f * style.padding;
    sp_node->SetSize({ text_bbox_w, text_bbox_h });
    sp_node->AddMesh(sp_mesh);

    struct TextAnchorState {
        std::string horizontal;
        std::string vertical;
        Vector3f    origin;
        float       width { 1.0f };
        float       height { 1.0f };
    };
    auto anchor_state = std::make_shared<TextAnchorState>(TextAnchorState {
        .horizontal = initial_halign,
        .vertical   = initial_valign,
        .origin     = Vector3f(obj.origin.data()),
        .width      = text_w,
        .height     = text_h,
    });

    auto layer_node =
        direct_text
            ? sp_node.clone()
            : Arc<SceneNode>::make(Vector3f::Zero(), Vector3f::Ones(), Vector3f::Zero(), obj.name);
    layer_node->SetReflected(obj.reflected);
    layer_node->ID() = obj.id;

    const float                    object_w = obj.size[0] > 0.0f ? obj.size[0] : text_bbox_w;
    const float                    object_h = obj.size[1] > 0.0f ? obj.size[1] : text_bbox_h;
    const text::TextGeometryPolicy geometry_policy {
        .frame_width        = object_w,
        .frame_height       = object_h,
        .dynamic            = wants_dynamic_text,
        .has_effect         = has_text_effect,
        .frame_bound        = linked_source,
        .preserve_text_bbox = has_bg || obj.copybackground,
    };
    const auto initial_geometry = text::ResolveTextGeometry(geometry_policy, layouter->Metrics());
    const auto [initial_layer_w, initial_layer_h] = TextLayerExtent(initial_geometry);
    auto&      scene                              = *context.scene;
    const auto text_node_id                       = scene.RegisterNode(
        *layer_node,
        obj.id >= i32() ? Some(WallpaperLayerId { .value = obj.id }) : None<WallpaperLayerId>());
    scene.RegisterNode(*sp_node);
    std::shared_ptr<TextRuntimeTargets> runtime_targets;
    std::string                         link_output;
    if (direct_text) {
        UniformNodeConfigDraft text_sv;
        text_sv.SetParallaxContract(obj.parallax, obj.id);
        SetUniformConfig(context, sp_node, rstd::move(text_sv));
    } else {
        context.text_uniform_configs.push(SceneParseContext::TextUniformConfigDraft {
            .node = sp_node.clone(),
        });
        runtime_targets = std::make_shared<TextRuntimeTargets>(
            scene, mut_ref<UniformSceneState>::from_raw_parts(context.uniform_state.as_ptr()));
        const std::string addr = rstd::cppstd::to_string(
            scene.NodeResourceKey(text_node_id, "text_camera"_str).as_str());
        const std::string composite = rstd::cppstd::to_string(
            scene.NodeResourceKey(text_node_id, "text_composite"_str).as_str());
        const std::string effect_final = rstd::cppstd::to_string(
            scene.NodeResourceKey(text_node_id, "text_effect_final"_str).as_str());
        runtime_targets->camera_key   = addr;
        runtime_targets->composite    = composite;
        runtime_targets->effect_final = effect_final;
        runtime_targets->has_effect   = has_text_effect;
        runtime_targets->layer_w      = initial_layer_w;
        runtime_targets->layer_h      = initial_layer_h;

        // Per-layer ortho camera. effect_camera_node sits at origin so the
        // view matrix is identity; ortho extents = bbox so glyph pixel
        // coords (centered around 0) map directly to [-1, +1] NDC.
        auto text_camera = Arc<SceneCamera>::make(
            SceneCamera::MakeOrthographic(rstd::as_cast<double>(initial_layer_w),
                                          rstd::as_cast<double>(initial_layer_h),
                                          -1.0,
                                          1.0));
        text_camera->AttatchNode(sp_node.as_ptr());
        scene.RegisterCamera(String::make(rstd::cppstd::as_str(addr).unwrap()),
                             text_camera.clone());

        SceneRenderTarget text_target {
            .width                = initial_layer_w,
            .height               = initial_layer_h,
            .allowReuse           = true,
            .force_clear          = ! copy_background_seed,
            .clear_on_first_write = false,
            .preserve_on_write    = copy_background_seed,
        };
        if (has_text_effect) {
            scene.RegisterRenderTarget(String::make(as_str(composite).unwrap()), text_target);
            scene.RegisterRenderTarget(String::make(as_str(effect_final).unwrap()),
                                       rstd::move(text_target));
        } else {
            scene.RegisterRenderTarget(String::make(as_str(composite).unwrap()),
                                       rstd::move(text_target));
        }

        layer_node->CopyTrans(*sp_node.as_ptr());
        layer_node->SetSize({ initial_geometry.draw_width, initial_geometry.draw_height });
        if (linked_source) {
            const auto link_source = WallpaperLayerId { .value = obj.id };
            const auto link_width = rstd::cmp::max(i32(1), rstd::as_cast<i32>(std::ceil(object_w)));
            const auto link_height =
                rstd::cmp::max(i32(1), rstd::as_cast<i32>(std::ceil(object_h)));
            scene.RegisterLayerLinkSource(
                link_source, *sp_node.as_ptr(), { link_width, link_height });
            link_output = scene.EnsureLinkRenderTarget(link_source, *sp_node.as_ptr());
        }

        auto layer = std::make_shared<SceneNodeLayer>(has_text_effect ? layer_node.as_ptr()
                                                                      : sp_node.as_ptr(),
                                                      rstd::as_cast<float>(initial_layer_w),
                                                      rstd::as_cast<float>(initial_layer_h),
                                                      composite);
        sp_node->AttachLayer(layer);

        if (copy_background_seed) {
            auto bg_node = Arc<SceneNode>::make();
            bg_node->SetCamera("effect");
            auto bg_mesh = std::make_shared<SceneMesh>();
            bg_mesh->ChangeMeshDataFrom(*scene.DefaultEffectMesh());
            SceneMaterial bg_material;
            bg_material.name                = "text_copybackground";
            bg_material.textures            = { rstd::cppstd::to_string(SpecTex_Default) };
            bg_material.defines             = { "g_Texture0" };
            bg_material.blenmode            = BlendMode::Normal;
            bg_material.customShader.shader = copy_background_shader;
            bg_mesh->AddMaterial(std::move(bg_material));
            bg_node->AddMesh(bg_mesh);

            auto text_projection =
                std::make_shared<text::TextEffectProjectionState>(text::TextEffectProjectionState {
                    .node = layer_node.clone(),
                    .size = { initial_geometry.effect_frame_width,
                              initial_geometry.effect_frame_height },
                });
            context.text_uniform_configs.push(SceneParseContext::TextUniformConfigDraft {
                .node              = bg_node.clone(),
                .effect_projection = text_projection,
            });
            runtime_targets->effect_nodes.push_back(TextRuntimeEffectNode {
                .node = bg_node.as_ptr(), .text_projection = rstd::move(text_projection) });
            layer->AddPrefillNode(SceneImageEffectNode {
                .output    = SceneEffectTarget::LayerNext(),
                .sceneNode = bg_node.clone(),
            });
        }

        UniformNodeConfigDraft compose_sv;
        compose_sv.SetParallaxContract(obj.parallax, obj.id);

        ShaderValueMap effect_base = NeutralColorUniforms(context.global_base_uniforms);

        struct LoadedTextMaterial {
            wpscene::Material      source;
            SceneMaterial          material;
            UniformNodeConfigDraft sv;
            ShaderInfo             shader_info;
        };
        auto load_passthrough_material = [&](std::string_view input) -> Option<LoadedTextMaterial> {
            auto pt_json =
                LoadJsonFile(*context.vfs, "/assets/materials/util/effectpassthrough.json");
            if (! pt_json) {
                rstd_error("text '{}': parse effectpassthrough.json failed", obj.name);
                return None();
            }
            wpscene::Material pt_mat;
            if (! pt_mat.FromJson(*pt_json)) {
                rstd_error("text '{}': Material::FromJson failed", obj.name);
                return None();
            }
            if (pt_mat.textures.empty())
                pt_mat.textures.push_back(std::string(input));
            else
                pt_mat.textures[0] = std::string(input);

            SceneMaterial          mat;
            UniformNodeConfigDraft sv;
            ShaderInfo             si;
            si.baseConstSvs      = effect_base;
            auto material_result = BuildMaterial(*context.vfs,
                                                 *context.shader_cache,
                                                 context.shader_environment,
                                                 pt_mat,
                                                 scene,
                                                 rstd::move(si));
            if (material_result.is_err()) {
                rstd_error("text '{}': compose BuildMaterial failed", obj.name);
                return None();
            }
            auto material_build = rstd::move(material_result).unwrap_unchecked();
            mat                 = rstd::move(material_build.material);
            si                  = rstd::move(material_build.shader_info);
            LoadConstvalue(mat, pt_mat, si);
            mat.blenmode = BlendMode::Translucent;
            return Some(LoadedTextMaterial {
                .source      = std::move(pt_mat),
                .material    = std::move(mat),
                .sv          = std::move(sv),
                .shader_info = std::move(si),
            });
        };

        if (has_text_effect) {
            SceneMaterial final_state;
            final_state.blenmode    = BlendMode::Normal;
            final_state.depth_test  = false;
            final_state.depth_write = false;
            layer->SetFullscreen(true);
            layer->SetFinalTarget(effect_final);
            layer->SetFinalMaterialState(final_state);

            for (const auto& wpeffobj : obj.effects) {
                if (! wpeffobj.visible && wpeffobj.visible_user.empty()) continue;

                auto effect             = std::make_shared<SceneImageEffect>();
                effect->name            = wpeffobj.name;
                effect->runtime_visible = wpeffobj.visible;
                const auto effect_id    = scene.RegisterEffect(text_node_id, *layer, effect);
                if (! wpeffobj.visible_user.empty()) {
                    effect->visible_user_binding =
                        ToSceneUserVisibilityBinding(wpeffobj.visible_user);
                }

                const std::string   inRT { composite };
                EffectRenderTargets render_targets;
                (void)render_targets.insert(String::make("previous"_str),
                                            String::make(as_str(inRT).unwrap()));

                for (const auto& wpfbo : wpeffobj.fbos) {
                    const std::string rtname = rstd::cppstd::to_string(
                        scene.EffectResourceKey(effect_id, as_str(wpfbo.name).unwrap()).as_str());
                    auto fbo_size = TextEffectFboExtent(initial_geometry, wpfbo.scale, wpfbo.fit);
                    scene.RegisterRenderTarget(String::make(as_str(rtname).unwrap()),
                                               SceneRenderTarget { .width      = fbo_size[usize()],
                                                                   .height     = fbo_size[usize(1)],
                                                                   .allowReuse = ! wpfbo.unique,
                                                                   .clear_on_first_write = true });
                    (void)render_targets.insert(String::make(as_str(wpfbo.name).unwrap()),
                                                String::make(as_str(rtname).unwrap()));
                    runtime_targets->fbos.push_back(TextRuntimeFbo {
                        .name  = rtname,
                        .scale = wpfbo.scale,
                        .fit   = wpfbo.fit,
                    });
                }

                for (const auto& cmd : wpeffobj.commands) {
                    if (cmd.command != "copy") {
                        rstd_error("Unknown effect command: {}", cmd.command);
                        continue;
                    }
                    auto target = render_targets.get(as_str(cmd.target).unwrap());
                    auto source = render_targets.get(as_str(cmd.source).unwrap());
                    if (target.is_none() || source.is_none()) {
                        rstd_error(
                            "Unknown effect command dst or src: {} {}", cmd.target, cmd.source);
                        continue;
                    }
                    auto command_target = cmd.target == "previous"
                                              ? SceneEffectTarget::LayerNext()
                                              : SceneEffectTarget::Named(
                                                    rstd::cppstd::to_string((**target).as_str()));
                    auto command_source = cmd.source == "previous"
                                              ? SceneEffectTarget::LayerPrevious()
                                              : SceneEffectTarget::Named(
                                                    rstd::cppstd::to_string((**source).as_str()));
                    effect->commands.push_back({ .cmd      = SceneImageEffect::CmdType::Copy,
                                                 .dst      = std::move(command_target),
                                                 .src      = std::move(command_source),
                                                 .afterpos = cmd.afterpos });
                }

                bool effect_ok = true;
                for (std::size_t i_mat = 0; i_mat < wpeffobj.materials.size(); ++i_mat) {
                    wpscene::Material         wpmat = wpeffobj.materials.at(i_mat).clone();
                    SceneEffectTarget         matOutRT { SceneEffectTarget::LayerNext() };
                    Option<wpscene::Material> user_texture_fallback;
                    if (wpeffobj.passes.size() > i_mat) {
                        const auto& pass = wpeffobj.passes.at(i_mat);
                        wpmat.MergePass(pass);
                        ApplyTextureBinds(wpmat, std::span(pass.bind), render_targets);
                        user_texture_fallback = Some(wpmat.clone());
                        ApplyUserTextureBindings(context, wpmat);
                        if (! pass.target.empty()) {
                            auto target = render_targets.get(as_str(pass.target).unwrap());
                            if (target.is_none())
                                rstd_error("fbo {} not found", pass.target);
                            else
                                matOutRT = SceneEffectTarget::Named(
                                    rstd::cppstd::to_string((**target).as_str()));
                        }
                    }
                    for (auto& tex : wpmat.textures) {
                        auto composite_id = ParseImageLayerCompositeId(as_str(tex).unwrap());
                        if (composite_id.is_some() && *composite_id == rstd::as_cast<u32>(obj.id)) {
                            tex = composite;
                        }
                    }
                    if (wpmat.textures.empty()) wpmat.textures.resize(1);
                    if (wpmat.textures.at(0).empty()) wpmat.textures[0] = inRT;

                    auto effect_node = Arc<SceneNode>::make();
                    scene.RegisterNode(*effect_node);
                    ShaderInfo shader_info;
                    shader_info.baseConstSvs = effect_base;
                    shader_info.baseConstSvs[rstd::cppstd::to_string(G_ETVP)] =
                        ShaderValue::fromMatrix(Eigen::Matrix4f::Identity());
                    shader_info.baseConstSvs[rstd::cppstd::to_string(G_ETVPI)] =
                        ShaderValue::fromMatrix(Eigen::Matrix4f::Identity());

                    SceneMaterial          mat;
                    UniformNodeConfigDraft sv;
                    sv.SetParallaxContract(obj.parallax, obj.id);
                    sv.effect_projection_node = Some(layer_node.clone());
                    sv.effect_projection_size = { initial_geometry.effect_frame_width,
                                                  initial_geometry.effect_frame_height };
                    SceneShaderValueAnimationMap final_quad_shader_values;
                    auto material_result = BuildMaterial(*context.vfs,
                                                         *context.shader_cache,
                                                         context.shader_environment,
                                                         wpmat,
                                                         scene,
                                                         rstd::move(shader_info));
                    if (material_result.is_err()) {
                        effect_ok = false;
                        break;
                    }
                    auto material_build = rstd::move(material_result).unwrap_unchecked();
                    mat                 = rstd::move(material_build.material);
                    shader_info         = rstd::move(material_build.shader_info);
                    LoadConstvalue(mat, wpmat, shader_info, &final_quad_shader_values);

                    auto mesh = std::make_shared<SceneMesh>();
                    mesh->AddMaterial(std::move(mat));
                    Option<ref<wpscene::Material>> binding_fallback;
                    if (user_texture_fallback.is_some()) {
                        binding_fallback = Some(ref<wpscene::Material>::from_raw_parts(
                            rstd::addressof(*user_texture_fallback)));
                    }
                    RegisterMaterialBindings(
                        scene, mesh->MaterialSlots().front(), wpmat, shader_info, binding_fallback);
                    RegisterLayerPreviousBindings(
                        scene, *mesh->Material(), wpmat, text_node_id, as_str(composite).unwrap());
                    WireMaterialShaderValueScripts(
                        context, layer_node, mesh->MaterialSlots().front(), wpmat, shader_info);
                    effect_node->AddMesh(mesh);
                    SetUniformConfig(context, effect_node, rstd::move(sv));
                    runtime_targets->effect_nodes.push_back(
                        TextRuntimeEffectNode { .node = effect_node.as_ptr() });
                    effect->nodes.push_back(SceneImageEffectNode {
                        .output                   = std::move(matOutRT),
                        .sceneNode                = effect_node.clone(),
                        .uses_unit_final_quad     = UsesUnitFinalQuad(wpmat),
                        .final_quad_shader_values = std::move(final_quad_shader_values),
                    });
                }

                if (effect_ok)
                    layer->AddEffect(effect);
                else
                    rstd_error("effect '{}' failed to load", wpeffobj.name);
            }

            auto resolve_node = Arc<SceneNode>::make();
            auto resolved     = load_passthrough_material(composite);
            if (resolved.is_none()) return;
            auto resolve_mesh = std::make_shared<SceneMesh>();
            resolve_mesh->AddMaterial(std::move(resolved->material));
            RegisterLayerPreviousBindings(scene,
                                          *resolve_mesh->Material(),
                                          resolved->source,
                                          text_node_id,
                                          as_str(composite).unwrap());
            resolve_node->AddMesh(std::move(resolve_mesh));
            SetUniformConfig(context, resolve_node, rstd::move(resolved->sv));
            runtime_targets->effect_nodes.push_back(
                TextRuntimeEffectNode { .node = resolve_node.as_ptr() });
            auto resolve_effect  = std::make_shared<SceneImageEffect>();
            resolve_effect->name = "text_resolve";
            resolve_effect->nodes.push_back(SceneImageEffectNode {
                .output    = SceneEffectTarget::LayerNext(),
                .sceneNode = resolve_node.clone(),
            });
            layer->SetFinalResolveEffect(std::move(resolve_effect));
        }

        if (linked_source) {
            const std::string_view input =
                has_text_effect ? std::string_view(effect_final) : std::string_view(composite);
            auto publish_node = Arc<SceneNode>::make();
            auto published    = load_passthrough_material(input);
            if (published.is_none()) return;
            auto publish_mesh = std::make_shared<SceneMesh>();
            publish_mesh->AddMaterial(std::move(published->material));
            RegisterMaterialBindings(scene,
                                     publish_mesh->MaterialSlots().front(),
                                     published->source,
                                     published->shader_info);
            RegisterLayerPreviousBindings(scene,
                                          *publish_mesh->Material(),
                                          published->source,
                                          text_node_id,
                                          as_str(composite).unwrap());
            publish_node->AddMesh(std::move(publish_mesh));
            scene.RegisterNode(*publish_node);
            SetUniformConfig(context, publish_node, std::move(published->sv));
            runtime_targets->effect_nodes.push_back(
                TextRuntimeEffectNode { .node = publish_node.as_ptr() });

            auto publish  = std::make_shared<SceneImageEffect>();
            publish->name = "linked_text_publish";
            publish->nodes.push_back(SceneImageEffectNode {
                .output    = link_output,
                .sceneNode = publish_node.clone(),
            });
            layer->SetPublishedEffect(std::move(publish));
        }

        auto compose_mesh = std::make_shared<SceneMesh>(/*dynamic=*/wants_dynamic_text);
        GenCardMesh(*compose_mesh,
                    { rstd::as_cast<float>(runtime_targets->layer_w),
                      rstd::as_cast<float>(runtime_targets->layer_h) });
        auto loaded = load_passthrough_material(has_text_effect ? effect_final : composite);
        if (loaded.is_none()) return;
        compose_sv = std::move(loaded->sv);
        compose_sv.SetParallaxContract(obj.parallax, obj.id);
        compose_mesh->AddMaterial(std::move(loaded->material));
        RegisterMaterialBindings(
            scene, compose_mesh->MaterialSlots().front(), loaded->source, loaded->shader_info);
        RegisterLayerPreviousBindings(scene,
                                      *compose_mesh->Material(),
                                      loaded->source,
                                      text_node_id,
                                      as_str(composite).unwrap());
        layer_node->AddMesh(compose_mesh);
        SetUniformConfig(context, layer_node, rstd::move(compose_sv));

        // Move sp_node into layer space — identity transform so the glyph
        // mesh renders at the ortho origin.
        sp_node->CopyTrans(SceneNode());
        sp_node->SetCamera(addr);
        layer->SetSourceDraw(*sp_node);
    }

    auto layer_hold        = SceneNodeArcHold(layer_node.clone());
    auto apply_text_anchor = [layer_hold, anchor_state]() {
        auto* layer_ptr = layer_hold.get();
        auto  contains  = [](const std::string& value, std::string_view token) {
            return value.find(token) != std::string::npos;
        };
        const auto& scale = layer_ptr->Scale();
        Vector3f    pos   = anchor_state->origin;
        if (contains(anchor_state->horizontal, "left"))
            pos.x() += anchor_state->width * scale.x() * 0.5f;
        if (contains(anchor_state->horizontal, "right"))
            pos.x() -= anchor_state->width * scale.x() * 0.5f;
        if (contains(anchor_state->vertical, "top"))
            pos.y() -= anchor_state->height * scale.y() * 0.5f;
        if (contains(anchor_state->vertical, "bottom"))
            pos.y() += anchor_state->height * scale.y() * 0.5f;
        layer_ptr->SetTranslate(pos);
    };

    auto update_text_layout = [layer_hold,
                               anchor_state,
                               apply_text_anchor,
                               runtime_targets,
                               geometry_policy,
                               direct_text,
                               text_padding = style.padding](text::TextLayoutMetrics metrics) {
        auto* layer_ptr      = layer_hold.get();
        metrics.padding      = text_padding;
        anchor_state->width  = metrics.text_width;
        anchor_state->height = metrics.text_height;
        if (direct_text) {
            layer_ptr->SetSize({ metrics.text_width + 2.0f * text_padding,
                                 metrics.text_height + 2.0f * text_padding });
            apply_text_anchor();
            return;
        }
        const auto geometry = text::ResolveTextGeometry(geometry_policy, metrics);
        (void)runtime_targets->Apply(geometry);
        layer_ptr->SetSize({ geometry.draw_width, geometry.draw_height });
        apply_text_anchor();
        const float                  hx = geometry.draw_width * 0.5f;
        const float                  hy = geometry.draw_height * 0.5f;
        const float                  cx = geometry.draw_offset_x;
        const float                  cy = geometry.draw_offset_y;
        const rstd::array<float, 12> pos {
            cx - hx, cy - hy, 0.0f, cx - hx, cy + hy, 0.0f,
            cx + hx, cy - hy, 0.0f, cx + hx, cy + hy, 0.0f,
        };
        const float u_half = 0.5f * std::min(1.0f,
                                             geometry.uv_source_width /
                                                 rstd::as_cast<float>(runtime_targets->layer_w));
        const float v_half = 0.5f * std::min(1.0f,
                                             geometry.uv_source_height /
                                                 rstd::as_cast<float>(runtime_targets->layer_h));
        const float u_l    = 0.5f - u_half;
        const float u_r    = 0.5f + u_half;
        const float v_t    = 0.5f - v_half;
        const float v_b    = 0.5f + v_half;
        const rstd::array<float, 8> uv {
            u_l, v_b, u_l, v_t, u_r, v_b, u_r, v_t,
        };
        auto* mesh = layer_ptr->Mesh();
        if (mesh == nullptr) return;
        auto& v = mesh->GetVertexArray(usize(0));
        v.SetVertex(as_string_view(WE_IN_POSITION), pos.as_slice());
        v.SetVertex(as_string_view(WE_IN_TEXCOORD), uv.as_slice());
        mesh->SetDirty();
    };
    update_text_layout(initial_metrics);

    auto set_text_origin = [anchor_state, apply_text_anchor](Vector3f next) {
        anchor_state->origin = next;
        apply_text_anchor();
    };
    auto apply_text_origin = [anchor_state, set_text_origin](const script::ScriptValue& value) {
        Vector3f current = anchor_state->origin;
        auto     next    = ScriptValueAsVec3(value, current);
        if (! next) return;
        set_text_origin(*next);
    };
    auto apply_text_scale = [layer_hold, apply_text_anchor](const script::ScriptValue& value) {
        auto*    layer_ptr = layer_hold.get();
        Vector3f current   = layer_ptr->Scale();
        auto     next      = ScriptValueAsVec3(value, current);
        if (! next) return;
        layer_ptr->SetScale(*next);
        apply_text_anchor();
    };

    auto set_halign = [layouter, update_text_layout, anchor_state](std::string_view align) {
        anchor_state->horizontal = std::string(align);
        layouter->SetHorizontalAlign(align);
        update_text_layout(layouter->Metrics());
    };
    auto set_valign = [anchor_state, apply_text_anchor](std::string_view align) {
        anchor_state->vertical = std::string(align);
        apply_text_anchor();
    };
    auto set_pointsize = [scene          = context.scene.get(),
                          font_cache_ptr = &font_cache,
                          font_source    = font_source,
                          sp_mesh,
                          layouter,
                          update_text_layout,
                          current_text,
                          current_point_size](double next_point_size) {
        if (scene == nullptr || font_cache_ptr == nullptr || ! std::isfinite(next_point_size) ||
            next_point_size <= 0.0) {
            return;
        }
        auto* next_face = font_cache_ptr->GetFace(
            font_source, TextPointSizeToPx(static_cast<float>(next_point_size)));
        if (next_face == nullptr) return;
        next_face->Populate(text::DecodeUtf8(*current_text));
        if (! EnsureTextAtlas(*scene, *next_face)) return;
        *current_point_size = next_point_size;
        if (auto* mat = sp_mesh->Material()) {
            (void)scene->SetMaterialTextureSlot(*mat, u32(), next_face->AtlasUrl());
        }
        layouter->SetFace(next_face);
        update_text_layout(layouter->Metrics());
    };

    auto& script_runtime = EnsureScriptScene(context).runtime();
    script_runtime.RegisterTextAlignSetters(
        layer_node.as_ptr(),
        anchor_state->horizontal,
        anchor_state->vertical,
        obj.pointsize,
        set_halign,
        set_valign,
        [current_point_size]() {
            return *current_point_size;
        },
        set_pointsize);
    script_runtime.RegisterNodeOriginAccessors(
        layer_node.as_ptr(),
        script::JsRuntime::NodeOriginGetter::make([anchor_state]() {
            const auto& origin = anchor_state->origin;
            return script::Vec3Value { .x = origin.x(), .y = origin.y(), .z = origin.z() };
        }),
        script::JsRuntime::NodeOriginSetter::make(
            [set_text_origin](script::Vec3Value origin) mutable {
                set_text_origin(Vector3f { static_cast<float>(origin.x),
                                           static_cast<float>(origin.y),
                                           static_cast<float>(origin.z) });
            }));

    AssignNodeFieldAnimations(*layer_node.as_ptr(), obj.field_bindings);
    WireFieldScripts(context, layer_node, obj.field_bindings, apply_text_origin, apply_text_scale);
    if (! obj.visible) layer_node->SetVisible(false);
    if (! obj.visible_user.empty())
        layer_node->SetVisibleUserBinding(ToSceneUserVisibilityBinding(obj.visible_user));

    auto set_text = [layouter, update_text_layout, current_text](std::string_view s) {
        if (s == *current_text) return;
        *current_text = std::string(s);
        if (auto* active_face = layouter->Face()) active_face->Populate(text::DecodeUtf8(s));
        layouter->SetText(s);
        update_text_layout(layouter->Metrics());
    };
    if (has_text_user) {
        context.scene->RegisterUserTextBinding(
            String::make(as_str(obj.text_user.name).unwrap()),
            Box<dyn<FnMut<void(ref<str>)>>>::make([set_text](ref<str> value) mutable {
                set_text(as_string_view(value));
            }));
    }
    if (has_text_script) {
        const auto& sb  = text_binding_it->second;
        auto&       ss  = EnsureScriptScene(context);
        std::string sha = utils::genSha1(std::span<const char>(sb.source));
        auto*       fs  = ss.runtime().MakeFieldScript(sb.source,
                                                       sha,
                                                       script::FieldKind::String,
                                                       sb.properties,
                                                       sb.initial_value,
                                                       layer_node.as_ptr());
        if (fs) {
            WireAnimationEventSources(ss.runtime(), *fs, obj.field_bindings, "text");
            SetScriptInitializationOrder(context, *fs, layer_node.as_ptr());
            TrackRegisteredAssets(context, fs);
            ss.AddActuator({
                fs,
                [set_text](const script::ScriptValue& v) {
                    if (auto* p = std::get_if<script::StringValue>(&v)) set_text(p->s);
                },
            });
        }
    }
    if (has_pointsize_script) {
        const auto& sb  = pointsize_binding_it->second;
        auto&       ss  = EnsureScriptScene(context);
        std::string sha = utils::genSha1(std::span<const char>(sb.source));
        auto*       fs  = ss.runtime().MakeFieldScript(sb.source,
                                                       sha,
                                                       script::FieldKind::Scalar,
                                                       sb.properties,
                                                       sb.initial_value,
                                                       layer_node.as_ptr());
        if (fs) {
            SetScriptInitializationOrder(context, *fs, layer_node.as_ptr());
            TrackRegisteredAssets(context, fs);
            ss.AddActuator({
                fs,
                [set_pointsize](const script::ScriptValue& v) {
                    auto scalar = ScriptValueAsFloat(v);
                    if (scalar) set_pointsize(*scalar);
                },
            });
        }
    }
    if (supports_runtime_text_write) {
        EnsureScriptScene(context).runtime().RegisterTextSetter(layer_node.as_ptr(),
                                                                [set_text](std::string_view s) {
                                                                    set_text(s);
                                                                });
    }

    Vec<Arc<SceneNode>> text_before_nodes;
    if (! direct_text) text_before_nodes.push(sp_node.clone());
    RegisterNodeRef(context,
                    obj.id,
                    SceneParseContext::NodeRef {
                        obj.parent,
                        Some(layer_node.clone()),
                        None(),
                        String::make(rstd::cppstd::as_str(obj.attachment).unwrap()),
                        None(),
                        Some(Box<dyn<FnMut<void(Vector3f)>>>::make(
                            [anchor_state, apply_text_anchor](Vector3f offset) {
                                anchor_state->origin += offset;
                                apply_text_anchor();
                            })),
                        rstd::move(text_before_nodes),
                    });

    std::string_view scripted_tag = has_text_script            ? " [scripted]"
                                    : has_indirect_text_script ? " [scripted-indirect]"
                                                               : "";
    rstd_info("text '{}': initial=\"{}\" px={} peak_quads={} bbox={}x{}{} ({})",
              obj.name,
              s_text,
              px,
              peak_quads,
              static_cast<int>(text_w),
              static_cast<int>(text_h),
              scripted_tag,
              resolved.source);
}

void ParseTextObj(SceneParseContext& context, wpscene::TextObject& text) {
    ParseTextObjImpl(context, text);
}

} // namespace owe
