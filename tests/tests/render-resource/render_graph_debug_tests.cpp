#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

import rstd;
import rstd.cppstd;
import eigen;
import wescene.resource;
import vrento;
import wescene.vulkan;
import wescene.scene;
import wescene.types;
import wescene.vulkan_render;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

using namespace owe::vulkan;

namespace
{
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

namespace fg = vrento::fg;
auto GraphMaterial(usize texture_count = usize()) -> owe::SceneMaterial {
    auto   backend = MakeShaderBackend();
    String source  = "#version 450\n"_Str;
    for (usize index {}; index < texture_count; ++index)
        source.push_str(
            rstd::format("layout(set=1,binding={}) uniform sampler2D g_Texture{};\n", index, index)
                .as_str());
    source.push_str("void main() { gl_Position = vec4(0, 0, 0, 1);"_str);
    for (usize index {}; index < texture_count; ++index)
        source.push_str(
            rstd::format("gl_Position += textureLod(g_Texture{}, vec2(0), 0);", index).as_str());
    source.push_str("}"_str);
    ShaderCompUnit     unit { .stage       = owe::ShaderType::VERTEX,
                              .src         = rstd::move(source),
                              .entry_point = "main"_Str };
    Vec<Uni_ShaderSpv> compiled;
    rstd_assert(backend->CompileAndLinkShaderUnits(
        slice<ShaderCompUnit>::from_raw_parts(&unit, usize(1)), {}, compiled));
    auto shader = Arc<owe::SceneShader>::make();
    shader->codes.push(compiled[usize()]->spirv.clone());
    for (usize index {}; index < texture_count; ++index)
        shader->sampler_bindings.push({ index.to_primitive(), rstd::format("g_Texture{}", index) });
    owe::SceneMaterial material;
    material.customShader.shader = Some(rstd::move(shader));
    return material;
}
auto CompositeDesc() -> SceneRenderTextureSpec {
    return { .key = "layer-composite"_Str,
             .definition =
                 resource::TextureRequest {
                     .kind       = resource::TextureRequestKind::RenderTarget,
                     .name       = "layer-composite"_Str,
                     .definition = Some(
                         resource::TextureDefinition { .width = i32(1920), .height = i32(1080) }),
                     .lifetime = resource::TextureLifetimeClass::FrameLocal },
             .allocation_family = Some("layer-composite"_Str) };
}
} // namespace
TEST(RenderGraphOwnership, PreservesDrawSourcesAcrossGrowthAndMoves) {
    fg::FrameGraphBuilder      builder(73);
    Vec<SceneRenderDrawSource> draws;
    Vec<const SceneDraw*>      addresses;
    for (unsigned i {}; i < 128; ++i) {
        auto id     = builder.add_pass({ i + 1 }).unwrap();
        auto source = Box<SceneDraw>::make(SceneDraw::Desc {});
        addresses.push(source.get());
        draws.push({ id, rstd::move(source) });
    }
    SceneRenderStateInput input { rstd::move(builder).finish().unwrap(),
                                  SceneRenderTextures(u64(73)),
                                  rstd::move(draws) };
    for (const auto& node : input.graph.passes())
        input.descriptions.push(
            make_test_pass(73,
                           1,
                           vrento::vulkan::PassExecutionInfo { .id = node.id, .kind = 7 },
                           TestPassDesc {}));
    auto moved = rstd::move(input);
    for (usize i {}; i < addresses.len(); ++i) {
        auto source = moved.draw({ i.to_primitive() + 1 });
        ASSERT_TRUE(source);
        EXPECT_EQ(source->as_raw_ptr(), addresses[i]);
    }
    EXPECT_FALSE(moved.draw({ 999 }));
    SceneRenderState state;
    ASSERT_TRUE(state.install(rstd::move(moved)).is_ok());
    auto relocated = rstd::move(state);
    for (usize i {}; i < addresses.len(); ++i) {
        auto source = relocated.draw(73, { i.to_primitive() + 1 });
        ASSERT_TRUE(source);
        EXPECT_EQ(source->as_raw_ptr(), addresses[i]);
    }
    EXPECT_FALSE(relocated.draw(74, { 1 }));
    fg::FrameGraphBuilder replacement(74);
    ASSERT_TRUE(relocated
                    .install(SceneRenderStateInput { rstd::move(replacement).finish().unwrap(),
                                                     SceneRenderTextures(u64(74)) })
                    .is_ok());
    EXPECT_FALSE(relocated.draw(73, { 1 }));
}
TEST(RenderGraphOwnership, RecordsPublicPassesAndDescriptionKinds) {
    namespace vk = vrento::vulkan;
    fg::FrameGraphBuilder builder(75);
    auto                  source  = builder.import_resource({ .id = { 1, 75 } }).unwrap();
    auto                  surface = builder.import_resource({ .id = { 2, 75 } }).unwrap();
    auto       output = vk::declare_surface_output(builder, { 1 }, { 2 }, source, surface).unwrap();
    const auto transfer_kind = output.transfer.execution().kind;
    const auto release_kind  = output.release.execution().kind;
    SceneRenderStateInput input { rstd::move(builder).finish().unwrap(),
                                  SceneRenderTextures(u64(75)) };
    input.descriptions.push(rstd::move(output.transfer));
    input.descriptions.push(rstd::move(output.release));
    SceneRenderState state;
    ASSERT_TRUE(state.install(rstd::move(input)).is_ok());
    ASSERT_EQ(state.inputGraph().passes().len(), usize(2));
    auto diagnostics = state.diagnostics(false, nullptr);
    ASSERT_EQ(diagnostics.len(), usize(2));
    ASSERT_TRUE(diagnostics[usize()].pass_kind);
    ASSERT_TRUE(diagnostics[usize(1)].pass_kind);
    EXPECT_EQ(*diagnostics[usize()].pass_kind, transfer_kind);
    EXPECT_EQ(*diagnostics[usize(1)].pass_kind, release_kind);
    EXPECT_EQ(diagnostics[usize()].scope, 75u);
    EXPECT_EQ(diagnostics[usize()].revision, 1u);
    Vec<vk::PassInput> stale;
    stale.push(
        make_test_pass(74, 99, vk::PassExecutionInfo { .id = { 1 }, .kind = 7 }, TestPassDesc {}));
    auto current = state.diagnostics(false, nullptr, stale.as_slice());
    EXPECT_EQ(current[usize()].scope, 75u);
    EXPECT_EQ(current[usize()].revision, 1u);
    EXPECT_EQ(*current[usize()].pass_kind, transfer_kind);
    EXPECT_EQ(state.inputGraph().find_pass({ 2 })->id, (fg::PassId { 2 }));
}
TEST(RenderGraphOwnership, DerivesCopyResourcesAndOwnedDiagnosticsFromGraph) {
    namespace vk = vrento::vulkan;
    fg::FrameGraphBuilder builder(76);
    SceneRenderTextures   textures(u64(76));
    auto                  source =
        textures
            .declare(builder,
                     { .key        = "source"_Str,
                       .imported   = true,
                       .definition = MakeRenderTargetTextureRequest(
                           "source"_str,
                           owe::SceneRenderTarget { .width = i32(64), .height = i32(32) }) })
            .unwrap();
    auto target =
        textures
            .declare(builder,
                     { .key        = "target"_Str,
                       .definition = MakeRenderTargetTextureRequest(
                           "target"_str,
                           owe::SceneRenderTarget { .width = i32(64), .height = i32(32) }) })
            .unwrap();
    auto                  copy = vk::declare_texture_copy(builder, { 1 }, source, target).unwrap();
    auto                  source_use = textures.handle(source).unwrap();
    auto                  target_use = textures.handle(copy.output).unwrap();
    SceneRenderStateInput input { rstd::move(builder).finish().unwrap(), rstd::move(textures) };
    input.descriptions.push(rstd::move(copy.description));
    SceneRenderState state;
    ASSERT_TRUE(state.install(rstd::move(input)).is_ok());
    const auto& uses = state.resourcePlan().textures;
    ASSERT_EQ(uses.len(), usize(2));
    EXPECT_EQ(uses[usize()].handle, source_use);
    EXPECT_EQ(uses[usize(1)].handle, target_use);
    auto diagnostics = state.diagnostics(false, nullptr);
    ASSERT_EQ(diagnostics.len(), usize(1));
    auto& requests = diagnostics[usize()].texture_requests;
    ASSERT_EQ(requests.len(), usize(2));
    EXPECT_EQ(requests[usize()].role, "read"_str);
    EXPECT_EQ(requests[usize(1)].role, "write"_str);
    ASSERT_TRUE(requests[usize()].use);
    ASSERT_TRUE(requests[usize(1)].use);
    EXPECT_EQ(*requests[usize()].use, source_use);
    EXPECT_EQ(*requests[usize(1)].use, target_use);
    state.clear();
    EXPECT_EQ(requests[usize()].name, "source"_str);
    EXPECT_EQ(requests[usize(1)].name, "target"_str);
    ASSERT_TRUE(requests[usize(1)].request);
    EXPECT_EQ(requests[usize(1)].request->definition->width, i32(64));
}
TEST(RenderGraphOwnership, ContentReplacementKeepsGraphUntilSpecificationChanges) {
    owe::Scene scene;
    scene.RegisterTexture("asset"_Str, owe::SceneTexture { .url = "asset"_Str });
    auto original     = Arc<owe::Image>::make();
    original->content = MakeMissingTexturePlaceholder("asset"_str);
    scene.RegisterRuntimeImage("asset"_Str, original.clone());
    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
    auto definition =
        snapshot.textureDesc(snapshot.textureDescId("asset"_str).unwrap())->desc.resource.clone();
    auto shape = vrento::texture::describe_image(*original->content).unwrap()[usize()];
    fg::FrameGraphBuilder builder(77);
    SceneRenderTextures   textures(u64(77));
    auto                  version = textures
                                        .declare(builder,
                                                 { .key        = "asset"_Str,
                                                   .imported   = true,
                                                   .definition = rstd::move(definition),
                                                   .metadata   = Some(shape) })
                                        .unwrap();
    SceneRenderState      state;
    ASSERT_TRUE(state
                    .install(SceneRenderStateInput { rstd::move(builder).finish().unwrap(),
                                                     rstd::move(textures) })
                    .is_ok());
    auto replacement     = Arc<owe::Image>::make();
    replacement->content = MakeMissingTexturePlaceholder("asset"_str);
    scene.RegisterRuntimeImage("asset"_Str, replacement.clone());
    snapshot = owe::ExtractRenderSceneSnapshot(scene);
    EXPECT_TRUE(state.acceptsTextureContent(scene, snapshot, scene.CaptureImageSource()));
    EXPECT_EQ(state.inputGraph().scope(), 77u);
    EXPECT_TRUE(state.inputGraph().find_version(version));
    auto changed                           = Arc<owe::Image>::make();
    changed->content                       = MakeMissingTexturePlaceholder("asset"_str);
    changed->content->slots[usize()].width = 4;
    scene.RegisterRuntimeImage("asset"_Str, changed.clone());
    snapshot = owe::ExtractRenderSceneSnapshot(scene);
    EXPECT_FALSE(state.acceptsTextureContent(scene, snapshot, scene.CaptureImageSource()));
    EXPECT_TRUE(state.inputGraph().find_version(version));
}

