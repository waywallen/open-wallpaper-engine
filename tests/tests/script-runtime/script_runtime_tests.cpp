#include <rstd/test/gtest.hpp>

import rstd.cppstd;
import rstd;
import eigen;
import wescene.json;
import wescene.pkg.parse;
import wescene.types;
import wescene.scene;
import wescene.script;
import wescene.testing.json_builder;

using namespace owe::script;
using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

namespace
{

// Build a one-shot FieldScript whose update() returns a host-visible counter.
// The module body schedules timers/intervals that mutate that counter, so we
// can observe the JsRuntime's deferred-callback sweep through FieldScript's
// last_value().
FieldScript* MakeProbe(JsRuntime& rt, const char* sha, const char* src) {
    return rt.MakeFieldScript(rstd::cppstd::as_str(src).unwrap(),
                              rstd::cppstd::as_str(sha).unwrap(),
                              FieldKind::Scalar,
                              /*properties_config=*/owe::MakeObject(),
                              /*initial_value=*/owe::IntoJson(0),
                              /*node=*/nullptr);
}

double Tick(JsRuntime& rt, double runtime) {
    FrameInputs fi {};
    fi.runtime = float(runtime);
    rt.SetFrameInputs(fi);
    rt.TickAll();
    // last_value() of the last MakeFieldScript'd script — caller must own a
    // pointer, but for this test the call site walks via ForEach.
    return 0.0;
}

double LastScalar(FieldScript* fs) {
    EXPECT_TRUE(fs->last_value().is_Scalar());
    if (! fs->last_value().is_Scalar()) return 0.0;
    return fs->last_value().as_Scalar().value.v;
}

struct ParticleControlState {
    std::array<float, 3> colorn { 1.0f, 1.0f, 1.0f };
    bool                 playing { true };
    int                  resets { 0 };
};

struct ParticleControlProbe {
    Arc<ParticleControlState> state;

    Vec<float> Get(ref<str> field) const {
        Vec<float> out;
        if (field != "colorn"_str) return out;
        for (float value : state->colorn) out.push(float(value));
        return out;
    }
    void Apply(ref<str> field, slice<float> values) {
        if (field != "colorn"_str || values.len() < usize(3)) return;
        state->colorn = { values[usize()], values[usize(1)], values[usize(2)] };
    }
    void Play() {
        state->playing = true;
        state->resets++;
    }
    void Stop() {
        state->playing = false;
        state->resets++;
    }
    void Pause() { state->playing = false; }
    bool IsPlaying() const { return state->playing; }
    void Emit(u32) const {}
};

struct SoundControlState {
    float volume { 1.0f };
    bool  playing { true };
};

struct SoundControlProbe {
    Arc<SoundControlState> state;

    void Play() { state->playing = true; }
    void Stop() { state->playing = false; }
    void Pause() { state->playing = false; }
    bool IsPlaying() const { return state->playing; }
    void SetVolume(float volume) { state->volume = volume; }
};

} // namespace

TEST(ScriptInitialization, UsesSceneOwnerOrderInsteadOfRegistrationOrder) {
    auto root = Arc<owe::SceneNode>::make();

    JsRuntime rt;
    auto*     consumer = rt.MakeFieldScript(
        R"JS(
            let seen = -1;
            export function init() { seen = shared.ready; }
            export function update() { return seen; }
        )JS"_str,
        "test/init_order_consumer"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0));
    auto* producer = rt.MakeFieldScript(
        R"JS(
            export function init() { shared.ready = 7; }
            export function update() { return 0; }
        )JS"_str,
        "test/init_order_producer"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0));
    ASSERT_NE(consumer, nullptr);
    ASSERT_NE(producer, nullptr);

    rt.SetInitializationOrder(*consumer, 1);
    rt.SetInitializationOrder(*producer, 0);
    rt.SetSceneRoot(root.as_ptr());
    rt.TickAll();

    EXPECT_EQ(LastScalar(consumer), 7.0);
}

TEST(ScriptValueCoercion, PreservesVec4InitialAndReturnValues) {
    JsRuntime rt;
    auto*     script = rt.MakeFieldScript(
        R"JS(
            export function update(value) { return value.add(new Vec4(1, 2, 3, 4)); }
        )JS"_str,
        "test/vec4_value"_str,
        FieldKind::Vec4,
        owe::MakeObject(),
        owe::IntoJson("0.5 1.5 2.5 3.5"));
    ASSERT_NE(script, nullptr);

    rt.TickAll();

    ASSERT_TRUE(script->last_value().is_Vec4());
    const auto& value = script->last_value().as_Vec4().value;
    EXPECT_DOUBLE_EQ(value.x, 1.5);
    EXPECT_DOUBLE_EQ(value.y, 3.5);
    EXPECT_DOUBLE_EQ(value.z, 5.5);
    EXPECT_DOUBLE_EQ(value.w, 7.5);
}

TEST(ScriptValueCoercion, ParsesDecimalPrefixesWithoutCStringCopies) {
    struct Case {
        ref<str>  text;
        Vec4Value expected;
    };
    const Case cases[] = {
        { " \t\r\n\v\f+.5 -2e1 3. 4E-1"_str, { .x = .5, .y = -20, .z = 3, .w = .4 } },
        { "1-2+.3.4"_str, { .x = 1, .y = -2, .z = .3, .w = .4 } },
        { "1 2oops 3"_str, { .x = 1, .y = 2, .z = 1, .w = 1 } },
        { "1 2e+ 3"_str, { .x = 1, .y = 2, .z = 1, .w = 1 } },
        { "1,2 3"_str, { .x = 1, .y = 1, .z = 1, .w = 1 } },
        { "1\0 2"_str, { .x = 1, .y = 1, .z = 1, .w = 1 } },
        { "1 2e9999 3"_str, { .x = 1, .y = 1, .z = 1, .w = 1 } },
        { "1 2e-9999 3"_str, { .x = 1, .y = 0, .z = 3, .w = 1 } },
        { "0x1p2 3"_str, {} },
        { "garbage"_str, {} },
        { ""_str, {} },
    };
    for (const auto& test : cases) {
        JsRuntime rt;
        auto*     script = rt.MakeFieldScript("export function update(value) { return value; }"_str,
                                              "test/decimal_prefix"_str,
                                              FieldKind::Vec4,
                                              owe::MakeObject(),
                                              owe::Json::String(String::make(test.text)));
        ASSERT_NE(script, nullptr);
        rt.TickAll();
        ASSERT_TRUE(script->last_value().is_Vec4());
        const auto& value = script->last_value().as_Vec4().value;
        EXPECT_DOUBLE_EQ(value.x, test.expected.x);
        EXPECT_DOUBLE_EQ(value.y, test.expected.y);
        EXPECT_DOUBLE_EQ(value.z, test.expected.z);
        EXPECT_DOUBLE_EQ(value.w, test.expected.w);
    }
}

TEST(ScriptValueCoercion, PreservesPartialVectorAndColorShapes) {
    JsRuntime  rt;
    const auto source = "export function update(value) { return value; }"_str;
    auto*      vec2   = rt.MakeFieldScript(
        source, "test/prefix_vec2"_str, FieldKind::Vec2, owe::MakeObject(), owe::IntoJson("2bad"));
    auto* vec3 = rt.MakeFieldScript(
        source, "test/prefix_vec3"_str, FieldKind::Vec3, owe::MakeObject(), owe::IntoJson("2bad"));
    auto* color = rt.MakeFieldScript(source,
                                     "test/prefix_color"_str,
                                     FieldKind::Color,
                                     owe::MakeObject(),
                                     owe::IntoJson(".1 .2 .3bad .4"));
    ASSERT_NE(vec2, nullptr);
    ASSERT_NE(vec3, nullptr);
    ASSERT_NE(color, nullptr);
    rt.TickAll();
    ASSERT_TRUE(vec2->last_value().is_Vec2());
    EXPECT_DOUBLE_EQ(vec2->last_value().as_Vec2().value.x, 2);
    EXPECT_DOUBLE_EQ(vec2->last_value().as_Vec2().value.y, 0);
    ASSERT_TRUE(vec3->last_value().is_Vec3());
    EXPECT_DOUBLE_EQ(vec3->last_value().as_Vec3().value.x, 2);
    EXPECT_DOUBLE_EQ(vec3->last_value().as_Vec3().value.y, 2);
    EXPECT_DOUBLE_EQ(vec3->last_value().as_Vec3().value.z, 2);
    ASSERT_TRUE(color->last_value().is_Color());
    EXPECT_DOUBLE_EQ(color->last_value().as_Color().value.r, .1);
    EXPECT_DOUBLE_EQ(color->last_value().as_Color().value.g, .2);
    EXPECT_DOUBLE_EQ(color->last_value().as_Color().value.b, .3);
}

TEST(ScriptValueCoercion, AcceptsNativeNonFiniteSpellings) {
    JsRuntime rt;
    auto*     script = rt.MakeFieldScript(
        R"JS(export function update(value) {
            return new Vec4(value.x === Infinity ? 1 : 0,
                            value.y === -Infinity ? 1 : 0,
                            Number.isNaN(value.z) ? 1 : 0, value.w);
        })JS"_str,
        "test/nonfinite_prefix"_str,
        FieldKind::Vec4,
        owe::MakeObject(),
        owe::IntoJson("+inf -infinity NaN 4"));
    ASSERT_NE(script, nullptr);
    rt.TickAll();
    ASSERT_TRUE(script->last_value().is_Vec4());
    const auto& value = script->last_value().as_Vec4().value;
    EXPECT_DOUBLE_EQ(value.x, 1);
    EXPECT_DOUBLE_EQ(value.y, 1);
    EXPECT_DOUBLE_EQ(value.z, 1);
    EXPECT_DOUBLE_EQ(value.w, 4);
}

TEST(ScriptTimer, SetTimeoutFiresAfterDelay) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = MakeProbe(rt,
                         "test/timer_fires",
                         R"JS(
        let fired = 0;
        setTimeout(() => { fired++; }, 100);
        export function update() { return fired; }
    )JS");
    ASSERT_NE(fs, nullptr);

    Tick(rt, 0.05);
    ASSERT_TRUE(fs->last_value().is_Scalar());
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 0.0);

    Tick(rt, 0.15);
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 1.0);

    Tick(rt, 0.30);
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 1.0);
}

TEST(ScriptTimer, SetIntervalRepeats) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = MakeProbe(rt,
                         "test/interval_repeats",
                         R"JS(
        let n = 0;
        setInterval(() => { n++; }, 100);
        export function update() { return n; }
    )JS");
    ASSERT_NE(fs, nullptr);

    Tick(rt, 0.25);
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 2.0);

    Tick(rt, 0.55);
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 5.0);
}

TEST(ScriptTimer, ClearTimeoutCancels) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = MakeProbe(rt,
                         "test/clear_cancels",
                         R"JS(
        let n = 0;
        let h = setTimeout(() => { n++; }, 100);
        clearTimeout(h);
        export function update() { return n; }
    )JS");
    ASSERT_NE(fs, nullptr);

    Tick(rt, 0.50);
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 0.0);
}

TEST(ScriptTimer, HandleSelfCallCancels) {
    // Corpus also calls the return value as a function to cancel (e.g.
    // `if (stopTimeout) stopTimeout()`). Both shapes must work.
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = MakeProbe(rt,
                         "test/handle_self_call",
                         R"JS(
        let n = 0;
        let h = setTimeout(() => { n++; }, 100);
        h();  // cancel by invoking handle
        export function update() { return n; }
    )JS");
    ASSERT_NE(fs, nullptr);

    Tick(rt, 0.50);
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 0.0);
}

TEST(ScriptCompat, RegExpLegacyCapturesSurviveTimerCallbacks) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = MakeProbe(rt,
                         "test/regexp_legacy_captures",
                         R"JS(
        let n = 0;
        setInterval(() => {
            if (/(H+)/.test('HH:mm')) n = RegExp.$1.length;
        }, 100);
        export function update() { return n; }
    )JS");
    ASSERT_NE(fs, nullptr);

    Tick(rt, 0.15);
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 2.0);
}

TEST(ScriptAudio, RegisterAudioBuffersUsesRequestedResolution) {
    JsRuntime   rt;
    FrameInputs fi {};
    for (std::size_t i = 0; i < 16; ++i) {
        fi.audio.bands16.left.data()[i]    = static_cast<float>(i);
        fi.audio.bands16.right.data()[i]   = static_cast<float>(200 + i);
        fi.audio.bands16.average.data()[i] = static_cast<float>(100 + i);
    }
    rt.SetFrameInputs(fi);

    auto* fs = MakeProbe(rt,
                         "test/audio_buffers_resample",
                         R"JS(
        let audio = engine.registerAudioBuffers(16);
        export function update() {
            return audio.left.length * 100000000
                + audio.right.length * 1000000
                + audio.average[15] * 10000
                + audio.left[15] * 100
                + audio.right[15];
        }
    )JS");
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_DOUBLE_EQ(LastScalar(fs),
                     16.0 * 100000000.0 + 16.0 * 1000000.0 + 115.0 * 10000.0 + 15.0 * 100.0 +
                         215.0);

    for (std::size_t i = 0; i < 16; ++i) {
        fi.audio.bands16.left.data()[i]    = static_cast<float>(100 + i);
        fi.audio.bands16.right.data()[i]   = static_cast<float>(300 + i);
        fi.audio.bands16.average.data()[i] = static_cast<float>(200 + i);
    }
    rt.SetFrameInputs(fi);
    rt.TickAll();
    EXPECT_DOUBLE_EQ(LastScalar(fs),
                     16.0 * 100000000.0 + 16.0 * 1000000.0 + 215.0 * 10000.0 + 115.0 * 100.0 +
                         315.0);
}

TEST(ScriptAudio, RegisterAudioBuffersAcceptsResolution64Constant) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);

    auto* fs = MakeProbe(rt,
                         "test/audio_buffers_resolution_64_constant",
                         R"JS(
        const audio = engine.registerAudioBuffers(engine.AUDIO_RESOLUTION_64);
        export function update() { return audio.average.length; }
    )JS");
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_DOUBLE_EQ(LastScalar(fs), 64.0);
}

