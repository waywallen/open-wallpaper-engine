#include <rstd/macro.hpp>

import rstd;
import rstd.argparse;
import wescene.cli;

using namespace rstd::prelude;
using namespace rstd::literals;
using rstd::ffi::CStr;
using rstd::ffi::OsStr;
using rstd::ffi::OsString;

int main(int argc, char** argv) {
    if (argc > 1) {
        auto command = rstd::argparse::Command::make("cli-test"_str);
        auto value   = command.add_arg(
            rstd::argparse::Arg<OsString>::value("value"_str, rstd::argparse::os_string_parser())
                .long_name("value"_str)
                .required());
        auto parsed = owe::cli::ParseArgs(rstd::move(command), argc - 1, argv + 1);
        if (parsed.is_err()) return parsed.unwrap_err().code;
        auto argument = parsed->get_one(value).unwrap().unwrap();
        auto output   = rstd::io::stdout();
        rstd::io::write_all(output, argument->as_os_str().as_encoded_bytes()).unwrap();
        return 0;
    }

    auto executable = ref<OsStr>::from_encoded_bytes_unchecked(CStr::from_ptr(argv[0]).to_bytes());
    auto run        = [&](ref<str> argument) {
        return rstd::process::Command::make(executable)
            .arg("child"_str)
            .arg(argument)
            .output()
            .unwrap();
    };
    auto help = run("--help"_str);
    rstd_assert(help.status.success());
    rstd_assert(help.stderr_buf.is_empty());
    auto help_text = String::from_utf8(rstd::move(help.stdout_buf)).unwrap();
    rstd_assert(help_text.as_str().contains("--value"_str));
    rstd_assert(help_text.as_str().ends_with("\n"_str));
    rstd_assert(! help_text.as_str().ends_with("\n\n"_str));

    auto error = run("--unknown"_str);
    rstd_assert(error.status.code().unwrap() == i32(2));
    rstd_assert(error.stdout_buf.is_empty());
    auto error_text = String::from_utf8(rstd::move(error.stderr_buf)).unwrap();
    rstd_assert(error_text.as_str().contains("--unknown"_str));
    rstd_assert(error_text.as_str().ends_with("\n"_str));

    array<u8, 4> raw { u8('a'), u8(255), u8(' '), u8('b') };
    auto         path   = ref<OsStr>::from_encoded_bytes_unchecked(raw.as_slice());
    auto         output = rstd::process::Command::make(executable)
                              .arg("child"_str)
                              .arg("--value"_str)
                              .arg(path)
                              .output()
                              .unwrap();
    rstd_assert(output.status.success());
    rstd_assert(output.stderr_buf.is_empty());
    rstd_assert(output.stdout_buf.as_slice() == raw.as_slice());
}
