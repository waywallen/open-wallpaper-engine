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
import wescene.utils;

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
        } else if (name == "g_TextureReductionScale"_str) {
            texture_reduction_shape = shape;
            found_texture_reduction = true;
        }
        return rstd::Ok(true);
    }

    owe::UniformValueShape model_shape;
    owe::UniformValueShape spectrum_shape;
    owe::UniformValueShape light_position_shape;
    owe::UniformValueShape texture_reduction_shape;
    bool                   found_model { false };
    bool                   found_spectrum { false };
    bool                   found_light_position { false };
    bool                   found_texture_reduction { false };
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
    auto mesh = Arc<owe::SceneMesh>::make();
    mesh->SetGeometryTransform(
        Eigen::Affine3d(Eigen::Translation3d(Eigen::Vector3d(10.0, -20.0, 0.0))).matrix());
    node->AddMesh(mesh.clone());
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

TEST(SceneNodeFramework, PreservesDoublePrecisionCompositionAndInvalidation) {
    auto            parent = Arc<owe::SceneNode>::make();
    auto            child  = Arc<owe::SceneNode>::make();
    owe::SceneNode  anchor;
    Eigen::Matrix4d frame = Eigen::Matrix4d::Identity();
    frame(0, 3)           = 100000000.125;
    parent->SetLocalFrame(frame);
    parent->SetRotation({ 0.2f, -0.3f, 0.4f });
    child->SetTranslate({ 1.0f, 2.0f, 3.0f });
    child->SetScale({ 2.0f, 3.0f, 4.0f });
    ASSERT_TRUE(parent->AppendChild(child.clone()));
    ASSERT_TRUE(anchor.SetParentAnchor(child.as_ptr()));
    anchor.UpdateTrans();
    Eigen::Matrix4d expected = parent->GetLocalTrans() * child->GetLocalTrans();
    EXPECT_TRUE(child->ModelTrans().isApprox(expected, 1e-14));
    EXPECT_TRUE(anchor.ModelTrans().isApprox(expected, 1e-14));
    auto revision = child->NodeState().WorldRevision();
    anchor.UpdateTrans();
    EXPECT_EQ(child->NodeState().WorldRevision(), revision);
    parent->SetTranslate({ 4.0f, 5.0f, 6.0f });
    anchor.UpdateTrans();
    expected = parent->GetLocalTrans() * child->GetLocalTrans();
    EXPECT_TRUE(anchor.ModelTrans().isApprox(expected, 1e-14));
    EXPECT_EQ(parent->GetChildren().len(), usize(1));
    EXPECT_TRUE(child->GetChildren().is_empty());
}

TEST(SceneNodeFramework, ReparentsAndRejectsCyclesWithoutDuplicateTraversal) {
    auto first  = Arc<owe::SceneNode>::make();
    auto second = Arc<owe::SceneNode>::make();
    auto child  = Arc<owe::SceneNode>::make();
    first->SetTranslate({ 10.0f, 0.0f, 0.0f });
    second->SetTranslate({ 20.0f, 0.0f, 0.0f });
    first->AppendChild(child.clone());
    child->UpdateTrans();
    ASSERT_TRUE(second->AppendChild(child.clone()));
    ASSERT_TRUE(second->AppendChild(child.clone()));
    EXPECT_TRUE(first->GetChildren().is_empty());
    EXPECT_EQ(second->GetChildren().len(), usize(1));
    child->UpdateTrans();
    EXPECT_DOUBLE_EQ(child->ModelTrans()(0, 3), 20.0);
    EXPECT_FALSE(child->AppendChild(second.clone()));
    EXPECT_FALSE(second->SetParentAnchor(child.as_ptr()));
    second->ClearChildren();
    child->UpdateTrans();
    EXPECT_EQ(child->Parent(), nullptr);
    EXPECT_DOUBLE_EQ(child->ModelTrans()(0, 3), 0.0);
}

TEST(SceneNodeFramework, InvalidatesCachedWorldAfterParentDestruction) {
    auto           child = Arc<owe::SceneNode>::make();
    owe::SceneNode anchor;
    child->SetTranslate({ 3.0f, 0.0f, 0.0f });
    {
        owe::SceneNode parent;
        parent.SetTranslate({ 10.0f, 0.0f, 0.0f });
        parent.AppendChild(child.clone());
        anchor.SetParentAnchor(&parent);
        child->UpdateTrans();
        anchor.UpdateTrans();
        EXPECT_DOUBLE_EQ(child->ModelTrans()(0, 3), 13.0);
    }
    EXPECT_EQ(child->Parent(), nullptr);
    EXPECT_EQ(anchor.Parent(), nullptr);
    child->UpdateTrans();
    anchor.UpdateTrans();
    EXPECT_DOUBLE_EQ(child->ModelTrans()(0, 3), 3.0);
    EXPECT_DOUBLE_EQ(anchor.ModelTrans()(0, 3), 0.0);
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

TEST(TextureUniformSource, PublishesUnreducedTextureScale) {
    owe::TextureUniformSource source;
    scene_test::ShapeSink     binding_sink;
    auto                      bindings = rstd::dyn<owe::UniformBindingSink>::from_ref(binding_sink);
    ASSERT_TRUE(source.Describe(bindings.as_mut_ref()).is_ok());
    ASSERT_TRUE(binding_sink.found_texture_reduction);
    EXPECT_EQ(binding_sink.texture_reduction_shape.scalar, owe::UniformScalarType::Float32);
    EXPECT_EQ(binding_sink.texture_reduction_shape.kind, owe::UniformValueKind::Linear);
    EXPECT_EQ(binding_sink.texture_reduction_shape.min_elements, u32(1));
    EXPECT_EQ(binding_sink.texture_reduction_shape.max_elements, u32(1));

    owe::SceneFrame frame;
    auto scale = scene_test::Capture(frame, source, owe::TextureUniformOutput::ReductionScale);
    ASSERT_EQ(scale.size(), usize(1));
    EXPECT_FLOAT_EQ(scale[usize()], 1.0f);
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

TEST(AudioResponseDemand, ReconcilesLeaseOwnerReplacementAtomically) {
    owe::AudioResponseDemand demand;
    std::vector<bool>        changes;
    demand.SetCallback([&changes](bool active) {
        changes.push_back(active);
    });

    auto lease = rstd::Some(demand.Acquire());
    ASSERT_EQ(changes, (std::vector<bool> { false, true }));
    {
        auto reconciliation = demand.BeginReconciliation();
        lease               = rstd::None();
        lease               = rstd::Some(demand.Acquire());
        EXPECT_TRUE(demand.Active());
    }
    EXPECT_EQ(changes, (std::vector<bool> { false, true }));

    {
        auto outer = demand.BeginReconciliation();
        {
            auto inner = demand.BeginReconciliation();
            lease      = rstd::None();
        }
        EXPECT_FALSE(demand.Active());
        EXPECT_EQ(changes, (std::vector<bool> { false, true }));
    }
    EXPECT_EQ(changes, (std::vector<bool> { false, true, false }));

    {
        auto reconciliation = demand.BeginReconciliation();
        lease               = rstd::Some(demand.Acquire());
    }
    EXPECT_EQ(changes, (std::vector<bool> { false, true, false, true }));
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
        })JSON"_str,
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
    auto controller = scene.scene->RootMut()->FindByName("BGM Controller"_str);
    auto target     = scene.scene->RootMut()->FindByName("BGM Target"_str);
    ASSERT_NE(controller, nullptr);
    ASSERT_NE(target, nullptr);
    EXPECT_FALSE(target->IsPlaying());

    auto property = rstd::json::from_str(R"({"type":"combo","value":"0"})"_str).unwrap();
    owe::script::SetSceneUserProperty(*scene.scene, "song_selection"_str, property);
    EXPECT_TRUE(target->IsPlaying());

    owe::script::TickSceneScripts(*scene.scene, owe::script::FrameInputs {});
    EXPECT_FLOAT_EQ(controller->Volume(), 0.35f);
}

TEST(SceneParserBindings, OwnsKeysAndNamesAfterDocumentDestruction) {
    auto document = owe::wpscene::ParseSceneDocumentJson(R"({
        "camera":{},
        "general":{
            "clearcolor":{"value":"0 0 0","user":"background"},
            "cameraparallaxamount":{"value":0.5,"user":"amount"},
            "camerashakeamplitude":{"value":0.5,"user":"shake"}
        },
        "objects":[
            {"id":1,"name":"parent","visible":{"value":false,"user":"enabled"}},
            {"id":2,"name":"child","parent":1,"text":""}
        ]
    })"_str,
                                                         owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());
    owe::fs::VFS                vfs;
    wavsen::audio::SoundManager sound_manager;
    owe::SceneParser            parser;
    auto                        parsed = parser.Parse(
        "binding-lifetime"_str,
        ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
        mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound_manager)));
    ASSERT_TRUE(parsed.is_ok());
    auto scene  = rstd::move(parsed).unwrap();
    document    = None();
    auto parent = scene.scene->RootMut()->FindByName("parent"_str);
    auto child  = scene.scene->RootMut()->FindByName("child"_str);
    ASSERT_NE(parent, nullptr);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(parent->VisibleUserBinding().key, "enabled"_str);
    EXPECT_FALSE(parent->Visible());
    EXPECT_EQ(scene.scene->ClearColorUserKey(), "background"_str);
    auto value = owe::ParseJson("0.75"_str).unwrap();
    EXPECT_TRUE(scene.scene->ApplyUserPropertyBindings("amount"_str, value));
    EXPECT_TRUE(scene.scene->ApplyUserPropertyBindings("shake"_str, value));
    EXPECT_FALSE(scene.scene->ApplyUserPropertyBindings("missing"_str, value));
    scene.scene->ApplyUserNodeVisibilityBindings("enabled"_str,
                                                 owe::ParseJson("true"_str).unwrap());
    EXPECT_TRUE(parent->Visible());
}