TEST(ScriptAudio, RegisterAudioBuffersKeepsMixedResolutionsIndependent) {
    FrameInputs fi {};
    for (std::size_t i = 0; i < 64; ++i) {
        fi.audio.bands64.average.data()[i] = static_cast<float>(100 + i);
    }
    for (std::size_t i = 0; i < 16; ++i) {
        fi.audio.bands16.average.data()[i] = static_cast<float>(100 + i * 4 + 3);
    }

    {
        JsRuntime rt;
        rt.SetFrameInputs(fi);
        auto* low  = MakeProbe(rt,
                               "test/audio_buffers_mixed_low_first",
                               R"JS(
            const audio = engine.registerAudioBuffers(16);
            export function update() { return audio.average.length * 1000 + audio.average[15]; }
        )JS");
        auto* full = MakeProbe(rt,
                               "test/audio_buffers_mixed_full_second",
                               R"JS(
            const audio = engine.registerAudioBuffers(64);
            export function update() { return audio.average.length * 1000 + audio.average[63]; }
        )JS");
        ASSERT_NE(low, nullptr);
        ASSERT_NE(full, nullptr);

        rt.TickAll();
        EXPECT_DOUBLE_EQ(LastScalar(low), 16163.0);
        EXPECT_DOUBLE_EQ(LastScalar(full), 64163.0);

        for (std::size_t i = 0; i < 64; ++i) {
            fi.audio.bands64.average.data()[i] = static_cast<float>(200 + i);
        }
        for (std::size_t i = 0; i < 16; ++i) {
            fi.audio.bands16.average.data()[i] = static_cast<float>(200 + i * 4 + 3);
        }
        rt.SetFrameInputs(fi);
        rt.TickAll();
        EXPECT_DOUBLE_EQ(LastScalar(low), 16263.0);
        EXPECT_DOUBLE_EQ(LastScalar(full), 64263.0);
    }

    {
        JsRuntime rt;
        rt.SetFrameInputs(fi);
        auto* full = MakeProbe(rt,
                               "test/audio_buffers_mixed_full_first",
                               R"JS(
            const audio = engine.registerAudioBuffers(64);
            export function update() { return audio.average.length; }
        )JS");
        auto* low  = MakeProbe(rt,
                               "test/audio_buffers_mixed_low_second",
                               R"JS(
            const audio = engine.registerAudioBuffers(16);
            export function update() { return audio.average.length; }
        )JS");
        ASSERT_NE(full, nullptr);
        ASSERT_NE(low, nullptr);

        rt.TickAll();
        EXPECT_DOUBLE_EQ(LastScalar(full), 64.0);
        EXPECT_DOUBLE_EQ(LastScalar(low), 16.0);
    }
}

TEST(ScriptAudio, RegisterAudioBuffersHoldsOneRuntimeDemandLease) {
    auto              demand = rstd::sync::Arc<owe::AudioResponseDemand>::make();
    std::vector<bool> changes;
    demand->SetCallback([&changes](bool active) {
        changes.push_back(active);
    });
    {
        JsRuntime rt;
        rt.SetAudioResponseDemand(rstd::Some(demand.clone()));
        auto* fs = MakeProbe(rt,
                             "test/audio_demand",
                             R"JS(
            engine.registerAudioBuffers(64);
            engine.registerAudioBuffers(32);
            export function update() { return 1; }
        )JS");
        ASSERT_NE(fs, nullptr);
        EXPECT_TRUE(demand->Active());
    }
    EXPECT_FALSE(demand->Active());
    EXPECT_EQ(changes, (std::vector<bool> { false, true, false }));
}

// ---------------------------------------------------------------------------
// SceneNode wrapper surface

TEST(ScriptNodeSize, ParserSetSizeFlowsToScript) {
    owe::SceneNode node;
    node.SetSize({ 320.0f, 240.0f });

    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update() { return thisLayer.size.x + thisLayer.size.y * 1000; }
        )JS"_str,
        "test/node_size_real"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 320.0 + 240.0 * 1000);
}

TEST(ScriptNodeParent, CursorCallbackParentChainTerminatesAtUnparentedNode) {
    auto root  = Arc<owe::SceneNode>::make();
    auto child = Arc<owe::SceneNode>::make();
    root->AppendChild(child.clone());
    child->SetTranslate({ 500.0f, 500.0f, 0.0f });
    child->SetSize({ 200.0f, 200.0f });

    JsRuntime   rt;
    FrameInputs fi {};
    fi.canvas_w               = 1920.0f;
    fi.canvas_h               = 1080.0f;
    fi.cursor_in_window       = true;
    fi.cursor_x               = 500.0f / fi.canvas_w;
    fi.cursor_y               = 1.0f - 500.0f / fi.canvas_h;
    fi.mouse_buttons_released = 1u << 0;
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let result = 0;
            export function cursorUp() {
                let layer = thisLayer;
                let depth = 0;
                while (typeof layer !== 'undefined' && layer != null && depth < 8) {
                    depth++;
                    layer = layer.getParent();
                }
                result = typeof layer === 'undefined' ? depth : -depth;
            }
            export function update() { return result; }
        )JS"_str,
        "test/parent_chain_terminates"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        child.as_ptr());
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(LastScalar(fs), 2.0);
}

TEST(ScriptNodeParent, DefaultLayerParentIsUndefined) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update() {
                return typeof thisLayer.getParent() === 'undefined' ? 1 : 0;
            }
        )JS"_str,
        "test/default_layer_parent"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0));
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(LastScalar(fs), 1.0);
}

TEST(ScriptNodeSoftMutation, VisibleAndAlphaWrites) {
    owe::SceneNode node;
    JsRuntime      rt;
    FrameInputs    fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            // Toggle alpha and visibility from script.
            thisLayer.alpha = 0.25;
            thisLayer.visible = false;
            export function update() {}
        )JS"_str,
        "test/visible_alpha_writes"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    EXPECT_TRUE(node.IsAlphaOverridden());
    EXPECT_EQ(node.UserAlpha(), 0.25f);
    EXPECT_FALSE(node.Visible());
    EXPECT_EQ(node.EffectiveAlpha(), 0.0f); // hidden wins
}

TEST(ScriptNodeSoftMutation, VisibleWritesUseSceneVisibilityOwner) {
    owe::Scene scene;
    auto       node = Arc<owe::SceneNode>::make();
    node->ID()      = rstd::i32(17);
    scene.RegisterNode(*node, Some(owe::WallpaperLayerId { .value = rstd::i32(17) }));
    scene.RootMut()->AppendChild(node.clone());

    JsRuntime rt;
    rt.SetScene(&scene);
    auto* script = rt.MakeFieldScript(
        R"JS(
            export function init() { thisLayer.visible = false; }
            export function update() { return thisLayer.visible ? 1 : 0; }
        )JS"_str,
        "test/scene_owned_visibility"_str,
        FieldKind::Bool,
        owe::MakeObject(),
        owe::IntoJson(true),
        node.as_ptr());
    ASSERT_NE(script, nullptr);

    rt.SetSceneRoot(scene.RootMut().as_raw_ptr());
    rt.TickAll();

    EXPECT_FALSE(node->Visible());
    EXPECT_TRUE(scene.IsLayerVisibilityElidable(owe::WallpaperLayerId { .value = rstd::i32(17) }));
    EXPECT_TRUE(scene.ConsumeRenderGraphDirty());
}

TEST(ScriptNodeSoftMutation, VisibleTrueRestoresUserAlpha) {
    owe::SceneNode node;
    JsRuntime      rt;
    FrameInputs    fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            thisLayer.alpha = 0.4;
            thisLayer.visible = false;
            thisLayer.visible = true;
            export function update() {}
        )JS"_str,
        "test/visible_restore"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    EXPECT_TRUE(node.Visible());
    EXPECT_FLOAT_EQ(node.EffectiveAlpha(), 0.4f);
}

TEST(ScriptNodeSoftMutation, PerspectiveWritesNodeFlag) {
    owe::SceneNode node;
    JsRuntime      rt;
    FrameInputs    fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            thisLayer.perspective = true;
            export function update() { return thisLayer.perspective ? 1 : 0; }
        )JS"_str,
        "test/perspective_write"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    EXPECT_TRUE(node.Perspective());
    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 1.0);
}

TEST(ScriptNodeSoftMutation, ImageAlignmentDispatchesRegisteredSetter) {
    owe::SceneNode  node;
    JsRuntime       rt;
    String          alignment;
    owe::SceneNode* target { nullptr };
    rt.RegisterImageAlignmentSetter(
        &node,
        "center"_str,
        JsRuntime::ImageAlignmentSetter::make([&](owe::SceneNode* node, ref<str> value) {
            target    = node;
            alignment = String::make(value);
        }));

    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            thisLayer.alignment = 'bottom';
            export function update() { return thisLayer.alignment === 'bottom' ? 1 : 0; }
        )JS"_str,
        "test/image_alignment_write"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    EXPECT_EQ(alignment.as_str(), "bottom"_str);
    EXPECT_EQ(target, &node);
    rt.TickAll();
    EXPECT_EQ(LastScalar(fs), 1.0);
}

TEST(ScriptNodeSoftMutation, ParallaxDepthDispatchesRegisteredAccessors) {
    owe::SceneNode  node;
    JsRuntime       rt;
    Vec2Value       depth { .x = 1.0, .y = 1.0 };
    owe::SceneNode* target { nullptr };
    rt.SetNodeParallaxDepthAccessors(
        JsRuntime::NodeParallaxDepthGetter::make([&depth](owe::SceneNode*) -> Option<Vec2Value> {
            return Some(depth);
        }),
        JsRuntime::NodeParallaxDepthSetter::make(
            [&depth, &target](owe::SceneNode* node, Vec2Value value) {
                target = node;
                depth  = value;
            }));

    auto* fs = rt.MakeFieldScript(
        R"JS(
            thisLayer.parallaxDepth = new Vec2(0.25, 0.75);
            export function update() {
                return thisLayer.parallaxDepth.x + thisLayer.parallaxDepth.y;
            }
        )JS"_str,
        "test/parallax_depth_write"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    EXPECT_EQ(target, &node);
    EXPECT_DOUBLE_EQ(depth.x, 0.25);
    EXPECT_DOUBLE_EQ(depth.y, 0.75);
    rt.TickAll();
    EXPECT_EQ(LastScalar(fs), 1.0);
}

TEST(ScriptNodeSoftMutation, RuntimeLayersKeepIndependentPendingParallaxDepth) {
    auto state  = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    auto first  = Arc<owe::SceneNode>::make();
    auto writer = Arc<owe::SceneNode>::make();
    auto second = Arc<owe::SceneNode>::make();
    state->RegisterNodeParallaxContract(
        *first, i32(-1), owe::wpscene::ParallaxDepthBinding { { 1.0f, 1.0f }, true });
    state->RegisterNodeParallaxContract(
        *writer, i32(-1), owe::wpscene::ParallaxDepthBinding { { 1.0f, 1.0f }, true });
    state->RegisterNodeParallaxContract(
        *second, i32(-2), owe::wpscene::ParallaxDepthBinding { { 1.0f, 1.0f }, true });
    EXPECT_TRUE(state->SetNodeParallaxDepth(*first, { 0.0f, 0.0f }));

    auto first_depth  = state->NodeParallaxDepth(*first);
    auto writer_depth = state->NodeParallaxDepth(*writer);
    auto second_depth = state->NodeParallaxDepth(*second);
    ASSERT_TRUE(first_depth.is_some());
    ASSERT_TRUE(writer_depth.is_some());
    ASSERT_TRUE(second_depth.is_some());
    EXPECT_FLOAT_EQ((*first_depth)[usize()], 0.0f);
    EXPECT_FLOAT_EQ((*first_depth)[usize(1)], 0.0f);
    EXPECT_FLOAT_EQ((*writer_depth)[usize()], 0.0f);
    EXPECT_FLOAT_EQ((*writer_depth)[usize(1)], 0.0f);
    EXPECT_FLOAT_EQ((*second_depth)[usize()], 1.0f);
    EXPECT_FLOAT_EQ((*second_depth)[usize(1)], 1.0f);

    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(1920, 1080, -1.0, 1.0));
    auto cameras           = Arc<owe::UniformCameraResolver>::make(rstd::move(camera));
    auto first_state       = Arc<owe::UniformNodeState>::make(first.clone(), cameras.clone());
    first_state->object_id = i32(-1);
    state->SetNodeState({ .index = u32(1), .generation = u32(1) }, first_state.clone());
    EXPECT_FLOAT_EQ(first_state->parallax.depth[rstd::usize(0)], 0.0f);
    EXPECT_FLOAT_EQ(first_state->parallax.depth[rstd::usize(1)], 0.0f);
}

TEST(ScriptNodeSoftMutation, ImageAlignmentBindingClonesForDynamicLayer) {
    owe::SceneNode  source;
    owe::SceneNode  clone;
    JsRuntime       rt;
    String          alignment;
    owe::SceneNode* target { nullptr };
    rt.RegisterImageAlignmentSetter(
        &source,
        "center"_str,
        JsRuntime::ImageAlignmentSetter::make([&](owe::SceneNode* node, ref<str> value) {
            target    = node;
            alignment = String::make(value);
        }));
    rt.CloneImageAlignmentBinding(&source, &clone);

    auto* fs = rt.MakeFieldScript(
        R"JS(
            thisLayer.alignment = 'bottom';
            export function update() { return thisLayer.alignment === 'bottom' ? 1 : 0; }
        )JS"_str,
        "test/cloned_image_alignment_write"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &clone);
    ASSERT_NE(fs, nullptr);

    EXPECT_EQ(target, &clone);
    EXPECT_EQ(alignment.as_str(), "bottom"_str);
    rt.TickAll();
    EXPECT_EQ(LastScalar(fs), 1.0);
}

TEST(ScriptNodeSoftMutation, OriginDispatchesRegisteredAccessors) {
    owe::SceneNode node;
    JsRuntime      rt;
    Vec3Value      logical_origin { .x = 10.0, .y = 20.0, .z = 0.0 };
    rt.RegisterNodeOriginAccessors(
        &node,
        JsRuntime::NodeOriginGetter::make([&logical_origin]() {
            return logical_origin;
        }),
        JsRuntime::NodeOriginSetter::make([&node, &logical_origin](Vec3Value origin) {
            logical_origin = origin;
            node.SetTranslate({ static_cast<float>(origin.x + 50.0),
                                static_cast<float>(origin.y),
                                static_cast<float>(origin.z) });
        }));

    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            thisLayer.origin = new Vec3(42, 7, 3);
            export function update() {
                const origin = thisLayer.origin;
                return origin.x * 100 + origin.y * 10 + origin.z;
            }
        )JS"_str,
        "test/node_origin_accessors"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    EXPECT_FLOAT_EQ(node.Translate().x(), 92.0f);
    EXPECT_FLOAT_EQ(node.Translate().y(), 7.0f);
    rt.TickAll();
    EXPECT_EQ(LastScalar(fs), 4273.0);
}

