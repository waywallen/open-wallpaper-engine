#include <rstd/test/gtest.hpp>

#include <cmath>

import eigen;
import rstd;
import rstd.cppstd;
import wavsen.audio;
import wescene.fs;
import wescene.json;
import wescene.pkg.parse;
import wescene.scene;
import wescene.script;
import wescene.text;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::cppstd::to_string;
using rstd::sync::Arc;

namespace scene_test
{

class UniformSink {
public:
    explicit UniformSink(owe::UniformOutputId output): m_output(output) {}

    bool Wants(owe::UniformOutputId output) const { return output == m_output; }

    auto Write(owe::UniformOutputId output, owe::UniformValueView value)
        -> rstd::Result<rstd::empty, owe::UniformError> {
        if (! Wants(output)) {
            return rstd::Err(owe::UniformError {
                .message = rstd::string::String::make("unexpected uniform output"_str),
            });
        }
        m_value   = owe::UniformValue(value);
        m_written = true;
        return rstd::Ok(rstd::empty {});
    }

    const owe::UniformValue& Value() const { return m_value; }
    bool                     Written() const { return m_written; }

private:
    owe::UniformOutputId m_output;
    owe::UniformValue    m_value;
    bool                 m_written { false };
};

class EmptyResources {
public:
    auto Texture(rstd::usize) const -> rstd::Option<owe::UniformTextureView> {
        return rstd::None();
    }
    auto Viewport() const -> rstd::array<float, 2> { return { 1920.0f, 1080.0f }; }
    auto TexelSize() const -> rstd::array<float, 2> { return { 1.0f / 1920.0f, 1.0f / 1080.0f }; }
};

class StaticTextureResources {
public:
    auto Texture(rstd::usize index) const -> rstd::Option<owe::UniformTextureView> {
        if (index != rstd::usize()) return rstd::None();
        return rstd::Some(owe::UniformTextureView {
            .has_extent    = true,
            .source_extent = { 512.0f, 512.0f },
            .sample_extent = { 512.0f, 512.0f },
        });
    }
    auto Viewport() const -> rstd::array<float, 2> { return { 1920.0f, 1080.0f }; }
    auto TexelSize() const -> rstd::array<float, 2> { return { 1.0f / 1920.0f, 1.0f / 1080.0f }; }
};

class ShapeSink {
public:
    auto Bind(owe::UniformOutputId, ref<str> name, owe::UniformValueShape shape)
        -> rstd::Result<bool, owe::UniformError> {
        if (name == "g_ModelMatrix"_str) {
            model_shape = shape;
            found_model = true;
        } else if (name == "g_AudioSpectrum16Left"_str) {
            spectrum_shape = shape;
            found_spectrum = true;
        } else if (name == "g_LightsPosition"_str) {
            light_position_shape = shape;
            found_light_position = true;
        }
        return rstd::Ok(true);
    }

    owe::UniformValueShape model_shape;
    owe::UniformValueShape spectrum_shape;
    owe::UniformValueShape light_position_shape;
    bool                   found_model { false };
    bool                   found_spectrum { false };
    bool                   found_light_position { false };
};

class UpdateContext {
public:
    template<typename Resources>
    UpdateContext(const owe::SceneFrame& frame, const Resources& resources)
        : m_frame(rstd::ref<owe::SceneFrame>::from_raw_parts(rstd::addressof(frame))),
          m_resources(rstd::dyn<owe::UniformResourceView>::from_ref(resources)) {}

    auto Frame() const -> rstd::ref<owe::SceneFrame> { return m_frame; }
    auto Resources() const -> rstd::ref<rstd::dyn<owe::UniformResourceView>> { return m_resources; }
    auto RenderView() const -> owe::SceneRenderViewKind {
        return owe::SceneRenderViewKind::Primary;
    }

private:
    rstd::ref<owe::SceneFrame>                     m_frame;
    rstd::ref<rstd::dyn<owe::UniformResourceView>> m_resources;
};

template<typename Source, typename Output>
auto Capture(const owe::SceneFrame& frame, const Source& source, Output output)
    -> owe::UniformValue {
    EmptyResources resources;
    UpdateContext  context_impl(frame, resources);
    UniformSink    sink_impl(owe::ToUniformOutput(output));
    auto           context = rstd::dyn<owe::UniformUpdateContext>::from_ref(context_impl);
    auto           sink    = rstd::dyn<owe::UniformValueSink>::from_ref(sink_impl);
    auto           result  = source.Evaluate(context.as_ref(), sink.as_mut_ref());
    EXPECT_TRUE(result.is_ok());
    EXPECT_TRUE(sink_impl.Written());
    return sink_impl.Value();
}

} // namespace scene_test

TEST(TransformUniformSource, DescribesModelAsMat4) {
    auto state = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(1.0, 1.0, -1.0, 1.0));
    auto resolver   = Arc<owe::UniformCameraResolver>::make(rstd::move(camera));
    auto scene_node = Arc<owe::SceneNode>::make();
    auto node = Arc<owe::UniformNodeState>::make(rstd::move(scene_node), rstd::move(resolver));
    owe::TransformUniformSource source(rstd::move(state), rstd::move(node));
    scene_test::ShapeSink       sink_impl;
    auto                        sink = rstd::dyn<owe::UniformBindingSink>::from_ref(sink_impl);

    auto result = source.Describe(sink.as_mut_ref());

    ASSERT_TRUE(result.is_ok());
    ASSERT_TRUE(sink_impl.found_model);
    EXPECT_EQ(sink_impl.model_shape.kind, owe::UniformValueKind::Matrix);
    EXPECT_EQ(sink_impl.model_shape.rows, rstd::u32(4));
    EXPECT_EQ(sink_impl.model_shape.columns, rstd::u32(4));
}

TEST(TransformUniformSource, AppliesGeometryTransformAfterNodeTransform) {
    auto state = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(1.0, 1.0, -1.0, 1.0));
    auto resolver = Arc<owe::UniformCameraResolver>::make(rstd::move(camera));
    auto node     = Arc<owe::SceneNode>::make();
    node->SetTranslate({ 100.0f, 200.0f, 0.0f });
    node->SetScale({ 2.0f, 3.0f, 1.0f });
    auto mesh = std::make_shared<owe::SceneMesh>();
    mesh->SetGeometryTransform(
        Eigen::Affine3d(Eigen::Translation3d(Eigen::Vector3d(10.0, -20.0, 0.0))).matrix());
    node->AddMesh(mesh);
    auto node_state = Arc<owe::UniformNodeState>::make(node.clone(), rstd::move(resolver));
    owe::TransformUniformSource source(state.clone(), rstd::move(node_state));

    auto model =
        scene_test::Capture(owe::SceneFrame {}, source, owe::TransformUniformOutput::Model);

    ASSERT_GT(model.size().to_primitive(), 13u);
    EXPECT_NEAR(model[usize(12)], 120.0f, 1e-5f);
    EXPECT_NEAR(model[usize(13)], 140.0f, 1e-5f);
}

TEST(TransformUniformSource, NormalModelMatrixDoesNotScaleTangentSpace) {
    auto state = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(1.0, 1.0, -1.0, 1.0));
    auto resolver = Arc<owe::UniformCameraResolver>::make(rstd::move(camera));
    auto node     = Arc<owe::SceneNode>::make();
    node->SetScale({ 0.00636f, 0.00636f, 0.00636f });
    auto node_state = Arc<owe::UniformNodeState>::make(node.clone(), rstd::move(resolver));
    owe::TransformUniformSource source(state.clone(), rstd::move(node_state));

    auto normal_model =
        scene_test::Capture(owe::SceneFrame {}, source, owe::TransformUniformOutput::NormalModel);

    ASSERT_EQ(normal_model.size(), usize(9));
    EXPECT_NEAR(normal_model[usize()], 1.0f, 1e-5f);
    EXPECT_NEAR(normal_model[usize(4)], 1.0f, 1e-5f);
    EXPECT_NEAR(normal_model[usize(8)], 1.0f, 1e-5f);
}

