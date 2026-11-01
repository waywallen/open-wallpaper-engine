#include <cstring>

#include <rstd/test/gtest.hpp>

import rstd;
import vvk;
import rstd.cppstd;
import wescene.scene;
import wescene.types;
import wescene.vulkan;
import wescene.vulkan_render;
import vrento;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

using namespace owe::vulkan;
using namespace rstd;
using namespace vrento::vulkan;
using namespace owe::resource_registry;

struct CountingShaderBackend {
    usize* reflections;
};

namespace rstd
{
template<>
struct Impl<vrento::vulkan::ShaderBackend, CountingShaderBackend>
    : ImplBase<CountingShaderBackend> {
    bool Preprocess(ref<str> source, owe::ShaderType stage, SourceLang language,
                    String& out) const {
        return owe::vulkan::Preprocess(source, stage, language, out);
    }
    auto CompileAndLinkShaderUnits(slice<ShaderCompUnit> units, const ShaderCompOpt& options,
                                   Vec<Uni_ShaderSpv>& out) const
        -> Result<empty, vrento::resource::ResourceError> {
        return owe::vulkan::TryCompileAndLinkShaderUnits(units, options, out);
    }
    bool GenReflect(slice<owe::ShaderCode> codes, Vec<Uni_ShaderSpv>& stages,
                    ShaderReflected& out) const {
        ++*self().reflections;
        return owe::vulkan::GenReflect(codes, stages, out);
    }
};
} // namespace rstd

namespace
{
struct TestPassDesc {
    using PassDescriptor = vrento::pass::PassDesc;
    vrento::vulkan::VulkanPassIO io;
};
template<class T>
auto make_test_pass(rstd::uint64_t scope, rstd::uint64_t revision,
                    vrento::vulkan::PassExecutionInfo execution, T desc)
    -> vrento::vulkan::PassInput {
    auto id = execution.id;
    if constexpr (requires { desc.resources; }) {
        namespace vk = vrento::vulkan;
        if (execution.graphics) {
            auto convert = [scope](const auto& value) -> rstd::Option<vk::GraphicsAttachment> {
                if (! value) return rstd::None();
                return rstd::Some(
                    vk::GraphicsAttachment { .target = vrento::pass::ContentRef::from_version(
                                                 scope, { value->resource, value->version }),
                                             .load    = value->load,
                                             .store   = value->store,
                                             .clear   = value->clear,
                                             .stencil = value->stencil });
            };
            desc.resources.attachments.color   = convert(execution.graphics->color);
            desc.resources.attachments.depth   = convert(execution.graphics->depth);
            desc.resources.attachments.resolve = convert(execution.graphics->resolve);
        }
        for (const auto& use : execution.uses) {
            if (use.access != vk::GraphAccess::Read) continue;
            if (use.usage == vk::PassUsage::Sampled)
                desc.resources.textures.push(
                    { vrento::pass::ContentRef::from_version(scope, { use.resource, use.version }),
                      use.range,
                      use.stages });
            else if (use.usage != vk::PassUsage::ColorAttachment &&
                     use.usage != vk::PassUsage::DepthAttachment)
                desc.resources.buffers.push(
                    { vrento::pass::ContentRef::from_version(scope, { use.resource, use.version }),
                      use.range,
                      use.usage,
                      use.stages });
        }
        if (! execution.graphics)
            desc.resources.attachments.color = rstd::Some(vk::GraphicsAttachment {
                .target = vrento::pass::ContentRef::from_version(scope, { { 1, scope }, 1 }) });
    } else {
        EXPECT_TRUE(execution.uses.is_empty());
        desc.io.kind  = execution.kind;
        desc.io.queue = execution.queue;
    }
    return vrento::vulkan::PassInput::publish(scope, revision, id, rstd::move(desc)).unwrap();
}
} // namespace

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
        if (reject)
            return rstd::Err(
                owe::resource::ResourceError { owe::resource::ResourceErrorKind::BackendFailure });
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
    bool                           reject {};
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

Vec<u8> Bytes(std::initializer_list<unsigned char> values) {
    Vec<u8> bytes;
    bytes.reserve(usize(values.size()));
    for (auto value : values) bytes.push(u8(value));
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

Arc<owe::SceneMesh> MakeUniformMesh(Arc<owe::SceneShader> shader) {
    auto               mesh = Arc<owe::SceneMesh>::make();
    owe::SceneMaterial material;
    material.customShader.shader = Some(rstd::move(shader));
    mesh->AddMaterial(std::move(material));
    owe::SceneMesh::Submesh submesh;
    submesh.material_slot = u32();
    mesh->Submeshes().push(std::move(submesh));
    return mesh;
}

float ReadFloat(const rstd::vec::Vec<rstd::u8>& bytes, rstd::usize offset) {
    float value {};
    std::memcpy(&value, bytes.data() + offset.to_primitive(), sizeof(value));
    return value;
}

} // namespace