TEST(FrameGraphDependencies, StoresHandlesAndDeduplicatesEdges) {
    fg::FrameGraphBuilder builder(1);
    auto a = builder.add_pass({ 1 }).unwrap(), b = builder.add_pass({ 2 }).unwrap();
    builder.depends_on(b, a).unwrap();
    builder.depends_on(b, a).unwrap();
    EXPECT_EQ(builder.data().dependencies.len(), usize(1));
    EXPECT_TRUE(builder.depends_on({ 3 }, a).is_err());
    builder.depends_on(a, b).unwrap();
    EXPECT_EQ(rstd::move(builder).finish().unwrap_err().kind, fg::ErrorKind::Cycle);
}
TEST(RenderGraphDebug, GraphvizIncludesResourceRefsAndAccessLabels) {
    fg::FrameGraphBuilder builder(1);
    auto                  pass = builder.add_pass({ 1 }, {}, "draw/main"_Str).unwrap();
    auto texture = builder.import_resource({ .id = { 1, 1 }, .name = "albedo"_Str }).unwrap();
    builder.read(pass, texture).unwrap();
    auto graph = rstd::move(builder).finish().unwrap();
    auto dot   = fg::graphviz(graph);
    EXPECT_TRUE(dot->contains("draw/main"_str));
    EXPECT_TRUE(dot->contains("albedo"_str));
    EXPECT_TRUE(dot->contains("read"_str));
    EXPECT_EQ(graph.find_pass(pass)->id, pass);
    EXPECT_EQ(graph.find_version(texture)->id, texture);
    EXPECT_EQ(graph.find_resource({ 1, 2 }), nullptr);
}
TEST(RenderGraphResources, CompilesBackendNeutralTexturePlan) {
    fg::FrameGraphBuilder builder(1);
    SceneRenderTextures   textures(u64(1));
    auto                  source = CompositeDesc();
    source.key                   = "albedo"_Str;
    source.imported              = true;
    auto source_request          = source.request().clone();
    source_request.kind          = resource::TextureRequestKind::Imported;
    source.definition            = source.definition.with_definition(rstd::move(source_request));
    auto in                      = textures.declare(builder, rstd::move(source)).unwrap();
    auto initial                 = textures.declare(builder, CompositeDesc()).unwrap();
    auto pass                    = builder.add_pass({ 1 }).unwrap();
    builder.read(pass, in).unwrap();
    auto out = builder.write(pass, initial).unwrap();
    textures.handle(in).unwrap();
    textures.handle(out).unwrap();
    auto graph = rstd::move(builder).finish().unwrap();
    auto plan  = textures.resourcePlan(graph).unwrap();
    ASSERT_EQ(plan.textures.len(), usize(2));
    EXPECT_EQ(plan.textures[usize()].access, resource::ResourceAccess::Read);
    EXPECT_EQ(plan.textures[usize(1)].access, resource::ResourceAccess::Write);
    EXPECT_EQ(plan.textures[usize(1)].request().definition->width, i32(1920));
    EXPECT_TRUE(
        textures.version(resource::TextureUseHandle { plan.textures[usize()].handle.index, u64(2) })
            .is_none());
    EXPECT_TRUE(textures.handle({ { 1, 2 }, 0 }).is_err());
    fg::FrameGraphBuilder other(2);
    EXPECT_TRUE(textures.resourcePlan(rstd::move(other).finish().unwrap()).is_err());
}
TEST(RenderGraphResources, KeepsGraphKeyOutsideTextureDefinition) {
    fg::FrameGraphBuilder builder(1);
    SceneRenderTextures   textures(u64(1));
    auto                  spec = CompositeDesc();
    spec.key                   = "snapshot"_Str;
    auto definition            = spec.definition.clone();
    auto initial               = textures.declare(builder, rstd::move(spec)).unwrap();
    auto pass                  = builder.add_pass({ 1 }).unwrap();
    auto out                   = builder.write(pass, initial).unwrap();
    textures.handle(out).unwrap();
    auto plan = textures.resourcePlan(rstd::move(builder).finish().unwrap()).unwrap();
    EXPECT_EQ(plan.textures[usize()].definition.id(), definition.id());
    EXPECT_EQ(plan.textures[usize()].definition.revision(), definition.revision());
    EXPECT_EQ(plan.textures[usize()].request().name, definition.definition().name);
}
TEST(RenderGraphResources, AlternatesPhysicalSlotsForLinearCompositeVersions) {
    fg::FrameGraphBuilder builder(1);
    SceneRenderTextures   textures(u64(1));
    auto                  current = textures.declare(builder, CompositeDesc()).unwrap();
    for (unsigned i {}; i < 3; ++i) {
        auto pass = builder.add_pass({ i + 1 }).unwrap();
        if (i) builder.read(pass, current).unwrap();
        current = builder.write(pass, current).unwrap();
        textures.handle(current).unwrap();
    }
    auto plan = textures.resourcePlan(rstd::move(builder).finish().unwrap()).unwrap();
    ASSERT_EQ(plan.textures.len(), usize(3));
    EXPECT_NE(plan.textures[usize()].allocation_key, plan.textures[usize(1)].allocation_key);
    EXPECT_EQ(plan.textures[usize()].allocation_key, plan.textures[usize(2)].allocation_key);
}
TEST(RenderGraphResources, ReusesOverwriteStorageAndKeepsConcurrentVersionsApart) {
    for (bool concurrent : { false, true }) {
        fg::FrameGraphBuilder builder(1);
        SceneRenderTextures   textures(u64(1));
        auto                  current = textures.declare(builder, CompositeDesc()).unwrap();
        auto a = builder.add_pass({ 1 }).unwrap(), b = builder.add_pass({ 2 }).unwrap(),
             c      = builder.add_pass({ 3 }).unwrap();
        auto first  = builder.write(a, current).unwrap();
        auto second = builder.write(b, first).unwrap();
        builder.depends_on(b, a).unwrap();
        builder.read(c, second).unwrap();
        if (concurrent) builder.read(c, first).unwrap();
        textures.handle(first).unwrap();
        textures.handle(second).unwrap();
        auto plan = textures.resourcePlan(rstd::move(builder).finish().unwrap()).unwrap();
        EXPECT_EQ(plan.textures[usize()].allocation_key == plan.textures[usize(1)].allocation_key,
                  ! concurrent);
    }
}
TEST(RenderGraphResources, PreservesFrameBoundaryTextureVersions) {
    fg::FrameGraphBuilder builder(1);
    SceneRenderTextures   textures(u64(1));
    auto                  spec = CompositeDesc();
    spec.allocation_family     = None();
    auto definition            = spec.definition.clone();
    auto history               = textures.declare(builder, rstd::move(spec)).unwrap();
    builder.import_initial(history.resource).unwrap();
    auto a = builder.add_pass({ 1 }).unwrap(), b = builder.add_pass({ 2 }).unwrap();
    builder.read(a, history).unwrap();
    auto out = builder.write(b, history).unwrap();
    textures.handle(history).unwrap();
    textures.handle(out).unwrap();
    auto graph = rstd::move(builder).finish().unwrap();
    EXPECT_TRUE(graph.inspect().precedes(0, 1));
    auto plan = textures.resourcePlan(graph).unwrap();
    EXPECT_EQ(plan.textures[usize()].allocation_key, plan.textures[usize(1)].allocation_key);
    for (const auto& entry : plan.textures) {
        EXPECT_TRUE(entry.preserve_contents);
        EXPECT_EQ(entry.definition.id(), definition.id());
        EXPECT_EQ(entry.definition.revision(), definition.revision());
        EXPECT_EQ(entry.request().lifetime, definition.definition().lifetime);
    }
}

