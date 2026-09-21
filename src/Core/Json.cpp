module;

#include <rstd/macro.hpp>

module wescene.json;
import rstd;
import rstd.json;
import rstd.log;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::mtp::is_arithmetic;
using rstd::mtp::same;

namespace owe
{

namespace
{

template<typename>
struct JsonArrayTarget {
    static constexpr bool enabled = false;
};

template<typename T>
struct JsonArrayTarget<Vec<T>> {
    static constexpr bool enabled = true;
    static constexpr bool dynamic = true;
    using value_type              = T;
};

template<typename T, rstd::size_t N>
struct JsonArrayTarget<array<T, N>> {
    static constexpr bool enabled = true;
    static constexpr bool dynamic = false;
    using value_type              = T;
};

auto InitialJsonValue(const Json& json) -> const Json& {
    if (auto value = json.get("value"_str); value.is_some()) return **value;
    return json;
}

template<typename T>
auto ParseNumber(ref<str> value) -> Result<T, JsonValueError> {
    auto parsed = [&] {
        if constexpr (same<T, float>)
            return rstd::from_str<f32>(value);
        else if constexpr (same<T, double>)
            return rstd::from_str<f64>(value);
        else if constexpr (same<T, rstd::int32_t>)
            return rstd::from_str<i32>(value);
        else if constexpr (same<T, rstd::uint32_t>)
            return rstd::from_str<u32>(value);
        else
            return rstd::from_str<T>(value);
    }();
    if (parsed.is_err()) {
        auto error = rstd::move(parsed).unwrap_err();
        bool overflow;
        if constexpr (same<T, float> || same<T, double> || rstd::num::Float<T>) {
            using rstd::num::FloatErrorKind;
            overflow = error.kind() == FloatErrorKind::PosOverflow ||
                       error.kind() == FloatErrorKind::NegOverflow;
        } else {
            overflow = error.kind()->is_PosOverflow() || error.kind()->is_NegOverflow();
        }
        return Err(overflow ? JsonValueError::NumberOutOfRange : JsonValueError::InvalidNumber);
    }
    return Ok(rstd::as_cast<T>(rstd::move(parsed).unwrap()));
}

template<typename T>
auto ConvertNumber(const Json& json) -> Result<T, JsonValueError> {
    auto number = json.as_number();
    if (number.is_none()) return Err(JsonValueError::WrongType);
    if ((*number)->is_f64()) return Ok(rstd::as_cast<T>(*(*number)->as_f64()));
    if ((*number)->is_u64()) return Ok(rstd::as_cast<T>(*(*number)->as_u64()));
    return Ok(rstd::as_cast<T>(*(*number)->as_i64()));
}

class ArrayComponents {
public:
    explicit ArrayComponents(ref<str> value)
        : remaining_(Some(value)), comma_(value.contains(","_str)) {}