TEST(TransformUniformSource, UsesResolvedPerspectiveCameraEyePosition) {
    auto state  = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    auto camera = Arc<owe::SceneCamera>::make(
        owe::SceneCamera::MakePerspective(16.0 / 9.0, 0.1, 10000.0, 60.0));
    camera->SetLookAt(
        Eigen::Vector3d { -0.25, 0.5, 3.25 }, Eigen::Vector3d::Zero(), Eigen::Vector3d::UnitY());
    auto resolver   = Arc<owe::UniformCameraResolver>::make(rstd::move(camera));
    auto scene_node = Arc<owe::SceneNode>::make();
    auto node = Arc<owe::UniformNodeState>::make(rstd::move(scene_node), rstd::move(resolver));
    owe::TransformUniformSource source(rstd::move(state), rstd::move(node));

    auto eye =
        scene_test::Capture(owe::SceneFrame {}, source, owe::TransformUniformOutput::EyePosition);

    ASSERT_EQ(eye.size(), usize(3));
    EXPECT_FLOAT_EQ(eye[usize()], -0.25f);
    EXPECT_FLOAT_EQ(eye[usize(1)], 0.5f);
    EXPECT_FLOAT_EQ(eye[usize(2)], 3.25f);
}

TEST(TransformUniformSource, UsesConfiguredEyePositionBeforePerspectiveCamera) {
    auto state  = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    auto camera = Arc<owe::SceneCamera>::make(
        owe::SceneCamera::MakePerspective(16.0 / 9.0, 0.1, 10000.0, 60.0));
    camera->SetLookAt(
        Eigen::Vector3d { 1.0, 2.0, 3.0 }, Eigen::Vector3d::Zero(), Eigen::Vector3d::UnitY());
    auto resolver   = Arc<owe::UniformCameraResolver>::make(rstd::move(camera));
    auto scene_node = Arc<owe::SceneNode>::make();
    auto node = Arc<owe::UniformNodeState>::make(rstd::move(scene_node), rstd::move(resolver));
    node->eye_position_override = Some(array<float, 3> { 4.0f, 5.0f, 6.0f });
    owe::TransformUniformSource source(rstd::move(state), rstd::move(node));

    auto eye =
        scene_test::Capture(owe::SceneFrame {}, source, owe::TransformUniformOutput::EyePosition);

    ASSERT_EQ(eye.size(), usize(3));
    EXPECT_FLOAT_EQ(eye[usize()], 4.0f);
    EXPECT_FLOAT_EQ(eye[usize(1)], 5.0f);
    EXPECT_FLOAT_EQ(eye[usize(2)], 6.0f);
}

TEST(AudioUniformSource, ExposesLogicalSpectrumValues) {
    auto state = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    owe::scene_audio::Buffers buffers {};
    for (usize band {}; band < usize(16); ++band) {
        buffers.bands16.left[band] = static_cast<float>(band.to_primitive() + 1);
    }
    state->SetAudioSpectrum(buffers);
    owe::AudioUniformSource source(state.clone());

    scene_test::ShapeSink shape_sink_impl;
    auto shape_sink = rstd::dyn<owe::UniformBindingSink>::from_ref(shape_sink_impl);
    auto described  = source.Describe(shape_sink.as_mut_ref());

    ASSERT_TRUE(described.is_ok());
    ASSERT_TRUE(shape_sink_impl.found_spectrum);
    EXPECT_EQ(shape_sink_impl.spectrum_shape.kind, owe::UniformValueKind::Linear);
    EXPECT_EQ(shape_sink_impl.spectrum_shape.min_elements, rstd::u32(16));
    EXPECT_EQ(shape_sink_impl.spectrum_shape.max_elements, rstd::u32(16));

    owe::SceneFrame frame;
    auto value = scene_test::Capture(frame, source, owe::AudioUniformOutput::Spectrum16Left);
    ASSERT_EQ(value.size(), usize(16));
    EXPECT_EQ(value.View().layout.kind, owe::UniformValueKind::Linear);
    for (usize band {}; band < usize(16); ++band) {
        EXPECT_FLOAT_EQ(value[band], static_cast<float>(band.to_primitive() + 1));
    }
}

TEST(LightUniformSource, ExposesLogicalVec3Array) {
    auto                    lights = Vec<ref<owe::SceneLight>>::make();
    owe::LightUniformSource source(rstd::move(lights));
    scene_test::ShapeSink   shape_sink_impl;
    auto shape_sink = rstd::dyn<owe::UniformBindingSink>::from_ref(shape_sink_impl);
    auto described  = source.Describe(shape_sink.as_mut_ref());

    ASSERT_TRUE(described.is_ok());
    ASSERT_TRUE(shape_sink_impl.found_light_position);
    EXPECT_EQ(shape_sink_impl.light_position_shape.kind, owe::UniformValueKind::Linear);
    EXPECT_EQ(shape_sink_impl.light_position_shape.min_elements, rstd::u32(12));
    EXPECT_EQ(shape_sink_impl.light_position_shape.max_elements, rstd::u32(12));

    owe::SceneFrame frame;
    auto            value = scene_test::Capture(frame, source, owe::LightUniformOutput::Position);
    ASSERT_EQ(value.size(), usize(12));
    for (usize index {}; index < value.size(); ++index) EXPECT_FLOAT_EQ(value[index], 0.0f);
}

TEST(LightUniformSource, PublishesWorldDirectionAndType) {
    auto parent = Arc<owe::SceneNode>::make();
    parent->SetTranslate({ 10.0f, 20.0f, 30.0f });
    parent->SetRotation({ 0.0f, 0.0f, -0.25f });
    auto node = Arc<owe::SceneNode>::make();
    node->SetTranslate({ 2.0f, 0.0f, 0.0f });
    parent->AppendChild(node.clone());

    owe::SceneLight::Desc desc;
    desc.type        = owe::SceneLightType::Directional;
    desc.color       = { 0.25f, 0.5f, 1.0f };
    desc.intensity   = 2.0f;
    desc.cast_shadow = true;
    owe::SceneLight light(desc);
    light.setNode(node.as_ptr());

    auto lights = Vec<ref<owe::SceneLight>>::make();
    lights.push(ref<owe::SceneLight>::from_raw_parts(rstd::addressof(light)));
    owe::LightUniformSource source(rstd::move(lights));
    owe::SceneFrame         frame;

    auto position = scene_test::Capture(frame, source, owe::LightUniformOutput::Position);
    ASSERT_EQ(position.size(), usize(12));
    EXPECT_NEAR(position[usize()], 10.0f + 2.0f * std::cos(0.25f), 1e-5f);
    EXPECT_NEAR(position[usize(1)], 20.0f - 2.0f * std::sin(0.25f), 1e-5f);
    EXPECT_NEAR(position[usize(2)], 30.0f, 1e-5f);

    auto direction = scene_test::Capture(frame, source, owe::LightUniformOutput::DirectionType);
    ASSERT_EQ(direction.size(), usize(16));
    EXPECT_NEAR(direction[usize()], -std::cos(0.25f), 1e-5f);
    EXPECT_NEAR(direction[usize(1)], std::sin(0.25f), 1e-5f);
    EXPECT_NEAR(direction[usize(2)], 0.0f, 1e-5f);
    EXPECT_FLOAT_EQ(direction[usize(3)], static_cast<float>(owe::SceneLightType::Directional));

    auto color = scene_test::Capture(frame, source, owe::LightUniformOutput::ColorRadius);
    ASSERT_EQ(color.size(), usize(16));
    EXPECT_FLOAT_EQ(color[usize()], 0.5f);
    EXPECT_FLOAT_EQ(color[usize(1)], 1.0f);
    EXPECT_FLOAT_EQ(color[usize(2)], 2.0f);

    auto cast_shadow = scene_test::Capture(frame, source, owe::LightUniformOutput::CastShadow);
    ASSERT_EQ(cast_shadow.size(), usize(4));
    EXPECT_FLOAT_EQ(cast_shadow[usize()], 1.0f);
}

