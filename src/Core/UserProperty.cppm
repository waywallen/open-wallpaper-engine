export module owe.user_property;

import rstd;

import wescene.json;
using namespace rstd::prelude;

export namespace owe
{

Json MakeUserPropertyWirePatch(ref<str> value);
Json MergeUserPropertyDescriptor(const Json& schema, const Json& patch);

} // namespace owe
