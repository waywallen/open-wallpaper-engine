module;

export module wescene.scene:lighting;
import eigen;
import rstd;
import :visibility;

using namespace rstd::prelude;
using rstd::sync::Arc;

// SceneNode lives in the primary interface unit of wescene.scene. SceneLight
// only needs a raw observation pointer here; ownership is carried by the
// scene tree.
export namespace owe
{

class SceneNode;

enum class SceneLightType : rstd::uint8_t
{
    Point       = 0,
    Spot        = 1,
    Directional = 2,
    Tube        = 3,
};

class SceneLight {
public:
    struct Parameters {
        float           radius {};
        float           intensity { 1.0f };
        float           exponent { 1.0f };
        Eigen::Vector3f control_point { 0.0f, 0.0f, 0.0f };
    };

    struct Desc {
        SceneLightType  type { SceneLightType::Point };
        Eigen::Vector3f color { 1.0f, 1.0f, 1.0f };
        Eigen::Vector3f control_point { 0.0f, 0.0f, 0.0f };
        float           radius { 0.0f };
        float           intensity { 1.0f };
        float           exponent { 1.0f };
        float           attenuation { 0.0f };
        float           mindistance { 0.0f };
        // Cone angle cosines. Identity (1.0) = no falloff.
        float inner_cone_cos { 1.0f };
        float outer_cone_cos { 1.0f };
        float light_source_size { 0.0f };
        float cascade_distances[3] { 0.0f, 0.0f, 0.0f };
        bool  cast_shadow { false };
        bool  cast_volumetrics { false };
        float volumetric_density { 0.0f };
        float volumetric_exponent { 1.0f };
    };

    explicit SceneLight(const Desc& d)
        : m_desc(d),
          m_parameters(Arc<Parameters>::make(Parameters { .radius        = d.radius,
                                                          .intensity     = d.intensity,
                                                          .exponent      = d.exponent,
                                                          .control_point = d.control_point })) {}
    ~SceneLight() = default;

    const Desc&     desc() const { return m_desc; }
    SceneLightType  type() const { return m_desc.type; }
    Eigen::Vector3f color() const { return m_desc.color * m_parameters->intensity; }
    float           radius() const { return m_parameters->radius; }
    float           intensity() const { return m_parameters->intensity; }
    float           exponent() const { return m_parameters->exponent; }
    Eigen::Vector3f controlPoint() const { return m_parameters->control_point; }
    auto            ParametersOwner() const -> Arc<Parameters> { return m_parameters.clone(); }
    SceneNode*      node() const { return m_node; }
    // Legacy uniform G_LCP layout (color * intensity * radius²) consumed by
    // shaders that bind g_LightsColorPremultiplied.
    Eigen::Vector3f premultipliedColor() const { return color() * radius() * radius(); }

    void setNode(SceneNode* node) { m_node = node; }

    ref<str> visibleUserKey() const { return m_visible_user_binding.key.as_str(); }
    void     setVisibleUserKey(String key) {
        m_visible_user_binding = SceneUserVisibilityBinding { .key = rstd::move(key) };
    }
    const SceneUserVisibilityBinding& visibleUserBinding() const { return m_visible_user_binding; }
    void                              setVisibleUserBinding(SceneUserVisibilityBinding binding) {
        m_visible_user_binding = rstd::move(binding);
    }
    bool runtimeVisible() const { return m_runtime_visible; }
    void setRuntimeVisible(bool v) { m_runtime_visible = v; }

private:
    Desc            m_desc;
    Arc<Parameters> m_parameters;
    SceneNode*      m_node { nullptr };

    SceneUserVisibilityBinding m_visible_user_binding {};
    bool                       m_runtime_visible { true };
};

} // namespace owe
