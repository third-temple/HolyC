#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <holyc-bin> <source-file>" >&2
  exit 2
fi

HOLYC_BIN="$1"
SRC_FILE="$2"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

OUT_DIR1="${TMP_DIR}/out-1"
OUT_DIR2="${TMP_DIR}/out-2"
ARTIFACTS1="${TMP_DIR}/artifacts-1"
ARTIFACTS2="${TMP_DIR}/artifacts-2"
mkdir -p "${OUT_DIR1}" "${OUT_DIR2}" "${ARTIFACTS1}" "${ARTIFACTS2}"
OUT1="${OUT_DIR1}/aout"
OUT2="${OUT_DIR2}/aout"

"${HOLYC_BIN}" build "${SRC_FILE}" -o "${OUT1}" --artifact-dir="${ARTIFACTS1}" --keep-temps >/dev/null
"${HOLYC_BIN}" build "${SRC_FILE}" -o "${OUT2}" --artifact-dir="${ARTIFACTS2}" --keep-temps >/dev/null

OBJ1="${ARTIFACTS1}/aout.o"
OBJ2="${ARTIFACTS2}/aout.o"
LL1="${ARTIFACTS1}/aout.ll"
LL2="${ARTIFACTS2}/aout.ll"

for path in "${OBJ1}" "${OBJ2}" "${LL1}" "${LL2}"; do
  if [[ ! -f "${path}" ]]; then
    echo "build reproducibility failure: missing artifact ${path}" >&2
    exit 1
  fi
done

OBJ_SHA1="$(shasum -a 256 "${OBJ1}" | awk '{print $1}')"
OBJ_SHA2="$(shasum -a 256 "${OBJ2}" | awk '{print $1}')"
LL_SHA1="$(shasum -a 256 "${LL1}" | awk '{print $1}')"
LL_SHA2="$(shasum -a 256 "${LL2}" | awk '{print $1}')"

if [[ "${OBJ_SHA1}" != "${OBJ_SHA2}" ]]; then
  echo "build reproducibility failure: object checksum mismatch" >&2
  echo "first : ${OBJ_SHA1}" >&2
  echo "second: ${OBJ_SHA2}" >&2
  exit 1
fi

if [[ "${LL_SHA1}" != "${LL_SHA2}" ]]; then
  echo "build reproducibility failure: LLVM IR checksum mismatch" >&2
  echo "first : ${LL_SHA1}" >&2
  echo "second: ${LL_SHA2}" >&2
  exit 1
fi
