#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

./scripts/format_check.sh

cmake -S . -B build-lint -DCMAKE_BUILD_TYPE=Release
cmake --build build-lint --parallel
ctest --test-dir build-lint --output-on-failure

echo "lint passed"
