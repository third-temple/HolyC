#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

./scripts/format_check.sh

cmake_args=(-DCMAKE_BUILD_TYPE=Release)
if [[ -n "${HOLYC_BUNDLED_LLVM_CONFIG_DIR:-}" ]]; then
  cmake_args+=("-DHOLYC_BUNDLED_LLVM_CONFIG_DIR=${HOLYC_BUNDLED_LLVM_CONFIG_DIR}")
fi

cmake -S . -B build-lint "${cmake_args[@]}"
cmake --build build-lint --parallel
ctest --test-dir build-lint --output-on-failure

echo "lint passed"