TEST(SceneRenderGraph, ReflectionSelectsSubmeshesByActiveMaterialInputs) {
    owe::Scene scene;
    scene.RegisterRenderTarget(
        "_rt_default"_Str,
        owe::SceneRenderTarget { .width = i32(64), .height = i32(32), .withDepth = true });
    scene.EnablePlanarReflection();
    auto node = Arc<owe::SceneNode>::make();
    node->SetReflected(true);
    auto mesh = Arc<owe::SceneMesh>::make();
    for (usize index {}; index < usize(3); ++index) {
        auto material = GraphMaterial(index == usize() ? usize(1) : usize());
        material.name = rstd::format("part-{}", index);
        material.SetDepthTest(true);
        material.SetDepthWrite(true);
        if (index != usize(1)) material.textures.push("_rt_Reflection"_Str);
        mesh->AddMaterial(rstd::move(material));
        owe::SceneMesh::Submesh submesh;
        submesh.material_slot = rstd::as_cast<u32>(index);
        mesh->Submeshes().push(rstd::move(submesh));
    }
    node->AddMesh(rstd::move(mesh));
    scene.RootMut()->AppendChild(rstd::move(node));

    auto graph = owe::BuildSceneRenderState(scene).unwrap();
    ASSERT_TRUE(graph.graph.compile().is_ok());
    auto          order = graph.graph.inspect().order.clone();
    Option<usize> clear_index, last_reflection_index, receiver_index;
    usize         reflection_draws {}, primary_draws {};
    for (usize index {}; index < order.len(); ++index) {
        const auto& pass = graph.graph.passes()[usize(order[index])];
        if (pass.name == "reflection/clear"_str) clear_index = Some(index);
        auto draw = graph.draw(pass.id);
        if (! draw) continue;
        const auto& parameters = (*draw)->parameters();
        if (parameters.render_view == owe::SceneRenderViewKind::Reflection) {
            ++reflection_draws;
            EXPECT_NE(parameters.submesh_index, u32());
            EXPECT_EQ(parameters.output, "_rt_Reflection"_str);
            for (const auto& binding : parameters.texture_bindings)
                if (binding.name == "_rt_Reflection"_str) EXPECT_FALSE(binding.use);
            ASSERT_TRUE(clear_index);
            EXPECT_LT(*clear_index, index);
            EXPECT_EQ(parameters.clear_depth, reflection_draws == usize(1));
            last_reflection_index = Some(index);
        } else {
            ++primary_draws;
            if (parameters.submesh_index == u32()) receiver_index = Some(index);
        }
    }
    EXPECT_EQ(reflection_draws, usize(2));
    EXPECT_EQ(primary_draws, usize(3));
    ASSERT_TRUE(last_reflection_index);
    ASSERT_TRUE(receiver_index);
    EXPECT_LT(*last_reflection_index, *receiver_index);
    auto plan = graph.textures.resourcePlan(graph.graph).unwrap();
    bool reflection_resource {};
    for (const auto& texture : plan.textures) {
        if (texture.request().name == "_rt_Reflection"_str) {
            reflection_resource = true;
            EXPECT_FALSE(texture.initialize_transparent);
        }
    }
    EXPECT_TRUE(reflection_resource);
}

TEST(SceneRenderGraph, ClearsReflectionWhenNoCasterDraws) {
    for (int mode = 0; mode < 3; ++mode) {
        owe::Scene scene;
        scene.RegisterRenderTarget(
            "_rt_default"_Str,
            owe::SceneRenderTarget { .width = i32(64), .height = i32(32), .withDepth = true });
        scene.EnablePlanarReflection();
        auto receiver = Arc<owe::SceneNode>::make();
        receiver->SetReflected(true);
        auto receiver_mesh = Arc<owe::SceneMesh>::make();
        auto material      = GraphMaterial(usize(1));
        material.name      = "receiver"_Str;
        material.textures.push("_rt_Reflection"_Str);
        receiver_mesh->AddMaterial(rstd::move(material));
        receiver_mesh->Submeshes().push(owe::SceneMesh::Submesh {});
        receiver->AddMesh(rstd::move(receiver_mesh));
        scene.RootMut()->AppendChild(rstd::move(receiver));
        if (mode != 0) {
            auto caster = Arc<owe::SceneNode>::make();
            caster->SetVisible(mode != 1);
            caster->SetReflected(mode != 2);
            auto mesh = Arc<owe::SceneMesh>::make();
            mesh->AddMaterial(GraphMaterial());
            mesh->Submeshes().push(owe::SceneMesh::Submesh {});
            caster->AddMesh(rstd::move(mesh));
            scene.RootMut()->AppendChild(rstd::move(caster));
        }
        auto graph = owe::BuildSceneRenderState(scene).unwrap();
        ASSERT_TRUE(graph.graph.compile().is_ok());
        Option<usize> clear_index, receiver_index;
        auto          order = graph.graph.inspect().order.clone();
        for (usize index {}; index < order.len(); ++index) {
            const auto& pass = graph.graph.passes()[usize(order[index])];
            if (pass.name == "reflection/clear"_str) clear_index = Some(index);
            if (pass.name == "receiver"_str) receiver_index = Some(index);
        }
        ASSERT_TRUE(clear_index);
        ASSERT_TRUE(receiver_index);
        EXPECT_LT(*clear_index, *receiver_index);
        for (const auto& draw : graph.draws)
            EXPECT_EQ(draw.source->parameters().render_view, owe::SceneRenderViewKind::Primary);
        auto plan = graph.textures.resourcePlan(graph.graph).unwrap();
        bool reflection_write {}, reflection_read {};
        for (const auto& texture : plan.textures) {
            if (texture.request().name != "_rt_Reflection"_str) continue;
            EXPECT_FALSE(texture.initialize_transparent);
            reflection_write = reflection_write ||
                               texture.access == owe::resource::ResourceAccess::Write ||
                               texture.access == owe::resource::ResourceAccess::ReadWrite;
            reflection_read  = reflection_read ||
                               texture.access == owe::resource::ResourceAccess::Read ||
                               texture.access == owe::resource::ResourceAccess::ReadWrite;
        }
        EXPECT_TRUE(reflection_write);
        EXPECT_TRUE(reflection_read);
    }
}

