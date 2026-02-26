#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: run_golden_hir.sh <holyc-bin> <source> <golden>" >&2
  exit 2
fi

HOLYC_BIN="$1"
SRC="$2"
GOLDEN="$3"

TMP_OUT="$(mktemp)"
trap 'rm -f "${TMP_OUT}"' EXIT

"${HOLYC_BIN}" emit-hir "${SRC}" >"${TMP_OUT}"
if ! diff -u "${GOLDEN}" "${TMP_OUT}"; then
  echo "HIR golden mismatch for ${SRC}" >&2
  exit 1
fi