TEST(SceneParserScript, FractionSliderPreservesAuthoredValue) {
    auto document = owe::wpscene::ParseSceneDocumentJson(
        R"JSON({
            "camera": {},
            "general": {},
            "objects": [{
                "id": 1,
                "name": "Clock Group",
                "scale": {
                    "script": "export var scriptProperties = createScriptProperties().addSlider({name: 'clock_size', value: 0.4, min: 0, max: 1}).finish(); export function update(value) { value.x = scriptProperties.clock_size / 10000; value.y = scriptProperties.clock_size / 10000; return value; }",
                    "scriptproperties": {
                        "clock_size": {"user": "clocksize", "value": 0.4}
                    },
                    "value": "0.00004 0.00004 0.00004"
                }
            }]
        })JSON"_str,
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());

    auto user_properties = rstd::json::Map::make();
    user_properties.insert(
        String::make("clocksize"_str),
        owe::ParseJson(R"({"type":"slider","fraction":true,"value":0.4})"_str).unwrap());

    owe::fs::VFS                vfs;
    wavsen::audio::SoundManager sound_manager;
    owe::SceneParser            parser;
    auto                        parsed = parser.Parse(
        "fraction-slider-scale"_str,
        ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
        mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
        mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound_manager)),
        owe::SceneParseOptions {
            .user_properties =
                Some(ref<rstd::json::Map>::from_raw_parts(rstd::addressof(user_properties))),
        });
    ASSERT_TRUE(parsed.is_ok());

    auto scene = rstd::move(parsed).unwrap();
    auto group = scene.scene->RootMut()->FindByName("Clock Group"_str);
    ASSERT_NE(group, nullptr);

    owe::script::TickSceneScripts(*scene.scene, owe::script::FrameInputs {});
    EXPECT_FLOAT_EQ(group->Scale().x(), 0.00004f);
    EXPECT_FLOAT_EQ(group->Scale().y(), 0.00004f);
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
        })JSON"_str,
        owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());

    auto assets = owe::fs::make_physical_fs(
        owe::fs::Path(rstd::cppstd::as_str(WAYWALLEN_ASSETS_DIR).unwrap()));
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
    auto controller = scene.scene->RootMut()->FindByName("Controller"_str);
    auto dynamic    = scene.scene->RootMut()->FindByName("__createLayer"_str);
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
        })JSON"_str,
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
    auto parent = scene.scene->RootMut()->FindByName("Empty Text Parent"_str);
    auto child  = scene.scene->RootMut()->FindByName("Authored Child"_str);
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
        })JSON"_str,
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
    auto value = scene.scene->RootMut()->FindByName("Value"_str);
    ASSERT_NE(value, nullptr);
    ASSERT_NE(value->Mesh(), nullptr);
    value->Mesh()->ConsumeDirtyFlags();

    owe::script::TickSceneScripts(*scene.scene, owe::script::FrameInputs {});
    EXPECT_NE(value->Mesh()->DirtyFlags(), owe::SceneMeshDirtyNone);
}

TEST(SceneParserText, AlignmentDoesNotMoveChildFrames) {
    for (auto blend_mode : { "0"_str, "11"_str }) {
        auto document = owe::wpscene::ParseSceneDocumentJson(
            R"JSON({
                "camera": {},
                "general": {"orthogonalprojection": {"width": 1920, "height": 1080}},
                "objects": [
                    {"id": 1, "name": "Parent",
                     "text": {"value": "12:34", "user": "caption"},
                     "font": "systemfont_DejaVu Sans", "pointsize": 12,
                     "horizontalalign": "right", "verticalalign": "top",
                     "origin": {"value": "1000 600 0", "script": "export function update() { return new Vec3(1200, 700, 0); }"},
                     "scale": {"value": "2 3 1", "script": "export function update() { return new Vec3(4, 5, 1); }"},
                     "visible": {"value": true, "script": "export function update(value) { thisLayer.pointsize = 24; thisLayer.horizontalalign = 'left'; thisLayer.verticalalign = 'bottom'; return value; }"}},
                    {"id": 2, "name": "Child", "parent": 1, "text": "12:34",
                     "font": "systemfont_DejaVu Sans", "pointsize": 12,
                     "horizontalalign": "right", "verticalalign": "top",
                     "origin": "-3 4 0"}
                ]
            })JSON"_str,
            owe::wpscene::kSceneVersionUnknown);
        ASSERT_TRUE(document.is_some());
        for (auto& object : document->objects) {
            object.authored.as_object_mut().unwrap()->insert("colorBlendMode"_Str,
                                                             owe::ParseJson(blend_mode).unwrap());
        }
        auto assets = owe::fs::make_physical_fs(
            owe::fs::Path(rstd::cppstd::as_str(WAYWALLEN_ASSETS_DIR).unwrap()));
        ASSERT_TRUE(assets.is_ok());
        owe::fs::VFS vfs;
        ASSERT_TRUE(vfs.mount("/assets"_str, rstd::move(assets).unwrap()).is_ok());
        wavsen::audio::SoundManager sound_manager;
        owe::SceneParser            parser;
        auto                        parsed = parser.Parse(
            "text-parent-alignment"_str,
            ref<owe::wpscene::SceneDocument>::from_raw_parts(rstd::addressof(*document)),
            mut_ref<owe::fs::VFS>::from_raw_parts(rstd::addressof(vfs)),
            mut_ref<wavsen::audio::SoundManager>::from_raw_parts(rstd::addressof(sound_manager)));
        ASSERT_TRUE(parsed.is_ok());
        auto  scene  = rstd::move(parsed).unwrap();
        auto* parent = scene.scene->RootMut()->FindByName("Parent"_str);
        auto* child  = scene.scene->RootMut()->FindByName("Child"_str);
        ASSERT_NE(parent, nullptr);
        ASSERT_NE(child, nullptr);
        EXPECT_EQ(child->Parent(), parent);
        EXPECT_TRUE(parent->Translate().isApprox(Eigen::Vector3f(1000, 600, 0)));
        child->UpdateTrans();
        EXPECT_NEAR(child->ModelTrans()(0, 3), 994.0, 1e-5);
        EXPECT_NEAR(child->ModelTrans()(1, 3), 612.0, 1e-5);
        const Eigen::Matrix4d initial_anchor = parent->GeometryTransform();
        EXPECT_LT(initial_anchor(0, 3), 0.0);
        EXPECT_LT(initial_anchor(1, 3), 0.0);
        EXPECT_TRUE(child->GeometryTransform().isApprox(initial_anchor));

        auto caption = owe::ParseJson(R"({"value":"12:3456789"})"_str).unwrap();
        ASSERT_TRUE(scene.scene->ApplyUserTextBindings("caption"_str, caption));
        EXPECT_LT(parent->GeometryTransform()(0, 3), initial_anchor(0, 3));
        EXPECT_TRUE(child->GeometryTransform().isApprox(initial_anchor));
        child->UpdateTrans();
        EXPECT_NEAR(child->ModelTrans()(0, 3), 994.0, 1e-5);
        EXPECT_NEAR(child->ModelTrans()(1, 3), 612.0, 1e-5);

        owe::script::TickSceneScripts(*scene.scene, owe::script::FrameInputs {});
        EXPECT_TRUE(parent->Translate().isApprox(Eigen::Vector3f(1200, 700, 0)));
        EXPECT_TRUE(parent->Scale().isApprox(Eigen::Vector3f(4, 5, 1)));
        EXPECT_GT(parent->GeometryTransform()(0, 3), -initial_anchor(0, 3));
        EXPECT_GT(parent->GeometryTransform()(1, 3), -initial_anchor(1, 3));
        child->UpdateTrans();
        EXPECT_NEAR(child->ModelTrans()(0, 3), 1188.0, 1e-5);
        EXPECT_NEAR(child->ModelTrans()(1, 3), 720.0, 1e-5);

        parent->SetRotation({ 0.0f, 0.0f, f32::consts::FRAC_PI_2.to_primitive() });
        child->UpdateTrans();
        EXPECT_NEAR(child->ModelTrans()(0, 3), 1180.0, 1e-4);
        EXPECT_NEAR(child->ModelTrans()(1, 3), 688.0, 1e-4);
        const Eigen::Matrix4d draw = parent->ModelTrans() * parent->GeometryTransform();
        EXPECT_NEAR(draw(0, 3), 1200.0 - 5.0 * parent->GeometryTransform()(1, 3), 1e-3);
        EXPECT_NEAR(draw(1, 3), 700.0 + 4.0 * parent->GeometryTransform()(0, 3), 1e-3);
    }
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

    auto property = owe::ParseJson(R"({"type":"textinput","value":"updated"})"_str).unwrap();
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

    auto property = owe::ParseJson(R"({"type":"textinput","value":""})"_str).unwrap();
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

    auto property = owe::ParseJson(R"({"value":true})"_str).unwrap();
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
    auto state    = Arc<owe::text::TextUniformState>::make(node.clone());
    state->camera = Some(camera.clone());

    owe::text::TextUniformSource source(state.clone());
    auto                         value = scene_test::Capture(
        scene.Runtime().Frame(), source, owe::text::TextUniformOutput::ModelViewProjection);

    EXPECT_EQ(value.size().to_primitive(), 16u);
}

