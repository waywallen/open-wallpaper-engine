#include <rstd/test/gtest.hpp>

import rstd;
import vvk;
import wescene.resource;
import wescene.types;
import wescene.vulkan_render;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::sync::Arc;
using namespace owe::vulkan;
namespace resource = owe::resource;

namespace
{

void AddDescriptor(resource::ShaderArtifact&                 artifact,
                   resource::ShaderArtifactDescriptorBinding binding) {
    for (auto& set : artifact.descriptor_sets) {
        if (set.set == binding.set) {
            set.bindings.push(binding.clone());
            artifact.descriptor_bindings.push(rstd::move(binding));
            return;
        }
    }
    rstd::panic { "test descriptor set is missing" };
}

auto MakeArtifact() -> resource::ShaderArtifact {
    resource::ShaderArtifact artifact {
        .source = { .index = u32(7), .generation = u64(1) },
    };
    resource::ShaderArtifactStage stage {
        .stage       = owe::ShaderType::VERTEX,
        .entry_point = "main"_Str,
        .code        = owe::ShaderCode::from(rstd::array<rstd::uint32_t, 2> { 7u, 9u }.as_slice()),
    };
    artifact.stages.push(rstd::move(stage));
    artifact.descriptor_sets.push(resource::ShaderArtifactDescriptorSet { .set = u32() });
    artifact.descriptor_sets.push(
        resource::ShaderArtifactDescriptorSet { .set = u32(1), .push_descriptor = true });
    AddDescriptor(artifact,
                  resource::ShaderArtifactDescriptorBinding {
                      .name            = "Frame"_Str,
                      .set             = u32(),
                      .binding         = u32(),
                      .descriptor_type = u32(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                      .stage_flags     = u32(VK_SHADER_STAGE_VERTEX_BIT),
                  });
    AddDescriptor(artifact,
                  resource::ShaderArtifactDescriptorBinding {
                      .name            = "Material"_Str,
                      .set             = u32(1),
                      .binding         = u32(2),
                      .descriptor_type = u32(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                      .stage_flags     = u32(VK_SHADER_STAGE_FRAGMENT_BIT),
                  });
    AddDescriptor(artifact,
                  resource::ShaderArtifactDescriptorBinding {
                      .name            = "g_Texture0"_Str,
                      .set             = u32(1),
                      .binding         = u32(3),
                      .descriptor_type = u32(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                      .stage_flags = u32(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT),
                  });
    AddDescriptor(artifact,
                  resource::ShaderArtifactDescriptorBinding {
                      .name            = "g_Texture1"_Str,
                      .set             = u32(1),
                      .binding         = u32(4),
                      .descriptor_type = u32(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                      .stage_flags     = u32(VK_SHADER_STAGE_GEOMETRY_BIT),
                  });
    artifact.uniform_blocks.push(resource::ShaderArtifactUniformBlock {
        .name     = "Frame"_Str,
        .size     = usize(16),
        .set      = u32(),
        .binding  = u32(),
        .scope    = resource::ShaderArtifactUniformBlock::Scope::Shared,
        .identity = u64(77),
    });
    artifact.uniform_blocks.push(resource::ShaderArtifactUniformBlock {
        .name     = "Material"_Str,
        .size     = usize(16),
        .set      = u32(1),
        .binding  = u32(2),
        .scope    = resource::ShaderArtifactUniformBlock::Scope::Local,
        .identity = u64(88),
    });
    for (auto& block : artifact.uniform_blocks) {
        block.members.push(resource::ShaderArtifactUniformMember {
            .name              = "value"_Str,
            .size              = usize(16),
            .scalar_kind       = owe::ShaderScalarKind::Float,
            .scalar_width      = u32(32),
            .vector_components = u32(4),
        });
    }
    artifact.vertex_inputs.push(resource::ShaderArtifactVertexInput {
        .name     = "a_Position"_Str,
        .location = u32(),
        .format   = u32(VK_FORMAT_R32G32B32_SFLOAT),
    });
    artifact.vertex_inputs.push(resource::ShaderArtifactVertexInput {
        .name     = "a_TexCoord"_Str,
        .location = u32(2),
        .format   = u32(VK_FORMAT_R32G32_SFLOAT),
    });
    return artifact;
}

auto MakeSamplers() -> Vec<String> {
    Vec<String> members;
    members.push("g_Texture0"_Str);
    members.push("g_Texture1"_Str);
    return members;
}

auto MakeVertices() -> Vec<DrawVertexLayout> {
    DrawVertexLayout vertex { .stride = usize(20) };
    vertex.attributes.push(DrawVertexAttribute { .name = "a_Position"_Str, .offset = usize() });
    vertex.attributes.push(DrawVertexAttribute { .name = "a_TexCoord"_Str, .offset = usize(12) });
    Vec<DrawVertexLayout> vertices;
    vertices.push(rstd::move(vertex));
    return vertices;
}

auto Pipeline(u64 generation) -> resource::PipelineUseHandle {
    return { .index = u64(5), .generation = generation };
}

auto FindAttribute(const SceneDrawPreparation& draw, rstd::uint32_t location,
                   rstd::uint32_t binding = 0) -> const VkVertexInputAttributeDescription* {
    for (const auto& attribute : draw.vertex_attributes)
        if (attribute.location == location && attribute.binding == binding) return &attribute;
    return nullptr;
}

void ExpectSamePreparation(const SceneDrawPreparation& cached,
                           const SceneDrawPreparation& uncached) {
    ASSERT_EQ(cached.samplers.len(), uncached.samplers.len());
    for (usize index; index < cached.samplers.len(); ++index) {
        EXPECT_EQ(cached.samplers[index].binding, uncached.samplers[index].binding);
        EXPECT_EQ(cached.samplers[index].set, uncached.samplers[index].set);
        EXPECT_EQ(cached.samplers[index].stages, uncached.samplers[index].stages);
    }
    ASSERT_EQ(cached.vertex_bindings.len(), uncached.vertex_bindings.len());
    for (usize index; index < cached.vertex_bindings.len(); ++index) {
        EXPECT_EQ(cached.vertex_bindings[index].binding, uncached.vertex_bindings[index].binding);
        EXPECT_EQ(cached.vertex_bindings[index].stride, uncached.vertex_bindings[index].stride);
        EXPECT_EQ(cached.vertex_bindings[index].inputRate,
                  uncached.vertex_bindings[index].inputRate);
    }
    ASSERT_EQ(cached.vertex_attributes.len(), uncached.vertex_attributes.len());
    for (const auto& attribute : cached.vertex_attributes) {
        auto* other = FindAttribute(uncached, attribute.location, attribute.binding);
        ASSERT_NE(other, nullptr);
        EXPECT_EQ(attribute.format, other->format);
        EXPECT_EQ(attribute.offset, other->offset);
    }
    auto left  = cached.pipelineLayout(Pipeline(u64(1)));
    auto right = uncached.pipelineLayout(Pipeline(u64(1)));
    EXPECT_EQ(left.pipeline, right.pipeline);
    ASSERT_EQ(left.descriptor_sets.len(), right.descriptor_sets.len());
    for (usize index; index < left.descriptor_sets.len(); ++index) {
        const auto& a = left.descriptor_sets[index];
        const auto& b = right.descriptor_sets[index];
        EXPECT_EQ(a.set, b.set);
        EXPECT_EQ(a.push_descriptor, b.push_descriptor);
        ASSERT_EQ(a.bindings.len(), b.bindings.len());
        for (usize binding; binding < a.bindings.len(); ++binding) {
            EXPECT_EQ(a.bindings[binding].binding, b.bindings[binding].binding);
            EXPECT_EQ(a.bindings[binding].descriptor_type, b.bindings[binding].descriptor_type);
            EXPECT_EQ(a.bindings[binding].descriptor_count, b.bindings[binding].descriptor_count);
            EXPECT_EQ(a.bindings[binding].stage_flags, b.bindings[binding].stage_flags);
            EXPECT_EQ(a.bindings[binding].shared_identity, b.bindings[binding].shared_identity);
        }
    }
}

} // namespace

TEST(SceneDrawPreparation, ReusesIndependentArtifactsAndOwnsPreparedInterface) {
    SceneDrawPreparationCache cache;
    auto                      first_artifact  = MakeArtifact();
    auto                      second_artifact = MakeArtifact();
    auto                      samplers        = MakeSamplers();
    auto                      vertices        = MakeVertices();
    auto                      second_samplers = MakeSamplers();
    auto                      second_vertices = MakeVertices();
    EXPECT_NE(first_artifact.vertex_inputs.data(), second_artifact.vertex_inputs.data());
    EXPECT_NE(first_artifact.stages[usize()].code.data(),
              second_artifact.stages[usize()].code.data());

    auto first_result = cache.prepare(first_artifact, samplers.as_slice(), vertices.as_slice());
    auto second_result =
        cache.prepare(second_artifact, second_samplers.as_slice(), second_vertices.as_slice());
    ASSERT_TRUE(first_result.is_ok());
    ASSERT_TRUE(second_result.is_ok());
    auto first  = rstd::move(first_result).unwrap();
    auto second = rstd::move(second_result).unwrap();
    EXPECT_TRUE(Arc<SceneDrawPreparation>::ptr_eq(first, second));
    EXPECT_EQ(cache.stats().hits, u64(1));
    EXPECT_EQ(cache.stats().misses, u64(1));
    EXPECT_EQ(cache.stats().entries, usize(1));

    first_artifact  = {};
    second_artifact = {};
    samplers.clear();
    vertices.clear();
    second_samplers.clear();
    second_vertices.clear();
    ASSERT_EQ(first->samplers.len(), usize(2));
    EXPECT_EQ(first->samplers[usize()].binding, i32(3));
    EXPECT_EQ(first->samplers[usize()].set, u32(1));
    EXPECT_EQ(first->samplers[usize()].stages,
              vrento::texture::Stages::Vertex | vrento::texture::Stages::Fragment);
    EXPECT_EQ(first->samplers[usize(1)].binding, i32(4));
    EXPECT_EQ(first->samplers[usize(1)].stages, vrento::texture::Stages::Geometry);
    ASSERT_EQ(first->vertex_bindings.len(), usize(1));
    EXPECT_EQ(first->vertex_bindings[usize()].binding, 0u);
    EXPECT_EQ(first->vertex_bindings[usize()].stride, 20u);
    EXPECT_EQ(first->vertex_bindings[usize()].inputRate, VK_VERTEX_INPUT_RATE_VERTEX);
    ASSERT_EQ(first->vertex_attributes.len(), usize(2));
    auto* position = FindAttribute(*first, 0);
    auto* texcoord = FindAttribute(*first, 2);
    ASSERT_NE(position, nullptr);
    ASSERT_NE(texcoord, nullptr);
    EXPECT_EQ(position->format, VK_FORMAT_R32G32B32_SFLOAT);
    EXPECT_EQ(position->offset, 0u);
    EXPECT_EQ(texcoord->format, VK_FORMAT_R32G32_SFLOAT);
    EXPECT_EQ(texcoord->offset, 12u);
    auto layout = first->pipelineLayout(Pipeline(u64(1)));
    ASSERT_EQ(layout.descriptor_sets.len(), usize(2));
    EXPECT_EQ(layout.descriptor_sets[usize()].bindings[usize()].shared_identity, Some(u64(77)));
    EXPECT_TRUE(layout.descriptor_sets[usize(1)].push_descriptor);

    auto fresh_artifact = MakeArtifact();
    auto fresh_samplers = MakeSamplers();
    auto fresh_vertices = MakeVertices();
    auto direct_result =
        PrepareSceneDraw(fresh_artifact, fresh_samplers.as_slice(), fresh_vertices.as_slice());
    ASSERT_TRUE(direct_result.is_ok());
    auto direct = rstd::move(direct_result).unwrap();
    ExpectSamePreparation(*first, *direct);
    auto hit = cache.prepare(fresh_artifact, fresh_samplers.as_slice(), fresh_vertices.as_slice());
    ASSERT_TRUE(hit.is_ok());
    EXPECT_TRUE(Arc<SceneDrawPreparation>::ptr_eq(first, hit.unwrap()));
}

TEST(SceneDrawPreparation, IgnoresShaderStorageVersionsAndUniformMembers) {
    SceneDrawPreparationCache cache;
    auto                      artifact = MakeArtifact();
    auto                      samplers = MakeSamplers();
    auto                      vertices = MakeVertices();
    auto original_result = cache.prepare(artifact, samplers.as_slice(), vertices.as_slice());
    ASSERT_TRUE(original_result.is_ok());
    auto original = rstd::move(original_result).unwrap();

    auto changed                        = artifact.clone();
    changed.source                      = { .index = u32(99), .generation = u64(42) };
    changed.content_version             = u64(123);
    changed.stages[usize()].entry_point = "another_main"_Str;
    changed.stages[usize()].code =
        owe::ShaderCode::from(rstd::array<rstd::uint32_t, 3> { 11u, 13u, 17u }.as_slice());
    for (auto& block : changed.uniform_blocks) {
        block.size                         = usize(64);
        block.members[usize()].name        = "other_value"_Str;
        block.members[usize()].offset      = u32(16);
        block.members[usize()].size        = usize(32);
        block.members[usize()].count       = usize(2);
        block.members[usize()].scalar_kind = owe::ShaderScalarKind::SignedInteger;
        block.members.push(resource::ShaderArtifactUniformMember {
            .name   = "extra_value"_Str,
            .offset = u32(48),
            .size   = usize(16),
        });
    }
    auto hit_result = cache.prepare(changed, samplers.as_slice(), vertices.as_slice());
    ASSERT_TRUE(hit_result.is_ok());
    auto hit = rstd::move(hit_result).unwrap();
    EXPECT_TRUE(Arc<SceneDrawPreparation>::ptr_eq(original, hit));
    EXPECT_EQ(cache.stats().hits, u64(1));
    EXPECT_EQ(cache.stats().misses, u64(1));
    EXPECT_EQ(cache.stats().entries, usize(1));
}

TEST(SceneDrawPreparation, KeysSamplerMembersOrderAndSlotCount) {
    SceneDrawPreparationCache cache;
    auto                      artifact = MakeArtifact();
    auto                      samplers = MakeSamplers();
    auto                      vertices = MakeVertices();
    auto baseline_result = cache.prepare(artifact, samplers.as_slice(), vertices.as_slice());
    ASSERT_TRUE(baseline_result.is_ok());
    auto baseline = rstd::move(baseline_result).unwrap();

    for (unsigned change = 0; change < 5; ++change) {
        SCOPED_TRACE(change);
        auto members = MakeSamplers();
        switch (change) {
        case 0: rstd::mem::swap(members[usize()], members[usize(1)]); break;
        case 1: members[usize()] = "g_TextureX"_Str; break;
        case 2: members[usize()] = String {}; break;
        case 3: members.push("g_Texture0"_Str); break;
        case 4: members.truncate(usize(1)); break;
        }
        auto result = cache.prepare(artifact, members.as_slice(), vertices.as_slice());
        ASSERT_TRUE(result.is_ok());
        auto prepared = rstd::move(result).unwrap();
        EXPECT_FALSE(Arc<SceneDrawPreparation>::ptr_eq(baseline, prepared));
        ASSERT_EQ(prepared->samplers.len(), members.len());
        if (change == 0) {
            EXPECT_EQ(prepared->samplers[usize()].binding, i32(4));
            EXPECT_EQ(prepared->samplers[usize(1)].binding, i32(3));
        } else if (change == 1 || change == 2) {
            EXPECT_EQ(prepared->samplers[usize()].binding, i32(-1));
            EXPECT_EQ(prepared->samplers[usize()].set, u32());
            EXPECT_EQ(prepared->samplers[usize()].stages, vrento::texture::Stages::None);
        } else if (change == 3) {
            EXPECT_EQ(prepared->samplers[usize(2)].binding, i32(3));
        }
        auto hit = cache.prepare(artifact, members.as_slice(), vertices.as_slice());
        ASSERT_TRUE(hit.is_ok());
        EXPECT_TRUE(Arc<SceneDrawPreparation>::ptr_eq(prepared, hit.unwrap()));
    }
    EXPECT_EQ(cache.stats().hits, u64(5));
    EXPECT_EQ(cache.stats().misses, u64(6));
    EXPECT_EQ(cache.stats().entries, usize(6));
}

TEST(SceneDrawPreparation, KeysShaderInputsAndVertexBufferLayouts) {
    SceneDrawPreparationCache cache;
    auto                      artifact = MakeArtifact();
    auto                      samplers = MakeSamplers();
    auto                      vertices = MakeVertices();
    auto                      cloned   = vertices[usize()].clone();
    EXPECT_TRUE(cloned == vertices[usize()]);
    EXPECT_TRUE(cloned.attributes[usize(1)] == vertices[usize()].attributes[usize(1)]);
    cloned.attributes[usize(1)].offset = usize(8);
    EXPECT_FALSE(cloned == vertices[usize()]);
    EXPECT_EQ(vertices[usize()].attributes[usize(1)].offset, usize(12));
    auto baseline_result = cache.prepare(artifact, samplers.as_slice(), vertices.as_slice());
    ASSERT_TRUE(baseline_result.is_ok());
    auto baseline = rstd::move(baseline_result).unwrap();

    for (unsigned change = 0; change < 7; ++change) {
        SCOPED_TRACE(change);
        auto shader  = MakeArtifact();
        auto layouts = MakeVertices();
        switch (change) {
        case 0: shader.vertex_inputs[usize(1)].format = u32(VK_FORMAT_R32_SFLOAT); break;
        case 1: shader.vertex_inputs[usize(1)].location = u32(5); break;
        case 2: layouts[usize()].stride = usize(24); break;
        case 3: layouts[usize()].attributes[usize(1)].offset = usize(8); break;
        case 4: shader.vertex_inputs[usize(1)].name = "a_Missing0"_Str; break;
        case 5: layouts[usize()].attributes[usize(1)].name = "a_Missing0"_Str; break;
        case 6: layouts.push(layouts[usize()].clone()); break;
        }
        auto result = cache.prepare(shader, samplers.as_slice(), layouts.as_slice());
        ASSERT_TRUE(result.is_ok());
        auto prepared = rstd::move(result).unwrap();
        EXPECT_FALSE(Arc<SceneDrawPreparation>::ptr_eq(baseline, prepared));
        auto* attribute = FindAttribute(*prepared, change == 1 ? 5u : 2u);
        ASSERT_NE(attribute, nullptr);
        EXPECT_EQ(attribute->format, change == 0 ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R32G32_SFLOAT);
        EXPECT_EQ(attribute->offset, change == 3 ? 8u : (change == 4 || change == 5 ? 0u : 12u));
        EXPECT_EQ(prepared->vertex_bindings[usize()].stride, change == 2 ? 24u : 20u);
        if (change == 6) {
            ASSERT_EQ(prepared->vertex_bindings.len(), usize(2));
            EXPECT_EQ(prepared->vertex_bindings[usize(1)].binding, 1u);
            ASSERT_EQ(prepared->vertex_attributes.len(), usize(4));
            auto* second = FindAttribute(*prepared, 2, 1);
            ASSERT_NE(second, nullptr);
            EXPECT_EQ(second->offset, 12u);
        }
        auto direct_result = PrepareSceneDraw(shader, samplers.as_slice(), layouts.as_slice());
        ASSERT_TRUE(direct_result.is_ok());
        auto direct = rstd::move(direct_result).unwrap();
        ExpectSamePreparation(*prepared, *direct);
    }
    EXPECT_EQ(cache.stats().misses, u64(8));
    EXPECT_EQ(cache.stats().entries, usize(8));
}

TEST(SceneDrawPreparation, KeysSamplerBindingSetNameAndStages) {
    SceneDrawPreparationCache cache;
    auto                      artifact = MakeArtifact();
    auto                      samplers = MakeSamplers();
    auto                      vertices = MakeVertices();
    auto baseline_result = cache.prepare(artifact, samplers.as_slice(), vertices.as_slice());
    ASSERT_TRUE(baseline_result.is_ok());
    auto baseline = rstd::move(baseline_result).unwrap();

    for (unsigned change = 0; change < 4; ++change) {
        SCOPED_TRACE(change);
        auto  shader = MakeArtifact();
        auto& flat   = shader.descriptor_bindings[usize(2)];
        auto& nested = shader.descriptor_sets[usize(1)].bindings[usize(1)];
        switch (change) {
        case 0: flat.binding = nested.binding = u32(7); break;
        case 1:
            shader.descriptor_sets[usize(1)].set = u32(2);
            shader.uniform_blocks[usize(1)].set  = u32(2);
            for (auto& binding : shader.descriptor_sets[usize(1)].bindings) binding.set = u32(2);
            for (auto& binding : shader.descriptor_bindings)
                if (binding.set == u32(1)) binding.set = u32(2);
            break;
        case 2:
            flat.stage_flags = nested.stage_flags =
                u32(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                    VK_SHADER_STAGE_GEOMETRY_BIT);
            break;
        case 3:
            flat.name   = "g_TextureX"_Str;
            nested.name = "g_TextureX"_Str;
            break;
        }
        auto result = cache.prepare(shader, samplers.as_slice(), vertices.as_slice());
        ASSERT_TRUE(result.is_ok());
        auto prepared = rstd::move(result).unwrap();
        EXPECT_FALSE(Arc<SceneDrawPreparation>::ptr_eq(baseline, prepared));
        EXPECT_EQ(prepared->samplers[usize()].binding,
                  change == 0 ? i32(7) : (change == 3 ? i32(-1) : i32(3)));
        EXPECT_EQ(prepared->samplers[usize()].set,
                  change == 1 ? u32(2) : (change == 3 ? u32() : u32(1)));
        auto expected_stages =
            change == 3 ? vrento::texture::Stages::None
                        : vrento::texture::Stages::Vertex | vrento::texture::Stages::Fragment;
        if (change == 2) expected_stages = expected_stages | vrento::texture::Stages::Geometry;
        EXPECT_EQ(prepared->samplers[usize()].stages, expected_stages);
    }
    EXPECT_EQ(cache.stats().misses, u64(5));
    EXPECT_EQ(cache.stats().entries, usize(5));
}

TEST(SceneDrawPreparation, KeysDescriptorRequirementsSharedIdentityAndPushMode) {
    SceneDrawPreparationCache cache;
    auto                      artifact = MakeArtifact();
    auto                      samplers = MakeSamplers();
    auto                      vertices = MakeVertices();
    auto baseline_result = cache.prepare(artifact, samplers.as_slice(), vertices.as_slice());
    ASSERT_TRUE(baseline_result.is_ok());
    auto baseline = rstd::move(baseline_result).unwrap();

    for (unsigned change = 0; change < 6; ++change) {
        SCOPED_TRACE(change);
        auto  shader = MakeArtifact();
        auto& local  = shader.descriptor_sets[usize(1)];
        switch (change) {
        case 0: shader.uniform_blocks[usize()].identity = u64(78); break;
        case 1: local.push_descriptor = false; break;
        case 2:
            local.bindings[usize(1)].descriptor_count             = u32(2);
            shader.descriptor_bindings[usize(2)].descriptor_count = u32(2);
            break;
        case 3:
            shader.descriptor_sets[usize()].bindings[usize()].stage_flags =
                u32(VK_SHADER_STAGE_FRAGMENT_BIT);
            shader.descriptor_bindings[usize()].stage_flags = u32(VK_SHADER_STAGE_FRAGMENT_BIT);
            break;
        case 4:
            local.bindings[usize(1)].descriptor_type = u32(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            shader.descriptor_bindings[usize(2)].descriptor_type =
                u32(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            break;
        case 5:
            shader.uniform_blocks[usize(1)].binding      = u32(6);
            local.bindings[usize()].binding              = u32(6);
            shader.descriptor_bindings[usize(1)].binding = u32(6);
            break;
        }
        auto result = cache.prepare(shader, samplers.as_slice(), vertices.as_slice());
        ASSERT_TRUE(result.is_ok());
        auto prepared = rstd::move(result).unwrap();
        EXPECT_FALSE(Arc<SceneDrawPreparation>::ptr_eq(baseline, prepared));
        auto layout = prepared->pipelineLayout(Pipeline(u64(2)));
        ASSERT_EQ(layout.descriptor_sets.len(), usize(2));
        const auto& shared   = layout.descriptor_sets[usize()].bindings[usize()];
        const auto& material = layout.descriptor_sets[usize(1)];
        EXPECT_EQ(shared.shared_identity, Some(change == 0 ? u64(78) : u64(77)));
        EXPECT_EQ(shared.stage_flags,
                  u32(change == 3 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT));
        EXPECT_EQ(material.push_descriptor, change != 1);
        EXPECT_EQ(material.bindings[usize(1)].descriptor_count, change == 2 ? u32(2) : u32(1));
        EXPECT_EQ(material.bindings[usize(1)].descriptor_type,
                  u32(change == 4 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                  : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
        EXPECT_EQ(material.bindings[usize()].binding, change == 5 ? u32(6) : u32(2));
    }
    EXPECT_EQ(cache.stats().misses, u64(7));
    EXPECT_EQ(cache.stats().entries, usize(7));
}

TEST(SceneDrawPreparation, InjectsFreshPipelineGenerationsIntoIndependentLayouts) {
    SceneDrawPreparationCache cache;
    auto                      artifact = MakeArtifact();
    auto                      samplers = MakeSamplers();
    auto                      vertices = MakeVertices();
    auto result = cache.prepare(artifact, samplers.as_slice(), vertices.as_slice());
    ASSERT_TRUE(result.is_ok());
    auto prepared = rstd::move(result).unwrap();
    auto first    = prepared->pipelineLayout(Pipeline(u64(41)));
    auto second   = prepared->pipelineLayout(Pipeline(u64(42)));
    EXPECT_EQ(first.pipeline, Pipeline(u64(41)));
    EXPECT_EQ(second.pipeline, Pipeline(u64(42)));
    ASSERT_EQ(first.descriptor_sets.len(), usize(2));
    ASSERT_EQ(second.descriptor_sets.len(), usize(2));
    first.descriptor_sets[usize()].bindings[usize()].shared_identity = Some(u64(999));
    first.descriptor_sets[usize(1)].bindings.clear();
    EXPECT_EQ(second.descriptor_sets[usize()].bindings[usize()].shared_identity, Some(u64(77)));
    EXPECT_EQ(second.descriptor_sets[usize(1)].bindings.len(), usize(3));
    auto hit = cache.prepare(artifact, samplers.as_slice(), vertices.as_slice());
    ASSERT_TRUE(hit.is_ok());
    auto same = rstd::move(hit).unwrap();
    EXPECT_TRUE(Arc<SceneDrawPreparation>::ptr_eq(prepared, same));
    auto third = same->pipelineLayout(Pipeline(u64(43)));
    EXPECT_EQ(third.pipeline, Pipeline(u64(43)));
    EXPECT_EQ(third.descriptor_sets[usize()].bindings[usize()].shared_identity, Some(u64(77)));
    EXPECT_EQ(third.descriptor_sets[usize(1)].bindings.len(), usize(3));
    EXPECT_EQ(first.pipeline, Pipeline(u64(41)));
    EXPECT_EQ(second.pipeline, Pipeline(u64(42)));
}

TEST(SceneDrawPreparation, RejectsInvalidUniformScopesAfterCacheWarmup) {
    SceneDrawPreparationCache cache;
    auto                      artifact = MakeArtifact();
    auto                      samplers = MakeSamplers();
    auto                      vertices = MakeVertices();
    auto warm = cache.prepare(artifact, samplers.as_slice(), vertices.as_slice());
    ASSERT_TRUE(warm.is_ok());
    for (unsigned change = 0; change < 3; ++change) {
        SCOPED_TRACE(change);
        auto invalid = MakeArtifact();
        switch (change) {
        case 0:
            invalid.uniform_blocks[usize()].scope =
                resource::ShaderArtifactUniformBlock::Scope::Local;
            break;
        case 1:
            invalid.uniform_blocks[usize(1)].scope =
                resource::ShaderArtifactUniformBlock::Scope::Shared;
            break;
        case 2: invalid.uniform_blocks[usize()].set = u32(1); break;
        }
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            auto rejected = cache.prepare(invalid, samplers.as_slice(), vertices.as_slice());
            ASSERT_TRUE(rejected.is_err());
            auto error = rstd::move(rejected).unwrap_err();
            EXPECT_EQ(error.kind, resource::ResourceErrorKind::MissingDefinition);
            EXPECT_FALSE(error.message.is_empty());
        }
        EXPECT_TRUE(PrepareSceneDraw(invalid, samplers.as_slice(), vertices.as_slice()).is_err());
        EXPECT_EQ(cache.stats().hits, u64());
        EXPECT_EQ(cache.stats().entries, usize(1));
    }
    auto hit = cache.prepare(artifact, samplers.as_slice(), vertices.as_slice());
    ASSERT_TRUE(hit.is_ok());
    EXPECT_TRUE(Arc<SceneDrawPreparation>::ptr_eq(warm.unwrap(), hit.unwrap()));
}

TEST(SceneDrawPreparation, RejectsInvalidDescriptorLayoutsAfterCacheWarmup) {
    SceneDrawPreparationCache cache;
    auto                      artifact = MakeArtifact();
    auto                      samplers = MakeSamplers();
    auto                      vertices = MakeVertices();
    auto warm = cache.prepare(artifact, samplers.as_slice(), vertices.as_slice());
    ASSERT_TRUE(warm.is_ok());
    for (unsigned change = 0; change < 4; ++change) {
        SCOPED_TRACE(change);
        auto invalid = MakeArtifact();
        switch (change) {
        case 0: invalid.descriptor_sets[usize(1)].bindings[usize(1)].set = u32(2); break;
        case 1: invalid.descriptor_sets[usize()].bindings.clear(); break;
        case 2:
            invalid.descriptor_sets[usize(1)].bindings[usize()].descriptor_type =
                u32(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            break;
        case 3:
            AddDescriptor(invalid,
                          resource::ShaderArtifactDescriptorBinding {
                              .name            = "unowned"_Str,
                              .set             = u32(),
                              .binding         = u32(9),
                              .descriptor_type = u32(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                              .stage_flags     = u32(VK_SHADER_STAGE_FRAGMENT_BIT),
                          });
            break;
        }
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            auto rejected = cache.prepare(invalid, samplers.as_slice(), vertices.as_slice());
            ASSERT_TRUE(rejected.is_err());
            auto error = rstd::move(rejected).unwrap_err();
            EXPECT_EQ(error.kind, resource::ResourceErrorKind::MissingDefinition);
            EXPECT_FALSE(error.message.is_empty());
        }
        EXPECT_TRUE(PrepareSceneDraw(invalid, samplers.as_slice(), vertices.as_slice()).is_err());
        EXPECT_EQ(cache.stats().hits, u64());
        EXPECT_EQ(cache.stats().entries, usize(1));
    }
    auto hit = cache.prepare(artifact, samplers.as_slice(), vertices.as_slice());
    ASSERT_TRUE(hit.is_ok());
    EXPECT_TRUE(Arc<SceneDrawPreparation>::ptr_eq(warm.unwrap(), hit.unwrap()));
}

TEST(SceneDrawPreparation, PrunesOnlyEntriesNoLongerReferencedByDraws) {
    SceneDrawPreparationCache cache;
    auto                      artifact = MakeArtifact();
    auto                      samplers = MakeSamplers();
    auto                      vertices = MakeVertices();
    auto result = cache.prepare(artifact, samplers.as_slice(), vertices.as_slice());
    ASSERT_TRUE(result.is_ok());
    auto                                           draw = rstd::move(result).unwrap();
    auto                                           live = draw.downgrade();
    Option<rstd::sync::Weak<SceneDrawPreparation>> idle;
    {
        auto other_members = MakeSamplers();
        other_members.push("g_Texture0"_Str);
        auto other_result = cache.prepare(artifact, other_members.as_slice(), vertices.as_slice());
        ASSERT_TRUE(other_result.is_ok());
        auto other = rstd::move(other_result).unwrap();
        idle       = Some(other.downgrade());
    }
    EXPECT_EQ(cache.stats().entries, usize(2));
    EXPECT_EQ(draw.strong_count(), usize(2));
    EXPECT_FALSE(idle->expired());
    cache.prune();
    EXPECT_EQ(cache.stats().entries, usize(1));
    EXPECT_TRUE(idle->expired());
    EXPECT_FALSE(live.expired());
    EXPECT_EQ(draw->samplers[usize()].binding, i32(3));
    auto hit = cache.prepare(artifact, samplers.as_slice(), vertices.as_slice());
    ASSERT_TRUE(hit.is_ok());
    auto another_draw = rstd::move(hit).unwrap();
    EXPECT_TRUE(Arc<SceneDrawPreparation>::ptr_eq(draw, another_draw));
    draw.reset();
    cache.prune();
    EXPECT_EQ(cache.stats().entries, usize(1));
    EXPECT_EQ(another_draw.strong_count(), usize(2));
    another_draw.reset();
    EXPECT_EQ(live.strong_count(), usize(1));
    cache.prune();
    EXPECT_EQ(cache.stats().entries, usize());
    EXPECT_TRUE(live.expired());
    cache.prune();
    EXPECT_EQ(cache.stats().entries, usize());
}

TEST(SceneDrawPreparation, ClearAndCacheDestructionPreserveExternalOwners) {
    Option<Arc<SceneDrawPreparation>> cleared_draw;
    Option<Arc<SceneDrawPreparation>> surviving_draw;
    {
        SceneDrawPreparationCache cache;
        auto                      artifact = MakeArtifact();
        auto                      samplers = MakeSamplers();
        auto                      vertices = MakeVertices();
        auto result = cache.prepare(artifact, samplers.as_slice(), vertices.as_slice());
        ASSERT_TRUE(result.is_ok());
        cleared_draw = Some(rstd::move(result).unwrap());
        cache.clear();
        EXPECT_EQ(cache.stats().entries, usize());
        EXPECT_EQ(cleared_draw->strong_count(), usize(1));
        EXPECT_EQ((*cleared_draw)->samplers[usize()].binding, i32(3));
        auto replacement = cache.prepare(artifact, samplers.as_slice(), vertices.as_slice());
        ASSERT_TRUE(replacement.is_ok());
        surviving_draw = Some(rstd::move(replacement).unwrap());
        EXPECT_FALSE(Arc<SceneDrawPreparation>::ptr_eq(*cleared_draw, *surviving_draw));
        EXPECT_EQ(cache.stats().entries, usize(1));
        EXPECT_EQ(surviving_draw->strong_count(), usize(2));
    }
    EXPECT_EQ(cleared_draw->strong_count(), usize(1));
    EXPECT_EQ(surviving_draw->strong_count(), usize(1));
    ExpectSamePreparation(**cleared_draw, **surviving_draw);
    auto weak   = surviving_draw->downgrade();
    auto layout = (*surviving_draw)->pipelineLayout(Pipeline(u64(91)));
    EXPECT_EQ(layout.pipeline, Pipeline(u64(91)));
    cleared_draw   = None();
    surviving_draw = None();
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(layout.descriptor_sets[usize()].bindings[usize()].shared_identity, Some(u64(77)));
}

#ifndef NDEBUG
TEST(SceneDrawPreparation, DebugMicrobaselineWithManyUniformMembers) {
    constexpr unsigned            repetitions = 256;
    Vec<resource::ShaderArtifact> artifacts;
    for (unsigned index = 0; index < 8; ++index) {
        auto artifact            = MakeArtifact();
        artifact.source          = { .index = u32(index + 1), .generation = u64(index + 1) };
        artifact.content_version = u64(index + 1);
        for (auto& block : artifact.uniform_blocks) {
            block.members.clear();
            block.size = usize(512 * 16);
            for (unsigned member = 0; member < 512; ++member) {
                block.members.push(resource::ShaderArtifactUniformMember {
                    .name              = rstd::format("member_{}_{}", index, member),
                    .offset            = u32(member * 16),
                    .size              = usize(16),
                    .scalar_kind       = owe::ShaderScalarKind::Float,
                    .scalar_width      = u32(32),
                    .vector_components = u32(4),
                });
            }
        }
        artifacts.push(rstd::move(artifact));
    }
    auto                      samplers = MakeSamplers();
    auto                      vertices = MakeVertices();
    SceneDrawPreparationCache cache;
    auto warm_result = cache.prepare(artifacts[usize()], samplers.as_slice(), vertices.as_slice());
    auto direct_result =
        PrepareSceneDraw(artifacts[usize()], samplers.as_slice(), vertices.as_slice());
    ASSERT_TRUE(warm_result.is_ok());
    ASSERT_TRUE(direct_result.is_ok());
    auto warm   = rstd::move(warm_result).unwrap();
    auto direct = rstd::move(direct_result).unwrap();
    ExpectSamePreparation(*warm, *direct);

    auto started = rstd::time::Instant::now();
    for (unsigned iteration = 0; iteration < repetitions; ++iteration) {
        auto reflected = ShaderReflectionFromArtifact(artifacts[usize(iteration % 8)]);
        rstd::hint::black_box(reflected);
    }
    const auto reflection_time = started.elapsed();
    usize      direct_checksum;
    started = rstd::time::Instant::now();
    for (unsigned iteration = 0; iteration < repetitions; ++iteration) {
        const auto& artifact = artifacts[usize(iteration % 8)];
        auto        result   = PrepareSceneDraw(artifact, samplers.as_slice(), vertices.as_slice());
        ASSERT_TRUE(result.is_ok());
        auto prepared = rstd::move(result).unwrap();
        direct_checksum += prepared->samplers.len() + prepared->vertex_attributes.len();
        rstd::hint::black_box(prepared);
    }
    const auto direct_time = started.elapsed();
    usize      cached_checksum;
    started = rstd::time::Instant::now();
    for (unsigned iteration = 0; iteration < repetitions; ++iteration) {
        const auto& artifact = artifacts[usize(iteration % 8)];
        auto        result   = cache.prepare(artifact, samplers.as_slice(), vertices.as_slice());
        ASSERT_TRUE(result.is_ok());
        auto prepared = rstd::move(result).unwrap();
        cached_checksum += prepared->samplers.len() + prepared->vertex_attributes.len();
        rstd::hint::black_box(prepared);
    }
    const auto cached_time = started.elapsed();
    EXPECT_EQ(direct_checksum, usize(repetitions * 4));
    EXPECT_EQ(cached_checksum, direct_checksum);
    EXPECT_EQ(cache.stats().hits, u64(repetitions));
    EXPECT_EQ(cache.stats().misses, u64(1));
    EXPECT_EQ(cache.stats().entries, usize(1));
    rstd::io::println("draw preparation debug: iterations={}, artifacts=8, uniform members=1024, "
                      "reflection_us={}, uncached_us={}, cached_us={}, hits={}, misses={}",
                      repetitions,
                      reflection_time.as_micros(),
                      direct_time.as_micros(),
                      cached_time.as_micros(),
                      cache.stats().hits,
                      cache.stats().misses);
}
#endif
