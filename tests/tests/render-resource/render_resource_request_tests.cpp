#include <vulkan/vulkan_core.h>

#include <cstring>

#include <rstd/test/gtest.hpp>

import rstd;
import rstd.cppstd;
import wescene.scene;
import wescene.types;
import wescene.vulkan;
import wescene.vulkan_render;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

namespace uniform_test
{

struct StaticSourceState {
    float     value { 0.0f };
    rstd::u64 version { 1 };
};

class BufferWriter {
public:
    auto UpdateBuffer(owe::resource::BufferUseHandle use, rstd::slice<rstd::u8> content)
        -> rstd::Result<rstd::empty, owe::resource::ResourceError> {
        last_use = use;
        ++update_count;
        bytes.clear();
        bytes.reserve(content.len().to_primitive());
        for (rstd::usize index; index < content.len(); ++index) {
            bytes.push_back(content[index]);
        }
        return rstd::Ok(rstd::empty {});
    }

    owe::resource::BufferUseHandle last_use;
    rstd::u64                      update_count { 0 };
    std::vector<rstd::u8>          bytes;
};

class StaticSource {
public:
    StaticSource(std::string name, float value)
        : StaticSource(std::move(name),
                       std::make_shared<StaticSourceState>(StaticSourceState { .value = value })) {}
    StaticSource(std::string name, std::shared_ptr<StaticSourceState> state)
        : m_name(std::move(name)), m_state(std::move(state)) {}
    StaticSource(std::string name, float value, rstd::sync::Arc<owe::AudioResponseDemand> demand)
        : StaticSource(std::move(name), value) {
        m_demand = rstd::Some(rstd::move(demand));
    }

    auto Describe(rstd::mut_ref<rstd::dyn<owe::UniformBindingSink>> sink) const
        -> rstd::Result<rstd::empty, owe::UniformError> {
        auto result = sink->Bind(m_output,
                                 rstd::cppstd::as_str(m_name).unwrap(),
                                 owe::UniformValueShape::FloatRange(rstd::u32(1), rstd::u32(4)));
        if (result.is_err()) return rstd::Err(std::move(result).unwrap_err_unchecked());
        return rstd::Ok(rstd::empty {});
    }
    auto Version(rstd::ref<rstd::dyn<owe::UniformUpdateContext>>) const -> rstd::u64 {
        return m_state->version;
    }
    auto Evaluate(rstd::ref<rstd::dyn<owe::UniformUpdateContext>>,
                  rstd::mut_ref<rstd::dyn<owe::UniformValueSink>> sink) const
        -> rstd::Result<rstd::empty, owe::UniformError> {
        if (! sink->Wants(m_output)) return rstd::Ok(rstd::empty {});
        auto value = owe::UniformValue(m_state->value);
        return sink->Write(m_output, value.View());
    }
    auto AcquireBindingLease() const
        -> rstd::Option<rstd::boxed::Box<rstd::dyn<owe::UniformBindingLease>>> {
        if (m_demand.is_none()) return rstd::None();
        return rstd::Some((*m_demand)->Acquire());
    }

private:
    std::string                                             m_name;
    std::shared_ptr<StaticSourceState>                      m_state;
    rstd::Option<rstd::sync::Arc<owe::AudioResponseDemand>> m_demand;
    owe::UniformOutputId                                    m_output { .value = rstd::u32() };
};

class TextureMetadataSource {
public:
    auto Describe(rstd::mut_ref<rstd::dyn<owe::UniformBindingSink>> sink) const
        -> rstd::Result<rstd::empty, owe::UniformError> {
        auto result =
            sink->Bind(m_output, "texture_extent"_str, owe::UniformValueShape::Float(rstd::u32(4)));
        if (result.is_err()) return rstd::Err(rstd::move(result).unwrap_err_unchecked());
        return rstd::Ok(rstd::empty {});
    }
    auto Version(rstd::ref<rstd::dyn<owe::UniformUpdateContext>> context) const -> rstd::u64 {
        return context->Frame()->revision;
    }
    auto Evaluate(rstd::ref<rstd::dyn<owe::UniformUpdateContext>> context,
                  rstd::mut_ref<rstd::dyn<owe::UniformValueSink>> sink) const
        -> rstd::Result<rstd::empty, owe::UniformError> {
        if (! sink->Wants(m_output)) return rstd::Ok(rstd::empty {});
        auto texture = context->Resources()->Texture(rstd::usize());
        if (texture.is_none() || ! texture->has_extent) return rstd::Ok(rstd::empty {});
        auto value = owe::UniformValue(rstd::array<float, 4> {
            texture->source_extent[rstd::usize()],
            texture->source_extent[rstd::usize(1)],
            texture->sample_extent[rstd::usize()],
            texture->sample_extent[rstd::usize(1)],
        });
        return sink->Write(m_output, value.View());
    }
    auto AcquireBindingLease() const
        -> rstd::Option<rstd::boxed::Box<rstd::dyn<owe::UniformBindingLease>>> {
        return rstd::None();
    }

private:
    owe::UniformOutputId m_output { .value = rstd::u32() };
};

} // namespace uniform_test

namespace
{

std::vector<std::byte> Bytes(std::initializer_list<unsigned char> values) {
    std::vector<std::byte> bytes;
    bytes.reserve(values.size());
    for (auto value : values) bytes.push_back(static_cast<std::byte>(value));
    return bytes;
}

VkImageView ImageView(std::uintptr_t value) { return reinterpret_cast<VkImageView>(value); }

owe::vulkan::FramebufferAttachmentIdentity
AttachmentIdentity(std::size_t value, std::initializer_list<unsigned char> bytes) {
    return owe::vulkan::FramebufferAttachmentIdentity {
        .value = value,
        .bytes = Bytes(bytes),
    };
}

owe::vulkan::FramebufferAttachmentDesc Attachment(std::uintptr_t view, std::size_t identity,
                                                  std::initializer_list<unsigned char> bytes) {
    return owe::vulkan::FramebufferAttachmentDesc {
        .view     = ImageView(view),
        .identity = AttachmentIdentity(identity, bytes),
    };
}

std::shared_ptr<owe::SceneMesh> MakeUniformMesh(std::shared_ptr<owe::SceneShader> shader) {
    auto               mesh = std::make_shared<owe::SceneMesh>();
    owe::SceneMaterial material;
    material.customShader.shader = std::move(shader);
    mesh->AddMaterial(std::move(material));
    owe::SceneMesh::Submesh submesh;
    submesh.material_slot = u32();
    mesh->Submeshes().push_back(std::move(submesh));
    return mesh;
}

float ReadFloat(const rstd::vec::Vec<rstd::u8>& bytes, rstd::usize offset) {
    float value {};
    std::memcpy(&value, bytes.data() + offset.to_primitive(), sizeof(value));
    return value;
}

} // namespace

TEST(PassCommon, ConfiguresAlphaToCoverageWithoutColorBlending) {
    VkPipelineColorBlendAttachmentState blend {};
    owe::vulkan::SetBlend(owe::BlendMode::AlphaToCoverage, blend);
    EXPECT_FALSE(blend.blendEnable);

    VkPipelineMultisampleStateCreateInfo multisample {};
    owe::vulkan::SetAlphaToCoverage(owe::BlendMode::AlphaToCoverage, multisample);
    EXPECT_TRUE(multisample.alphaToCoverageEnable);

    VkAttachmentLoadOp load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    owe::vulkan::SetAttachmentLoadOp(owe::BlendMode::AlphaToCoverage, load_op);
    EXPECT_EQ(load_op, VK_ATTACHMENT_LOAD_OP_LOAD);

    owe::SceneMaterial material;
    material.SetBlendMode(owe::BlendMode::AlphaToCoverage);
    material.SetDepthWrite(true);
    EXPECT_TRUE(owe::vulkan::EffectiveDepthWrite(material.Pipeline()));
}

TEST(UniformBufferLayout, PreservesReflectedSlots) {
    auto members = rstd::vec::Vec<owe::resource::ShaderArtifactUniformMember>::make();
    members.push(owe::resource::ShaderArtifactUniformMember {
        .name   = rstd::string::String::make("g_Time"_str),
        .offset = rstd::u32(),
        .size   = rstd::usize(4),
    });
    members.push(owe::resource::ShaderArtifactUniformMember {
        .name   = rstd::string::String::make("time_alias"_str),
        .offset = rstd::u32(16),
        .size   = rstd::usize(4),
    });
    members.push(owe::resource::ShaderArtifactUniformMember {
        .name   = rstd::string::String::make("unmapped"_str),
        .offset = rstd::u32(32),
        .size   = rstd::usize(4),
    });
    auto block = owe::resource::ShaderArtifactUniformBlock {
        .name    = rstd::string::String::make("Globals"_str),
        .size    = rstd::usize(48),
        .members = rstd::move(members),
    };
    auto result = owe::vulkan::CompileUniformBufferLayout(block);

    ASSERT_TRUE(result.is_ok());
    auto layout = std::move(result).unwrap_unchecked();
    ASSERT_EQ(layout.slots.len(), rstd::usize(3));
    EXPECT_EQ(rstd::cppstd::as_string_view(layout.slots[rstd::usize()].name.as_str()), "g_Time");
    EXPECT_EQ(rstd::cppstd::as_string_view(layout.slots[rstd::usize(1)].name.as_str()),
              "time_alias");
    EXPECT_EQ(rstd::cppstd::as_string_view(layout.slots[rstd::usize(2)].name.as_str()), "unmapped");
}

TEST(UniformBufferLayout, RejectsMemberOutsideBlock) {
    auto members = rstd::vec::Vec<owe::resource::ShaderArtifactUniformMember>::make();
    members.push(owe::resource::ShaderArtifactUniformMember {
        .name   = rstd::string::String::make("outside"_str),
        .offset = rstd::u32(8),
        .size   = rstd::usize(8),
    });
    auto block = owe::resource::ShaderArtifactUniformBlock {
        .name    = rstd::string::String::make("Globals"_str),
        .size    = rstd::usize(12),
        .members = rstd::move(members),
    };

    EXPECT_TRUE(owe::vulkan::CompileUniformBufferLayout(block).is_err());
}

TEST(UniformBufferSerializer, PacksColumnVectorMatrixUsingReflectedStride) {
    const rstd::array<float, 9> values { 1.0f, 4.0f, 7.0f, 2.0f, 5.0f, 8.0f, 3.0f, 6.0f, 9.0f };
    auto value = owe::UniformValue::fromMatrixArray(values.data(),
                                                    rstd::u32(3),
                                                    rstd::u32(3),
                                                    rstd::usize(1),
                                                    owe::UniformMatrixStorage::ColumnMajor);
    auto bytes = rstd::vec::Vec<rstd::u8>::make();
    bytes.resize(rstd::usize(48), rstd::u8(0xff));
    auto slot = owe::vulkan::UniformSlot {
        .name              = rstd::string::String::make("matrix"_str),
        .size              = rstd::usize(48),
        .scalar_kind       = owe::ShaderScalarKind::Float,
        .scalar_width      = rstd::u32(32),
        .vector_components = rstd::u32(3),
        .matrix_rows       = rstd::u32(3),
        .matrix_columns    = rstd::u32(3),
        .matrix_stride     = rstd::u32(16),
        .matrix_major      = owe::ShaderMatrixMajor::Row,
    };

    auto result = owe::vulkan::SerializeUniformValue(bytes.as_mut_slice().as_mut_ref(),
                                                     slot,
                                                     value.View(),
                                                     owe::ShaderMatrixConvention::ColumnVector);

    ASSERT_TRUE(result.is_ok());
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(0)), 1.0f);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(4)), 2.0f);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(8)), 3.0f);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(12)), 0.0f);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(16)), 4.0f);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(32)), 7.0f);
}

TEST(UniformBufferSerializer, ConvertsColumnVectorTransformToWeRowVectorMatrix) {
    const rstd::array<float, 16> values { 1.0f, 4.0f, 7.0f, 0.0f, 2.0f,  5.0f,  8.0f,  0.0f,
                                          3.0f, 6.0f, 9.0f, 0.0f, 10.0f, 20.0f, 30.0f, 1.0f };
    auto value = owe::UniformValue::fromMatrixArray(values.data(),
                                                    rstd::u32(4),
                                                    rstd::u32(4),
                                                    rstd::usize(1),
                                                    owe::UniformMatrixStorage::ColumnMajor);
    auto bytes = rstd::vec::Vec<rstd::u8>::make();
    bytes.resize(rstd::usize(48), rstd::u8(0xff));
    auto slot = owe::vulkan::UniformSlot {
        .name              = rstd::string::String::make("transform"_str),
        .size              = rstd::usize(48),
        .scalar_kind       = owe::ShaderScalarKind::Float,
        .scalar_width      = rstd::u32(32),
        .vector_components = rstd::u32(3),
        .matrix_rows       = rstd::u32(3),
        .matrix_columns    = rstd::u32(4),
        .matrix_stride     = rstd::u32(16),
        .matrix_major      = owe::ShaderMatrixMajor::Row,
    };

    auto result = owe::vulkan::SerializeUniformValue(bytes.as_mut_slice().as_mut_ref(),
                                                     slot,
                                                     value.View(),
                                                     owe::ShaderMatrixConvention::RowVector,
                                                     owe::ShaderMatrixAbi::Hlsl);

    ASSERT_TRUE(result.is_ok());
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(0)), 1.0f);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(4)), 2.0f);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(8)), 3.0f);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(12)), 10.0f);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(16)), 4.0f);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(32)), 7.0f);
}