TEST(ShadowUniformSource, UsesAuthoredCascadeExtentAndLightObjectFrame) {
    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakePerspective(1.0, 0.1, 1000.0, 50.0));
    camera->SetLookAt(Eigen::Vector3d::Zero(), -Eigen::Vector3d::UnitZ(), Eigen::Vector3d::UnitY());

    auto                  light_node = Arc<owe::SceneNode>::make();
    owe::SceneLight::Desc desc;
    desc.type                 = owe::SceneLightType::Directional;
    desc.cast_shadow          = true;
    desc.cascade_distances[0] = 10.0f;
    desc.cascade_distances[1] = 20.0f;
    desc.cascade_distances[2] = 40.0f;
    owe::SceneLight light(desc);
    light.setNode(light_node.as_ptr());
    owe::ShadowUniformSource source(rstd::move(camera),
                                    ref<owe::SceneLight>::from_raw_parts(rstd::addressof(light)));

    owe::SceneFrame frame;
    auto            matrices =
        scene_test::Capture(frame, source, owe::ShadowUniformOutput::ViewProjectionMatrices);
    ASSERT_EQ(matrices.size(), usize(96));
    EXPECT_NEAR(matrices[usize(8)], 0.2f, 1e-6f);
    EXPECT_NEAR(matrices[usize(5)], 0.2f, 1e-6f);
    EXPECT_NEAR(matrices[usize(2)], -1.0f / 80.0f, 1e-6f);
    EXPECT_NEAR(matrices[usize(12)], 1.0f, 1e-6f);
    EXPECT_NEAR(matrices[usize(14)], 0.5f, 1e-6f);
    EXPECT_NEAR(matrices[usize(16 + 8)], 0.1f, 1e-6f);
    EXPECT_NEAR(matrices[usize(32 + 8)], 0.05f, 1e-6f);
    EXPECT_FLOAT_EQ(matrices[usize(48)], 0.0f);

    auto atlas = scene_test::Capture(frame, source, owe::ShadowUniformOutput::AtlasTransforms);
    ASSERT_EQ(atlas.size(), usize(12));
    EXPECT_FLOAT_EQ(atlas[usize()], 0.0f);
    EXPECT_FLOAT_EQ(atlas[usize(2)], 1.0f / 3.0f);
    EXPECT_FLOAT_EQ(atlas[usize(4)], 1.0f / 3.0f);
    EXPECT_FLOAT_EQ(atlas[usize(8)], 2.0f / 3.0f);
}

TEST(TextureUniformSource, StaticTextureUsesIdentityTransform) {
    owe::SceneFrame                    frame;
    scene_test::StaticTextureResources resources;
    scene_test::UpdateContext          context_impl(frame, resources);
    owe::TextureUniformSource          source;
    scene_test::UniformSink            sink_impl(owe::TextureRotationOutput(0));
    auto context = rstd::dyn<owe::UniformUpdateContext>::from_ref(context_impl);
    auto sink    = rstd::dyn<owe::UniformValueSink>::from_ref(sink_impl);

    auto result = source.Evaluate(context.as_ref(), sink.as_mut_ref());

    ASSERT_TRUE(result.is_ok());
    ASSERT_TRUE(sink_impl.Written());
    const auto& rotation = sink_impl.Value();
    ASSERT_EQ(rotation.size(), usize(4));
    EXPECT_FLOAT_EQ(rotation[usize()], 1.0f);
    EXPECT_FLOAT_EQ(rotation[usize(1)], 0.0f);
    EXPECT_FLOAT_EQ(rotation[usize(2)], 0.0f);
    EXPECT_FLOAT_EQ(rotation[usize(3)], 1.0f);
}

TEST(TextureUniformSource, PublishesSampleTexelExtent) {
    owe::SceneFrame                    frame;
    scene_test::StaticTextureResources resources;
    scene_test::UpdateContext          context_impl(frame, resources);
    owe::TextureUniformSource          source;
    scene_test::UniformSink            sink_impl(owe::TextureTexelOutput(0));
    auto context = rstd::dyn<owe::UniformUpdateContext>::from_ref(context_impl);
    auto sink    = rstd::dyn<owe::UniformValueSink>::from_ref(sink_impl);

    auto result = source.Evaluate(context.as_ref(), sink.as_mut_ref());

    ASSERT_TRUE(result.is_ok());
    ASSERT_TRUE(sink_impl.Written());
    const auto& texel = sink_impl.Value();
    ASSERT_EQ(texel.size(), usize(4));
    EXPECT_FLOAT_EQ(texel[usize()], 1.0f / 512.0f);
    EXPECT_FLOAT_EQ(texel[usize(1)], 1.0f / 512.0f);
    EXPECT_FLOAT_EQ(texel[usize(2)], 512.0f);
    EXPECT_FLOAT_EQ(texel[usize(3)], 512.0f);
}

TEST(AudioResponseDemand, AggregatesLeasesAndHonorsRuntimeGate) {
    owe::AudioResponseDemand demand;
    std::vector<bool>        changes;
    demand.SetCallback([&changes](bool active) {
        changes.push_back(active);
    });
    ASSERT_EQ(changes, (std::vector<bool> { false }));

    auto first  = rstd::Some(demand.Acquire());
    auto second = rstd::Some(demand.Acquire());
    EXPECT_TRUE(demand.Active());
    EXPECT_EQ(changes, (std::vector<bool> { false, true }));
    first = rstd::None();
    EXPECT_TRUE(demand.Active());
    second = rstd::None();
    EXPECT_FALSE(demand.Active());
    EXPECT_EQ(changes, (std::vector<bool> { false, true, false }));

    auto gated = rstd::Some(demand.Acquire());
    demand.SetEnabled(false);
    demand.SetEnabled(true);
    gated = rstd::None();
    EXPECT_EQ(changes, (std::vector<bool> { false, true, false, true, false, true, false }));
}

TEST(SceneAudioAverage, SharesAtomicStateWithStreamOwner) {
    owe::Scene scene;
    auto       stream_owner = scene.AudioAverageHandle();

    stream_owner->Store(usize(3), f32(0.75f));

    EXPECT_FLOAT_EQ(scene.AudioAverage(usize(3)).to_primitive(), 0.75f);
}

TEST(SceneParserSoundScript, UserPropertyCanStartSilentSoundFromVolumeField) {
    auto document = owe::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {},
            "objects": [
                {
                    "id": 1,
                    "name": "BGM Controller",
                    "sound": ["sounds/controller.mp3"],
                    "startsilent": true,
                    "volume": {
                        "value": 0.7,
                        "script": "export function applyUserProperties(properties) { if (properties.song_selection) thisScene.getLayer('BGM Target').play(); } export function update(value) { return value * 0.5; }"
                    }
                },
                {
                    "id": 2,
                    "name": "BGM Target",
                    "sound": ["sounds/target.mp3"],
                    "startsilent": true,
                    "volume": 0.7
                }
            ]
        })JSON",
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());

    owe::fs::VFS                vfs;
    wavsen::audio::SoundManager sound_manager;
    owe::SceneParser            parser;
    auto                        parsed = parser.Parse(
        "sound-script"_str,
        ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
        mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound_manager)));
    ASSERT_TRUE(parsed.is_ok());

    auto scene      = rstd::move(parsed).unwrap();
    auto controller = scene.scene->RootMut()->FindByName("BGM Controller");
    auto target     = scene.scene->RootMut()->FindByName("BGM Target");
    ASSERT_NE(controller, nullptr);
    ASSERT_NE(target, nullptr);
    EXPECT_FALSE(target->IsPlaying());

    auto property = rstd::json::from_str(R"({"type":"combo","value":"0"})"_str).unwrap();
    owe::script::SetSceneUserProperty(*scene.scene, "song_selection", property);
    EXPECT_TRUE(target->IsPlaying());

    owe::script::TickSceneScripts(*scene.scene, owe::script::FrameInputs {});
    EXPECT_FLOAT_EQ(controller->Volume(), 0.35f);
}

