#include <rstd/test/gtest.hpp>

import rstd;
import rstd.cppstd;
import wescene.resource;
import wescene.rgraph;
import wescene.scene;
import wescene.vulkan_render;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;

namespace
{

class DebugPass : public owe::rg::Pass {
public:
    struct Desc {};
    explicit DebugPass(const Desc&) {}
};

struct PassPrefix {
    virtual ~PassPrefix() = default;
    int marker { 73 };
};

class alignas(128) OwnedPass : public PassPrefix, public owe::rg::Pass {
public:
    struct Desc {
        int* destroyed {};
    };

    explicit OwnedPass(const Desc& desc): destroyed(desc.destroyed) {}
    ~OwnedPass() override { ++*destroyed; }

private:
    int* destroyed;
};

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

auto CompositeDesc() -> owe::rg::TextureDesc {
    return owe::rg::TextureDesc {
        .name              = String::make("layer-composite"_str),
        .key               = String::make("layer-composite"_str),
        .kind              = owe::rg::TextureKind::Temp,
        .request           = rstd::Some(owe::resource::TextureRequest {
            .kind       = owe::resource::TextureRequestKind::RenderTarget,
            .name       = String::make("layer-composite"_str),
            .definition = rstd::Some(owe::resource::TextureDefinition {
                .width  = rstd::i32(1920),
                .height = rstd::i32(1080),
            }),
            .lifetime   = owe::resource::TextureLifetimeClass::FrameLocal,
        }),
        .allocation_family = rstd::Some(String::make("layer-composite"_str)),
    };
}

} // namespace

TEST(RenderGraphOwnership, PreservesConcreteObjectsAcrossGrowthAndMoves) {
    int destroyed          = 0;
    int replaced_destroyed = 0;
    {
        owe::rg::RenderGraph     graph;
        Vec<owe::rg::PassHandle> handles;
        Vec<OwnedPass*>          addresses;
        for (int i = 0; i < 128; ++i) {
            auto node =
                graph.addPass<OwnedPass>("owned"_str,
                                         owe::rg::PassNode::Type::CustomShader,
                                         [&](owe::rg::RenderGraphBuilder&, OwnedPass::Desc& desc) {
                                             desc.destroyed = &destroyed;
                                         });
            auto state = graph.passState(node);
            ASSERT_TRUE(state.is_some());
            auto pass = graph.getPass(state->pass);
            ASSERT_TRUE(pass.is_some());
            auto* concrete = dynamic_cast<OwnedPass*>(&*pass);
            ASSERT_NE(concrete, nullptr);
            EXPECT_EQ(reinterpret_cast<rstd::uintptr_t>(concrete) % alignof(OwnedPass), 0u);
            EXPECT_EQ(concrete->marker, 73);
            handles.push(owe::rg::PassHandle(state->pass));
            addresses.push(rstd::move(concrete));
        }
        EXPECT_EQ(destroyed, 0);
        owe::rg::RenderGraph moved(rstd::move(graph));
        owe::rg::RenderGraph assigned;
        assigned.addPass<OwnedPass>("replaced"_str,
                                    owe::rg::PassNode::Type::CustomShader,
                                    [&](owe::rg::RenderGraphBuilder&, OwnedPass::Desc& desc) {
                                        desc.destroyed = &replaced_destroyed;
                                    });
        assigned = rstd::move(moved);
        EXPECT_EQ(replaced_destroyed, 1);
        const auto& borrowed = assigned;
        for (usize i {}; i < handles.len(); ++i) {
            auto pass = borrowed.getPass(handles[i]);
            ASSERT_TRUE(pass.is_some());
            EXPECT_EQ(dynamic_cast<const OwnedPass*>(&*pass), addresses[i]);
        }
        EXPECT_TRUE(assigned.getPass(owe::rg::PassHandle {}).is_none());
        EXPECT_EQ(destroyed, 0);
    }
    EXPECT_EQ(destroyed, 128);
    EXPECT_EQ(replaced_destroyed, 1);
}