TEST(UniformBufferSerializer, PreservesColumnVectorSemanticsForHlslShader) {
    const rstd::array<float, 9> values { 1.0f, 4.0f, 7.0f, 2.0f, 5.0f, 8.0f, 3.0f, 6.0f, 9.0f };
    auto value = owe::UniformValue::fromMatrixArray(values.data(),
                                                    rstd::u32(3),
                                                    rstd::u32(3),
                                                    rstd::usize(1),
                                                    owe::UniformMatrixStorage::ColumnMajor);
    auto bytes = rstd::vec::Vec<rstd::u8>::make();
    bytes.resize(rstd::usize(48), rstd::u8(0xff));
    auto slot = owe::vulkan::UniformSlot {
        .name              = rstd::string::String::make("matrix"_str),
        .size              = rstd::usize(48),
        .scalar_kind       = owe::ShaderScalarKind::Float,
        .scalar_width      = rstd::u32(32),
        .vector_components = rstd::u32(3),
        .matrix_rows       = rstd::u32(3),
        .matrix_columns    = rstd::u32(3),
        .matrix_stride     = rstd::u32(16),
        .matrix_major      = owe::ShaderMatrixMajor::Row,
    };

    auto result = owe::vulkan::SerializeUniformValue(bytes.as_mut_slice().as_mut_ref(),
                                                     slot,
                                                     value.View(),
                                                     owe::ShaderMatrixConvention::ColumnVector,
                                                     owe::ShaderMatrixAbi::Hlsl);

    ASSERT_TRUE(result.is_ok());
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(0)), 1.0f);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(4)), 4.0f);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(8)), 7.0f);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(16)), 2.0f);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(32)), 3.0f);
}

TEST(UniformBufferSerializer, RejectsLinearValueForMatrixSlot) {
    const rstd::array<float, 4> values { 1.0f, 2.0f, 3.0f, 4.0f };
    auto                        value = owe::UniformValue(values);
    auto                        bytes = rstd::vec::Vec<rstd::u8>::make();
    bytes.resize(rstd::usize(16), rstd::u8());
    auto slot = owe::vulkan::UniformSlot {
        .name              = rstd::string::String::make("matrix"_str),
        .size              = rstd::usize(16),
        .scalar_kind       = owe::ShaderScalarKind::Float,
        .scalar_width      = rstd::u32(32),
        .vector_components = rstd::u32(2),
        .matrix_rows       = rstd::u32(2),
        .matrix_columns    = rstd::u32(2),
        .matrix_stride     = rstd::u32(8),
        .matrix_major      = owe::ShaderMatrixMajor::Row,
    };

    auto result = owe::vulkan::SerializeUniformValue(bytes.as_mut_slice().as_mut_ref(),
                                                     slot,
                                                     value.View(),
                                                     owe::ShaderMatrixConvention::ColumnVector);

    EXPECT_TRUE(result.is_err());
}

TEST(UniformBufferSerializer, RejectsShortMatrixArray) {
    const rstd::array<float, 4> values { 1.0f, 3.0f, 2.0f, 4.0f };
    auto value = owe::UniformValue::fromMatrixArray(values.data(),
                                                    rstd::u32(2),
                                                    rstd::u32(2),
                                                    rstd::usize(1),
                                                    owe::UniformMatrixStorage::ColumnMajor);
    auto bytes = rstd::vec::Vec<rstd::u8>::make();
    bytes.resize(rstd::usize(32), rstd::u8());
    auto slot = owe::vulkan::UniformSlot {
        .name              = rstd::string::String::make("matrices"_str),
        .size              = rstd::usize(32),
        .count             = rstd::usize(2),
        .scalar_kind       = owe::ShaderScalarKind::Float,
        .scalar_width      = rstd::u32(32),
        .vector_components = rstd::u32(2),
        .matrix_rows       = rstd::u32(2),
        .matrix_columns    = rstd::u32(2),
        .matrix_stride     = rstd::u32(8),
        .matrix_major      = owe::ShaderMatrixMajor::Row,
        .array_stride      = rstd::u32(16),
    };

    auto result = owe::vulkan::SerializeUniformValue(bytes.as_mut_slice().as_mut_ref(),
                                                     slot,
                                                     value.View(),
                                                     owe::ShaderMatrixConvention::ColumnVector);

    EXPECT_TRUE(result.is_err());
}

TEST(UniformBufferSerializer, RejectsShortVectorArray) {
    const rstd::array<float, 3> values { 1.0f, 2.0f, 3.0f };
    auto                        value = owe::UniformValue(values);
    auto                        bytes = rstd::vec::Vec<rstd::u8>::make();
    bytes.resize(rstd::usize(32), rstd::u8());
    auto slot = owe::vulkan::UniformSlot {
        .name              = rstd::string::String::make("vectors"_str),
        .size              = rstd::usize(32),
        .count             = rstd::usize(2),
        .scalar_kind       = owe::ShaderScalarKind::Float,
        .scalar_width      = rstd::u32(32),
        .vector_components = rstd::u32(2),
        .array_stride      = rstd::u32(16),
    };

    auto result = owe::vulkan::SerializeUniformValue(bytes.as_mut_slice().as_mut_ref(),
                                                     slot,
                                                     value.View(),
                                                     owe::ShaderMatrixConvention::ColumnVector);

    EXPECT_TRUE(result.is_err());
}

TEST(UniformBufferSerializer, PacksVectorArraysUsingReflectedArrayStride) {
    const rstd::array<float, 4> values { 1.0f, 2.0f, 3.0f, 4.0f };
    auto                        value = owe::UniformValue(values);
    auto                        bytes = rstd::vec::Vec<rstd::u8>::make();
    bytes.resize(rstd::usize(32), rstd::u8(0xff));
    auto slot = owe::vulkan::UniformSlot {
        .name              = rstd::string::String::make("vectors"_str),
        .size              = rstd::usize(32),
        .count             = rstd::usize(2),
        .scalar_kind       = owe::ShaderScalarKind::Float,
        .scalar_width      = rstd::u32(32),
        .vector_components = rstd::u32(2),
        .array_stride      = rstd::u32(16),
    };

    auto result = owe::vulkan::SerializeUniformValue(bytes.as_mut_slice().as_mut_ref(),
                                                     slot,
                                                     value.View(),
                                                     owe::ShaderMatrixConvention::ColumnVector);

    ASSERT_TRUE(result.is_ok());
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(0)), 1.0f);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(4)), 2.0f);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(8)), 0.0f);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(16)), 3.0f);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(20)), 4.0f);
}

TEST(UniformBufferSerializer, ExplicitZeroExtensionClearsMissingArrayComponents) {
    const rstd::array<float, 3> values { 1.0f, 2.0f, 3.0f };
    auto                        value = owe::UniformValue::fromZeroExtended(values.as_slice());
    auto                        bytes = rstd::vec::Vec<rstd::u8>::make();
    bytes.resize(rstd::usize(48), rstd::u8(0xff));
    const auto slot = owe::vulkan::UniformSlot {
        .name              = rstd::string::String::make("vectors"_str),
        .size              = rstd::usize(48),
        .count             = rstd::usize(3),
        .scalar_kind       = owe::ShaderScalarKind::Float,
        .scalar_width      = rstd::u32(32),
        .vector_components = rstd::u32(2),
        .array_stride      = rstd::u32(16),
    };
    const auto result =
        owe::vulkan::SerializeUniformValue(bytes.as_mut_slice().as_mut_ref(),
                                           slot,
                                           value.View(),
                                           owe::ShaderMatrixConvention::ColumnVector);
    ASSERT_TRUE(result.is_ok());
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(0)), 1.0f);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(4)), 2.0f);
    EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(16)), 3.0f);
    for (auto offset : { 8, 12, 20, 24, 28, 32, 36, 40, 44 })
        EXPECT_FLOAT_EQ(ReadFloat(bytes, rstd::usize(offset)), 0.0f);
}

TEST(UniformBufferBinding, UpdatesGenericSceneThroughBufferWriterTrait) {
    owe::Scene scene;
    auto       camera_node = Arc<owe::SceneNode>::make();
    auto       camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(1920, 1080, -1.0, 1.0));
    camera->AttatchNode(camera_node.as_ptr());
    scene.RegisterCamera(String::make("default"_str), camera.clone());
    ASSERT_TRUE(scene.SetActiveCamera("default"_str));

    auto registrar   = rstd::dyn<owe::UniformSourceRegistrar>::from_ref(scene);
    auto attachments = rstd::dyn<owe::UniformAttachmentWriter>::from_ref(scene);
    auto source      = registrar->Register(rstd::boxed::Box<rstd::dyn<owe::UniformSource>>::make(
        uniform_test::StaticSource("scene_time", 2.5f)));
    ASSERT_TRUE(attachments->AttachGlobal(source));

    auto shader = std::make_shared<owe::SceneShader>();
    auto node   = rstd::sync::Arc<owe::SceneNode>::make();
    node->ID()  = rstd::i32(1);
    node->AddMesh(MakeUniformMesh(std::move(shader)));
    scene.RootMut()->AppendChild(node.clone());
    scene.RebuildResourceIndex();
    auto node_id = scene.ResourceIndex().nodeId(*node.as_ptr());
    ASSERT_TRUE(node_id.is_some());
    auto draw_id = scene.ResourceIndex().drawItemFor(*node_id, rstd::u32());
    ASSERT_TRUE(draw_id.is_some());

    auto members = rstd::vec::Vec<owe::resource::ShaderArtifactUniformMember>::make();
    members.push(owe::resource::ShaderArtifactUniformMember {
        .name   = rstd::string::String::make("scene_time"_str),
        .offset = rstd::u32(),
        .size   = rstd::usize(16),
    });
    auto block = owe::resource::ShaderArtifactUniformBlock {
        .name    = rstd::string::String::make("Globals"_str),
        .size    = rstd::usize(16),
        .members = rstd::move(members),
    };
    const auto buffer =
        owe::resource::BufferUseHandle { .index = rstd::u64(3), .generation = rstd::u64(1) };
    owe::vulkan::SceneUniformBindingPrepareContext prepare_impl(scene);
    auto prepare = rstd::dyn<owe::vulkan::UniformBindingPrepareContext>::from_ref(prepare_impl);
    auto binding = owe::vulkan::MakeUniformBufferBinding(prepare.as_ref(), *draw_id, buffer, block);
    ASSERT_TRUE(binding.is_ok());

    uniform_test::BufferWriter writer;
    auto writer_trait   = rstd::dyn<owe::resource::BufferContentWriter>::from_ref(writer);
    auto texture_frames = rstd::dyn<owe::SceneTextureAnimationView>::from_ref(scene);
    owe::vulkan::ProgramUniformFrameContext frame_impl(
        scene.Runtime().Frame(), { 1920.0f, 1080.0f }, texture_frames.as_ref());
    auto frame   = rstd::dyn<owe::vulkan::UniformBufferFrameContext>::from_ref(frame_impl);
    auto updated = binding.unwrap_unchecked()->Update(frame.as_ref(), writer_trait.as_mut_ref());

    ASSERT_TRUE(updated.is_ok());
    EXPECT_EQ(writer.last_use, buffer);
    ASSERT_EQ(writer.bytes.size(), 16u);
    std::array<float, 4> values {};
    std::memcpy(values.data(), writer.bytes.data(), writer.bytes.size());
    EXPECT_FLOAT_EQ(values[0], 2.5f);
    EXPECT_FLOAT_EQ(values[1], 0.0f);
    EXPECT_FLOAT_EQ(values[2], 0.0f);
    EXPECT_FLOAT_EQ(values[3], 0.0f);
}

