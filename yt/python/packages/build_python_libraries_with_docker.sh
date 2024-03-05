#!/bin/bash -eu

mkdir -p ../ya_build
rm -rf ../ya_build/*

YA_BUILD_PATH=$(realpath $(pwd)/../ya_build)

CONTAINER_ID=$(docker container run --name yson-bindings-build --entrypoint /bin/bash -v $SOURCE_ROOT:/ytsaurus -v $YA_BUILD_PATH:/ya_build -dit quay.io/pypa/manylinux2014_x86_64)

docker exec $CONTAINER_ID /ytsaurus/yt/python/packages/build_manylinux_yson_bindings.sh
