module;
#include "quickjs.h"

export module wescene.script:programs;

export namespace owe::script::programs
{
const char*  BootstrapSource();
JSModuleDef* LoadBuiltinModule(JSContext*, const char*, void*);
} // namespace owe::script::programs