TEST(SceneParserScript, DynamicObjectsUseSceneIdentity) {
    auto document = owe::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {},
            "objects": [
                {
                    "id": 90,
                    "name": "Controller",
                    "visible": {
                        "value": true,
                        "script": "let created; export function init(value) { created = thisScene.createLayer({size: '2 2'}); created.visible = false; return value; } export function update(value) { return value; }"
                    }
                },
                {"id": 7, "name": "Low"}
            ]
        })JSON",
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());

    auto assets = owe::fs::make_physical_fs(owe::fs::ToPath(WAYWALLEN_ASSETS_DIR));
    ASSERT_TRUE(assets.is_ok());
    owe::fs::VFS vfs;
    ASSERT_TRUE(vfs.mount("/assets"_str, rstd::move(assets).unwrap_unchecked()).is_ok());

    wavsen::audio::SoundManager sound_manager;
    owe::SceneParser            parser;
    auto                        parsed = parser.Parse(
        "dynamic-object-id"_str,
        ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
        mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound_manager)));
    ASSERT_TRUE(parsed.is_ok());

    auto scene      = rstd::move(parsed).unwrap();
    auto controller = scene.scene->RootMut()->FindByName("Controller");
    auto dynamic    = scene.scene->RootMut()->FindByName("__createLayer");
    ASSERT_NE(controller, nullptr);
    ASSERT_NE(dynamic, nullptr);
    EXPECT_TRUE(dynamic->Identity().Valid());
    EXPECT_NE(dynamic->Identity().index, controller->Identity().index);
    EXPECT_TRUE(dynamic->WallpaperIdentity().is_none());
    EXPECT_EQ(dynamic->ID(), i32(-1));
    EXPECT_FALSE(dynamic->Visible());
    EXPECT_TRUE(scene.scene->ConsumeRenderGraphDirty());

    EXPECT_TRUE(scene.scene->SetNodeVisible(*dynamic, true));
    EXPECT_TRUE(scene.scene->SetNodeVisible(*dynamic, false));
    EXPECT_FALSE(scene.scene->ConsumeRenderGraphDirty());

    EXPECT_TRUE(scene.scene->SetNodeVisible(*dynamic, true));
    EXPECT_TRUE(scene.scene->ConsumeRenderGraphDirty());
}

TEST(SceneParserText, EmptyStaticTextPreservesLayerHierarchy) {
    auto document = owe::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {},
            "objects": [
                {
                    "id": 1,
                    "name": "Empty Text Parent",
                    "text": "",
                    "origin": [100, 200, 0],
                    "scale": [2, 3, 1],
                    "size": [400, 300]
                },
                {
                    "id": 2,
                    "name": "Authored Child",
                    "parent": 1,
                    "origin": [10, 20, 0]
                }
            ]
        })JSON",
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());

    owe::fs::VFS                vfs;
    wavsen::audio::SoundManager sound_manager;
    owe::SceneParser            parser;
    auto                        parsed = parser.Parse(
        "empty-text-parent"_str,
        ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
        mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound_manager)));
    ASSERT_TRUE(parsed.is_ok());

    auto scene  = rstd::move(parsed).unwrap();
    auto parent = scene.scene->RootMut()->FindByName("Empty Text Parent");
    auto child  = scene.scene->RootMut()->FindByName("Authored Child");
    ASSERT_NE(parent, nullptr);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->Parent(), parent);
}

TEST(SceneParserText, ScriptSceneExposesTextWritesWithoutSourceInspection) {
    auto document = owe::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {},
            "objects": [
                {
                    "id": 1,
                    "name": "Controller",
                    "text": "controller",
                    "font": "systemfont_arial",
                    "visible": {
                        "value": false,
                        "script": "let target; export function init(value) { target = thisScene['get' + 'Layer']('Value'); return value; } export function update(value) { target['te' + 'xt'] = '31'; return value; }"
                    }
                },
                {
                    "id": 2,
                    "name": "Value",
                    "text": "00",
                    "font": "systemfont_arial"
                }
            ]
        })JSON",
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());

    owe::fs::VFS                vfs;
    wavsen::audio::SoundManager sound_manager;
    owe::SceneParser            parser;
    auto                        parsed = parser.Parse(
        "runtime-text-write"_str,
        ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
        mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound_manager)));
    ASSERT_TRUE(parsed.is_ok());

    auto scene = rstd::move(parsed).unwrap();
    auto value = scene.scene->RootMut()->FindByName("Value");
    ASSERT_NE(value, nullptr);
    ASSERT_NE(value->Mesh(), nullptr);
    value->Mesh()->ConsumeDirtyFlags();

    owe::script::TickSceneScripts(*scene.scene, owe::script::FrameInputs {});
    EXPECT_NE(value->Mesh()->DirtyFlags(), owe::SceneMeshDirtyNone);
}

TEST(SceneUserTextBinding, AppliesDescriptorPayloadToMatchingBindings) {
    owe::Scene  scene;
    std::string first;
    std::string second;
    scene.RegisterUserTextBinding(String::make("title"_str),
                                  Box<dyn<FnMut<void(ref<str>)>>>::make([&](ref<str> value) {
                                      first = to_string(value);
                                  }));
    scene.RegisterUserTextBinding(String::make("title"_str),
                                  Box<dyn<FnMut<void(ref<str>)>>>::make([&](ref<str> value) {
                                      second = to_string(value);
                                  }));

    auto property = owe::ParseJson(R"({"type":"textinput","value":"updated"})").unwrap();
    EXPECT_TRUE(scene.ApplyUserTextBindings("title"_str, property));
    EXPECT_EQ(first, "updated");
    EXPECT_EQ(second, "updated");
    EXPECT_FALSE(scene.ApplyUserTextBindings("other"_str, property));
}

TEST(SceneUserTextBinding, AppliesEmptyString) {
    owe::Scene  scene;
    std::string value = "default";
    scene.RegisterUserTextBinding(String::make("title"_str),
                                  Box<dyn<FnMut<void(ref<str>)>>>::make([&](ref<str> next) {
                                      value = to_string(next);
                                  }));

    auto property = owe::ParseJson(R"({"type":"textinput","value":""})").unwrap();
    EXPECT_TRUE(scene.ApplyUserTextBindings("title"_str, property));
    EXPECT_TRUE(value.empty());
}

TEST(SceneUserPropertyBinding, AppliesJsonPayloadToOwnedCallback) {
    owe::Scene scene;
    bool       called = false;
    scene.RegisterUserPropertyBinding(
        String::make("camera"_str),
        Box<dyn<FnMut<void(ref<owe::Json>)>>>::make([&](ref<owe::Json> property) {
            called = property->is_object();
        }));

    auto property = owe::ParseJson(R"({"value":true})").unwrap();
    EXPECT_TRUE(scene.ApplyUserPropertyBindings("camera"_str, property));
    EXPECT_TRUE(called);
    EXPECT_FALSE(scene.ApplyUserPropertyBindings("other"_str, property));
}

TEST(SceneTransformUpdater, ReceivesRuntimeElapsedTime) {
    owe::Scene scene;
    f64        observed;
    scene.RegisterTransformUpdater(Box<dyn<FnMut<void(f64)>>>::make([&](f64 elapsed) {
        observed = elapsed;
    }));

    scene.PassFrameTime(0.25);
    scene.TickTransformUpdaters();
    EXPECT_DOUBLE_EQ(observed.to_primitive(), 0.25);
}

TEST(TextUniformSource, OwnsTextProjectionOutputs) {
    owe::Scene scene;
    auto       node = Arc<owe::SceneNode>::make();
    auto       camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(1920, 1080, -1.0, 1.0));
    auto state    = std::make_shared<owe::text::TextUniformState>(node.clone());
    state->camera = Some(camera.clone());

    owe::text::TextUniformSource source(state);
    auto                         value = scene_test::Capture(
        scene.Runtime().Frame(), source, owe::text::TextUniformOutput::ModelViewProjection);

    EXPECT_EQ(value.size().to_primitive(), 16u);
}

TEST(SceneCameraProjection, UsesExplicitProjectionFactories) {
    auto orthographic = owe::SceneCamera::MakeOrthographic(1920.5, 1080.25, -1.0, 1.0);
    EXPECT_FALSE(orthographic.IsPerspective());
    EXPECT_DOUBLE_EQ(orthographic.Width(), 1920.5);
    EXPECT_DOUBLE_EQ(orthographic.Height(), 1080.25);

    auto perspective = owe::SceneCamera::MakePerspective(16.0 / 9.0, 0.01, 1000.0, 45.0);
    EXPECT_TRUE(perspective.IsPerspective());
    EXPECT_DOUBLE_EQ(perspective.Aspect(), 16.0 / 9.0);
    EXPECT_DOUBLE_EQ(perspective.Fov(), 45.0);
}

