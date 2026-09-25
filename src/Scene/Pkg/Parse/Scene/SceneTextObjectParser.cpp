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
import wescene.utils;
import wescene.scene;
import wescene.text;
import wescene.script;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::path::PathBuf;
using rstd::sync::Arc;
using namespace owe;
using namespace Eigen;

namespace owe
{

array<i32, 2> TextLayerExtent(const text::TextGeometry& geometry) {
    return {
        rstd::cmp::max(rstd::as_cast<i32>(f32(geometry.rt_width).ceil().to_primitive()), i32(1)),
        rstd::cmp::max(rstd::as_cast<i32>(f32(geometry.rt_height).ceil().to_primitive()), i32(1)),
    };
}

rstd::uint32_t TextPointSizeToPx(float point_size) {
    constexpr float kPointsizeToPx = 4.0f;
    if (! f32(point_size).is_finite() || point_size <= 0.0f) return 1;
    auto px = static_cast<rstd::uint32_t>(f32(point_size * kPointsizeToPx).round().to_primitive());
    return rstd::cmp::min<rstd::uint32_t>(1024, rstd::cmp::max<rstd::uint32_t>(1, px));
}

array<i32, 2> TextEffectFboExtent(const text::TextGeometry& geometry, u32 scale, u32 fit) {
    if (fit > u32()) {
        const float max_size =
            rstd::cmp::max(geometry.effect_frame_height, geometry.effect_frame_width);
        if (max_size > 0.0f) {
            const float fit_scale = rstd::as_cast<float>(fit) / max_size;
            return {
                rstd::cmp::max(
                    rstd::as_cast<i32>(
                        f32(geometry.effect_frame_width * fit_scale).round().to_primitive()),
                    i32(1)),
                rstd::cmp::max(
                    rstd::as_cast<i32>(
                        f32(geometry.effect_frame_height * fit_scale).round().to_primitive()),
                    i32(1)),
            };
        }
    }
    const float fbo_scale = rstd::cmp::max(rstd::as_cast<float>(scale), 1.0f);
    return {
        rstd::cmp::max(
            rstd::as_cast<i32>(f32(geometry.effect_frame_width / fbo_scale).round().to_primitive()),
            i32(1)),
        rstd::cmp::max(rstd::as_cast<i32>(
                           f32(geometry.effect_frame_height / fbo_scale).round().to_primitive()),
                       i32(1)),
    };
}

struct TextRuntimeFbo {
    String name;
    u32    scale { 1 };
    u32    fit {};
};

struct TextRuntimeEffectNode {
    SceneNode*                                   node { nullptr };
    Option<Arc<text::TextEffectProjectionState>> text_projection;
};

struct TextRuntimeTargets {
    TextRuntimeTargets(Scene& scene_owner, mut_ref<UniformSceneState> state)
        : scene(&scene_owner), uniform_state(state) {}

    Scene*                     scene;
    mut_ref<UniformSceneState> uniform_state;
    String                     camera_key;
    String                     composite;
    String                     effect_final;
    bool                       has_effect { false };
    i32                        layer_w { 1 };
    i32                        layer_h { 1 };
    Vec<TextRuntimeFbo>        fbos;
    Vec<TextRuntimeEffectNode> effect_nodes;

    bool Apply(const text::TextGeometry& geometry) {
        if (scene == nullptr) return false;

        bool changed          = false;
        auto [next_w, next_h] = TextLayerExtent(geometry);
        changed |= scene->ResizeRenderTarget(composite.as_str(), next_w, next_h);
        if (has_effect) {
            changed |= scene->ResizeRenderTarget(effect_final.as_str(), next_w, next_h);
        }

        auto camera = scene->CameraMut(camera_key.as_str());
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
            changed |= scene->ResizeRenderTarget(fbo.name, w, h);
        }

