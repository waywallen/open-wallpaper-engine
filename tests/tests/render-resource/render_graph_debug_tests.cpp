#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>

import rstd;
import rstd.cppstd;
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
    auto shape = vrento::texture::describe_image(*original->content).unwrap()[usize()];
    fg::FrameGraphBuilder builder(77);
    SceneRenderTextures   textures(u64(77));
    auto             version = textures
                                   .declare(builder,
                                            { .key        = "asset"_Str,
                                              .imported   = true,
                                              .definition = MakeImportedTextureRequest("asset"_str),
                                              .metadata   = Some(shape) })
                                   .unwrap();
    SceneRenderState state;
    ASSERT_TRUE(state
                    .install(SceneRenderStateInput { rstd::move(builder).finish().unwrap(),
                                                     rstd::move(textures) })
                    .is_ok());
    auto replacement     = Arc<owe::Image>::make();
    replacement->content = MakeMissingTexturePlaceholder("asset"_str);
    scene.RegisterRuntimeImage("asset"_Str, replacement.clone());
    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
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