TEST(SceneCameraProjection, AppliesViewportScaleToOrthographicExtent) {
    owe::Scene scene;
    scene.SetOrtho({ i32(1920), i32(1080) });
    scene.SetViewportScale(f32(1.5f));

    auto extent = scene.OrthographicProjectionExtent();
    EXPECT_DOUBLE_EQ(extent[usize()], 1280.0);
    EXPECT_DOUBLE_EQ(extent[usize(1)], 720.0);

    scene.SetViewportScale(f32());
    extent = scene.OrthographicProjectionExtent();
    EXPECT_DOUBLE_EQ(extent[usize()], 1920.0);
    EXPECT_DOUBLE_EQ(extent[usize(1)], 1080.0);
}

TEST(SceneCameraPath, UserBindingMutatesRegisteredArc) {
    owe::Scene scene;
    auto       path                = Arc<owe::SceneCameraPath>::make();
    path->visible_user_binding.key = String::make("camera-path"_str);
    scene.RegisterCameraPath(path.clone());
    scene.RegisterCameraPathUserBinding(String::make("camera-path"_str), path.clone());

    auto disabled = owe::ParseJson(R"({"value":false})").unwrap();
    EXPECT_TRUE(scene.ApplyUserCameraPathVisibilityBindings("camera-path", disabled));
    EXPECT_FALSE(path->enabled);

    auto enabled = owe::ParseJson(R"({"value":true})").unwrap();
    EXPECT_TRUE(scene.ApplyUserCameraPathVisibilityBindings("camera-path", enabled));
    EXPECT_TRUE(path->enabled);
}

TEST(UniformSourceRuntimeAlpha, Color4UsesBaseColorAndRuntimeAlpha) {
    owe::Scene scene;
    auto       node = Arc<owe::SceneNode>::make();
    node->SetBaseColor({ 0.25f, 0.5f, 0.75f }, 0.8f);
    node->SetUserAlpha(0.125f);

    owe::ColorUniformSource source(node.clone());
    const auto              color =
        scene_test::Capture(scene.Runtime().Frame(), source, owe::ColorUniformOutput::Color4);
    ASSERT_EQ(color.size().to_primitive(), 4u);
    EXPECT_FLOAT_EQ(color[rstd::usize()], 0.25f);
    EXPECT_FLOAT_EQ(color[rstd::usize(1)], 0.5f);
    EXPECT_FLOAT_EQ(color[rstd::usize(2)], 0.75f);
    EXPECT_FLOAT_EQ(color[rstd::usize(3)], 0.125f);
}

TEST(UniformSourceRuntimeAlpha, VisibleTrueRestoresLayerAlpha) {
    owe::Scene scene;
    auto       node = Arc<owe::SceneNode>::make();
    node->SetBaseColor({ 0.0f, 0.0f, 0.0f }, 0.35f);
    owe::ColorUniformSource source(node.clone());

    node->SetVisible(true);
    auto visible =
        scene_test::Capture(scene.Runtime().Frame(), source, owe::ColorUniformOutput::Color4);
    ASSERT_EQ(visible.size().to_primitive(), 4u);
    EXPECT_FLOAT_EQ(visible[rstd::usize(3)], 0.35f);

    node->SetVisible(false);
    auto hidden =
        scene_test::Capture(scene.Runtime().Frame(), source, owe::ColorUniformOutput::Color4);
    ASSERT_EQ(hidden.size().to_primitive(), 4u);
    EXPECT_FLOAT_EQ(hidden[rstd::usize(3)], 0.0f);

    node->SetVisible(true);
    auto restored =
        scene_test::Capture(scene.Runtime().Frame(), source, owe::ColorUniformOutput::Color4);
    ASSERT_EQ(restored.size().to_primitive(), 4u);
    EXPECT_FLOAT_EQ(restored[rstd::usize(3)], 0.35f);
}

TEST(UniformSourceParallax, UserPropertiesDriveEveryParallaxField) {
    auto state = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    state->CameraParallax() = { true, 0.03f, 0.1f, 0.36f };

    auto disable = owe::ParseJson(R"({"value":false})").unwrap();
    state->ApplyUserProperty("cameraparallax", disable);
    EXPECT_FALSE(state->CameraParallax().enable);

    auto enable = owe::ParseJson(R"({"value":true})").unwrap();
    state->ApplyUserProperty("cameraparallax", enable);
    EXPECT_TRUE(state->CameraParallax().enable);

    auto amount = owe::ParseJson(R"({"value":0.25})").unwrap();
    state->ApplyUserProperty("cameraparallaxamount", amount);
    EXPECT_FLOAT_EQ(state->CameraParallax().amount, 0.25f);

    auto delay = owe::ParseJson(R"({"value":0.5})").unwrap();
    state->ApplyUserProperty("cameraparallaxdelay", delay);
    EXPECT_FLOAT_EQ(state->CameraParallax().delay, 0.5f);

    auto influence = owe::ParseJson(R"({"value":0.75})").unwrap();
    state->ApplyUserProperty("cameraparallaxmouseinfluence", influence);
    EXPECT_FLOAT_EQ(state->CameraParallax().mouse_influence, 0.75f);
}