    auto Next() -> Option<ref<str>> {
        if (! remaining_) return None();
        auto value = *remaining_;
        if (auto split = value.split_once(comma_ ? ","_str : " "_str)) {
            auto [head, tail] = *split;
            value             = head;
            remaining_        = Some(tail);
        } else {
            remaining_ = None();
        }
        return Some(comma_ ? value.trim_ascii() : value);
    }

private:
    Option<ref<str>> remaining_;
    bool             comma_;
};

auto ArrayComponentCount(ref<str> value) -> usize {
    usize           count;
    ArrayComponents components(value);
    while (components.Next().is_some()) ++count;
    return count;
}

template<typename T>
auto ConvertArray(ref<str> value, Vec<T>& target) -> Result<bool, JsonValueError> {
    const auto count = ArrayComponentCount(value);
    // Preserve existing slots and preallocate before parsing, including on failure.
    if (target.len() < count) target.resize(count, T {});
    usize           index {};
    ArrayComponents components(value);
    while (auto component = components.Next())
        target[index++] = rstd_try(ParseNumber<T>(*component));
    return Ok(true);
}

template<typename T, rstd::size_t N>
auto ConvertArray(ref<str> value, array<T, N>& target) -> Result<bool, JsonValueError> {
    if (ArrayComponentCount(value) != usize(N)) return Err(JsonValueError::WrongArraySize);
    usize           index {};
    ArrayComponents components(value);
    while (auto component = components.Next())
        target[index++] = rstd_try(ParseNumber<T>(*component));
    return Ok(true);
}

template<typename T>
auto ReadJsonValue(const Json& json, T& value) -> Result<bool, JsonValueError> {
    const auto& input = InitialJsonValue(json);
    if constexpr (JsonArrayTarget<T>::enabled) {
        using Value = typename JsonArrayTarget<T>::value_type;
        if (input.is_number()) {
            if constexpr (JsonArrayTarget<T>::dynamic) {
                value.clear();
                value.push(rstd_try(ConvertNumber<Value>(input)));
            } else {
                bool first = true;
                for (auto& item : value) {
                    item  = first ? rstd_try(ConvertNumber<Value>(input)) : Value {};
                    first = false;
                }
            }
            return Ok(true);
        }
        if (auto array = input.as_array(); array.is_some()) {
            if constexpr (JsonArrayTarget<T>::dynamic) {
                value.clear();
                for (const auto& item : **array) value.push(rstd_try(ConvertNumber<Value>(item)));
            } else {
                usize count {};
                for (auto& item : value) {
                    if (count >= (*array)->len()) return Err(JsonValueError::WrongArraySize);
                    item = rstd_try(ConvertNumber<Value>((**array)[count]));
                    ++count;
                }
                if (count != (*array)->len()) return Err(JsonValueError::WrongArraySize);
            }
            return Ok(true);
        }
        auto string = input.as_str();
        if (string.is_none()) return Err(JsonValueError::WrongType);
        return ConvertArray(*string, value);
    } else if constexpr (same<T, bool>) {
        auto boolean = input.as_bool();
        if (boolean.is_none()) return Err(JsonValueError::WrongType);
        value = *boolean;
        return Ok(true);
    } else if constexpr (rstd::num::Numeric<T>) {
        auto boolean = input.as_bool();
        value        = boolean.is_some() ? rstd::as_cast<T>(static_cast<rstd::uint8_t>(*boolean))
                                         : rstd_try(ConvertNumber<T>(input));
        return Ok(true);
    } else if constexpr (is_arithmetic<T>) {
        auto boolean = input.as_bool();
        value = boolean.is_some() ? static_cast<T>(*boolean) : rstd_try(ConvertNumber<T>(input));
        return Ok(true);
    } else if constexpr (same<T, String>) {
        auto string = input.as_str();
        if (string.is_none()) return Err(JsonValueError::WrongType);
        value = rstd::into(*string);
        return Ok(true);
    }
}

template<typename T>
auto ReadJsonValue(const Json& json, T& value, Option<ref<str>> name, source_location loc) -> bool {
    auto result = ReadJsonValue(json, value);
    if (result.is_ok()) return result.unwrap();
    auto     error     = rstd::move(result).unwrap_err();
    auto     name_info = name.is_some() ? rstd::format("(key: {})", *name) : String {};
    ref<str> message;
    switch (error) {
    case JsonValueError::WrongType: message = "Wrong json value type"_str; break;
    case JsonValueError::WrongArraySize: message = "Wrong size of the array"_str; break;
    case JsonValueError::InvalidNumber: message = "invalid number"_str; break;
    case JsonValueError::NumberOutOfRange: message = "number out of range"_str; break;
    }
    if (error == JsonValueError::WrongType) {
        rstd_info("{} {} at {} {}:{}\n{}",
                  message,
                  name_info,
                  loc.function_name(),
                  loc.file_name(),
                  loc.line(),
                  DumpString(json, Some(usize(4))));
    } else {
        rstd_error("{} {} at {} {}:{}",
                   message,
                   name_info,
                   loc.function_name(),
                   loc.file_name(),
                   loc.line());
    }
    return false;
}

} // namespace

auto ParseJsonFloat(ref<str> value) -> Result<float, JsonValueError> {
    return ParseNumber<float>(value);
}

template<typename T>
typename JsonTemplateTypeCheck<T>::type GetJsonValue(const Json& json, T& value,
                                                     source_location loc) {
    return ReadJsonValue(json, value, None(), loc);
}

template<typename T>
typename JsonTemplateTypeCheck<T>::type GetJsonValue(const Json& json, ref<str> name_view, T& value,
                                                     bool warn, source_location loc) {
    auto member = json.get(name_view);
    if (member.is_none()) {
        if (warn)
            rstd_info("read json \"{}\" not a key at {}({}:{})",
                      name_view,
                      loc.function_name(),
                      loc.file_name(),
                      loc.line());
        return false;
    }
    if ((*member)->is_null()) {
        if (warn)
            rstd_info("read json \"{}\" is null at {}({}:{})",
                      name_view,
                      loc.function_name(),
                      loc.file_name(),
                      loc.line());
        return false;
    }
    return ReadJsonValue(**member, value, Some(name_view), loc);
}

#define OWE_IMPL_GET_JSON(TYPE)                                    \
    template JsonTemplateTypeCheck<TYPE>::type GetJsonValue<TYPE>( \
        const Json&, TYPE&, source_location);                      \
    template JsonTemplateTypeCheck<TYPE>::type GetJsonValue<TYPE>( \
        const Json&, ref<str>, TYPE&, bool, source_location)

OWE_IMPL_GET_JSON(bool);
OWE_IMPL_GET_JSON(i32);
OWE_IMPL_GET_JSON(u32);
OWE_IMPL_GET_JSON(rstd::int32_t);
OWE_IMPL_GET_JSON(rstd::uint32_t);
OWE_IMPL_GET_JSON(float);
OWE_IMPL_GET_JSON(double);
OWE_IMPL_GET_JSON(String);
OWE_IMPL_GET_JSON(Vec<float>);
OWE_IMPL_GET_JSON(Vec<rstd::int32_t>);
OWE_IMPL_GET_JSON(Vec<i32>);
OWE_IMPL_GET_JSON(Vec<u32>);

using RstdI32Array3 = array<i32, 3>;
OWE_IMPL_GET_JSON(RstdI32Array3);

using RstdIntArray3   = array<int, 3>;
using RstdFloatArray2 = array<float, 2>;
using RstdFloatArray3 = array<float, 3>;
OWE_IMPL_GET_JSON(RstdIntArray3);
OWE_IMPL_GET_JSON(RstdFloatArray2);
OWE_IMPL_GET_JSON(RstdFloatArray3);

#undef OWE_IMPL_GET_JSON

auto ParseJson(ref<str> source, rstd::json::ParseOptions options) -> rstd::json::ParseResult {
    return rstd::json::from_str(source, options);
}

auto ReadJsonFile(fs::VFS& vfs, fs::Path path, rstd::json::ParseOptions options)
    -> rstd::Result<Json, JsonFileError> {
    auto io_error = [](auto error) {
        return JsonFileError { JsonFileErrorKind::Io, rstd::format("{}", error) };
    };
    auto parse_error = [](auto error) {
        return JsonFileError { JsonFileErrorKind::Parse, rstd::format("{}", error) };
    };
    auto content = rstd_try(fs::ReadFileContent(vfs, path), io_error);
    auto parsed  = rstd_try(ParseJson(content.as_str(), options), parse_error);
    return Ok(rstd::move(parsed));
}

auto ReadAssetJsonFile(fs::VFS& vfs, ref<str> path, rstd::json::ParseOptions options)
    -> rstd::Result<Json, JsonFileError> {
    auto resolved = fs::ResolveAssetPath(path);
    if (resolved.is_err()) {
        auto error = rstd::move(resolved).unwrap_err_unchecked();
        return Err(JsonFileError { JsonFileErrorKind::Io, rstd::format("{}", error) });
    }
    return ReadJsonFile(vfs, resolved->as_path(), options);
}

auto DumpString(const Json& value, Option<usize> indent) -> String {
    auto options = rstd::json::FormatOptions {};
    if (indent) {
        options.pretty = true;
        options.indent = *indent;
    }
    return rstd::json::to_string(value, options);
}

} // namespace owe