TEST(UniformBufferBinding, HoldsDemandOnlyForAReflectedLiveOutput) {
    owe::Scene scene;
    auto       demand = rstd::sync::Arc<owe::AudioResponseDemand>::make();
    bool       active = false;
    demand->SetCallback([&active](bool next) {
        active = next;
    });
    auto registrar   = rstd::dyn<owe::UniformSourceRegistrar>::from_ref(scene);
    auto attachments = rstd::dyn<owe::UniformAttachmentWriter>::from_ref(scene);
    auto source      = registrar->Register(rstd::boxed::Box<rstd::dyn<owe::UniformSource>>::make(
        uniform_test::StaticSource("audio_signal", 0.0f, demand.clone())));
    ASSERT_TRUE(attachments->AttachGlobal(source));

    auto node = rstd::sync::Arc<owe::SceneNode>::make();
    node->AddMesh(MakeUniformMesh(std::make_shared<owe::SceneShader>()));
    scene.RootMut()->AppendChild(node.clone());
    scene.RebuildResourceIndex();
    auto node_id = scene.ResourceIndex().nodeId(*node.as_ptr());
    ASSERT_TRUE(node_id.is_some());
    auto draw_id = scene.ResourceIndex().drawItemFor(*node_id, rstd::u32());
    ASSERT_TRUE(draw_id.is_some());
    owe::vulkan::SceneUniformBindingPrepareContext prepare_impl(scene);
    auto prepare = rstd::dyn<owe::vulkan::UniformBindingPrepareContext>::from_ref(prepare_impl);

    auto make_block = [](std::string_view name) {
        auto members = rstd::vec::Vec<owe::resource::ShaderArtifactUniformMember>::make();
        members.push(owe::resource::ShaderArtifactUniformMember {
            .name   = rstd::string::String::make(rstd::cppstd::as_str(name).unwrap()),
            .offset = rstd::u32(),
            .size   = rstd::usize(4),
        });
        return owe::resource::ShaderArtifactUniformBlock {
            .name    = rstd::string::String::make("Globals"_str),
            .size    = rstd::usize(4),
            .members = rstd::move(members),
        };
    };

    {
        auto unbound = owe::vulkan::MakeUniformBufferBinding(
            prepare.as_ref(),
            *draw_id,
            owe::resource::BufferUseHandle { .index = rstd::u64(1), .generation = rstd::u64(1) },
            make_block("unrelated"));
        ASSERT_TRUE(unbound.is_ok());
        EXPECT_FALSE(active);
    }
    {
        auto bound = owe::vulkan::MakeUniformBufferBinding(
            prepare.as_ref(),
            *draw_id,
            owe::resource::BufferUseHandle { .index = rstd::u64(2), .generation = rstd::u64(1) },
            make_block("audio_signal"));
        ASSERT_TRUE(bound.is_ok());
        EXPECT_TRUE(active);
    }
    EXPECT_FALSE(active);
}

TEST(UniformBufferBinding, ProvidesPreparedTextureMetadataToGenericSource) {
    owe::Scene scene;
    auto       registrar   = rstd::dyn<owe::UniformSourceRegistrar>::from_ref(scene);
    auto       attachments = rstd::dyn<owe::UniformAttachmentWriter>::from_ref(scene);
    auto       source = registrar->Register(rstd::boxed::Box<rstd::dyn<owe::UniformSource>>::make(
        uniform_test::TextureMetadataSource {}));
    ASSERT_TRUE(attachments->AttachGlobal(source));

    auto shader = std::make_shared<owe::SceneShader>();
    auto node   = rstd::sync::Arc<owe::SceneNode>::make();
    node->AddMesh(MakeUniformMesh(std::move(shader)));
    scene.RootMut()->AppendChild(node.clone());
    scene.RebuildResourceIndex();
    auto node_id = scene.ResourceIndex().nodeId(*node.as_ptr());
    ASSERT_TRUE(node_id.is_some());
    auto draw_id = scene.ResourceIndex().drawItemFor(*node_id, rstd::u32());
    ASSERT_TRUE(draw_id.is_some());

    auto members = rstd::vec::Vec<owe::resource::ShaderArtifactUniformMember>::make();
    members.push(owe::resource::ShaderArtifactUniformMember {
        .name   = rstd::string::String::make("texture_extent"_str),
        .offset = rstd::u32(),
        .size   = rstd::usize(16),
    });
    auto block = owe::resource::ShaderArtifactUniformBlock {
        .name    = rstd::string::String::make("Globals"_str),
        .size    = rstd::usize(16),
        .members = rstd::move(members),
    };
    auto textures = rstd::vec::Vec<owe::vulkan::PreparedUniformTextureMetadata>::make();
    textures.push(owe::vulkan::PreparedUniformTextureMetadata {
        .available     = true,
        .source_extent = { 640.0f, 360.0f },
        .sample_extent = { 1024.0f, 512.0f },
        .revision      = rstd::u64(4),
    });
    owe::vulkan::SceneUniformBindingPrepareContext prepare_impl(scene);
    auto prepare = rstd::dyn<owe::vulkan::UniformBindingPrepareContext>::from_ref(prepare_impl);
    auto binding = owe::vulkan::MakeUniformBufferBinding(
        prepare.as_ref(),
        *draw_id,
        owe::resource::BufferUseHandle { .index = rstd::u64(3), .generation = rstd::u64(1) },
        block,
        rstd::move(textures));
    ASSERT_TRUE(binding.is_ok());

    uniform_test::BufferWriter writer;
    auto writer_trait   = rstd::dyn<owe::resource::BufferContentWriter>::from_ref(writer);
    auto texture_frames = rstd::dyn<owe::SceneTextureAnimationView>::from_ref(scene);
    owe::vulkan::ProgramUniformFrameContext frame_impl(
        scene.Runtime().Frame(), { 1920.0f, 1080.0f }, texture_frames.as_ref());
    auto frame = rstd::dyn<owe::vulkan::UniformBufferFrameContext>::from_ref(frame_impl);
    ASSERT_TRUE(
        binding.unwrap_unchecked()->Update(frame.as_ref(), writer_trait.as_mut_ref()).is_ok());

    ASSERT_EQ(writer.bytes.size(), 16u);
    std::array<float, 4> values {};
    std::memcpy(values.data(), writer.bytes.data(), writer.bytes.size());
    EXPECT_EQ(values, (std::array<float, 4> { 640.0f, 360.0f, 1024.0f, 512.0f }));
}

TEST(UniformBufferBinding, OrdersSourcesAndSkipsUnchangedVersions) {
    owe::Scene scene;
    auto       camera_node = Arc<owe::SceneNode>::make();
    auto       camera =
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(1920, 1080, -1.0, 1.0));
    camera->AttatchNode(camera_node.as_ptr());
    scene.RegisterCamera(String::make("default"_str), camera.clone());
    ASSERT_TRUE(scene.SetActiveCamera("default"_str));

    auto registrar   = rstd::dyn<owe::UniformSourceRegistrar>::from_ref(scene);
    auto attachments = rstd::dyn<owe::UniformAttachmentWriter>::from_ref(scene);
    auto low_state   = std::make_shared<uniform_test::StaticSourceState>(
        uniform_test::StaticSourceState { .value = 3.0f });
    auto high_priority = registrar->Register(rstd::boxed::Box<rstd::dyn<owe::UniformSource>>::make(
        uniform_test::StaticSource("static_value", 7.0f)));
    auto low_priority  = registrar->Register(rstd::boxed::Box<rstd::dyn<owe::UniformSource>>::make(
        uniform_test::StaticSource("static_value", low_state)));
    auto shader        = std::make_shared<owe::SceneShader>();
    auto node          = rstd::sync::Arc<owe::SceneNode>::make();
    node->ID()         = rstd::i32(1);
    node->AddMesh(MakeUniformMesh(std::move(shader)));
    scene.RootMut()->AppendChild(node.clone());
    scene.RebuildResourceIndex();
    auto node_id = scene.ResourceIndex().nodeId(*node.as_ptr());
    ASSERT_TRUE(node_id.is_some());
    ASSERT_TRUE(attachments->AttachNode(*node_id, high_priority, i32(10)));
    ASSERT_TRUE(attachments->AttachNode(*node_id, low_priority, i32(-10)));
    auto draw_id = scene.ResourceIndex().drawItemFor(*node_id, rstd::u32());
    ASSERT_TRUE(draw_id.is_some());

    auto members = rstd::vec::Vec<owe::resource::ShaderArtifactUniformMember>::make();
    members.push(owe::resource::ShaderArtifactUniformMember {
        .name   = rstd::string::String::make("static_value"_str),
        .offset = rstd::u32(),
        .size   = rstd::usize(4),
    });
    auto block = owe::resource::ShaderArtifactUniformBlock {
        .name    = rstd::string::String::make("Globals"_str),
        .size    = rstd::usize(4),
        .members = rstd::move(members),
    };
    owe::vulkan::SceneUniformBindingPrepareContext prepare_impl(scene);
    auto prepare = rstd::dyn<owe::vulkan::UniformBindingPrepareContext>::from_ref(prepare_impl);
    auto binding = owe::vulkan::MakeUniformBufferBinding(
        prepare.as_ref(),
        *draw_id,
        owe::resource::BufferUseHandle { .index = rstd::u64(3), .generation = rstd::u64(1) },
        block);
    ASSERT_TRUE(binding.is_ok());

    uniform_test::BufferWriter writer;
    auto writer_trait   = rstd::dyn<owe::resource::BufferContentWriter>::from_ref(writer);
    auto update         = binding.unwrap_unchecked();
    auto texture_frames = rstd::dyn<owe::SceneTextureAnimationView>::from_ref(scene);
    owe::vulkan::ProgramUniformFrameContext frame_impl(
        scene.Runtime().Frame(), { 1920.0f, 1080.0f }, texture_frames.as_ref());
    auto frame = rstd::dyn<owe::vulkan::UniformBufferFrameContext>::from_ref(frame_impl);
    ASSERT_TRUE(update->Update(frame.as_ref(), writer_trait.as_mut_ref()).is_ok());
    ASSERT_TRUE(update->Update(frame.as_ref(), writer_trait.as_mut_ref()).is_ok());

    EXPECT_EQ(writer.update_count, rstd::u64(1));
    low_state->value = 5.0f;
    ++low_state->version;
    ASSERT_TRUE(update->Update(frame.as_ref(), writer_trait.as_mut_ref()).is_ok());

    EXPECT_EQ(writer.update_count, rstd::u64(2));
    ASSERT_EQ(writer.bytes.size(), 4u);
    float value = 0.0f;
    std::memcpy(&value, writer.bytes.data(), writer.bytes.size());
    EXPECT_FLOAT_EQ(value, 7.0f);
}

TEST(UniformBufferBinding, UpdatesRegisteredSharedBlockOncePerVersion) {
    owe::Scene scene;
    auto       registrar = rstd::dyn<owe::UniformSourceRegistrar>::from_ref(scene);
    auto       state     = std::make_shared<uniform_test::StaticSourceState>(
        uniform_test::StaticSourceState { .value = 4.0f });
    auto source  = registrar->Register(rstd::boxed::Box<rstd::dyn<owe::UniformSource>>::make(
        uniform_test::StaticSource("shared_value", state)));
    auto sources = rstd::vec::Vec<owe::UniformSourceAttachment>::make();
    sources.push(owe::UniformSourceAttachment { .source = source });
    ASSERT_TRUE(scene.RegisterUniformBlock(owe::UniformBlockDefinition {
        .identity = rstd::u64(42),
        .name     = rstd::string::String::make("SharedBlock"_str),
        .scope    = owe::UniformBlockScope::Shared,
        .sources  = rstd::move(sources),
    }));

    auto members = rstd::vec::Vec<owe::resource::ShaderArtifactUniformMember>::make();
    members.push(owe::resource::ShaderArtifactUniformMember {
        .name   = rstd::string::String::make("shared_value"_str),
        .offset = rstd::u32(),
        .size   = rstd::usize(4),
    });
    auto block = owe::resource::ShaderArtifactUniformBlock {
        .name     = rstd::string::String::make("SharedBlock"_str),
        .size     = rstd::usize(4),
        .set      = rstd::u32(3),
        .binding  = rstd::u32(2),
        .scope    = owe::resource::ShaderArtifactUniformBlock::Scope::Shared,
        .identity = rstd::u64(42),
        .members  = rstd::move(members),
    };
    const auto buffer =
        owe::resource::BufferUseHandle { .index = rstd::u64(8), .generation = rstd::u64(1) };
    owe::vulkan::SceneUniformBindingPrepareContext prepare_impl(scene);
    auto prepare = rstd::dyn<owe::vulkan::UniformBindingPrepareContext>::from_ref(prepare_impl);
    auto update =
        owe::vulkan::MakeSharedUniformBufferBinding(prepare.as_ref(),
                                                    buffer,
                                                    block,
                                                    owe::ShaderMatrixConvention::ColumnVector,
                                                    owe::ShaderMatrixAbi::NativeSpirv);
    ASSERT_TRUE(update.is_ok());

    uniform_test::BufferWriter writer;
    auto writer_trait   = rstd::dyn<owe::resource::BufferContentWriter>::from_ref(writer);
    auto texture_frames = rstd::dyn<owe::SceneTextureAnimationView>::from_ref(scene);
    owe::vulkan::ProgramUniformFrameContext frame_impl(
        scene.Runtime().Frame(), { 1.0f, 1.0f }, texture_frames.as_ref());
    auto frame = rstd::dyn<owe::vulkan::UniformBufferFrameContext>::from_ref(frame_impl);
    auto owner = rstd::move(update).unwrap_unchecked();
    ASSERT_TRUE(owner->Update(frame.as_ref(), writer_trait.as_mut_ref()).is_ok());
    ASSERT_TRUE(owner->Update(frame.as_ref(), writer_trait.as_mut_ref()).is_ok());
    EXPECT_EQ(owner->Buffer(), buffer);
    EXPECT_EQ(writer.update_count, rstd::u64(1));
    ASSERT_EQ(writer.bytes.size(), 4u);
    float value {};
    std::memcpy(&value, writer.bytes.data(), sizeof(value));
    EXPECT_FLOAT_EQ(value, 4.0f);
}