TEST(SceneRenderGraph, GroupsPerspectiveDepthDrawsWithoutCrossingOtherDraws) {
    owe::Scene scene;
    scene.RegisterRenderTarget(
        "_rt_default"_Str,
        owe::SceneRenderTarget { .width = i32(1280), .height = i32(720), .withDepth = true });
    scene.RegisterCamera("perspective"_Str,
                         Arc<owe::SceneCamera>::make(
                             owe::SceneCamera::MakePerspective(16.0 / 9.0, 0.1, 1000.0, 50.0)));
    ASSERT_TRUE(scene.SetActiveCamera("perspective"_str));
    auto add = [&](ref<str> name, bool depth_test, bool depth_write) {
        auto node     = Arc<owe::SceneNode>::make();
        auto mesh     = Arc<owe::SceneMesh>::make();
        auto material = GraphMaterial();
        material.name = String::make(name);
        material.SetBlendMode(depth_write ? owe::BlendMode::Disable : owe::BlendMode::Translucent);
        material.SetDepthTest(depth_test);
        material.SetDepthWrite(depth_write);
        mesh->AddMaterial(rstd::move(material));
        mesh->Submeshes().push(owe::SceneMesh::Submesh {});
        node->AddMesh(rstd::move(mesh));
        scene.RootMut()->AppendChild(rstd::move(node));
    };
    add("opaque-first"_str, true, true);
    add("transparent-first"_str, true, false);
    add("opaque-second"_str, true, true);
    add("transparent-second"_str, true, false);
    add("overlay"_str, false, false);
    add("opaque-after-overlay"_str, true, true);

    auto graph = owe::BuildSceneRenderState(scene).unwrap();
    ASSERT_TRUE(graph.graph.compile().is_ok());
    auto        order = graph.graph.inspect().order.clone();
    Vec<String> names;
    for (auto id : order) {
        auto* pass = &graph.graph.passes()[usize(id)];
        if (! pass->name.is_empty() && pass->name != "frame/clear"_str)
            names.push(pass->name.clone());
    }
    ASSERT_EQ(names.len(), usize(6));
    EXPECT_EQ(names[usize(0)], "opaque-first"_str);
    EXPECT_EQ(names[usize(1)], "opaque-second"_str);
    EXPECT_EQ(names[usize(2)], "transparent-first"_str);
    EXPECT_EQ(names[usize(3)], "transparent-second"_str);
    EXPECT_EQ(names[usize(4)], "overlay"_str);
    EXPECT_EQ(names[usize(5)], "opaque-after-overlay"_str);

    scene.RegisterCamera(
        "orthographic"_Str,
        Arc<owe::SceneCamera>::make(owe::SceneCamera::MakeOrthographic(1280, 720, -1, 1)));
    (void)scene.ConsumeRenderGraphDirty();
    ASSERT_TRUE(scene.SetActiveCamera("orthographic"_str));
    EXPECT_TRUE(scene.ConsumeRenderGraphDirty());
    auto ortho_graph = owe::BuildSceneRenderState(scene).unwrap();
    ASSERT_TRUE(ortho_graph.graph.compile().is_ok());
    names.clear();
    for (auto id : ortho_graph.graph.inspect().order) {
        auto* pass = &ortho_graph.graph.passes()[usize(id)];
        if (! pass->name.is_empty() && pass->name != "frame/clear"_str)
            names.push(pass->name.clone());
    }
    ASSERT_EQ(names.len(), usize(6));
    EXPECT_EQ(names[usize(0)], "opaque-first"_str);
    EXPECT_EQ(names[usize(1)], "transparent-first"_str);
    EXPECT_EQ(names[usize(2)], "opaque-second"_str);
    EXPECT_EQ(names[usize(3)], "transparent-second"_str);
}

TEST(SceneRenderGraph, SortsBlendedDepthDrawsBackToFrontAndTracksChanges) {
    owe::Scene scene;
    scene.RegisterRenderTarget(
        "_rt_default"_Str,
        owe::SceneRenderTarget { .width = i32(1280), .height = i32(720), .withDepth = true });
    auto camera = Arc<owe::SceneCamera>::make(
        owe::SceneCamera::MakePerspective(16.0 / 9.0, 0.1, 1000.0, 50.0));
    camera->SetLookAt({ 0.0, 0.0, 9.0 }, { 0.0, 0.0, 0.0 }, Eigen::Vector3d::UnitY());
    scene.RegisterCamera("perspective"_Str, camera.clone());
    ASSERT_TRUE(scene.SetActiveCamera("perspective"_str));
    auto add = [&](ref<str> name, float z) {
        auto node = Arc<owe::SceneNode>::make();
        node->SetTranslate({ 0.0f, 0.0f, z });
        auto mesh     = Arc<owe::SceneMesh>::make();
        auto material = GraphMaterial();
        material.name = String::make(name);
        material.SetBlendMode(owe::BlendMode::Translucent);
        material.SetDepthTest(true);
        material.SetDepthWrite(false);
        mesh->AddMaterial(rstd::move(material));
        mesh->Submeshes().push(owe::SceneMesh::Submesh {});
        node->AddMesh(rstd::move(mesh));
        scene.RootMut()->AppendChild(node.clone());
        return node;
    };
    auto arrow      = add("arrow"_str, 1.0f);
    auto background = add("background"_str, -30.0f);
    auto peer       = add("peer"_str, 1.0f);
    auto graph      = owe::BuildSceneRenderState(scene).unwrap();
    ASSERT_TRUE(graph.graph.compile().is_ok());
    Vec<String> names;
    for (auto id : graph.graph.inspect().order) {
        const auto& pass = graph.graph.passes()[usize(id)];
        if (pass.name == "arrow"_str || pass.name == "background"_str || pass.name == "peer"_str)
            names.push(pass.name.clone());
    }
    ASSERT_EQ(names.len(), usize(3));
    EXPECT_EQ(names[usize()], "background"_str);
    EXPECT_EQ(names[usize(1)], "arrow"_str);
    EXPECT_EQ(names[usize(2)], "peer"_str);
    ASSERT_EQ(graph.depth_orders.len(), usize(1));
    const auto& order = graph.depth_orders[usize()];
    EXPECT_TRUE(order.Matches(scene));
    arrow->SetTranslate({ 2.0f, 0.0f, 1.0f });
    EXPECT_TRUE(order.Matches(scene));
    background->SetTranslate({ 0.0f, 0.0f, 2.0f });
    EXPECT_FALSE(order.Matches(scene));
    background->SetTranslate({ 0.0f, 0.0f, -30.0f });
    camera->SetLookAt({ 0.0, 0.0, -40.0 }, { 0.0, 0.0, 0.0 }, Eigen::Vector3d::UnitY());
    EXPECT_FALSE(order.Matches(scene));
}

TEST(SceneRenderGraph, DoesNotReorderAcrossFramebufferSampling) {
    owe::Scene scene;
    scene.RegisterRenderTarget(
        "_rt_default"_Str,
        owe::SceneRenderTarget { .width = i32(1280), .height = i32(720), .withDepth = true });
    scene.RegisterCamera("perspective"_Str,
                         Arc<owe::SceneCamera>::make(
                             owe::SceneCamera::MakePerspective(16.0 / 9.0, 0.1, 1000.0, 50.0)));
    ASSERT_TRUE(scene.SetActiveCamera("perspective"_str));
    auto add = [&](ref<str> name, bool writes, bool samples_framebuffer) {
        auto node     = Arc<owe::SceneNode>::make();
        auto mesh     = Arc<owe::SceneMesh>::make();
        auto material = GraphMaterial(samples_framebuffer ? usize(1) : usize());
        material.name = String::make(name);
        material.SetDepthTest(true);
        material.SetDepthWrite(writes);
        material.SetBlendMode(writes ? owe::BlendMode::Disable : owe::BlendMode::Translucent);
        if (samples_framebuffer) material.textures.push("_rt_default"_Str);
        mesh->AddMaterial(rstd::move(material));
        mesh->Submeshes().push(owe::SceneMesh::Submesh {});
        node->AddMesh(rstd::move(mesh));
        scene.RootMut()->AppendChild(rstd::move(node));
    };
    add("transparent-before"_str, false, false);
    add("framebuffer-reader"_str, false, true);
    add("opaque-after"_str, true, false);
    auto graph = owe::BuildSceneRenderState(scene).unwrap();
    ASSERT_TRUE(graph.graph.compile().is_ok());
    Vec<String> names;
    for (auto id : graph.graph.inspect().order) {
        auto* pass = &graph.graph.passes()[usize(id)];
        if (pass->name == "transparent-before"_str || pass->name == "framebuffer-reader"_str ||
            pass->name == "opaque-after"_str)
            names.push(pass->name.clone());
    }
    ASSERT_EQ(names.len(), usize(3));
    EXPECT_EQ(names[usize(0)], "transparent-before"_str);
    EXPECT_EQ(names[usize(1)], "framebuffer-reader"_str);
    EXPECT_EQ(names[usize(2)], "opaque-after"_str);
}