TEST(ScriptNodeActuator, AlphaFieldReturnWritesNodeAlpha) {
    auto node = rstd::sync::Arc<owe::SceneNode>::make();

    ScriptScene ss;
    auto*       fs = ss.runtime().MakeFieldScript(
        R"JS(
            export function update() { return 0.125; }
        )JS"_str,
        "test/alpha_field_return"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(1.0),
        node.as_ptr());
    ASSERT_NE(fs, nullptr);
    ss.AddActuator({ fs, MakeNodeAlphaApply(node.clone()) });

    FrameInputs fi {};
    ss.Tick(fi);

    EXPECT_TRUE(node->IsAlphaOverridden());
    EXPECT_FLOAT_EQ(node->UserAlpha(), 0.125f);
    EXPECT_FLOAT_EQ(node->EffectiveAlpha(), 0.125f);
}

TEST(ScriptNodeActuator, ColorFieldReturnWritesNodeColor) {
    auto node = Arc<owe::SceneNode>::make();

    ScriptScene ss;
    auto*       fs = ss.runtime().MakeFieldScript(
        R"JS(
            export function update() { return new Vec3(0.2, 0.4, 0.6); }
        )JS"_str,
        "test/color_field_return"_str,
        FieldKind::Vec3,
        owe::MakeObject(),
        owe::IntoJson("1 1 1"),
        node.as_ptr());
    ASSERT_NE(fs, nullptr);
    ss.AddActuator({ fs, MakeNodeColorApply(node.clone()) });

    FrameInputs fi {};
    ss.Tick(fi);

    EXPECT_FLOAT_EQ(node->Color().x(), 0.2f);
    EXPECT_FLOAT_EQ(node->Color().y(), 0.4f);
    EXPECT_FLOAT_EQ(node->Color().z(), 0.6f);
}

TEST(SceneNodeRuntimeAlpha, AlphaSourceContributesOverride) {
    owe::SceneNode source;
    owe::SceneNode composite;
    composite.SetAlphaSource(&source);

    EXPECT_FALSE(composite.IsAlphaOverridden());
    EXPECT_FLOAT_EQ(composite.EffectiveAlpha(), 1.0f);

    source.SetUserAlpha(0.25f);
    EXPECT_TRUE(composite.IsAlphaOverridden());
    EXPECT_FLOAT_EQ(composite.EffectiveAlpha(), 0.25f);

    composite.SetUserAlpha(0.5f);
    EXPECT_FLOAT_EQ(composite.EffectiveAlpha(), 0.125f);

    source.SetVisible(false);
    EXPECT_FLOAT_EQ(composite.EffectiveAlpha(), 0.0f);
}

TEST(ScriptNodeSoftMutation, BrightnessAndColorWrites) {
    owe::SceneNode node;
    JsRuntime      rt;
    FrameInputs    fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            thisLayer.brightness = 1.5;
            thisLayer.color = new Vec3(1, 0.5, 0);
            export function update() {}
        )JS"_str,
        "test/brightness_color"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    EXPECT_TRUE(node.IsBrightnessOverridden());
    EXPECT_FLOAT_EQ(node.Brightness(), 1.5f);
    EXPECT_TRUE(node.IsColorOverridden());
    EXPECT_FLOAT_EQ(node.Color().x(), 1.0f);
    EXPECT_FLOAT_EQ(node.Color().y(), 0.5f);
    EXPECT_FLOAT_EQ(node.Color().z(), 0.0f);
}

TEST(ScriptNodeSoftMutation, NoWritesLeaveOverridesUnset) {
    owe::SceneNode node;
    JsRuntime      rt;
    FrameInputs    fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            // Reads only; baked material values should stand.
            let a = thisLayer.alpha;
            let v = thisLayer.visible;
            export function update() {}
        )JS"_str,
        "test/no_writes"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    EXPECT_FALSE(node.IsAlphaOverridden());
    EXPECT_FALSE(node.IsBrightnessOverridden());
    EXPECT_FALSE(node.IsColorOverridden());
}

// ---------------------------------------------------------------------------
// Cursor event dispatch

namespace
{
FrameInputs MakeFi(float canvas_w = 1920.0f, float canvas_h = 1080.0f) {
    FrameInputs fi {};
    fi.canvas_w = canvas_w;
    fi.canvas_h = canvas_h;
    return fi;
}
} // namespace

TEST(ScriptCursor, EnterLeaveAndMove) {
    // A 200×200 layer centered at (500, 500). Cursor at (500/1920, 500/1080)
    // sits inside; (100/1920, 100/1080) sits outside.
    owe::SceneNode node;
    node.SetTranslate({ 500.0f, 500.0f, 0.0f });
    node.SetSize({ 200.0f, 200.0f });

    JsRuntime rt;
    rt.SetFrameInputs(MakeFi());
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let enters = 0, leaves = 0, moves = 0;
            export function cursorEnter() { enters++; }
            export function cursorLeave() { leaves++; }
            export function cursorMove()  { moves++;  }
            export function update() { return enters * 1000000 + leaves * 1000 + moves; }
        )JS"_str,
        "test/cursor_enter_leave_move"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    // Outside: no enter, no move.
    auto fi             = MakeFi();
    fi.cursor_in_window = true;
    fi.cursor_x         = 100.0f / 1920.0f;
    fi.cursor_y         = 100.0f / 1080.0f;
    rt.SetFrameInputs(fi);
    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 0.0);

    // Move inside: 1 enter + 1 move.
    fi.cursor_x = 500.0f / 1920.0f;
    fi.cursor_y = 500.0f / 1080.0f;
    rt.SetFrameInputs(fi);
    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 1'000'001);

    // Still inside (no edge): +1 move.
    rt.SetFrameInputs(fi);
    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 1'000'002);

    // Move outside: +1 leave (no move when outside).
    fi.cursor_x = 100.0f / 1920.0f;
    rt.SetFrameInputs(fi);
    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 1'001'002);
}

TEST(ScriptCursor, ClickAndDownUpInside) {
    owe::SceneNode node;
    node.SetTranslate({ 500.0f, 500.0f, 0.0f });
    node.SetSize({ 200.0f, 200.0f });

    JsRuntime rt;
    rt.SetFrameInputs(MakeFi());
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let down = 0, up = 0, click = 0, last_btn = -9;
            export function cursorDown(e)  { down++;  last_btn = e.button; }
            export function cursorUp(e)    { up++;    last_btn = e.button; }
            export function cursorClick(e) { click++; last_btn = e.button; }
            export function update() {
                return down * 10000 + up * 100 + click + (last_btn + 1) * 1000000;
            }
        )JS"_str,
        "test/cursor_click"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    auto fi             = MakeFi();
    fi.cursor_in_window = true;
    fi.cursor_x         = 500.0f / 1920.0f;
    fi.cursor_y         = 500.0f / 1080.0f;
    // Press left button (bit 0) this frame.
    fi.mouse_buttons_pressed = 1u << 0;
    fi.mouse_buttons_down    = 1u << 0;
    rt.SetFrameInputs(fi);
    rt.TickAll();
    // 1 down, 1 click, last_btn = 0 → 1*1000000 + 1*10000 + 0*100 + 1 = 1010001
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 1'010'001);

    // Release this frame (no press): 1 up, last_btn=0.
    fi.mouse_buttons_pressed  = 0;
    fi.mouse_buttons_released = 1u << 0;
    fi.mouse_buttons_down     = 0;
    rt.SetFrameInputs(fi);
    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 1'010'101);
}

TEST(ScriptCursor, ClickRestartsNamedAnimationLayer) {
    owe::SceneNode node;
    node.SetTranslate({ 500.0f, 500.0f, 0.0f });
    node.SetSize({ 200.0f, 200.0f });

    auto clip     = Arc<owe::SceneAnimationClip>::make(owe::SceneAnimationClipSpec {
        .name = String::make("Arona Drool"_str),
        .mode = String::make("single"_str),
        .fps  = 30.0f,
        .end  = i32(60),
    });
    auto playback = Arc<owe::SceneAnimationPlayback>::make(rstd::move(clip));
    node.RegisterAnimation(playback.clone());

    JsRuntime rt;
    rt.SetFrameInputs(MakeFi());
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let animation;
            export function init(value) {
                animation = thisLayer.getAnimationLayer("Arona Drool");
                animation.pause();
                return value;
            }
            export function cursorClick() {
                animation.stop();
                animation.play();
            }
            export function update() {
                return animation.getFrame() + (animation.isPlaying() ? 1000 : 0);
            }
        )JS"_str,
        "test/cursor_animation_layer"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);
    rt.SetSceneRoot(&node);
    EXPECT_FALSE(playback->IsPlaying());
    playback->SetFrame(i32(17));

    auto fi                  = MakeFi();
    fi.cursor_in_window      = true;
    fi.cursor_x              = 500.0f / 1920.0f;
    fi.cursor_y              = 500.0f / 1080.0f;
    fi.mouse_buttons_pressed = 1u << 0;
    rt.SetFrameInputs(fi);
    rt.TickAll();

    EXPECT_TRUE(playback->IsPlaying());
    EXPECT_EQ(playback->Frame(), i32());
    EXPECT_EQ(LastScalar(fs), 1000.0);
}

TEST(ScriptCursor, ClickOutsideIsIgnored) {
    owe::SceneNode node;
    node.SetTranslate({ 500.0f, 500.0f, 0.0f });
    node.SetSize({ 200.0f, 200.0f });

    JsRuntime rt;
    rt.SetFrameInputs(MakeFi());
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let click = 0;
            export function cursorClick() { click++; }
            export function update() { return click; }
        )JS"_str,
        "test/cursor_outside_click"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    auto fi                  = MakeFi();
    fi.cursor_in_window      = true;
    fi.cursor_x              = 100.0f / 1920.0f; // outside the AABB
    fi.cursor_y              = 100.0f / 1080.0f;
    fi.mouse_buttons_pressed = 1u << 0;
    rt.SetFrameInputs(fi);
    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 0.0);
}

TEST(ScriptCursor, CursorOutOfWindowSuppressesEvents) {
    owe::SceneNode node;
    node.SetTranslate({ 500.0f, 500.0f, 0.0f });
    node.SetSize({ 200.0f, 200.0f });

    JsRuntime rt;
    rt.SetFrameInputs(MakeFi());
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let n = 0;
            export function cursorEnter() { n++; }
            export function cursorMove()  { n++; }
            export function update() { return n; }
        )JS"_str,
        "test/cursor_out_of_window"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    auto fi             = MakeFi();
    fi.cursor_in_window = false; // outside window: events suppressed
    fi.cursor_x         = 500.0f / 1920.0f;
    fi.cursor_y         = 500.0f / 1080.0f;
    rt.SetFrameInputs(fi);
    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 0.0);
}

TEST(ScriptCursor, GlobalInputRefreshesFrameFields) {
    JsRuntime rt;
    rt.SetFrameInputs(MakeFi());
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update() {
                return input.cursorScreenPosition.x +
                       input.cursorScreenPosition.y * 1000 +
                       (input.cursorLeftDown ? 1000000 : 0) +
                       input.mouseButtonsDown * 10000000;
            }
        )JS"_str,
        "test/global_input_refresh"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0));
    ASSERT_NE(fs, nullptr);

    auto fi               = MakeFi();
    fi.screen_w           = 800.0f;
    fi.screen_h           = 600.0f;
    fi.cursor_x           = 0.25f;
    fi.cursor_y           = 0.5f;
    fi.mouse_buttons_down = 1u << 0;
    fi.cursor_in_window   = true;
    rt.SetFrameInputs(fi);
    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 11'300'200.0);

    fi.cursor_x           = 0.5f;
    fi.mouse_buttons_down = 0;
    rt.SetFrameInputs(fi);
    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 300'400.0);
}

TEST(ScriptCursor, WorldPositionFlipsTopDownInputY) {
    JsRuntime rt;
    rt.SetFrameInputs(MakeFi());
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update() {
                return new Vec3(
                    input.cursorWorldPosition.x,
                    input.cursorWorldPosition.y,
                    input.cursorScreenPosition.y);
            }
        )JS"_str,
        "test/global_input_world_y"_str,
        FieldKind::Vec3,
        owe::MakeObject(),
        owe::IntoJson("0.0 0.0 0.0"));
    ASSERT_NE(fs, nullptr);

    auto fi     = MakeFi();
    fi.screen_h = 600.0f;
    fi.cursor_x = 0.25f;
    fi.cursor_y = 0.25f;
    rt.SetFrameInputs(fi);
    rt.TickAll();

    ASSERT_TRUE(fs->last_value().is_Vec3());
    const auto& v = fs->last_value().as_Vec3().value;
    EXPECT_NEAR(v.x, 480.0, 0.001);
    EXPECT_NEAR(v.y, 810.0, 0.001);
    EXPECT_NEAR(v.z, 150.0, 0.001);
}

