module wescene.utils;
import rstd;
import licrypto;

using namespace rstd::prelude;

String utils::genSha1(slice<rstd::byte> input) {
    return licrypto::sha1_hex(rstd::as_u8_slice(input));
}