TEST(SceneRenderGraph, OrdersShadowAtlasWriterBeforeReceiver) {
    owe::Scene scene;
    scene.RegisterRenderTarget(String::make("_rt_default"_str),
                               owe::SceneRenderTarget { .width = i32(1280), .height = i32(720) });
    scene.RegisterRenderTarget(String::make("_rt_shadowAtlas"_str),
                               owe::SceneRenderTarget {
                                   .width  = i32(768),
                                   .height = i32(256),
                                   .kind   = owe::SceneRenderTargetKind::DepthSampled,
                               });
    auto viewports = Vec<owe::SceneShadowViewport>::make();
    viewports.push(owe::SceneShadowViewport {
        .width  = rstd::f32(256.0f),
        .height = rstd::f32(256.0f),
    });
    scene.RegisterShadowDefinition(owe::SceneShadowDefinition {
        .target    = String::make("_rt_shadowAtlas"_str),
        .viewports = rstd::move(viewports),
    });

    auto caster                             = Arc<owe::SceneNode>::make();
    caster->shadow.cast                     = true;
    auto caster_mesh                        = Arc<owe::SceneMesh>::make();
    auto caster_material                    = GraphMaterial();
    caster_material.name                    = "caster-main"_Str;
    caster_material.shadow_variant          = Some(Arc<owe::SceneMaterial>::make(GraphMaterial()));
    (*caster_material.shadow_variant)->name = "caster-shadow"_Str;
    caster_mesh->AddMaterial(rstd::move(caster_material));
    caster_mesh->Submeshes().push(owe::SceneMesh::Submesh {});
    caster->AddMesh(rstd::move(caster_mesh));
    scene.RootMut()->AppendChild(caster.clone());

    auto receiver          = Arc<owe::SceneNode>::make();
    auto receiver_mesh     = Arc<owe::SceneMesh>::make();
    auto receiver_material = GraphMaterial(usize(1));
    receiver_material.name = "receiver"_Str;
    receiver_material.textures.clear();
    receiver_material.textures.push("_rt_shadowAtlas"_Str);
    receiver_mesh->AddMaterial(rstd::move(receiver_material));
    receiver_mesh->Submeshes().push(owe::SceneMesh::Submesh {});
    receiver->AddMesh(rstd::move(receiver_mesh));
    scene.RootMut()->AppendChild(receiver.clone());

    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
    ASSERT_EQ(snapshot.ShadowCasters().len(), usize(1));
    auto graph = owe::BuildSceneRenderState(scene, snapshot);
    ASSERT_TRUE(graph.is_ok());
    auto ordered = graph->graph.compile();
    ASSERT_TRUE(ordered.is_ok());
    auto order = graph->graph.inspect().order.clone();

    Option<usize> shadow_index;
    Option<usize> receiver_index;
    for (usize index {}; index < order.len(); ++index) {
        auto pass = &graph->graph.passes()[usize(order[index])];
        ASSERT_NE(pass, nullptr);
        if (pass->name == "caster-shadow"_str) shadow_index = Some(index);
        if (pass->name == "receiver"_str) receiver_index = Some(index);
    }
    ASSERT_TRUE(shadow_index.is_some());
    ASSERT_TRUE(receiver_index.is_some());
    EXPECT_LT(*shadow_index, *receiver_index);

    auto plan = graph->textures.resourcePlan(graph->graph).unwrap();
    bool atlas_write { false };
    bool atlas_read { false };
    for (const auto& texture : plan.textures) {
        if (texture.request().name != "_rt_shadowAtlas"_str) continue;
        atlas_write = atlas_write || texture.access == owe::resource::ResourceAccess::Write ||
                      texture.access == owe::resource::ResourceAccess::ReadWrite;
        atlas_read  = atlas_read || texture.access == owe::resource::ResourceAccess::Read ||
                      texture.access == owe::resource::ResourceAccess::ReadWrite;
    }
    EXPECT_TRUE(atlas_write);
    EXPECT_TRUE(atlas_read);
}

TEST(SceneRenderGraph, SamplesCurrentDepthAndSupportsDepthOnlyPostProcess) {
    owe::Scene scene;
    scene.RegisterRenderTarget(
        "_rt_default"_Str,
        owe::SceneRenderTarget { .width = i32(1920), .height = i32(1080), .withDepth = true });
    scene.RegisterRenderTarget(
        "_rt_depth"_Str,
        owe::SceneRenderTarget { .width       = i32(240),
                                 .height      = i32(135),
                                 .kind        = owe::SceneRenderTargetKind::DepthSampled,
                                 .force_clear = true });
    auto pp                        = Box<owe::ScenePostProcess>::make();
    pp->name                       = "depth-consumer"_Str;
    pp->single_sample_depth_target = Some("_rt_default"_Str);
    auto add                       = [&](owe::SceneMaterial material, ref<str> output) {
        auto node = Arc<owe::SceneNode>::make();
        auto mesh = Arc<owe::SceneMesh>::make();
        mesh->AddMaterial(rstd::move(material));
        mesh->Submeshes().push(owe::SceneMesh::Submesh {});
        node->AddMesh(rstd::move(mesh));
        pp->steps.push(owe::ScenePostProcessStep::Pass(
            owe::ScenePostProcessPass { node.clone(), String::make(output) }));
    };
    auto back = GraphMaterial();
    back.SetDepthWrite(true);
    back.SetDepthTest(true);
    add(rstd::move(back), "_rt_depth"_str);
    auto consumer = GraphMaterial(usize(2));
    consumer.textures.push("_rt_default"_Str);
    consumer.textures.push("_rt_depth"_Str);
    ASSERT_TRUE(scene.SetMaterialDepthAttachmentSource(consumer, u32(), "_rt_default"_str));
    add(rstd::move(consumer), "_rt_default"_str);
    (void)scene.RegisterPostProcess(rstd::move(pp));
    auto graph = owe::BuildSceneRenderState(scene);
    ASSERT_TRUE(graph);
    ASSERT_TRUE(graph->graph.compile());
    ASSERT_EQ(graph->draws.len(), usize(2));
    EXPECT_TRUE(graph->draws[usize()].source->parameters().depth_only);
    EXPECT_TRUE(graph->draws[usize()].source->parameters().output_use.is_none());
    const auto& bindings = graph->draws[usize(1)].source->parameters().texture_bindings;
    ASSERT_EQ(bindings.len(), usize(2));
    ASSERT_TRUE(bindings[usize()].request);
    EXPECT_EQ(bindings[usize()].request->kind, owe::resource::TextureRequestKind::DepthAttachment);
    auto plan    = graph->textures.resourcePlan(graph->graph).unwrap();
    bool sampled = false;
    for (const auto& texture : plan.textures) {
        if (texture.request().kind != owe::resource::TextureRequestKind::DepthAttachment) continue;
        if (texture.access == owe::resource::ResourceAccess::Read ||
            texture.access == owe::resource::ResourceAccess::ReadWrite)
            sampled = true;
        EXPECT_TRUE(owe::resource::HasTextureUsage(texture.request().definition->usage,
                                                   owe::resource::TextureUsage::Sampled));
    }
    EXPECT_TRUE(sampled);
    EXPECT_TRUE(scene.ResizeRenderTarget("_rt_default"_str, i32(1280), i32(720)));
    (void)graph->draws[usize(1)].source->finalizeResourceRequests(scene);
    const auto& refreshed =
        graph->draws[usize(1)].source->parameters().texture_bindings[usize()].request;
    ASSERT_TRUE(refreshed);
    EXPECT_EQ(refreshed->kind, owe::resource::TextureRequestKind::DepthAttachment);
    EXPECT_EQ(refreshed->definition->width, i32(1280));
    scene.RenderTargetMut("_rt_default"_str).unwrap()->sample_count = 4;
    auto multisampled = owe::BuildSceneRenderState(scene);
    ASSERT_TRUE(multisampled);
    EXPECT_TRUE(multisampled->draws.is_empty());
    EXPECT_TRUE(multisampled->graph.compile());
}

TEST(SceneRenderGraph, InitializesOnlyThePreviousFrameInColorFeedback) {
    owe::Scene scene;
    for (auto key : { "_rt_default"_str, "_rt_force"_str, "_rt_state"_str })
        scene.RegisterRenderTarget(
            String::make(key),
            owe::SceneRenderTarget { .width = i32(32), .height = i32(32), .allowReuse = true });
    auto append = [&](ref<str> name, ref<str> input, ref<str> output) {
        auto node     = Arc<owe::SceneNode>::make();
        auto mesh     = Arc<owe::SceneMesh>::make();
        auto material = GraphMaterial(usize(1));
        material.name = String::make(name);
        material.textures.push(String::make(input));
        mesh->AddMaterial(rstd::move(material));
        owe::SceneMesh::Submesh submesh;
        submesh.output_override = String::make(output);
        mesh->Submeshes().push(rstd::move(submesh));
        node->AddMesh(rstd::move(mesh));
        scene.RootMut()->AppendChild(rstd::move(node));
    };
    append("force"_str, "_rt_state"_str, "_rt_force"_str);
    append("simulate"_str, "_rt_force"_str, "_rt_state"_str);
    append("combine"_str, "_rt_state"_str, "_rt_default"_str);
    auto  snapshot = owe::ExtractRenderSceneSnapshot(scene);
    auto  graph    = owe::BuildSceneRenderState(scene, snapshot).unwrap();
    auto  plan     = graph.textures.resourcePlan(graph.graph).unwrap();
    usize history_versions {};
    for (const auto& entry : plan.textures) {
        const bool history = entry.request().name == "_rt_state"_str;
        EXPECT_EQ(entry.initialize_transparent, history);
        if (! history) continue;
        ++history_versions;
        EXPECT_TRUE(entry.preserve_contents);
        auto authored = scene.TargetResource("_rt_state"_str).unwrap();
        EXPECT_EQ(entry.definition.id(), authored.id());
        EXPECT_EQ(entry.definition.revision(), authored.revision());
    }
    EXPECT_EQ(history_versions, usize(2));
}

