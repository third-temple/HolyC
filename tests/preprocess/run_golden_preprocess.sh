#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 3 || $# -gt 4 ]]; then
  echo "usage: run_golden_preprocess.sh <holyc-bin> <source-file> <golden-file> [mode]" >&2
  exit 2
fi

HOLYC_BIN="$1"
SRC_FILE="$2"
GOLDEN_FILE="$3"
MODE="${4:-jit}"
TMP_OUT="$(mktemp)"
trap 'rm -f "${TMP_OUT}"' EXIT

"${HOLYC_BIN}" preprocess "${SRC_FILE}" "--mode=${MODE}" >"${TMP_OUT}"

if ! diff -u "${GOLDEN_FILE}" "${TMP_OUT}"; then
  echo "golden preprocess mismatch for ${SRC_FILE} (mode=${MODE})" >&2
  exit 1
fi
