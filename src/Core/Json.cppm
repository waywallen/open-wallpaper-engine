export module wescene.json;
export import rstd;
export import rstd.json;
import wescene.fs;

using namespace rstd::prelude;
using rstd::mtp::is_const;

export namespace owe
{

using Json = rstd::json::Value;

enum class JsonFileErrorKind : rstd::uint8_t
{
    Io,
    Parse,
};

struct JsonFileError {
    JsonFileErrorKind kind;
    String            message;
};

template<typename T>
struct JsonTemplateTypeCheck {
    using type = bool;
    static_assert(! is_const<T>, "GetJsonValue need a non const value");
};

template<typename T>
typename JsonTemplateTypeCheck<T>::type
GetJsonValue(const Json& json, T& value, source_location loc = source_location::current());

template<typename T>
typename JsonTemplateTypeCheck<T>::type
GetJsonValue(const Json& json, ref<str> name, T& value, bool warn = true,
             source_location loc = source_location::current());

enum class JsonValueError
{
    WrongType,
    WrongArraySize,
    InvalidNumber,
    NumberOutOfRange,
};

auto ParseJsonFloat(ref<str> value) -> Result<float, JsonValueError>;

auto ParseJson(ref<str> source, rstd::json::ParseOptions options = {}) -> rstd::json::ParseResult;
auto ReadJsonFile(fs::VFS& vfs, fs::Path path, rstd::json::ParseOptions options = {})
    -> rstd::Result<Json, JsonFileError>;
auto ReadAssetJsonFile(fs::VFS& vfs, ref<str> path, rstd::json::ParseOptions options = {})
    -> rstd::Result<Json, JsonFileError>;
auto DumpString(const Json& value, Option<usize> indent = None()) -> String;

} // namespace owe

export namespace rstd
{

template<>
struct Impl<fmt::Display, owe::JsonFileError> : ImplBase<owe::JsonFileError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return formatter.write_fmt(fmt::Arguments::make("{}", this->self().message));
    }
};

template<>
struct Impl<fmt::Debug, owe::JsonFileError> : ImplBase<owe::JsonFileError> {
    auto fmt(fmt::Formatter& formatter) const -> bool {
        return formatter.write_fmt(fmt::Arguments::make("JsonFileError(kind={}, message={})",
                                                        static_cast<int>(this->self().kind),
                                                        this->self().message));
    }
};

template<>
struct Impl<error::Error, owe::JsonFileError> : DefaultInImpl<error::Error, owe::JsonFileError> {};

} // namespace rstd

static_assert(rstd::Impled<owe::JsonFileError, rstd::error::Error>);
