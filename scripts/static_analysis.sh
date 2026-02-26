#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

if ! command -v cppcheck >/dev/null 2>&1; then
  echo "cppcheck not installed; skipping static analysis"
  exit 0
fi

cppcheck \
  --quiet \
  --error-exitcode=1 \
  --std=c++20 \
  --language=c++ \
  --enable=warning,style,performance,portability \
  --inline-suppr \
  -I frontend \
  -I lowering \
  -I runtime \
  -I src \
  frontend lowering runtime src

echo "static analysis passed"
