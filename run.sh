#!/usr/bin/env bash
set -e

BUILD_TYPE="Debug"
if [[ "$1" == "release" ]]; then
    BUILD_TYPE="Release"
elif [[ "$1" == "relwithdebinfo" ]]; then
    BUILD_TYPE="RelWithDebInfo"
fi

cmake -S . -B _build -DCMAKE_BUILD_TYPE=$BUILD_TYPE
cmake --build _build
cd _build && ./main