TEST(ScriptCursor, CroppedViewportUsesCameraForInputAndClicks) {
    owe::Scene scene;
    scene.SetOrtho({ rstd::i32(5120), rstd::i32(1440) });
    auto camera_node = Arc<owe::SceneNode>::make();
    camera_node->SetTranslate({ 2560.0f, 720.0f, 0.0f });
    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(2560, 1440, -1, 1));
    camera->AttatchNode(camera_node.as_ptr());
    scene.RegisterCamera("default"_Str, camera.clone());
    ASSERT_TRUE(scene.SetActiveCamera("default"_str));

    auto button = Arc<owe::SceneNode>::make();
    button->SetTranslate({ 3750.0f, 1380.0f, 0.0f });
    button->SetSize({ 112.0f, 112.0f });
    scene.RootMut()->AppendChild(button.clone());
    JsRuntime rt;
    rt.SetScene(&scene);
    rt.SetSceneRoot(scene.RootMut().as_raw_ptr());
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let clicks = 0;
            export function cursorClick(event) {
                if (Math.abs(event.worldPosition.x - input.cursorWorldPosition.x) < 0.001)
                    clicks++;
            }
            export function update() {
                return new Vec3(input.cursorWorldPosition.x, input.cursorWorldPosition.y, clicks);
            }
        )JS"_str,
        "test/cropped_cursor"_str,
        FieldKind::Vec3,
        owe::MakeObject(),
        owe::IntoJson("0 0 0"),
        button.as_ptr());
    ASSERT_NE(fs, nullptr);
    auto fi                  = MakeFi(5120, 1440);
    fi.cursor_x              = 1235.0f / 1280.0f;
    fi.cursor_y              = 30.0f / 720.0f;
    fi.cursor_in_window      = true;
    fi.mouse_buttons_pressed = 1;
    rt.SetFrameInputs(fi);
    rt.TickAll();
    ASSERT_TRUE(fs->last_value().is_Vec3());
    const auto first = fs->last_value().as_Vec3().value;
    EXPECT_NEAR(first.x, 3750.0, 0.001);
    EXPECT_NEAR(first.y, 1380.0, 0.001);
    EXPECT_EQ(first.z, 1.0);

    camera->SetWidth(5120);
    fi.cursor_x = 3750.0f / 5120.0f;
    rt.SetFrameInputs(fi);
    rt.TickAll();
    const auto second = fs->last_value().as_Vec3().value;
    EXPECT_NEAR(second.x, 3750.0, 0.001);
    EXPECT_NEAR(second.y, 1380.0, 0.001);
    EXPECT_EQ(second.z, 2.0);
}

TEST(ScriptCursor, SceneProjectionTracksViewportAndCameraChanges) {
    owe::Scene scene;
    scene.SetOrtho({ rstd::i32(5120), rstd::i32(1440) });
    EXPECT_TRUE(scene.ScreenToWorld({ 0.25f, 0.25f }).isApprox(Eigen::Vector3d(1280, 1080, 0)));
    auto node = Arc<owe::SceneNode>::make();
    node->SetTranslate({ 2560.0f, 720.0f, 0.0f });
    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(2560, 1440, -1, 1));
    camera->AttatchNode(node.as_ptr());
    scene.RegisterCamera("default"_Str, camera.clone());
    ASSERT_TRUE(scene.SetActiveCamera("default"_str));
    EXPECT_TRUE(scene.ScreenToWorld({ 0.0f, 0.0f }).isApprox(Eigen::Vector3d(1280, 1440, 0)));
    EXPECT_TRUE(scene.ScreenToWorld({ 1.0f, 1.0f }).isApprox(Eigen::Vector3d(3840, 0, 0)));
    camera->SetWidth(5120);
    camera->SetHeight(2880);
    EXPECT_TRUE(scene.ScreenToWorld({ 0.0f, 0.0f }).isApprox(Eigen::Vector3d(0, 2160, 0)));
    node->SetTranslate({ 100.0f, 200.0f, 0.0f });
    EXPECT_TRUE(scene.ScreenToWorld({ 0.5f, 0.5f }).isApprox(Eigen::Vector3d(100, 200, 0)));
    camera->SetWidth(0);
    EXPECT_TRUE(scene.ScreenToWorld({ 0.25f, 0.25f }).isApprox(Eigen::Vector3d(1280, 1080, 0)));
}

// ---------------------------------------------------------------------------
// Texture animation override

TEST(ScriptTexAnim, SetFramePinsAndStopsPlayback) {
    owe::SceneNode node;
    JsRuntime      rt;
    FrameInputs    fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let anim = thisLayer.getTextureAnimation();
            anim.setFrame(2);
            export function update() {
                return anim.getFrame() * 10 + (anim.isPlaying() ? 1 : 0);
            }
        )JS"_str,
        "test/texanim_setframe"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(node.TexAnim().current_frame, 2);
    EXPECT_FALSE(node.TexAnim().playing);
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 20.0);
}

TEST(ScriptTexAnim, PlayResumesAutoAdvance) {
    owe::SceneNode node;
    node.TexAnim().current_frame = 5;
    node.TexAnim().playing       = false;

    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            thisLayer.getTextureAnimation().play();
            export function update() {}
        )JS"_str,
        "test/texanim_play"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(node.TexAnim().current_frame, -1);
    EXPECT_TRUE(node.TexAnim().playing);
}

TEST(ScriptTexAnim, PauseFreezesAtCurrent) {
    owe::SceneNode node;
    JsRuntime      rt;
    FrameInputs    fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            thisLayer.getTextureAnimation().pause();
            export function update() {}
        )JS"_str,
        "test/texanim_pause"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_FALSE(node.TexAnim().playing);
    EXPECT_EQ(node.TexAnim().current_frame, -1); // pause keeps auto cursor
}

TEST(ScriptTexAnim, UnboundLayerFallsBackToJsStub) {
    // No node bound — getTextureAnimation() returns the JS-side stub from
    // the bootstrap, which silently accepts setFrame / play / etc.
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let a = thisLayer.getTextureAnimation();
            a.setFrame(7);
            export function update() { return a.getFrame(); }
        )JS"_str,
        "test/texanim_unbound"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    // JS stub records the frame in a closure local; getFrame returns it.
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 7.0);
}

TEST(ScriptVideoTexture, ControlsStableNativePlaybackState) {
    owe::SceneNode node;
    auto           playback = Arc<owe::VideoPlaybackState>::make();
    playback->PublishTime(f64(2.5), Some(f64(10.0)));
    node.SetVideoControl(playback.clone());

    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            const video = thisLayer.getVideoTexture();
            video.pause();
            video.rate = 1.5;
            video.setCurrentTime(4);
            export function update() {
                return video.duration + video.getCurrentTime() + (video.isPlaying() ? 1 : 0);
            }
        )JS"_str,
        "test/video_texture_control"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    auto state = playback->Snapshot();
    EXPECT_FALSE(state.playing);
    EXPECT_EQ(state.rate, f64(1.5));
    EXPECT_EQ(state.seek_sequence, u64(1));
    EXPECT_EQ(state.seek_seconds, f64(4.0));
    EXPECT_EQ(LastScalar(fs), 14.0);
}

// ---------------------------------------------------------------------------
// localStorage

namespace
{
std::string MakeTmpLsPath(const char* tag) {
    auto p = std::filesystem::temp_directory_path() / (std::string("owe_ls_") + tag + ".json");
    std::error_code ec;
    std::filesystem::remove(p, ec);
    return p.native();
}
} // namespace

TEST(ScriptLocalStorage, InMemoryWithoutPersistencePath) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            localStorage.set('k', 42);
            localStorage.set('o', { a: 1, b: 'two' });
            export function update() {
                let v = localStorage.get('k');
                let o = localStorage.get('o');
                return v + (o ? o.a + (o.b === 'two' ? 100 : 0) : 0);
            }
        )JS"_str,
        "test/ls_inmemory"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 42 + 1 + 100);
}

TEST(ScriptLocalStorage, RemoveDeletesKey) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            localStorage.set('k', 9);
            localStorage.remove('k');
            export function update() {
                let v = localStorage.get('k');
                return v === undefined ? -1 : v;
            }
        )JS"_str,
        "test/ls_remove"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, -1.0);
}

TEST(ScriptLocalStorage, PersistsAcrossRuntimes) {
    const std::string path = MakeTmpLsPath("persist");

    {
        JsRuntime rt;
        rt.SetPersistence(rstd::path::PathBuf::from(rstd::cppstd::as_str(path).unwrap()));
        FrameInputs fi {};
        rt.SetFrameInputs(fi);
        auto* fs = rt.MakeFieldScript(
            R"JS(
                localStorage.set('count', 7);
                localStorage.set('label', 'hello');
                export function update() {}
            )JS"_str,
            "test/ls_writer"_str,
            FieldKind::Scalar,
            owe::MakeObject(),
            owe::IntoJson(0),
            nullptr);
        ASSERT_NE(fs, nullptr);
    }

    // Fresh runtime reading the same file should see the prior writes.
    {
        JsRuntime rt;
        rt.SetPersistence(rstd::path::PathBuf::from(rstd::cppstd::as_str(path).unwrap()));
        FrameInputs fi {};
        rt.SetFrameInputs(fi);
        auto* fs = rt.MakeFieldScript(
            R"JS(
                export function update() {
                    let c = localStorage.get('count');
                    let l = localStorage.get('label');
                    return (c ?? -1) + (l === 'hello' ? 1000 : 0);
                }
            )JS"_str,
            "test/ls_reader"_str,
            FieldKind::Scalar,
            owe::MakeObject(),
            owe::IntoJson(0),
            nullptr);
        ASSERT_NE(fs, nullptr);
        rt.TickAll();
        EXPECT_EQ(fs->last_value().as_Scalar().value.v, 7 + 1000);
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ScriptLocalStorage, PreservesNativePathBytes) {
    auto directory = rstd::fs::TempDir::make("owe-localstorage"_str).unwrap();
    auto raw_name  = rstd::ffi::CStr::from_ptr("storage-\xff.json");
    auto name      = rstd::os::unix::ffi::OsStrExt::from_bytes(raw_name.to_bytes());
    auto path = rstd::path::PathBuf::from(directory.path()).join(rstd::ref<rstd::path::Path>(name));
    {
        JsRuntime rt;
        rt.SetPersistence(path.clone());
        auto* script =
            rt.MakeFieldScript("localStorage.set('count', 17); export function update() {}"_str,
                               "test/native_writer"_str,
                               FieldKind::Scalar,
                               owe::MakeObject(),
                               owe::IntoJson(0),
                               nullptr);
        ASSERT_NE(script, nullptr);
    }
    ASSERT_TRUE(rstd::fs::metadata(path).is_ok());
    {
        JsRuntime rt;
        rt.SetPersistence(path.clone());
        auto* script =
            rt.MakeFieldScript("export function update() { return localStorage.get('count'); }"_str,
                               "test/native_reader"_str,
                               FieldKind::Scalar,
                               owe::MakeObject(),
                               owe::IntoJson(0),
                               nullptr);
        ASSERT_NE(script, nullptr);
        rt.TickAll();
        EXPECT_EQ(script->last_value().as_Scalar().value.v, 17);
    }
}

TEST(ScriptLocalStorage, ObjectRoundTrip) {
    const std::string path = MakeTmpLsPath("obj");

    {
        JsRuntime rt;
        rt.SetPersistence(rstd::path::PathBuf::from(rstd::cppstd::as_str(path).unwrap()));
        FrameInputs fi {};
        rt.SetFrameInputs(fi);
        rt.MakeFieldScript(
            R"JS(
                localStorage.set('pos', { x: 10, y: 20 });
                export function update() {}
            )JS"_str,
            "test/ls_obj_write"_str,
            FieldKind::Scalar,
            owe::MakeObject(),
            owe::IntoJson(0),
            nullptr);
    }
    {
        JsRuntime rt;
        rt.SetPersistence(rstd::path::PathBuf::from(rstd::cppstd::as_str(path).unwrap()));
        FrameInputs fi {};
        rt.SetFrameInputs(fi);
        auto* fs = rt.MakeFieldScript(
            R"JS(
                export function update() {
                    let p = localStorage.get('pos');
                    return (p && p.x === 10 && p.y === 20) ? 1 : 0;
                }
            )JS"_str,
            "test/ls_obj_read"_str,
            FieldKind::Scalar,
            owe::MakeObject(),
            owe::IntoJson(0),
            nullptr);
        rt.TickAll();
        EXPECT_EQ(fs->last_value().as_Scalar().value.v, 1.0);
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(ScriptNodeChildren, WalksSceneNodeChildren) {
    auto parent = rstd::sync::Arc<owe::SceneNode>::make();
    auto a      = rstd::sync::Arc<owe::SceneNode>::make();
    auto b      = rstd::sync::Arc<owe::SceneNode>::make();
    parent->AppendChild(a.clone());
    parent->AppendChild(b.clone());
    a->SetTranslate({ 10.0f, 0.0f, 0.0f });
    b->SetTranslate({ 20.0f, 0.0f, 0.0f });

    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update() {
                const cs = thisLayer.getChildren();
                return cs.length * 1000 + (cs[0] ? cs[0].origin.x : 0)
                                       + (cs[1] ? cs[1].origin.x : 0);
            }
        )JS"_str,
        "test/getChildren_walk"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        parent.as_ptr());
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 2000 + 10 + 20);
}

TEST(ScriptLayerLookup, MissingLayerHandleResolvesLater) {
    auto root = rstd::sync::Arc<owe::SceneNode>::make();

    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    rt.SetSceneRoot(root.as_ptr());
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let late;
            export function init() {
                late = thisScene.getLayer("late-sound");
                late.stop();
            }
            export function applyUserProperties(changed) {
                if (changed.go) late.play();
            }
            export function update() { return late.isPlaying() ? 1 : 0; }
        )JS"_str,
        "test/lazy_layer_lookup"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        root.as_ptr());
    ASSERT_NE(fs, nullptr);

    auto late = rstd::sync::Arc<owe::SceneNode>::make(Eigen::Vector3f::Zero(),
                                                      Eigen::Vector3f::Ones(),
                                                      Eigen::Vector3f::Zero(),
                                                      "late-sound"_str);
    root->AppendChild(late.clone());
    rt.SetUserProperty("go"_str,
                       rstd::json::from_str(R"({"type":"bool","value":true})"_str).unwrap());
    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 1.0);
}

TEST(ScriptLayerLookup, GetEffectVisibleWritesSceneDirty) {
    owe::Scene scene;
    auto       root  = Box<owe::SceneNode>::make();
    auto       layer = rstd::sync::Arc<owe::SceneNode>::make(Eigen::Vector3f::Zero(),
                                                             Eigen::Vector3f::Ones(),
                                                             Eigen::Vector3f::Zero(),
                                                             "audio-layer"_str);
    root->AppendChild(layer.clone());
    auto root_pointer = root.get();
    scene.SetRoot(rstd::move(root));

    layer->SetCamera("audio-effect-camera"_str);
    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(256, 256, -1.0, 1.0));
    auto effect_layer = Arc<owe::SceneNodeLayer>::make(
        layer.as_ptr(), 256.0f, 256.0f, "_rt_effect_composite_test"_str);
    auto effect             = Arc<owe::SceneImageEffect>::make();
    effect->name            = "audio-color"_Str;
    effect->runtime_visible = true;
    effect_layer->AddEffect(effect);
    layer->AttachLayer(effect_layer.clone());
    scene.RegisterCamera(String::make("audio-effect-camera"_str), rstd::move(camera));

    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    rt.SetScene(&scene);
    rt.SetSceneRoot(root_pointer);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update() {
                const effect = thisScene.getLayer("audio-layer").getEffect("audio-color");
                effect.visible = false;
                return effect.visible ? 1 : 0;
            }
        )JS"_str,
        "test/layer_get_effect_visible"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        root_pointer);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 0.0);
    EXPECT_FALSE(effect->runtime_visible);
    EXPECT_TRUE(scene.ConsumeRenderGraphDirty());
    EXPECT_FALSE(scene.ConsumeRenderGraphDirty());
}