TEST(TextUniformSource, EffectProjectionIncludesGeometryAlignment) {
    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(200, 100, -1.0, 1.0));
    auto node  = Arc<owe::SceneNode>::make();
    auto layer = Arc<owe::SceneNode>::make();
    layer->SetTranslate({ 100, 50, 0 });
    layer->SetScale({ 2, 3, 1 });
    layer->SetGeometryTransform(Eigen::Affine3d(Eigen::Translation3d(-20, -10, 0)).matrix());
    auto state               = Arc<owe::text::TextUniformState>::make(node.clone());
    state->camera            = Some(camera.clone());
    state->effect_projection = Some(Arc<owe::text::TextEffectProjectionState>::make(
        owe::text::TextEffectProjectionState { .node = layer.clone(), .size = { 40.0f, 20.0f } }));
    owe::text::TextUniformSource source(state.clone());
    auto                         projected = scene_test::Capture(
        owe::SceneFrame {}, source, owe::text::TextUniformOutput::EffectModelViewProjection);
    ASSERT_EQ(projected.size(), usize(16));
    EXPECT_FLOAT_EQ(projected[usize()], 0.4f);
    EXPECT_FLOAT_EQ(projected[usize(5)], 0.6f);
    EXPECT_FLOAT_EQ(projected[usize(12)], 0.6f);
    EXPECT_FLOAT_EQ(projected[usize(13)], 0.4f);

    auto uniform_state = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    auto resolver      = Arc<owe::UniformCameraResolver>::make(camera.clone());
    auto node_state    = Arc<owe::UniformNodeState>::make(node.clone(), resolver.clone());
    node_state->effect_projection_node = Some(layer.clone());
    node_state->effect_projection_size = { 40.0f, 20.0f };
    owe::TransformUniformSource transform_source(uniform_state.clone(), node_state.clone());
    auto effect = scene_test::Capture(owe::SceneFrame {},
                                      transform_source,
                                      owe::TransformUniformOutput::EffectModelViewProjection);
    ASSERT_EQ(effect.size(), projected.size());
    for (usize i {}; i < effect.size(); ++i) EXPECT_FLOAT_EQ(effect[i], projected[i]);
    auto model = scene_test::Capture(
        owe::SceneFrame {}, transform_source, owe::TransformUniformOutput::LayerModel);
    ASSERT_EQ(model.size(), usize(16));
    EXPECT_FLOAT_EQ(model[usize(12)], 60.0f);
    EXPECT_FLOAT_EQ(model[usize(13)], 20.0f);
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

TEST(SceneCameraState, PublishesStableOwnedMatricesAndProjectionChanges) {
    auto       camera = owe::SceneCamera::MakeOrthographic(200, 100, -1, 1);
    const auto first  = camera.CameraSnapshot();
    EXPECT_TRUE(first.view.isIdentity());
    EXPECT_TRUE(first.view_projection.isApprox(first.projection * first.view));
    EXPECT_EQ(camera.CameraSnapshot().revision, first.revision);
    camera.SetWidth(400);
    const auto changed = camera.CameraSnapshot();
    EXPECT_EQ(changed.revision, first.revision + u64(1));
    EXPECT_DOUBLE_EQ(first.projection(0, 0), 0.01);
    EXPECT_DOUBLE_EQ(changed.projection(0, 0), 0.005);
    auto clone = camera;
    clone.SetHeight(50);
    EXPECT_FALSE(clone.CameraSnapshot().projection.isApprox(changed.projection));
    EXPECT_EQ(camera.CameraSnapshot().revision, changed.revision);
    camera.Clone(clone);
    EXPECT_EQ(camera.CameraSnapshot().revision, changed.revision + u64(1));
}

TEST(SceneCameraState, TracksParentChangesAndPreservesNodeInverse) {
    auto parent = Arc<owe::SceneNode>::make();
    auto child  = Arc<owe::SceneNode>::make();
    parent->AppendChild(child.clone());
    parent->SetTranslate({ 20, 30, 0 });
    child->SetScale({ 9, 9, 1 });
    auto camera = owe::SceneCamera::MakeOrthographic(200, 100, -1, 1);
    camera.AttatchNode(child.as_ptr());
    const auto first = camera.CameraSnapshot();
    EXPECT_TRUE((first.view * child->ModelTrans()).isIdentity(1e-12));
    parent->SetTranslate({ 40, 30, 0 });
    const auto changed = camera.CameraSnapshot();
    EXPECT_EQ(changed.revision, first.revision + u64(1));
    EXPECT_TRUE((changed.view * child->ModelTrans()).isIdentity(1e-12));
    EXPECT_FALSE(first.view.isApprox(changed.view));
    EXPECT_EQ(camera.CameraSnapshot().revision, changed.revision);
}

TEST(SceneCameraState, ReflectionPreservesUpAndDoesNotReplacePrimary) {
    auto                  camera = owe::SceneCamera::MakePerspective(1.5, 0.1, 1000, 60);
    const Eigen::Vector3d eye(2, 4, 10), center(1, 2, 0), up(0, 1, 0);
    camera.SetLookAt(eye, center, up);
    const auto            primary   = camera.CameraSnapshot();
    const auto            reflected = camera.CameraSnapshot(owe::SceneRenderViewKind::Reflection);
    const Eigen::Vector3d reflected_eye(2, -4, 10), reflected_center(1, -2, 0);
    EXPECT_TRUE(reflected.view.isApprox(Eigen::LookAt(reflected_eye, reflected_center, up)));
    EXPECT_TRUE(primary.view.isApprox(Eigen::LookAt(eye, center, up)));
    EXPECT_TRUE(reflected.projection.isApprox(primary.projection));
    EXPECT_TRUE(reflected.view_projection.isApprox(reflected.projection * reflected.view));
    EXPECT_EQ(camera.CameraSnapshot().revision, primary.revision);
    EXPECT_EQ(camera.CameraSnapshot(owe::SceneRenderViewKind::Reflection).revision,
              reflected.revision);
    camera.SetFov(45);
    EXPECT_EQ(camera.CameraSnapshot().revision, primary.revision + u64(1));
    EXPECT_EQ(camera.CameraSnapshot(owe::SceneRenderViewKind::Reflection).revision,
              reflected.revision + u64(1));
}

TEST(FrameUniformSource, PublishesUpdatedAmbientAndSkylightColors) {
    auto state = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    owe::FrameUniformSource source(state.clone());
    state->SetAmbientColor({ 0.85f, 0.85f, 0.85f });
    state->SetSkylightColor({ 0.69f, 0.69f, 0.69f });
    auto ambient =
        scene_test::Capture(owe::SceneFrame {}, source, owe::FrameUniformOutput::AmbientColor);
    ASSERT_EQ(ambient.size(), usize(3));
    EXPECT_FLOAT_EQ(ambient[usize()], 0.85f);
    state->SetAmbientColor({ 0.9f, 0.8f, 0.7f });
    state->SetSkylightColor({ 0.7f, 0.6f, 0.5f });
    ambient =
        scene_test::Capture(owe::SceneFrame {}, source, owe::FrameUniformOutput::AmbientColor);
    auto sky =
        scene_test::Capture(owe::SceneFrame {}, source, owe::FrameUniformOutput::SkylightColor);
    ASSERT_EQ(sky.size(), usize(3));
    EXPECT_FLOAT_EQ(ambient[usize()], 0.9f);
    EXPECT_FLOAT_EQ(ambient[usize(2)], 0.7f);
    EXPECT_FLOAT_EQ(sky[usize()], 0.7f);
    EXPECT_FLOAT_EQ(sky[usize(2)], 0.5f);
}

