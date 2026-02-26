#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: run_emit_llvm_repro.sh <holyc-bin> <source>" >&2
  exit 2
fi

HOLYC_BIN="$1"
SRC="$2"

TMP_A="$(mktemp)"
TMP_B="$(mktemp)"
trap 'rm -f "${TMP_A}" "${TMP_B}"' EXIT

"${HOLYC_BIN}" emit-llvm "${SRC}" >"${TMP_A}"
"${HOLYC_BIN}" emit-llvm "${SRC}" >"${TMP_B}"

if ! diff -u "${TMP_A}" "${TMP_B}"; then
  echo "emit-llvm output is not deterministic for ${SRC}" >&2
  exit 1
fi
