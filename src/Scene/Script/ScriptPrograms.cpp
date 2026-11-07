module;
#include "quickjs.h"

module wescene.script;
import :programs;
import rstd;
import rstd.cppstd;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::ffi::CStr;

namespace owe::script::programs
{
namespace
{

constexpr unsigned char kBootstrapJs[] = {
#embed "programs/bootstrap.js" suffix(, 0)
};

struct BuiltinModule {
    const char*          name;
    const unsigned char* source;
};

constexpr unsigned char kWEMathSrc[] = {
#embed "programs/WEMath.js" suffix(, 0)
};

// WE's `WEVector` module. angleVector2 and vectorAngle2 use degrees and the
// standard x = cos, y = sin axes. Other helpers are unambiguous vector math.
constexpr unsigned char kWEVectorSrc[] = {
#embed "programs/WEVector.js" suffix(, 0)
};

constexpr unsigned char kWEColorSrc[] = {
#embed "programs/WEColor.js" suffix(, 0)
};

static constexpr BuiltinModule kBuiltinModules[] = {
    { "WEMath", kWEMathSrc },
    { "WEVector", kWEVectorSrc },
    { "WEColor", kWEColorSrc },
};

} // namespace

const char* BootstrapSource() { return reinterpret_cast<const char*>(kBootstrapJs); }

JSModuleDef* LoadBuiltinModule(JSContext* ctx, const char* module_name, void*) {
    if (! module_name) return nullptr;
    auto name = rstd::cppstd::as_str(module_name).unwrap();
    if (name.ends_with(".js"_str)) {
        name = name.get(usize(0), name.len() - usize(3)).unwrap();
    }
    for (const auto& m : kBuiltinModules) {
        auto m_name = rstd::cppstd::as_str(m.name).unwrap();
        bool match  = false;
        if (name.len() == m_name.len()) {
            match = true;
            for (usize i(0); i < name.len(); ++i) {
                if (std::tolower(static_cast<unsigned char>(name[i].to_primitive())) !=
                    std::tolower(static_cast<unsigned char>(m_name[i].to_primitive()))) {
                    match = false;
                    break;
                }
            }
        }
        if (match) {
            const auto* source   = reinterpret_cast<const char*>(m.source);
            JSValue     compiled = JS_Eval(ctx,
                                           source,
                                           CStr::from_ptr(source).to_bytes().len().to_primitive(),
                                           module_name,
                                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
            if (JS_IsException(compiled)) {
                // JS_Eval already set the pending exception; QuickJS propagates.
                return nullptr;
            }
            JSModuleDef* def = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
            // Don't free `compiled` — the pointer is the live module def.
            return def;
        }
    }
    JS_ThrowReferenceError(ctx, "could not load module '%s'", module_name);
    return nullptr;
}

} // namespace owe::script::programs
