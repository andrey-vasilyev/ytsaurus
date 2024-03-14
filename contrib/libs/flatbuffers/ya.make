# Generated by devtools/yamaker from nixpkgs 22.11.

LIBRARY()

VERSION(23.5.26)

ORIGINAL_SOURCE(https://github.com/google/flatbuffers/archive/v23.5.26.tar.gz)

LICENSE(Apache-2.0)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/restricted/abseil-cpp/absl/base
)

ADDINCL(
    contrib/libs/flatbuffers/include
)

NO_COMPILER_WARNINGS()

NO_UTIL()

CFLAGS(
    -DFLATBUFFERS_LOCALE_INDEPENDENT=1
)

SRCS(
    src/idl_gen_text.cpp
    src/idl_parser.cpp
    src/reflection.cpp
    src/util.cpp
)

END()

RECURSE(
    flatc
)