TEST(SceneRenderGraph, SharesOneLinkTargetForMultipleConsumers) {
    owe::Scene scene;
    scene.SetOrtho({ rstd::i32(1920), rstd::i32(1080) });
    scene.RegisterRenderTarget(String::make("_rt_default"_str),
                               owe::SceneRenderTarget { .width = i32(1920), .height = i32(1080) });

    auto source  = rstd::sync::Arc<owe::SceneNode>::make();
    source->ID() = rstd::i32(7);
    source->SetSize({ 64.0f, 32.0f });
    auto source_mesh     = Arc<owe::SceneMesh>::make();
    auto source_material = GraphMaterial();
    source_material.name = "source"_Str;
    source_mesh->AddMaterial(std::move(source_material));
    source_mesh->Submeshes().push(owe::SceneMesh::Submesh {});
    source->AddMesh(std::move(source_mesh));
    scene.RootMut()->AppendChild(source.clone());
    scene.RegisterLayerLinkSource(owe::WallpaperLayerId { .value = rstd::i32(7) }, *source);
    scene.MarkLayerStaticElidable(owe::WallpaperLayerId { .value = rstd::i32(7) });

    auto consumer          = rstd::sync::Arc<owe::SceneNode>::make();
    consumer->ID()         = rstd::i32(42);
    auto consumer_mesh     = Arc<owe::SceneMesh>::make();
    auto consumer_material = GraphMaterial(usize(2));
    consumer_material.name = "consumer"_Str;
    consumer_material.textures.clear();
    consumer_material.textures.push("_rt_link_7"_Str);
    consumer_material.textures.push("_rt_link_7"_Str);
    consumer_mesh->AddMaterial(std::move(consumer_material));
    consumer_mesh->Submeshes().push(owe::SceneMesh::Submesh {});
    consumer->AddMesh(std::move(consumer_mesh));
    scene.RootMut()->AppendChild(consumer.clone());

    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
    auto graph    = owe::BuildSceneRenderState(scene, snapshot);
    ASSERT_TRUE(graph.is_ok());
    auto ordered = graph->graph.compile();
    ASSERT_TRUE(ordered.is_ok());
    auto order = graph->graph.inspect().order.clone();

    EXPECT_EQ(graph->draws.len(), rstd::usize(2));
    rstd::usize copy_count {};
    for (const auto& desc : graph->descriptions)
        if (desc.execution().kind == vrento::vulkan::texture_copy_kind) ++copy_count;
    EXPECT_EQ(copy_count, rstd::usize());
    auto        plan = graph->textures.resourcePlan(graph->graph).unwrap();
    rstd::usize link_versions {};
    for (const auto& entry : plan.textures) {
        if (entry.request().name != "_rt_link_7"_str) continue;
        ++link_versions;
        EXPECT_EQ(entry.version, rstd::u32(1));
        EXPECT_EQ(entry.request().content &
                      owe::resource::TextureContentFlag(
                          owe::resource::TextureContent::PreserveAcrossFrames),
                  rstd::u32());
    }
    EXPECT_EQ(link_versions, rstd::usize(1));
}

TEST(SceneRenderGraph, LinkedSurfaceReadsFollowSceneOrder) {
    owe::Scene scene;
    scene.SetOrtho({ rstd::i32(1920), rstd::i32(1080) });
    scene.RegisterRenderTarget(String::make("_rt_default"_str),
                               owe::SceneRenderTarget { .width = i32(1920), .height = i32(1080) });

    auto make_consumer = [](std::string name) {
        auto node     = Arc<owe::SceneNode>::make();
        auto mesh     = Arc<owe::SceneMesh>::make();
        auto material = GraphMaterial(usize(1));
        material.name = rstd::into(rstd::cppstd::as_str(name).unwrap());
        material.textures.clear();
        material.textures.push("_rt_link_7"_Str);
        mesh->AddMaterial(std::move(material));
        mesh->Submeshes().push(owe::SceneMesh::Submesh {});
        node->AddMesh(std::move(mesh));
        return node;
    };

    auto before = make_consumer("before");
    scene.RootMut()->AppendChild(rstd::move(before));

    auto source = Arc<owe::SceneNode>::make();
    source->SetSize({ 64.0f, 32.0f });
    auto source_mesh     = Arc<owe::SceneMesh>::make();
    auto source_material = GraphMaterial();
    source_material.name = "source"_Str;
    source_mesh->AddMaterial(std::move(source_material));
    source_mesh->Submeshes().push(owe::SceneMesh::Submesh {});
    source->AddMesh(std::move(source_mesh));
    scene.RootMut()->AppendChild(source.clone());
    scene.RegisterLayerLinkSource(owe::WallpaperLayerId { .value = rstd::i32(7) }, *source);
    scene.MarkLayerStaticElidable(owe::WallpaperLayerId { .value = rstd::i32(7) });

    auto after = make_consumer("after");
    scene.RootMut()->AppendChild(rstd::move(after));

    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
    auto graph    = owe::BuildSceneRenderState(scene, snapshot);
    ASSERT_TRUE(graph.is_ok());
    auto ordered = graph->graph.compile();
    ASSERT_TRUE(ordered.is_ok());

    auto        plan = graph->textures.resourcePlan(graph->graph).unwrap();
    rstd::usize link_versions {};
    String      link_allocation;
    for (const auto& entry : plan.textures) {
        if (entry.request().name != "_rt_link_7"_str) continue;
        ++link_versions;
        if (link_allocation.is_empty()) link_allocation = entry.allocation_key.clone();
        EXPECT_EQ(entry.allocation_key, link_allocation);
        EXPECT_LE(entry.version, rstd::u32(1));
        EXPECT_EQ(entry.request().lifetime, owe::resource::TextureLifetimeClass::Retained);
        EXPECT_TRUE(entry.preserve_contents);

        EXPECT_NE(entry.request().content &
                      owe::resource::TextureContentFlag(
                          owe::resource::TextureContent::InitializeTransparent),
                  rstd::u32());
    }
    EXPECT_EQ(link_versions, rstd::usize(2));

    bool source_emitted {};
    bool before_reads_history {};
    bool after_reads_current {};
    auto ordered_passes = graph->graph.inspect().order.clone();
    for (auto handle : ordered_passes) {
        auto state = &graph->graph.passes()[usize(handle)];
        ASSERT_NE(state, nullptr);
        if (state->name == "source"_str) {
            source_emitted = true;
            continue;
        }
        if (state->name != "before"_str && state->name != "after"_str) continue;
        EXPECT_EQ(source_emitted, state->name == "after"_str);
        auto pass = graph->draw(state->id);
        ASSERT_TRUE(pass.is_some());
        auto uses = (*pass)->resourceUses();
        for (auto use : uses.textures) {
            for (const auto& entry : plan.textures) {
                if (entry.handle != use || entry.request().name != "_rt_link_7"_str) continue;
                before_reads_history |= state->name == "before"_str && entry.version == rstd::u32();
                after_reads_current |= state->name == "after"_str && entry.version == rstd::u32(1);
            }
        }
    }
    EXPECT_TRUE(before_reads_history);
    EXPECT_TRUE(after_reads_current);

    // A later composite source copies the framebuffer after the first consumer.
    source->Mesh()->Material()->textures.push("_rt_default"_Str);
    source->Mesh()->Material()->customShader.shader = GraphMaterial(usize(1)).customShader.shader;
    auto feedback_snapshot                          = owe::ExtractRenderSceneSnapshot(scene);
    auto feedback_graph = owe::BuildSceneRenderState(scene, feedback_snapshot);
    ASSERT_TRUE(feedback_graph.is_ok());
    auto feedback_order = feedback_graph->graph.compile();
    ASSERT_TRUE(feedback_order.is_ok());
    rstd::usize before_count {}, source_count {}, after_count {};
    for (auto handle : feedback_graph->graph.inspect().order) {
        auto state = &feedback_graph->graph.passes()[usize(handle)];
        ASSERT_NE(state, nullptr);
        if (state->name == "before"_str) {
            EXPECT_EQ(source_count, rstd::usize());
            ++before_count;
        } else if (state->name == "source"_str) {
            EXPECT_EQ(before_count, rstd::usize(1));
            ++source_count;
        } else if (state->name == "after"_str) {
            EXPECT_EQ(source_count, rstd::usize(1));
            ++after_count;
        }
    }
    EXPECT_EQ(before_count, rstd::usize(1));
    EXPECT_EQ(source_count, rstd::usize(1));
    EXPECT_EQ(after_count, rstd::usize(1));
}