TEST(ScriptLayerLookup, EffectIndexAndMaterialWritesUseSceneMaterialOwner) {
    owe::Scene scene;
    auto       root  = Box<owe::SceneNode>::make();
    auto       layer = Arc<owe::SceneNode>::make(Eigen::Vector3f::Zero(),
                                                 Eigen::Vector3f::Ones(),
                                                 Eigen::Vector3f::Zero(),
                                                 "color-layer"_str);
    root->AppendChild(layer.clone());
    auto* root_pointer = root.get();
    scene.SetRoot(rstd::move(root));

    layer->SetCamera("color-effect-camera"_str);
    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(256, 256, -1.0, 1.0));
    auto effect_layer = Arc<owe::SceneNodeLayer>::make(
        layer.as_ptr(), 256.0f, 256.0f, "_rt_effect_composite_color"_str);
    auto effect                             = Arc<owe::SceneImageEffect>::make();
    effect->name                            = "color"_Str;
    auto                        effect_node = Arc<owe::SceneNode>::make();
    auto                        mesh        = Arc<owe::SceneMesh>::make();
    owe::SceneMaterial          material;
    owe::SceneShaderVariantDesc variant;
    (void)variant.uniform_aliases.insert("color"_Str, "g_TintColor"_Str);
    (void)variant.uniform_aliases.insert("channelMask"_Str, "g_ChannelMask"_Str);
    material.customShader.variant = Some(rstd::move(variant));
    (void)material.customShader.constValues.insert(
        "g_TintColor"_Str,
        owe::ShaderValue(owe::ShaderValue(rstd::array<float, 3> { 1.0f, 0.0f, 0.0f })));
    (void)material.customShader.constValues.insert(
        "g_ChannelMask"_Str,
        owe::ShaderValue(owe::ShaderValue(rstd::array<float, 4> { 1.0f, 1.0f, 1.0f, 1.0f })));
    mesh->AddMaterial(std::move(material));
    auto* effect_material = mesh->Material();
    effect_node->AddMesh(std::move(mesh));
    effect->AddNode(owe::SceneImageEffectNode {
        .output    = owe::SceneEffectTarget::LayerNext(),
        .sceneNode = effect_node.clone(),
    });
    effect_layer->AddEffect(effect);
    layer->AttachLayer(effect_layer.clone());
    scene.RegisterCamera(String::make("color-effect-camera"_str), rstd::move(camera));

    JsRuntime rt;
    rt.SetScene(&scene);
    rt.SetSceneRoot(root_pointer);
    auto  properties = rstd::json::from_str(R"({"color":"0.2 0.4 0.6"})"_str).unwrap();
    auto* fs         = rt.MakeFieldScript(
        R"JS(
            export var scriptProperties = createScriptProperties()
                .addColor({ name: 'color', value: new Vec3(1, 0, 0) })
                .finish();
            export function update() {
                const effect = thisLayer.getEffect(0);
                effect.getMaterial(0).color = scriptProperties.color;
                effect.getMaterial(0).channelMask = new Vec4(0, 0.25, 0.5, 0.75);
                return thisLayer.getEffectCount() + (effect.name === "color" ? 1 : 0);
            }
        )JS"_str,
        "test/layer_effect_material"_str,
        FieldKind::Scalar,
        properties,
        owe::IntoJson(0),
        layer.as_ptr());
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(LastScalar(fs), 2.0);
    ASSERT_NE(effect_material, nullptr);
    auto color = effect_material->customShader.constValues.get("g_TintColor"_str);
    ASSERT_TRUE(color.is_some());
    ASSERT_EQ((**color).size(), usize(3));
    EXPECT_FLOAT_EQ((**color)[usize()], 0.2f);
    EXPECT_FLOAT_EQ((**color)[usize(1)], 0.4f);
    EXPECT_FLOAT_EQ((**color)[usize(2)], 0.6f);
    auto channel_mask = effect_material->customShader.constValues.get("g_ChannelMask"_str);
    ASSERT_TRUE(channel_mask.is_some());
    ASSERT_EQ((**channel_mask).size(), usize(4));
    EXPECT_FLOAT_EQ((**channel_mask)[usize()], 0.0f);
    EXPECT_FLOAT_EQ((**channel_mask)[usize(1)], 0.25f);
    EXPECT_FLOAT_EQ((**channel_mask)[usize(2)], 0.5f);
    EXPECT_FLOAT_EQ((**channel_mask)[usize(3)], 0.75f);
}

TEST(ScriptLayerLookup, MissingLayerKeepsDefaultTransformShape) {
    auto root = rstd::sync::Arc<owe::SceneNode>::make();

    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    rt.SetSceneRoot(root.as_ptr());
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let resolved = -1;
            export function init() {
                const late = thisScene.getLayer("late-sound");
                resolved = late.scale.x + late.origin.x + late.angles.x;
            }
            export function update() { return resolved; }
        )JS"_str,
        "test/lazy_layer_default_transform"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        root.as_ptr());
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 1.0);
}

TEST(ScriptWEMath, SmoothStepCamelCaseAndAliases) {
    // ~165 corpus callsites use camelCase smoothStep; lowercase exists too.
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            import * as M from 'WEMath';
            export function update() {
                // smoothStep(0,1,0.5) → 0.5
                let a = M.smoothStep(0, 1, 0.5);
                let b = M.smoothstep(0, 1, 0.5);
                let c = M.deg2rad(180);   // ≈ π
                let d = M.rad2deg(Math.PI);  // 180
                let e = 180 * M.deg2rad;
                let f = Math.PI * M.rad2deg;
                return Math.round(a * 100) + Math.round(b * 100) * 100
                       + Math.round(c * 1000) * 10000   // π*1000 ≈ 3142
                       + Math.round(d) * 1000000000
                       + Math.round((e - c) * 1000000)
                       + Math.round((f - d) * 1000000);
            }
        )JS"_str,
        "test/wemath_smoothstep"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    // expected: 50 + 50*100 + 3142*10000 + 180*1e9
    EXPECT_EQ(fs->last_value().as_Scalar().value.v,
              50.0 + 50.0 * 100 + 3142.0 * 10000 + 180.0 * 1e9);
}

TEST(ScriptModule, ImportedBindingInitializesTopLevelConstBeforeUpdate) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            import * as WEColor from 'WEColor';
            const colors = {
                blue: WEColor.normalizeColor(new Vec3(110, 168, 255)),
            };
            export function update() {
                return WEColor.expandColor(colors.blue).divide(255);
            }
        )JS"_str,
        "test/module_top_level_import_binding"_str,
        FieldKind::Vec3,
        owe::MakeObject(),
        owe::IntoJson("0.0 0.0 0.0"),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    ASSERT_TRUE(fs->last_value().is_Vec3());
    const auto& value = fs->last_value().as_Vec3().value;
    EXPECT_NEAR(value.x, 110.0 / 255.0, 0.0001);
    EXPECT_NEAR(value.y, 168.0 / 255.0, 0.0001);
    EXPECT_NEAR(value.z, 1.0, 0.0001);
}

TEST(ScriptWEVector, VectorAngle2UsesDegrees) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            import * as V from 'WEVector';
            export function update() {
                return new Vec3(
                    V.vectorAngle2(new Vec2(1, 0)),
                    V.vectorAngle2(new Vec2(0, 1)),
                    V.vectorAngle2(V.angleVector2(-135)));
            }
        )JS"_str,
        "test/wevector_vector_angle2"_str,
        FieldKind::Vec3,
        owe::MakeObject(),
        owe::IntoJson("0.0 0.0 0.0"),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    ASSERT_TRUE(fs->last_value().is_Vec3());
    const auto& value = fs->last_value().as_Vec3().value;
    EXPECT_NEAR(value.x, 0.0, 0.001);
    EXPECT_NEAR(value.y, 90.0, 0.001);
    EXPECT_NEAR(value.z, -135.0, 0.001);
}

TEST(ScriptVector, InstanceMixInterpolatesVectors) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update(value) {
                let a = new Vec3(1, 2, 3).mix(new Vec3(5, 6, 7), 0.25);
                let b = new Vec2(2, 6).mix(new Vec2(10, 14), 0.5);
                let c = new Vec3(2).mix(6, 0.25);
                return new Vec3(a.x + b.x, a.y + b.y, a.z + c.z);
            }
        )JS"_str,
        "test/vector_mix"_str,
        FieldKind::Vec3,
        owe::MakeObject(),
        owe::IntoJson("0.0 0.0 0.0"),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    ASSERT_TRUE(fs->last_value().is_Vec3());
    const auto& v = fs->last_value().as_Vec3().value;
    EXPECT_NEAR(v.x, 8.0, 0.001);
    EXPECT_NEAR(v.y, 13.0, 0.001);
    EXPECT_NEAR(v.z, 7.0, 0.001);
}

TEST(ScriptVector, Vec2ConstructorCopiesVectorComponents) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update(value) {
                let fromVec3 = new Vec2(new Vec3(100, 200, 300));
                let fromObject = new Vec2({ x: 3, y: 4, z: 5 });
                return new Vec3(fromVec3.x, fromVec3.y, fromObject.length());
            }
        )JS"_str,
        "test/vector_vec2_copy_ctor"_str,
        FieldKind::Vec3,
        owe::MakeObject(),
        owe::IntoJson("0.0 0.0 0.0"),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    ASSERT_TRUE(fs->last_value().is_Vec3());
    const auto& v = fs->last_value().as_Vec3().value;
    EXPECT_NEAR(v.x, 100.0, 0.001);
    EXPECT_NEAR(v.y, 200.0, 0.001);
    EXPECT_NEAR(v.z, 5.0, 0.001);
}

TEST(ScriptVector, LengthSqrMatchesWallpaperEngineVectors) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update(value) {
                return new Vec3(2, 3, 6).lengthSqr() + new Vec2(5, 12).lengthSqr();
            }
        )JS"_str,
        "test/vector_length_sqr"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_NEAR(fs->last_value().as_Scalar().value.v, 218.0, 0.001);
}

TEST(ScriptVector, NormalizeReturnsUnitVectors) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update(value) {
                let a = new Vec3(3, 4, 0).normalize();
                let b = new Vec2(0, 5).normalize();
                let z = new Vec3(0, 0, 0).normalize();
                return new Vec3(a.x, a.y + b.y * 10, z.length());
            }
        )JS"_str,
        "test/vector_normalize"_str,
        FieldKind::Vec3,
        owe::MakeObject(),
        owe::IntoJson("0.0 0.0 0.0"),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    ASSERT_TRUE(fs->last_value().is_Vec3());
    const auto& v = fs->last_value().as_Vec3().value;
    EXPECT_NEAR(v.x, 0.6, 0.001);
    EXPECT_NEAR(v.y, 10.8, 0.001);
    EXPECT_NEAR(v.z, 0.0, 0.001);
}

TEST(ScriptVector, EngineCanvasSizeSupportsVectorMethods) {
    JsRuntime   rt;
    FrameInputs fi {};
    fi.canvas_w = 3840.0f;
    fi.canvas_h = 2160.0f;
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update() {
                const v = engine.canvasSize.divide(2);
                return v.x + v.y * 10000;
            }
        )JS"_str,
        "test/canvas_size_vec2_methods"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 1920.0 + 1080.0 * 10000);
}

TEST(ScriptScene, InitialLayerConfigPreservesAuthoredEffects) {
    owe::SceneNode node;
    JsRuntime      rt;
    FrameInputs    fi {};
    rt.SetFrameInputs(fi);
    rt.RegisterInitialLayerConfig(
        &node,
        rstd::json::from_str(
            R"({"name":"Brush","effects":[{"name":"Square"},{"name":"Glider"}]})"_str)
            .unwrap());
    rt.SetSceneRoot(&node);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let seen = -1;
            export function init() {
                const cfg = thisScene.getInitialLayerConfig(thisLayer);
                seen = cfg.name === 'Brush' &&
                       cfg.effects[0].name === 'Square' &&
                       cfg.effects[1].name === 'Glider'
                    ? cfg.effects.length
                    : -1;
            }
            export function update() { return seen; }
        )JS"_str,
        "test/initial_layer_config"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 2.0);
}

TEST(ScriptScene, DestroyLayerHidesSceneNode) {
    auto root  = rstd::sync::Arc<owe::SceneNode>::make();
    auto child = rstd::sync::Arc<owe::SceneNode>::make(
        Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), Eigen::Vector3f::Zero(), "coin"_str);
    root->AppendChild(child.clone());

    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    rt.SetSceneRoot(root.as_ptr());
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let hidden = 0;
            export function init() {
                const coin = thisScene.getLayer("coin");
                thisScene.destroyLayer(coin);
                hidden = coin.visible ? 0 : 1;
            }
            export function update() { return hidden; }
        )JS"_str,
        "test/destroy_layer_hides_node"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        root.as_ptr());
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_FALSE(child->Visible());
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 1.0);
}

TEST(ScriptScene, CameraTransformsRoundTripThroughSceneOwner) {
    owe::Scene scene;
    auto       camera = Arc<owe::SceneCamera>::make(
        owe::SceneCamera::MakePerspective(16.0 / 9.0, 0.01, 1000.0, 53.0));
    camera->SetLookAt({ 0.0, 0.0, 250.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 1.0, 0.0 });
    scene.RegisterCamera(String::make("main"_str), rstd::move(camera));
    ASSERT_TRUE(scene.SetActiveCamera("main"_str));

    JsRuntime rt;
    rt.SetScene(&scene);
    auto* script = rt.MakeFieldScript(
        R"JS(
            export function init() {
                const camera = thisScene.getCameraTransforms();
                camera.eye = new Vec3(3, 4, 5);
                camera.center = new Vec3(0, 0, 0);
                thisScene.setCameraTransforms(camera);
            }
            export function update() {
                const camera = thisScene.getCameraTransforms();
                return camera.eye.x * 100 + camera.eye.y * 10 + camera.eye.z;
            }
        )JS"_str,
        "test/camera_transforms_round_trip"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0));
    ASSERT_NE(script, nullptr);

    rt.SetSceneRoot(scene.RootMut().as_raw_ptr());
    rt.TickAll();

    auto transforms = scene.ActiveCameraTransforms();
    ASSERT_TRUE(transforms.is_some());
    EXPECT_TRUE(transforms->eye.isApprox(Eigen::Vector3d { 3.0, 4.0, 5.0 }));
    EXPECT_TRUE(transforms->center.isApprox(Eigen::Vector3d::Zero()));
    EXPECT_TRUE(transforms->up.isApprox(Eigen::Vector3d::UnitY()));
    EXPECT_DOUBLE_EQ(LastScalar(script), 345.0);
}