TEST(SceneParserScript, GeneralLightingPublishesFieldScriptResults) {
    auto document = owe::wpscene::ParseSceneDocumentJson(R"JSON({
        "camera": {},
        "general": {
            "ambientcolor": {"value":"0.85 0.85 0.85", "script": "export function update(value) { return new Vec3(0.9, 0.8, 0.7); }"},
            "skylightcolor": {"value":"0.69 0.69 0.69", "script": "export function update(value) { return new Vec3(0.7, 0.6, 0.5); }"}
        }, "objects": []
    })JSON"_str,
                                                         owe::wpscene::kSceneVersionUnknown);
    ASSERT_TRUE(document.is_some());
    owe::fs::VFS                vfs;
    wavsen::audio::SoundManager sound_manager;
    owe::SceneParser            parser;
    auto                        parsed =
        parser.Parse("general-lighting"_str,
                     ref<owe::wpscene::SceneDocument>::from_raw_parts(&*document),
                     mut_ref<owe::fs::VFS>::from_raw_parts(&vfs),
                     mut_ref<wavsen::audio::SoundManager>::from_raw_parts(&sound_manager));
    ASSERT_TRUE(parsed.is_ok());
    auto scene = rstd::move(parsed).unwrap();
    ASSERT_EQ(scene.scene->GlobalSources().len(), usize(1));
    auto source  = scene.scene->Resolve(scene.scene->GlobalSources()[usize()].source).unwrap();
    auto capture = [&](owe::FrameUniformOutput output) {
        owe::SceneFrame            frame;
        scene_test::EmptyResources resources;
        scene_test::UpdateContext  context_impl(frame, resources);
        scene_test::UniformSink    sink_impl(owe::ToUniformOutput(output));
        auto                       context = dyn<owe::UniformUpdateContext>::from_ref(context_impl);
        auto                       sink    = dyn<owe::UniformValueSink>::from_ref(sink_impl);
        EXPECT_TRUE(source->Evaluate(context.as_ref(), sink.as_mut_ref()).is_ok());
        EXPECT_TRUE(sink_impl.Written());
        return sink_impl.Value();
    };
    EXPECT_FLOAT_EQ(capture(owe::FrameUniformOutput::AmbientColor)[usize()], 0.85f);
    owe::script::TickSceneScripts(*scene.scene, owe::script::FrameInputs {});
    auto ambient = capture(owe::FrameUniformOutput::AmbientColor);
    auto sky     = capture(owe::FrameUniformOutput::SkylightColor);
    ASSERT_EQ(ambient.size(), usize(3));
    ASSERT_EQ(sky.size(), usize(3));
    EXPECT_FLOAT_EQ(ambient[usize()], 0.9f);
    EXPECT_FLOAT_EQ(ambient[usize(2)], 0.7f);
    EXPECT_FLOAT_EQ(sky[usize()], 0.7f);
    EXPECT_FLOAT_EQ(sky[usize(2)], 0.5f);
}

TEST(CameraShake, SamplesThreeAxesAndVectorLengthRoughness) {
    owe::UniformCameraShake shake { true, 3.0f, 0.5f, 0.0f };
    const Eigen::Vector3d   expected(0.162090692, 0.291557827, 0.252441295);
    EXPECT_TRUE(shake.Sample(4.0f, false, 1080).isApprox(expected, 1e-6));
    EXPECT_TRUE(shake.Sample(0.0f, false, 1080).isApprox(Eigen::Vector3d(0.3, 0, 0), 1e-6));
    EXPECT_TRUE(shake.Sample(1.1951633f, false, 1080)
                    .isApprox(Eigen::Vector3d(0.286707908, 0.116352516, 0.088309434), 2e-6));
    EXPECT_TRUE(shake.Sample(15.430774f, false, 1080)
                    .isApprox(Eigen::Vector3d(-0.226311371, -0.272700263, -0.196934554), 2e-6));
    shake.roughness = 0.5f;
    EXPECT_TRUE(shake.Sample(4.0f, false, 1080)
                    .isApprox(Eigen::Vector3d(0.121172355, 0.217956677, 0.188714762), 1e-6));
    shake.roughness = 1.0f;
    EXPECT_TRUE(shake.Sample(4.0f, false, 1080).isApprox(expected, 1e-6));
    shake.roughness = 2.0f;
    EXPECT_TRUE(shake.Sample(4.0f, false, 1080)
                    .isApprox(Eigen::Vector3d(1.66185942, 2.98924088, 2.58819270), 2e-6));
}

TEST(CameraShake, OrthographicHeightZeroSpeedAndDisabledState) {
    owe::UniformCameraShake shake { true, 3.0f, 0.0f, 0.0f };
    EXPECT_TRUE(shake.Sample(30.0f, false, 1920).isApprox(Eigen::Vector3d(0.3, 0, 0), 1e-6));
    EXPECT_TRUE(shake.Sample(30.0f, true, 1920).isApprox(Eigen::Vector3d(57.6, 0, 0), 1e-6));
    shake.speed = 0.5f;
    auto flat   = shake.Sample(4.0f, true, 1920);
    EXPECT_NEAR(flat.x(), 31.1214128, 1e-5);
    EXPECT_NEAR(flat.y(), 55.9791027, 1e-5);
    EXPECT_DOUBLE_EQ(flat.z(), 0.0);
    shake.roughness     = 0.5f;
    const double length = f64(flat.head<2>().norm() / 57.6).powf(f64(0.125)).to_primitive() * 57.6;
    EXPECT_NEAR(shake.Sample(4.0f, true, 1920).norm(), length, 1e-5);
    shake.amplitude = 0.0f;
    EXPECT_TRUE(shake.Sample(4.0f, true, 1920).isZero());
    shake.amplitude = 3.0f;
    shake.enable    = false;
    EXPECT_TRUE(shake.Sample(4.0f, false, 1920).isZero());
}

TEST(SceneCameraState, ViewOffsetPreservesBaseAndNodeAffineInverse) {
    auto parent = Arc<owe::SceneNode>::make();
    auto child  = Arc<owe::SceneNode>::make();
    parent->SetTranslate({ 20, 30, 40 });
    parent->SetRotation({ 0.2f, 0.3f, 0.4f });
    child->SetScale({ 9, 7, 2 });
    parent->AppendChild(child.clone());
    auto camera = owe::SceneCamera::MakeOrthographic(200, 100, -100, 100);
    camera.AttatchNode(child.as_ptr());
    const auto            base     = camera.Transforms();
    const auto            authored = camera.AuthoredTransforms();
    const Eigen::Vector3d offset(2, 3, 4);
    camera.SetViewOffset(offset);
    const Eigen::Matrix4d translated =
        Eigen::Affine3d(Eigen::Translation3d(offset)).matrix() * child->ModelTrans();
    const auto snapshot = camera.CameraSnapshot();
    EXPECT_TRUE((snapshot.view * translated).isIdentity(1e-12));
    EXPECT_TRUE(camera.GetPosition().isApprox(base.eye + offset));
    EXPECT_TRUE(camera.Transforms().eye.isApprox(base.eye));
    EXPECT_TRUE(camera.AuthoredTransforms().eye.isApprox(authored.eye));
    camera.SetViewOffset(offset);
    EXPECT_EQ(camera.CameraSnapshot().revision, snapshot.revision);
    EXPECT_TRUE(camera.GetViewMatrix().isApprox(snapshot.view));
    camera.SetViewOffset(Eigen::Vector3d::Zero());
    EXPECT_TRUE((camera.GetViewMatrix() * child->ModelTrans()).isIdentity(1e-12));
}

TEST(SceneCameraState, ViewOffsetMovesReflectionAndUniformEyeTogether) {
    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakePerspective(1.5, 0.1, 1000, 60));
    const Eigen::Vector3d eye(2, 4, 10), center(1, 2, 0), up(0, 1, 0), offset(3, 5, 7);
    camera->SetLookAt(eye, center, up);
    camera->SetViewOffset(offset);
    const auto snapshot = camera->CameraSnapshot();
    EXPECT_TRUE(snapshot.view.isApprox(Eigen::LookAt(eye + offset, center + offset, up)));
    EXPECT_TRUE(camera->CameraSnapshot(owe::SceneRenderViewKind::Reflection)
                    .view.isApprox(
                        Eigen::LookAt(Eigen::Vector3d(5, -9, 17), Eigen::Vector3d(4, -7, 7), up)));
    EXPECT_TRUE(camera->GetPosition(owe::SceneRenderViewKind::Reflection)
                    .isApprox(Eigen::Vector3d(5, -9, 17)));
    auto state    = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    auto resolver = Arc<owe::UniformCameraResolver>::make(camera.clone());
    auto node     = Arc<owe::UniformNodeState>::make(Arc<owe::SceneNode>::make(), resolver.clone());
    owe::TransformUniformSource source(state.clone(), node.clone());
    auto                        uniform_eye =
        scene_test::Capture(owe::SceneFrame {}, source, owe::TransformUniformOutput::EyePosition);
    ASSERT_EQ(uniform_eye.size(), usize(3));
    EXPECT_FLOAT_EQ(uniform_eye[usize()], 5.0f);
    EXPECT_FLOAT_EQ(uniform_eye[usize(1)], 9.0f);
    EXPECT_FLOAT_EQ(uniform_eye[usize(2)], 17.0f);
    auto vp = scene_test::Capture(
        owe::SceneFrame {}, source, owe::TransformUniformOutput::ViewProjection);
    ASSERT_EQ(vp.size(), usize(16));
    for (usize index {}; index < usize(16); ++index)
        EXPECT_NEAR(vp[index], snapshot.view_projection.data()[index.to_primitive()], 1e-5);
    EXPECT_TRUE(camera->AuthoredTransforms().eye.isApprox(eye));
}

