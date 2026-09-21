export module wescene.pkg.scene_obj:material;
import rstd;

import wescene.fs;
import :scene_document;
export import :field_binding;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::collections::HashMap;

export namespace owe

{
namespace wpscene
{

class MaterialPassBindItem {
public:
    bool   FromJson(const owe::Json&);
    String name;
    i32    index;
};

class MaterialPass {
public:
    bool                        FromJson(const owe::Json&);
    void                        Update(const MaterialPass&);
    u32                         id { 0 }; // pass id (PKGV0001+)
    Vec<String>                 textures;
    rstd::json::Array           usertextures; // PKGV0018+; polymorphic
    HashMap<String, i32>        combos;
    HashMap<String, Vec<float>> constantshadervalues;
    FieldBindings               constantshadervalues_bindings;
    // Legacy `usershadervalues`: project.json key -> shader material key.
    HashMap<String, String>   user_shader_values;
    String                    target;
    Vec<MaterialPassBindItem> bind;
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
    void        MergeBindingOverrides(slice<String> textures, const rstd::json::Array& usertextures,
                                      const HashMap<String, i32>& combos);
    String      blending { "translucent"_Str };
    String      cullmode { "nocull"_Str };
    String      shader;
    String      alphawriting { "default"_Str };
    String      depthtest { "disabled"_Str };
    String      depthwrite { "disabled"_Str };
    Vec<String> textures;
    rstd::json::Array           usertextures;
    HashMap<String, i32>        combos;
    HashMap<String, Vec<float>> constantshadervalues;
    FieldBindings               constantshadervalues_bindings;
    HashMap<String, String>     user_shader_values;

    bool use_puppet { false };
};

} // namespace wpscene
} // namespace owe
