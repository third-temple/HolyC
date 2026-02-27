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

if command -v rg >/dev/null 2>&1; then
  if rg --hidden --glob '!third_party/**' --glob '!.git/**' --glob '!build*/**' -n '[[:blank:]]+$' "${EXISTING_TARGETS[@]}"; then
    echo "format check failed: trailing whitespace detected" >&2
    exit 1
  fi
else
  if grep -R -n -E \
    --exclude-dir=third_party \
    --exclude-dir=.git \
    --exclude-dir='build*' \
    '[[:blank:]]+$' "${EXISTING_TARGETS[@]}"; then
    echo "format check failed: trailing whitespace detected" >&2
    exit 1
  fi
fi

SOURCE_TARGETS=()
for target in frontend lowering runtime src; do
  if [[ -e "${target}" ]]; then
    SOURCE_TARGETS+=("${target}")
  fi
done

if [[ ${#SOURCE_TARGETS[@]} -gt 0 ]]; then
  if command -v rg >/dev/null 2>&1; then
    if rg --hidden --glob '!third_party/**' --glob '!.git/**' --glob '!build*/**' -n $'\t' "${SOURCE_TARGETS[@]}"; then
      echo "format check failed: tab characters detected in C++ sources" >&2
      exit 1
    fi
  else
    if grep -R -n \
      --exclude-dir=third_party \
      --exclude-dir=.git \
      --exclude-dir='build*' \
      $'\t' "${SOURCE_TARGETS[@]}"; then
      echo "format check failed: tab characters detected in C++ sources" >&2
      exit 1
    fi
  fi
fi

echo "format check passed"
