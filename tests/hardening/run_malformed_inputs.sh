#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: run_malformed_inputs.sh <holyc-bin>" >&2
  exit 2
fi

HOLYC_BIN="$1"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

run_expect_failure() {
  local src="$1"
  local label="$2"
  local out_file="${TMP_DIR}/${label}.out"
  local err_file="${TMP_DIR}/${label}.err"

  if "${HOLYC_BIN}" check "${src}" >"${out_file}" 2>"${err_file}"; then
    echo "expected failure for ${label}, but command succeeded" >&2
    return 1
  fi

  if [[ ! -s "${err_file}" ]]; then
    echo "expected diagnostics for ${label}, but stderr was empty" >&2
    return 1
  fi

  return 0
}

BINARY_SRC="${TMP_DIR}/binary_noise.HC"
dd if=/dev/urandom of="${BINARY_SRC}" bs=128 count=1 >/dev/null 2>&1
run_expect_failure "${BINARY_SRC}" "binary_noise"

UNTERMINATED_STRING_SRC="${TMP_DIR}/unterminated_string.HC"
cat >"${UNTERMINATED_STRING_SRC}" <<'SRC1'
I64 Main()
{
  "unterminated;
  return 0;
}
SRC1
run_expect_failure "${UNTERMINATED_STRING_SRC}" "unterminated_string"

HUGE_TOKEN_SRC="${TMP_DIR}/huge_token.HC"
cat >"${HUGE_TOKEN_SRC}" <<'SRC2'
I64 Main()
{
  return THIS_TOKEN_IS_INVALID_AND_SHOULD_TRIGGER_A_DIAGNOSTIC_WITHOUT_A_CRASH;
}
SRC2
run_expect_failure "${HUGE_TOKEN_SRC}" "huge_token"