TEST(DependencyGraph, StoresHandlesAndDeduplicatesEdges) {
    owe::rg::DependencyGraph graph;
    auto                     first  = graph.AddNode();
    auto                     second = graph.AddNode();
    auto                     third  = graph.AddNode();

    EXPECT_TRUE(first.valid());
    EXPECT_TRUE(graph.Connect(first, second));
    EXPECT_TRUE(graph.Connect(first, second));
    EXPECT_TRUE(graph.Connect(second, third));
    EXPECT_FALSE(graph.Connect(first, owe::rg::NodeHandle {}));
    EXPECT_EQ(graph.NodeNum(), rstd::usize(3));
    EXPECT_EQ(graph.EdgeNum(), rstd::usize(2));
    EXPECT_EQ(graph.GetNodeOut(first).len(), rstd::usize(1));
    EXPECT_EQ(graph.GetNodeIn(third).len(), rstd::usize(1));

    auto order = graph.TopologicalOrder();
    ASSERT_EQ(order.len(), rstd::usize(3));
    EXPECT_EQ(order[rstd::usize()], first);
    EXPECT_EQ(order[rstd::usize(1)], second);
    EXPECT_EQ(order[rstd::usize(2)], third);
    EXPECT_FALSE(graph.HasCycle());

    EXPECT_TRUE(graph.Connect(third, first));
    EXPECT_TRUE(graph.HasCycle());
}

TEST(RenderGraphDebug, GraphvizIncludesResourceRefsAndAccessLabels) {
    owe::rg::RenderGraph graph;

    graph.addPass<DebugPass>(
        "draw/main"_str,
        owe::rg::PassNode::Type::CustomShader,
        [](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
            auto input = builder.createTexture(owe::rg::TextureDesc {
                .name = String::make("albedo"_str),
                .key  = String::make("tex/albedo"_str),
                .kind = owe::rg::TextureKind::Imported,
            });
            builder.read(input);

            auto output = builder.createTexture(
                owe::rg::TextureDesc {
                    .name              = String::make("default"_str),
                    .key               = String::make("_rt_default"_str),
                    .kind              = owe::rg::TextureKind::Temp,
                    .request           = rstd::Some(owe::resource::TextureRequest {
                        .kind     = owe::resource::TextureRequestKind::RenderTarget,
                        .name     = String::make("_rt_default"_str),
                        .lifetime = owe::resource::TextureLifetimeClass::FrameLocal,
                    }),
                    .allocation_family = rstd::Some(String::make("_rt_default"_str)),
                },
                true);
            builder.write(output);
        });

    graph.addPass<DebugPass>("draw/overlay"_str,
                             owe::rg::PassNode::Type::CustomShader,
                             [](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                 auto output = builder.createTexture(
                                     owe::rg::TextureDesc {
                                         .name = String::make("default"_str),
                                         .key  = String::make("_rt_default"_str),
                                         .kind = owe::rg::TextureKind::Temp,
                                     },
                                     true);
                                 builder.write(output);
                             });

    auto path = std::filesystem::temp_directory_path() / "owe-render-graph-debug-test.dot";
    graph.ToGraphviz(rstd::cppstd::as_str(path.native()).unwrap());

    auto dot = ReadFile(path);
    EXPECT_NE(dot.find("ref=n"), std::string::npos);
    EXPECT_NE(dot.find("pass-ref=p"), std::string::npos);
    EXPECT_NE(dot.find("pass: draw/main"), std::string::npos);
    EXPECT_NE(dot.find("type=CustomShader"), std::string::npos);
    EXPECT_NE(dot.find("resource: albedo"), std::string::npos);
    EXPECT_NE(dot.find("key=tex/albedo"), std::string::npos);
    EXPECT_NE(dot.find("kind=Imported"), std::string::npos);
    EXPECT_NE(dot.find("resource: default"), std::string::npos);
    EXPECT_NE(dot.find("kind=Temp"), std::string::npos);
    EXPECT_NE(dot.find("version=1"), std::string::npos);
    EXPECT_NE(dot.find("physical=_rt_default::slot_"), std::string::npos);
    EXPECT_NE(dot.find("access=read"), std::string::npos);
    EXPECT_NE(dot.find("access=read/version"), std::string::npos);
    EXPECT_NE(dot.find("access=write"), std::string::npos);
}

TEST(RenderGraphDebug, PassStateExposesPublicDebugRecord) {
    owe::rg::RenderGraph graph;

    auto pass = graph.addPass<DebugPass>("draw/main"_str,
                                         owe::rg::PassNode::Type::CustomShader,
                                         [](owe::rg::RenderGraphBuilder&, DebugPass::Desc&) {
                                         });

    auto state = graph.passState(pass);
    ASSERT_TRUE(state.is_some());
    EXPECT_EQ(state->handle, pass);
    EXPECT_TRUE(state->pass.valid());
    EXPECT_TRUE(graph.getPass(state->pass).is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view(state->name.as_str()), "draw/main");
    EXPECT_EQ(state->type, owe::rg::PassNode::Type::CustomShader);
    EXPECT_TRUE(graph.passState(owe::rg::NodeHandle {}).is_none());
    EXPECT_TRUE(graph.getPass(owe::rg::PassHandle {}).is_none());
}