TEST(ShaderArtifact, ReconstructsPreparedInterfaceWithoutCacheLookup) {
    owe::resource::ShaderArtifact artifact;
    artifact.matrix_convention = owe::ShaderMatrixConvention::RowVector;
    artifact.matrix_abi        = owe::ShaderMatrixAbi::Hlsl;
    auto code                  = rstd::vec::Vec<rstd::u32>::make();
    code.push(rstd::u32(1));
    code.push(rstd::u32(2));
    code.push(rstd::u32(3));
    artifact.stages.push(owe::resource::ShaderArtifactStage {
        .stage       = owe::ShaderType::VERTEX,
        .entry_point = rstd::string::String::make("main"_str),
        .code        = rstd::move(code),
    });
    auto members = rstd::vec::Vec<owe::resource::ShaderArtifactUniformMember>::make();
    members.push(owe::resource::ShaderArtifactUniformMember {
        .name   = rstd::string::String::make("g_Time"_str),
        .offset = rstd::u32(16),
        .size   = rstd::usize(4),
    });
    artifact.uniform_blocks.push(owe::resource::ShaderArtifactUniformBlock {
        .name     = rstd::string::String::make("Globals"_str),
        .size     = rstd::usize(32),
        .set      = rstd::u32(4),
        .binding  = rstd::u32(7),
        .scope    = owe::resource::ShaderArtifactUniformBlock::Scope::Shared,
        .identity = rstd::u64(91),
        .members  = rstd::move(members),
    });
    artifact.descriptor_bindings.push(owe::resource::ShaderArtifactDescriptorBinding {
        .name    = rstd::string::String::make("g_Texture0"_str),
        .set     = rstd::u32(4),
        .binding = rstd::u32(2),
        .descriptor_type =
            rstd::u32(static_cast<rstd::uint32_t>(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)),
        .descriptor_count = rstd::u32(1),
        .stage_flags      = rstd::u32(static_cast<rstd::uint32_t>(VK_SHADER_STAGE_FRAGMENT_BIT)),
    });
    artifact.vertex_inputs.push(owe::resource::ShaderArtifactVertexInput {
        .name     = rstd::string::String::make("a_Position"_str),
        .location = rstd::u32(3),
        .format   = rstd::u32(static_cast<rstd::uint32_t>(VK_FORMAT_R32G32_SFLOAT)),
    });

    auto cloned = artifact.clone();
    EXPECT_EQ(cloned.matrix_convention, owe::ShaderMatrixConvention::RowVector);
    EXPECT_EQ(cloned.matrix_abi, owe::ShaderMatrixAbi::Hlsl);

    auto stages     = owe::vulkan::ShaderSpvsFromArtifact(artifact);
    auto reflection = owe::vulkan::ShaderReflectionFromArtifact(artifact);
    ASSERT_EQ(stages.size(), 1u);
    EXPECT_EQ(stages[0]->entry_point, "main");
    EXPECT_EQ(stages[0]->spirv.size(), 3u);
    ASSERT_EQ(reflection.blocks.size(), 1u);
    EXPECT_EQ(reflection.blocks[0].set, 4u);
    EXPECT_EQ(reflection.blocks[0].binding, 7u);
    EXPECT_EQ(reflection.blocks[0].member_map.at("g_Time").offset, 16u);
    EXPECT_EQ(reflection.binding_map.at("g_Texture0").layout.binding, 2u);
    EXPECT_EQ(reflection.binding_map.at("g_Texture0").set, 4u);
    EXPECT_EQ(reflection.input_location_map.at("a_Position").format, VK_FORMAT_R32G32_SFLOAT);
}

TEST(TextureRequest, BuildsImportedRequestWithoutCacheKey) {
    auto request = owe::vulkan::MakeImportedTextureRequest("textures/main.png");

    EXPECT_EQ(request.kind, owe::vulkan::TextureRequestKind::Imported);
    EXPECT_EQ(rstd::cppstd::as_string_view(request.name.as_str()), "textures/main.png");
    EXPECT_TRUE(request.source.is_none());
    EXPECT_TRUE(request.definition.is_none());
    EXPECT_EQ(request.lifetime, owe::resource::TextureLifetimeClass::Retained);
}

TEST(TextureBindingRequest, CarriesNameAndTypedRequest) {
    auto request = owe::vulkan::MakeImportedTextureRequest("texture-slot");
    owe::vulkan::TextureBindingRequest binding {
        .name    = rstd::string::String::make("texture-slot"_str),
        .request = rstd::Some(std::move(request)),
    };

    EXPECT_FALSE(binding.empty());
    ASSERT_TRUE(binding.request.is_some());
    EXPECT_EQ(binding.request->kind, owe::vulkan::TextureRequestKind::Imported);
    EXPECT_EQ(rstd::cppstd::as_string_view(binding.request->name.as_str()), "texture-slot");

    owe::vulkan::TextureBindingRequest empty;
    EXPECT_TRUE(empty.empty());
}

TEST(ResourcePlan, UpdatesTextureRequestByStableUse) {
    auto use =
        owe::resource::TextureUseHandle { .index = rstd::u64(3), .generation = rstd::u64(7) };
    owe::resource::ResourcePlan plan { .generation = rstd::u64(7) };
    plan.textures.push(owe::resource::TexturePlanEntry {
        .handle  = use,
        .request = owe::vulkan::MakeImportedTextureRequest("texture-old"),
    });

    EXPECT_TRUE(
        plan.UpdateTextureRequest(use, owe::vulkan::MakeImportedTextureRequest("texture-new")));
    EXPECT_EQ(plan.textures[rstd::usize()].request.name, "texture-new"_str);
    EXPECT_FALSE(plan.UpdateTextureRequest(
        owe::resource::TextureUseHandle { .index = use.index, .generation = rstd::u64(8) },
        owe::vulkan::MakeImportedTextureRequest("texture-invalid")));
    EXPECT_EQ(plan.textures[rstd::usize()].request.name, "texture-new"_str);
}

TEST(ShaderBackend, CompilesBothLanguagesAndReusesReflection) {
    using namespace owe::vulkan;
    auto                  backend = MakeShaderBackend();
    ShaderReflectionCache cache(backend.as_ref());
    for (auto language : { SourceLang::Glsl, SourceLang::Hlsl }) {
        ShaderCompUnit unit {
            .stage       = owe::ShaderType::VERTEX,
            .src         = language == SourceLang::Glsl
                               ? "#version 450\nvoid main() { gl_Position = vec4(0, 0, 0, 1); }"
                               : "float4 main() : SV_Position { return float4(0, 0, 0, 1); }",
            .entry_point = "main",
            .lang        = language,
        };
        std::string preprocessed;
        ASSERT_TRUE(backend->Preprocess(unit.src, unit.stage, language, preprocessed));
        EXPECT_FALSE(preprocessed.empty());
        std::vector<Uni_ShaderSpv> spvs;
        ASSERT_TRUE(backend->CompileAndLinkShaderUnits(std::span(&unit, 1), {}, spvs));
        ASSERT_EQ(spvs.size(), 1u);
        owe::SceneShader shader;
        shader.codes.push_back(spvs[0]->spirv);
        auto first = cache.Query(shader);
        ASSERT_TRUE(first.is_some());
        auto second = cache.Query(shader);
        ASSERT_TRUE(second.is_some());
        EXPECT_EQ(&**first, &**second);
        cache.Clear();
        EXPECT_TRUE(cache.Query(shader).is_some());
        unit.src = "invalid shader";
        spvs.clear();
        EXPECT_FALSE(backend->CompileAndLinkShaderUnits(std::span(&unit, 1), {}, spvs));
    }
}

TEST(TextureLoader, RetainsCapturedRuntimeContentAfterSceneReplacement) {
    using namespace rstd;
    auto original          = sync::Arc<owe::Image>::make();
    original->content->key = "original";
    Option<sync::Arc<dyn<owe::resource::TextureLoader>>> loader;
    {
        owe::Scene scene;
        scene.RegisterRuntimeImage(String::make("runtime"_str), original.clone());
        auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
        owe::vulkan::SnapshotImportedTextureProvider provider(
            snapshot, ref<owe::Scene>::from_raw_parts(&scene));
        auto opened = provider.OpenTextureLoader();
        ASSERT_TRUE(opened.is_ok());
        loader = Some(rstd::move(opened).unwrap_unchecked());
        scene.RegisterRuntimeImage(String::make("runtime"_str), sync::Arc<owe::Image>::make());
    }
    auto loaded = (*loader)->LoadTexture("runtime"_str);
    ASSERT_TRUE(loaded.is_ok());
    auto image = rstd::move(loaded).unwrap_unchecked();
    EXPECT_EQ(&*image, &*original->content);
    EXPECT_EQ(image->key, "original");
}

TEST(FramePassResources, DeclaresTargetUsesOutsideTheRenderGraphPlan) {
    owe::SceneRenderTarget render_target {
        .width        = i32(1920),
        .height       = i32(1080),
        .sample_count = 4,
    };
    auto target = owe::vulkan::MakeRenderTargetTextureRequest("_rt_default", render_target);
    auto msaa   = owe::vulkan::MakeMsaaTextureRequest(
        "_rt_default::msaa4", render_target, VK_SAMPLE_COUNT_4_BIT);
    owe::resource::ResourcePlan plan { .generation = rstd::u64(7) };
    plan.textures.push(owe::resource::TexturePlanEntry {
        .handle =
            owe::resource::TextureUseHandle { .index = rstd::u64(4), .generation = rstd::u64(7) },
        .request = owe::vulkan::MakeImportedTextureRequest("graph-input"),
    });
    auto                               shader_backend = owe::vulkan::MakeShaderBackend();
    owe::vulkan::ShaderReflectionCache shader_cache(shader_backend.as_ref());
    owe::vulkan::PrePass               pre(owe::vulkan::PrePass::Desc {
        .result_request      = rstd::Some(target.clone()),
        .result_msaa_request = rstd::Some(rstd::move(msaa)),
    });
    owe::vulkan::FinPass               fin(owe::vulkan::FinPass::Desc {
        .result_request = rstd::Some(target.clone()),
    });
    {
        owe::vulkan::ResourceDeclarationContext declarations(plan, shader_cache);
        pre.declareResources(declarations);
        fin.declareResources(declarations);
    }

    auto pre_diagnostics = pre.textureRequestDiagnostics();
    auto fin_diagnostics = fin.textureRequestDiagnostics();
    ASSERT_EQ(pre_diagnostics.size(), 2u);
    ASSERT_EQ(fin_diagnostics.size(), 1u);
    ASSERT_TRUE(pre_diagnostics[0].use.is_some());
    ASSERT_TRUE(pre_diagnostics[1].use.is_some());
    ASSERT_TRUE(fin_diagnostics[0].use.is_some());
    EXPECT_EQ(pre_diagnostics[0].use->index, rstd::u64(5));
    EXPECT_EQ(pre_diagnostics[1].use->index, rstd::u64(6));
    EXPECT_EQ(fin_diagnostics[0].use->index, rstd::u64(7));
    EXPECT_NE(*pre_diagnostics[0].use, *fin_diagnostics[0].use);
    ASSERT_EQ(plan.textures.len(), rstd::usize(4));
    EXPECT_EQ(plan.textures[rstd::usize(1)].access, owe::resource::ResourceAccess::Write);
    EXPECT_EQ(plan.textures[rstd::usize(2)].access, owe::resource::ResourceAccess::Write);
    EXPECT_EQ(plan.textures[rstd::usize(3)].access, owe::resource::ResourceAccess::Read);
    EXPECT_EQ(plan.textures[rstd::usize(1)].request.name, "_rt_default"_str);
    EXPECT_EQ(plan.textures[rstd::usize(2)].request.name, "_rt_default::msaa4"_str);
    EXPECT_EQ(plan.textures[rstd::usize(3)].request.name, "_rt_default"_str);

    plan.textures.truncate(rstd::usize(1));
    {
        owe::vulkan::ResourceDeclarationContext declarations(plan, shader_cache);
        pre.declareResources(declarations);
        fin.declareResources(declarations);
    }
    pre_diagnostics = pre.textureRequestDiagnostics();
    fin_diagnostics = fin.textureRequestDiagnostics();
    ASSERT_EQ(plan.textures.len(), rstd::usize(4));
    EXPECT_EQ(pre_diagnostics[0].use->index, rstd::u64(5));
    EXPECT_EQ(pre_diagnostics[1].use->index, rstd::u64(6));
    EXPECT_EQ(fin_diagnostics[0].use->index, rstd::u64(7));
}

TEST(ResourceDeclarationContext, ScopesLocalResourceNamesToThePlanGeneration) {
    owe::resource::ResourcePlan             plan { .generation = rstd::u64(7) };
    auto                                    shader_backend = owe::vulkan::MakeShaderBackend();
    owe::vulkan::ShaderReflectionCache      shader_cache(shader_backend.as_ref());
    owe::vulkan::ResourceDeclarationContext declarations(plan, shader_cache);

    auto name = declarations.ScopeResourceName(String::make("pass:25:0:vertex:0"_str));

    EXPECT_EQ(rstd::cppstd::as_string_view(name.as_str()), "plan:7:pass:25:0:vertex:0");
}