        const array<float, 2> effect_size {
            geometry.effect_frame_width,
            geometry.effect_frame_height,
        };
        for (auto& item : effect_nodes) {
            if (item.text_projection) {
                (*item.text_projection)->size = effect_size;
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
    auto atlas_url = face.AtlasUrl();
    if (scene.Texture(atlas_url).is_some()) return true;
    auto atlas_image = text::BuildAtlasImage(face, atlas_url);
    if (atlas_image.is_none()) return false;
    auto         image = rstd::move(atlas_image).unwrap_unchecked();
    SceneTexture stex;
    stex.url    = rstd::into(atlas_url);
    stex.sample = image->header.sample;
    scene.RegisterTexture(String::make(atlas_url), rstd::move(stex));
    scene.RegisterRuntimeImage(String::make(atlas_url), rstd::move(image));
    face.ClearDirtyRects();
    return true;
}

auto UserPropertyValue(Option<ref<rstd::json::Map>> user_props, ref<str> key) -> Option<ref<Json>> {
    if (key->is_empty()) return None();
    auto        props   = rstd_try(user_props);
    auto        value   = rstd_try(props->get(key));
    const auto& payload = SceneUserPropertyPayload(*value);
    return Some(ref<Json>::from_raw_parts(rstd::addressof(payload)));
}

void ParseTextObjImpl(SceneParseContext& context, wpscene::TextObject& obj) {
    if (! obj.visible) {
        context.scene->MarkLayerVisibilityElidable(WallpaperLayerId { .value = obj.id });
    }
    MarkHiddenLinkSource(context, obj.id);

    // --- determine initial text + whether a runtime binding will rewrite it
    auto text_binding      = obj.field_bindings.Get("text"_str);
    bool has_text_script   = text_binding.is_some() && (**text_binding).script.is_some();
    auto pointsize_binding = obj.field_bindings.Get("pointsize"_str);
    bool has_pointsize_script =
        pointsize_binding.is_some() && (**pointsize_binding).script.is_some();
    // Scripts can also drive `text` indirectly: a script attached to any
    // other field (commonly `visible`) writes `thisLayer.text = "..."` from
    // its update() side-effect (e.g. workshop 2283810443's clock). Transform
    // scripts alone should not force large dynamic text RTs.
    bool has_indirect_text_script = false;
    if (! has_text_script) {
        for (const auto& binding : obj.field_bindings.Entries()) {
            if (binding.script.is_none()) continue;
            const auto& source = binding.script->source;
            if (source.as_str().contains(".text"_str) ||
                source.as_str().contains("[\"text\"]"_str) ||
                source.as_str().contains("['text']"_str)) {
                has_indirect_text_script = true;
                break;
            }
        }
    }
    const bool has_text_user = ! obj.text_user.empty();
    bool wants_dynamic_text = has_text_script || has_indirect_text_script || has_pointsize_script ||
                              has_text_user || context.scene_layer_text_writes;
    bool has_text_effect    = ! obj.effects.is_empty();
    const bool linked_source        = context.IsLinkedSource(obj.id);
    const auto text_render_mode     = ResolveTextRenderMode(TextSurfaceRequirements {
        .color_blend       = obj.colorBlendMode != i32(),
        .has_effect        = has_text_effect,
        .copy_background   = obj.copybackground,
        .opaque_background = obj.opaquebackground,
        .linked_source     = linked_source,
    });
    const bool direct_text          = text_render_mode == TextRenderMode::Direct;
    const bool copy_background_seed = has_text_effect || obj.copybackground;

    String s_text;
    if (obj.text.is_string()) {
        s_text = String::make(*obj.text.as_str());
    } else if (obj.text.is_object()) {
        auto value = obj.text.get("value"_str);
        if (value.is_none()) value = obj.text.get("text"_str);
        if (value.is_some()) {
            auto string = (*value)->as_str();
            if (string.is_some()) s_text = String::make(*string);
        }
    }
    if (has_text_user) {
        auto value = UserPropertyValue(context.user_properties, obj.text_user.name.as_str());
        if (value.is_some()) {
            auto text = SceneJsonScalarString(**value);
            if (text.is_some()) s_text = String::make(text->as_str());
        }
    }
    if (s_text.is_empty() && ! wants_dynamic_text) {
        // Empty text layers can still be transform parents for authored child layers.
        auto node  = Arc<SceneNode>::make(Vector3f(obj.origin.data()),
                                          Vector3f(obj.scale.data()),
                                          Vector3f(obj.angles.data()),
                                          obj.name.as_str());
        node->ID() = obj.id;
        node->SetSize({ obj.size[usize(0)], obj.size[usize(1)] });
        node->SetReflected(obj.reflected);
        AssignNodeFieldAnimations(context, *node.as_ptr(), obj.field_bindings);
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
                            obj.attachment.clone(),
                        });
        return;
    }

    // --- font resolution: VFS first (WE shared /assets + pkg overlay),
    //     then host system font dirs.
    String font_name;
    if (obj.font.is_string()) {
        font_name = String::make(*obj.font.as_str());
    } else if (obj.font.is_object()) {
        if (auto value = obj.font.get("value"_str); value.is_some()) {
            auto string = (*value)->as_str();
            if (string.is_some()) font_name = String::make(*string);
        }
    }

    text::FontCache::ResolvedBlob resolved;
    // `systemfont_<family>` is WE's alias for a host system font — never exists
    // in the pkg, so skip the VFS round-trip and let fontconfig resolve it.
    // Some scenes write it with a leading dir (e.g. `fonts/systemfont_arial`),
    // so match on the basename.
    const bool is_systemfont = [&] {
        auto file = fs::Path(font_name.as_str()).file_name();
        return file.is_some() && file->to_str().unwrap().starts_with("systemfont_"_str);
    }();
    String font_source_key;
    if (! font_name.is_empty() && ! is_systemfont) {
        // scene.json's `font` is a pkg-relative path, e.g. `fonts/2.ttf` or
        // `fonts/workshop/<id>/X.otf`. The pkg mounts at /assets so the full
        // VFS path is /assets/<font_name>.
        auto joined     = PathBuf::from("/assets"_str).join(font_name.as_str());
        auto normalized = joined.as_path().normalize_lexically();
        if (normalized.is_ok()) {
            auto vfs_path   = String::make(normalized->as_path().to_str().unwrap());
            font_source_key = vfs_path.clone();
            auto key        = font_source_key.as_str();
            if (auto cached = context.font_sources.get(key); cached.is_some()) {
                resolved = (**cached).clone();
            } else {
                auto blob = fs::ReadFileBytes(*context.vfs, fs::Path(vfs_path));
                if (blob.is_ok() && ! blob->is_empty()) {
                    resolved.bytes  = Some(Arc<Vec<u8>>::make(rstd::move(blob).unwrap_unchecked()));
                    resolved.source = vfs_path.clone();
                }
            }
        }
    }
    if (! resolved.bytes) {
        font_source_key = rstd::format("system:{}", font_name);
        auto key        = font_source_key.as_str();
        if (auto cached = context.font_sources.get(key); cached.is_some()) {
            resolved = (**cached).clone();
        } else {
            resolved = text::FontCache::ResolveSystemFont(font_name,
                                                          /*fallback_to_any=*/true);
        }
    }
    if (! resolved.bytes) {
        rstd_error("text '{}': could not resolve font '{}'", obj.name, font_name);
        return;
    }

    if (font_source_key.is_empty()) font_source_key = String::make(resolved.source.as_str());
    auto source_key = font_source_key.as_str();
    if (! context.font_sources.contains_key(source_key)) {
        (void)context.font_sources.insert(String::make(source_key), resolved.clone());
    }
    const auto& font_source = **context.font_sources.get(source_key);

    rstd::uint32_t px = TextPointSizeToPx(obj.pointsize);

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
        copy_background_seed ? text::GetTextCopyBackgroundSceneShader() : None();
    if (copy_background_seed && ! copy_background_shader) {
        rstd_error("text '{}': copy-background shader compile failed", obj.name);
        return;
    }

