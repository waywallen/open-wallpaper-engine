export module wescene.scene:uniform;
export import vrento.uniform_value;
export import vrento.uniform_source;
import :id;
import :runtime;
import eigen;
import rstd;
import rstd.cppstd;
import wescene.core;

using namespace rstd::prelude;
using rstd::collections::HashMap;

export namespace owe
{

using vrento::UniformMatrixStorage;
using vrento::UniformScalarType;
using vrento::UniformValueKind;
using vrento::UniformValueLayout;
using vrento::UniformValueView;

class ShaderValue : public vrento::UniformValue {
public:
    using vrento::UniformValue::UniformValue;
    ShaderValue() = default;
    ShaderValue(vrento::UniformValue value): vrento::UniformValue(rstd::move(value)) {}

    static ShaderValue fromMatrix(const Eigen::Ref<const Eigen::MatrixXf>& mat) {
        return vrento::UniformValue::fromMatrixArray(mat.data(),
                                                     u32(static_cast<rstd::uint32_t>(mat.rows())),
                                                     u32(static_cast<rstd::uint32_t>(mat.cols())),
                                                     usize(1),
                                                     UniformMatrixStorage::ColumnMajor);
    }
    static ShaderValue fromMatrix(const Eigen::Ref<const Eigen::MatrixXd>& mat) {
        Eigen::MatrixXf converted = mat.cast<float>();
        return fromMatrix(converted);
    }
};

using UniformValue   = ShaderValue;
using ShaderValues   = Map<std::string, ShaderValue>;
using ShaderValueMap = ShaderValues;

using vrento::UniformBlockDefinition;
using vrento::UniformBlockScope;
using vrento::UniformError;
using vrento::UniformOutputId;
using vrento::UniformSourceAttachment;
using vrento::UniformSourceId;
using vrento::UniformValueShape;
using SceneRenderViewKind = vrento::RenderViewKind;
using vrento::UniformBindingLease;
using vrento::UniformBindingSink;
using vrento::UniformResourceView;
using vrento::UniformSource;
using vrento::UniformSourceRegistrar;
using vrento::UniformTextureView;
using vrento::UniformUpdateContext;
using vrento::UniformValueSink;

struct UniformAttachmentWriter {
    using Trait                  = UniformAttachmentWriter;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = UniformAttachmentWriter;

        bool AttachGlobal(UniformSourceId source, i32 priority = i32()) {
            return rstd::trait_call<0>(this, source, priority);
        }
        bool AttachNode(SceneNodeId node, UniformSourceId source, i32 priority = i32()) {
            return rstd::trait_call<1>(this, node, source, priority);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::AttachGlobal, &T::AttachNode>;
};

struct UniformSourceCatalog {
    using Trait                  = UniformSourceCatalog;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = UniformSourceCatalog;

        auto Resolve(UniformSourceId id) const -> Option<ref<dyn<UniformSource>>> {
            return rstd::trait_call<0>(this, id);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::Resolve>;
};

struct UniformAttachmentCatalog {
    using Trait                  = UniformAttachmentCatalog;
    static constexpr bool direct = false;

    template<typename Self, typename = void>
    struct Api {
        using Trait = UniformAttachmentCatalog;

        auto GlobalSources() const -> slice<UniformSourceAttachment> {
            return rstd::trait_call<0>(this);
        }
        auto NodeSources(SceneNodeId node) const -> slice<UniformSourceAttachment> {
            return rstd::trait_call<1>(this, node);
        }
    };

    template<typename T>
    using Funcs = TraitFuncs<&T::GlobalSources, &T::NodeSources>;
};

using SceneUniformRegistry = vrento::UniformRegistry<SceneNodeId>;

} // namespace owe