TEST(FramePassResources, ClearsStaleUsesBeforeFramePassInjection) {
    owe::SceneRenderTarget render_target {
        .width        = i32(1920),
        .height       = i32(1080),
        .sample_count = 4,
    };
    auto target = owe::vulkan::MakeRenderTargetTextureRequest("_rt_default", render_target);
    auto msaa   = owe::vulkan::MakeMsaaTextureRequest(
        "_rt_default::msaa4", render_target, VK_SAMPLE_COUNT_4_BIT);
    owe::resource::ResourcePlan plan { .generation = rstd::u64(7) };
    plan.textures.push(owe::resource::TexturePlanEntry {
        .handle =
            owe::resource::TextureUseHandle { .index = rstd::u64(4), .generation = rstd::u64(7) },
        .request = owe::vulkan::MakeImportedTextureRequest("particle/fog/fog1"),
    });

    auto                               shader_backend = owe::vulkan::MakeShaderBackend();
    owe::vulkan::ShaderReflectionCache shader_cache(shader_backend.as_ref());
    owe::vulkan::PrePass               pre(owe::vulkan::PrePass::Desc {
        .result_request      = rstd::Some(target.clone()),
        .result_msaa_request = rstd::Some(rstd::move(msaa)),
    });
    owe::vulkan::FinPass               fin(owe::vulkan::FinPass::Desc {
        .result_request = rstd::Some(target.clone()),
    });
    {
        owe::vulkan::ResourceDeclarationContext declarations(plan, shader_cache);
        pre.declareResources(declarations);
        fin.declareResources(declarations);
    }
    ASSERT_TRUE(pre.textureRequestDiagnostics()[0].use.is_some());
    ASSERT_TRUE(fin.textureRequestDiagnostics()[0].use.is_some());

    owe::vulkan::RenderProgram program;
    program.injectFramePasses(pre, fin);

    auto pre_diagnostics = pre.textureRequestDiagnostics();
    auto fin_diagnostics = fin.textureRequestDiagnostics();
    ASSERT_EQ(pre_diagnostics.size(), 2u);
    ASSERT_EQ(fin_diagnostics.size(), 1u);
    EXPECT_TRUE(pre_diagnostics[0].use.is_none());
    EXPECT_TRUE(pre_diagnostics[1].use.is_none());
    EXPECT_TRUE(fin_diagnostics[0].use.is_none());
    EXPECT_TRUE(pre_diagnostics[0].request.is_some());
    EXPECT_TRUE(pre_diagnostics[1].request.is_some());
    EXPECT_TRUE(fin_diagnostics[0].request.is_some());
}

TEST(CustomShaderPass, RefreshesImportedTextureOnStableUse) {
    owe::Scene scene;
    scene.RootMut()->ID() = rstd::i32(1);
    scene.RegisterTexture(String::make("texture-old"_str),
                          owe::SceneTexture { .url = "texture-old" });
    scene.RegisterTexture(String::make("texture-new"_str),
                          owe::SceneTexture { .url = "texture-new" });

    auto node  = Arc<owe::SceneNode>::make();
    node->ID() = rstd::i32(2);
    auto mesh  = MakeUniformMesh(std::make_shared<owe::SceneShader>());
    mesh->MaterialSlots()[0]->textures.push_back("texture-old");
    node->AddMesh(mesh);
    scene.RootMut()->AppendChild(node.clone());

    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
    auto old_desc = snapshot.textureDescId("texture-old"_str);
    auto new_desc = snapshot.textureDescId("texture-new"_str);
    ASSERT_TRUE(old_desc.is_some());
    ASSERT_TRUE(new_desc.is_some());
    auto use =
        owe::resource::TextureUseHandle { .index = rstd::u64(4), .generation = rstd::u64(9) };
    std::vector<owe::vulkan::TextureBindingRequest> bindings;
    bindings.push_back(owe::vulkan::TextureBindingRequest {
        .name    = String::make("texture-old"_str),
        .use     = rstd::Some(use),
        .request = rstd::Some(owe::vulkan::MakeImportedTextureRequest("texture-old", old_desc)),
    });
    owe::vulkan::CustomShaderPass pass(owe::vulkan::CustomShaderPass::Desc {
        .node = rstd::Some(rstd::mut_ref<owe::SceneNode>::from_raw_parts(node.as_ptr())),
        .texture_bindings = std::move(bindings),
    });

    ASSERT_TRUE(scene.SetMaterialTextureSlot(*mesh->MaterialSlots()[0], rstd::u32(), "texture-new")
                    .changed);
    auto refresh = pass.refreshMaterialTextureBindings(snapshot);
    EXPECT_NE(refresh.invalidation_flags, owe::vulkan::PassInvalidationNone);
    EXPECT_FALSE(refresh.requires_graph_rebuild);

    auto diagnostics = pass.textureRequestDiagnostics();
    ASSERT_EQ(diagnostics.size(), 1u);
    ASSERT_TRUE(diagnostics[0].use.is_some());
    EXPECT_EQ(*diagnostics[0].use, use);
    ASSERT_TRUE(diagnostics[0].request.is_some());
    EXPECT_EQ(diagnostics[0].request->name, "texture-new"_str);
    ASSERT_TRUE(diagnostics[0].request->source.is_some());
    EXPECT_EQ(diagnostics[0].request->source->index, new_desc->index);
    EXPECT_EQ(diagnostics[0].request->source->generation, new_desc->generation);
}

TEST(CustomShaderPass, RebuildsForImportedTextureMissingFromSnapshot) {
    owe::Scene scene;
    scene.RootMut()->ID() = rstd::i32(1);
    scene.RegisterTexture(String::make("texture-old"_str),
                          owe::SceneTexture { .url = "texture-old" });

    auto node  = Arc<owe::SceneNode>::make();
    node->ID() = rstd::i32(2);
    auto mesh  = MakeUniformMesh(std::make_shared<owe::SceneShader>());
    mesh->MaterialSlots()[0]->textures.push_back("texture-old");
    node->AddMesh(mesh);
    scene.RootMut()->AppendChild(node.clone());

    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
    auto old_desc = snapshot.textureDescId("texture-old"_str);
    ASSERT_TRUE(old_desc.is_some());
    auto use =
        owe::resource::TextureUseHandle { .index = rstd::u64(4), .generation = rstd::u64(9) };
    std::vector<owe::vulkan::TextureBindingRequest> bindings;
    bindings.push_back(owe::vulkan::TextureBindingRequest {
        .name    = String::make("texture-old"_str),
        .use     = rstd::Some(use),
        .request = rstd::Some(owe::vulkan::MakeImportedTextureRequest("texture-old", old_desc)),
    });
    owe::vulkan::CustomShaderPass pass(owe::vulkan::CustomShaderPass::Desc {
        .node = rstd::Some(rstd::mut_ref<owe::SceneNode>::from_raw_parts(node.as_ptr())),
        .texture_bindings = std::move(bindings),
    });

    scene.RegisterTexture(String::make("texture-new"_str),
                          owe::SceneTexture { .url = "texture-new" });
    ASSERT_TRUE(scene.SetMaterialTextureSlot(*mesh->MaterialSlots()[0], rstd::u32(), "texture-new")
                    .changed);

    auto refresh = pass.refreshMaterialTextureBindings(snapshot);

    EXPECT_EQ(refresh.invalidation_flags, owe::vulkan::PassInvalidationNone);
    EXPECT_TRUE(refresh.requires_graph_rebuild);
    auto diagnostics = pass.textureRequestDiagnostics();
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].name, "texture-old");
}

TEST(PreparedPassResources, ResolvesOnlyDeclaredUses) {
    owe::resource_registry::PreparedResourceTable table(rstd::u64(6));
    auto                                          allowed =
        owe::resource::TextureUseHandle { .index = rstd::u64(1), .generation = rstd::u64(6) };
    auto hidden =
        owe::resource::TextureUseHandle { .index = rstd::u64(2), .generation = rstd::u64(6) };
    auto insert = [&](owe::resource::TextureUseHandle use, std::string_view name) {
        owe::vulkan::ImageSlots slots;
        slots.slots.resize(1);
        auto allocation = rstd::sync::Arc<owe::vulkan::TextureAllocation>::make(rstd::move(slots));
        return table.Insert(owe::resource_registry::PreparedTexture {
            .use = use,
            .resource =
                owe::resource::TextureHandle { .index = use.index, .generation = rstd::u64(1) },
            .request =
                owe::resource::TextureRequest {
                    .name = rstd::string::String::make(rstd::cppstd::as_str(name).unwrap()),
                },
            .physical = allocation.clone(),
            .image    = allocation->View(),
        });
    };
    ASSERT_TRUE(insert(allowed, "allowed"));
    ASSERT_TRUE(insert(hidden, "hidden"));

    owe::vulkan::PassResourceUses uses;
    uses.textures.push(owe::resource::TextureUseHandle(allowed));
    owe::vulkan::PreparedPassResources view(table, uses);

    EXPECT_TRUE(view.Resolve(allowed).is_some());
    EXPECT_TRUE(view.Resolve(hidden).is_none());
}

TEST(TextureRequest, ResolvesImportedTextureNameFromSnapshotCatalog) {
    owe::Scene scene;
    scene.RegisterTexture(String::make("texture-slot"_str),
                          owe::SceneTexture { .url = "textures/main.png" });

    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
    auto desc_id  = snapshot.textureDescId("texture-slot"_str);
    ASSERT_TRUE(desc_id.is_some());

    auto request = owe::vulkan::MakeImportedTextureRequest("texture-slot", desc_id);
    EXPECT_EQ(request.source->index, desc_id->index);

    auto resolved = owe::vulkan::ResolveImportedTextureName(snapshot, request);
    ASSERT_TRUE(resolved.is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view(resolved->as_str()), "textures/main.png");

    auto lookup_request = owe::vulkan::MakeImportedTextureRequest("texture-slot");
    resolved            = owe::vulkan::ResolveImportedTextureName(snapshot, lookup_request);
    ASSERT_TRUE(resolved.is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view(resolved->as_str()), "textures/main.png");

    auto missing_request = owe::vulkan::MakeImportedTextureRequest("missing");
    EXPECT_TRUE(owe::vulkan::ResolveImportedTextureName(snapshot, missing_request).is_none());
}

TEST(TextureRequest, BuildsRenderTargetCacheKey) {
    owe::SceneRenderTarget rt {
        .width        = i32(256),
        .height       = i32(128),
        .allowReuse   = false,
        .mipmap_level = 3,
    };

    auto request = owe::vulkan::MakeRenderTargetTextureRequest("_rt_default", rt);

    EXPECT_EQ(request.kind, owe::vulkan::TextureRequestKind::RenderTarget);
    EXPECT_EQ(rstd::cppstd::as_string_view(request.name.as_str()), "_rt_default");
    ASSERT_TRUE(request.definition.is_some());
    EXPECT_EQ(request.definition->width, rstd::i32(256));
    EXPECT_EQ(request.definition->height, rstd::i32(128));
    EXPECT_EQ(request.definition->usage, owe::resource::TextureUsage::Color);
    EXPECT_EQ(request.definition->format, owe::TextureFormat::RGBA8);
    EXPECT_EQ(request.definition->mip_levels, rstd::u32(3));
    EXPECT_EQ(request.lifetime, owe::resource::TextureLifetimeClass::Retained);

    rt.allowReuse = true;
    EXPECT_EQ(owe::vulkan::MakeRenderTargetTextureRequest("_rt_default", rt).lifetime,
              owe::resource::TextureLifetimeClass::FrameLocal);

    auto no_mip = owe::vulkan::MakeRenderTargetNoMipTextureRequest("_rt_default", rt);
    ASSERT_TRUE(no_mip.definition.is_some());
    EXPECT_EQ(no_mip.definition->mip_levels, rstd::u32(1));
}

TEST(TextureRequest, DetectsRequestChanges) {
    owe::SceneRenderTarget rt {
        .width        = i32(256),
        .height       = i32(128),
        .allowReuse   = false,
        .mipmap_level = 3,
    };

    auto a = owe::vulkan::MakeRenderTargetTextureRequest("_rt_default", rt);
    auto b = owe::vulkan::MakeRenderTargetTextureRequest("_rt_default", rt);

    EXPECT_TRUE(owe::vulkan::SameTextureRequest(a, b));

    rt.width     = i32(512);
    auto resized = owe::vulkan::MakeRenderTargetTextureRequest("_rt_default", rt);
    EXPECT_FALSE(owe::vulkan::SameTextureRequest(a, resized));

    rstd::Option<owe::vulkan::TextureRequest> target = rstd::Some(a.clone());
    EXPECT_FALSE(owe::vulkan::SetTextureRequestIfChanged(target, b.clone()));
    EXPECT_TRUE(owe::vulkan::SetTextureRequestIfChanged(target, std::move(resized)));
    ASSERT_TRUE(target.is_some());
    ASSERT_TRUE(target->definition.is_some());
    EXPECT_EQ(target->definition->width, rstd::i32(512));
}

