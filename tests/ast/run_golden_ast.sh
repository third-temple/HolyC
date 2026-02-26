#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: run_golden_ast.sh <holyc-bin> <source> <golden>" >&2
  exit 2
fi

HOLYC_BIN="$1"
SRC="$2"
GOLDEN="$3"

TMP_OUT="$(mktemp)"
TMP_NORM_OUT="$(mktemp)"
TMP_NORM_GOLDEN="$(mktemp)"
trap 'rm -f "${TMP_OUT}" "${TMP_NORM_OUT}" "${TMP_NORM_GOLDEN}"' EXIT

"${HOLYC_BIN}" ast-dump "${SRC}" >"${TMP_OUT}"
sed -E 's#^Program: .*#Program: <source>#' "${TMP_OUT}" >"${TMP_NORM_OUT}"
sed -E 's#^Program: .*#Program: <source>#' "${GOLDEN}" >"${TMP_NORM_GOLDEN}"

if ! diff -u "${TMP_NORM_GOLDEN}" "${TMP_NORM_OUT}"; then
  echo "AST golden mismatch for ${SRC}" >&2
  exit 1
fi
