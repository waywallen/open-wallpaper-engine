#include <rstd/test/gtest.hpp>

import rstd;
import wescene.utils;

using namespace rstd::prelude;
using namespace rstd::literals;

TEST(Sha1, PreservesKnownDigestsAndBinaryLength) {
    EXPECT_EQ(utils::genSha1(rstd::as_bytes(""_str.as_bytes())),
              "da39a3ee5e6b4b0d3255bfef95601890afd80709"_str);
    EXPECT_EQ(utils::genSha1(rstd::as_bytes("abc"_str.as_bytes())),
              "a9993e364706816aba3e25717850c26c9cd0d89d"_str);
    EXPECT_EQ(utils::genSha1(rstd::as_bytes("a\0b"_str.as_bytes())),
              "4a3dec2d1f8245280855c42db0ee4239f917fdb8"_str);
    EXPECT_EQ(utils::genSha1(rstd::as_bytes("\xe4\xbd\xa0\xe5\xa5\xbd"_str.as_bytes())),
              "440ee0853ad1e99f962b63e459ef992d7c211722"_str);
}

TEST(Sha1, HashesOnlyBorrowedSubstring) {
    auto owner  = "abcdef"_Str;
    auto prefix = owner.as_str().get(usize(), usize(3)).unwrap();
    auto digest = utils::genSha1(rstd::as_bytes(prefix.as_bytes()));
    owner       = "replaced"_Str;
    EXPECT_EQ(digest, "a9993e364706816aba3e25717850c26c9cd0d89d"_str);
}