TEST(UniformSourceParallax, ParentPropagationSelectsAncestorConfiguration) {
    owe::Scene scene;
    scene.SetOrtho({ i32(3840), i32(2160) });

    auto camera_node = Arc<owe::SceneNode>::make(Eigen::Vector3f { 1920.0f, 1080.0f, 0.0f },
                                                 Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
                                                 Eigen::Vector3f::Zero());
    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(3840, 2160, -1.0, 1.0));
    camera->AttatchNode(camera_node.as_ptr());
    scene.RegisterCamera(String::make("default"_str), camera.clone());
    ASSERT_TRUE(scene.SetActiveCamera("default"_str));

    auto parent = Arc<owe::SceneNode>::make(Eigen::Vector3f { 1982.0f, 1053.0f, 0.0f },
                                            Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
                                            Eigen::Vector3f::Zero());
    auto child  = Arc<owe::SceneNode>::make(Eigen::Vector3f { -76.0f, -3.0f, 0.0f },
                                            Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
                                            Eigen::Vector3f::Zero());
    auto effect = Arc<owe::SceneNode>::make();
    auto mesh   = std::make_shared<owe::SceneMesh>();
    mesh->AddMaterial(owe::SceneMaterial {});
    owe::SceneMesh::Submesh submesh;
    submesh.material_slot = u32();
    mesh->Submeshes().push_back(std::move(submesh));
    child->AddMesh(mesh);
    parent->AppendChild(child.clone());
    scene.RootMut()->AppendChild(parent.clone());
    scene.RebuildResourceIndex();
    effect->SetParentAnchor(child.as_ptr());

    auto state = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    state->SetLayerParallaxPolicy(true, true);
    state->CameraParallax() = { true, 0.03f, 0.0f, 0.36f };
    state->SetOrtho(3840.0f, 2160.0f);
    state->SetPointerInput(0.0, 1.0);
    state->Advance(owe::SceneFrame {});

    auto camera_resolver = Arc<owe::UniformCameraResolver>::make(camera.clone());
    camera_resolver->Add(String::make("default"_str), camera.clone());

    auto parent_state = Arc<owe::UniformNodeState>::make(parent.clone(), camera_resolver.clone());
    parent_state->object_id               = i32(1);
    parent_state->parallax_depth          = { -1.56f, -0.79f };
    parent_state->parallax_depth_authored = true;
    auto child_state = Arc<owe::UniformNodeState>::make(child.clone(), camera_resolver.clone());
    child_state->object_id               = i32(2);
    child_state->parallax_depth          = { 1.0f, 1.0f };
    child_state->parallax_depth_authored = false;
    auto effect_state = Arc<owe::UniformNodeState>::make(effect.clone(), camera_resolver.clone());
    effect_state->object_id              = i32(2);
    effect_state->parallax_depth         = { 1.0f, 1.0f };
    effect_state->parallax_depth_authored = false;
    effect_state->effect_projection_node = Some(child.clone());
    state->SetNodeState({ .index = rstd::u32(1), .generation = rstd::u32(1) },
                        parent_state.clone());
    state->SetNodeState({ .index = rstd::u32(2), .generation = rstd::u32(1) }, child_state.clone());
    state->SetNodeState({ .index = rstd::u32(3), .generation = rstd::u32(1) },
                        effect_state.clone());
    owe::TransformUniformSource source(state.clone(), effect_state.clone());

    auto capture_mvp = [&]() {
        return scene_test::Capture(
            scene.Runtime().Frame(), source, owe::TransformUniformOutput::ModelViewProjection);
    };
    auto expected_translation = [](Eigen::Vector2f base, Eigen::Vector2f depth) {
        const Eigen::Vector2f camera_pos { 1920.0f, 1080.0f };
        const Eigen::Vector2f mouse_vec { 691.2f, 388.8f };
        Eigen::Vector2f       offset = (base - camera_pos + mouse_vec).cwiseProduct(depth) * 0.03f;
        Eigen::Vector2f       final_pos = Eigen::Vector2f { 1906.0f, 1050.0f } + offset;
        return Eigen::Vector2f {
            (final_pos.x() - 1920.0f) / 1920.0f,
            (final_pos.y() - 1080.0f) / 1080.0f,
        };
    };

    auto mvp                = capture_mvp();
    auto expected_inherited = expected_translation({ 1982.0f, 1053.0f }, { -1.56f, -0.79f });
    ASSERT_GT(mvp.size().to_primitive(), 13u);
    EXPECT_NEAR(mvp[rstd::usize(12)], expected_inherited.x(), 1e-5f);
    EXPECT_NEAR(mvp[rstd::usize(13)], expected_inherited.y(), 1e-5f);

    child_state->parallax_depth = { 0.0f, 0.0f };
    mvp                         = capture_mvp();
    EXPECT_NEAR(mvp[rstd::usize(12)], expected_inherited.x(), 1e-5f);
    EXPECT_NEAR(mvp[rstd::usize(13)], expected_inherited.y(), 1e-5f);

    child_state->parallax_depth = { 0.5f, 0.5f };
    mvp                         = capture_mvp();
    EXPECT_NEAR(mvp[rstd::usize(12)], expected_inherited.x(), 1e-5f);
    EXPECT_NEAR(mvp[rstd::usize(13)], expected_inherited.y(), 1e-5f);

    parent_state->parallax_depth = { 0.0f, 0.0f };
    child_state->parallax_depth  = { -0.7f, -0.7f };
    mvp                          = capture_mvp();
    auto expected_frozen         = expected_translation({ 1982.0f, 1053.0f }, { 0.0f, 0.0f });
    EXPECT_NEAR(mvp[rstd::usize(12)], expected_frozen.x(), 1e-5f);
    EXPECT_NEAR(mvp[rstd::usize(13)], expected_frozen.y(), 1e-5f);

    parent_state->parallax_depth  = { 0.321f, 0.321f };
    child_state->parallax_depth   = { 0.321f, 0.321f };
    mvp                           = capture_mvp();
    auto expected_parent_authored = expected_translation({ 1982.0f, 1053.0f }, { 0.321f, 0.321f });
    EXPECT_NEAR(mvp[rstd::usize(12)], expected_parent_authored.x(), 1e-5f);
    EXPECT_NEAR(mvp[rstd::usize(13)], expected_parent_authored.y(), 1e-5f);

    auto layer_camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(3840, 2160, -1.0, 1.0));
    layer_camera->AttatchNode(effect.as_ptr());
    effect->AttachLayer(std::make_shared<owe::SceneNodeLayer>(
        child.as_ptr(), 3840.0f, 2160.0f, "_rt_effect_composite_test"));
    scene.RegisterCamera(String::make("layer"_str), layer_camera.clone());
    camera_resolver->Add(String::make("layer"_str), layer_camera.clone());
    effect->SetCamera("layer");
    mvp = capture_mvp();
    EXPECT_NEAR(mvp[rstd::usize(12)], 0.0f, 1e-5f);
    EXPECT_NEAR(mvp[rstd::usize(13)], 0.0f, 1e-5f);
}

TEST(UniformSourceParallax, OrthographicOmittedDepthUsesImplicitParallax) {
    owe::Scene scene;
    scene.SetOrtho({ i32(1920), i32(1080) });

    auto camera_node = Arc<owe::SceneNode>::make(Eigen::Vector3f { 960.0f, 540.0f, 0.0f },
                                                 Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
                                                 Eigen::Vector3f::Zero());
    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(1920, 1080, -1.0, 1.0));
    camera->AttatchNode(camera_node.as_ptr());
    scene.RegisterCamera(String::make("default"_str), camera.clone());
    ASSERT_TRUE(scene.SetActiveCamera("default"_str));

    auto layer = Arc<owe::SceneNode>::make(Eigen::Vector3f { 1200.0f, 700.0f, 0.0f },
                                           Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
                                           Eigen::Vector3f::Zero());
    auto mesh = std::make_shared<owe::SceneMesh>();
    mesh->AddMaterial(owe::SceneMaterial {});
    owe::SceneMesh::Submesh submesh;
    submesh.material_slot = u32();
    mesh->Submeshes().push_back(std::move(submesh));
    layer->AddMesh(mesh);
    scene.RootMut()->AppendChild(layer.clone());
    scene.RebuildResourceIndex();

    auto state = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    state->SetLayerParallaxPolicy(true, true);
    state->CameraParallax() = { true, 0.5f, 0.0f, 0.36f };
    state->SetOrtho(1920.0f, 1080.0f);
    state->SetPointerInput(0.0, 1.0);
    state->Advance(owe::SceneFrame {});

    auto camera_resolver = Arc<owe::UniformCameraResolver>::make(camera.clone());
    camera_resolver->Add(String::make("default"_str), camera.clone());

    auto layer_state = Arc<owe::UniformNodeState>::make(layer.clone(), camera_resolver.clone());
    layer_state->object_id               = i32(1);
    layer_state->parallax_depth          = { 0.0f, 0.0f };
    layer_state->parallax_depth_authored = false;
    state->SetNodeState({ .index = rstd::u32(1), .generation = rstd::u32(1) }, layer_state.clone());

    owe::TransformUniformSource source(state.clone(), layer_state.clone());
    state->CameraParallax().enable = false;
    auto mvp_without_parallax = scene_test::Capture(
        scene.Runtime().Frame(), source, owe::TransformUniformOutput::ModelViewProjection);
    state->CameraParallax().enable = true;
    auto mvp_with_parallax = scene_test::Capture(
        scene.Runtime().Frame(), source, owe::TransformUniformOutput::ModelViewProjection);
    ASSERT_GT(mvp_without_parallax.size().to_primitive(), 13u);
    ASSERT_GT(mvp_with_parallax.size().to_primitive(), 13u);
    EXPECT_TRUE(std::abs(mvp_without_parallax[rstd::usize(12)] - mvp_with_parallax[rstd::usize(12)]) >
                    1e-5f ||
                std::abs(mvp_without_parallax[rstd::usize(13)] - mvp_with_parallax[rstd::usize(13)]) >
                    1e-5f);
}

