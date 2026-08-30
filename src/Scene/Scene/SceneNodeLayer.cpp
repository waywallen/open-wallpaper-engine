module;

#include <rstd/macro.hpp>

module wescene.scene;
import wescene.spec_names;
import wescene.core;
import wescene.types;
import rstd;
import rstd.cppstd;

using namespace rstd::prelude;
using namespace owe;

namespace
{

void ChangeMeshToUnitQuad(SceneMesh& target) {
    SceneMesh mesh;
    // clang-format off
    const rstd::array<float, 12> pos = {
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
    };
    const rstd::array<float, 8> tex_coord = {
        0.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 0.0f,
        1.0f, 1.0f,
    };
    // clang-format on

    SceneVertexArray vertex(MakeAttrSet({ VAttr::Position, VAttr::TexCoord }), usize(4));
    vertex.SetVertex(rstd::cppstd::as_string_view(WE_IN_POSITION), pos.as_slice());
    vertex.SetVertex(rstd::cppstd::as_string_view(WE_IN_TEXCOORD), tex_coord.as_slice());
    mesh.AddVertexArray(std::move(vertex));
    target.ChangeMeshDataFrom(mesh);
}

} // namespace

SceneNodeLayer::SceneNodeLayer(SceneNode* node, float w, float h, std::string_view composite_target)
    : m_worldNode(node),
      m_sourceNode(node),
      m_width(w),
      m_height(h),
      m_composite_target(composite_target),
      m_source_camera(node != nullptr ? node->Camera() : std::string()),
      m_final_mesh(Box<SceneMesh>::make()) {};

void SceneNodeLayer::SetSourceDraw(SceneNode& node) {
    m_sourceNode    = &node;
    m_source_camera = node.Camera();
}

void SceneNodeLayer::ConfigureSourceDraw(bool intermediate) {
    if (m_sourceNode == nullptr) return;
    if (intermediate) {
        m_sourceNode->SetCamera(m_source_camera);
        return;
    }
    if (! m_final_camera.empty()) {
        m_sourceNode->SetCamera(m_final_camera);
    } else {
        m_sourceNode->SetCamera(m_sourceNode->Perspective() ? "global_perspective" : "");
    }
}

void SceneNodeLayer::ResolveEffect(const SceneMesh& default_mesh, std::string_view effect_cam) {
    if (m_resolved) return;
    m_resolved_effects.clear();
    m_direct_final_output = nullptr;
    auto default_node     = SceneNode();

    SceneImageEffectNode* last_output { nullptr };
    auto                  resolve_effect = [&](SceneImageEffect& eff) {
        SceneImageEffectNode* effect_output { nullptr };
        for (auto it = eff.nodes.begin(); it != eff.nodes.end(); it++) {
            rstd_assert(it->sceneNode->HasMaterial());
            auto& material = *(it->sceneNode->Mesh()->Material());
            if (it->output.kind == SceneEffectTargetKind::LayerNext) last_output = &(*it);
            effect_output = &(*it);

            {
                material.blenmode = BlendMode::Normal;
                it->sceneNode->SetCamera(effect_cam.data());
                it->sceneNode->CopyTrans(default_node);
                it->sceneNode->Mesh()->ChangeMeshDataFrom(default_mesh);
            }
        }
        m_resolved_effects.push_back(&eff);
        return effect_output;
    };
    for (auto& eff : m_effects) {
        if (eff && eff->runtime_visible) resolve_effect(*eff);
    }
    SceneImageEffectNode* final_resolve_output { nullptr };
    if (m_final_resolve_effect) final_resolve_output = resolve_effect(*m_final_resolve_effect);
    SceneImageEffectNode* published_output { nullptr };
    if (m_published_effect) published_output = resolve_effect(*m_published_effect);
    SceneImageEffectNode* visible_output { nullptr };
    if (m_visible_output_enabled && m_visible_resolve_effect)
        visible_output = resolve_effect(*m_visible_resolve_effect);

    auto* final_output = visible_output != nullptr
                             ? visible_output
                             : (final_resolve_output != nullptr
                                    ? final_resolve_output
                                    : (published_output == nullptr ? last_output : nullptr));
    if (final_output != nullptr) {
        m_direct_final_output = final_output;
        auto& mesh            = *(final_output->sceneNode->Mesh());
        auto& material        = *mesh.Material();
        material.blenmode     = m_final_blend;
        material.depth_test   = m_final_depth_test;
        material.depth_write  = m_final_depth_write;
        material.cull_mode    = m_final_cull_mode;
        if (m_final_local) {
            final_output->sceneNode->SetCamera(std::string(effect_cam));
            final_output->sceneNode->SetParentAnchor(nullptr);
            final_output->sceneNode->CopyTrans(default_node);
            mesh.ChangeMeshDataFrom(default_mesh);
        } else if (fullscreen) {
            final_output->sceneNode->SetCamera(std::string(effect_cam));
            final_output->sceneNode->SetParentAnchor(nullptr);
            final_output->sceneNode->CopyTrans(default_node);
            mesh.ChangeMeshDataFrom(default_mesh);
        } else {
            const bool perspective = m_worldNode != nullptr && m_worldNode->Perspective();
            final_output->sceneNode->SetCamera(m_final_camera.empty()
                                                   ? (perspective ? "global_perspective" : "")
                                                   : m_final_camera);
            final_output->sceneNode->SetPerspective(perspective);
            // Anchor to the layer's primary SceneNode so the composite quad
            // inherits the layer's world transform (including any container
            // parent chain) via ModelTrans. Identity local — no CopyTrans dance.
            final_output->sceneNode->SetParentAnchor(m_worldNode);
            if (final_output->uses_unit_final_quad) {
                final_output->sceneNode->SetTranslate({ -m_width * 0.5f, -m_height * 0.5f, 0.0f });
                final_output->sceneNode->SetScale({ m_width, m_height, 1.0f });
                ChangeMeshToUnitQuad(mesh);
            } else {
                mesh.ChangeMeshDataFrom(*m_final_mesh.as_ptr());
            }
            final_output->final_quad_shader_values.iter().for_each([&](auto entry) {
                auto [name, value] = entry;
                material.SetShaderValue(rstd::cppstd::to_string(name->as_str()), value->base);
                if (value->track.is_some() && ! (**value->track).Empty()) {
                    (void)material.customShader.valueAnimations.insert(name->clone(),
                                                                       value->Share());
                } else {
                    (void)material.customShader.valueAnimations.remove(name->as_str());
                }
            });
        }
        final_output->sceneNode->SetAlphaSource(m_worldNode);
    }
    m_resolved = true;
}
