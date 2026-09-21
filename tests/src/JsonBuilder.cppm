export module wescene.testing.json_builder;

import rstd.cppstd;
export import wescene.json;
using namespace rstd::literals;

export namespace owe
{

auto MakeObject() -> Json { return Json::Object(rstd::json::Map::make()); }
auto MakeArray() -> Json { return Json::Array(rstd::json::Array::make()); }

inline auto IntoJson(Json value) -> Json { return value; }
inline auto IntoJson(std::string_view value) -> Json {
    return rstd::into<owe::Json>(rstd::string::String::make(rstd::cppstd::as_str(value).unwrap()));
}
inline auto IntoJson(const std::string& value) -> Json {
    return rstd::into<owe::Json>(rstd::string::String::make(rstd::cppstd::as_str(value).unwrap()));
}
inline auto IntoJson(const char* value) -> Json {
    return rstd::into<owe::Json>(rstd::string::String::make(rstd::cppstd::as_str(value).unwrap()));
}

template<typename T>
    requires std::is_arithmetic_v<std::remove_cvref_t<T>>
auto IntoJson(T value) -> Json {
    using Type = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<Type, bool>)
        return rstd::into<Json>(bool { value });
    else if constexpr (std::is_integral_v<Type> && std::is_signed_v<Type>)
        return rstd::into<Json>(static_cast<rstd::i64>(value));
    else if constexpr (std::is_integral_v<Type>)
        return rstd::into<Json>(static_cast<rstd::u64>(value));
    else
        return rstd::into<Json>(static_cast<rstd::f64>(value));
}

template<typename T>
    requires requires(T value) { value.to_primitive(); }
auto IntoJson(T value) -> Json {
    return IntoJson(value.to_primitive());
}

bool SetJson(Json& object, std::string_view key, Json value) {
    if (object.is_null()) object = MakeObject();
    auto values = object.as_object_mut();
    if (values.is_none()) return false;
    (*values)->insert(::alloc::string::String::make(rstd::cppstd::as_str(key).unwrap()),
                      rstd::move(value));
    return true;
}

bool AppendJson(Json& array, Json value) {
    auto values = array.as_array_mut();
    if (values.is_none()) return false;
    (*values)->push(rstd::move(value));
    return true;
}

template<typename T>
bool SetMember(Json& object, std::string_view key, T&& value) {
    return SetJson(object, key, IntoJson(std::forward<T>(value)));
}

template<typename T>
bool AppendElement(Json& array, T&& value) {
    return AppendJson(array, IntoJson(std::forward<T>(value)));
}

template<typename... T>
auto MakeArray(T&&... values) -> Json
    requires(sizeof...(T) > 0)
{
    auto array = MakeArray();
    (AppendElement(array, std::forward<T>(values)), ...);
    return array;
}

} // namespace owe