TEST(RenderGraphDebug, RejectsDependencyCycles) {
    owe::rg::RenderGraph graph;
    auto                 source = owe::rg::TextureNodeRef {};
    graph.addPass<DebugPass>("draw/source"_str,
                             owe::rg::PassNode::Type::CustomShader,
                             [&](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                 source = builder.createTexture(
                                     owe::rg::TextureDesc {
                                         .name = String::make("source"_str),
                                         .key  = String::make("source"_str),
                                         .kind = owe::rg::TextureKind::Temp,
                                     },
                                     true);
                                 builder.write(source);
                             });

    auto consumer = graph.addPass<DebugPass>("draw/consumer"_str,
                                             owe::rg::PassNode::Type::CustomShader,
                                             [](owe::rg::RenderGraphBuilder&, DebugPass::Desc&) {
                                             });

    auto link = owe::rg::TextureNodeRef {};
    graph.addPass<DebugPass>("copy/link/first"_str,
                             owe::rg::PassNode::Type::Copy,
                             [&](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                 builder.read(source);
                                 link = builder.createTexture(
                                     owe::rg::TextureDesc {
                                         .name = String::make("link"_str),
                                         .key  = String::make("link"_str),
                                         .kind = owe::rg::TextureKind::Temp,
                                     },
                                     true);
                                 builder.write(link);
                             });
    ASSERT_TRUE(graph.readTexture(consumer, link));

    graph.addPass<DebugPass>("copy/link/second"_str,
                             owe::rg::PassNode::Type::Copy,
                             [&](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                 builder.read(source);
                                 auto next = builder.createTexture(
                                     owe::rg::TextureDesc {
                                         .name = String::make("link"_str),
                                         .key  = String::make("link"_str),
                                         .kind = owe::rg::TextureKind::Temp,
                                     },
                                     true);
                                 builder.write(next);
                                 ASSERT_TRUE(graph.readTexture(consumer, next));
                             });

    auto ordered = graph.topologicalOrder();
    ASSERT_TRUE(ordered.is_err());
    EXPECT_EQ(ordered.unwrap_err_unchecked(), owe::rg::RenderGraphOrderError::Cycle);
}

TEST(RenderGraphResources, CompilesBackendNeutralTexturePlan) {
    owe::rg::RenderGraph graph;

    graph.addPass<DebugPass>(
        "draw/main"_str,
        owe::rg::PassNode::Type::CustomShader,
        [](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
            auto input = builder.createTexture(owe::rg::TextureDesc {
                .name    = String::make("albedo"_str),
                .key     = String::make("tex/albedo"_str),
                .kind    = owe::rg::TextureKind::Imported,
                .request = rstd::Some(owe::resource::TextureRequest {
                    .kind = owe::resource::TextureRequestKind::Imported,
                    .name = String::make("tex/albedo"_str),
                }),
            });
            builder.read(input);

            auto output = builder.createTexture(
                owe::rg::TextureDesc {
                    .name    = String::make("default"_str),
                    .key     = String::make("_rt_default"_str),
                    .kind    = owe::rg::TextureKind::Temp,
                    .request = rstd::Some(owe::resource::TextureRequest {
                        .kind       = owe::resource::TextureRequestKind::RenderTarget,
                        .name       = String::make("_rt_default"_str),
                        .definition = rstd::Some(owe::resource::TextureDefinition {
                            .width  = rstd::i32(1920),
                            .height = rstd::i32(1080),
                        }),
                    }),
                },
                true);
            builder.write(output);
        });

    auto plan = graph.resourcePlan();
    ASSERT_EQ(plan.textures.len(), rstd::usize(2));
    EXPECT_TRUE(plan.textures[rstd::usize()].handle.Valid());
    EXPECT_EQ(plan.textures[rstd::usize()].access, owe::resource::ResourceAccess::Read);
    EXPECT_EQ(plan.textures[rstd::usize()].request.kind,
              owe::resource::TextureRequestKind::Imported);
    EXPECT_TRUE(plan.textures[rstd::usize(1)].handle.Valid());
    EXPECT_EQ(plan.textures[rstd::usize(1)].access, owe::resource::ResourceAccess::Write);
    ASSERT_TRUE(plan.textures[rstd::usize(1)].request.definition.is_some());
    EXPECT_EQ(plan.textures[rstd::usize(1)].request.definition->width, rstd::i32(1920));
}

