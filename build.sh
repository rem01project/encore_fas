#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
NDK_HOME=${ANDROID_NDK_HOME:-}
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build/android"}

if [ -z "$NDK_HOME" ] || [ ! -f "$NDK_HOME/build/cmake/android.toolchain.cmake" ]; then
    echo "ANDROID_NDK_HOME must point to an Android NDK installation" >&2
    exit 1
fi

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="${ANDROID_ABI:-arm64-v8a}" \
    -DANDROID_PLATFORM="${ANDROID_PLATFORM:-android-23}" 
    "$@"

cmake --build "$BUILD_DIR" --parallel