TEST(CameraShake, RuntimeUpdatesLinkedViewAndClearsDisabledOrPreviousCamera) {
    owe::Scene scene;
    auto       camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(1080, 1920, -1, 1));
    auto linked = Arc<owe::SceneCamera>::make(*camera);
    auto effect = Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(2, 2, -1, 1));
    scene.RegisterCamera("main"_Str, camera.clone());
    scene.RegisterCamera("linked"_Str, linked.clone());
    scene.RegisterCamera("effect"_Str, effect.clone());
    scene.RegisterLinkedCamera("main"_Str, "linked"_Str);
    ASSERT_TRUE(scene.SetActiveCamera("main"_str));
    auto state = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    state->SetOrtho(1080, 1920);
    state->CameraShake() = { true, 3.0f, 0.5f, 0.0f };
    owe::UniformRuntimeSystem runtime(state.clone(), scene, true);
    owe::SceneFrame           frame;
    frame.elapsed  = f64(4.0);
    auto frame_ref = ref<owe::SceneFrame>::from_raw_parts(&frame);
    runtime.Update(frame_ref);
    const auto first = camera->CameraSnapshot();
    EXPECT_TRUE(linked->GetViewMatrix().isApprox(first.view));
    EXPECT_TRUE(effect->GetViewMatrix().isIdentity());
    EXPECT_NEAR(camera->GetPosition().y(), 55.9791027, 1e-5);
    EXPECT_TRUE(scene.ActiveCameraTransforms()->eye.isZero());
    runtime.Update(frame_ref);
    EXPECT_EQ(camera->CameraSnapshot().revision, first.revision);
    const Eigen::Vector3d origin(10, 20, 0);
    ASSERT_TRUE(scene.SetActiveCameraTransforms(
        { origin, origin - Eigen::Vector3d::UnitZ(), Eigen::Vector3d::UnitY() }));
    runtime.Update(frame_ref);
    EXPECT_TRUE(scene.ActiveCameraTransforms()->eye.isApprox(origin));
    EXPECT_TRUE(scene.ScreenToWorld({ 0.5f, 0.5f }).isApprox(camera->GetPosition(), 1e-6));
    state->ApplyUserProperty("camerashakespeed"_str, owe::ParseJson("0"_str).unwrap());
    runtime.Update(frame_ref);
    EXPECT_TRUE(camera->GetPosition().isApprox(origin + Eigen::Vector3d(57.6, 0, 0), 1e-6));
    state->ApplyUserProperty("camerashake"_str, owe::ParseJson("false"_str).unwrap());
    runtime.Update(frame_ref);
    EXPECT_TRUE(camera->GetPosition().isApprox(origin));
    EXPECT_TRUE(linked->GetPosition().isApprox(origin));
    state->CameraShake().enable = true;
    runtime.Update(frame_ref);
    ASSERT_TRUE(scene.SetActiveCamera("effect"_str));
    EXPECT_TRUE(camera->GetPosition().isApprox(origin));
    EXPECT_TRUE(linked->GetPosition().isApprox(origin));
    EXPECT_TRUE(effect->GetPosition().isZero());
}

TEST(ShadowUniformSource, CascadeCentersUseFinalCameraPosition) {
    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakePerspective(1.0, 0.1, 1000, 50));
    const Eigen::Vector3d origin(2, 4, 10), offset(3, 5, 7);
    camera->SetLookAt(origin, origin - Eigen::Vector3d::UnitZ(), Eigen::Vector3d::UnitY());
    camera->SetViewOffset(offset);
    auto                  light_node = Arc<owe::SceneNode>::make();
    owe::SceneLight::Desc desc;
    desc.type        = owe::SceneLightType::Directional;
    desc.cast_shadow = true;
    owe::SceneLight light(desc);
    light.setNode(light_node.as_ptr());
    owe::ShadowUniformSource source(camera.clone(), ref<owe::SceneLight>::from_raw_parts(&light));
    auto                     translated = scene_test::Capture(
        owe::SceneFrame {}, source, owe::ShadowUniformOutput::ViewProjectionMatrices);
    camera->SetViewOffset(Eigen::Vector3d::Zero());
    camera->SetLookAt(
        origin + offset, origin + offset - Eigen::Vector3d::UnitZ(), Eigen::Vector3d::UnitY());
    auto explicit_camera = scene_test::Capture(
        owe::SceneFrame {}, source, owe::ShadowUniformOutput::ViewProjectionMatrices);
    ASSERT_EQ(translated.size(), explicit_camera.size());
    for (usize index {}; index < translated.size(); ++index)
        EXPECT_FLOAT_EQ(translated[index], explicit_camera[index]);
}

TEST(SceneCameraPath, UserBindingMutatesRegisteredArc) {
    owe::Scene scene;
    auto       path                = Arc<owe::SceneCameraPath>::make();
    path->visible_user_binding.key = String::make("camera-path"_str);
    scene.RegisterCameraPath(path.clone());
    scene.RegisterCameraPathUserBinding(String::make("camera-path"_str), path.clone());

    auto disabled = owe::ParseJson(R"({"value":false})"_str).unwrap();
    EXPECT_TRUE(scene.ApplyUserCameraPathVisibilityBindings("camera-path"_str, disabled));
    EXPECT_FALSE(path->enabled);

    auto enabled = owe::ParseJson(R"({"value":true})"_str).unwrap();
    EXPECT_TRUE(scene.ApplyUserCameraPathVisibilityBindings("camera-path"_str, enabled));
    EXPECT_TRUE(path->enabled);
}

TEST(SceneCameraPath, SequentialQueueSamplesClipAndHoldsEmptyClip) {
    auto camera = Arc<owe::SceneCamera>::make(
        owe::SceneCamera::MakePerspective(16.0 / 9.0, 0.01, 1000.0, 45.0));
    camera->SetLookAt({ 0.0, 0.0, 5.0 }, { 0.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 });
    camera->Update();

    auto eye = Arc<owe::SceneAnimationCurve>::make();
    eye->c2.push({ .frame = i32(), .value = 5.0f });
    eye->c2.push({ .frame = i32(30), .value = 7.0f });
    auto fov = Arc<owe::SceneAnimationCurve>::make();
    fov->c0.push({ .frame = i32(), .value = 45.0f });
    fov->c0.push({ .frame = i32(30), .value = 60.0f });

    owe::SceneCameraPath path;
    path.camera      = Some(camera.clone());
    path.perspective = true;
    path.CaptureViewport();
    path.queue.push(owe::SceneCameraPathClip {
        .fps    = 30.0f,
        .length = i32(30),
        .eye    = Some(rstd::move(eye)),
        .fov    = Some(rstd::move(fov)),
    });
    path.queue.push(owe::SceneCameraPathClip { .fps = 30.0f, .length = i32(30) });

    ASSERT_TRUE(path.Tick(0.0));
    EXPECT_DOUBLE_EQ(camera->Transforms().eye.z(), 5.0);
    EXPECT_DOUBLE_EQ(camera->Fov(), 45.0);

    ASSERT_TRUE(path.Tick(0.5));
    EXPECT_DOUBLE_EQ(camera->Transforms().eye.z(), 6.0);
    EXPECT_DOUBLE_EQ(camera->Fov(), 52.5);

    ASSERT_TRUE(path.Tick(1.0));
    EXPECT_DOUBLE_EQ(camera->Transforms().eye.z(), 7.0);
    EXPECT_DOUBLE_EQ(camera->Fov(), 60.0);

    ASSERT_TRUE(path.Tick(1.5));
    EXPECT_DOUBLE_EQ(camera->Transforms().eye.z(), 7.0);
    EXPECT_DOUBLE_EQ(camera->Fov(), 60.0);
}