TEST(RenderGraphResources, UsesDistinctResourceGenerationsAcrossGraphs) {
    owe::rg::RenderGraph    first;
    owe::rg::RenderGraph    second;
    owe::rg::TextureNodeRef first_texture;
    owe::rg::TextureNodeRef second_texture;

    first.addPass<DebugPass>("first"_str,
                             owe::rg::PassNode::Type::CustomShader,
                             [&](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                 first_texture = builder.createTexture(CompositeDesc(), true);
                                 builder.write(first_texture);
                             });
    second.addPass<DebugPass>("second"_str,
                              owe::rg::PassNode::Type::CustomShader,
                              [&](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                  second_texture = builder.createTexture(CompositeDesc(), true);
                                  builder.write(second_texture);
                              });

    auto first_plan   = first.resourcePlan();
    auto second_plan  = second.resourcePlan();
    auto first_state  = first.textureState(first_texture);
    auto second_state = second.textureState(second_texture);

    ASSERT_TRUE(first_state.is_some());
    ASSERT_TRUE(second_state.is_some());
    EXPECT_NE(first_plan.generation, second_plan.generation);
    EXPECT_EQ(first_state->use.generation, first_plan.generation);
    EXPECT_EQ(second_state->use.generation, second_plan.generation);
}

TEST(RenderGraphResources, UsesGraphKeyAsTextureRequestIdentity) {
    owe::rg::RenderGraph graph;
    graph.addPass<DebugPass>(
        "copy/snapshot"_str,
        owe::rg::PassNode::Type::Copy,
        [](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
            auto snapshot = builder.createTexture(
                owe::rg::TextureDesc {
                    .name    = String::make("_rt_default_1_copy"_str),
                    .key     = String::make("_rt_default_1_copy"_str),
                    .kind    = owe::rg::TextureKind::Temp,
                    .request = rstd::Some(owe::resource::TextureRequest {
                        .kind = owe::resource::TextureRequestKind::RenderTarget,
                        .name = String::make("_rt_default"_str),
                    }),
                },
                true);
            builder.write(snapshot);
        });

    auto plan = graph.resourcePlan();
    ASSERT_EQ(plan.textures.len(), rstd::usize(1));
    EXPECT_EQ(rstd::cppstd::as_string_view(plan.textures[rstd::usize()].request.name.as_str()),
              "_rt_default_1_copy");
}

TEST(RenderGraphResources, AlternatesPhysicalSlotsForLinearCompositeVersions) {
    owe::rg::RenderGraph    graph;
    owe::rg::TextureNodeRef current;

    graph.addPass<DebugPass>("source"_str,
                             owe::rg::PassNode::Type::CustomShader,
                             [&](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                 current = builder.createTexture(CompositeDesc(), true);
                                 builder.write(current);
                             });
    for (usize index {}; index < usize(2); ++index) {
        graph.addPass<DebugPass>("effect"_str,
                                 owe::rg::PassNode::Type::CustomShader,
                                 [&](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                     builder.read(current);
                                     current = builder.createTexture(CompositeDesc(), true);
                                     builder.write(current);
                                 });
    }

    auto plan = graph.resourcePlan();
    ASSERT_EQ(plan.textures.len(), usize(3));
    EXPECT_NE(plan.textures[usize()].allocation_key,
              plan.textures[usize(1)].allocation_key.as_str());
    EXPECT_EQ(plan.textures[usize()].allocation_key,
              plan.textures[usize(2)].allocation_key.as_str());
}

TEST(RenderGraphResources, ReusesOnePhysicalSlotForOverwriteOnlyVersions) {
    owe::rg::RenderGraph graph;
    for (usize index {}; index < usize(3); ++index) {
        graph.addPass<DebugPass>("overwrite"_str,
                                 owe::rg::PassNode::Type::CustomShader,
                                 [](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                     auto output = builder.createTexture(CompositeDesc(), true);
                                     builder.write(output);
                                 });
    }

    auto plan = graph.resourcePlan();
    ASSERT_EQ(plan.textures.len(), usize(3));
    EXPECT_EQ(plan.textures[usize()].allocation_key,
              plan.textures[usize(1)].allocation_key.as_str());
    EXPECT_EQ(plan.textures[usize()].allocation_key,
              plan.textures[usize(2)].allocation_key.as_str());
}