    // Populate the seed text's glyphs up front so the first SetText has the
    // initial layout's bbox. Runtime SetText calls (from the script actuator)
    // do their own Populate of the latest string each tick.
    {
        auto seed = text::DecodeUtf8(s_text.as_str().as_bytes());
        face->Populate(seed.as_slice());
    }

    // --- atlas-texture registration. We snapshot the per-face CPU atlas
    // (seed glyphs + the white cell) and register it with the Scene.
    // TextureCache::CreateTex will pick this up on first material bind.
    // Subsequent glyph adds emit dirty rects which the renderer re-uploads
    // each frame via TextureCache::PumpFontAtlases.
    auto atlas_url = face->AtlasUrl();
    if (! EnsureTextAtlas(*context.scene, *face)) {
        rstd_error("text '{}': atlas snapshot failed", obj.name);
        return;
    }

    // --- mesh capacity. Static text exactly fits its initial layout;
    //     dynamic text reserves headroom so SetText can grow
    //     the string at runtime without reallocating GPU buffers.
    rstd::size_t initial_codepoints =
        text::DecodeUtf8(s_text.as_str().as_bytes()).len().to_primitive();
    bool         has_bg = obj.opaquebackground;
    rstd::size_t peak_quads;
    if (wants_dynamic_text) {
        // The glyph mesh renders into the layer RT (sized below to the same
        // ceiling), so the only quads that can ever be visible are those that
        // fit the RT grid. Budget to that cap — terminal/log scripts (e.g.
        // 2268178377) append unbounded text but the layouter clips everything
        // past the RT anyway. Conservative narrow-glyph advance avoids
        // undercounting columns for tight fonts.
        const auto& fm  = face->Metrics();
        const float adv = rstd::cmp::max(static_cast<float>(fm.pixel_size) * 0.25f, 1.0f);
        const float lh = fm.line_height > 1.0f ? fm.line_height : static_cast<float>(fm.pixel_size);
        const float obj_w       = obj.size[usize(0)] > 0.0f ? obj.size[usize(0)] : 1024.0f;
        const float obj_h       = obj.size[usize(1)] > 0.0f ? obj.size[usize(1)] : 256.0f;
        const float rt_w        = rstd::cmp::max(obj_w * 3.0f, 1024.0f);
        const float rt_h        = rstd::cmp::max(obj_h * 2.0f, 256.0f);
        const rstd::size_t cols = static_cast<rstd::size_t>(f32(rt_w / adv).ceil().to_primitive());
        const rstd::size_t rows =
            static_cast<rstd::size_t>(f32(rt_h / rstd::cmp::max(lh, 1.0f)).ceil().to_primitive());
        const rstd::size_t rt_cap =
            rstd::cmp::min<rstd::size_t>(16384, rstd::cmp::max<rstd::size_t>(64, cols * rows));
        peak_quads = rstd::cmp::max<rstd::size_t>(rt_cap, initial_codepoints * 4);
        if (has_bg) ++peak_quads;
    } else {
        peak_quads = initial_codepoints + (has_bg ? 1u : 0u);
        if (peak_quads == 0) return;
    }

    const bool supports_runtime_text_write = wants_dynamic_text || context.scene_has_scripts;
    auto       sp_mesh = Arc<SceneMesh>::make(/*dynamic=*/supports_runtime_text_write);
    {
        SceneVertexArray vertex(MakeAttrSet({ VAttr::Position, VAttr::TexCoord, VAttr::Color }),
                                usize(peak_quads * 4));
        sp_mesh->AddVertexArray(rstd::move(vertex));
        sp_mesh->AddIndexArray(SceneIndexArray(usize(peak_quads * 6)));
    }
    {
        SceneMaterial material;
        material.name = "text"_Str;
        material.textures.push(rstd::into(atlas_url));
        material.defines = Vec<String>::make();
        material.defines.push("g_Texture0"_Str);
        material.SetBlendMode(direct_text || copy_background_seed ? BlendMode::Translucent
                                                                  : BlendMode::Normal);
        material.customShader.shader = shader.clone();
        sp_mesh->AddMaterial(rstd::move(material));
    }

    // --- layouter owns the cache (FontFace lifetime) + mesh ref + style.
    text::TextLayoutStyle style;
    style.mesh_origin =
        direct_text ? text::TextMeshOrigin::Layout : text::TextMeshOrigin::InkBounds;
    style.color                 = { obj.color[usize(0)], obj.color[usize(1)], obj.color[usize(2)] };
    style.alpha                 = obj.alpha;
    style.brightness            = obj.brightness;
    style.opaquebackground      = has_bg;
    style.background_color      = { obj.backgroundcolor[usize(0)],
                                    obj.backgroundcolor[usize(1)],
                                    obj.backgroundcolor[usize(2)] };
    style.background_brightness = obj.backgroundbrightness;
    style.halign  = (obj.horizontalalign.is_empty() ? obj.alignment : obj.horizontalalign).clone();
    style.padding = rstd::as_cast<float>(obj.padding);
    if (obj.limitwidth && obj.maxwidth > 0.0f) style.wrap_width = obj.maxwidth;
    if (obj.limitrows && obj.maxrows > u32()) style.max_rows = obj.maxrows.to_primitive();
    style.row_limit_ellipsis = obj.limituseellipsis;

    auto align_or_default =
        [](ref<str> value, ref<str> fallback, ref<str> negative, ref<str> positive) -> ref<str> {
        if (! value->is_empty()) return value;
        if (fallback->contains(negative)) return negative;
        if (fallback->contains(positive)) return positive;
        return "center"_str;
    };
    const auto initial_halign = align_or_default(
        obj.horizontalalign.as_str(), obj.alignment.as_str(), "left"_str, "right"_str);
    const auto initial_valign = align_or_default(
        obj.verticalalign.as_str(), obj.alignment.as_str(), "top"_str, "bottom"_str);
    style.halign = rstd::into(initial_halign);