TEST(SceneCameraPath, OrthographicQueueUsesAuthoredFrameAndLoopsHoldClip) {
    auto camera = Arc<owe::SceneCamera>::make(
        owe::SceneCamera::MakeOrthographic(1920.0, 1080.0, -5000.0, 5000.0));
    auto node = Arc<owe::SceneNode>::make();
    node->SetTranslate({ 960.0f, 540.0f, 500.0f });
    camera->AttatchNode(node.as_ptr());
    camera->Update();

    auto eye = Arc<owe::SceneAnimationCurve>::make();
    eye->c0.push({ .frame = i32(), .value = -320.0f });
    eye->c0.push({ .frame = i32(120), .value = 0.0f });
    auto center = eye.clone();
    auto zoom   = Arc<owe::SceneAnimationCurve>::make();
    zoom->c0.push({ .frame = i32(), .value = 4.0f });
    zoom->c0.push({ .frame = i32(120), .value = 1.0f });

    owe::SceneCameraPath path;
    path.camera            = Some(camera.clone());
    path.node              = node.as_ptr();
    path.default_translate = node->Translate();
    path.CaptureViewport();
    path.queue.push(owe::SceneCameraPathClip {
        .fps    = 30.0f,
        .length = i32(120),
        .eye    = Some(rstd::move(eye)),
        .center = Some(rstd::move(center)),
        .zoom   = Some(rstd::move(zoom)),
    });
    path.queue.push(owe::SceneCameraPathClip {
        .fps    = 30.0f,
        .length = i32(60),
        .loop   = true,
    });

    ASSERT_TRUE(path.Tick(0.0));
    EXPECT_DOUBLE_EQ(camera->Transforms().eye.x(), 640.0);
    EXPECT_DOUBLE_EQ(camera->Transforms().center.x(), 640.0);
    EXPECT_DOUBLE_EQ(camera->Transforms().eye.y(), 540.0);
    EXPECT_DOUBLE_EQ(camera->Width(), 480.0);
    ASSERT_TRUE(path.Tick(2.0));
    EXPECT_DOUBLE_EQ(camera->Transforms().eye.x(), 800.0);
    EXPECT_DOUBLE_EQ(camera->Width(), 768.0);
    ASSERT_TRUE(path.Tick(4.0));
    EXPECT_DOUBLE_EQ(camera->Transforms().eye.x(), 960.0);
    EXPECT_DOUBLE_EQ(camera->Width(), 1920.0);
    ASSERT_TRUE(path.Tick(8.0));
    EXPECT_DOUBLE_EQ(camera->Transforms().eye.x(), 960.0);
    EXPECT_DOUBLE_EQ(camera->Transforms().eye.y(), 540.0);
    EXPECT_DOUBLE_EQ(camera->Width(), 1920.0);
    ASSERT_TRUE(path.Tick(100.0));
    EXPECT_DOUBLE_EQ(camera->Transforms().eye.x(), 960.0);

    path.SetEnabled(false);
    ASSERT_TRUE(path.Tick(101.0));
    EXPECT_DOUBLE_EQ(camera->Transforms().eye.x(), 960.0);
    EXPECT_DOUBLE_EQ(camera->Width(), 1920.0);
    path.SetEnabled(true);
    ASSERT_TRUE(path.Tick(102.0));
    EXPECT_DOUBLE_EQ(camera->Transforms().eye.x(), 640.0);
    EXPECT_DOUBLE_EQ(camera->Width(), 480.0);
}

TEST(SceneCameraPath, LoopingClipWrapsWithoutAdvancingQueue) {
    auto camera = Arc<owe::SceneCamera>::make(
        owe::SceneCamera::MakePerspective(16.0 / 9.0, 0.01, 1000.0, 45.0));
    camera->SetLookAt({ 0.0, 0.0, 5.0 }, { 0.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 });
    camera->Update();
    auto eye = Arc<owe::SceneAnimationCurve>::make();
    eye->c2.push({ .frame = i32(), .value = 5.0f });
    eye->c2.push({ .frame = i32(30), .value = 9.0f });

    owe::SceneCameraPath path;
    path.camera      = Some(camera.clone());
    path.perspective = true;
    path.CaptureViewport();
    path.queue.push(owe::SceneCameraPathClip {
        .fps    = 30.0f,
        .length = i32(30),
        .loop   = true,
        .eye    = Some(rstd::move(eye)),
    });
    path.queue.push(owe::SceneCameraPathClip { .fps = 30.0f, .length = i32(300) });

    ASSERT_TRUE(path.Tick(0.0));
    ASSERT_TRUE(path.Tick(0.5));
    EXPECT_DOUBLE_EQ(camera->Transforms().eye.z(), 7.0);
    ASSERT_TRUE(path.Tick(1.0));
    EXPECT_DOUBLE_EQ(camera->Transforms().eye.z(), 5.0);
    ASSERT_TRUE(path.Tick(12.25));
    EXPECT_NEAR(camera->Transforms().eye.z(), 6.0, 0.0001);
    ASSERT_TRUE(path.Tick(0.0));
    EXPECT_DOUBLE_EQ(camera->Transforms().eye.z(), 5.0);
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

    auto disable = owe::ParseJson(R"({"value":false})"_str).unwrap();
    state->ApplyUserProperty("cameraparallax"_str, disable);
    EXPECT_FALSE(state->CameraParallax().enable);

    auto enable = owe::ParseJson(R"({"value":true})"_str).unwrap();
    state->ApplyUserProperty("cameraparallax"_str, enable);
    EXPECT_TRUE(state->CameraParallax().enable);

    auto amount = owe::ParseJson(R"({"value":0.25})"_str).unwrap();
    state->ApplyUserProperty("cameraparallaxamount"_str, amount);
    EXPECT_FLOAT_EQ(state->CameraParallax().amount, 0.25f);

    for (auto source : { R"({"value":"0.5tail"})"_str, R"({"value":"1e999"})"_str }) {
        auto invalid = owe::ParseJson(source).unwrap();
        state->ApplyUserProperty("cameraparallaxamount"_str, invalid);
        EXPECT_FLOAT_EQ(state->CameraParallax().amount, 0.25f);
    }
    auto numeric_text = owe::ParseJson(R"({"value":"0.5"})"_str).unwrap();
    state->ApplyUserProperty("cameraparallaxamount"_str, numeric_text);
    EXPECT_FLOAT_EQ(state->CameraParallax().amount, 0.5f);

    auto delay = owe::ParseJson(R"({"value":0.5})"_str).unwrap();
    state->ApplyUserProperty("cameraparallaxdelay"_str, delay);
    EXPECT_FLOAT_EQ(state->CameraParallax().delay, 0.5f);

    auto influence = owe::ParseJson(R"({"value":0.75})"_str).unwrap();
    state->ApplyUserProperty("cameraparallaxmouseinfluence"_str, influence);
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
    auto mesh   = Arc<owe::SceneMesh>::make();
    mesh->AddMaterial(owe::SceneMaterial {});
    owe::SceneMesh::Submesh submesh;
    submesh.material_slot = u32();
    mesh->Submeshes().push(std::move(submesh));
    child->AddMesh(mesh.clone());
    parent->AppendChild(child.clone());
    scene.RootMut()->AppendChild(parent.clone());
    scene.RebuildResourceIndex();
    effect->SetParentAnchor(child.as_ptr());

    auto state = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    state->SetOrthographicImplicitParallax(true);
    state->CameraParallax() = { true, 0.03f, 0.0f, 0.36f };
    state->SetOrtho(3840.0f, 2160.0f);
    state->SetPointerInput(0.0, 1.0);
    state->Advance(owe::SceneFrame {});

    auto camera_resolver = Arc<owe::UniformCameraResolver>::make(camera.clone());
    camera_resolver->Add(String::make("default"_str), camera.clone());

    auto parent_state = Arc<owe::UniformNodeState>::make(parent.clone(), camera_resolver.clone());
    parent_state->object_id         = i32(1);
    parent_state->parallax.depth    = { -1.56f, -0.79f };
    parent_state->parallax.authored = true;
    auto child_state = Arc<owe::UniformNodeState>::make(child.clone(), camera_resolver.clone());
    child_state->object_id         = i32(2);
    child_state->parallax.depth    = { 1.0f, 1.0f };
    child_state->parallax.authored = true;
    auto effect_state = Arc<owe::UniformNodeState>::make(effect.clone(), camera_resolver.clone());
    effect_state->object_id              = i32(2);
    effect_state->parallax.depth         = { 1.0f, 1.0f };
    effect_state->parallax.authored      = true;
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

    child_state->parallax.depth = { 0.0f, 0.0f };
    mvp                         = capture_mvp();
    EXPECT_NEAR(mvp[rstd::usize(12)], expected_inherited.x(), 1e-5f);
    EXPECT_NEAR(mvp[rstd::usize(13)], expected_inherited.y(), 1e-5f);

    child_state->parallax.depth = { 0.5f, 0.5f };
    mvp                         = capture_mvp();
    EXPECT_NEAR(mvp[rstd::usize(12)], expected_inherited.x(), 1e-5f);
    EXPECT_NEAR(mvp[rstd::usize(13)], expected_inherited.y(), 1e-5f);

    parent_state->parallax.depth = { 0.0f, 0.0f };
    child_state->parallax.depth  = { -0.7f, -0.7f };
    mvp                          = capture_mvp();
    auto expected_frozen         = expected_translation({ 1982.0f, 1053.0f }, { 0.0f, 0.0f });
    EXPECT_NEAR(mvp[rstd::usize(12)], expected_frozen.x(), 1e-5f);
    EXPECT_NEAR(mvp[rstd::usize(13)], expected_frozen.y(), 1e-5f);

    parent_state->parallax.depth  = { 0.321f, 0.321f };
    child_state->parallax.depth   = { 0.321f, 0.321f };
    mvp                           = capture_mvp();
    auto expected_parent_authored = expected_translation({ 1982.0f, 1053.0f }, { 0.321f, 0.321f });
    EXPECT_NEAR(mvp[rstd::usize(12)], expected_parent_authored.x(), 1e-5f);
    EXPECT_NEAR(mvp[rstd::usize(13)], expected_parent_authored.y(), 1e-5f);

    auto layer_camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(3840, 2160, -1.0, 1.0));
    layer_camera->AttatchNode(effect.as_ptr());
    effect->AttachLayer(Arc<owe::SceneNodeLayer>::make(
        child.as_ptr(), 3840.0f, 2160.0f, "_rt_effect_composite_test"_str));
    scene.RegisterCamera(String::make("layer"_str), layer_camera.clone());
    camera_resolver->Add(String::make("layer"_str), layer_camera.clone());
    effect->SetCamera("layer"_str);
    mvp = capture_mvp();
    EXPECT_NEAR(mvp[rstd::usize(12)], 0.0f, 1e-5f);
    EXPECT_NEAR(mvp[rstd::usize(13)], 0.0f, 1e-5f);
    child->SetCamera("layer"_str);
    owe::TransformUniformSource child_source(state.clone(), child_state.clone());
    for (bool authored : { false, true }) {
        child_state->parallax.authored = authored;
        state->CameraParallax().enable = false;
        const auto without_parallax =
            scene_test::Capture(scene.Runtime().Frame(),
                                child_source,
                                owe::TransformUniformOutput::ModelViewProjection);
        state->CameraParallax().enable = true;
        const auto with_parallax =
            scene_test::Capture(scene.Runtime().Frame(),
                                child_source,
                                owe::TransformUniformOutput::ModelViewProjection);
        ASSERT_EQ(with_parallax.size(), without_parallax.size());
        for (rstd::usize i {}; i < with_parallax.size(); ++i)
            EXPECT_NEAR(with_parallax[i], without_parallax[i], 1e-5f);
    }
}

