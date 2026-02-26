#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

TARGETS=(
  CMakeLists.txt
  cmake
  docs
  frontend
  lowering
  runtime
  scripts
  src
  tests
)

EXISTING_TARGETS=()
for target in "${TARGETS[@]}"; do
  if [[ -e "${target}" ]]; then
    EXISTING_TARGETS+=("${target}")
  fi
done

if rg --hidden --glob '!third_party/**' --glob '!.git/**' --glob '!build*/**' -n '[[:blank:]]+$' "${EXISTING_TARGETS[@]}"; then
  echo "format check failed: trailing whitespace detected" >&2
  exit 1
fi

if rg --hidden --glob '!third_party/**' --glob '!.git/**' --glob '!build*/**' -n $'\t' frontend lowering runtime src; then
  echo "format check failed: tab characters detected in C++ sources" >&2
  exit 1
fi

echo "format check passed"