TEST(ScriptScene, OrthographicCameraTransformsUseAttachedNodeCoordinates) {
    owe::Scene scene;
    auto       camera_node = Arc<owe::SceneNode>::make(Eigen::Vector3f { 1920.0f, 1080.0f, 0.0f },
                                                       Eigen::Vector3f::Ones(),
                                                       Eigen::Vector3f::Zero());
    auto       camera      = Arc<owe::SceneCamera>::make(
        owe::SceneCamera::MakeOrthographic(3840.0, 2160.0, -5000.0, 5000.0));
    camera->AttatchNode(camera_node.as_ptr());
    scene.RegisterCamera(String::make("main"_str), rstd::move(camera));
    ASSERT_TRUE(scene.SetActiveCamera("main"_str));

    JsRuntime rt;
    rt.SetScene(&scene);
    auto* script = rt.MakeFieldScript(
        R"JS(
            export function init() {
                const camera = thisScene.getCameraTransforms();
                camera.eye = new Vec3(10, 20, 0);
                camera.center = new Vec3(10, 20, -1);
                thisScene.setCameraTransforms(camera);
            }
            export function update() {
                const camera = thisScene.getCameraTransforms();
                return camera.eye.x * 100 + camera.eye.y;
            }
        )JS"_str,
        "test/orthographic_camera_attached_coordinates"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0));
    ASSERT_NE(script, nullptr);

    rt.SetSceneRoot(scene.RootMut().as_raw_ptr());
    rt.TickAll();

    auto active = scene.ActiveCamera();
    ASSERT_TRUE(active.is_some());
    auto world = (**active).Transforms();
    EXPECT_TRUE(world.eye.isApprox(Eigen::Vector3d { 1930.0, 1100.0, 0.0 }));
    EXPECT_TRUE(world.center.isApprox(Eigen::Vector3d { 1930.0, 1100.0, -1.0 }));
    EXPECT_TRUE(world.up.isApprox(Eigen::Vector3d::UnitY()));
    EXPECT_DOUBLE_EQ(LastScalar(script), 1020.0);
}

TEST(ScriptScene, CreateLayerRoutesConfigurationAndLayerCloneToFactory) {
    auto root  = Arc<owe::SceneNode>::make();
    auto owner = Arc<owe::SceneNode>::make(
        Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), Eigen::Vector3f::Zero(), "owner"_str);
    auto style = Arc<owe::SceneNode>::make(
        Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), Eigen::Vector3f::Zero(), "Style1"_str);
    root->AppendChild(owner.clone());
    root->AppendChild(style.clone());

    Vec<owe::Json>           configs;
    Vec<Arc<owe::SceneNode>> created;
    JsRuntime                rt;
    rt.RegisterInitialLayerConfig(
        style.as_ptr(),
        rstd::json::from_str(R"({"name":"Style1","text":"template"})"_str).unwrap());
    rt.SetLayerConfigFactory(JsRuntime::LayerConfigFactory::make(
        [&root, &configs, &created](owe::SceneNode*,
                                    owe::Json config) -> Option<Arc<owe::SceneNode>> {
            auto node = Arc<owe::SceneNode>::make();
            root->AppendChild(node.clone());
            configs.push(config.clone());
            created.push(node.clone());
            return Some(rstd::move(node));
        }));
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let result = 0;
            export function init() {
                const background = thisScene.createLayer({
                    color: new Vec3(0.1, 0.2, 0.3),
                    size: new Vec3(10, 20, 0).toString()
                });
                const text = thisScene.createLayer(thisScene.getLayer('Style1'));
                result = background.visible && text.visible ? 2 : -1;
            }
            export function update() { return result; }
        )JS"_str,
        "test/create_layer_configuration"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        owner.as_ptr());
    ASSERT_NE(fs, nullptr);

    rt.SetSceneRoot(root.as_ptr());
    rt.ClearLayerFactory();
    rt.ClearLayerConfigFactory();
    rt.TickAll();

    ASSERT_EQ(configs.len(), usize(2));
    String color;
    String size;
    String text;
    EXPECT_TRUE(owe::GetJsonValue(configs[usize()], "color"_str, color, false));
    EXPECT_TRUE(owe::GetJsonValue(configs[usize()], "size"_str, size, false));
    EXPECT_TRUE(owe::GetJsonValue(configs[usize(1)], "text"_str, text, false));
    EXPECT_EQ(color, "0.1 0.2 0.3"_str);
    EXPECT_EQ(size, "10 20 0"_str);
    EXPECT_EQ(text, "template"_str);
    EXPECT_DOUBLE_EQ(LastScalar(fs), 2.0);
}

TEST(ScriptScene, CreateLayerRestoresTheCallingFieldScriptBinding) {
    auto root  = Arc<owe::SceneNode>::make();
    auto owner = Arc<owe::SceneNode>::make(Eigen::Vector3f { 344.0f, 328.0f, 0.0f },
                                           Eigen::Vector3f::Ones(),
                                           Eigen::Vector3f::Zero(),
                                           "owner"_str);
    root->AppendChild(owner.clone());

    Vec<Arc<owe::SceneNode>> created;
    JsRuntime                rt;
    rt.SetLayerFactory(JsRuntime::LayerFactory::make(
        [&root, &created, &rt](owe::SceneNode*,
                               LayerAssetReference) -> Option<Arc<owe::SceneNode>> {
            auto node = Arc<owe::SceneNode>::make();
            root->AppendChild(node.clone());
            created.push(node.clone());
            auto* nested = rt.MakeFieldScript(R"JS(export function init() {})JS"_str,
                                              "test/create_layer_nested_binding"_str,
                                              FieldKind::Scalar,
                                              owe::MakeObject(),
                                              owe::IntoJson(0),
                                              node.as_ptr());
            EXPECT_NE(nested, nullptr);
            return Some(rstd::move(node));
        }));
    auto* script = rt.MakeFieldScript(
        R"JS(
            let created = false;
            export function update() {
                if (!created) {
                    thisScene.createLayer('models/bar.json');
                    created = true;
                }
                return thisLayer.origin.x;
            }
        )JS"_str,
        "test/create_layer_restores_binding"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        owner.as_ptr());
    ASSERT_NE(script, nullptr);

    rt.SetSceneRoot(root.as_ptr());
    rt.TickAll();
    rt.ClearLayerFactory();

    ASSERT_EQ(created.len(), usize(1));
    EXPECT_DOUBLE_EQ(LastScalar(script), 344.0);
}

TEST(ScriptScene, CreatedLayersCanBeSortedBeforeAnExistingLayer) {
    owe::Scene scene;
    auto       ring = Arc<owe::SceneNode>::make(
        Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), Eigen::Vector3f::Zero(), "ring"_str);
    auto body = Arc<owe::SceneNode>::make(
        Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), Eigen::Vector3f::Zero(), "body"_str);
    scene.AttachRuntimeNode(*scene.RootMut(), ring.clone());
    scene.AttachRuntimeNode(*scene.RootMut(), body.clone());
    (void)scene.ConsumeRenderGraphDirty();

    Vec<Arc<owe::SceneNode>> created;
    JsRuntime                rt;
    rt.SetScene(&scene);
    rt.SetLayerFactory(JsRuntime::LayerFactory::make(
        [&scene, &created](owe::SceneNode*, LayerAssetReference) -> Option<Arc<owe::SceneNode>> {
            auto node = Arc<owe::SceneNode>::make();
            scene.AttachRuntimeNode(*scene.RootMut(), node.clone());
            created.push(node.clone());
            return Some(rstd::move(node));
        }));
    auto* fs = rt.MakeFieldScript(
        R"JS(
            let result = -1;
            export function init() {
                const target = thisScene.getLayerIndex(thisLayer);
                const first = thisScene.createLayer('models/bar.json');
                thisScene.sortLayer(first, target);
                const second = thisScene.createLayer('models/bar.json');
                thisScene.sortLayer(second, target);
                result = thisScene.getLayerIndex(first) * 1000
                       + thisScene.getLayerIndex(second) * 100
                       + thisScene.getLayerIndex(thisLayer) * 10
                       + thisScene.getLayerIndex('body');
            }
            export function update() { return result; }
        )JS"_str,
        "test/sort_created_layers"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        ring.as_ptr());
    ASSERT_NE(fs, nullptr);

    rt.SetSceneRoot(scene.RootMut().as_raw_ptr());
    rt.ClearLayerFactory();
    rt.TickAll();

    ASSERT_EQ(created.len(), usize(2));
    const auto& children = scene.Root()->GetChildren();
    ASSERT_EQ(children.len(), usize(4));
    EXPECT_EQ(children[usize()].as_ptr(), created[usize(1)].as_ptr());
    EXPECT_EQ(children[usize(1)].as_ptr(), created[usize()].as_ptr());
    EXPECT_EQ(children[usize(2)].as_ptr(), ring.as_ptr());
    EXPECT_EQ(children[usize(3)].as_ptr(), body.as_ptr());
    EXPECT_DOUBLE_EQ(LastScalar(fs), 1023.0);
    EXPECT_TRUE(scene.ConsumeRenderGraphDirty());
}

TEST(ScriptScene, RegisteredAssetFactoryCreatesAndReusesDestroyedLayer) {
    auto                     root = Arc<owe::SceneNode>::make();
    Vec<Arc<owe::SceneNode>> created;

    JsRuntime rt;
    rt.SetLayerFactory(JsRuntime::LayerFactory::make(
        [&root, &created](owe::SceneNode*,
                          LayerAssetReference asset) -> Option<Arc<owe::SceneNode>> {
            if (asset.path != "models/prism.mdl"_str) return None();
            auto node = Arc<owe::SceneNode>::make();
            node->SetVisible(false);
            root->AppendChild(node.clone());
            created.push(node.clone());
            return Some(rstd::move(node));
        }));
    auto* fs = rt.MakeFieldScript(
        R"JS(
            const prism = engine.registerAsset('models/prism.mdl');
            let result = 0;
            export function init() {
                const layers = [];
                for (let i = 0; i < 12; ++i) {
                    const layer = thisScene.createLayer(prism);
                    layer.origin = new Vec3(i, 0, 0);
                    layers.push(layer);
                }
                thisScene.destroyLayer(layers[0]);
                const reused = thisScene.createLayer(prism);
                result = reused.origin.x === 0 ? 12 : -1;
            }
            export function update() { return result; }
        )JS"_str,
        "test/registered_asset_factory"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        root.as_ptr());
    ASSERT_NE(fs, nullptr);
    ASSERT_EQ(fs->RegisteredAssets().len(), usize(1));

    rt.SetSceneRoot(root.as_ptr());
    rt.ClearLayerFactory();
    rt.TickAll();

    EXPECT_EQ(created.len(), usize(12));
    EXPECT_EQ(root->GetChildren().len(), usize(12));
    EXPECT_DOUBLE_EQ(LastScalar(fs), 12.0);
    EXPECT_TRUE(created[usize()]->Visible());
}

TEST(ScriptScene, DirectWorkshopAssetUsesLayerFactoryWithoutFixedCloneCapacity) {
    auto                     root = Arc<owe::SceneNode>::make();
    Vec<Arc<owe::SceneNode>> created;
    Vec<String>              paths;
    Vec<String>              workshop_ids;

    JsRuntime rt;
    rt.SetLayerFactory(JsRuntime::LayerFactory::make(
        [&root, &created, &paths, &workshop_ids](
            owe::SceneNode*, LayerAssetReference asset) -> Option<Arc<owe::SceneNode>> {
            paths.push(String::make(asset.path));
            if (asset.workshop_id.is_some()) workshop_ids.push(String::make(**asset.workshop_id));
            auto node = Arc<owe::SceneNode>::make();
            node->SetVisible(false);
            root->AppendChild(node.clone());
            created.push(node.clone());
            return Some(rstd::move(node));
        }));
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export let __workshopId = '3365654061';
            let result = 0;
            export function init() {
                for (let i = 0; i < 127; ++i) {
                    const layer = thisScene.createLayer('models/cav_default_texture.json');
                    if (layer.visible) ++result;
                }
            }
            export function update() { return result; }
        )JS"_str,
        "test/direct_workshop_asset_factory"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        root.as_ptr());
    ASSERT_NE(fs, nullptr);

    rt.SetSceneRoot(root.as_ptr());
    rt.ClearLayerFactory();
    rt.TickAll();

    ASSERT_EQ(created.len(), usize(127));
    ASSERT_EQ(paths.len(), usize(127));
    ASSERT_EQ(workshop_ids.len(), usize(127));
    EXPECT_EQ(paths[usize()], "models/cav_default_texture.json"_str);
    EXPECT_EQ(workshop_ids[usize()], "3365654061"_str);
    EXPECT_DOUBLE_EQ(LastScalar(fs), 127.0);
}

TEST(ScriptScene, ParticleInstanceAndPlaybackUseNodeCapability) {
    auto root  = Arc<owe::SceneNode>::make();
    auto layer = Arc<owe::SceneNode>::make();
    auto state = Arc<ParticleControlState>::make();
    layer->SetParticleControl(
        Arc<dyn<owe::SceneParticleControl>>::make(ParticleControlProbe { state.clone() }));
    root->AppendChild(layer.clone());

    JsRuntime rt;
    auto*     fs = rt.MakeFieldScript(
        R"JS(
            export function init() {
                thisLayer.instance.colorn = new Vec3(0.2, 0.4, 0.8);
                thisLayer.stop();
                thisLayer.play();
            }
            export function update() {
                const color = thisLayer.instance.colorn;
                return color.x * 100 + color.y * 10 + color.z + (thisLayer.isPlaying() ? 1000 : 0);
            }
        )JS"_str,
        "test/particle_instance_control"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        layer.as_ptr());
    ASSERT_NE(fs, nullptr);
    rt.SetSceneRoot(root.as_ptr());
    rt.TickAll();

    EXPECT_FLOAT_EQ(state->colorn[0], 0.2f);
    EXPECT_FLOAT_EQ(state->colorn[1], 0.4f);
    EXPECT_FLOAT_EQ(state->colorn[2], 0.8f);
    EXPECT_EQ(state->resets, 2);
    EXPECT_NEAR(LastScalar(fs), 1024.8, 1e-5);
}