TEST(UniformSourceParallax, InactiveEffectDoesNotReplaceLayerOrigin) {
    auto camera_node = Arc<owe::SceneNode>::make(Eigen::Vector3f { 1920.0f, 1080.0f, 0.0f },
                                                 Eigen::Vector3f::Ones(),
                                                 Eigen::Vector3f::Zero());
    auto camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(3840, 2160, -1.0, 1.0));
    camera->AttatchNode(camera_node.as_ptr());
    auto resolver = Arc<owe::UniformCameraResolver>::make(camera.clone());
    auto state    = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    state->SetOrtho(3840.0f, 2160.0f);
    state->CameraParallax() = { true, 0.13f, 0.0f, 0.5f };
    state->SetPointerInput(0.5, 0.5);
    state->Advance(owe::SceneFrame {});

    auto layer       = Arc<owe::SceneNode>::make(Eigen::Vector3f { 2667.97363f, 1349.58765f, 0.0f },
                                                 Eigen::Vector3f::Ones(),
                                                 Eigen::Vector3f::Zero());
    auto effect      = Arc<owe::SceneNode>::make();
    auto layer_state = Arc<owe::UniformNodeState>::make(layer.clone(), resolver.clone());
    auto effect_state      = Arc<owe::UniformNodeState>::make(effect.clone(), resolver.clone());
    layer_state->object_id = effect_state->object_id = i32(887);
    layer_state->parallax = effect_state->parallax = { { 0.6f, 0.6f }, true };
    effect_state->effect_projection_node           = Some(layer.clone());
    state->SetNodeState({ .index = u32(1), .generation = u32(1) }, effect_state.clone());
    state->SetNodeState({ .index = u32(2), .generation = u32(1) }, layer_state.clone());

    for (auto effect_camera : { ""_str, "effect"_str }) {
        effect->SetCamera(effect_camera);
        for (auto layer_camera : { ""_str, "global_perspective"_str }) {
            layer->SetCamera(layer_camera);
            for (const auto source : { layer_state.as_ptr(), effect_state.as_ptr() }) {
                const auto offset = state->ComputeParallaxOffset(
                    *source, *camera, owe::SceneRenderViewKind::Primary);
                EXPECT_NEAR(offset[usize()], (2667.97363f - 1920.0f) * 0.6f * 0.13f, 1e-4f);
                EXPECT_NEAR(offset[usize(1)], (1349.58765f - 1080.0f) * 0.6f * 0.13f, 1e-4f);
            }
        }
    }
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
    auto mesh  = Arc<owe::SceneMesh>::make();
    mesh->AddMaterial(owe::SceneMaterial {});
    owe::SceneMesh::Submesh submesh;
    submesh.material_slot = u32();
    mesh->Submeshes().push(std::move(submesh));
    layer->AddMesh(mesh.clone());
    scene.RootMut()->AppendChild(layer.clone());
    scene.RebuildResourceIndex();

    auto state = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    state->SetOrthographicImplicitParallax(true);
    state->CameraParallax() = { true, 0.5f, 0.0f, 0.36f };
    state->SetOrtho(1920.0f, 1080.0f);
    state->SetPointerInput(0.0, 1.0);
    state->Advance(owe::SceneFrame {});

    auto camera_resolver = Arc<owe::UniformCameraResolver>::make(camera.clone());
    camera_resolver->Add(String::make("default"_str), camera.clone());

    auto layer_state = Arc<owe::UniformNodeState>::make(layer.clone(), camera_resolver.clone());
    layer_state->object_id         = i32(1);
    layer_state->parallax.depth    = { 0.0f, 0.0f };
    layer_state->parallax.authored = false;
    state->SetNodeState({ .index = rstd::u32(1), .generation = rstd::u32(1) }, layer_state.clone());

    owe::TransformUniformSource source(state.clone(), layer_state.clone());
    state->CameraParallax().enable = false;
    auto mvp_without_parallax      = scene_test::Capture(
        scene.Runtime().Frame(), source, owe::TransformUniformOutput::ModelViewProjection);
    state->CameraParallax().enable = true;
    auto mvp_with_parallax         = scene_test::Capture(
        scene.Runtime().Frame(), source, owe::TransformUniformOutput::ModelViewProjection);
    ASSERT_GT(mvp_without_parallax.size().to_primitive(), 13u);
    ASSERT_GT(mvp_with_parallax.size().to_primitive(), 13u);
    EXPECT_TRUE(std::abs(mvp_without_parallax[rstd::usize(12)] -
                         mvp_with_parallax[rstd::usize(12)]) > 1e-5f ||
                std::abs(mvp_without_parallax[rstd::usize(13)] -
                         mvp_with_parallax[rstd::usize(13)]) > 1e-5f);
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
    auto child     = Arc<owe::SceneNode>::make(Eigen::Vector3f { -76.0f, -3.0f, 0.0f },
                                               Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
                                               Eigen::Vector3f::Zero());
    auto mesh      = Arc<owe::SceneMesh>::make();
    mesh->AddMaterial(owe::SceneMaterial {});
    owe::SceneMesh::Submesh submesh;
    submesh.material_slot = u32();
    mesh->Submeshes().push(std::move(submesh));
    child->AddMesh(mesh.clone());
    container->AppendChild(child.clone());
    scene.RootMut()->AppendChild(container.clone());
    scene.RebuildResourceIndex();

    auto state = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    state->SetOrthographicImplicitParallax(true);
    state->CameraParallax() = { true, 0.03f, 0.0f, 0.36f };
    state->SetOrtho(3840.0f, 2160.0f);
    state->SetPointerInput(0.0, 1.0);
    state->Advance(owe::SceneFrame {});

    auto camera_resolver = Arc<owe::UniformCameraResolver>::make(camera.clone());
    camera_resolver->Add(String::make("default"_str), camera.clone());

    auto child_state = Arc<owe::UniformNodeState>::make(child.clone(), camera_resolver.clone());
    child_state->object_id         = i32(2);
    child_state->parallax.depth    = { -1.12f, -1.36f };
    child_state->parallax.authored = true;
    state->SetNodeState({ .index = rstd::u32(2), .generation = rstd::u32(1) }, child_state.clone());

    owe::TransformUniformSource source(state.clone(), child_state.clone());
    auto                        mvp = scene_test::Capture(
        scene.Runtime().Frame(), source, owe::TransformUniformOutput::ModelViewProjection);

    const Eigen::Vector2f camera_pos { 1920.0f, 1080.0f };
    const Eigen::Vector2f mouse_vec { 691.2f, 388.8f };
    const Eigen::Vector2f depth { -1.12f, -1.36f };
    const Eigen::Vector2f base { 1906.0f, 1050.0f };
    const Eigen::Vector2f offset    = (base - camera_pos + mouse_vec).cwiseProduct(depth) * 0.03f;
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
    auto mesh   = Arc<owe::SceneMesh>::make();
    mesh->AddMaterial(owe::SceneMaterial {});
    owe::SceneMesh::Submesh submesh;
    submesh.material_slot = u32();
    mesh->Submeshes().push(std::move(submesh));
    child->AddMesh(mesh.clone());
    parent->AppendChild(child.clone());
    scene.RootMut()->AppendChild(parent.clone());
    scene.RebuildResourceIndex();

    auto state = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    state->SetOrthographicImplicitParallax(true);
    state->CameraParallax() = { true, 0.03f, 0.0f, 0.36f };
    state->SetOrtho(3840.0f, 2160.0f);
    state->SetPointerInput(0.0, 1.0);
    state->Advance(owe::SceneFrame {});

    auto camera_resolver = Arc<owe::UniformCameraResolver>::make(camera.clone());
    camera_resolver->Add(String::make("default"_str), camera.clone());

    auto parent_state = Arc<owe::UniformNodeState>::make(parent.clone(), camera_resolver.clone());
    parent_state->object_id                      = i32(1);
    parent_state->parallax.depth                 = { -1.56f, -0.79f };
    parent_state->parallax.authored              = true;
    parent_state->propagate_parallax_to_children = false;
    auto child_state = Arc<owe::UniformNodeState>::make(child.clone(), camera_resolver.clone());
    child_state->object_id         = i32(2);
    child_state->parallax.depth    = { 0.0f, 0.0f };
    child_state->parallax.authored = false;
    state->SetNodeState({ .index = rstd::u32(1), .generation = rstd::u32(1) },
                        parent_state.clone());
    state->SetNodeState({ .index = rstd::u32(2), .generation = rstd::u32(1) }, child_state.clone());

    owe::TransformUniformSource source(state.clone(), child_state.clone());
    auto                        capture_mvp = [&]() {
        return scene_test::Capture(
            scene.Runtime().Frame(), source, owe::TransformUniformOutput::ModelViewProjection);
    };

    parent_state->propagate_parallax_to_children = true;
    auto mvp_inherited                           = capture_mvp();

    parent_state->propagate_parallax_to_children = false;
    auto mvp_blocked                             = capture_mvp();

    ASSERT_GT(mvp_inherited.size().to_primitive(), 13u);
    ASSERT_GT(mvp_blocked.size().to_primitive(), 13u);
    EXPECT_TRUE(std::abs(mvp_inherited[rstd::usize(12)] - mvp_blocked[rstd::usize(12)]) > 1e-5f ||
                std::abs(mvp_inherited[rstd::usize(13)] - mvp_blocked[rstd::usize(13)]) > 1e-5f);
}

