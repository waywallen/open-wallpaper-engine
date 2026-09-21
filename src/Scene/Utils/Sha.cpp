module;
#include <vog/sha1.hpp>

module wescene.utils;
import rstd;
import rstd.cppstd;

using namespace rstd::prelude;

String utils::genSha1(slice<rstd::byte> input) {
    SHA1 sha1;
    if (! input.is_empty()) {
        sha1.update(std::string(reinterpret_cast<const char*>(input.as_raw_ptr()),
                                input.len().to_primitive()));
    }
    auto digest = sha1.final();
    return rstd::into(rstd::cppstd::as_str(digest).unwrap());
}