TEST(RenderGraphResources, ExpandsPhysicalSlotsForConcurrentVersions) {
    owe::rg::RenderGraph    graph;
    owe::rg::TextureNodeRef first;
    owe::rg::TextureNodeRef second;

    graph.addPass<DebugPass>("source"_str,
                             owe::rg::PassNode::Type::CustomShader,
                             [&](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                 first = builder.createTexture(CompositeDesc(), true);
                                 builder.write(first);
                             });
    graph.addPass<DebugPass>("effect/first"_str,
                             owe::rg::PassNode::Type::CustomShader,
                             [&](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                 builder.read(first);
                                 second = builder.createTexture(CompositeDesc(), true);
                                 builder.write(second);
                             });
    graph.addPass<DebugPass>("effect/second"_str,
                             owe::rg::PassNode::Type::CustomShader,
                             [&](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                 builder.read(first);
                                 builder.read(second);
                                 auto output = builder.createTexture(CompositeDesc(), true);
                                 builder.write(output);
                             });

    auto plan = graph.resourcePlan();
    ASSERT_EQ(plan.textures.len(), usize(3));
    EXPECT_NE(plan.textures[usize()].allocation_key,
              plan.textures[usize(1)].allocation_key.as_str());
    EXPECT_NE(plan.textures[usize()].allocation_key,
              plan.textures[usize(2)].allocation_key.as_str());
    EXPECT_NE(plan.textures[usize(1)].allocation_key,
              plan.textures[usize(2)].allocation_key.as_str());
}

TEST(RenderGraphResources, CanAliasAnOutputWithItsPreviousVersion) {
    owe::rg::RenderGraph    graph;
    owe::rg::TextureNodeRef current;

    graph.addPass<DebugPass>("source"_str,
                             owe::rg::PassNode::Type::CustomShader,
                             [&](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                 current = builder.createTexture(CompositeDesc(), true);
                                 builder.write(current);
                             });
    graph.addPass<DebugPass>("blend"_str,
                             owe::rg::PassNode::Type::CustomShader,
                             [&](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                 current = builder.createTexture(CompositeDesc(), true);
                                 builder.reusePreviousAllocation(current);
                                 builder.write(current);
                             });

    auto plan = graph.resourcePlan();
    ASSERT_EQ(plan.textures.len(), usize(2));
    EXPECT_EQ(plan.textures[usize()].allocation_key,
              plan.textures[usize(1)].allocation_key.as_str());
}