TEST(SceneDrawPolicy, ConfiguresAlphaToCoverageWithoutColorBlending) {
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

TEST(SceneDrawPolicy, PreservesExplicitDepthWriteForEveryBlendMode) {
    for (auto mode : { owe::BlendMode::Disable,
                       owe::BlendMode::Normal,
                       owe::BlendMode::AlphaToCoverage,
                       owe::BlendMode::Translucent,
                       owe::BlendMode::Additive }) {
        owe::SceneMaterial material;
        material.SetBlendMode(mode);
        for (bool write : { false, true }) {
            material.SetDepthWrite(write);
            for (bool test : { false, true }) {
                material.SetDepthTest(test);
                VkPipelineDepthStencilStateCreateInfo state {};
                owe::vulkan::SetDepthState(material.Pipeline(), state);
                EXPECT_EQ(state.depthTestEnable != 0, test);
                EXPECT_EQ(state.depthWriteEnable != 0, write);
                EXPECT_EQ(owe::vulkan::EffectiveDepthWrite(material.Pipeline()), write);
                EXPECT_EQ(owe::vulkan::UsesDepthAttachment(material.Pipeline()), test || write);
            }
        }
    }
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

    auto shader = Arc<owe::SceneShader>::make();
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
    owe::vulkan::SceneRenderUniformFrameContext frame_impl(
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

TEST(UniformBufferBinding, RetainsDefaultVersionAndAppliesLiveOverridesAfterRetry) {
    owe::Scene scene;
    auto       registrar   = dyn<owe::UniformSourceRegistrar>::from_ref(scene);
    auto       attachments = dyn<owe::UniformAttachmentWriter>::from_ref(scene);
    auto       source      = registrar->Register(
        Box<dyn<owe::UniformSource>>::make(uniform_test::StaticSource("live", 2.0f)));
    ASSERT_TRUE(attachments->AttachGlobal(source));
    auto publish = [](float value) {
        owe::ShaderValues defaults;
        for (auto name : { "fallback"_str, "instance"_str, "live"_str })
            (void)defaults.insert(String::make(name), owe::ShaderValue(value));
        return owe::SceneShaderDefaults::from(rstd::move(defaults));
    };
    auto shader              = Arc<owe::SceneShader>::make();
    shader->default_uniforms = publish(0.25f);
    auto node                = Arc<owe::SceneNode>::make();
    node->AddMesh(MakeUniformMesh(shader.clone()));
    scene.RootMut()->AppendChild(node.clone());
    scene.RebuildResourceIndex();
    auto draw = scene.ResourceIndex().drawItemFor(*scene.ResourceIndex().nodeId(*node), u32());
    ASSERT_TRUE(draw);
    auto& material = *node->Mesh()->MaterialSlots()[usize()];
    ASSERT_TRUE(material.SetShaderValue("instance"_str, owe::ShaderValue(1.0f)));
    ASSERT_TRUE(material.SetShaderValue("live"_str, owe::ShaderValue(1.5f)));
    owe::resource::ShaderArtifactUniformBlock block { .name = "local"_Str, .size = usize(12) };
    for (auto name : { "fallback"_str, "instance"_str, "live"_str }) {
        block.members.push({ .name   = String::make(name),
                             .offset = rstd::as_cast<u32>(block.members.len() * usize(4)),
                             .size   = usize(4) });
    }
    SceneUniformBindingPrepareContext prepare_impl(scene);
    auto prepare = dyn<UniformBindingPrepareContext>::from_ref(prepare_impl);
    const owe::resource::BufferUseHandle buffer { .index = u64(3), .generation = u64(1) };
    auto old_binding  = MakeUniformBufferBinding(prepare.as_ref(), *draw, buffer, block);
    auto same_version = MakeUniformBufferBinding(prepare.as_ref(), *draw, buffer, block);
    ASSERT_TRUE(old_binding.is_ok());
    ASSERT_TRUE(same_version.is_ok());
    shader->default_uniforms = publish(0.75f);
    auto new_binding         = MakeUniformBufferBinding(prepare.as_ref(), *draw, buffer, block);
    ASSERT_TRUE(new_binding.is_ok());
    auto override_material = material;
    ASSERT_TRUE(override_material.SetShaderValue("instance"_str, owe::ShaderValue(3.0f)));
    auto override_binding =
        MakeUniformBufferBinding(prepare.as_ref(),
                                 *draw,
                                 buffer,
                                 block,
                                 {},
                                 owe::SceneRenderViewKind::Primary,
                                 owe::ShaderMatrixConvention::ColumnVector,
                                 owe::ShaderMatrixAbi::NativeSpirv,
                                 Some(ref<owe::SceneMaterial>::from_raw_parts(&override_material)));
    ASSERT_TRUE(override_binding.is_ok());
    auto                           textures = dyn<owe::SceneTextureAnimationView>::from_ref(scene);
    SceneRenderUniformFrameContext frame_impl(
        scene.Runtime().Frame(), { 1280.0f, 720.0f }, textures.as_ref());
    auto                       frame = dyn<UniformBufferFrameContext>::from_ref(frame_impl);
    uniform_test::BufferWriter writer;
    auto                       target = dyn<owe::resource::BufferContentWriter>::from_ref(writer);
    writer.reject                     = true;
    EXPECT_TRUE((*old_binding)->Update(frame.as_ref(), target.as_mut_ref()).is_err());
    writer.reject = false;
    auto check    = [&](auto& binding, float fallback, float instance) {
        ASSERT_TRUE((*binding)->Update(frame.as_ref(), target.as_mut_ref()).is_ok());
        ASSERT_EQ(writer.bytes.size(), 12u);
        array<float, 3> values {};
        rstd::mem::memcpy(values.data(), writer.bytes.data(), usize(12));
        EXPECT_FLOAT_EQ(values[usize()], fallback);
        EXPECT_FLOAT_EQ(values[usize(1)], instance);
        EXPECT_FLOAT_EQ(values[usize(2)], 2.0f);
    };
    check(old_binding, 0.25f, 1.0f);
    check(same_version, 0.25f, 1.0f);
    check(new_binding, 0.75f, 1.0f);
    check(override_binding, 0.75f, 3.0f);
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
    node->AddMesh(MakeUniformMesh(Arc<owe::SceneShader>::make()));
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

    auto shader = Arc<owe::SceneShader>::make();
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
    owe::vulkan::SceneRenderUniformFrameContext frame_impl(
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
    auto shader        = Arc<owe::SceneShader>::make();
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
    owe::vulkan::SceneRenderUniformFrameContext frame_impl(
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
    owe::vulkan::SceneRenderUniformFrameContext frame_impl(
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
    auto code = owe::ShaderCode::from(array<rstd::uint32_t, 3> { 1u, 2u, 3u }.as_slice());
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
    ASSERT_EQ(stages.len(), rstd::usize(1));
    EXPECT_EQ(stages[rstd::usize()]->entry_point.as_str(), "main"_str);
    EXPECT_EQ(stages[rstd::usize()]->spirv.len(), rstd::usize(3));
    EXPECT_EQ(stages[usize()]->spirv.data(), artifact.stages[usize()].code.data());
    EXPECT_EQ(cloned.stages[usize()].code.data(), artifact.stages[usize()].code.data());
    ASSERT_EQ(reflection.blocks.len(), rstd::usize(1));
    EXPECT_EQ(reflection.blocks[rstd::usize()].set, 4u);
    EXPECT_EQ(reflection.blocks[rstd::usize()].binding, 7u);
    EXPECT_EQ(reflection.blocks[rstd::usize()].member_map.get("g_Time"_str).unwrap()->offset, 16u);
    EXPECT_EQ(reflection.binding_map.get("g_Texture0"_str).unwrap()->layout.binding, 2u);
    EXPECT_EQ(reflection.binding_map.get("g_Texture0"_str).unwrap()->set, 4u);
    EXPECT_EQ(reflection.input_location_map.get("a_Position"_str).unwrap()->format,
              VK_FORMAT_R32G32_SFLOAT);
}

TEST(TextureRequest, BuildsImportedRequestWithoutCacheKey) {
    auto request = owe::vulkan::MakeImportedTextureRequest("textures/main.png"_str);

    EXPECT_EQ(request.kind, owe::vulkan::TextureRequestKind::Imported);
    EXPECT_EQ(rstd::cppstd::as_string_view(request.name.as_str()), "textures/main.png");
    EXPECT_TRUE(request.source.is_none());
    EXPECT_TRUE(request.definition.is_none());
    EXPECT_EQ(request.lifetime, owe::resource::TextureLifetimeClass::Retained);
}

TEST(TextureBindingRequest, CarriesNameAndTypedRequest) {
    auto request = owe::vulkan::MakeImportedTextureRequest("texture-slot"_str);
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

TEST(ResourcePlan, SourceBindingDoesNotChangeStableDefinition) {
    owe::Scene scene;
    scene.RegisterTexture("texture"_Str, owe::SceneTexture {});
    auto                            first    = owe::ExtractRenderSceneSnapshot(scene);
    auto                            next     = owe::ExtractRenderSceneSnapshot(scene);
    auto                            first_id = first.textureDescId("texture"_str).unwrap();
    auto                            next_id  = next.textureDescId("texture"_str).unwrap();
    owe::resource::TexturePlanEntry entry {
        .definition = first.textureDesc(first_id)->desc.resource.clone(),
        .source = Some(owe::resource::TextureDefinitionId { first_id.index, first_id.generation })
    };
    owe::vulkan::SnapshotImportedTextureProvider provider(next, scene.CaptureImageSource());
    EXPECT_TRUE(provider.ResolveTextureContent(entry.source_request()).is_err());
    auto revision = entry.definition.revision();
    entry.source  = Some(owe::resource::TextureDefinitionId { next_id.index, next_id.generation });
    auto identity = provider.ResolveTextureContent(entry.source_request());
    ASSERT_TRUE(identity.is_ok());
    EXPECT_EQ(entry.definition.revision(), revision);
    EXPECT_FALSE(entry.request().source);
    owe::Scene other;
    other.RegisterTexture("texture"_Str, owe::SceneTexture {});
    auto other_snapshot = owe::ExtractRenderSceneSnapshot(other);
    owe::vulkan::SnapshotImportedTextureProvider other_provider(other_snapshot,
                                                                other.CaptureImageSource());
    auto request        = owe::vulkan::MakeImportedTextureRequest("texture"_str);
    auto other_identity = other_provider.ResolveTextureContent(request).unwrap();
    EXPECT_NE(identity->scope, other_identity.scope);
}

TEST(ShaderBackend, CompilesBothLanguagesAndReusesReflection) {
    auto                  backend = MakeShaderBackend();
    ShaderReflectionCache cache(backend.as_ref());
    for (auto language : { SourceLang::Glsl, SourceLang::Hlsl }) {
        ShaderCompUnit unit {
            .stage       = owe::ShaderType::VERTEX,
            .src         = language == SourceLang::Glsl
                               ? "#version 450\nvoid main() { gl_Position = vec4(0, 0, 0, 1); }"_Str
                               : "float4 main() : SV_Position { return float4(0, 0, 0, 1); }"_Str,
            .entry_point = "main"_Str,
            .lang        = language,
        };
        rstd::string::String preprocessed;
        ASSERT_TRUE(backend->Preprocess(unit.src.as_str(), unit.stage, language, preprocessed));
        EXPECT_FALSE(preprocessed.is_empty());
        rstd::vec::Vec<Uni_ShaderSpv> spvs;
        ASSERT_TRUE(backend->CompileAndLinkShaderUnits(
            rstd::slice<ShaderCompUnit>::from_raw_parts(&unit, rstd::usize(1)), {}, spvs));
        ASSERT_EQ(spvs.len(), rstd::usize(1));
        owe::SceneShader shader;
        shader.codes.push(spvs[rstd::usize()]->spirv.clone());
        auto first = cache.Query(shader);
        ASSERT_TRUE(first.is_some());
        auto second = cache.Query(shader);
        ASSERT_TRUE(second.is_some());
        EXPECT_EQ(&**first, &**second);
        SceneShaderArtifactProvider provider(cache, shader);
        auto                        original_request = provider.Request();
        EXPECT_TRUE(provider.LoadShader(original_request).is_ok());
        shader.matrix_abi    = owe::ShaderMatrixAbi::Hlsl;
        auto changed_request = provider.Request();
        EXPECT_NE(changed_request.content_version, original_request.content_version);
        EXPECT_TRUE(provider.LoadShader(original_request).is_err());
        EXPECT_TRUE(provider.LoadShader(changed_request).is_ok());
        cache.Clear();
        EXPECT_TRUE(cache.Query(shader).is_some());
        unit.src = "invalid shader"_Str;
        spvs.clear();
        EXPECT_FALSE(backend->CompileAndLinkShaderUnits(
            rstd::slice<ShaderCompUnit>::from_raw_parts(&unit, rstd::usize(1)), {}, spvs));
    }
}

TEST(ShaderBackend, PreprocessesBoundedSourceAndAllowsAliasedOutput) {
    auto              backend = MakeShaderBackend();
    const std::string prefix  = "#version 450\nvoid main() { gl_Position = vec4(0, 0, 0, 1); }\n";
    const auto        storage = prefix + "#error outside borrowed source\n";
    const auto        source =
        rstd::cppstd::as_str(std::string_view(storage.data(), prefix.size())).unwrap();
    String output;
    ASSERT_TRUE(backend->Preprocess(source, owe::ShaderType::VERTEX, SourceLang::Glsl, output));
    EXPECT_FALSE(output.is_empty());
    const auto expected = output.clone();
    output              = rstd::into(source);
    ASSERT_TRUE(
        backend->Preprocess(output.as_str(), owe::ShaderType::VERTEX, SourceLang::Glsl, output));
    EXPECT_EQ(output.as_str(), expected.as_str());
}

TEST(ShaderBackend, ReusesPreparedReflectionAndRejectsChangedCode) {
    usize          reflections {};
    auto           backend = Box<dyn<ShaderBackend>>::make(CountingShaderBackend { &reflections });
    ShaderCompUnit unit {
        .stage       = owe::ShaderType::VERTEX,
        .src         = "#version 450\nvoid main() { gl_Position = vec4(0, 0, 0, 1); }"_Str,
        .entry_point = "main"_Str,
    };
    Vec<Uni_ShaderSpv> compiled;
    ASSERT_TRUE(backend->CompileAndLinkShaderUnits(
        slice<ShaderCompUnit>::from_raw_parts(&unit, usize(1)), {}, compiled));
    owe::SceneShader shader;
    shader.codes.push(compiled[usize()]->spirv.clone());
    ShaderReflected reflected;
    ASSERT_TRUE(backend->GenReflect(shader.codes.as_slice(), compiled, reflected));
    EXPECT_EQ(reflections, usize(1));
    auto stages = Vec<owe::SceneShaderStage>::with_capacity(compiled.len());
    for (auto& stage : compiled) {
        stages.push(owe::SceneShaderStage {
            .entry_point = rstd::move(stage->entry_point),
            .stage       = stage->stage,
            .spirv       = rstd::move(stage->spirv),
        });
    }
    auto prepared =
        Arc<owe::SceneShaderReflection>::make(rstd::move(stages), rstd::move(reflected));
    shader.reflection = Some(prepared.clone());
    ShaderReflectionCache cache(backend.as_ref());
    auto                  first = cache.Query(shader);
    ASSERT_TRUE(first.is_some());
    EXPECT_EQ(&**first, &*prepared);
    ShaderReflectionCache graph_cache(backend.as_ref());
    EXPECT_EQ(&**graph_cache.Query(shader), &*prepared);
    SceneShaderArtifactProvider provider(graph_cache, shader);
    auto                        loaded = provider.LoadShader(provider.Request());
    ASSERT_TRUE(loaded.is_ok());
    auto artifact = rstd::move(loaded).unwrap();
    EXPECT_EQ(artifact.stages[usize()].code.data(), shader.codes[usize()].data());
    EXPECT_EQ(shader.codes[usize()].data(), prepared->Stages()[usize()].spirv.data());
    graph_cache.Clear();
    EXPECT_EQ(reflections, usize(1));
    cache.Clear();
    auto reloaded = cache.Query(shader);
    ASSERT_TRUE(reloaded.is_some());
    EXPECT_EQ(&**reloaded, &*prepared);
    EXPECT_EQ(reflections, usize(1));
    auto original          = shader.codes.clone();
    auto changed_words     = Vec<rstd::uint32_t>::from(shader.codes[usize()].as_slice());
    changed_words[usize()] = 0;
    shader.codes[usize()]  = owe::ShaderCode::from(rstd::move(changed_words));
    EXPECT_TRUE(cache.Query(shader).is_none());
    EXPECT_EQ(reflections, usize(2));
    shader.codes = rstd::move(original);
    EXPECT_EQ(&**cache.Query(shader), &*prepared);
    EXPECT_EQ(reflections, usize(2));
    shader.reflection = None();
    shader.codes.clear();
    cache.Clear();
    EXPECT_FALSE(artifact.stages.is_empty());
    EXPECT_EQ(artifact.stages[usize()].code, prepared->Stages()[usize()].spirv);
}

TEST(ShaderBackend, ReflectionFailureDoesNotPublishPartialOutput) {
    ShaderCompUnit unit {
        .stage       = owe::ShaderType::VERTEX,
        .src         = "#version 450\nvoid main() { gl_Position = vec4(0, 0, 0, 1); }"_Str,
        .entry_point = "main"_Str,
    };
    Vec<Uni_ShaderSpv> compiled;
    ASSERT_TRUE(CompileAndLinkShaderUnits(
        slice<ShaderCompUnit>::from_raw_parts(&unit, usize(1)), {}, compiled));
    Vec<owe::ShaderCode> invalid;
    invalid.push(compiled[usize()]->spirv.clone());
    invalid.push(owe::ShaderCode {});
    Vec<Uni_ShaderSpv> stages;
    auto               sentinel = Box<ShaderSpv>::make();
    sentinel->entry_point       = "unchanged"_Str;
    stages.push(rstd::move(sentinel));
    ShaderReflected reflection;
    reflection.blocks.push(ShaderReflected::Block { .name = "unchanged"_Str });
    EXPECT_FALSE(GenReflect(invalid.as_slice(), stages, reflection));
    ASSERT_EQ(stages.len(), usize(1));
    EXPECT_EQ(stages[usize()]->entry_point.as_str(), "unchanged"_str);
    ASSERT_EQ(reflection.blocks.len(), usize(1));
    EXPECT_EQ(reflection.blocks[usize()].name.as_str(), "unchanged"_str);
}

TEST(ShaderBackend, RejectsUnsupportedDescriptorArrays) {
    ShaderCompUnit unit {
        .stage       = owe::ShaderType::FRAGMENT,
        .src         = "#version 450\nlayout(set=0,binding=0) uniform sampler2D images[2];\n"
                       "layout(location=0) out vec4 color;\n"
                       "void main() { color = texture(images[0], vec2(0)); }"_Str,
        .entry_point = "main"_Str,
    };
    Vec<Uni_ShaderSpv> compiled;
    ASSERT_TRUE(CompileAndLinkShaderUnits(
        slice<ShaderCompUnit>::from_raw_parts(&unit, usize(1)), {}, compiled));
    Vec<owe::ShaderCode> codes;
    codes.push(compiled[usize()]->spirv.clone());
    ShaderReflected reflected;
    EXPECT_FALSE(GenReflect(codes.as_slice(), compiled, reflected));
    EXPECT_TRUE(reflected.binding_map.is_empty());
}

TEST(ShaderBackend, CompilesOwnedSourcesWithDefaultAndExplicitEntryPoints) {
    auto backend = MakeShaderBackend();
    for (bool explicit_entry : { false, true }) {
        auto name = explicit_entry ? "custom_vertex"_Str : "main_vs"_Str;
        auto source =
            rstd::format("float4 {}() : SV_Position {{ return float4(0, 0, 0, 1); }}", name);
        rstd::array<ShaderCompUnit, 1> units {
            ShaderCompUnit {
                .stage       = owe::ShaderType::VERTEX,
                .src         = rstd::move(source),
                .entry_point = explicit_entry ? name.clone() : String {},
                .lang        = SourceLang::Hlsl,
            },
        };
        rstd::vec::Vec<Uni_ShaderSpv> compiled;
        ASSERT_TRUE(backend->CompileAndLinkShaderUnits(units.as_slice(), {}, compiled));
        ASSERT_EQ(compiled.len(), rstd::usize(1));
        EXPECT_EQ(compiled[rstd::usize()]->entry_point.as_str(), name.as_str());
        EXPECT_FALSE(compiled[rstd::usize()]->spirv.is_empty());
    }
}

TEST(ShaderBackend, OwnsOrderedReflectionAcrossCacheAndArtifact) {
    auto backend = MakeShaderBackend();
    for (int fragment_count : { 2, 3 }) {
        owe::SceneShader shader;
        for (auto stage : { owe::ShaderType::VERTEX, owe::ShaderType::FRAGMENT }) {
            const bool     vertex = stage == owe::ShaderType::VERTEX;
            ShaderCompUnit unit {
                .stage = stage,
                .src   = rstd::format(
                    "#version 450\nlayout(set=1,binding=3,std140) uniform Shared {{ "
                    "vec4 z_Last[{}]; vec4 a_First; }};\n{}",
                    vertex ? 2 : fragment_count,
                    vertex ? "layout(location=0) in vec4 position;\n"
                             "void main() { gl_Position = position + z_Last[0] + a_First; }"_str
                           : "layout(location=0) out vec4 color;\n"
                             "void main() { color = z_Last[1] + a_First; }"_str),
                .entry_point = "main"_Str,
            };
            rstd::vec::Vec<Uni_ShaderSpv> compiled;
            ASSERT_TRUE(backend->CompileAndLinkShaderUnits(
                rstd::slice<ShaderCompUnit>::from_raw_parts(&unit, rstd::usize(1)), {}, compiled));
            ASSERT_EQ(compiled.len(), usize(1));
            shader.codes.push(rstd::move(compiled[rstd::usize()]->spirv));
        }
        ShaderReflectionCache cache(backend.as_ref());
        auto                  cached = cache.Query(shader);
        if (fragment_count != 2) {
            EXPECT_TRUE(cached.is_none());
            continue;
        }
        ASSERT_TRUE(cached.is_some());
        const auto& reflected = (**cached).Reflected();
        ASSERT_EQ(reflected.blocks.len(), rstd::usize(1));
        const auto& block = reflected.blocks[rstd::usize()];
        EXPECT_EQ(block.name.as_str(), "Shared"_str);
        ASSERT_EQ(block.member_map.len(), rstd::usize(2));
        auto names = block.member_map.keys();
        EXPECT_EQ(names.next().unwrap()->as_str(), "a_First"_str);
        EXPECT_EQ(names.next().unwrap()->as_str(), "z_Last"_str);
        auto binding = reflected.binding_map.get("Shared"_str);
        ASSERT_TRUE(binding.is_some());
        EXPECT_EQ((**binding).layout.stageFlags,
                  VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

        SceneShaderArtifactProvider provider(cache, shader);
        auto                        loaded = provider.LoadShader(provider.Request());
        ASSERT_TRUE(loaded.is_ok());
        auto artifact = rstd::move(loaded).unwrap();
        cache.Clear();
        shader.codes.clear();
        auto restored = ShaderReflectionFromArtifact(artifact);
        artifact      = {};
        ASSERT_EQ(restored.blocks.len(), rstd::usize(1));
        const auto& restored_block = restored.blocks[rstd::usize()];
        EXPECT_EQ(restored_block.name.as_str(), "Shared"_str);
        auto array = restored_block.member_map.get("z_Last"_str);
        ASSERT_TRUE(array.is_some());
        EXPECT_EQ((**array).array_stride, 16u);
        ASSERT_EQ((**array).array_dimensions.len(), rstd::usize(1));
        EXPECT_EQ((**array).array_dimensions[rstd::usize()], rstd::u32(2));
        EXPECT_EQ(restored_block.member_map.get("a_First"_str).unwrap()->offset, 32u);
        EXPECT_TRUE(restored.input_location_map.contains_key("position"_str));
        EXPECT_EQ(restored.binding_map.get("Shared"_str).unwrap()->layout.stageFlags,
                  VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    }
}

TEST(TextureLoader, DeclarationUsesMetadataWithoutLoadingContent) {
    struct MetadataParser {
        Arc<rstd::sync::atomic::Atomic<u32>> parses;
        auto                                 Retention(ref<str>) const -> owe::ImageRetention {
            return owe::ImageRetention::Parser;
        }
        auto Describe(ref<str>) const -> Result<owe::ImageDescription, owe::ImageParseError> {
            owe::ImageDescription result;
            result.header.format = owe::TextureFormat::BC3;
            result.slots.push({ 64, 32, usize(3) });
            result.slots.push({ 32, 16, usize(3) });
            return Ok(rstd::move(result));
        }
        auto ParseHeader(ref<str>) const -> Result<owe::ImageHeader, owe::ImageParseError> {
            return Ok(owe::ImageHeader {});
        }
        auto Parse(ref<str>) const -> Result<Arc<owe::Image>, owe::ImageParseError> {
            parses->fetch_add(u32(1));
            return Err(owe::ImageParseError { .kind = owe::ImageParseErrorKind::DecodeFailed });
        }
        auto ParseMany(slice<String> names) const
            -> Vec<Result<Arc<owe::Image>, owe::ImageParseError>> {
            Vec<Result<Arc<owe::Image>, owe::ImageParseError>> result;
            for (const auto& name : names) result.push(Parse(name.as_str()));
            return result;
        }
    };
    auto       parses = Arc<rstd::sync::atomic::Atomic<u32>>::make(u32());
    owe::Scene scene;
    scene.SetImageParser(Arc<dyn<owe::IImageParser>>::make(MetadataParser { parses.clone() }));
    auto                            snapshot = owe::ExtractRenderSceneSnapshot(scene);
    SnapshotImportedTextureProvider provider(snapshot, scene.CaptureImageSource());
    auto declared = provider.Declare(MakeImportedTextureRequest("metadata"_str));
    ASSERT_TRUE(declared.is_ok());
    EXPECT_EQ(declared->extent.width, 64u);
    EXPECT_EQ(declared->extent.height, 32u);
    EXPECT_EQ(declared->mip_levels, 3u);
    EXPECT_EQ(declared->format, owe::TextureFormat::BC3);
    EXPECT_EQ(parses->load(), u32());
    auto loader = provider.OpenTextureLoader().unwrap();
    EXPECT_TRUE(loader->LoadTexture("metadata"_str).is_err());
    EXPECT_EQ(parses->load(), u32(1));
}

namespace
{
struct AtlasMipParser {
    usize first_mips;
    usize second_mips;
    auto  Retention(ref<str>) const -> owe::ImageRetention { return owe::ImageRetention::Parser; }
    auto  Describe(ref<str>) const -> Result<owe::ImageDescription, owe::ImageParseError> {
        owe::ImageDescription result;
        result.header.format = owe::TextureFormat::RGBA8;
        result.slots.push({ 64, 32, first_mips });
        result.slots.push({ 128, 64, second_mips });
        return Ok(rstd::move(result));
    }
    auto ParseHeader(ref<str>) const -> Result<owe::ImageHeader, owe::ImageParseError> {
        return Ok(owe::ImageHeader {});
    }
    auto Parse(ref<str>) const -> Result<Arc<owe::Image>, owe::ImageParseError> {
        return Err(owe::ImageParseError { .kind = owe::ImageParseErrorKind::DecodeFailed });
    }
    auto ParseMany(slice<String> names) const
        -> Vec<Result<Arc<owe::Image>, owe::ImageParseError>> {
        Vec<Result<Arc<owe::Image>, owe::ImageParseError>> result;
        for (const auto& name : names) result.push(Parse(name.as_str()));
        return result;
    }
};
} // namespace

TEST(TextureLoader, DeclarationAcceptsAtlasSlotsWithDifferentMipCounts) {
    // Each atlas slot is uploaded as its own texture, so only the shortest mip chain is
    // guaranteed for every frame.
    auto check = [](usize first, usize second) {
        owe::Scene scene;
        scene.SetImageParser(Arc<dyn<owe::IImageParser>>::make(AtlasMipParser { first, second }));
        auto                            snapshot = owe::ExtractRenderSceneSnapshot(scene);
        SnapshotImportedTextureProvider provider(snapshot, scene.CaptureImageSource());
        auto declared = provider.Declare(MakeImportedTextureRequest("atlas"_str));
        ASSERT_TRUE(declared.is_ok());
        EXPECT_EQ(declared->mip_levels, 4u);
        EXPECT_EQ(declared->layers, 1u);
    };
    check(usize(4), usize(5));
    check(usize(5), usize(4));
}

TEST(TextureLoader, DeclarationAndMissingPlaceholderHaveMatchingDescriptions) {
    owe::Scene                      scene;
    auto                            snapshot = owe::ExtractRenderSceneSnapshot(scene);
    SnapshotImportedTextureProvider provider(snapshot, scene.CaptureImageSource());
    auto declared     = provider.Declare(MakeImportedTextureRequest("missing"_str)).unwrap();
    auto loaded       = provider.OpenTextureLoader().unwrap()->LoadTexture("missing"_str).unwrap();
    auto descriptions = vrento::texture::describe_image(*loaded).unwrap();
    EXPECT_EQ(declared, descriptions[usize()]);
}

TEST(TextureLoader, RetainsCapturedRuntimeContentAfterSceneReplacement) {
    auto original          = sync::Arc<owe::Image>::make();
    original->content->key = "original"_Str;
    vrento::Image::Slot slot;
    slot.width  = 32;
    slot.height = 16;
    slot.mipmaps.push(owe::ImageData { .width = 32, .height = 16 });
    original->content->slots.push(rstd::move(slot));
    Option<sync::Arc<dyn<owe::resource::TextureLoader>>> loader;
    {
        owe::Scene scene;
        scene.RegisterRuntimeImage(String::make("runtime"_str), original.clone());
        auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
        owe::vulkan::SnapshotImportedTextureProvider provider(snapshot, scene.CaptureImageSource());
        auto shape = provider.Declare(MakeImportedTextureRequest("runtime"_str)).unwrap();
        EXPECT_EQ(shape.extent.width, 32u);
        EXPECT_EQ(shape.extent.height, 16u);
        auto opened = provider.OpenTextureLoader();
        ASSERT_TRUE(opened.is_ok());
        loader = Some(rstd::move(opened).unwrap_unchecked());
        scene.RegisterRuntimeImage(String::make("runtime"_str), sync::Arc<owe::Image>::make());
    }
    auto loaded = (*loader)->LoadTexture("runtime"_str);
    ASSERT_TRUE(loaded.is_ok());
    auto image = rstd::move(loaded).unwrap_unchecked();
    EXPECT_EQ(&*image, &*original->content);
    EXPECT_EQ(image->key, "original"_str);
}

TEST(SceneDraw, TextureReadsFollowSamplerReflection) {
    namespace vk                     = vrento::vulkan;
    namespace fg                     = vrento::fg;
    auto                     backend = MakeShaderBackend();
    array<ShaderCompUnit, 3> units {
        ShaderCompUnit { .stage       = owe::ShaderType::VERTEX,
                         .src         = R"(#version 450
                layout(set=1, binding=5) uniform sampler2D vertexTexture;
                layout(set=1, binding=9) uniform sampler2D sharedTexture;
                void main() {
                    gl_Position = textureLod(vertexTexture, vec2(0), 0) +
                                  textureLod(sharedTexture, vec2(0), 0);
                })"_Str,
                         .entry_point = "main"_Str },
        ShaderCompUnit { .stage       = owe::ShaderType::GEOMETRY,
                         .src         = R"(#version 450
                layout(points) in;
                layout(points, max_vertices=1) out;
                layout(set=2, binding=4) uniform sampler2D geometryTexture;
                void main() {
                    gl_Position = gl_in[0].gl_Position +
                                  textureLod(geometryTexture, vec2(0), 0);
                    EmitVertex();
                    EndPrimitive();
                })"_Str,
                         .entry_point = "main"_Str },
        ShaderCompUnit { .stage       = owe::ShaderType::FRAGMENT,
                         .src         = R"(#version 450
                layout(set=1, binding=9) uniform sampler2D sharedTexture;
                layout(location=0) out vec4 color;
                void main() { color = texture(sharedTexture, vec2(0)); })"_Str,
                         .entry_point = "main"_Str },
    };
    Vec<Uni_ShaderSpv> compiled;
    ASSERT_TRUE(backend->CompileAndLinkShaderUnits(units.as_slice(), {}, compiled));
    auto shader = Arc<owe::SceneShader>::make();
    for (auto& stage : compiled) shader->codes.push(rstd::move(stage->spirv));
    shader->sampler_bindings.push({ 1, "vertexTexture"_Str });
    shader->sampler_bindings.push({ 2, "geometryTexture"_Str });
    shader->sampler_bindings.push({ 3, "sharedTexture"_Str });
    auto node = Arc<owe::SceneNode>::make();
    node->AddMesh(MakeUniformMesh(shader.clone()));

    fg::FrameGraphBuilder builder(7);
    SceneRenderTextures   textures(u64(7));
    SceneDraw::Desc       desc;
    for (unsigned index {}; index < 5; ++index) {
        auto name = rstd::format("texture-{}", index);
        auto source =
            textures
                .declare(builder,
                         {
                             .key        = name.clone(),
                             .imported   = true,
                             .definition = MakeRenderTargetTextureRequest(
                                 name.as_str(),
                                 owe::SceneRenderTarget { .width = i32(64), .height = i32(64) }),
                         })
                .unwrap();
        desc.texture_bindings.push(
            { .name = rstd::move(name), .use = Some(textures.handle(source).unwrap()) });
    }
    auto pass = builder.add_pass({ 1 }).unwrap();
    auto initial =
        textures
            .declare(builder,
                     {
                         .key        = "output"_Str,
                         .definition = MakeRenderTargetTextureRequest(
                             "output"_str,
                             owe::SceneRenderTarget { .width = i32(64), .height = i32(64) }),
                     })
            .unwrap();
    auto output     = builder.reserve_write(pass, initial).unwrap();
    desc.output_use = Some(textures.handle(output).unwrap());
    SceneDraw draw(rstd::move(desc),
                   { .node = Some(mut_ref<owe::SceneNode>::from_raw_parts(node.as_ptr())) });
    owe::resource::ResourcePlan plan { .generation = u64(7) };
    ShaderReflectionCache       cache(backend.as_ref());
    ResourceDeclarationContext  declarations(plan, cache);
    ASSERT_TRUE(draw.declareResources(declarations));
    auto resources = draw.describePass(builder, textures, pass).unwrap();
    ASSERT_EQ(resources.textures.len(), usize(3));
    const array<vk::PassStages, 3> stages {
        vk::PassStages::Vertex,
        vk::PassStages::Geometry,
        vk::PassStages::Vertex | vk::PassStages::Fragment,
    };
    for (usize index {}; index < stages.len(); ++index) {
        EXPECT_EQ(resources.textures[index].stages, stages[index]);
        auto version = resources.textures[index].content.resolve(7).unwrap();
        EXPECT_EQ(textures.handle(version).unwrap(),
                  *draw.parameters().texture_bindings[index + usize(1)].use);
    }
    auto accesses = draw.textureAccesses();
    ASSERT_EQ(accesses.len(), usize(4));
    for (usize index {}; index < usize(3); ++index)
        EXPECT_EQ(accesses[index].use, *draw.parameters().texture_bindings[index + usize(1)].use);
    EXPECT_EQ(draw.parameters().texture_bindings.len(), usize(5));
    EXPECT_EQ(draw.parameters().vk_tex_binding[usize()], -1);
    EXPECT_EQ(draw.parameters().vk_tex_binding[usize(4)], -1);
    EXPECT_EQ(draw.resourceUses().textures.len(), usize(6));
}

TEST(SceneDraw, StaticRecipeSurvivesSceneAndDeclarationDestruction) {
    Option<vrento::vulkan::PassInput>        description;
    Option<vrento::vulkan::GraphicsDrawDesc> override_description;
    {
        auto           backend = MakeShaderBackend();
        ShaderCompUnit unit {
            .stage       = owe::ShaderType::VERTEX,
            .src         = "#version 450\nvoid main() { gl_Position = vec4(0, 0, 0, 1); }"_Str,
            .entry_point = "main"_Str
        };
        Vec<Uni_ShaderSpv> compiled;
        ASSERT_TRUE(backend->CompileAndLinkShaderUnits(
            slice<ShaderCompUnit>::from_raw_parts(&unit, usize(1)), {}, compiled));
        auto shader = Arc<owe::SceneShader>::make();
        shader->codes.push(compiled[usize()]->spirv.clone());
        auto node = Arc<owe::SceneNode>::make();
        node->AddMesh(MakeUniformMesh(shader.clone()));
        auto& material = *node->Mesh()->MaterialSlots()[usize()];
        material.SetBlendMode(owe::BlendMode::Translucent);
        SceneDraw draw(SceneDraw::Desc {},
                       { .node = Some(mut_ref<owe::SceneNode>::from_raw_parts(node.as_ptr())) });
        auto      override_material = Arc<owe::SceneMaterial>::make(material);
        override_material->SetBlendMode(owe::BlendMode::Additive);
        SceneDraw override_draw(
            SceneDraw::Desc { .graph_pass_index = usize(1) },
            { .node              = Some(mut_ref<owe::SceneNode>::from_raw_parts(node.as_ptr())),
              .material_override = Some(override_material.clone()) });
        material.SetBlendMode(owe::BlendMode::Disable);
        material.customShader.shader = None();
        override_material->SetBlendMode(owe::BlendMode::Disable);
        override_material->customShader.shader = None();
        owe::resource::ResourcePlan plan { .generation = u64(7) };
        ShaderReflectionCache       cache(backend.as_ref());
        ResourceDeclarationContext  declarations(plan, cache);
        auto                        recipe = draw.declareResources(declarations);
        ASSERT_TRUE(recipe);
        override_description = override_draw.declareResources(declarations);
        ASSERT_TRUE(override_description);
        description =
            Some(make_test_pass(7,
                                1,
                                vrento::vulkan::PassExecutionInfo {
                                    .id = { 1 }, .kind = vrento::vulkan::graphics_draw_kind },
                                rstd::move(*recipe)));
    }
    auto recipe = description->get<vrento::vulkan::GraphicsDrawDesc>();
    ASSERT_TRUE(recipe);
    ASSERT_TRUE((**recipe).layout);
    EXPECT_EQ((**recipe).request.shader_stages.len(), usize(1));
    EXPECT_TRUE((**recipe).request.color_blend.blendEnable);
    EXPECT_TRUE(override_description->request.color_blend.blendEnable);
    EXPECT_EQ(override_description->request.color_blend.dstColorBlendFactor, VK_BLEND_FACTOR_ONE);
    Vec<PipelineLayoutRequirement> requirements;
    requirements.push((**recipe).layout->clone());
    EXPECT_TRUE(PlanPipelineLayouts(requirements.as_slice(), false).is_ok());
}

TEST(SceneRenderState, RetainsDeclaredInputsAndPublishesOutputAssociation) {
    namespace fg = vrento::fg;
    fg::FrameGraphBuilder graph(7);
    graph.add_pass({ 1 }).unwrap();
    owe::resource::ResourcePlan plan { .generation = u64(7) };
    auto                        source = plan.DeclareTexture(
        MakeRenderTargetTextureRequest(
            "output"_str, owe::SceneRenderTarget { .width = i32(64), .height = i32(32) }),
        owe::resource::ResourceAccess::Read);
    SceneRenderStateInput input { .graph         = rstd::move(graph).finish().unwrap(),
                                  .textures      = SceneRenderTextures(u64(7)),
                                  .output_pass   = { 1 },
                                  .result_use    = Some(source),
                                  .resource_plan = rstd::move(plan) };
    input.descriptions.push(make_test_pass(
        7, 9, vrento::vulkan::PassExecutionInfo { .id = { 1 }, .kind = 71 }, TestPassDesc {}));
    SceneRenderState state;
    state.install(rstd::move(input)).unwrap();
    EXPECT_EQ(state.resourcePlan().textures.len(), usize(1));
    EXPECT_EQ(state.inputDescriptions()[usize()].revision(), 9u);
    auto                  backend = MakeShaderBackend();
    ShaderReflectionCache cache(backend.as_ref());
    auto                  output = state.createOutput(VK_FORMAT_R8G8B8A8_UNORM, cache);
    EXPECT_EQ(output.source().unwrap(), source);
    EXPECT_TRUE(output.pipeline());
    EXPECT_TRUE(output.render_pass());
    EXPECT_EQ(state.resourcePlan().textures.len(), usize(1));
    EXPECT_EQ(state.inputDescriptions()[usize()].execution().kind, 71u);
    auto moved = rstd::move(state);
    EXPECT_EQ(moved.inputGraph().scope(), 7u);
    moved.clear();
    EXPECT_FALSE(moved.installed());
    EXPECT_FALSE(moved.valid());
    fg::FrameGraphBuilder other(8);
    SceneRenderStateInput mismatch { .graph         = rstd::move(other).finish().unwrap(),
                                     .textures      = SceneRenderTextures(u64(7)),
                                     .resource_plan = { .generation = u64(7) } };
    EXPECT_EQ(moved.install(rstd::move(mismatch)).unwrap_err().kind, fg::ErrorKind::Conflict);
    EXPECT_FALSE(moved.installed());
}

TEST(ResourceDeclarationContext, ScopesLocalResourceNamesToThePlanGeneration) {
    owe::resource::ResourcePlan             plan { .generation = rstd::u64(7) };
    auto                                    shader_backend = owe::vulkan::MakeShaderBackend();
    owe::vulkan::ShaderReflectionCache      shader_cache(shader_backend.as_ref());
    owe::vulkan::ResourceDeclarationContext declarations(plan, shader_cache);

    auto name = declarations.ScopeResourceName(String::make("pass:25:0:vertex:0"_str));

    EXPECT_EQ(rstd::cppstd::as_string_view(name.as_str()), "plan:7:pass:25:0:vertex:0");
}

TEST(ResourceDeclarationContext, RetainsSharedContentByDefinitionVersion) {
    using namespace owe::vulkan;
    owe::resource::ResourcePlan plan { .generation = u64(2) };
    auto                        backend = MakeShaderBackend();
    ShaderReflectionCache       cache(backend.as_ref());
    ResourceDeclarationContext  declarations(plan, cache);
    Vec<u8>                     input;
    input.resize(usize(4), u8(3));
    auto pointer = input.as_slice().as_raw_ptr();
    auto content = owe::resource::BufferContent::from(rstd::move(input));
    declarations.AddBuffer(
        owe::resource::BufferRequest {
            .name             = "shared-bytes"_Str,
            .definition       = { .size = usize(4096) },
            .content_version  = u64(4),
            .content_identity = u64(8),
        },
        content.clone());
    auto definition                     = plan.buffers[usize()].definition.clone();
    content                             = {};
    auto                       retained = declarations.takeData();
    ResourceDeclarationContext restored(plan, cache, rstd::move(retained));
    auto                       loaded = restored.LoadBuffer(definition).unwrap();
    EXPECT_EQ(loaded.as_slice().as_raw_ptr(), pointer);
    EXPECT_EQ(loaded.len(), usize(4));
    auto request = definition.definition().clone();
    request.content_version += u64(1);
    EXPECT_TRUE(restored.LoadBuffer(definition.with_definition(rstd::move(request))).is_err());
    auto loaded_again = restored.LoadBuffer(definition).unwrap();
    EXPECT_EQ(loaded_again.as_slice()[usize()], u8(3));
}

TEST(SceneDraw, RefreshesImportedTextureOnStableUse) {
    owe::Scene scene;
    scene.RootMut()->ID() = rstd::i32(1);
    scene.RegisterTexture(String::make("texture-old"_str),
                          owe::SceneTexture { .url = "texture-old"_Str });
    scene.RegisterTexture(String::make("texture-new"_str),
                          owe::SceneTexture { .url = "texture-new"_Str });

    auto node  = Arc<owe::SceneNode>::make();
    node->ID() = rstd::i32(2);
    auto mesh  = MakeUniformMesh(Arc<owe::SceneShader>::make());
    mesh->MaterialSlots()[usize()]->textures.push("texture-old"_Str);
    node->AddMesh(mesh.clone());
    scene.RootMut()->AppendChild(node.clone());

    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
    auto old_desc = snapshot.textureDescId("texture-old"_str);
    auto new_desc = snapshot.textureDescId("texture-new"_str);
    ASSERT_TRUE(old_desc.is_some());
    ASSERT_TRUE(new_desc.is_some());
    auto use =
        owe::resource::TextureUseHandle { .index = rstd::u64(4), .generation = rstd::u64(9) };
    Vec<owe::vulkan::TextureBindingRequest> bindings;
    bindings.push(owe::vulkan::TextureBindingRequest {
        .name    = String::make("texture-old"_str),
        .use     = rstd::Some(use),
        .request = rstd::Some(owe::vulkan::MakeImportedTextureRequest("texture-old"_str, old_desc)),
    });
    owe::vulkan::SceneDraw pass(
        owe::vulkan::SceneDraw::Desc {
            .texture_bindings = std::move(bindings),
        },
        { .node = rstd::Some(rstd::mut_ref<owe::SceneNode>::from_raw_parts(node.as_ptr())) });

    ASSERT_TRUE(
        scene
            .SetMaterialTextureSlot(*mesh->MaterialSlots()[usize()], rstd::u32(), "texture-new"_str)
            .changed);
    auto refresh = pass.refreshMaterialTextureBindings(snapshot);
    EXPECT_NE(refresh.invalidation_flags, owe::vulkan::PassInvalidationNone);
    EXPECT_FALSE(refresh.requires_graph_rebuild);

    auto diagnostics = pass.textureRequestDiagnostics();
    ASSERT_EQ(diagnostics.len().to_primitive(), 1u);
    ASSERT_TRUE(diagnostics[rstd::usize(0)].use.is_some());
    EXPECT_EQ(*diagnostics[rstd::usize(0)].use, use);
    ASSERT_TRUE(diagnostics[rstd::usize(0)].request.is_some());
    EXPECT_EQ(diagnostics[rstd::usize(0)].request->name, "texture-new"_str);
    ASSERT_TRUE(diagnostics[rstd::usize(0)].request->source.is_some());
    EXPECT_EQ(diagnostics[rstd::usize(0)].request->source->index, new_desc->index);
    EXPECT_EQ(diagnostics[rstd::usize(0)].request->source->generation, new_desc->generation);
}

TEST(SceneDraw, RebuildsForImportedTextureMissingFromSnapshot) {
    owe::Scene scene;
    scene.RootMut()->ID() = rstd::i32(1);
    scene.RegisterTexture(String::make("texture-old"_str),
                          owe::SceneTexture { .url = "texture-old"_Str });

    auto node  = Arc<owe::SceneNode>::make();
    node->ID() = rstd::i32(2);
    auto mesh  = MakeUniformMesh(Arc<owe::SceneShader>::make());
    mesh->MaterialSlots()[usize()]->textures.push("texture-old"_Str);
    node->AddMesh(mesh.clone());
    scene.RootMut()->AppendChild(node.clone());

    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
    auto old_desc = snapshot.textureDescId("texture-old"_str);
    ASSERT_TRUE(old_desc.is_some());
    auto use =
        owe::resource::TextureUseHandle { .index = rstd::u64(4), .generation = rstd::u64(9) };
    Vec<owe::vulkan::TextureBindingRequest> bindings;
    bindings.push(owe::vulkan::TextureBindingRequest {
        .name    = String::make("texture-old"_str),
        .use     = rstd::Some(use),
        .request = rstd::Some(owe::vulkan::MakeImportedTextureRequest("texture-old"_str, old_desc)),
    });
    owe::vulkan::SceneDraw pass(
        owe::vulkan::SceneDraw::Desc {
            .texture_bindings = std::move(bindings),
        },
        { .node = rstd::Some(rstd::mut_ref<owe::SceneNode>::from_raw_parts(node.as_ptr())) });

    scene.RegisterTexture(String::make("texture-new"_str),
                          owe::SceneTexture { .url = "texture-new"_Str });
    ASSERT_TRUE(
        scene
            .SetMaterialTextureSlot(*mesh->MaterialSlots()[usize()], rstd::u32(), "texture-new"_str)
            .changed);

    auto refresh = pass.refreshMaterialTextureBindings(snapshot);

    EXPECT_EQ(refresh.invalidation_flags, owe::vulkan::PassInvalidationNone);
    EXPECT_TRUE(refresh.requires_graph_rebuild);
    auto diagnostics = pass.textureRequestDiagnostics();
    ASSERT_EQ(diagnostics.len().to_primitive(), 1u);
    EXPECT_EQ(diagnostics[rstd::usize(0)].name, "texture-old"_str);
}

TEST(PreparedPassResources, ResolvesOnlyDeclaredUses) {
    owe::resource_registry::PreparedResourceTable table(rstd::u64(6));
    auto                                          allowed =
        owe::resource::TextureUseHandle { .index = rstd::u64(1), .generation = rstd::u64(6) };
    auto hidden =
        owe::resource::TextureUseHandle { .index = rstd::u64(2), .generation = rstd::u64(6) };
    auto insert = [&](owe::resource::TextureUseHandle use, std::string_view name) {
        auto allocation = rstd::sync::Arc<owe::resource_registry::TextureAllocation>::make(
            Vec<vrento::resource_registry::TextureSlot>::make(), owe::vulkan::ImageSlotsRef {});
        return table.Insert(owe::resource_registry::PreparedTexture {
            .use = use,
            .resource =
                owe::resource::TextureHandle { .index = use.index, .generation = rstd::u64(1) },
            .definition =
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
                          owe::SceneTexture { .url = "textures/main.png"_Str });

    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
    auto desc_id  = snapshot.textureDescId("texture-slot"_str);
    ASSERT_TRUE(desc_id.is_some());

    auto request = owe::vulkan::MakeImportedTextureRequest("texture-slot"_str, desc_id);
    EXPECT_EQ(request.source->index, desc_id->index);

    auto resolved = owe::vulkan::ResolveImportedTextureName(snapshot, request);
    ASSERT_TRUE(resolved.is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view(resolved->as_str()), "textures/main.png");

    auto lookup_request = owe::vulkan::MakeImportedTextureRequest("texture-slot"_str);
    resolved            = owe::vulkan::ResolveImportedTextureName(snapshot, lookup_request);
    ASSERT_TRUE(resolved.is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view(resolved->as_str()), "textures/main.png");

    auto missing_request = owe::vulkan::MakeImportedTextureRequest("missing"_str);
    EXPECT_TRUE(owe::vulkan::ResolveImportedTextureName(snapshot, missing_request).is_none());
}

TEST(TextureRequest, BuildsRenderTargetCacheKey) {
    owe::SceneRenderTarget rt {
        .width        = i32(256),
        .height       = i32(128),
        .allowReuse   = false,
        .mipmap_level = 3,
    };

    auto request = owe::vulkan::MakeRenderTargetTextureRequest("_rt_default"_str, rt);

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
    EXPECT_EQ(owe::vulkan::MakeRenderTargetTextureRequest("_rt_default"_str, rt).lifetime,
              owe::resource::TextureLifetimeClass::FrameLocal);

    auto no_mip = owe::vulkan::MakeRenderTargetNoMipTextureRequest("_rt_default"_str, rt);
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

    auto a = owe::vulkan::MakeRenderTargetTextureRequest("_rt_default"_str, rt);
    auto b = owe::vulkan::MakeRenderTargetTextureRequest("_rt_default"_str, rt);

    EXPECT_TRUE(owe::vulkan::SameTextureRequest(a, b));

    rt.width     = i32(512);
    auto resized = owe::vulkan::MakeRenderTargetTextureRequest("_rt_default"_str, rt);
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
        owe::vulkan::MakeMsaaTextureRequest("_rt_default::msaa4"_str, rt, VK_SAMPLE_COUNT_4_BIT);
    EXPECT_EQ(msaa.kind, owe::vulkan::TextureRequestKind::RenderTargetMsaa);
    EXPECT_EQ(rstd::cppstd::as_string_view(msaa.name.as_str()), "_rt_default::msaa4");
    ASSERT_TRUE(msaa.definition.is_some());
    EXPECT_EQ(msaa.definition->samples, rstd::u32(4));
    EXPECT_EQ(msaa.lifetime, owe::resource::TextureLifetimeClass::Dedicated);

    auto depth = owe::vulkan::MakeDepthTextureRequest("_rt_default::depth"_str, rt);
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

    auto imported = owe::vulkan::MakeImportedTextureRequest("textures/main.png"_str);
    auto output   = owe::vulkan::MakeRenderTargetTextureRequest("_rt_default"_str, rt);
    auto msaa =
        owe::vulkan::MakeMsaaTextureRequest("_rt_default::msaa4"_str, rt, VK_SAMPLE_COUNT_4_BIT);
    auto depth = owe::vulkan::MakeDepthTextureRequest("_rt_default::depth"_str, rt);

    auto texture_use =
        owe::resource::TextureUseHandle { .index = rstd::u64(1), .generation = rstd::u64(1) };
    owe::vulkan::SceneDraw custom(owe::vulkan::SceneDraw::Desc {
        .texture_bindings =
            [texture_use](owe::vulkan::TextureRequest request) mutable {
                Vec<owe::vulkan::TextureBindingRequest> bindings;
                bindings.push(owe::vulkan::TextureBindingRequest {
                    .name    = rstd::string::String::make("textures/main.png"_str),
                    .use     = rstd::Some(texture_use),
                    .request = rstd::Some(std::move(request)),
                });
                return bindings;
            }(std::move(imported)),
        .output         = "_rt_default"_Str,
        .output_request = rstd::Some(output.clone()),
        .depth_request  = rstd::Some(std::move(depth)),
    });
    auto                   custom_diag = custom.textureRequestDiagnostics();
    ASSERT_EQ(custom_diag.len().to_primitive(), 3u);
    EXPECT_EQ(custom_diag[rstd::usize(0)].role, "sampled"_str);
    EXPECT_EQ(custom_diag[rstd::usize(0)].slot, rstd::u32());
    EXPECT_EQ(*custom_diag[rstd::usize(0)].use, texture_use);
    EXPECT_EQ(custom_diag[rstd::usize(0)].request->kind, owe::vulkan::TextureRequestKind::Imported);
    EXPECT_EQ(custom_diag[rstd::usize(1)].role, "output"_str);
    EXPECT_EQ(custom_diag[rstd::usize(1)].request->kind,
              owe::vulkan::TextureRequestKind::RenderTarget);
    EXPECT_EQ(custom_diag[rstd::usize(2)].role, "depth"_str);
    EXPECT_EQ(custom_diag[rstd::usize(2)].request->kind,
              owe::vulkan::TextureRequestKind::DepthAttachment);
}

TEST(PassTextureRequestDiagnostics, RetainsOwnedDataAfterPassDestruction) {
    Vec<owe::vulkan::PassTextureRequestDiagnostic> diagnostics;
    {
        owe::vulkan::SceneDraw pass(owe::vulkan::SceneDraw::Desc {
            .output         = "owned-target"_Str,
            .output_request = Some(owe::vulkan::MakeRenderTargetTextureRequest(
                "owned-target"_str,
                owe::SceneRenderTarget { .width = i32(64), .height = i32(32) })),
        });
        diagnostics = pass.textureRequestDiagnostics();
        ASSERT_EQ(diagnostics.len(), usize(1));
        diagnostics[usize()].name.push_str("-snapshot"_str);
        diagnostics[usize()].request->name.push_str("-snapshot"_str);
        auto live = pass.textureRequestDiagnostics();
        EXPECT_EQ(live[usize()].name, "owned-target"_str);
        EXPECT_EQ(live[usize()].request->name, "owned-target"_str);
    }
    EXPECT_EQ(diagnostics[usize()].name, "owned-target-snapshot"_str);
    EXPECT_EQ(diagnostics[usize()].request->name, "owned-target-snapshot"_str);
    EXPECT_EQ(diagnostics[usize()].request->definition->width, i32(64));
    EXPECT_EQ(owe::vulkan::MsaaTwinName("owned-target"_str, VK_SAMPLE_COUNT_4_BIT),
              "owned-target::msaa4"_str);
}

TEST(RenderTargets, PreservesPhysicalExtentAndMipLevelsOnResize) {
    owe::Scene scene;
    scene.RegisterRenderTarget("bounded"_Str,
                               owe::SceneRenderTarget {
                                   .width      = i32(5000),
                                   .height     = i32(1000),
                                   .has_mipmap = true,
                               });
    owe::vulkan::FinalizeRenderTargetSizes(
        scene, { 1920, 1080 }, { 2048, 512 }, VK_SAMPLE_COUNT_1_BIT);
    auto target = scene.RenderTargetMut("bounded"_str);
    ASSERT_TRUE(target.is_some());
    EXPECT_EQ((**target).width, i32(5000));
    EXPECT_EQ((**target).physical_width, i32(2048));
    EXPECT_EQ((**target).physical_height, i32(512));
    EXPECT_EQ((**target).mipmap_level, 7u);
    (**target).width  = i32(7);
    (**target).height = i32(9);
    owe::vulkan::FinalizeRenderTargetSizes(
        scene, { 1920, 1080 }, { 2048, 512 }, VK_SAMPLE_COUNT_1_BIT);
    EXPECT_EQ((**target).physical_width, i32(7));
    EXPECT_EQ((**target).physical_height, i32(9));
    EXPECT_EQ((**target).mipmap_level, 1u);
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
        request.vertex_bindings.push(VkVertexInputBindingDescription {
            .binding   = 0,
            .stride    = 16,
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        });
        request.vertex_attrs.push(VkVertexInputAttributeDescription {
            .location = 0,
            .binding  = 0,
            .format   = VK_FORMAT_R32G32B32A32_SFLOAT,
            .offset   = 0,
        });
        auto spv         = rstd::boxed::Box<owe::vulkan::ShaderSpv>::make();
        spv->stage       = owe::ShaderType::VERTEX;
        spv->entry_point = "main"_Str;
        spv->spirv =
            owe::ShaderCode::from(rstd::array<rstd::uint32_t, 4> { 1u, 2u, 3u, 4u }.as_slice());
        request.shader_stages.push(rstd::move(spv));
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
    dynamic_state_request.dynamic_states = Vec<VkDynamicState>::from(
        rstd::array<VkDynamicState, 2> { VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_VIEWPORT }
            .as_slice());
    auto key_dynamic_state_order         = owe::vulkan::MakePipelineCacheKey(dynamic_state_request);
    dynamic_state_request.dynamic_states = Vec<VkDynamicState>::from(
        rstd::array<VkDynamicState, 1> { VK_DYNAMIC_STATE_VIEWPORT }.as_slice());
    auto key_dynamic_state = owe::vulkan::MakePipelineCacheKey(dynamic_state_request);

    EXPECT_FALSE(key_a.bytes.is_empty());
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
    auto                                  first = diagnostics.Record(key_a.clone());
    EXPECT_FALSE(first.hit);
    EXPECT_EQ(first.observed_count, rstd::u64(1));

    auto second = diagnostics.Record(key_b.clone());
    EXPECT_TRUE(second.hit);
    EXPECT_EQ(second.observed_count, rstd::u64(2));

    auto third = diagnostics.Record(key_c.clone());
    EXPECT_FALSE(third.hit);
    EXPECT_EQ(third.observed_count, rstd::u64(1));

    auto collision_first = diagnostics.Record(colliding_a.clone());
    EXPECT_FALSE(collision_first.hit);
    EXPECT_EQ(collision_first.observed_count, rstd::u64(1));

    auto collision_second = diagnostics.Record(colliding_b.clone());
    EXPECT_FALSE(collision_second.hit);
    EXPECT_EQ(collision_second.observed_count, rstd::u64(1));

    rstd::collections::HashMap<owe::vulkan::PipelineCacheKey,
                               int,
                               rstd::hash::RandomState,
                               owe::vulkan::PipelineCacheKeyEqual>
        cache_entries;
    (void)cache_entries.insert(colliding_a.clone(), 1);
    (void)cache_entries.insert(colliding_b.clone(), 2);
    EXPECT_EQ(cache_entries.len(), usize(2));
}

TEST(PipelineCacheKey, SeparatesIndependentDepthClipState) {
    PipelineResourceRequest request;
    const auto              implicit = MakePipelineCacheKey(request);
    request.depth_clip               = Some(true);
    const auto enabled               = MakePipelineCacheKey(request);
    EXPECT_FALSE(SamePipelineCacheKey(implicit, enabled));
    EXPECT_TRUE(
        SamePipelineCacheKey(enabled, MakePipelineCacheKey(MakePipelineResourceDesc(request))));
    request.depth_clip  = Some(false);
    const auto disabled = MakePipelineCacheKey(request);
    EXPECT_FALSE(SamePipelineCacheKey(implicit, disabled));
    EXPECT_FALSE(SamePipelineCacheKey(enabled, disabled));
}

TEST(PipelineCacheKey, PreservesShaderEncodingAndOwnedStages) {
    PipelineResourceRequest request;
    auto                    stage = Box<ShaderSpv>::make();
    stage->stage                  = owe::ShaderType::VERTEX;
    stage->spirv =
        owe::ShaderCode::from(array<rstd::uint32_t, 2> { 0x07230203u, 0xffffffffu }.as_slice());
    request.shader_stages.push(rstd::move(stage));
    auto desc                                   = MakePipelineResourceDesc(request);
    request.shader_stages[usize()]->entry_point = "changed"_Str;
    request.shader_stages[usize()]->spirv =
        owe::ShaderCode::from(array<rstd::uint32_t, 2> { 0u, 0xffffffffu }.as_slice());
    request.shader_stages.clear();

    PipelineKeyWriter writer;
    WritePipelineShaderStages(writer, desc.shader_stages.as_slice());
    auto encoded = rstd::move(writer).finish();
    EXPECT_EQ(encoded.bytes,
              Bytes({
                  6, 1, 0, 0, 0, 0, 0, 0, 0, 2,    0,   0,   0,    0,    0,    0,    0,
                  0, 5, 4, 0, 0, 0, 0, 0, 0, 0,    'm', 'a', 'i',  'n',  6,    2,    0,
                  0, 0, 0, 0, 0, 0, 1, 3, 2, 0x23, 7,   1,   0xff, 0xff, 0xff, 0xff,
              }));
    auto fragment        = desc.shader_stages[usize()].clone();
    fragment.stage       = owe::ShaderType::FRAGMENT;
    fragment.entry_point = "fragment"_Str;
    desc.shader_stages.push(rstd::move(fragment));
    auto ordered = MakePipelineCacheKey(desc);
    std::swap(desc.shader_stages[usize()], desc.shader_stages[usize(1)]);
    EXPECT_TRUE(SamePipelineCacheKey(ordered, MakePipelineCacheKey(desc)));
    desc.shader_stages[usize()].spirv =
        owe::ShaderCode::from(array<rstd::uint32_t, 2> { 0x07230203u, 1u }.as_slice());
    EXPECT_FALSE(SamePipelineCacheKey(ordered, MakePipelineCacheKey(desc)));
}

TEST(PipelineCacheKey, OwnsInputDescriptionsAndCanonicalizesOrder) {
    PipelineResourceRequest request;
    request.vertex_bindings.push(
        VkVertexInputBindingDescription { 1, 16, VK_VERTEX_INPUT_RATE_VERTEX });
    request.vertex_bindings.push(
        VkVertexInputBindingDescription { 0, 8, VK_VERTEX_INPUT_RATE_VERTEX });
    request.vertex_attrs.push(
        VkVertexInputAttributeDescription { 1, 1, VK_FORMAT_R32G32_SFLOAT, 8 });
    request.vertex_attrs.push(
        VkVertexInputAttributeDescription { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 });
    auto desc = MakePipelineResourceDesc(request);
    auto key  = MakePipelineCacheKey(desc);
    request.vertex_bindings.clear();
    request.vertex_attrs.clear();
    request.dynamic_states.clear();
    EXPECT_EQ(desc.vertex_bindings.len(), usize(2));
    EXPECT_EQ(desc.vertex_attrs.len(), usize(2));
    EXPECT_EQ(desc.dynamic_states.len(), usize(2));
    std::swap(desc.vertex_bindings[usize()], desc.vertex_bindings[usize(1)]);
    std::swap(desc.vertex_attrs[usize()], desc.vertex_attrs[usize(1)]);
    std::swap(desc.dynamic_states[usize()], desc.dynamic_states[usize(1)]);
    EXPECT_TRUE(SamePipelineCacheKey(key, MakePipelineCacheKey(desc)));
    EXPECT_EQ(desc.vertex_bindings[usize()].binding, 0u);
    EXPECT_EQ(desc.vertex_attrs[usize()].location, 0u);
    desc.vertex_attrs[usize()].offset = 4;
    EXPECT_FALSE(SamePipelineCacheKey(key, MakePipelineCacheKey(desc)));
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

    EXPECT_FALSE(key_a.bytes.is_empty());
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

    rstd::collections::HashMap<owe::vulkan::RenderPassCacheKey,
                               int,
                               rstd::hash::RandomState,
                               owe::vulkan::RenderPassCacheKeyEqual>
        cache_entries;
    (void)cache_entries.insert(colliding_a.clone(), 1);
    (void)cache_entries.insert(colliding_b.clone(), 2);
    EXPECT_EQ(cache_entries.len(), usize(2));
}

TEST(FramebufferCacheDiagnostics, RecordsStableFramebufferKeys) {
    owe::vulkan::FramebufferResourceRequest request {
        .render_pass_key =
            owe::vulkan::RenderPassCacheKey {
                .value = 17u,
                .bytes = Bytes({ 0x17u }),
            },
        .extent = { 320u, 180u },
    };

    request.attachments.push(Attachment(0x101u, 101u, { 0x01u }));
    request.attachments.push(Attachment(0x102u, 102u, { 0x02u }));

    auto key_a = owe::vulkan::MakeFramebufferCacheKey(request);
    auto key_b = owe::vulkan::MakeFramebufferCacheKey(request);

    auto resized         = owe::vulkan::MakeFramebufferResourceDesc(request);
    resized.extent.width = 640u;
    auto key_resized     = owe::vulkan::MakeFramebufferCacheKey(resized);

    auto layered    = owe::vulkan::MakeFramebufferResourceDesc(request);
    layered.layers  = 2u;
    auto key_layers = owe::vulkan::MakeFramebufferCacheKey(layered);

    auto different_attachment                  = owe::vulkan::MakeFramebufferResourceDesc(request);
    different_attachment.attachments[usize(1)] = Attachment(0x103u, 103u, { 0x03u });
    auto key_attachment = owe::vulkan::MakeFramebufferCacheKey(different_attachment);

    auto different_attachment_identity = owe::vulkan::MakeFramebufferResourceDesc(request);
    different_attachment_identity.attachments[usize(1)].identity =
        AttachmentIdentity(104u, { 0x04u });
    auto key_attachment_identity =
        owe::vulkan::MakeFramebufferCacheKey(different_attachment_identity);

    auto different_attachment_view = owe::vulkan::MakeFramebufferResourceDesc(request);
    different_attachment_view.attachments[usize(1)].view = ImageView(0x104u);
    auto key_attachment_view = owe::vulkan::MakeFramebufferCacheKey(different_attachment_view);

    auto different_render_pass            = owe::vulkan::MakeFramebufferResourceDesc(request);
    different_render_pass.render_pass_key = owe::vulkan::RenderPassCacheKey {
        .value = 17u,
        .bytes = Bytes({ 0x18u }),
    };
    auto key_render_pass = owe::vulkan::MakeFramebufferCacheKey(different_render_pass);

    EXPECT_FALSE(key_a.bytes.is_empty());
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
    auto                                     first = diagnostics.Record(key_a.clone());
    EXPECT_FALSE(first.hit);
    EXPECT_EQ(first.observed_count, rstd::u64(1));

    auto second = diagnostics.Record(key_b.clone());
    EXPECT_TRUE(second.hit);
    EXPECT_EQ(second.observed_count, rstd::u64(2));

    auto third = diagnostics.Record(key_resized.clone());
    EXPECT_FALSE(third.hit);
    EXPECT_EQ(third.observed_count, rstd::u64(1));

    auto collision_first = diagnostics.Record(colliding_a.clone());
    EXPECT_FALSE(collision_first.hit);
    EXPECT_EQ(collision_first.observed_count, rstd::u64(1));

    auto collision_second = diagnostics.Record(colliding_b.clone());
    EXPECT_FALSE(collision_second.hit);
    EXPECT_EQ(collision_second.observed_count, rstd::u64(1));

    rstd::collections::HashMap<owe::vulkan::FramebufferCacheKey,
                               int,
                               rstd::hash::RandomState,
                               owe::vulkan::FramebufferCacheKeyEqual>
        cache_entries;
    (void)cache_entries.insert(colliding_a.clone(), 1);
    (void)cache_entries.insert(colliding_b.clone(), 2);
    EXPECT_EQ(cache_entries.len(), usize(2));
}

TEST(PipelineCacheKey, PreservesCanonicalBytesAndHash) {
    vrento::vulkan::PipelineKeyWriter writer;
    writer.writeU32(0x12345678u);
    writer.writeU64(0x0102030405060708ull);
    writer.writeBool(true);
    writer.writeBool(false);
    writer.writeF32(1.0f);
    writer.writeString("a\0b"_str);
    writer.writeArraySize(2);
    auto bytes = Bytes({ 0, 255 });
    writer.writeBytes(bytes.as_slice());
    auto encoded = rstd::move(writer).finish();
    EXPECT_EQ(encoded.bytes,
              Bytes({
                  1, 0x78, 0x56, 0x34, 0x12, 2, 8, 7, 6, 5, 4, 3, 2, 1,   3, 1,   3,   0, 4,
                  0, 0,    0x80, 0x3f, 5,    3, 0, 0, 0, 0, 0, 0, 0, 'a', 0, 'b', 6,   2, 0,
                  0, 0,    0,    0,    0,    0, 7, 2, 0, 0, 0, 0, 0, 0,   0, 0,   255,
              }));
    EXPECT_EQ(encoded.value, static_cast<rstd::size_t>(0x585aef244043fe2bull));
}

TEST(FramebufferCacheDiagnostics, OwnsKeysAndAttachmentDescriptions) {
    FramebufferResourceRequest request;
    request.render_pass_key = { .value = 17, .bytes = Bytes({ 17 }) };
    request.attachments.push(Attachment(0x101, 1, { 1 }));
    request.attachments.push(Attachment(0x102, 2, { 2 }));
    auto desc = MakeFramebufferResourceDesc(request);
    auto key  = MakeFramebufferCacheKey(desc);
    request.render_pass_key.bytes.clear();
    request.attachments[usize()].identity.bytes.clear();
    request.attachments.clear();
    EXPECT_TRUE(SameFramebufferCacheKey(key, MakeFramebufferCacheKey(desc)));
    std::swap(desc.attachments[usize()], desc.attachments[usize(1)]);
    EXPECT_FALSE(SameFramebufferCacheKey(key, MakeFramebufferCacheKey(desc)));
    owe::vulkan::FramebufferCacheDiagnostics diagnostics;
    auto                                     probe = diagnostics.Record(key.clone());
    probe.key.bytes.clear();
    auto next = diagnostics.Record(key.clone());
    EXPECT_TRUE(next.hit);
    EXPECT_EQ(next.observed_count, u64(2));
    auto cloned = next.key.clone();
    cloned.clone_from(key);
    next.key.bytes.clear();
    EXPECT_TRUE(SameFramebufferCacheKey(key, cloned));
    diagnostics.Reset();
    EXPECT_FALSE(diagnostics.Record(rstd::move(key)).hit);
}

TEST(PreparedResourceTable, ClonesRenderPassKeyIndependently) {
    PreparedResourceTable table(u64(3));
    auto use      = owe::resource::RenderPassUseHandle { .index = u64(1), .generation = u64(3) };
    auto physical = rstd::sync::Arc<vvk::RenderPass>::make();
    ASSERT_TRUE(table.Insert(PreparedRenderPass {
        .use       = use,
        .resource  = { .index = u64(2), .generation = u64(1) },
        .cache_key = { .value = 17, .bytes = Bytes({ 17 }) },
        .physical  = physical.clone(),
    }));
    auto snapshot = table.clone();
    table         = PreparedResourceTable(u64(4));
    auto saved    = snapshot.Resolve(use);
    ASSERT_TRUE(saved.is_some());
    EXPECT_EQ((**saved).cache_key.bytes, Bytes({ 17 }));
    EXPECT_EQ((**saved).cache_key.value, 17u);
    EXPECT_EQ(physical.strong_count(), usize(2));
}

TEST(FramebufferAttachmentIdentity, TracksTextureGeneration) {
    owe::SceneRenderTarget rt {
        .width      = i32(320),
        .height     = i32(180),
        .allowReuse = true,
    };
    auto request = owe::vulkan::MakeRenderTargetTextureRequest("_rt_default"_str, rt);

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
    EXPECT_FALSE(attachment_a.identity.bytes.is_empty());
    EXPECT_NE(attachment_a.identity.bytes, attachment_b.identity.bytes);

    owe::vulkan::FramebufferResourceRequest framebuffer_a {
        .render_pass_key =
            owe::vulkan::RenderPassCacheKey {
                .value = 17u,
                .bytes = Bytes({ 0x17u }),
            },
        .extent = { 320u, 180u },
    };
    framebuffer_a.attachments.push(rstd::move(attachment_a));
    auto framebuffer_b = owe::vulkan::MakeFramebufferResourceDesc(framebuffer_a);
    framebuffer_b.attachments.clear();
    framebuffer_b.attachments.push(rstd::move(attachment_b));

    EXPECT_FALSE(
        owe::vulkan::SameFramebufferCacheKey(owe::vulkan::MakeFramebufferCacheKey(framebuffer_a),
                                             owe::vulkan::MakeFramebufferCacheKey(framebuffer_b)));
}
