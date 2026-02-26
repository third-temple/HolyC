#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: run_diff_test.sh <holyc-bin> <source-file>" >&2
  exit 2
fi

HOLYC_BIN="$1"
SRC_FILE="$2"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

AOT_BIN="${TMP_DIR}/aot_prog"
ARTIFACT_DIR="${TMP_DIR}/artifacts"
JIT_OUT_FILE="${TMP_DIR}/jit.out"
AOT_OUT_FILE="${TMP_DIR}/aot.out"

"${HOLYC_BIN}" jit "${SRC_FILE}" >"${JIT_OUT_FILE}"

"${HOLYC_BIN}" build "${SRC_FILE}" -o "${AOT_BIN}" --artifact-dir="${ARTIFACT_DIR}" >/dev/null
set +e
"${AOT_BIN}" >"${AOT_OUT_FILE}"
AOT_RC=$?
set -e

JIT_RAW="$(cat "${JIT_OUT_FILE}")"
JIT_LAST_LINE="$(printf '%s\n' "${JIT_RAW}" | tail -n 1)"
JIT_STDOUT="$(printf '%s\n' "${JIT_RAW}" | sed '$d')"

if [[ -z "${JIT_LAST_LINE}" ]]; then
  echo "differential test failure: jit produced empty result line" >&2
  exit 1
fi

if ! [[ "${JIT_LAST_LINE}" =~ ^-?[0-9]+$ ]]; then
  echo "differential test failure: jit final line is not integer: ${JIT_LAST_LINE}" >&2
  exit 1
fi

JIT_RC=$(( JIT_LAST_LINE & 255 ))
AOT_STDOUT="$(cat "${AOT_OUT_FILE}")"

if [[ "${JIT_STDOUT}" != "${AOT_STDOUT}" ]]; then
  echo "differential test failure: stdout mismatch" >&2
  echo "--- jit stdout ---" >&2
  printf '%s\n' "${JIT_STDOUT}" >&2
  echo "--- aot stdout ---" >&2
  printf '%s\n' "${AOT_STDOUT}" >&2
  exit 1
fi

if [[ ${JIT_RC} -ne ${AOT_RC} ]]; then
  echo "differential test failure: return code mismatch (jit=${JIT_RC}, aot=${AOT_RC})" >&2
  exit 1
fi
