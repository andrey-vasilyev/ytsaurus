# Generated by devtools/yamaker from nixpkgs 22.11.

LIBRARY()

LICENSE(BSL-1.0)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

VERSION(1.84.0)

ORIGINAL_SOURCE(https://github.com/boostorg/static_assert/archive/boost-1.84.0.tar.gz)

PEERDIR(
    contrib/restricted/boost/config
)

ADDINCL(
    GLOBAL contrib/restricted/boost/static_assert/include
)

NO_COMPILER_WARNINGS()

NO_UTIL()

END()
