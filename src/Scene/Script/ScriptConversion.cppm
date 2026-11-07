module;

#include "quickjs.h"

export module wescene.script:conversion;
import wescene.script;
import wescene.json;

export namespace owe::script::conversion
{
ScriptValue CoerceReturn(JSContext*, JSValue, FieldKind);
JSValue     ScriptValueToJs(JSContext*, const ScriptValue&);
JSValue     JsonToJs(JSContext*, const Json&);
JSValue     UserPropertyValueToJs(JSContext*, const Json&);
JSValue     ResolveConfigValue(JSContext*, const Json&);
JSValue     CoerceInitialValue(JSContext*, const Json&, FieldKind);
} // namespace owe::script::conversion
