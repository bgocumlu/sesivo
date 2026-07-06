#!/usr/bin/env bash
set -euo pipefail

build_dir="${BUILD_DIR:-build}"
config="${CONFIG:-Release}"
package_dir="${PACKAGE_DIR:-${build_dir}/package}"

cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE="$config" -DJAM_BUILD_CLIENT=ON
cmake --build "$build_dir" --target client --config "$config"
rm -rf "$package_dir"
cpack --config "$build_dir/CPackConfig.cmake" -G DragNDrop -C "$config" -B "$package_dir"