TEST(SceneNodeSound, VisibilityStartsAndStopsTheSoundControl) {
    auto layer = Arc<owe::SceneNode>::make();
    auto state = Arc<SoundControlState>::make();
    layer->SetSoundControl(
        Arc<dyn<owe::SceneSoundControl>>::make(SoundControlProbe { state.clone() }));
    ASSERT_TRUE(state->playing);

    layer->SetVisible(false);
    EXPECT_FALSE(state->playing);

    layer->SetVisible(true);
    EXPECT_TRUE(state->playing);
}

TEST(ScriptScene, SoundVolumeUsesSoundControl) {
    auto root  = Arc<owe::SceneNode>::make();
    auto layer = Arc<owe::SceneNode>::make();
    auto state = Arc<SoundControlState>::make();
    layer->SetSoundControl(
        Arc<dyn<owe::SceneSoundControl>>::make(SoundControlProbe { state.clone() }));
    root->AppendChild(layer.clone());

    JsRuntime rt;
    auto*     fs = rt.MakeFieldScript(
        R"JS(
            export function init() {
                thisLayer.stop();
                thisLayer.volume = 0.25;
                thisLayer.play();
            }
            export function update() {
                return thisLayer.volume + (thisLayer.isPlaying() ? 1 : 0);
            }
        )JS"_str,
        "test/sound_volume_control"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        layer.as_ptr());
    ASSERT_NE(fs, nullptr);
    rt.SetSceneRoot(root.as_ptr());
    rt.TickAll();

    EXPECT_FLOAT_EQ(state->volume, 0.25f);
    EXPECT_TRUE(state->playing);
    EXPECT_DOUBLE_EQ(LastScalar(fs), 1.25);
}

// ---------------------------------------------------------------------------
// Workshop 3327063360 repro: scripted-origin layer should land at canvas
// center when scriptProperties.{x,y} fall back to their declared 0.5.

TEST(ScriptUserProperty, UserPropertyOverridesFallback) {
    // ResolveConfigValue stores the {user, value} wrapper verbatim; the
    // bootstrap getter unwraps at access time. SetUserProperty in
    // between should win.
    JsRuntime rt;
    owe::Json properties = rstd::json::from_str(R"({"x":{"user":"x1","value":0.5}})"_str).unwrap();
    rt.SetUserProperty("x1"_str,
                       rstd::json::from_str(R"({"type":"slider","value":-0.665})"_str).unwrap());
    FrameInputs fi {};
    fi.canvas_w = 3840.0f;
    fi.canvas_h = 2160.0f;
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export var scriptProperties = createScriptProperties()
              .addSlider({ name: 'x', value: 0.5, min: 0, max: 1 })
              .finish();
            export function update() { return scriptProperties.x; }
        )JS"_str,
        "test/user_prop_override"_str,
        FieldKind::Scalar,
        properties,
        owe::IntoJson(0),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    // User value passes through verbatim — WE doesn't clamp, even when the
    // user's slider range (e.g. project.json [-1,1]) exceeds the script's
    // declared range. Workshop 3327063360 relies on this: x1=-0.665 fed
    // into `scriptProperties.x * canvasSize.x` produces a negative offset
    // that shifts the Clock cluster off the master-component origin.
    EXPECT_NEAR(fs->last_value().as_Scalar().value.v, -0.665, 1e-4);
}

TEST(ScriptUserProperty, FallbackWhenUserPropMissing) {
    JsRuntime rt;
    owe::Json properties =
        rstd::json::from_str(R"({"x":{"user":"missing","value":0.5}})"_str).unwrap();
    FrameInputs fi {};
    fi.canvas_w = 3840.0f;
    fi.canvas_h = 2160.0f;
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export var scriptProperties = createScriptProperties()
              .addSlider({ name: 'x', value: 0.5, min: 0, max: 1 })
              .finish();
            export function update() { return scriptProperties.x; }
        )JS"_str,
        "test/user_prop_fallback"_str,
        FieldKind::Scalar,
        properties,
        owe::IntoJson(0),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_NEAR(fs->last_value().as_Scalar().value.v, 0.5, 1e-4);
}

TEST(ScriptUserProperty, ApplyUserPropertiesReceivesUnwrappedValue) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = MakeProbe(rt,
                         "test/apply_user_properties_value",
                         R"JS(
        let seen = 0;
        export function applyUserProperties(changed) {
            if (changed.music === "5") seen = 1;
        }
        export function update() { return seen; }
    )JS");
    ASSERT_NE(fs, nullptr);

    rt.SetUserProperty("music"_str,
                       rstd::json::from_str(R"({"type":"combo","value":"5"})"_str).unwrap());
    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 1.0);
}

TEST(ScriptUserProperty, DirectReadsReceiveUpdatedComboValue) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = MakeProbe(rt,
                         "test/direct_combo_user_property",
                         R"JS(
        export function update() {
            if (engine.userProperties.timeofday != 0) {
                return engine.userProperties.timeofday - 1;
            }
            return -1;
        }
    )JS");
    ASSERT_NE(fs, nullptr);

    rt.SetUserProperty("timeofday"_str,
                       rstd::json::from_str(R"({"type":"combo","value":"1"})"_str).unwrap());
    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 0.0);

    rt.SetUserProperty("timeofday"_str,
                       rstd::json::from_str(R"({"type":"combo","value":"2"})"_str).unwrap());
    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 1.0);
}

TEST(ScriptUserProperty, TextInputValueRemainsAString) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = MakeProbe(rt,
                         "test/apply_text_user_property",
                         R"JS(
        let seen = 0;
        export function applyUserProperties(changed) {
            if (changed.text === "true" && typeof changed.text === "string") seen = 1;
        }
        export function update() { return seen; }
    )JS");
    ASSERT_NE(fs, nullptr);

    rt.SetUserProperty("text"_str,
                       rstd::json::from_str(R"({"type":"textinput","value":"true"})"_str).unwrap());
    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 1.0);
}

TEST(ScriptMedia, DispatchesPropertiesPlaybackAndThumbnailEvents) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = MakeProbe(rt,
                         "test/media_events",
                         R"JS(
        let props = 0, playback = 0, thumb = 0;
        export function mediaPropertiesChanged(event) {
            if (event.title === "Song" && event.artist === "Artist" &&
                event.album === "Album" && event.albumTitle === "Album" &&
                event.albumArtist === "Album Artist") {
                props = 1;
            }
        }
        export function mediaPlaybackChanged(event) {
            if (event.state === MediaPlaybackEvent.PLAYBACK_PLAYING) playback = 1;
        }
        export function mediaThumbnailChanged(event) {
            thisObject.visible = event.hasThumbnail;
            const colorDelta = event.textColor.subtract(event.tertiaryColor)
                .add(event.highContrastColor);
            if (event.hasThumbnail && event.thumbnail === "/tmp/cover.png" &&
                event.previousThumbnail === "/tmp/previous.png" &&
                event.primaryColor.x === 1 && event.secondaryColor.x === 0 &&
                event.tertiaryColor.x === 0 && colorDelta.lengthSqr() === 0) {
                thumb = 1;
            }
        }
        export function update() { return props && playback && thumb && thisObject.visible ? 1 : 0; }
    )JS");
    ASSERT_NE(fs, nullptr);

    rt.SetMediaStatus(MediaStatus { .state            = 1,
                                    .title            = "Song"_Str,
                                    .artist           = "Artist"_Str,
                                    .album            = "Album"_Str,
                                    .album_artist     = "Album Artist"_Str,
                                    .art_url          = "/tmp/cover.png"_Str,
                                    .previous_art_url = "/tmp/previous.png"_Str });
    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 1.0);
}

TEST(ScriptUserProperty, ScriptedOriginLandsAtCenter) {
    JsRuntime   rt;
    FrameInputs fi {};
    fi.canvas_w = 3840.0f;
    fi.canvas_h = 2160.0f;
    rt.SetFrameInputs(fi);

    owe::Json properties =
        rstd::json::from_str(R"({"x":{"user":"x7","value":0.5},"y":{"user":"y8","value":0.5}})"_str)
            .unwrap();

    auto* fs = rt.MakeFieldScript(
        R"JS(
            'use strict';
            export var scriptProperties = createScriptProperties()
              .addSlider({ name: 'x', label: 'X', value: 0.5, min: 0, max: 1 })
              .addSlider({ name: 'y', label: 'Y', value: 0.5, min: 0, max: 1 })
              .finish();
            export function update(value) {
                value.x = scriptProperties.x * engine.canvasSize.x;
                value.y = scriptProperties.y * engine.canvasSize.y;
                return value;
            }
        )JS"_str,
        "test/workshop_3327_repro"_str,
        FieldKind::Vec3,
        properties,
        owe::IntoJson("1315.0 1419.0 0.0"),
        nullptr);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    ASSERT_TRUE(fs->last_value().is_Vec3());
    const auto& v = fs->last_value().as_Vec3().value;
    EXPECT_NEAR(v.x, 1920.0, 0.5);
    EXPECT_NEAR(v.y, 1080.0, 0.5);
}

TEST(ScriptAnimation, AnimatedRotationPassThroughStaysSynchronized) {
    owe::Scene scene;
    auto       node  = Arc<owe::SceneNode>::make();
    auto       peer  = Arc<owe::SceneNode>::make();
    auto       curve = Arc<owe::SceneAnimationCurve>::make();
    curve->c1.push({ .frame = i32(), .value = 0.0f });
    curve->c1.push({ .frame = i32(1), .value = 2.0f * rstd::f32::consts::PI.to_primitive() });
    auto clip          = Arc<owe::SceneAnimationClip>::make(owe::SceneAnimationClipSpec {
        .name = String::make("spin"_str),
        .mode = String::make("loop"_str),
        .fps  = 0.1f,
        .end  = i32(1),
    });
    auto playback      = Arc<owe::SceneAnimationPlayback>::make(clip.clone());
    auto peer_playback = Arc<owe::SceneAnimationPlayback>::make(clip.clone());
    node->SetRotationAnimation({ .curve = curve.clone(), .playback = playback.clone() });
    peer->SetRotationAnimation({ .curve = curve.clone(), .playback = peer_playback.clone() });
    scene.RootMut()->AppendChild(node.clone());
    scene.RootMut()->AppendChild(peer.clone());
    JsRuntime rt;
    auto*     fs = rt.MakeFieldScript(
        "export function update(value) { return value; }"_str,
        "test/animated_rotation_passthrough"_str,
        FieldKind::Vec3,
        owe::MakeObject(),
        owe::IntoJson("0 0 0"),
        ScriptBindingContext::ForLayer(node.as_ptr(), "angles"_str, Some(playback.clone())));
    ASSERT_NE(fs, nullptr);
    auto apply = MakeNodeTransformApply(node.clone(), NodeTransformTarget::Rotation);
    for (double delta : { 0.0, 2.5, 2.5, 2.5 }) {
        scene.Runtime().Advance(rstd::f64(delta));
        scene.TickNodeFieldAnimations();
        rt.TickAll();
        apply->operator()(fs->last_value());
        EXPECT_TRUE(node->Rotation().isApprox(peer->Rotation(), 0.00001f));
    }
    playback->Pause();
    peer_playback->Pause();
    scene.Runtime().Advance(rstd::f64(1.0));
    scene.TickNodeFieldAnimations();
    rt.TickAll();
    apply->operator()(fs->last_value());
    EXPECT_TRUE(node->Rotation().isApprox(peer->Rotation(), 0.00001f));
    playback->Stop();
    peer_playback->Stop();
    scene.TickNodeFieldAnimations();
    rt.TickAll();
    apply->operator()(fs->last_value());
    EXPECT_TRUE(node->Rotation().isZero(0.00001f));
    playback->Play();
    peer_playback->Play();
    scene.Runtime().Advance(rstd::f64(1.0));
    scene.TickNodeFieldAnimations();
    rt.TickAll();
    apply->operator()(fs->last_value());
    EXPECT_TRUE(node->Rotation().isApprox(peer->Rotation(), 0.00001f));
    EXPECT_GT(node->Rotation().y(), 0.0f);
}

TEST(ScriptAnimation, AnimatedValueRefreshPrecedesEventOverrides) {
    auto node  = Arc<owe::SceneNode>::make();
    auto curve = Arc<owe::SceneAnimationCurve>::make();
    curve->c0.push({ .frame = i32(), .value = 0.25f });
    auto clip     = Arc<owe::SceneAnimationClip>::make(owe::SceneAnimationClipSpec {
        .fps = 1.0f,
        .end = i32(1),
    });
    auto playback = Arc<owe::SceneAnimationPlayback>::make(rstd::move(clip));
    node->SetAlphaAnimation({ .curve = rstd::move(curve), .playback = playback.clone() });
    JsRuntime rt;
    auto*     fs = rt.MakeFieldScript(
        "export function animationEvent(event, value) { return value + 0.5; } "
        "export function update(value) { return value; }"_str,
        "test/animated_value_event"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        ScriptBindingContext::ForLayer(node.as_ptr(), "alpha"_str, Some(playback.clone())));
    ASSERT_NE(fs, nullptr);
    Vec<owe::SceneAnimationEventDispatch> events;
    node->TickFieldAnimations(0.0, events);
    events.push({ .node = node.as_ptr(), .event = { .name = String::make("beat"_str) } });
    rt.TickAll(events.as_slice());
    EXPECT_DOUBLE_EQ(LastScalar(fs), 0.75);
    MakeNodeAlphaApply(node.clone())->operator()(fs->last_value());
    events.clear();
    node->TickFieldAnimations(0.5, events);
    rt.TickAll();
    EXPECT_DOUBLE_EQ(LastScalar(fs), 0.25);
}

