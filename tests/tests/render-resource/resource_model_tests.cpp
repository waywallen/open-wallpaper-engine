#include <rstd/test/gtest.hpp>

import rstd;
import rstd.cppstd;
import wescene.resource;
import wescene.types;

using namespace rstd::literals;

namespace
{

auto TextureRequest(std::string_view name, rstd::u32 width) -> owe::resource::TextureRequest {
    return owe::resource::TextureRequest {
        .kind       = owe::resource::TextureRequestKind::RenderTarget,
        .name       = rstd::string::String::make(rstd::cppstd::as_str(name).unwrap()),
        .definition = rstd::Some(owe::resource::TextureDefinition {
            .width      = rstd::as_cast<rstd::i32>(width),
            .height     = rstd::i32(128),
            .usage      = owe::resource::TextureUsage::Color,
            .format     = owe::TextureFormat::RGBA8,
            .mip_levels = rstd::u32(2),
        }),
        .lifetime   = owe::resource::TextureLifetimeClass::Retained,
    };
}

} // namespace

namespace resource_model_test
{

struct CountingVisitor {
    rstd::usize textures { 0 };
    rstd::usize buffers { 0 };
    rstd::usize shaders { 0 };
};

} // namespace resource_model_test

namespace rstd
{

template<>
struct Impl<owe::resource::ResourcePlanVisitor, resource_model_test::CountingVisitor>
    : ImplBase<resource_model_test::CountingVisitor> {
    auto VisitTexture(const owe::resource::TexturePlanEntry&)
        -> Result<empty, owe::resource::ResourceError> {
        ++this->self().textures;
        return Ok(empty {});
    }

    auto VisitBuffer(const owe::resource::BufferPlanEntry&)
        -> Result<empty, owe::resource::ResourceError> {
        ++this->self().buffers;
        return Ok(empty {});
    }

    auto VisitShader(const owe::resource::ShaderPlanEntry&)
        -> Result<empty, owe::resource::ResourceError> {
        ++this->self().shaders;
        return Ok(empty {});
    }
};

} // namespace rstd

TEST(ResourceModel, ClonesMoveOnlyTextureRequestsExplicitly) {
    auto request = TextureRequest("frame", rstd::u32(256));
    auto cloned  = request.clone();

    EXPECT_TRUE(owe::resource::SameTextureRequest(request, cloned));
    cloned.name.push_str("-copy"_str);
    EXPECT_FALSE(owe::resource::SameTextureRequest(request, cloned));
    EXPECT_EQ(rstd::cppstd::as_string_view(request.name.as_str()), "frame");
}

TEST(ResourceModel, ReplacesRequestsOnlyWhenTheirDefinitionChanges) {
    auto                                        current = TextureRequest("frame", rstd::u32(256));
    rstd::Option<owe::resource::TextureRequest> slot    = rstd::Some(current.clone());

    EXPECT_FALSE(owe::resource::SetTextureRequestIfChanged(slot, current.clone()));
    EXPECT_TRUE(
        owe::resource::SetTextureRequestIfChanged(slot, TextureRequest("frame", rstd::u32(512))));
    ASSERT_TRUE(slot.is_some());
    ASSERT_TRUE(slot->definition.is_some());
    EXPECT_EQ(slot->definition->width, rstd::i32(512));
}

TEST(ResourceModel, DistinguishesDepthSamplingAndComparisonSamplerDefinitions) {
    auto attachment               = TextureRequest("shadow", rstd::u32(768));
    attachment.definition->usage  = owe::resource::TextureUsage::DepthAttachment;
    attachment.definition->format = owe::TextureFormat::D32F;

    auto sampled = attachment.clone();
    sampled.definition->usage =
        owe::resource::TextureUsage::DepthAttachment | owe::resource::TextureUsage::Sampled;
    EXPECT_FALSE(owe::resource::SameTextureRequest(attachment, sampled));

    auto compared                              = sampled.clone();
    compared.definition->sample.compare_enable = true;
    compared.definition->sample.compare_op     = owe::CompareOp::LessEqual;
    EXPECT_FALSE(owe::resource::SameTextureRequest(sampled, compared));
    auto border                            = compared.clone();
    border.definition->sample.border_color = owe::TextureBorderColor::TransparentBlack;
    EXPECT_FALSE(owe::resource::SameTextureRequest(compared, border));
    EXPECT_TRUE(owe::resource::HasTextureUsage(compared.definition->usage,
                                               owe::resource::TextureUsage::DepthAttachment));
    EXPECT_TRUE(owe::resource::HasTextureUsage(compared.definition->usage,
                                               owe::resource::TextureUsage::Sampled));
}