TEST(UniformSourceParallax, UnregisteredContainerRootDoesNotPoisonChildren) {
    owe::Scene scene;
    scene.SetOrtho({ i32(3840), i32(2160) });

    auto camera_node = Arc<owe::SceneNode>::make(Eigen::Vector3f { 1920.0f, 1080.0f, 0.0f },
                                                 Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
                                                 Eigen::Vector3f::Zero());
    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(3840, 2160, -1.0, 1.0));
    camera->AttatchNode(camera_node.as_ptr());
    scene.RegisterCamera(String::make("default"_str), camera.clone());
    ASSERT_TRUE(scene.SetActiveCamera("default"_str));

    auto container = Arc<owe::SceneNode>::make(Eigen::Vector3f { 1982.0f, 1053.0f, 0.0f },
                                              Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
                                              Eigen::Vector3f::Zero());
    auto child = Arc<owe::SceneNode>::make(Eigen::Vector3f { -76.0f, -3.0f, 0.0f },
                                          Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
                                          Eigen::Vector3f::Zero());
    auto mesh  = std::make_shared<owe::SceneMesh>();
    mesh->AddMaterial(owe::SceneMaterial {});
    owe::SceneMesh::Submesh submesh;
    submesh.material_slot = u32();
    mesh->Submeshes().push_back(std::move(submesh));
    child->AddMesh(mesh);
    container->AppendChild(child.clone());
    scene.RootMut()->AppendChild(container.clone());
    scene.RebuildResourceIndex();

    auto state = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    state->SetLayerParallaxPolicy(true, true);
    state->CameraParallax() = { true, 0.03f, 0.0f, 0.36f };
    state->SetOrtho(3840.0f, 2160.0f);
    state->SetPointerInput(0.0, 1.0);
    state->Advance(owe::SceneFrame {});

    auto camera_resolver = Arc<owe::UniformCameraResolver>::make(camera.clone());
    camera_resolver->Add(String::make("default"_str), camera.clone());

    auto child_state = Arc<owe::UniformNodeState>::make(child.clone(), camera_resolver.clone());
    child_state->object_id               = i32(2);
    child_state->parallax_depth          = { -1.12f, -1.36f };
    child_state->parallax_depth_authored = true;
    state->SetNodeState({ .index = rstd::u32(2), .generation = rstd::u32(1) }, child_state.clone());

    owe::TransformUniformSource source(state.clone(), child_state.clone());
    auto mvp = scene_test::Capture(
        scene.Runtime().Frame(), source, owe::TransformUniformOutput::ModelViewProjection);

    const Eigen::Vector2f camera_pos { 1920.0f, 1080.0f };
    const Eigen::Vector2f mouse_vec { 691.2f, 388.8f };
    const Eigen::Vector2f depth { -1.12f, -1.36f };
    const Eigen::Vector2f base { 1906.0f, 1050.0f };
    const Eigen::Vector2f offset =
        (base - camera_pos + mouse_vec).cwiseProduct(depth) * 0.03f;
    const Eigen::Vector2f final_pos = base + offset;
    const Eigen::Vector2f expected {
        (final_pos.x() - 1920.0f) / 1920.0f,
        (final_pos.y() - 1080.0f) / 1080.0f,
    };

    ASSERT_GT(mvp.size().to_primitive(), 13u);
    EXPECT_NEAR(mvp[rstd::usize(12)], expected.x(), 1e-5f);
    EXPECT_NEAR(mvp[rstd::usize(13)], expected.y(), 1e-5f);
}

TEST(UniformSourceParallax, DisablePropagationBlocksInheritance) {
    owe::Scene scene;
    scene.SetOrtho({ i32(3840), i32(2160) });

    auto camera_node = Arc<owe::SceneNode>::make(Eigen::Vector3f { 1920.0f, 1080.0f, 0.0f },
                                                 Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
                                                 Eigen::Vector3f::Zero());
    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(3840, 2160, -1.0, 1.0));
    camera->AttatchNode(camera_node.as_ptr());
    scene.RegisterCamera(String::make("default"_str), camera.clone());
    ASSERT_TRUE(scene.SetActiveCamera("default"_str));

    auto parent = Arc<owe::SceneNode>::make(Eigen::Vector3f { 1982.0f, 1053.0f, 0.0f },
                                            Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
                                            Eigen::Vector3f::Zero());
    auto child  = Arc<owe::SceneNode>::make(Eigen::Vector3f { -76.0f, -3.0f, 0.0f },
                                            Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
                                            Eigen::Vector3f::Zero());
    auto mesh   = std::make_shared<owe::SceneMesh>();
    mesh->AddMaterial(owe::SceneMaterial {});
    owe::SceneMesh::Submesh submesh;
    submesh.material_slot = u32();
    mesh->Submeshes().push_back(std::move(submesh));
    child->AddMesh(mesh);
    parent->AppendChild(child.clone());
    scene.RootMut()->AppendChild(parent.clone());
    scene.RebuildResourceIndex();

    auto state = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    state->SetLayerParallaxPolicy(true, true);
    state->CameraParallax() = { true, 0.03f, 0.0f, 0.36f };
    state->SetOrtho(3840.0f, 2160.0f);
    state->SetPointerInput(0.0, 1.0);
    state->Advance(owe::SceneFrame {});

    auto camera_resolver = Arc<owe::UniformCameraResolver>::make(camera.clone());
    camera_resolver->Add(String::make("default"_str), camera.clone());

    auto parent_state = Arc<owe::UniformNodeState>::make(parent.clone(), camera_resolver.clone());
    parent_state->object_id                      = i32(1);
    parent_state->parallax_depth                 = { -1.56f, -0.79f };
    parent_state->parallax_depth_authored        = true;
    parent_state->propagate_parallax_to_children = false;
    auto child_state = Arc<owe::UniformNodeState>::make(child.clone(), camera_resolver.clone());
    child_state->object_id               = i32(2);
    child_state->parallax_depth          = { 0.0f, 0.0f };
    child_state->parallax_depth_authored = false;
    state->SetNodeState({ .index = rstd::u32(1), .generation = rstd::u32(1) },
                        parent_state.clone());
    state->SetNodeState({ .index = rstd::u32(2), .generation = rstd::u32(1) }, child_state.clone());

    owe::TransformUniformSource source(state.clone(), child_state.clone());
    auto capture_mvp = [&]() {
        return scene_test::Capture(
            scene.Runtime().Frame(), source, owe::TransformUniformOutput::ModelViewProjection);
    };

    parent_state->propagate_parallax_to_children = true;
    auto mvp_inherited = capture_mvp();

    parent_state->propagate_parallax_to_children = false;
    auto mvp_blocked = capture_mvp();

    ASSERT_GT(mvp_inherited.size().to_primitive(), 13u);
    ASSERT_GT(mvp_blocked.size().to_primitive(), 13u);
    EXPECT_TRUE(std::abs(mvp_inherited[rstd::usize(12)] - mvp_blocked[rstd::usize(12)]) > 1e-5f ||
                std::abs(mvp_inherited[rstd::usize(13)] - mvp_blocked[rstd::usize(13)]) > 1e-5f);
}

TEST(UniformSourceParallax, EffectModelReceivesParallaxShift) {
    owe::Scene scene;
    scene.SetOrtho({ i32(3840), i32(2160) });

    auto camera_node = Arc<owe::SceneNode>::make(Eigen::Vector3f { 1920.0f, 1080.0f, 0.0f },
                                                 Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
                                                 Eigen::Vector3f::Zero());
    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(3840, 2160, -1.0, 1.0));
    camera->AttatchNode(camera_node.as_ptr());
    scene.RegisterCamera(String::make("default"_str), camera.clone());
    ASSERT_TRUE(scene.SetActiveCamera("default"_str));

    auto parent = Arc<owe::SceneNode>::make(Eigen::Vector3f { 1982.0f, 1053.0f, 0.0f },
                                            Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
                                            Eigen::Vector3f::Zero());
    auto child  = Arc<owe::SceneNode>::make(Eigen::Vector3f { -76.0f, -3.0f, 0.0f },
                                            Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
                                            Eigen::Vector3f::Zero());
    auto effect = Arc<owe::SceneNode>::make();
    auto mesh   = std::make_shared<owe::SceneMesh>();
    mesh->AddMaterial(owe::SceneMaterial {});
    owe::SceneMesh::Submesh submesh;
    submesh.material_slot = u32();
    mesh->Submeshes().push_back(std::move(submesh));
    child->AddMesh(mesh);
    parent->AppendChild(child.clone());
    scene.RootMut()->AppendChild(parent.clone());
    scene.RebuildResourceIndex();
    effect->SetParentAnchor(child.as_ptr());

    auto state = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    state->SetLayerParallaxPolicy(true, true);
    state->CameraParallax() = { true, 0.03f, 0.0f, 0.36f };
    state->SetOrtho(3840.0f, 2160.0f);
    state->SetPointerInput(0.0, 1.0);
    state->Advance(owe::SceneFrame {});

    auto camera_resolver = Arc<owe::UniformCameraResolver>::make(camera.clone());
    camera_resolver->Add(String::make("default"_str), camera.clone());

    auto parent_state = Arc<owe::UniformNodeState>::make(parent.clone(), camera_resolver.clone());
    parent_state->object_id               = i32(1);
    parent_state->parallax_depth          = { -1.56f, -0.79f };
    parent_state->parallax_depth_authored = true;
    auto child_state = Arc<owe::UniformNodeState>::make(child.clone(), camera_resolver.clone());
    child_state->object_id               = i32(2);
    child_state->parallax_depth          = { 1.0f, 1.0f };
    child_state->parallax_depth_authored = false;
    auto effect_state = Arc<owe::UniformNodeState>::make(effect.clone(), camera_resolver.clone());
    effect_state->object_id               = i32(2);
    effect_state->parallax_depth          = { 1.0f, 1.0f };
    effect_state->parallax_depth_authored = false;
    effect_state->effect_projection_node  = Some(child.clone());
    effect_state->effect_projection_size  = { 3840.0f, 2160.0f };
    state->SetNodeState({ .index = rstd::u32(1), .generation = rstd::u32(1) },
                        parent_state.clone());
    state->SetNodeState({ .index = rstd::u32(2), .generation = rstd::u32(1) }, child_state.clone());
    state->SetNodeState({ .index = rstd::u32(3), .generation = rstd::u32(1) },
                        effect_state.clone());

    owe::TransformUniformSource source(state.clone(), effect_state.clone());
    auto effect_enabled =
        scene_test::Capture(scene.Runtime().Frame(), source, owe::TransformUniformOutput::EffectModel);
    state->CameraParallax().enable = false;
    auto effect_disabled =
        scene_test::Capture(scene.Runtime().Frame(), source, owe::TransformUniformOutput::EffectModel);
    ASSERT_GT(effect_enabled.size().to_primitive(), 13u);
    ASSERT_GT(effect_disabled.size().to_primitive(), 13u);
    EXPECT_TRUE(std::abs(effect_enabled[rstd::usize(12)] - effect_disabled[rstd::usize(12)]) > 1e-4f ||
                std::abs(effect_enabled[rstd::usize(13)] - effect_disabled[rstd::usize(13)]) > 1e-4f);
}

