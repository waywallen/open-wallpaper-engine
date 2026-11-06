#include <rstd/test/gtest.hpp>
import rstd;
import wescene.scene_render_pipeline;
import wescene.shader_compile;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

TEST(PrepareError, OwnsNestedResourceCauseAfterContextRelease) {
    auto make = [] {
        auto resource = vrento::resource::ResourceError {
            .message = "prepare texture from scene/image.tex"_Str,
            .source  = Some(vrento::resource::ResourceErrorSource::Texture(
                vrento::texture::Error { vrento::texture::ErrorKind::Lost, -4 }))
        };
        auto error = owe::vulkan::RenderPrepareError::Resources(
            owe::vulkan::ResourcePrepareOperation::Continue, rstd::move(resource));
        error.context = { u64(17), 23, 29 };
        return owe::ScenePrepareError::Renderer(rstd::move(error));
    };
    auto  error    = make();
    auto& renderer = error.as_Renderer().source;
    EXPECT_EQ(renderer.context.resource_transaction, u64(17));
    EXPECT_EQ(renderer.context.compilation, 23u);
    EXPECT_EQ(renderer.context.scope, 29u);
    auto& resource = renderer.as_Resources().source;
    EXPECT_TRUE(resource.message.as_str().contains("scene/image.tex"_str));
    EXPECT_EQ(resource.source->as_Texture().source.native, -4);
    auto     source = as<rstd::error::Error>(error).source();
    unsigned depth {};
    while (source) {
        ++depth;
        source = (*source)->source();
    }
    EXPECT_EQ(depth, 4u);
}

TEST(PrepareError, NativeCompileCauseSurvivesStatusCopies) {
    vrento::vulkan::GraphExecutionError error { vrento::vulkan::GraphExecutionStage::Prepare,
                                                { 7 } };
    error.cause   = vrento::vulkan::GraphExecutionCause::Resource(vrento::resource::ResourceError {
        .message = "create pipeline failed"_Str,
        .source  = Some(vrento::resource::ResourceErrorSource::Native(-3)) });
    auto snapshot = vrento::vulkan::GraphExecutionErrorSnapshot::capture(error);
    error.cause =
        vrento::vulkan::GraphExecutionCause::Reason(vrento::vulkan::GraphFailure::InvalidState);
    EXPECT_EQ(snapshot.pass.value, 7u);
    EXPECT_TRUE(snapshot.diagnostic.sources.len() >= usize(2));
}

TEST(PrepareError, RetainsDrawIdentityAndUniformCause) {
    owe::RenderItemId item { u32(11), u64(3) };
    auto              error = owe::vulkan::RenderPrepareError::Draw(
        vrento::vulkan::PassId { 9 },
        item,
        owe::vulkan::DrawPrepareError::Uniform(
            vrento::UniformBufferUpdateError { "missing uniform binding"_Str }));
    EXPECT_EQ(error.as_Draw().pass.value, 9u);
    EXPECT_EQ(error.as_Draw().item.index, u32(11));
    EXPECT_TRUE(
        error.as_Draw().source.as_Uniform().source.message.as_str().contains("binding"_str));
}

TEST(PrepareError, PipelineOwnsFailureAcrossRetryAndDestruction) {
    Option<Arc<owe::ScenePrepareFailure>> first;
    {
        owe::Scene               scene;
        owe::UniformRuntimeInput input(
            Arc<owe::UniformSceneState>::make(scene.AudioDemandHandle()));
        owe::vulkan::VulkanRender renderer;
        owe::SceneRenderPipeline  pipeline(scene, input, renderer);
        auto                      failed = pipeline.rebuild();
        ASSERT_TRUE(failed.is_err());
        first = pipeline.diagnostics().last_failure;
        ASSERT_TRUE(first.is_some());
        EXPECT_EQ((*first)->attempt(), u64(1));
        auto retried = pipeline.retryPreparation();
        ASSERT_TRUE(retried.is_err());
        auto diagnostics = pipeline.diagnostics();
        EXPECT_EQ(diagnostics.attempt, u64(2));
        EXPECT_EQ((*diagnostics.last_failure)->attempt(), u64(2));
        EXPECT_EQ((*first)->attempt(), u64(1));
        EXPECT_EQ(pipeline.stats().preparation_failures, 2u);
    }
    EXPECT_TRUE((*first)->error().message.as_str().contains("renderer"_str));
    EXPECT_TRUE((*first)->error().sources.len() > usize());
}

TEST(PrepareError, ShaderBackendReturnsCompilerDiagnostic) {
    auto                                backend = owe::vulkan::MakeShaderBackend();
    Vec<vrento::vulkan::ShaderCompUnit> units;
    units.push({ vrento::ShaderType::FRAGMENT, "#version 450\nthis is invalid shader source"_Str });
    Vec<vrento::vulkan::Uni_ShaderSpv> output;
    auto result = backend->CompileAndLinkShaderUnits(units.as_slice(), {}, output);
    ASSERT_TRUE(result.is_err());
    auto error = rstd::move(result).unwrap_err();
    EXPECT_TRUE(error.message.as_str().contains("glslang parse"_str));
    EXPECT_TRUE(error.message.as_str().contains("ERROR"_str));
}