TEST(RenderGraphResources, PreservesFrameBoundaryTextureVersions) {
    owe::rg::RenderGraph graph;
    auto                 history      = owe::rg::TextureNodeRef {};
    auto                 history_desc = [] {
        return owe::rg::TextureDesc {
            .name    = String::make("history"_str),
            .key     = String::make("history"_str),
            .kind    = owe::rg::TextureKind::Temp,
            .request = rstd::Some(owe::resource::TextureRequest {
                .kind       = owe::resource::TextureRequestKind::RenderTarget,
                .name       = String::make("history"_str),
                .definition = rstd::Some(owe::resource::TextureDefinition {
                    .width  = rstd::i32(1920),
                    .height = rstd::i32(1080),
                }),
                .lifetime   = owe::resource::TextureLifetimeClass::FrameLocal,
            }),
        };
    };

    graph.addPass<DebugPass>("motion/accumulate"_str,
                             owe::rg::PassNode::Type::CustomShader,
                             [&](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                 history = builder.createTexture(history_desc());
                                 builder.markVirtualWrite(history);
                                 builder.read(history);
                             });
    graph.addPass<DebugPass>("motion/store"_str,
                             owe::rg::PassNode::Type::Copy,
                             [&](owe::rg::RenderGraphBuilder& builder, DebugPass::Desc&) {
                                 auto next = builder.createTexture(history_desc(), true);
                                 builder.write(next);
                             });

    auto plan    = graph.resourcePlan();
    auto ordered = graph.topologicalOrder();
    ASSERT_TRUE(ordered.is_ok());
    auto order = rstd::move(ordered).unwrap_unchecked();
    ASSERT_EQ(order.len(), rstd::usize(2));
    EXPECT_EQ(rstd::cppstd::as_string_view(graph.passState(order[rstd::usize()])->name.as_str()),
              "motion/accumulate");
    EXPECT_EQ(rstd::cppstd::as_string_view(graph.passState(order[rstd::usize(1)])->name.as_str()),
              "motion/store");
    ASSERT_EQ(plan.textures.len(), rstd::usize(2));
    for (const auto& entry : plan.textures) {
        EXPECT_EQ(entry.request.lifetime, owe::resource::TextureLifetimeClass::Retained);
        EXPECT_NE(entry.request.content & owe::resource::TextureContentFlag(
                                              owe::resource::TextureContent::PreserveAcrossFrames),
                  rstd::u32());
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

    auto caster                    = Arc<owe::SceneNode>::make();
    caster->shadow.cast            = true;
    auto               caster_mesh = Arc<owe::SceneMesh>::make();
    owe::SceneMaterial caster_material;
    caster_material.name                    = "caster-main"_Str;
    caster_material.shadow_variant          = Some(Arc<owe::SceneMaterial>::make());
    (*caster_material.shadow_variant)->name = "caster-shadow"_Str;
    caster_mesh->AddMaterial(rstd::move(caster_material));
    caster_mesh->Submeshes().push(owe::SceneMesh::Submesh {});
    caster->AddMesh(rstd::move(caster_mesh));
    scene.RootMut()->AppendChild(caster.clone());

    auto               receiver      = Arc<owe::SceneNode>::make();
    auto               receiver_mesh = Arc<owe::SceneMesh>::make();
    owe::SceneMaterial receiver_material;
    receiver_material.name = "receiver"_Str;
    receiver_material.textures.clear();
    receiver_material.textures.push("_rt_shadowAtlas"_Str);
    receiver_mesh->AddMaterial(rstd::move(receiver_material));
    receiver_mesh->Submeshes().push(owe::SceneMesh::Submesh {});
    receiver->AddMesh(rstd::move(receiver_mesh));
    scene.RootMut()->AppendChild(receiver.clone());

    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
    ASSERT_EQ(snapshot.ShadowCasters().len(), usize(1));
    auto graph   = owe::sceneToRenderGraph(scene, snapshot);
    auto ordered = graph->topologicalOrder();
    ASSERT_TRUE(ordered.is_ok());
    auto order = rstd::move(ordered).unwrap_unchecked();

    Option<usize> shadow_index;
    Option<usize> receiver_index;
    for (usize index {}; index < order.len(); ++index) {
        auto pass = graph->passState(order[index]);
        ASSERT_TRUE(pass.is_some());
        if (pass->name == "caster-shadow"_str) shadow_index = Some(index);
        if (pass->name == "receiver"_str) receiver_index = Some(index);
    }
    ASSERT_TRUE(shadow_index.is_some());
    ASSERT_TRUE(receiver_index.is_some());
    EXPECT_LT(*shadow_index, *receiver_index);

    auto plan = graph->resourcePlan();
    bool atlas_write { false };
    bool atlas_read { false };
    for (const auto& texture : plan.textures) {
        if (texture.request.name != "_rt_shadowAtlas"_str) continue;
        atlas_write = atlas_write || texture.access == owe::resource::ResourceAccess::Write ||
                      texture.access == owe::resource::ResourceAccess::ReadWrite;
        atlas_read  = atlas_read || texture.access == owe::resource::ResourceAccess::Read ||
                      texture.access == owe::resource::ResourceAccess::ReadWrite;
    }
    EXPECT_TRUE(atlas_write);
    EXPECT_TRUE(atlas_read);
}

TEST(SceneRenderGraph, SharesOneLinkTargetForMultipleConsumers) {
    owe::Scene scene;
    scene.SetOrtho({ rstd::i32(1920), rstd::i32(1080) });
    scene.RegisterRenderTarget(String::make("_rt_default"_str),
                               owe::SceneRenderTarget { .width = i32(1920), .height = i32(1080) });

    auto source  = rstd::sync::Arc<owe::SceneNode>::make();
    source->ID() = rstd::i32(7);
    source->SetSize({ 64.0f, 32.0f });
    auto               source_mesh = Arc<owe::SceneMesh>::make();
    owe::SceneMaterial source_material;
    source_material.name = "source"_Str;
    source_mesh->AddMaterial(std::move(source_material));
    source_mesh->Submeshes().push(owe::SceneMesh::Submesh {});
    source->AddMesh(std::move(source_mesh));
    scene.RootMut()->AppendChild(source.clone());
    scene.RegisterLayerLinkSource(owe::WallpaperLayerId { .value = rstd::i32(7) }, *source);
    scene.MarkLayerStaticElidable(owe::WallpaperLayerId { .value = rstd::i32(7) });

    auto consumer                    = rstd::sync::Arc<owe::SceneNode>::make();
    consumer->ID()                   = rstd::i32(42);
    auto               consumer_mesh = Arc<owe::SceneMesh>::make();
    owe::SceneMaterial consumer_material;
    consumer_material.name = "consumer"_Str;
    consumer_material.textures.clear();
    consumer_material.textures.push("_rt_link_7"_Str);
    consumer_material.textures.push("_rt_link_7"_Str);
    consumer_mesh->AddMaterial(std::move(consumer_material));
    consumer_mesh->Submeshes().push(owe::SceneMesh::Submesh {});
    consumer->AddMesh(std::move(consumer_mesh));
    scene.RootMut()->AppendChild(consumer.clone());

    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
    auto graph    = owe::sceneToRenderGraph(scene, snapshot);
    auto ordered  = graph->topologicalOrder();
    ASSERT_TRUE(ordered.is_ok());
    auto order = rstd::move(ordered).unwrap_unchecked();

    EXPECT_EQ(order.len(), rstd::usize(2));
    rstd::usize copy_count {};
    for (auto handle : order) {
        auto state = graph->passState(handle);
        ASSERT_TRUE(state.is_some());
        if (state->type == owe::rg::PassNode::Type::Copy) ++copy_count;
    }
    EXPECT_EQ(copy_count, rstd::usize());
}

TEST(SceneRenderGraph, ResolvesLinkedSurfaceFromProducerRegardlessOfSceneOrder) {
    owe::Scene scene;
    scene.SetOrtho({ rstd::i32(1920), rstd::i32(1080) });
    scene.RegisterRenderTarget(String::make("_rt_default"_str),
                               owe::SceneRenderTarget { .width = i32(1920), .height = i32(1080) });

    auto make_consumer = [](std::string name) {
        auto               node = Arc<owe::SceneNode>::make();
        auto               mesh = Arc<owe::SceneMesh>::make();
        owe::SceneMaterial material;
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
    auto               source_mesh = Arc<owe::SceneMesh>::make();
    owe::SceneMaterial source_material;
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
    auto graph    = owe::sceneToRenderGraph(scene, snapshot);
    auto ordered  = graph->topologicalOrder();
    ASSERT_TRUE(ordered.is_ok());

    auto        plan = graph->resourcePlan();
    rstd::usize link_versions {};
    for (const auto& entry : plan.textures) {
        if (entry.request.name != "_rt_link_7"_str) continue;
        ++link_versions;
        EXPECT_EQ(entry.version, rstd::u32());
        EXPECT_EQ(entry.request.lifetime, owe::resource::TextureLifetimeClass::Retained);
        EXPECT_EQ(entry.request.content & owe::resource::TextureContentFlag(
                                              owe::resource::TextureContent::PreserveAcrossFrames),
                  rstd::u32());
        EXPECT_NE(entry.request.content & owe::resource::TextureContentFlag(
                                              owe::resource::TextureContent::InitializeTransparent),
                  rstd::u32());
    }
    EXPECT_EQ(link_versions, rstd::usize(1));

    bool source_emitted {};
    bool before_reads_current {};
    bool after_reads_current {};
    auto ordered_passes = rstd::move(ordered).unwrap_unchecked();
    for (auto handle : ordered_passes) {
        auto state = graph->passState(handle);
        ASSERT_TRUE(state.is_some());
        if (state->name == "source"_str) {
            source_emitted = true;
            continue;
        }
        if (state->name != "before"_str && state->name != "after"_str) continue;
        EXPECT_TRUE(source_emitted);
        auto pass = graph->getPass(state->pass);
        ASSERT_TRUE(pass.is_some());
        auto uses = static_cast<owe::vulkan::VulkanPass&>(*pass).resourceUses();
        for (auto use : uses.textures) {
            for (const auto& entry : plan.textures) {
                if (entry.handle != use || entry.request.name != "_rt_link_7"_str) continue;
                before_reads_current |= state->name == "before"_str && entry.version == rstd::u32();
                after_reads_current |= state->name == "after"_str && entry.version == rstd::u32();
            }
        }
    }
    EXPECT_TRUE(before_reads_current);
    EXPECT_TRUE(after_reads_current);
}

TEST(SceneRenderGraph, ElidesSceneOwnedVisibilityHiddenSubtreeAndRestoresIt) {
    owe::Scene scene;
    scene.SetOrtho({ rstd::i32(1920), rstd::i32(1080) });
    scene.RegisterRenderTarget(String::make("_rt_default"_str),
                               owe::SceneRenderTarget { .width = i32(1920), .height = i32(1080) });

    auto parent             = Arc<owe::SceneNode>::make();
    parent->ID()            = rstd::i32(7);
    auto child              = Arc<owe::SceneNode>::make();
    child->ID()             = rstd::i32(8);
    auto               mesh = Arc<owe::SceneMesh>::make();
    owe::SceneMaterial material;
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
    auto hidden_graph    = owe::sceneToRenderGraph(scene, hidden_snapshot);
    auto hidden_order    = hidden_graph->topologicalOrder();
    ASSERT_TRUE(hidden_order.is_ok());
    EXPECT_TRUE(hidden_order.unwrap_unchecked().is_empty());

    EXPECT_TRUE(scene.SetNodeVisible(*parent, true));
    auto visible_snapshot = owe::ExtractRenderSceneSnapshot(scene);
    auto visible_graph    = owe::sceneToRenderGraph(scene, visible_snapshot);
    auto visible_order    = visible_graph->topologicalOrder();
    ASSERT_TRUE(visible_order.is_ok());
    EXPECT_EQ(visible_order.unwrap_unchecked().len(), rstd::usize(1));
}

TEST(SceneRenderGraph, PreservesLinkedSourceBelowVisibilityHiddenParent) {
    owe::Scene scene;
    scene.SetOrtho({ rstd::i32(1920), rstd::i32(1080) });
    scene.RegisterRenderTarget(String::make("_rt_default"_str),
                               owe::SceneRenderTarget { .width = i32(1920), .height = i32(1080) });

    auto parent                    = Arc<owe::SceneNode>::make();
    parent->ID()                   = rstd::i32(7);
    auto source                    = Arc<owe::SceneNode>::make();
    source->ID()                   = rstd::i32(8);
    auto               source_mesh = Arc<owe::SceneMesh>::make();
    owe::SceneMaterial source_material;
    source_material.name = "source"_Str;
    source_mesh->AddMaterial(std::move(source_material));
    source_mesh->Submeshes().push(owe::SceneMesh::Submesh {});
    source->AddMesh(std::move(source_mesh));
    parent->AppendChild(source.clone());
    scene.RootMut()->AppendChild(parent.clone());
    scene.RegisterNode(*parent, Some(owe::WallpaperLayerId { .value = rstd::i32(7) }));
    scene.RegisterLayerLinkSource(owe::WallpaperLayerId { .value = rstd::i32(8) }, *source);
    scene.MarkLayerStaticElidable(owe::WallpaperLayerId { .value = rstd::i32(8) });

    auto consumer                    = Arc<owe::SceneNode>::make();
    consumer->ID()                   = rstd::i32(42);
    auto               consumer_mesh = Arc<owe::SceneMesh>::make();
    owe::SceneMaterial consumer_material;
    consumer_material.name = "consumer"_Str;
    consumer_material.textures.clear();
    consumer_material.textures.push("_rt_link_8"_Str);
    consumer_mesh->AddMaterial(std::move(consumer_material));
    consumer_mesh->Submeshes().push(owe::SceneMesh::Submesh {});
    consumer->AddMesh(std::move(consumer_mesh));
    scene.RootMut()->AppendChild(consumer.clone());

    EXPECT_TRUE(scene.SetNodeVisible(*parent, false));
    auto snapshot = owe::ExtractRenderSceneSnapshot(scene);
    auto graph    = owe::sceneToRenderGraph(scene, snapshot);
    auto ordered  = graph->topologicalOrder();
    ASSERT_TRUE(ordered.is_ok());
    EXPECT_EQ(ordered.unwrap_unchecked().len(), rstd::usize(2));
}

TEST(VulkanRenderDiagnostics, EmptyBeforeProgramBuild) {
    owe::vulkan::VulkanRender render;
    EXPECT_TRUE(render.preparedPassDiagnostics().is_empty());
}