TEST(UniformSourceParallax, PerspectiveWithoutAuthoredParallaxDepthSkipsShift) {
    owe::Scene scene;
    scene.SetOrtho({ i32(1920), i32(1080) });

    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakePerspective(16.0 / 9.0, 0.1, 10000.0, 60.0));
    camera->SetLookAt(Eigen::Vector3d { -1.69, 0.62, 9.29 },
                      Eigen::Vector3d { -1.58, 0.60, 8.30 },
                      Eigen::Vector3d::UnitY());
    scene.RegisterCamera(String::make("default"_str), camera.clone());
    ASSERT_TRUE(scene.SetActiveCamera("default"_str));

    auto layer = Arc<owe::SceneNode>::make(Eigen::Vector3f { 0.0f, 0.0f, 3.0f },
                                           Eigen::Vector3f { 0.0025f, 0.0025f, 5.0f },
                                           Eigen::Vector3f::Zero());
    auto mesh = std::make_shared<owe::SceneMesh>();
    mesh->AddMaterial(owe::SceneMaterial {});
    owe::SceneMesh::Submesh submesh;
    submesh.material_slot = u32();
    mesh->Submeshes().push_back(std::move(submesh));
    layer->AddMesh(mesh);
    scene.RootMut()->AppendChild(layer.clone());
    scene.RebuildResourceIndex();

    auto state = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    state->SetLayerParallaxPolicy(false, false);
    state->CameraParallax() = { true, 2.0f, 0.0f, 1.0f };
    state->SetOrtho(1920.0f, 1080.0f);
    state->SetPointerInput(0.0, 1.0);
    state->Advance(owe::SceneFrame {});

    auto camera_resolver = Arc<owe::UniformCameraResolver>::make(camera.clone());
    camera_resolver->Add(String::make("default"_str), camera.clone());

    auto layer_state = Arc<owe::UniformNodeState>::make(layer.clone(), camera_resolver.clone());
    layer_state->object_id               = i32(31);
    layer_state->parallax_depth          = { 0.0f, 0.0f };
    layer_state->parallax_depth_authored = false;
    state->SetNodeState({ .index = rstd::u32(1), .generation = rstd::u32(1) }, layer_state.clone());

    owe::TransformUniformSource source(state.clone(), layer_state.clone());
    state->CameraParallax().enable = false;
    auto mvp_without_parallax = scene_test::Capture(
        scene.Runtime().Frame(), source, owe::TransformUniformOutput::ModelViewProjection);
    state->CameraParallax().enable = true;
    auto mvp_with_parallax = scene_test::Capture(
        scene.Runtime().Frame(), source, owe::TransformUniformOutput::ModelViewProjection);
    ASSERT_GT(mvp_without_parallax.size().to_primitive(), 13u);
    ASSERT_GT(mvp_with_parallax.size().to_primitive(), 13u);
    EXPECT_NEAR(mvp_without_parallax[rstd::usize(12)], mvp_with_parallax[rstd::usize(12)], 1e-5f);
    EXPECT_NEAR(mvp_without_parallax[rstd::usize(13)], mvp_with_parallax[rstd::usize(13)], 1e-5f);
}

TEST(UniformSourceParallax, PerspectiveWithAuthoredParallaxDepthAppliesShift) {
    owe::Scene scene;
    scene.SetOrtho({ i32(1920), i32(1080) });

    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakePerspective(16.0 / 9.0, 0.1, 10000.0, 60.0));
    camera->SetLookAt(Eigen::Vector3d { 0.0, 0.0, 5.0 },
                      Eigen::Vector3d::Zero(),
                      Eigen::Vector3d::UnitY());
    scene.RegisterCamera(String::make("default"_str), camera.clone());
    ASSERT_TRUE(scene.SetActiveCamera("default"_str));

    auto layer = Arc<owe::SceneNode>::make(Eigen::Vector3f { 1.0f, 2.0f, -3.0f },
                                           Eigen::Vector3f::Ones(),
                                           Eigen::Vector3f::Zero());
    auto mesh = std::make_shared<owe::SceneMesh>();
    mesh->AddMaterial(owe::SceneMaterial {});
    owe::SceneMesh::Submesh submesh;
    submesh.material_slot = u32();
    mesh->Submeshes().push_back(std::move(submesh));
    layer->AddMesh(mesh);
    scene.RootMut()->AppendChild(layer.clone());
    scene.RebuildResourceIndex();

    auto state = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    state->SetLayerParallaxPolicy(true, false);
    state->CameraParallax() = { true, 0.5f, 0.0f, 1.0f };
    state->SetOrtho(1920.0f, 1080.0f);
    state->SetPointerInput(0.0, 1.0);
    state->Advance(owe::SceneFrame {});

    auto camera_resolver = Arc<owe::UniformCameraResolver>::make(camera.clone());
    camera_resolver->Add(String::make("default"_str), camera.clone());

    auto layer_state = Arc<owe::UniformNodeState>::make(layer.clone(), camera_resolver.clone());
    layer_state->object_id               = i32(1);
    layer_state->parallax_depth          = { 1.0f, 1.0f };
    layer_state->parallax_depth_authored = true;
    state->SetNodeState({ .index = rstd::u32(1), .generation = rstd::u32(1) }, layer_state.clone());

    owe::TransformUniformSource source(state.clone(), layer_state.clone());
    state->CameraParallax().enable = false;
    auto mvp_without_parallax = scene_test::Capture(
        scene.Runtime().Frame(), source, owe::TransformUniformOutput::ModelViewProjection);
    state->CameraParallax().enable = true;
    auto mvp_with_parallax = scene_test::Capture(
        scene.Runtime().Frame(), source, owe::TransformUniformOutput::ModelViewProjection);
    ASSERT_GT(mvp_without_parallax.size().to_primitive(), 13u);
    ASSERT_GT(mvp_with_parallax.size().to_primitive(), 13u);
    EXPECT_TRUE(std::abs(mvp_without_parallax[rstd::usize(12)] - mvp_with_parallax[rstd::usize(12)]) >
                    1e-5f ||
                std::abs(mvp_without_parallax[rstd::usize(13)] - mvp_with_parallax[rstd::usize(13)]) >
                    1e-5f);
}