    const float text_padding = style.padding;
    const float wrap_width   = style.wrap_width;
    auto        layouter =
        Arc<text::TextLayouter>::make(face, sp_mesh.clone(), rstd::move(style), peak_quads);
    layouter->SetText(s_text);
    auto current_text       = Arc<String>::make(s_text.clone());
    auto current_point_size = Arc<double>::make(obj.pointsize);

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
                                                   direct_text ? obj.name.as_str() : ""_str);
    const float text_bbox_w = text_w + 2.0f * text_padding;
    const float text_bbox_h = text_h + 2.0f * text_padding;
    sp_node->SetSize({ text_bbox_w, text_bbox_h });
    sp_node->AddMesh(sp_mesh.clone());

    struct TextAnchorState {
        String horizontal;
        String vertical;
        float  width { 1.0f };
        float  height { 1.0f };
    };
    auto anchor_state = Arc<TextAnchorState>::make(TextAnchorState {
        .horizontal = rstd::into(initial_halign),
        .vertical   = rstd::into(initial_valign),
        .width      = text_w,
        .height     = text_h,
    });

    auto layer_node =
        direct_text ? sp_node.clone()
                    : Arc<SceneNode>::make(
                          Vector3f::Zero(), Vector3f::Ones(), Vector3f::Zero(), obj.name.as_str());
    layer_node->SetReflected(obj.reflected);
    layer_node->ID() = obj.id;