TEST(SceneRenderGraph, ElidesSceneOwnedVisibilityHiddenSubtreeAndRestoresIt) {
    owe::Scene scene;
    scene.SetOrtho({ rstd::i32(1920), rstd::i32(1080) });
    scene.RegisterRenderTarget(String::make("_rt_default"_str),
                               owe::SceneRenderTarget { .width = i32(1920), .height = i32(1080) });

    auto parent   = Arc<owe::SceneNode>::make();
    parent->ID()  = rstd::i32(7);
    auto child    = Arc<owe::SceneNode>::make();
    child->ID()   = rstd::i32(8);
    auto mesh     = Arc<owe::SceneMesh>::make();
    auto material = GraphMaterial();
    material.name = "child"_Str;
    mesh->AddMaterial(std::move(material));
    mesh->Submeshes().push(owe::SceneMesh::Submesh {});
    child->AddMesh(std::move(mesh));
    parent->AppendChild(child.clone());
    scene.RootMut()->AppendChild(parent.clone());
    scene.RegisterNode(*parent);
    scene.RegisterNode(*child, Some(owe::WallpaperLayerId { .value = rstd::i32(8) }));
    EXPECT_TRUE(parent->WallpaperIdentity().is_none());

    EXPECT_TRUE(scene.SetNodeVisible(*parent, false));
    auto hidden_snapshot = owe::ExtractRenderSceneSnapshot(scene);
    auto hidden_graph    = owe::BuildSceneRenderState(scene, hidden_snapshot);
    ASSERT_TRUE(hidden_graph.is_ok());
    auto hidden_order = hidden_graph->graph.compile();
    ASSERT_TRUE(hidden_order.is_ok());
    EXPECT_TRUE(hidden_graph->draws.is_empty());

    EXPECT_TRUE(scene.SetNodeVisible(*parent, true));
    auto visible_snapshot = owe::ExtractRenderSceneSnapshot(scene);
    auto visible_graph    = owe::BuildSceneRenderState(scene, visible_snapshot);
    ASSERT_TRUE(visible_graph.is_ok());
    auto visible_order = visible_graph->graph.compile();
    ASSERT_TRUE(visible_order.is_ok());
    EXPECT_EQ(visible_graph->draws.len(), rstd::usize(1));
    EXPECT_NE(hidden_graph->graph.scope(), visible_graph->graph.scope());
    usize shared {};
    for (const auto& old : hidden_graph->textures.textures())
        for (const auto& current : visible_graph->textures.textures()) {
            if (old.spec.key != current.spec.key) continue;
            ++shared;
            EXPECT_EQ(old.spec.definition.id(), current.spec.definition.id());
            EXPECT_EQ(old.spec.definition.revision(), current.spec.definition.revision());
        }
    EXPECT_GT(shared, usize());
}

TEST(SceneRenderGraph, PreservesLinkedSourceBelowVisibilityHiddenParent) {
    owe::Scene scene;
    scene.SetOrtho({ rstd::i32(1920), rstd::i32(1080) });
    scene.RegisterRenderTarget(String::make("_rt_default"_str),
                               owe::SceneRenderTarget { .width = i32(1920), .height = i32(1080) });

    auto parent          = Arc<owe::SceneNode>::make();
    parent->ID()         = rstd::i32(7);
    auto parent_mesh     = Arc<owe::SceneMesh>::make();
    auto parent_material = GraphMaterial();
    parent_material.name = "hidden-parent"_Str;
    parent_mesh->AddMaterial(std::move(parent_material));
    parent_mesh->Submeshes().push(owe::SceneMesh::Submesh {});
    parent->AddMesh(std::move(parent_mesh));
    auto source          = Arc<owe::SceneNode>::make();
    source->ID()         = rstd::i32(8);
    auto source_mesh     = Arc<owe::SceneMesh>::make();
    auto source_material = GraphMaterial();
    source_material.name = "source"_Str;
    source_mesh->AddMaterial(std::move(source_material));
    source_mesh->Submeshes().push(owe::SceneMesh::Submesh {});
    source->AddMesh(std::move(source_mesh));
    auto source_child          = Arc<owe::SceneNode>::make();
    auto source_child_mesh     = Arc<owe::SceneMesh>::make();
    auto source_child_material = GraphMaterial();
    source_child_material.name = "source-child"_Str;
    source_child_mesh->AddMaterial(std::move(source_child_material));
    source_child_mesh->Submeshes().push(owe::SceneMesh::Submesh {});
    source_child->AddMesh(std::move(source_child_mesh));
    source->AppendChild(source_child.clone());
    parent->AppendChild(source.clone());
    scene.RootMut()->AppendChild(parent.clone());
    scene.RegisterNode(*parent, Some(owe::WallpaperLayerId { .value = rstd::i32(7) }));
    scene.RegisterLayerLinkSource(owe::WallpaperLayerId { .value = rstd::i32(8) }, *source);
    scene.MarkLayerStaticElidable(owe::WallpaperLayerId { .value = rstd::i32(8) });

    auto consumer          = Arc<owe::SceneNode>::make();
    consumer->ID()         = rstd::i32(42);
    auto consumer_mesh     = Arc<owe::SceneMesh>::make();
    auto consumer_material = GraphMaterial(usize(1));
    consumer_material.name = "consumer"_Str;
    consumer_material.textures.clear();
    consumer_material.textures.push("_rt_link_8"_Str);
    consumer_mesh->AddMaterial(std::move(consumer_material));
    consumer_mesh->Submeshes().push(owe::SceneMesh::Submesh {});
    consumer->AddMesh(std::move(consumer_mesh));
    scene.RootMut()->AppendChild(consumer.clone());

    EXPECT_TRUE(scene.SetNodeVisible(*parent, false));
    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
    auto graph    = owe::BuildSceneRenderState(scene, snapshot);
    ASSERT_TRUE(graph.is_ok());
    auto ordered = graph->graph.compile();
    ASSERT_TRUE(ordered.is_ok());
    EXPECT_EQ(graph->draws.len(), rstd::usize(3));
    auto plan                   = graph->textures.resourcePlan(graph->graph).unwrap();
    bool child_uses_link_target = false;
    for (auto handle : graph->graph.inspect().order) {
        auto& pass = graph->graph.passes()[usize(handle)];
        if (pass.name != "source-child"_str) continue;
        auto draw = graph->draw(pass.id);
        ASSERT_TRUE(draw.is_some());
        for (auto use : (*draw)->resourceUses().textures)
            for (const auto& texture : plan.textures)
                if (texture.handle == use && texture.request().name == "_rt_link_8"_str)
                    child_uses_link_target = true;
    }
    EXPECT_TRUE(child_uses_link_target);
    EXPECT_TRUE(scene.SetNodeVisible(*parent, true));
    auto visible_snapshot = owe::ExtractRenderSceneSnapshot(scene);
    auto visible_graph    = owe::BuildSceneRenderState(scene, visible_snapshot);
    ASSERT_TRUE(visible_graph.is_ok());
    EXPECT_TRUE(visible_graph->graph.compile().is_ok());
    EXPECT_EQ(visible_graph->draws.len(), rstd::usize(4));
}

TEST(VulkanRenderDiagnostics, EmptyBeforeProgramBuild) {
    owe::vulkan::VulkanRender render;
    EXPECT_TRUE(render.preparedPassDiagnostics().is_empty());
}

TEST(SceneRenderGraph, RejectsMissingOutputWithoutInstallingPartialProgram) {
    owe::Scene scene;
    auto       mesh = Arc<owe::SceneMesh>::make();
    mesh->AddMaterial(owe::SceneMaterial {});
    mesh->Submeshes().push(owe::SceneMesh::Submesh {});
    scene.RootMut()->AddMesh(rstd::move(mesh));
    auto result = owe::BuildSceneRenderState(scene);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.unwrap_err().kind, vrento::fg::ErrorKind::MissingResource);
}

