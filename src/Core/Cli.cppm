export module wescene.cli;

import rstd.argparse;
import rstd;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::ffi::CStr;
using rstd::ffi::OsStr;
using rstd::ffi::OsString;

export namespace owe::cli
{

struct ParseExit {
    int code;
};

} // namespace owe::cli

namespace
{

void WriteMessage(ref<str> text, rstd::argparse::OutputTarget::Tag target) {
    auto ending = text.ends_with("\n"_str) ? ""_str : "\n"_str;
    if (target == rstd::argparse::OutputTarget::Tag::Stdout)
        rstd::io::print { "{}{}", text, ending };
    else
        rstd::io::eprint { "{}{}", text, ending };
}

auto Build(rstd::argparse::Command&& command)
    -> Result<rstd::argparse::Parser, owe::cli::ParseExit> {
    auto built = rstd::move(command).build();
    if (built.is_ok()) return Ok(rstd::move(built).unwrap());

    auto message = rstd::format("error: invalid command definition: {}\n", built.unwrap_err());
    WriteMessage(message.as_str(), rstd::argparse::OutputTarget::Tag::Stderr);
    return Err(owe::cli::ParseExit { 2 });
}

auto Finish(rstd::argparse::Parser&              parser,
            Result<rstd::argparse::ParseOutcome<rstd::argparse::Matches>,
                   rstd::argparse::ParseError>&& parsed)
    -> Result<rstd::argparse::Matches, owe::cli::ParseExit> {
    if (parsed.is_err()) {
        auto report = parser.render_error(parsed.as_ref().unwrap_err());
        WriteMessage(report.text(), report.target());
        return Err(owe::cli::ParseExit { report.exit_code().to_primitive() });
    }

    auto outcome = rstd::move(parsed).unwrap();
    if (outcome.is_Display()) {
        auto request = rstd::move(outcome).as_Display().request;
        WriteMessage(request.text(), request.target());
        return Err(owe::cli::ParseExit { request.exit_code().to_primitive() });
    }
    return Ok(rstd::move(outcome).as_Parsed().value);
}

} // namespace

export namespace owe::cli
{

auto ParseEnv(rstd::argparse::Command&& command) -> Result<rstd::argparse::Matches, ParseExit> {
    auto built = Build(rstd::move(command));
    if (built.is_err()) return Err(built.unwrap_err());
    auto parser = rstd::move(built).unwrap();
    return Finish(parser, parser.parse_env());
}

auto ParseArgs(rstd::argparse::Command&& command, int argc, char** argv)
    -> Result<rstd::argparse::Matches, ParseExit> {
    auto arguments = Vec<OsString>::with_capacity(static_cast<usize>(argc));
    for (int i = 0; i < argc; ++i) {
        auto bytes = CStr::from_ptr(argv[i]).to_bytes();
        auto os    = ref<OsStr>::from_encoded_bytes_unchecked(bytes);
        arguments.push(OsString::from(os));
    }

    auto built = Build(rstd::move(command));
    if (built.is_err()) return Err(built.unwrap_err());
    auto parser = rstd::move(built).unwrap();
    return Finish(parser, parser.parse_from(rstd::move(arguments)));
}

} // namespace owe::cli