TEST(TextureRequest, BuildsMsaaAndDepthCacheKeys) {
    owe::SceneRenderTarget rt {
        .width        = i32(512),
        .height       = i32(256),
        .allowReuse   = false,
        .mipmap_level = 2,
        .sample_count = 4,
    };

    auto msaa =
        owe::vulkan::MakeMsaaTextureRequest("_rt_default::msaa4", rt, VK_SAMPLE_COUNT_4_BIT);
    EXPECT_EQ(msaa.kind, owe::vulkan::TextureRequestKind::RenderTargetMsaa);
    EXPECT_EQ(rstd::cppstd::as_string_view(msaa.name.as_str()), "_rt_default::msaa4");
    ASSERT_TRUE(msaa.definition.is_some());
    EXPECT_EQ(msaa.definition->samples, rstd::u32(4));
    EXPECT_EQ(msaa.lifetime, owe::resource::TextureLifetimeClass::Dedicated);

    auto depth = owe::vulkan::MakeDepthTextureRequest("_rt_default::depth", rt);
    EXPECT_EQ(depth.kind, owe::vulkan::TextureRequestKind::DepthAttachment);
    ASSERT_TRUE(depth.definition.is_some());
    EXPECT_EQ(depth.definition->usage, owe::resource::TextureUsage::Depth);
    EXPECT_EQ(depth.definition->format, owe::TextureFormat::D32F);
    EXPECT_EQ(depth.definition->mip_levels, rstd::u32(1));
    EXPECT_EQ(depth.definition->samples, rstd::u32(4));
    EXPECT_EQ(depth.lifetime, owe::resource::TextureLifetimeClass::Retained);
}

TEST(PassTextureRequestDiagnostics, ReportsPassOwnedTextureRequests) {
    owe::SceneRenderTarget rt {
        .width        = i32(512),
        .height       = i32(256),
        .allowReuse   = false,
        .mipmap_level = 2,
        .sample_count = 4,
    };

    auto imported = owe::vulkan::MakeImportedTextureRequest("textures/main.png");
    auto output   = owe::vulkan::MakeRenderTargetTextureRequest("_rt_default", rt);
    auto msaa =
        owe::vulkan::MakeMsaaTextureRequest("_rt_default::msaa4", rt, VK_SAMPLE_COUNT_4_BIT);
    auto depth = owe::vulkan::MakeDepthTextureRequest("_rt_default::depth", rt);

    auto texture_use =
        owe::resource::TextureUseHandle { .index = rstd::u64(1), .generation = rstd::u64(1) };
    owe::vulkan::CustomShaderPass custom(owe::vulkan::CustomShaderPass::Desc {
        .texture_bindings =
            [texture_use](owe::vulkan::TextureRequest request) mutable {
                std::vector<owe::vulkan::TextureBindingRequest> bindings;
                bindings.push_back(owe::vulkan::TextureBindingRequest {
                    .name    = rstd::string::String::make("textures/main.png"_str),
                    .use     = rstd::Some(texture_use),
                    .request = rstd::Some(std::move(request)),
                });
                return bindings;
            }(std::move(imported)),
        .output         = "_rt_default",
        .output_request = rstd::Some(output.clone()),
        .depth_request  = rstd::Some(std::move(depth)),
    });
    auto                          custom_diag = custom.textureRequestDiagnostics();
    ASSERT_EQ(custom_diag.size(), 3u);
    EXPECT_EQ(custom_diag[0].role, "sampled");
    EXPECT_EQ(custom_diag[0].slot, rstd::u32());
    EXPECT_EQ(*custom_diag[0].use, texture_use);
    EXPECT_EQ(custom_diag[0].request->kind, owe::vulkan::TextureRequestKind::Imported);
    EXPECT_EQ(custom_diag[1].role, "output");
    EXPECT_EQ(custom_diag[1].request->kind, owe::vulkan::TextureRequestKind::RenderTarget);
    EXPECT_EQ(custom_diag[2].role, "depth");
    EXPECT_EQ(custom_diag[2].request->kind, owe::vulkan::TextureRequestKind::DepthAttachment);

    owe::vulkan::CopyPass copy(owe::vulkan::CopyPass::Desc {
        .src         = "_rt_a",
        .dst         = "_rt_b",
        .src_request = rstd::Some(output.clone()),
        .dst_request = rstd::Some(output.clone()),
    });
    auto                  copy_diag = copy.textureRequestDiagnostics();
    ASSERT_EQ(copy_diag.size(), 2u);
    EXPECT_EQ(copy_diag[0].role, "copy-src");
    EXPECT_EQ(copy_diag[1].role, "copy-dst");

    owe::vulkan::PrePass pre(owe::vulkan::PrePass::Desc {
        .result              = "_rt_default",
        .result_request      = rstd::Some(output.clone()),
        .result_msaa_request = rstd::Some(std::move(msaa)),
    });
    auto                 pre_diag = pre.textureRequestDiagnostics();
    ASSERT_EQ(pre_diag.size(), 2u);
    EXPECT_EQ(pre_diag[0].role, "frame-result");
    EXPECT_EQ(pre_diag[1].role, "frame-result-msaa");

    owe::vulkan::FinPass fin(owe::vulkan::FinPass::Desc {
        .result         = "_rt_default",
        .result_request = rstd::Some(std::move(output)),
    });
    auto                 fin_diag = fin.textureRequestDiagnostics();
    ASSERT_EQ(fin_diag.size(), 1u);
    EXPECT_EQ(fin_diag[0].role, "frame-result");
}