    // A layer that limits its width lays its text out inside a maxwidth box.
    // The authored `size` only measures the text the layer shipped with, which
    // on a scripted layer is a placeholder ("..." in workshop 3425253832), so
    // on its own it clips every longer message the script writes.
    const float limit_w = wrap_width > 0.0f ? wrap_width + 2.0f * text_padding : 0.0f;
    const float object_w =
        rstd::cmp::max(limit_w, obj.size[usize(0)] > 0.0f ? obj.size[usize(0)] : text_bbox_w);
    const float object_h = obj.size[usize(1)] > 0.0f ? obj.size[usize(1)] : text_bbox_h;
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
    Option<Arc<TextRuntimeTargets>> runtime_targets;
    String                          link_output;
    if (direct_text) {
        UniformNodeConfigDraft text_sv;
        text_sv.SetParallaxContract(obj.parallax, obj.id);
        SetUniformConfig(context, sp_node, rstd::move(text_sv));
    } else {
        context.text_uniform_configs.push(SceneParseContext::TextUniformConfigDraft {
            .node = sp_node.clone(),
        });
        runtime_targets         = Some(Arc<TextRuntimeTargets>::make(
            scene, mut_ref<UniformSceneState>::from_raw_parts(context.uniform_state.as_ptr())));
        const auto addr         = scene.NodeResourceKey(text_node_id, "text_camera"_str);
        const auto composite    = scene.NodeResourceKey(text_node_id, "text_composite"_str);
        const auto effect_final = scene.NodeResourceKey(text_node_id, "text_effect_final"_str);
        (*runtime_targets)->camera_key   = addr.clone();
        (*runtime_targets)->composite    = composite.clone();
        (*runtime_targets)->effect_final = effect_final.clone();
        (*runtime_targets)->has_effect   = has_text_effect;
        (*runtime_targets)->layer_w      = initial_layer_w;
        (*runtime_targets)->layer_h      = initial_layer_h;

        // Per-layer ortho camera. effect_camera_node sits at origin so the
        // view matrix is identity; ortho extents = bbox so glyph pixel
        // coords (centered around 0) map directly to [-1, +1] NDC.
        auto text_camera = Arc<SceneCamera>::make(
            SceneCamera::MakeOrthographic(rstd::as_cast<double>(initial_layer_w),
                                          rstd::as_cast<double>(initial_layer_h),
                                          -1.0,
                                          1.0));
        text_camera->AttatchNode(sp_node.as_ptr());
        scene.RegisterCamera(String::make(addr), text_camera.clone());

        SceneRenderTarget text_target {
            .width                = initial_layer_w,
            .height               = initial_layer_h,
            .allowReuse           = true,
            .force_clear          = ! copy_background_seed,
            .clear_on_first_write = false,
            .preserve_on_write    = copy_background_seed,
        };
        if (has_text_effect) {
            scene.RegisterRenderTarget(String::make(composite), text_target.clone());
            scene.RegisterRenderTarget(String::make(effect_final), rstd::move(text_target));
        } else {
            scene.RegisterRenderTarget(String::make(composite), rstd::move(text_target));
        }

        layer_node->CopyTrans(*sp_node.as_ptr());
        layer_node->SetSize({ initial_geometry.draw_width, initial_geometry.draw_height });
        if (linked_source) {
            const auto link_source = WallpaperLayerId { .value = obj.id };
            const auto link_width =
                rstd::cmp::max(i32(1), rstd::as_cast<i32>(f32(object_w).ceil().to_primitive()));
            const auto link_height =
                rstd::cmp::max(i32(1), rstd::as_cast<i32>(f32(object_h).ceil().to_primitive()));
            scene.RegisterLayerLinkSource(
                link_source, *sp_node.as_ptr(), { link_width, link_height });
            link_output = scene.EnsureLinkRenderTarget(link_source, *sp_node.as_ptr());
        }

        auto layer =
            Arc<SceneNodeLayer>::make(has_text_effect ? layer_node.as_ptr() : sp_node.as_ptr(),
                                      rstd::as_cast<float>(initial_layer_w),
                                      rstd::as_cast<float>(initial_layer_h),
                                      composite);
        sp_node->AttachLayer(layer.clone());

        if (copy_background_seed) {
            auto bg_node = Arc<SceneNode>::make();
            bg_node->SetCamera("effect"_str);
            auto bg_mesh = Arc<SceneMesh>::make();
            bg_mesh->ChangeMeshDataFrom(*scene.DefaultEffectMesh());
            SceneMaterial bg_material;
            bg_material.name = "text_copybackground"_Str;
            bg_material.textures.push(rstd::into(SpecTex_Default));
            bg_material.defines = Vec<String>::make();
            bg_material.defines.push("g_Texture0"_Str);
            bg_material.SetBlendMode(BlendMode::Normal);
            bg_material.customShader.shader = copy_background_shader.clone();
            bg_mesh->AddMaterial(rstd::move(bg_material));
            bg_node->AddMesh(bg_mesh.clone());

            auto text_projection =
                Arc<text::TextEffectProjectionState>::make(text::TextEffectProjectionState {
                    .node = layer_node.clone(),
                    .size = { initial_geometry.effect_frame_width,
                              initial_geometry.effect_frame_height },
                });
            context.text_uniform_configs.push(SceneParseContext::TextUniformConfigDraft {
                .node              = bg_node.clone(),
                .effect_projection = Some(text_projection.clone()),
            });
            (*runtime_targets)
                ->effect_nodes.push(
                    TextRuntimeEffectNode { .node            = bg_node.as_ptr(),
                                            .text_projection = Some(rstd::move(text_projection)) });
            layer->AddPrefillNode(SceneImageEffectNode {
                .output    = SceneEffectTarget::LayerNext(),
                .sceneNode = bg_node.clone(),
            });
        }

        UniformNodeConfigDraft compose_sv;
        compose_sv.SetParallaxContract(obj.parallax, obj.id);

        ShaderValueMap effect_base = NeutralColorUniforms(context.global_base_uniforms.clone());

        struct LoadedTextMaterial {
            wpscene::Material      source;
            SceneMaterial          material;
            UniformNodeConfigDraft sv;
            ShaderInfo             shader_info;
        };
        auto load_passthrough_material =
            [&](ref<str> input, bool final_composite = false) -> Option<LoadedTextMaterial> {
            auto pt_json =
                LoadJsonFile(*context.vfs, "/assets/materials/util/effectpassthrough.json"_str);
            if (! pt_json) {
                rstd_error("text '{}': parse effectpassthrough.json failed", obj.name);
                return None();
            }
            wpscene::Material pt_mat;
            if (! pt_mat.FromJson(*pt_json)) {
                rstd_error("text '{}': Material::FromJson failed", obj.name);
                return None();
            }
            if (pt_mat.textures.is_empty())
                pt_mat.textures.push(rstd::into(input));
            else
                pt_mat.textures[usize()] = rstd::into(input);

            auto attachment_override =
                ApplyLayerColorBlend(pt_mat, final_composite ? obj.colorBlendMode : i32());
            SceneMaterial          mat;
            UniformNodeConfigDraft sv;
            ShaderInfo             si;
            si.baseConstSvs      = effect_base.clone();
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
            LoadConstvalue(context, mat, pt_mat, si);
            mat.SetBlendMode(attachment_override.unwrap_or(BlendMode::Translucent));
            return Some(LoadedTextMaterial {
                .source      = rstd::move(pt_mat),
                .material    = rstd::move(mat),
                .sv          = rstd::move(sv),
                .shader_info = rstd::move(si),
            });
        };

        if (has_text_effect) {
            SceneMaterial final_state;
            final_state.SetBlendMode(BlendMode::Normal);
            final_state.SetDepthTest(false);
            final_state.SetDepthWrite(false);
            layer->SetFullscreen(true);
            layer->SetFinalTarget(effect_final);
            layer->SetFinalMaterialState(final_state);

            for (const auto& wpeffobj : obj.effects) {
                auto effect             = Arc<SceneImageEffect>::make();
                effect->name            = rstd::into(wpeffobj.name.as_str());
                effect->runtime_visible = wpeffobj.visible;
                const auto effect_id = scene.RegisterEffect(text_node_id, *layer, effect.clone());
                if (! wpeffobj.visible_user.empty()) {
                    effect->visible_user_binding =
                        ToSceneUserVisibilityBinding(wpeffobj.visible_user);
                }
                WireImageEffectVisibilityScript(context, layer_node.as_ptr(), wpeffobj, effect_id);

                const auto          inRT = composite.as_str();
                EffectRenderTargets render_targets;
                (void)render_targets.insert("previous"_Str, String::make(inRT));

                for (const auto& wpfbo : wpeffobj.fbos) {
                    const auto rtname = scene.EffectResourceKey(effect_id, wpfbo.name.as_str());
                    auto fbo_size = TextEffectFboExtent(initial_geometry, wpfbo.scale, wpfbo.fit);
                    scene.RegisterRenderTarget(String::make(rtname),
                                               SceneRenderTarget { .width      = fbo_size[usize()],
                                                                   .height     = fbo_size[usize(1)],
                                                                   .allowReuse = ! wpfbo.unique,
                                                                   .clear_on_first_write = true });
                    (void)render_targets.insert(String::make(wpfbo.name.as_str()),
                                                String::make(rtname));
                    (*runtime_targets)
                        ->fbos.push(TextRuntimeFbo {
                            .name  = rtname.clone(),
                            .scale = wpfbo.scale,
                            .fit   = wpfbo.fit,
                        });
                }

                for (const auto& cmd : wpeffobj.commands) {
                    if (cmd.command != "copy"_str) {
                        rstd_error("Unknown effect command: {}", cmd.command);
                        continue;
                    }
                    auto target = render_targets.get(cmd.target.as_str());
                    auto source = render_targets.get(cmd.source.as_str());
                    if (target.is_none() || source.is_none()) {
                        rstd_error(
                            "Unknown effect command dst or src: {} {}", cmd.target, cmd.source);
                        continue;
                    }
                    auto command_target = cmd.target == "previous"_str
                                              ? SceneEffectTarget::LayerNext()
                                              : SceneEffectTarget::Named((**target).as_str());
                    auto command_source = cmd.source == "previous"_str
                                              ? SceneEffectTarget::LayerPrevious()
                                              : SceneEffectTarget::Named((**source).as_str());
                    effect->commands.push({ .cmd      = SceneImageEffect::CmdType::Copy,
                                            .dst      = rstd::move(command_target),
                                            .src      = rstd::move(command_source),
                                            .afterpos = cmd.afterpos });
                }

                bool effect_ok = true;
                for (usize i_mat {}; i_mat < wpeffobj.materials.len(); ++i_mat) {
                    wpscene::Material         wpmat = wpeffobj.materials.at(i_mat).clone();
                    SceneEffectTarget         matOutRT { SceneEffectTarget::LayerNext() };
                    Option<wpscene::Material> user_texture_fallback;
                    if (wpeffobj.passes.len() > i_mat) {
                        const auto& pass = wpeffobj.passes.at(i_mat);
                        wpmat.MergePass(pass);
                        ApplyTextureBinds(wpmat, pass.bind.as_slice(), render_targets);
                        user_texture_fallback = Some(wpmat.clone());
                        ApplyUserTextureBindings(context, wpmat);
                        if (! pass.target.is_empty()) {
                            auto target = render_targets.get(pass.target.as_str());
                            if (target.is_none())
                                rstd_error("fbo {} not found", pass.target);
                            else
                                matOutRT = SceneEffectTarget::Named((**target).as_str());
                        }
                    }
                    for (auto& tex : wpmat.textures) {
                        auto composite_id = ParseImageLayerCompositeId(tex.as_str());
                        if (composite_id.is_some() && *composite_id == rstd::as_cast<u32>(obj.id)) {
                            tex = composite.clone();
                        }
                    }
                    if (wpmat.textures.is_empty()) wpmat.textures.resize(usize(1), String {});
                    if (wpmat.textures[usize()].is_empty())
                        wpmat.textures[usize(0)] = rstd::into(inRT);

                    auto effect_node = Arc<SceneNode>::make();
                    scene.RegisterNode(*effect_node);
                    ShaderInfo shader_info;
                    shader_info.baseConstSvs = effect_base.clone();
                    (void)shader_info.baseConstSvs.insert(
                        rstd::into(G_ETVP),
                        ShaderValue(ShaderValue::fromMatrix(Eigen::Matrix4f::Identity())));
                    (void)shader_info.baseConstSvs.insert(
                        rstd::into(G_ETVPI),
                        ShaderValue(ShaderValue::fromMatrix(Eigen::Matrix4f::Identity())));

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
                    LoadConstvalue(context, mat, wpmat, shader_info, &final_quad_shader_values);

                    auto mesh = Arc<SceneMesh>::make();
                    mesh->AddMaterial(rstd::move(mat));
                    Option<ref<wpscene::Material>> binding_fallback;
                    if (user_texture_fallback.is_some()) {
                        binding_fallback = Some(ref<wpscene::Material>::from_raw_parts(
                            rstd::addressof(*user_texture_fallback)));
                    }
                    RegisterMaterialBindings(scene,
                                             mesh->MaterialSlots().first_mut().unwrap().get_mut(),
                                             wpmat,
                                             shader_info,
                                             binding_fallback);
                    RegisterLayerPreviousBindings(
                        scene, *mesh->Material(), wpmat, text_node_id, composite);
                    WireMaterialShaderValueScripts(
                        context,
                        layer_node,
                        mesh->MaterialSlots().first_mut().unwrap().get_mut(),
                        wpmat,
                        shader_info);
                    effect_node->AddMesh(mesh.clone());
                    SetUniformConfig(context, effect_node, rstd::move(sv));
                    (*runtime_targets)
                        ->effect_nodes.push(TextRuntimeEffectNode { .node = effect_node.as_ptr() });
                    effect->AddNode(SceneImageEffectNode {
                        .output                   = rstd::move(matOutRT),
                        .sceneNode                = effect_node.clone(),
                        .uses_unit_final_quad     = UsesUnitFinalQuad(wpmat),
                        .final_quad_shader_values = rstd::move(final_quad_shader_values),
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
            auto resolve_mesh = Arc<SceneMesh>::make();
            resolve_mesh->AddMaterial(rstd::move(resolved->material));
            RegisterLayerPreviousBindings(
                scene, *resolve_mesh->Material(), resolved->source, text_node_id, composite);
            resolve_node->AddMesh(rstd::move(resolve_mesh));
            SetUniformConfig(context, resolve_node, rstd::move(resolved->sv));
            (*runtime_targets)
                ->effect_nodes.push(TextRuntimeEffectNode { .node = resolve_node.as_ptr() });
            auto resolve_effect  = Arc<SceneImageEffect>::make();
            resolve_effect->name = "text_resolve"_Str;
            resolve_effect->AddNode(SceneImageEffectNode {
                .output    = SceneEffectTarget::LayerNext(),
                .sceneNode = resolve_node.clone(),
            });
            layer->SetFinalResolveEffect(rstd::move(resolve_effect));
        }

        if (linked_source) {
            const ref<str> input = has_text_effect ? effect_final.as_str() : composite.as_str();
            auto           publish_node = Arc<SceneNode>::make();
            auto           published    = load_passthrough_material(input);
            if (published.is_none()) return;
            auto publish_mesh = Arc<SceneMesh>::make();
            publish_mesh->AddMaterial(rstd::move(published->material));
            RegisterMaterialBindings(scene,
                                     publish_mesh->MaterialSlots().first_mut().unwrap().get_mut(),
                                     published->source,
                                     published->shader_info);
            RegisterLayerPreviousBindings(
                scene, *publish_mesh->Material(), published->source, text_node_id, composite);
            publish_node->AddMesh(rstd::move(publish_mesh));
            scene.RegisterNode(*publish_node);
            SetUniformConfig(context, publish_node, rstd::move(published->sv));
            (*runtime_targets)
                ->effect_nodes.push(TextRuntimeEffectNode { .node = publish_node.as_ptr() });

            auto publish  = Arc<SceneImageEffect>::make();
            publish->name = "linked_text_publish"_Str;
            publish->AddNode(SceneImageEffectNode {
                .output    = SceneEffectTarget::Named(link_output.as_str()),
                .sceneNode = publish_node.clone(),
            });
            layer->SetPublishedEffect(rstd::move(publish));
        }

        auto compose_mesh = Arc<SceneMesh>::make(/*dynamic=*/wants_dynamic_text);
        GenCardMesh(*compose_mesh,
                    { rstd::as_cast<float>((*runtime_targets)->layer_w),
                      rstd::as_cast<float>((*runtime_targets)->layer_h) });
        auto loaded = load_passthrough_material(has_text_effect ? effect_final : composite, true);
        if (loaded.is_none()) return;
        compose_sv = rstd::move(loaded->sv);
        compose_sv.SetParallaxContract(obj.parallax, obj.id);
        compose_mesh->AddMaterial(rstd::move(loaded->material));
        RegisterMaterialBindings(scene,
                                 compose_mesh->MaterialSlots().first_mut().unwrap().get_mut(),
                                 loaded->source,
                                 loaded->shader_info);
        RegisterLayerPreviousBindings(
            scene, *compose_mesh->Material(), loaded->source, text_node_id, composite);
        layer_node->AddMesh(compose_mesh.clone());
        SetUniformConfig(context, layer_node, rstd::move(compose_sv));

        // Move sp_node into layer space — identity transform so the glyph
        // mesh renders at the ortho origin.
        sp_node->CopyTrans(SceneNode());
        sp_node->SetCamera(addr);
        layer->SetSourceDraw(*sp_node);
    }

    auto layer_hold        = layer_node.clone();
    auto apply_text_anchor = Arc<dyn<Fn<void()>>>::make(
        [layer_hold = layer_hold.clone(), anchor_state = anchor_state.clone()]() {
            auto*    layer_ptr = layer_hold.as_ptr().as_raw_ptr();
            Vector3d offset    = Vector3d::Zero();
            if (anchor_state->horizontal.as_str().contains("left"_str))
                offset.x() += anchor_state->width * 0.5;
            if (anchor_state->horizontal.as_str().contains("right"_str))
                offset.x() -= anchor_state->width * 0.5;
            if (anchor_state->vertical.as_str().contains("top"_str))
                offset.y() -= anchor_state->height * 0.5;
            if (anchor_state->vertical.as_str().contains("bottom"_str))
                offset.y() += anchor_state->height * 0.5;
            // Alignment moves the text geometry, not the frame inherited by children.
            layer_ptr->SetGeometryTransform(Eigen::Affine3d(Eigen::Translation3d(offset)).matrix());
        });

    auto update_text_layout = Arc<dyn<Fn<void(text::TextLayoutMetrics)>>>::make(
        [layer_hold        = layer_hold.clone(),
         anchor_state      = anchor_state.clone(),
         apply_text_anchor = apply_text_anchor.clone(),
         runtime_targets   = runtime_targets.clone(),
         geometry_policy,
         direct_text,
         text_padding](text::TextLayoutMetrics metrics) {
            auto* layer_ptr      = layer_hold.as_ptr().as_raw_ptr();
            metrics.padding      = text_padding;
            anchor_state->width  = metrics.text_width;
            anchor_state->height = metrics.text_height;
            if (direct_text) {
                layer_ptr->SetSize({ metrics.text_width + 2.0f * text_padding,
                                     metrics.text_height + 2.0f * text_padding });
                apply_text_anchor->operator()();
                return;
            }
            const auto geometry = text::ResolveTextGeometry(geometry_policy, metrics);
            (void)(*runtime_targets)->Apply(geometry);
            layer_ptr->SetSize({ geometry.draw_width, geometry.draw_height });
            apply_text_anchor->operator()();
            const float            hx = geometry.draw_width * 0.5f;
            const float            hy = geometry.draw_height * 0.5f;
            const float            cx = geometry.draw_offset_x;
            const float            cy = geometry.draw_offset_y;
            const array<float, 12> pos {
                cx - hx, cy - hy, 0.0f, cx - hx, cy + hy, 0.0f,
                cx + hx, cy - hy, 0.0f, cx + hx, cy + hy, 0.0f,
            };
            const float u_half =
                0.5f * rstd::cmp::min(geometry.uv_source_width /
                                          rstd::as_cast<float>((*runtime_targets)->layer_w),
                                      1.0f);
            const float v_half =
                0.5f * rstd::cmp::min(geometry.uv_source_height /
                                          rstd::as_cast<float>((*runtime_targets)->layer_h),
                                      1.0f);
            const float           u_l = 0.5f - u_half;
            const float           u_r = 0.5f + u_half;
            const float           v_t = 0.5f - v_half;
            const float           v_b = 0.5f + v_half;
            const array<float, 8> uv {
                u_l, v_b, u_l, v_t, u_r, v_b, u_r, v_t,
            };
            auto* mesh = layer_ptr->Mesh();
            if (mesh == nullptr) return;
            auto& v = mesh->GetVertexArray(usize(0));
            v.SetVertex(WE_IN_POSITION, pos.as_slice());
            v.SetVertex(WE_IN_TEXCOORD, uv.as_slice());
            mesh->SetDirty();
        });
    update_text_layout->operator()(initial_metrics);

    auto set_halign =
        Arc<dyn<Fn<void(ref<str>)>>>::make([layouter           = layouter.clone(),
                                            update_text_layout = update_text_layout.clone(),
                                            anchor_state = anchor_state.clone()](ref<str> align) {
            anchor_state->horizontal = rstd::into(align);
            layouter->SetHorizontalAlign(align);
            update_text_layout->operator()(layouter->Metrics());
        });
    auto set_valign = Arc<dyn<Fn<void(ref<str>)>>>::make(
        [anchor_state      = anchor_state.clone(),
         apply_text_anchor = apply_text_anchor.clone()](ref<str> align) {
            anchor_state->vertical = rstd::into(align);
            apply_text_anchor->operator()();
        });
    auto set_pointsize = script::JsRuntime::PointSizeSetter::make(
        [scene              = context.scene.get(),
         font_cache_ptr     = &font_cache,
         font_source        = font_source.clone(),
         layouter           = layouter.clone(),
         update_text_layout = update_text_layout.clone(),
         current_text       = current_text.clone(),
         current_point_size = current_point_size.clone()](double next_point_size) {
            if (scene == nullptr || font_cache_ptr == nullptr ||
                ! f64(next_point_size).is_finite() || next_point_size <= 0.0) {
                return;
            }
            auto* next_face = font_cache_ptr->GetFace(
                font_source, TextPointSizeToPx(static_cast<float>(next_point_size)));
            if (next_face == nullptr) return;
            next_face->Populate(text::DecodeUtf8(current_text->as_str().as_bytes()).as_slice());
            if (! EnsureTextAtlas(*scene, *next_face)) return;
            *current_point_size = next_point_size;
            if (auto* mat = layouter->Mesh().Material()) {
                (void)scene->SetMaterialTextureSlot(*mat, u32(), next_face->AtlasUrl());
            }
            layouter->SetFace(next_face);
            update_text_layout->operator()(layouter->Metrics());
        });

    auto& script_runtime = EnsureScriptScene(context).runtime();
    script_runtime.RegisterTextAlignSetters(
        layer_node.as_ptr(),
        rstd::into(anchor_state->horizontal.as_str()),
        rstd::into(anchor_state->vertical.as_str()),
        obj.pointsize,
        script::JsRuntime::TextSetter::make([set_halign = set_halign.clone()](ref<str> value) {
            set_halign->operator()(value);
        }),
        script::JsRuntime::TextSetter::make([set_valign = set_valign.clone()](ref<str> value) {
            set_valign->operator()(value);
        }),
        Some(script::JsRuntime::PointSizeGetter::make(
            [current_point_size = current_point_size.clone()]() {
                return *current_point_size;
            })),
        Some(set_pointsize.clone()));
    AssignNodeFieldAnimations(context, *layer_node.as_ptr(), obj.field_bindings);
    WireFieldScripts(context, layer_node, obj.field_bindings);
    if (! obj.visible) layer_node->SetVisible(false);
    if (! obj.visible_user.empty())
        layer_node->SetVisibleUserBinding(ToSceneUserVisibilityBinding(obj.visible_user));

    auto set_text =
        Arc<dyn<Fn<void(ref<str>)>>>::make([layouter           = layouter.clone(),
                                            update_text_layout = update_text_layout.clone(),
                                            current_text       = current_text.clone()](ref<str> s) {
            if (s == current_text->as_str()) return;
            *current_text = rstd::into(s);
            if (auto* active_face = layouter->Face())
                active_face->Populate(text::DecodeUtf8(s.as_bytes()).as_slice());
            layouter->SetText(s);
            update_text_layout->operator()(layouter->Metrics());
        });
    if (has_text_user) {
        context.scene->RegisterUserTextBinding(
            obj.text_user.name.clone(),
            Box<dyn<FnMut<void(ref<str>)>>>::make(
                [set_text = set_text.clone()](ref<str> value) mutable {
                    set_text->operator()(value);
                }));
    }
    if (has_text_script) {
        const auto& binding = **text_binding;
        const auto& sb      = *binding.script;
        auto&       ss      = EnsureScriptScene(context);
        auto        sha     = utils::genSha1(rstd::as_bytes(sb.source.as_str().as_bytes()));
        auto*       fs      = ss.runtime().MakeFieldScript(
            sb.source.as_str(),
            sha.as_str(),
            script::FieldKind::String,
            binding.ScriptProperties(),
            sb.initial_value,
            script::ScriptBindingContext::ForLayer(
                layer_node.as_ptr(), "text"_str, layer_node->FieldAnimation("text"_str)));
        if (fs) {
            SetScriptInitializationOrder(context, *fs, layer_node.as_ptr());
            TrackRegisteredAssets(context, fs);
            ss.AddActuator({
                fs,
                [set_text = set_text.clone()](const script::ScriptValue& v) {
                    if (auto* p = (v.is_String() ? &v.as_String().value : nullptr))
                        set_text->operator()(p->s.as_str());
                },
            });
        }
    }
    if (has_pointsize_script) {
        const auto& binding = **pointsize_binding;
        const auto& sb      = *binding.script;
        auto&       ss      = EnsureScriptScene(context);
        auto        sha     = utils::genSha1(rstd::as_bytes(sb.source.as_str().as_bytes()));
        auto*       fs      = ss.runtime().MakeFieldScript(
            sb.source.as_str(),
            sha.as_str(),
            script::FieldKind::Scalar,
            binding.ScriptProperties(),
            sb.initial_value,
            script::ScriptBindingContext::ForLayer(
                layer_node.as_ptr(), "pointsize"_str, layer_node->FieldAnimation("pointsize"_str)));
        if (fs) {
            SetScriptInitializationOrder(context, *fs, layer_node.as_ptr());
            TrackRegisteredAssets(context, fs);
            ss.AddActuator({
                fs,
                [set_pointsize = set_pointsize.clone()](const script::ScriptValue& v) {
                    auto scalar = ScriptValueAsFloat(v);
                    if (scalar) set_pointsize->operator()(*scalar);
                },
            });
        }
    }
    if (supports_runtime_text_write) {
        EnsureScriptScene(context).runtime().RegisterTextSetter(
            layer_node.as_ptr(),
            script::JsRuntime::TextSetter::make([set_text = set_text.clone()](ref<str> s) {
                set_text->operator()(s);
            }));
    }

    Vec<Arc<SceneNode>> text_before_nodes;
    if (! direct_text) text_before_nodes.push(sp_node.clone());
    RegisterNodeRef(context,
                    obj.id,
                    SceneParseContext::NodeRef {
                        obj.parent,
                        Some(layer_node.clone()),
                        None(),
                        obj.attachment.clone(),
                        None(),
                        None(),
                        rstd::move(text_before_nodes),
                    });

    ref<str> scripted_tag = has_text_script            ? " [scripted]"_str
                            : has_indirect_text_script ? " [scripted-indirect]"_str
                                                       : ""_str;
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
    PrepareAnimationBindings(context, text);
    ParseTextObjImpl(context, text);
}

} // namespace owe
