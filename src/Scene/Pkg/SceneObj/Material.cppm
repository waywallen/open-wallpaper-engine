export module wescene.pkg.scene_obj:material;
import rstd.cppstd;
import wescene.fs;
import :scene_document;
export import :field_binding;

export namespace owe

{
namespace wpscene
{

class MaterialPassBindItem {
public:
    bool        FromJson(const owe::Json&);
    std::string name;
    i32         index;
};

class MaterialPass {
public:
    bool                                                FromJson(const owe::Json&);
    void                                                Update(const MaterialPass&);
    u32                                                 id { 0 }; // pass id (PKGV0001+)
    std::vector<std::string>                            textures;
    rstd::json::Array                                   usertextures; // PKGV0018+; polymorphic
    std::unordered_map<std::string, i32>                combos;
    std::unordered_map<std::string, std::vector<float>> constantshadervalues;
    FieldBindings                                       constantshadervalues_bindings;
    // Legacy `usershadervalues`: project.json key -> shader material key.
    std::unordered_map<std::string, std::string> user_shader_values;
    std::string                                  target;
    std::vector<MaterialPassBindItem>            bind;
};

class Material : public rstd::DefaultInClass<Material, rstd::clone::Clone> {
public:
    Material()                               = default;
    Material(const Material&)                = delete;
    Material& operator=(const Material&)     = delete;
    Material(Material&&) noexcept            = default;
    Material& operator=(Material&&) noexcept = default;

    bool        FromJson(const owe::Json&);               // legacy
    bool        FromJson(const owe::Json&, SceneVersion); // canonical
    auto        clone() const -> Material;
    void        MergePass(const MaterialPass&);
    void        MergeBindingOverrides(const std::vector<std::string>&             textures,
                                      const rstd::json::Array&                    usertextures,
                                      const std::unordered_map<std::string, i32>& combos);
    std::string blending { "translucent" };
    std::string cullmode { "nocull" };
    std::string shader;
    std::string alphawriting { "default" };
    std::string depthtest { "disabled" };
    std::string depthwrite { "disabled" };
    std::vector<std::string>                            textures;
    rstd::json::Array                                   usertextures;
    std::unordered_map<std::string, i32>                combos;
    std::unordered_map<std::string, std::vector<float>> constantshadervalues;
    FieldBindings                                       constantshadervalues_bindings;
    std::unordered_map<std::string, std::string>        user_shader_values;

    bool use_puppet { false };
};

} // namespace wpscene
} // namespace owe