TEST(ResourcePlan, OwnsBackendNeutralRequestsByTypedUseHandle) {
    owe::resource::ResourcePlan plan { .generation = rstd::u64(7) };
    plan.textures.push(owe::resource::TexturePlanEntry {
        .handle =
            owe::resource::TextureUseHandle { .index = rstd::u64(2), .generation = rstd::u64(1) },
        .request = TextureRequest("frame", rstd::u32(256)),
        .access  = owe::resource::ResourceAccess::Write,
        .version = rstd::u32(3),
    });

    ASSERT_EQ(plan.textures.len(), rstd::usize(1));
    EXPECT_TRUE(plan.textures[rstd::usize()].handle.Valid());
    EXPECT_EQ(plan.textures[rstd::usize()].version, rstd::u32(3));
    EXPECT_EQ(plan.textures[rstd::usize()].access, owe::resource::ResourceAccess::Write);
    EXPECT_EQ(rstd::cppstd::as_string_view(plan.textures[rstd::usize()].request.name.as_str()),
              "frame");
}

TEST(ResourcePlan, VisitsTypedRequestsThroughPublicTrait) {
    owe::resource::ResourcePlan plan { .generation = rstd::u64(11) };
    plan.textures.push(owe::resource::TexturePlanEntry {
        .handle =
            owe::resource::TextureUseHandle { .index = rstd::u64(1), .generation = rstd::u64(11) },
        .request = TextureRequest("frame", rstd::u32(64)),
    });
    plan.buffers.push(owe::resource::BufferPlanEntry {
        .handle =
            owe::resource::BufferUseHandle { .index = rstd::u64(2), .generation = rstd::u64(11) },
        .request =
            owe::resource::BufferRequest {
                .name       = rstd::string::String::make("vertices"_str),
                .definition = { .size  = rstd::usize(128),
                                .usage = owe::resource::BufferUsage::Vertex },
            },
    });
    plan.shaders.push(owe::resource::ShaderPlanEntry {
        .handle =
            owe::resource::ShaderUseHandle { .index = rstd::u64(3), .generation = rstd::u64(11) },
        .request =
            owe::resource::ShaderRequest {
                .name   = rstd::string::String::make("sprite"_str),
                .source = owe::resource::ShaderDefinitionId { .index      = rstd::u32(4),
                                                              .generation = rstd::u64(2) },
            },
    });

    resource_model_test::CountingVisitor counter;
    auto visitor = rstd::dyn<owe::resource::ResourcePlanVisitor>::from_ref(counter);
    auto result  = owe::resource::VisitResourcePlan(plan, visitor);

    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(counter.textures, rstd::usize(1));
    EXPECT_EQ(counter.buffers, rstd::usize(1));
    EXPECT_EQ(counter.shaders, rstd::usize(1));
}

TEST(ShaderArtifact, ClonesOwnedStageCode) {
    owe::resource::ShaderArtifact artifact {
        .source =
            owe::resource::ShaderDefinitionId { .index = rstd::u32(1), .generation = rstd::u64(3) },
    };
    owe::resource::ShaderArtifactStage stage {
        .stage       = owe::ShaderType::VERTEX,
        .entry_point = rstd::string::String::make("main"_str),
    };
    stage.code.push(7u);
    artifact.stages.push(rstd::move(stage));

    auto cloned                                      = artifact.clone();
    cloned.stages[rstd::usize()].code[rstd::usize()] = 9u;

    EXPECT_EQ(artifact.stages[rstd::usize()].code[rstd::usize()], 7u);
    EXPECT_EQ(cloned.stages[rstd::usize()].code[rstd::usize()], 9u);
}