TEST(UniformSourceParallax, PerspectiveWithoutAuthoredParallaxDepthSkipsShift) {
    owe::Scene scene;
    scene.SetOrtho({ i32(1920), i32(1080) });

    auto camera = Arc<owe::SceneCamera>::make(
        owe::SceneCamera::MakePerspective(16.0 / 9.0, 0.1, 10000.0, 60.0));
    camera->SetLookAt(Eigen::Vector3d { -1.69, 0.62, 9.29 },
                      Eigen::Vector3d { -1.58, 0.60, 8.30 },
                      Eigen::Vector3d::UnitY());
    scene.RegisterCamera(String::make("default"_str), camera.clone());
    ASSERT_TRUE(scene.SetActiveCamera("default"_str));

    auto layer = Arc<owe::SceneNode>::make(Eigen::Vector3f { 0.0f, 0.0f, 3.0f },
                                           Eigen::Vector3f { 0.0025f, 0.0025f, 5.0f },
                                           Eigen::Vector3f::Zero());
    auto mesh  = Arc<owe::SceneMesh>::make();
    mesh->AddMaterial(owe::SceneMaterial {});
    owe::SceneMesh::Submesh submesh;
    submesh.material_slot = u32();
    mesh->Submeshes().push(std::move(submesh));
    layer->AddMesh(mesh.clone());
    scene.RootMut()->AppendChild(layer.clone());
    scene.RebuildResourceIndex();

    auto state = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    state->SetOrthographicImplicitParallax(false);
    state->CameraParallax() = { true, 2.0f, 0.0f, 1.0f };
    state->SetOrtho(1920.0f, 1080.0f);
    state->SetPointerInput(0.0, 1.0);
    state->Advance(owe::SceneFrame {});

    auto camera_resolver = Arc<owe::UniformCameraResolver>::make(camera.clone());
    camera_resolver->Add(String::make("default"_str), camera.clone());

    auto layer_state = Arc<owe::UniformNodeState>::make(layer.clone(), camera_resolver.clone());
    layer_state->object_id         = i32(31);
    layer_state->parallax.depth    = { 0.0f, 0.0f };
    layer_state->parallax.authored = false;
    state->SetNodeState({ .index = rstd::u32(1), .generation = rstd::u32(1) }, layer_state.clone());

    owe::TransformUniformSource source(state.clone(), layer_state.clone());
    state->CameraParallax().enable = false;
    auto mvp_without_parallax      = scene_test::Capture(
        scene.Runtime().Frame(), source, owe::TransformUniformOutput::ModelViewProjection);
    state->CameraParallax().enable = true;
    auto mvp_with_parallax         = scene_test::Capture(
        scene.Runtime().Frame(), source, owe::TransformUniformOutput::ModelViewProjection);
    ASSERT_GT(mvp_without_parallax.size().to_primitive(), 13u);
    ASSERT_GT(mvp_with_parallax.size().to_primitive(), 13u);
    EXPECT_NEAR(mvp_without_parallax[rstd::usize(12)], mvp_with_parallax[rstd::usize(12)], 1e-5f);
    EXPECT_NEAR(mvp_without_parallax[rstd::usize(13)], mvp_with_parallax[rstd::usize(13)], 1e-5f);
}

TEST(UniformSourceParallax, PerspectiveWithAuthoredParallaxDepthAppliesShift) {
    owe::Scene scene;
    scene.SetOrtho({ i32(1920), i32(1080) });

    auto camera = Arc<owe::SceneCamera>::make(
        owe::SceneCamera::MakePerspective(16.0 / 9.0, 0.1, 10000.0, 60.0));
    camera->SetLookAt(
        Eigen::Vector3d { 0.0, 0.0, 5.0 }, Eigen::Vector3d::Zero(), Eigen::Vector3d::UnitY());
    scene.RegisterCamera(String::make("default"_str), camera.clone());
    ASSERT_TRUE(scene.SetActiveCamera("default"_str));

    auto layer = Arc<owe::SceneNode>::make(
        Eigen::Vector3f { 1.0f, 2.0f, -3.0f }, Eigen::Vector3f::Ones(), Eigen::Vector3f::Zero());
    auto mesh = Arc<owe::SceneMesh>::make();
    mesh->AddMaterial(owe::SceneMaterial {});
    owe::SceneMesh::Submesh submesh;
    submesh.material_slot = u32();
    mesh->Submeshes().push(std::move(submesh));
    layer->AddMesh(mesh.clone());
    scene.RootMut()->AppendChild(layer.clone());
    scene.RebuildResourceIndex();

    auto state = Arc<owe::UniformSceneState>::make(Arc<owe::AudioResponseDemand>::make());
    state->SetOrthographicImplicitParallax(false);
    state->CameraParallax() = { true, 0.5f, 0.0f, 1.0f };
    state->SetOrtho(1920.0f, 1080.0f);
    state->SetPointerInput(0.0, 1.0);
    state->Advance(owe::SceneFrame {});

    auto camera_resolver = Arc<owe::UniformCameraResolver>::make(camera.clone());
    camera_resolver->Add(String::make("default"_str), camera.clone());

    auto layer_state = Arc<owe::UniformNodeState>::make(layer.clone(), camera_resolver.clone());
    layer_state->object_id         = i32(1);
    layer_state->parallax.depth    = { 1.0f, 1.0f };
    layer_state->parallax.authored = true;
    state->SetNodeState({ .index = rstd::u32(1), .generation = rstd::u32(1) }, layer_state.clone());

    owe::TransformUniformSource source(state.clone(), layer_state.clone());
    state->CameraParallax().enable = false;
    auto mvp_without_parallax      = scene_test::Capture(
        scene.Runtime().Frame(), source, owe::TransformUniformOutput::ModelViewProjection);
    state->CameraParallax().enable = true;
    auto mvp_with_parallax         = scene_test::Capture(
        scene.Runtime().Frame(), source, owe::TransformUniformOutput::ModelViewProjection);
    ASSERT_GT(mvp_without_parallax.size().to_primitive(), 13u);
    ASSERT_GT(mvp_with_parallax.size().to_primitive(), 13u);
    EXPECT_TRUE(std::abs(mvp_without_parallax[rstd::usize(12)] -
                         mvp_with_parallax[rstd::usize(12)]) > 1e-5f ||
                std::abs(mvp_without_parallax[rstd::usize(13)] -
                         mvp_with_parallax[rstd::usize(13)]) > 1e-5f);
}