TEST(SceneRenderGraph, ReusesDrawInterfacesWithoutRetainingGraphState) {
    owe::Scene scene;
    scene.RegisterRenderTarget("_rt_default"_Str,
                               owe::SceneRenderTarget { .width = i32(64), .height = i32(32) });
    auto node = Arc<owe::SceneNode>::make();
    auto mesh = Arc<owe::SceneMesh>::make();
    mesh->AddMaterial(GraphMaterial());
    mesh->Submeshes().push(owe::SceneMesh::Submesh {});
    node->AddMesh(mesh.clone());
    scene.RootMut()->AppendChild(node.clone());

    SceneDrawPreparationCache cache;
    auto                      build = [&] {
        auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
        return owe::BuildSceneRenderState(
                   scene,
                   snapshot,
                   nullptr,
                   Some(mut_ref<SceneDrawPreparationCache>::from_raw_parts(&cache)))
            .unwrap();
    };
    Option<SceneRenderStateInput> first = Some(build());
    ASSERT_EQ(first->draws.len(), usize(1));
    auto recipe = [](const SceneRenderStateInput& input) -> ref<vrento::vulkan::GraphicsDrawDesc> {
        for (const auto& description : input.descriptions)
            if (auto draw = description.get<vrento::vulkan::GraphicsDrawDesc>()) return *draw;
        rstd::panic { "missing draw description" };
    };
    auto original = recipe(*first);
    EXPECT_EQ(original->request.multisample.rasterizationSamples, VK_SAMPLE_COUNT_1_BIT);
    EXPECT_FALSE(original->request.has_depth_attachment);
    EXPECT_EQ(original->pipeline.generation, first->resource_plan.generation);
    auto initial = cache.stats();
    EXPECT_EQ(initial.misses, u64(1));

    auto target          = scene.RenderTargetMut("_rt_default"_str).unwrap();
    target->width        = i32(128);
    target->height       = i32(64);
    target->sample_count = 4;
    target->format       = owe::TextureFormat::RGBA16F;
    target->withDepth    = true;
    target->force_clear  = true;
    auto& material       = *mesh->MaterialSlots()[usize()];
    material.SetBlendMode(owe::BlendMode::Additive);
    material.SetDepthTest(true);
    material.SetDepthWrite(true);

    Option<SceneRenderStateInput> second = Some(build());
    ASSERT_TRUE(second->graph.compile());
    auto current = recipe(*second);
    EXPECT_EQ(cache.stats().misses, initial.misses);
    EXPECT_GT(cache.stats().hits, initial.hits);
    EXPECT_NE(first->graph.scope(), second->graph.scope());
    EXPECT_EQ(current->pipeline.generation, second->resource_plan.generation);
    EXPECT_NE(original->pipeline, current->pipeline);
    ASSERT_TRUE(current->layout);
    EXPECT_EQ(current->layout->pipeline, current->pipeline);
    EXPECT_FALSE(current->request.pipeline_layout.Valid());
    EXPECT_EQ(current->request.multisample.rasterizationSamples, VK_SAMPLE_COUNT_4_BIT);
    EXPECT_EQ(current->request.color_format, VK_FORMAT_R16G16B16A16_SFLOAT);
    EXPECT_EQ(current->request.color_load_op, VK_ATTACHMENT_LOAD_OP_CLEAR);
    EXPECT_TRUE(current->request.has_depth_attachment);
    EXPECT_TRUE(current->request.depth.depthTestEnable);
    EXPECT_TRUE(current->request.depth.depthWriteEnable);
    EXPECT_TRUE(current->request.color_blend.blendEnable);
    EXPECT_EQ(current->request.color_blend.dstColorBlendFactor, VK_BLEND_FACTOR_ONE);
    EXPECT_EQ(original->request.multisample.rasterizationSamples, VK_SAMPLE_COUNT_1_BIT);
    EXPECT_FALSE(original->request.has_depth_attachment);
    EXPECT_EQ(second->draws[usize()].source->parameters().output_request->definition->width,
              i32(128));
    cache.prune();
    EXPECT_EQ(cache.stats().entries, usize(1));
    first  = None();
    second = None();
    cache.prune();
    EXPECT_EQ(cache.stats().entries, usize());
}

TEST(SceneRenderGraph, SharesTransientSlotsAcrossSequentialLayersAndPreservesHistoryReads) {
    owe::Scene scene;
    scene.RegisterRenderTarget("_rt_default"_Str,
                               owe::SceneRenderTarget { .width = i32(1920), .height = i32(1080) });
    scene.RegisterRenderTarget("_rt_layer_a"_Str,
                               owe::SceneRenderTarget { .width                = i32(1024),
                                                        .height               = i32(512),
                                                        .allowReuse           = true,
                                                        .clear_on_first_write = true });
    scene.RegisterRenderTarget("_rt_layer_b"_Str,
                               owe::SceneRenderTarget { .width                = i32(1024),
                                                        .height               = i32(512),
                                                        .allowReuse           = true,
                                                        .clear_on_first_write = true });
    scene.RegisterRenderTarget(
        "_rt_history_fbo"_Str,
        owe::SceneRenderTarget { .width = i32(1024), .height = i32(512), .allowReuse = true });

    auto add_layer = [&](ref<str> composite_name, ref<str> label) {
        auto node = Arc<owe::SceneNode>::make();
        auto mesh = Arc<owe::SceneMesh>::make();
        auto mat  = GraphMaterial();
        mat.name  = rstd::format("{}-src", label);
        mesh->AddMaterial(rstd::move(mat));
        mesh->Submeshes().push(owe::SceneMesh::Submesh {});
        node->AddMesh(rstd::move(mesh));

        auto layer = Arc<owe::SceneNodeLayer>::make(node.as_ptr(), 1024.0f, 512.0f, composite_name);
        auto eff   = Arc<owe::SceneImageEffect>::make();
        eff->name  = rstd::format("{}-eff", label);
        auto eff_node = Arc<owe::SceneNode>::make();
        auto eff_mesh = Arc<owe::SceneMesh>::make();
        auto eff_mat  = GraphMaterial(usize(1));
        eff_mat.name  = rstd::format("{}-final", label);
        eff_mat.textures.clear();
        eff_mat.textures.push(String::make(composite_name));
        eff_mesh->AddMaterial(rstd::move(eff_mat));
        eff_mesh->Submeshes().push(owe::SceneMesh::Submesh {});
        eff_node->AddMesh(rstd::move(eff_mesh));
        eff->AddNode(owe::SceneImageEffectNode {
            .output    = owe::SceneEffectTarget::LayerNext(),
            .sceneNode = rstd::move(eff_node),
        });
        layer->AddEffect(rstd::move(eff));
        node->AttachLayer(rstd::move(layer));
        scene.RootMut()->AppendChild(rstd::move(node));
    };

    add_layer("_rt_layer_a"_str, "a"_str);
    add_layer("_rt_layer_b"_str, "b"_str);

    // Read _rt_history_fbo before writing it in the same frame; it must stay frame-preserved
    // and never join the shared transient allocation family.
    {
        auto reader      = Arc<owe::SceneNode>::make();
        auto reader_mesh = Arc<owe::SceneMesh>::make();
        auto reader_mat  = GraphMaterial(usize(1));
        reader_mat.name  = "history-reader"_Str;
        reader_mat.textures.clear();
        reader_mat.textures.push("_rt_history_fbo"_Str);
        reader_mesh->AddMaterial(rstd::move(reader_mat));
        reader_mesh->Submeshes().push(owe::SceneMesh::Submesh {});
        reader->AddMesh(rstd::move(reader_mesh));
        scene.RootMut()->AppendChild(rstd::move(reader));

        auto writer      = Arc<owe::SceneNode>::make();
        auto writer_mesh = Arc<owe::SceneMesh>::make();
        auto writer_mat  = GraphMaterial();
        writer_mat.name  = "history-writer"_Str;
        writer_mesh->AddMaterial(rstd::move(writer_mat));
        owe::SceneMesh::Submesh submesh;
        submesh.output_override = "_rt_history_fbo"_Str;
        writer_mesh->Submeshes().push(rstd::move(submesh));
        writer->AddMesh(rstd::move(writer_mesh));
        scene.RootMut()->AppendChild(rstd::move(writer));
    }

    auto built = owe::BuildSceneRenderState(scene);
    ASSERT_TRUE(built.is_ok());
    ASSERT_TRUE(built->graph.compile().is_ok());
    auto plan = built->textures.resourcePlan(built->graph).unwrap();

    String key_a, key_b;
    usize  history_entries {};
    for (const auto& entry : plan.textures) {
        if (entry.request().name == "_rt_layer_a"_str) key_a = entry.allocation_key.clone();
        if (entry.request().name == "_rt_layer_b"_str) key_b = entry.allocation_key.clone();
        if (entry.request().name == "_rt_history_fbo"_str) {
            ++history_entries;
            EXPECT_TRUE(entry.preserve_contents);
            EXPECT_TRUE(entry.allocation_key.is_empty());
        }
    }
    EXPECT_FALSE(key_a.is_empty());
    EXPECT_EQ(key_a, key_b);
    EXPECT_EQ(history_entries, usize(2));
}