TEST(PipelineLayoutPlanner, MergesCompatibleRequirementsWithOneGlobalPrefix) {
    auto pipeline = [](rstd::u64 index) {
        return owe::resource::PipelineUseHandle {
            .index      = index,
            .generation = rstd::u64(1),
        };
    };
    auto global_set = [](rstd::uint32_t stages) {
        auto bindings = rstd::vec::Vec<owe::vulkan::PipelineLayoutBindingRequirement>::make();
        bindings.push(owe::vulkan::PipelineLayoutBindingRequirement {
            .binding          = rstd::u32(),
            .descriptor_type  = rstd::u32(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
            .descriptor_count = rstd::u32(1),
            .stage_flags      = rstd::u32(stages),
            .shared_identity  = rstd::Some(rstd::u64(77)),
        });
        return owe::vulkan::PipelineLayoutSetRequirement {
            .set      = rstd::u32(),
            .bindings = rstd::move(bindings),
        };
    };
    auto local_set = [](rstd::u32 binding, rstd::uint32_t stages) {
        auto bindings = rstd::vec::Vec<owe::vulkan::PipelineLayoutBindingRequirement>::make();
        bindings.push(owe::vulkan::PipelineLayoutBindingRequirement {
            .binding          = binding,
            .descriptor_type  = rstd::u32(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            .descriptor_count = rstd::u32(1),
            .stage_flags      = rstd::u32(stages),
        });
        return owe::vulkan::PipelineLayoutSetRequirement {
            .set             = rstd::u32(2),
            .push_descriptor = true,
            .bindings        = rstd::move(bindings),
        };
    };

    auto first_sets = rstd::vec::Vec<owe::vulkan::PipelineLayoutSetRequirement>::make();
    first_sets.push(global_set(VK_SHADER_STAGE_VERTEX_BIT));
    first_sets.push(local_set(rstd::u32(3), VK_SHADER_STAGE_FRAGMENT_BIT));
    auto second_sets = rstd::vec::Vec<owe::vulkan::PipelineLayoutSetRequirement>::make();
    second_sets.push(global_set(VK_SHADER_STAGE_FRAGMENT_BIT));
    second_sets.push(local_set(rstd::u32(4), VK_SHADER_STAGE_VERTEX_BIT));

    auto requirements = rstd::vec::Vec<owe::vulkan::PipelineLayoutRequirement>::make();
    requirements.push(owe::vulkan::PipelineLayoutRequirement {
        .pipeline        = pipeline(rstd::u64(1)),
        .descriptor_sets = rstd::move(first_sets),
    });
    requirements.push(owe::vulkan::PipelineLayoutRequirement {
        .pipeline        = pipeline(rstd::u64(2)),
        .descriptor_sets = rstd::move(second_sets),
    });

    auto planned = owe::vulkan::PlanPipelineLayouts(requirements.as_slice(), true, rstd::u32(32));
    ASSERT_TRUE(planned.is_ok());
    auto plan = rstd::move(planned).unwrap_unchecked();
    ASSERT_EQ(plan.families.len(), rstd::usize(1));
    const auto& family = plan.families[rstd::usize()];
    ASSERT_EQ(family.pipelines.len(), rstd::usize(2));
    ASSERT_EQ(family.request.descriptor_sets.len(), rstd::usize(3));
    ASSERT_EQ(family.request.descriptor_sets[rstd::usize()].bindings.len(), rstd::usize(1));
    EXPECT_FALSE(family.request.descriptor_sets[rstd::usize()].push_descriptor);
    EXPECT_EQ(family.request.descriptor_sets[rstd::usize()].bindings[rstd::usize()].stageFlags,
              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    EXPECT_TRUE(family.request.descriptor_sets[rstd::usize(1)].bindings.is_empty());
    ASSERT_EQ(family.request.descriptor_sets[rstd::usize(2)].bindings.len(), rstd::usize(2));
    EXPECT_TRUE(family.request.descriptor_sets[rstd::usize(2)].push_descriptor);
    EXPECT_EQ(family.request.descriptor_sets[rstd::usize(2)].bindings[rstd::usize()].binding, 3u);
    EXPECT_EQ(family.request.descriptor_sets[rstd::usize(2)].bindings[rstd::usize(1)].binding, 4u);
}

TEST(PipelineLayoutPlanner, UnifiesDistinctGlobalBindingsAcrossPipelines) {
    auto requirement = [](rstd::u64 pipeline_index, rstd::u32 binding, rstd::u64 identity) {
        auto bindings = rstd::vec::Vec<owe::vulkan::PipelineLayoutBindingRequirement>::make();
        bindings.push(owe::vulkan::PipelineLayoutBindingRequirement {
            .binding          = binding,
            .descriptor_type  = rstd::u32(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
            .descriptor_count = rstd::u32(1),
            .stage_flags      = rstd::u32(VK_SHADER_STAGE_VERTEX_BIT),
            .shared_identity  = rstd::Some(identity),
        });
        auto sets = rstd::vec::Vec<owe::vulkan::PipelineLayoutSetRequirement>::make();
        sets.push(owe::vulkan::PipelineLayoutSetRequirement {
            .set      = rstd::u32(),
            .bindings = rstd::move(bindings),
        });
        return owe::vulkan::PipelineLayoutRequirement {
            .pipeline =
                owe::resource::PipelineUseHandle {
                    .index      = pipeline_index,
                    .generation = rstd::u64(1),
                },
            .descriptor_sets = rstd::move(sets),
        };
    };

    auto requirements = rstd::vec::Vec<owe::vulkan::PipelineLayoutRequirement>::make();
    requirements.push(requirement(rstd::u64(1), rstd::u32(), rstd::u64(10)));
    requirements.push(requirement(rstd::u64(2), rstd::u32(1), rstd::u64(11)));
    requirements.push(requirement(rstd::u64(3), rstd::u32(2), rstd::u64(12)));

    auto planned = owe::vulkan::PlanPipelineLayouts(requirements.as_slice(), true, rstd::u32(32));
    ASSERT_TRUE(planned.is_ok());
    auto plan = rstd::move(planned).unwrap_unchecked();
    ASSERT_EQ(plan.global_bindings.len(), rstd::usize(3));
    ASSERT_EQ(plan.families.len(), rstd::usize(1));
    const auto& bindings =
        plan.families[rstd::usize()].request.descriptor_sets[rstd::usize()].bindings;
    ASSERT_EQ(bindings.len(), rstd::usize(3));
    for (rstd::usize index {}; index < bindings.len(); ++index) {
        EXPECT_EQ(bindings[index].binding, static_cast<rstd::uint32_t>(index.to_primitive()));
        EXPECT_EQ(plan.global_bindings[index].binding,
                  rstd::u32(static_cast<rstd::uint32_t>(index.to_primitive())));
    }
}

TEST(PipelineLayoutPlanner, SplitsLocalConflictsAndRejectsGlobalConflicts) {
    auto make_requirement = [](rstd::u64        pipeline_index,
                               VkDescriptorType local_type,
                               rstd::u64        global_identity) {
        auto global_bindings =
            rstd::vec::Vec<owe::vulkan::PipelineLayoutBindingRequirement>::make();
        global_bindings.push(owe::vulkan::PipelineLayoutBindingRequirement {
            .binding          = rstd::u32(),
            .descriptor_type  = rstd::u32(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
            .descriptor_count = rstd::u32(1),
            .stage_flags      = rstd::u32(VK_SHADER_STAGE_ALL_GRAPHICS),
            .shared_identity  = rstd::Some(global_identity),
        });
        auto local_bindings = rstd::vec::Vec<owe::vulkan::PipelineLayoutBindingRequirement>::make();
        local_bindings.push(owe::vulkan::PipelineLayoutBindingRequirement {
            .binding          = rstd::u32(),
            .descriptor_type  = rstd::u32(local_type),
            .descriptor_count = rstd::u32(1),
            .stage_flags      = rstd::u32(VK_SHADER_STAGE_FRAGMENT_BIT),
        });
        auto sets = rstd::vec::Vec<owe::vulkan::PipelineLayoutSetRequirement>::make();
        sets.push(owe::vulkan::PipelineLayoutSetRequirement {
            .set      = rstd::u32(),
            .bindings = rstd::move(global_bindings),
        });
        sets.push(owe::vulkan::PipelineLayoutSetRequirement {
            .set             = rstd::u32(1),
            .push_descriptor = true,
            .bindings        = rstd::move(local_bindings),
        });
        return owe::vulkan::PipelineLayoutRequirement {
            .pipeline =
                owe::resource::PipelineUseHandle {
                    .index      = pipeline_index,
                    .generation = rstd::u64(1),
                },
            .descriptor_sets = rstd::move(sets),
        };
    };

    auto local_conflicts = rstd::vec::Vec<owe::vulkan::PipelineLayoutRequirement>::make();
    local_conflicts.push(
        make_requirement(rstd::u64(1), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, rstd::u64(9)));
    local_conflicts.push(
        make_requirement(rstd::u64(2), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, rstd::u64(9)));
    auto split = owe::vulkan::PlanPipelineLayouts(local_conflicts.as_slice(), true, rstd::u32(32));
    ASSERT_TRUE(split.is_ok());
    auto split_plan = rstd::move(split).unwrap_unchecked();
    EXPECT_EQ(split_plan.families.len(), rstd::usize(2));
    ASSERT_EQ(split_plan.conflicts.len(), rstd::usize(1));
    EXPECT_FALSE(split_plan.conflicts[rstd::usize()].message.is_empty());

    auto global_conflicts = rstd::vec::Vec<owe::vulkan::PipelineLayoutRequirement>::make();
    global_conflicts.push(
        make_requirement(rstd::u64(1), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, rstd::u64(9)));
    global_conflicts.push(
        make_requirement(rstd::u64(2), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, rstd::u64(10)));
    auto rejected =
        owe::vulkan::PlanPipelineLayouts(global_conflicts.as_slice(), true, rstd::u32(32));
    EXPECT_TRUE(rejected.is_err());
}

TEST(PipelineLayoutPlanner, ResolvesUnsupportedAndOversizedPushSetsAsOrdinary) {
    auto bindings = rstd::vec::Vec<owe::vulkan::PipelineLayoutBindingRequirement>::make();
    bindings.push(owe::vulkan::PipelineLayoutBindingRequirement {
        .binding          = rstd::u32(4),
        .descriptor_type  = rstd::u32(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        .descriptor_count = rstd::u32(3),
        .stage_flags      = rstd::u32(VK_SHADER_STAGE_FRAGMENT_BIT),
    });
    auto sets = rstd::vec::Vec<owe::vulkan::PipelineLayoutSetRequirement>::make();
    sets.push(owe::vulkan::PipelineLayoutSetRequirement {
        .set             = rstd::u32(3),
        .push_descriptor = true,
        .bindings        = rstd::move(bindings),
    });
    auto requirements = rstd::vec::Vec<owe::vulkan::PipelineLayoutRequirement>::make();
    requirements.push(owe::vulkan::PipelineLayoutRequirement {
        .pipeline =
            owe::resource::PipelineUseHandle {
                .index      = rstd::u64(1),
                .generation = rstd::u64(1),
            },
        .descriptor_sets = rstd::move(sets),
    });

    auto unsupported =
        owe::vulkan::PlanPipelineLayouts(requirements.as_slice(), false, rstd::u32());
    ASSERT_TRUE(unsupported.is_ok());
    EXPECT_FALSE(unsupported.unwrap_unchecked()
                     .families[rstd::usize()]
                     .request.descriptor_sets[rstd::usize(3)]
                     .push_descriptor);

    auto oversized = owe::vulkan::PlanPipelineLayouts(requirements.as_slice(), true, rstd::u32(2));
    ASSERT_TRUE(oversized.is_ok());
    EXPECT_FALSE(oversized.unwrap_unchecked()
                     .families[rstd::usize()]
                     .request.descriptor_sets[rstd::usize(3)]
                     .push_descriptor);

    VkPhysicalDeviceLimits limits {};
    limits.maxBoundDescriptorSets   = 8;
    limits.maxDescriptorSetSamplers = 2;
    auto over_device_limit          = owe::vulkan::PlanPipelineLayouts(
        requirements.as_slice(), true, rstd::u32(32), rstd::u32(128), &limits);
    EXPECT_TRUE(over_device_limit.is_err());
}

TEST(PipelineLayoutPlanner, CanonicalizesPushConstantsBeforeCreatingFamilies) {
    auto requirement = [](rstd::u64 index, rstd::uint32_t stage, rstd::uint32_t offset) {
        auto ranges = rstd::vec::Vec<VkPushConstantRange>::make();
        ranges.push(VkPushConstantRange {
            .stageFlags = stage,
            .offset     = offset,
            .size       = 16,
        });
        return owe::vulkan::PipelineLayoutRequirement {
            .pipeline =
                owe::resource::PipelineUseHandle {
                    .index      = index,
                    .generation = rstd::u64(1),
                },
            .push_constants = rstd::move(ranges),
        };
    };

    auto compatible = rstd::vec::Vec<owe::vulkan::PipelineLayoutRequirement>::make();
    compatible.push(requirement(rstd::u64(1), VK_SHADER_STAGE_VERTEX_BIT, 0));
    compatible.push(requirement(rstd::u64(2), VK_SHADER_STAGE_FRAGMENT_BIT, 0));
    auto planned = owe::vulkan::PlanPipelineLayouts(
        compatible.as_slice(), true, rstd::u32(32), rstd::u32(128));
    ASSERT_TRUE(planned.is_ok());
    auto plan = rstd::move(planned).unwrap_unchecked();
    ASSERT_EQ(plan.families.len(), rstd::usize(1));
    ASSERT_EQ(plan.families[rstd::usize()].request.push_constants.len(), rstd::usize(1));
    EXPECT_EQ(plan.families[rstd::usize()].request.push_constants[rstd::usize()].stageFlags,
              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

    auto too_small =
        owe::vulkan::PlanPipelineLayouts(compatible.as_slice(), true, rstd::u32(32), rstd::u32(8));
    EXPECT_TRUE(too_small.is_err());

    auto overlapping = rstd::vec::Vec<owe::vulkan::PipelineLayoutRequirement>::make();
    overlapping.push(requirement(rstd::u64(1), VK_SHADER_STAGE_VERTEX_BIT, 0));
    overlapping.push(requirement(rstd::u64(2), VK_SHADER_STAGE_VERTEX_BIT, 8));
    auto rejected = owe::vulkan::PlanPipelineLayouts(
        overlapping.as_slice(), true, rstd::u32(32), rstd::u32(128));
    EXPECT_TRUE(rejected.is_err());

    auto exact_then_partial = rstd::vec::Vec<owe::vulkan::PipelineLayoutRequirement>::make();
    auto disjoint_ranges    = rstd::vec::Vec<VkPushConstantRange>::make();
    disjoint_ranges.push(VkPushConstantRange {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset     = 0,
        .size       = 16,
    });
    disjoint_ranges.push(VkPushConstantRange {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 8,
        .size       = 16,
    });
    exact_then_partial.push(owe::vulkan::PipelineLayoutRequirement {
        .pipeline =
            owe::resource::PipelineUseHandle {
                .index      = rstd::u64(1),
                .generation = rstd::u64(1),
            },
        .push_constants = rstd::move(disjoint_ranges),
    });
    exact_then_partial.push(requirement(rstd::u64(2), VK_SHADER_STAGE_FRAGMENT_BIT, 0));
    auto exact_overlap = owe::vulkan::PlanPipelineLayouts(
        exact_then_partial.as_slice(), true, rstd::u32(32), rstd::u32(128));
    EXPECT_TRUE(exact_overlap.is_err());
}

TEST(PipelineLayoutPlanner, SplitsCompatibleSupersetsAtDeviceLimits) {
    auto requirement = [](rstd::u64 pipeline_index, rstd::u32 binding) {
        auto bindings = rstd::vec::Vec<owe::vulkan::PipelineLayoutBindingRequirement>::make();
        bindings.push(owe::vulkan::PipelineLayoutBindingRequirement {
            .binding          = binding,
            .descriptor_type  = rstd::u32(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            .descriptor_count = rstd::u32(1),
            .stage_flags      = rstd::u32(VK_SHADER_STAGE_FRAGMENT_BIT),
        });
        auto sets = rstd::vec::Vec<owe::vulkan::PipelineLayoutSetRequirement>::make();
        sets.push(owe::vulkan::PipelineLayoutSetRequirement {
            .set      = rstd::u32(1),
            .bindings = rstd::move(bindings),
        });
        return owe::vulkan::PipelineLayoutRequirement {
            .pipeline =
                owe::resource::PipelineUseHandle {
                    .index      = pipeline_index,
                    .generation = rstd::u64(1),
                },
            .descriptor_sets = rstd::move(sets),
        };
    };

    auto requirements = rstd::vec::Vec<owe::vulkan::PipelineLayoutRequirement>::make();
    requirements.push(requirement(rstd::u64(1), rstd::u32(3)));
    requirements.push(requirement(rstd::u64(2), rstd::u32(4)));
    VkPhysicalDeviceLimits limits {};
    limits.maxBoundDescriptorSets   = 8;
    limits.maxDescriptorSetSamplers = 1;
    auto planned                    = owe::vulkan::PlanPipelineLayouts(
        requirements.as_slice(), false, rstd::u32(), rstd::u32(128), &limits);
    ASSERT_TRUE(planned.is_ok());
    auto plan = rstd::move(planned).unwrap_unchecked();
    EXPECT_EQ(plan.families.len(), rstd::usize(2));
    EXPECT_EQ(plan.conflicts.len(), rstd::usize(1));
}

TEST(PipelineCacheDiagnostics, RecordsStableKeys) {
    auto make_request = [](VkPrimitiveTopology topology) {
        owe::vulkan::PipelineResourceRequest request;
        request.topology        = topology;
        request.pipeline_layout = owe::resource::PipelineLayoutHandle {
            .index      = rstd::u64(7),
            .generation = rstd::u64(1),
        };
        request.vertex_bindings.push_back(VkVertexInputBindingDescription {
            .binding   = 0,
            .stride    = 16,
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        });
        request.vertex_attrs.push_back(VkVertexInputAttributeDescription {
            .location = 0,
            .binding  = 0,
            .format   = VK_FORMAT_R32G32B32A32_SFLOAT,
            .offset   = 0,
        });
        auto spv         = rstd::boxed::Box<owe::vulkan::ShaderSpv>::make();
        spv->stage       = owe::ShaderType::VERTEX;
        spv->entry_point = "main";
        spv->spirv       = { 1u, 2u, 3u, 4u };
        request.shader_stages.push_back(std::move(spv));
        return request;
    };

    auto key_a =
        owe::vulkan::MakePipelineCacheKey(make_request(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST));
    auto key_b =
        owe::vulkan::MakePipelineCacheKey(make_request(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST));
    auto key_c = owe::vulkan::MakePipelineCacheKey(make_request(VK_PRIMITIVE_TOPOLOGY_POINT_LIST));
    auto layout_request                  = make_request(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    layout_request.pipeline_layout.index = rstd::u64(8);
    auto key_layout                      = owe::vulkan::MakePipelineCacheKey(layout_request);
    auto desc_request                    = make_request(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    auto key_from_desc =
        owe::vulkan::MakePipelineCacheKey(owe::vulkan::MakePipelineResourceDesc(desc_request));
    auto primitive_restart_request = make_request(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    primitive_restart_request.primitive_restart_enable = true;
    auto key_primitive_restart       = owe::vulkan::MakePipelineCacheKey(primitive_restart_request);
    auto viewport_request            = make_request(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    viewport_request.viewport_count  = 2u;
    auto key_viewport                = owe::vulkan::MakePipelineCacheKey(viewport_request);
    auto logic_op_request            = make_request(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    logic_op_request.logic_op_enable = true;
    logic_op_request.logic_op        = VK_LOGIC_OP_XOR;
    auto key_logic_op                = owe::vulkan::MakePipelineCacheKey(logic_op_request);
    auto create_flags_request        = make_request(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    create_flags_request.create_flags =
        static_cast<VkPipelineCreateFlags>(VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT);
    auto key_create_flags        = owe::vulkan::MakePipelineCacheKey(create_flags_request);
    auto subpass_request         = make_request(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    subpass_request.subpass      = 1u;
    auto key_subpass             = owe::vulkan::MakePipelineCacheKey(subpass_request);
    auto blend_constants_request = make_request(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    blend_constants_request.blend_constants[rstd::usize()] = 1.0f;
    auto key_blend_constants   = owe::vulkan::MakePipelineCacheKey(blend_constants_request);
    auto dynamic_state_request = make_request(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    dynamic_state_request.dynamic_states = { VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_VIEWPORT };
    auto key_dynamic_state_order         = owe::vulkan::MakePipelineCacheKey(dynamic_state_request);
    dynamic_state_request.dynamic_states = { VK_DYNAMIC_STATE_VIEWPORT };
    auto key_dynamic_state               = owe::vulkan::MakePipelineCacheKey(dynamic_state_request);

    EXPECT_FALSE(key_a.bytes.empty());
    EXPECT_TRUE(owe::vulkan::SamePipelineCacheKey(key_a, key_b));
    EXPECT_TRUE(owe::vulkan::SamePipelineCacheKey(key_a, key_from_desc));
    EXPECT_FALSE(owe::vulkan::SamePipelineCacheKey(key_a, key_c));
    EXPECT_FALSE(owe::vulkan::SamePipelineCacheKey(key_a, key_layout));
    EXPECT_FALSE(owe::vulkan::SamePipelineCacheKey(key_a, key_primitive_restart));
    EXPECT_FALSE(owe::vulkan::SamePipelineCacheKey(key_a, key_viewport));
    EXPECT_FALSE(owe::vulkan::SamePipelineCacheKey(key_a, key_logic_op));
    EXPECT_FALSE(owe::vulkan::SamePipelineCacheKey(key_a, key_create_flags));
    EXPECT_FALSE(owe::vulkan::SamePipelineCacheKey(key_a, key_subpass));
    EXPECT_FALSE(owe::vulkan::SamePipelineCacheKey(key_a, key_blend_constants));
    EXPECT_TRUE(owe::vulkan::SamePipelineCacheKey(key_a, key_dynamic_state_order));
    EXPECT_FALSE(owe::vulkan::SamePipelineCacheKey(key_a, key_dynamic_state));

    auto colliding_a = owe::vulkan::PipelineCacheKey {
        .value = key_a.value,
        .bytes = Bytes({ 0x01u }),
    };
    auto colliding_b = owe::vulkan::PipelineCacheKey {
        .value = key_a.value,
        .bytes = Bytes({ 0x02u }),
    };
    EXPECT_FALSE(owe::vulkan::SamePipelineCacheKey(colliding_a, colliding_b));

    owe::vulkan::PipelineCacheDiagnostics diagnostics;
    auto                                  first = diagnostics.Record(key_a);
    EXPECT_FALSE(first.hit);
    EXPECT_EQ(first.observed_count, rstd::u64(1));

    auto second = diagnostics.Record(key_b);
    EXPECT_TRUE(second.hit);
    EXPECT_EQ(second.observed_count, rstd::u64(2));

    auto third = diagnostics.Record(key_c);
    EXPECT_FALSE(third.hit);
    EXPECT_EQ(third.observed_count, rstd::u64(1));

    auto collision_first = diagnostics.Record(colliding_a);
    EXPECT_FALSE(collision_first.hit);
    EXPECT_EQ(collision_first.observed_count, rstd::u64(1));

    auto collision_second = diagnostics.Record(colliding_b);
    EXPECT_FALSE(collision_second.hit);
    EXPECT_EQ(collision_second.observed_count, rstd::u64(1));

    std::unordered_map<owe::vulkan::PipelineCacheKey,
                       int,
                       owe::vulkan::CanonicalCacheKeyStdHash,
                       owe::vulkan::PipelineCacheKeyEqual>
        cache_entries;
    cache_entries.emplace(colliding_a, 1);
    cache_entries.emplace(colliding_b, 2);
    EXPECT_EQ(cache_entries.size(), 2u);
}

TEST(RenderPassCacheKey, TracksRenderPassCompatibilityInputs) {
    auto make_request = [](VkFormat color_format, VkSampleCountFlagBits samples, bool depth) {
        owe::vulkan::PipelineResourceRequest request;
        request.color_format                     = color_format;
        request.color_final_layout               = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        request.color_load_op                    = VK_ATTACHMENT_LOAD_OP_CLEAR;
        request.depth_load_op                    = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        request.has_depth_attachment             = depth;
        request.multisample.rasterizationSamples = samples;
        return request;
    };

    auto key_a = owe::vulkan::MakeRenderPassCacheKey(
        make_request(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, true));
    auto key_b = owe::vulkan::MakeRenderPassCacheKey(
        make_request(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, true));
    auto key_format = owe::vulkan::MakeRenderPassCacheKey(
        make_request(VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, true));
    auto key_samples = owe::vulkan::MakeRenderPassCacheKey(
        make_request(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_4_BIT, true));
    auto key_depth = owe::vulkan::MakeRenderPassCacheKey(
        make_request(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, false));
    auto depth_only = make_request(VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, true);
    depth_only.has_color_attachment = false;
    depth_only.depth_load_op        = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_only.depth_final_layout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    auto key_depth_only             = owe::vulkan::MakeRenderPassCacheKey(depth_only);
    auto depth_attachment_layout = make_request(VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, true);
    depth_attachment_layout.has_color_attachment = false;
    depth_attachment_layout.depth_load_op        = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment_layout.depth_final_layout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    auto key_depth_attachment_layout = owe::vulkan::MakeRenderPassCacheKey(depth_attachment_layout);
    auto desc_store                  = owe::vulkan::MakeRenderPassResourceDesc(
        make_request(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, true));
    desc_store.color_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    auto key_store            = owe::vulkan::MakeRenderPassCacheKey(desc_store);
    auto desc_layout          = owe::vulkan::MakeRenderPassResourceDesc(
        make_request(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, true));
    desc_layout.color_attachment_layout = VK_IMAGE_LAYOUT_GENERAL;
    auto key_layout                     = owe::vulkan::MakeRenderPassCacheKey(desc_layout);

    EXPECT_FALSE(key_a.bytes.empty());
    EXPECT_TRUE(owe::vulkan::SameRenderPassCacheKey(key_a, key_b));
    EXPECT_FALSE(owe::vulkan::SameRenderPassCacheKey(key_a, key_format));
    EXPECT_FALSE(owe::vulkan::SameRenderPassCacheKey(key_a, key_samples));
    EXPECT_FALSE(owe::vulkan::SameRenderPassCacheKey(key_a, key_depth));
    EXPECT_FALSE(owe::vulkan::SameRenderPassCacheKey(key_a, key_depth_only));
    EXPECT_FALSE(owe::vulkan::SameRenderPassCacheKey(key_depth_only, key_depth_attachment_layout));
    EXPECT_FALSE(owe::vulkan::SameRenderPassCacheKey(key_a, key_store));
    EXPECT_FALSE(owe::vulkan::SameRenderPassCacheKey(key_a, key_layout));

    auto colliding_a = owe::vulkan::RenderPassCacheKey {
        .value = key_a.value,
        .bytes = Bytes({ 0x01u }),
    };
    auto colliding_b = owe::vulkan::RenderPassCacheKey {
        .value = key_a.value,
        .bytes = Bytes({ 0x02u }),
    };
    EXPECT_FALSE(owe::vulkan::SameRenderPassCacheKey(colliding_a, colliding_b));

    std::unordered_map<owe::vulkan::RenderPassCacheKey,
                       int,
                       owe::vulkan::CanonicalCacheKeyStdHash,
                       owe::vulkan::RenderPassCacheKeyEqual>
        cache_entries;
    cache_entries.emplace(colliding_a, 1);
    cache_entries.emplace(colliding_b, 2);
    EXPECT_EQ(cache_entries.size(), 2u);
}

TEST(FramebufferCacheDiagnostics, RecordsStableFramebufferKeys) {
    owe::vulkan::FramebufferResourceRequest request {
        .render_pass_key =
            owe::vulkan::RenderPassCacheKey {
                .value = 17u,
                .bytes = Bytes({ 0x17u }),
            },
        .attachments = { Attachment(0x101u, 101u, { 0x01u }), Attachment(0x102u, 102u, { 0x02u }) },
        .extent      = { 320u, 180u },
    };

    auto key_a = owe::vulkan::MakeFramebufferCacheKey(request);
    auto key_b = owe::vulkan::MakeFramebufferCacheKey(request);

    auto resized         = request;
    resized.extent.width = 640u;
    auto key_resized     = owe::vulkan::MakeFramebufferCacheKey(resized);

    auto layered    = request;
    layered.layers  = 2u;
    auto key_layers = owe::vulkan::MakeFramebufferCacheKey(layered);

    auto different_attachment           = request;
    different_attachment.attachments[1] = Attachment(0x103u, 103u, { 0x03u });
    auto key_attachment = owe::vulkan::MakeFramebufferCacheKey(different_attachment);

    auto different_attachment_identity                    = request;
    different_attachment_identity.attachments[1].identity = AttachmentIdentity(104u, { 0x04u });
    auto key_attachment_identity =
        owe::vulkan::MakeFramebufferCacheKey(different_attachment_identity);

    auto different_attachment_view                = request;
    different_attachment_view.attachments[1].view = ImageView(0x104u);
    auto key_attachment_view = owe::vulkan::MakeFramebufferCacheKey(different_attachment_view);

    auto different_render_pass            = request;
    different_render_pass.render_pass_key = owe::vulkan::RenderPassCacheKey {
        .value = 17u,
        .bytes = Bytes({ 0x18u }),
    };
    auto key_render_pass = owe::vulkan::MakeFramebufferCacheKey(different_render_pass);

    EXPECT_FALSE(key_a.bytes.empty());
    EXPECT_TRUE(owe::vulkan::SameFramebufferCacheKey(key_a, key_b));
    EXPECT_FALSE(owe::vulkan::SameFramebufferCacheKey(key_a, key_resized));
    EXPECT_FALSE(owe::vulkan::SameFramebufferCacheKey(key_a, key_layers));
    EXPECT_FALSE(owe::vulkan::SameFramebufferCacheKey(key_a, key_attachment));
    EXPECT_FALSE(owe::vulkan::SameFramebufferCacheKey(key_a, key_attachment_identity));
    EXPECT_FALSE(owe::vulkan::SameFramebufferCacheKey(key_a, key_attachment_view));
    EXPECT_FALSE(owe::vulkan::SameFramebufferCacheKey(key_a, key_render_pass));

    auto colliding_a = owe::vulkan::FramebufferCacheKey {
        .value = key_a.value,
        .bytes = Bytes({ 0x01u }),
    };
    auto colliding_b = owe::vulkan::FramebufferCacheKey {
        .value = key_a.value,
        .bytes = Bytes({ 0x02u }),
    };
    EXPECT_FALSE(owe::vulkan::SameFramebufferCacheKey(colliding_a, colliding_b));

    owe::vulkan::FramebufferCacheDiagnostics diagnostics;
    auto                                     first = diagnostics.Record(key_a);
    EXPECT_FALSE(first.hit);
    EXPECT_EQ(first.observed_count, rstd::u64(1));

    auto second = diagnostics.Record(key_b);
    EXPECT_TRUE(second.hit);
    EXPECT_EQ(second.observed_count, rstd::u64(2));

    auto third = diagnostics.Record(key_resized);
    EXPECT_FALSE(third.hit);
    EXPECT_EQ(third.observed_count, rstd::u64(1));

    auto collision_first = diagnostics.Record(colliding_a);
    EXPECT_FALSE(collision_first.hit);
    EXPECT_EQ(collision_first.observed_count, rstd::u64(1));

    auto collision_second = diagnostics.Record(colliding_b);
    EXPECT_FALSE(collision_second.hit);
    EXPECT_EQ(collision_second.observed_count, rstd::u64(1));

    std::unordered_map<owe::vulkan::FramebufferCacheKey,
                       int,
                       owe::vulkan::CanonicalCacheKeyStdHash,
                       owe::vulkan::FramebufferCacheKeyEqual>
        cache_entries;
    cache_entries.emplace(colliding_a, 1);
    cache_entries.emplace(colliding_b, 2);
    EXPECT_EQ(cache_entries.size(), 2u);
}

TEST(FramebufferAttachmentIdentity, TracksTextureGeneration) {
    owe::SceneRenderTarget rt {
        .width      = i32(320),
        .height     = i32(180),
        .allowReuse = true,
    };
    auto request = owe::vulkan::MakeRenderTargetTextureRequest("_rt_default", rt);

    owe::vulkan::ImageParameters image_a;
    image_a.handle       = reinterpret_cast<VkImage>(0x201u);
    image_a.view         = ImageView(0x301u);
    image_a.extent       = { 320u, 180u, 1u };
    image_a.mipmap_level = 1u;
    image_a.generation   = rstd::u64(10);
    auto image_b         = image_a;
    image_b.generation   = rstd::u64(11);

    auto attachment_a = owe::vulkan::MakeFramebufferAttachment(request, image_a);
    auto attachment_b = owe::vulkan::MakeFramebufferAttachment(request, image_b);
    EXPECT_NE(attachment_a.identity.value, 0u);
    EXPECT_FALSE(attachment_a.identity.bytes.empty());
    EXPECT_NE(attachment_a.identity.bytes, attachment_b.identity.bytes);

    owe::vulkan::FramebufferResourceRequest framebuffer_a {
        .render_pass_key =
            owe::vulkan::RenderPassCacheKey {
                .value = 17u,
                .bytes = Bytes({ 0x17u }),
            },
        .attachments = { attachment_a },
        .extent      = { 320u, 180u },
    };
    auto framebuffer_b        = framebuffer_a;
    framebuffer_b.attachments = { attachment_b };

    EXPECT_FALSE(
        owe::vulkan::SameFramebufferCacheKey(owe::vulkan::MakeFramebufferCacheKey(framebuffer_a),
                                             owe::vulkan::MakeFramebufferCacheKey(framebuffer_b)));
}