TEST(SceneAnimationPlayback, SharedFieldBindingsAdvanceOnceAndPreserveEventOrder) {
    owe::Scene scene;
    auto       node = Arc<owe::SceneNode>::make();

    Vec<owe::SceneAnimationEvent> authored_events;
    authored_events.push(
        { .frame = i32(2), .order = usize(1), .name = String::make("second"_str) });
    authored_events.push({ .frame = i32(2), .order = usize(), .name = String::make("first"_str) });
    auto clip = Arc<owe::SceneAnimationClip>::make(owe::SceneAnimationClipSpec {
        .events = rstd::move(authored_events),
        .mode   = String::make("loop"_str),
        .fps    = 10.0f,
        .end    = i32(10),
    });

    auto playback = Arc<owe::SceneAnimationPlayback>::make(rstd::move(clip), true);
    node->BindFieldAnimation(String::make("origin"_str), playback.clone());
    node->BindFieldAnimation(String::make("alpha"_str), playback.clone());
    auto peer = Arc<owe::SceneNode>::make();
    peer->BindFieldAnimation(String::make("amount"_str), playback.clone());
    scene.RootMut()->AppendChild(node.clone());
    scene.RootMut()->AppendChild(peer.clone());

    scene.Runtime().Advance(rstd::f64(0.2));
    scene.TickNodeFieldAnimations();
    EXPECT_TRUE(scene.ConsumeAnimationEvents().is_empty());
    EXPECT_EQ(playback->Frame(), i32());

    playback->Play();
    scene.Runtime().Advance(rstd::f64(0.2));
    scene.TickNodeFieldAnimations();
    auto events = scene.ConsumeAnimationEvents();
    ASSERT_EQ(events.len(), usize(2));
    EXPECT_EQ(events[usize()].event.name, "first"_str);
    EXPECT_EQ(events[usize()].event.frame, i32(2));
    EXPECT_EQ(events[usize(1)].event.name, "second"_str);
    EXPECT_EQ(events[usize(1)].event.frame, i32(2));
    EXPECT_EQ(playback->Frame(), i32(2));

    playback->Pause();
    scene.Runtime().Advance(rstd::f64(0.3));
    scene.TickNodeFieldAnimations();
    EXPECT_EQ(playback->Frame(), i32(2));

    playback->SetFrame(i32(4));
    playback->SetRate(2.0f);
    playback->Play();
    scene.Runtime().Advance(rstd::f64(0.1));
    scene.TickNodeFieldAnimations();
    EXPECT_EQ(playback->Frame(), i32(6));

    playback->SetFrame(i32(10));
    scene.Runtime().Advance(rstd::f64(0.1));
    scene.TickNodeFieldAnimations();
    EXPECT_EQ(playback->Frame(), i32(2));
}

TEST(ScriptAnimation, BroadcastsMarkersAndControlsSharedPlayback) {
    owe::SceneNode layer;
    owe::SceneNode other_layer;

    auto clip     = Arc<owe::SceneAnimationClip>::make(owe::SceneAnimationClipSpec {
        .name = String::make("main"_str),
        .mode = String::make("loop"_str),
        .fps  = 6.0f,
        .end  = i32(12),
    });
    auto playback = Arc<owe::SceneAnimationPlayback>::make(rstd::move(clip));

    auto secondary_clip = Arc<owe::SceneAnimationClip>::make(owe::SceneAnimationClipSpec {
        .name = String::make("secondary"_str),
        .mode = String::make("loop"_str),
        .fps  = 8.0f,
        .end  = i32(16),
    });
    auto secondary      = Arc<owe::SceneAnimationPlayback>::make(rstd::move(secondary_clip));
    layer.BindFieldAnimation(String::make("scale"_str), secondary.clone());
    layer.BindFieldAnimation(String::make("origin"_str), playback.clone());

    JsRuntime rt;
    auto*     controller = rt.MakeFieldScript(
        R"JS(
            const animation = thisObject.getAnimation();
            const secondary = thisLayer.getAnimation("secondary");
            export function animationEvent(event, value) {
                return event.name === "beat" ? value + event.frame : -1000;
            }
            export function update(value) {
                animation.setFrame(4);
                animation.rate = 2;
                animation.pause();
                secondary.setFrame(3);
                secondary.pause();
                const metadata = animation.name === "main" && animation.fps === 6 &&
                    animation.frameCount === 12 && animation.duration === 2 &&
                    secondary.name === "secondary" ? 100 : 0;
                return value + animation.getFrame() + animation.rate + secondary.getFrame() + metadata;
            }
        )JS"_str,
        "test/animation_controller"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        ScriptBindingContext::ForLayer(&layer, "origin"_str, Some(playback.clone())));
    auto* peer = rt.MakeFieldScript(
        R"JS(
            export function animationEvent(event) { return event.frame + 1; }
            export function update(value) { return value; }
        )JS"_str,
        "test/animation_peer"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &layer);
    auto* unrelated = rt.MakeFieldScript(
        R"JS(
            export function animationEvent() { return 99; }
            export function update(value) { return value; }
        )JS"_str,
        "test/animation_unrelated"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &other_layer);
    ASSERT_NE(controller, nullptr);
    ASSERT_NE(peer, nullptr);
    ASSERT_NE(unrelated, nullptr);

    Vec<owe::SceneAnimationEventDispatch> events;
    events.push(
        { .node  = &layer,
          .event = { .frame = i32(3), .order = usize(), .name = String::make("beat"_str) } });
    rt.TickAll(events.as_slice());

    EXPECT_EQ(LastScalar(controller), 112.0);
    EXPECT_EQ(LastScalar(peer), 4.0);
    EXPECT_EQ(LastScalar(unrelated), 0.0);
    EXPECT_EQ(playback->Frame(), i32(4));
    EXPECT_FLOAT_EQ(playback->Rate(), 2.0f);
    EXPECT_FALSE(playback->IsPlaying());
    EXPECT_EQ(secondary->Frame(), i32(3));
    EXPECT_FALSE(secondary->IsPlaying());
}

TEST(ScriptAnimation, SeparatesLayerAndCurrentPropertyLookup) {
    owe::SceneNode layer;
    auto           clip     = Arc<owe::SceneAnimationClip>::make(owe::SceneAnimationClipSpec {
        .name = String::make("layer-track"_str),
        .fps  = 12.0f,
        .end  = i32(24),
    });
    auto           playback = Arc<owe::SceneAnimationPlayback>::make(rstd::move(clip));
    layer.BindFieldAnimation(String::make("origin"_str), playback.clone());

    JsRuntime rt;
    auto*     unanimated = rt.MakeFieldScript(
        R"JS(
            const propertyAnimation = thisObject.getAnimation();
            const layerAnimation = thisLayer.getAnimation("layer-track");
            export function update() {
                return thisObject !== thisLayer && propertyAnimation.frameCount === 1 &&
                    layerAnimation.frameCount === 24 ? 1 : 0;
            }
        )JS"_str,
        "test/animation_property_scope"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        ScriptBindingContext::ForLayer(&layer, "alpha"_str));
    ASSERT_NE(unanimated, nullptr);

    owe::SceneMaterial material;
    auto*              material_property = rt.MakeFieldScript(
        R"JS(
            const animation = thisObject.getAnimation();
            export function update() {
                return thisObject !== thisLayer && animation.name === "layer-track" ? 1 : 0;
            }
        )JS"_str,
        "test/animation_material_property_scope"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        ScriptBindingContext::ForMaterial(&layer, &material, "amount"_str, Some(playback.clone())));
    ASSERT_NE(material_property, nullptr);

    auto* scene_property = rt.MakeFieldScript(
        R"JS(
            const animation = thisObject.getAnimation();
            export function update() {
                return thisObject !== thisLayer && animation.name === "layer-track" ? 1 : 0;
            }
        )JS"_str,
        "test/animation_scene_property_scope"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        ScriptBindingContext::ForLayer(nullptr, "camerashake"_str, Some(playback.clone())));
    ASSERT_NE(scene_property, nullptr);

    rt.TickAll();
    EXPECT_EQ(LastScalar(unanimated), 1.0);
    EXPECT_EQ(LastScalar(material_property), 1.0);
    EXPECT_EQ(LastScalar(scene_property), 1.0);
}

TEST(ScriptAnimation, TimerKeepsCurrentPropertyAnimation) {
    owe::SceneNode layer;
    auto           clip     = Arc<owe::SceneAnimationClip>::make(owe::SceneAnimationClipSpec {
        .name = String::make("timer-track"_str),
        .fps  = 10.0f,
        .end  = i32(20),
    });
    auto           playback = Arc<owe::SceneAnimationPlayback>::make(rstd::move(clip));

    JsRuntime rt;
    auto*     script = rt.MakeFieldScript(
        R"JS(
            let observed = 0;
            export function init(value) {
                setTimeout(() => {
                    const animation = thisObject.getAnimation();
                    observed = animation.name === "timer-track" ? animation.frameCount : -1;
                }, 10);
                return value;
            }
            export function update() { return observed; }
        )JS"_str,
        "test/animation_timer_property_scope"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        ScriptBindingContext::ForLayer(&layer, "origin"_str, Some(rstd::move(playback))));
    ASSERT_NE(script, nullptr);
    rt.SetSceneRoot(&layer);

    FrameInputs inputs {};
    inputs.runtime = 0.02f;
    rt.SetFrameInputs(inputs);
    rt.TickAll();
    EXPECT_EQ(LastScalar(script), 20.0);
}

TEST(SceneNodeTrans, SetTranslateRecomputesModelTrans) {
    owe::SceneNode parent;
    parent.SetTranslate({ 100.0f, 200.0f, 0.0f });
    auto child = rstd::sync::Arc<owe::SceneNode>::make();
    child->SetTranslate({ 10.0f, 20.0f, 0.0f });
    parent.AppendChild(child.clone());

    child->UpdateTrans();
    Eigen::Matrix4d m1 = child->ModelTrans();
    EXPECT_DOUBLE_EQ(m1(0, 3), 110.0); // world x
    EXPECT_DOUBLE_EQ(m1(1, 3), 220.0); // world y

    // Mutate the parent and re-read the child without explicit dirty.
    parent.SetTranslate({ 500.0f, 600.0f, 0.0f });
    child->UpdateTrans();
    Eigen::Matrix4d m2 = child->ModelTrans();
    EXPECT_DOUBLE_EQ(m2(0, 3), 510.0);
    EXPECT_DOUBLE_EQ(m2(1, 3), 620.0);
}

TEST(SceneNodeTrans, SetScaleAndRotationMarkDirty) {
    owe::SceneNode n;
    n.UpdateTrans();
    // After first UpdateTrans the cache is clean.
    n.SetScale({ 2.0f, 2.0f, 1.0f });
    n.UpdateTrans();
    Eigen::Matrix4d m = n.ModelTrans();
    EXPECT_DOUBLE_EQ(m(0, 0), 2.0);
    EXPECT_DOUBLE_EQ(m(1, 1), 2.0);
}

TEST(ScriptNodeSize, UnsetFallsBackTo100x100) {
    owe::SceneNode node; // m_size defaults to (0,0)
    JsRuntime      rt;
    FrameInputs    fi {};
    rt.SetFrameInputs(fi);
    auto* fs = rt.MakeFieldScript(
        R"JS(
            export function update() { return thisLayer.size.x + thisLayer.size.y * 1000; }
        )JS"_str,
        "test/node_size_unset"_str,
        FieldKind::Scalar,
        owe::MakeObject(),
        owe::IntoJson(0),
        &node);
    ASSERT_NE(fs, nullptr);

    rt.TickAll();
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 100.0 + 100.0 * 1000);
}

TEST(ScriptTimer, ClearIntervalStops) {
    JsRuntime   rt;
    FrameInputs fi {};
    rt.SetFrameInputs(fi);
    auto* fs = MakeProbe(rt,
                         "test/clear_interval",
                         R"JS(
        let n = 0;
        let h = setInterval(() => { n++; }, 100);
        export function update() {
            if (n >= 2) clearInterval(h);
            return n;
        }
    )JS");
    ASSERT_NE(fs, nullptr);

    Tick(rt, 0.25); // fires at 0.1, 0.2 → n=2
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 2.0);

    Tick(rt, 1.50); // would have fired many more, but update cleared it
    EXPECT_EQ(fs->last_value().as_Scalar().value.v, 2.0);
}

TEST(ScriptActuator, OwnsMoveOnlyMutableCapture) {
    auto     owner = Box<int>::make(0);
    auto*    count = owner.get();
    Actuator actuator(nullptr, [owner = rstd::move(owner)](const ScriptValue&) mutable {
        ++*owner;
    });
    actuator.apply->operator()(ScriptValue::Empty());
    actuator.apply->operator()(ScriptValue::Empty());
    EXPECT_EQ(*count, 2);
}

TEST(ScriptEnumeration, DefersScriptsAddedDuringEnumeration) {
    JsRuntime runtime;
    ASSERT_NE(MakeProbe(runtime, "enumeration/first", "export function update() { return 1; }"),
              nullptr);
    struct State {
        JsRuntime* runtime;
        int        count;
    };
    State state { &runtime, 0 };
    runtime.ForEachScript(
        [](FieldScript*, void* opaque) {
            auto& state = *static_cast<State*>(opaque);
            ++state.count;
            for (int i = 0; i < 64; ++i) {
                auto key = std::to_string(i);
                EXPECT_NE(MakeProbe(*state.runtime,
                                    key.c_str(),
                                    "export function update() { return 2; }"),
                          nullptr);
            }
        },
        &state);
    EXPECT_EQ(state.count, 1);
    state.count = 0;
    runtime.ForEachScript(
        [](FieldScript*, void* opaque) {
            ++static_cast<State*>(opaque)->count;
        },
        &state);
    EXPECT_EQ(state.count, 65);
}

TEST(ScriptSource, AcceptsBorrowedSubstringWithoutTerminator) {
    JsRuntime runtime;
    auto      storage = "export function update() { return 42; }trailing invalid source"_Str;
    auto      source =
        storage.as_str().get(usize(), "export function update() { return 42; }"_str.len()).unwrap();
    auto* script = runtime.MakeFieldScript(
        source, "bounded-source"_str, FieldKind::Scalar, owe::MakeObject(), owe::IntoJson(0));
    ASSERT_NE(script, nullptr);
    runtime.TickAll();
    ASSERT_TRUE(script->last_value().is_Scalar());
    EXPECT_EQ(script->last_value().as_Scalar().value.v, 42.0);
}
