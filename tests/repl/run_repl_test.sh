#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: run_repl_test.sh <holyc-bin> <case>" >&2
  exit 2
fi

HOLYC_BIN="$1"
CASE_NAME="$2"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT
OUT_FILE="${TMP_DIR}/repl.out"

case "${CASE_NAME}" in
  basic)
    cat <<'INPUT' | "${HOLYC_BIN}" repl >"${OUT_FILE}"
I64 Add(I64 x){ return x + 1; }
Add(41);
:quit
INPUT
    if ! grep -Eq '^42$' "${OUT_FILE}"; then
      echo "repl basic case failed: expected expression result 42" >&2
      cat "${OUT_FILE}" >&2
      exit 1
    fi
    ;;

  multiline)
    cat <<'INPUT' | "${HOLYC_BIN}" repl >"${OUT_FILE}"
:{
I64 Mul2(I64 x)
{
  return x * 2;
}
:}
Mul2(5);
:quit
INPUT
    if ! grep -Eq '^10$' "${OUT_FILE}"; then
      echo "repl multiline case failed: expected expression result 10" >&2
      cat "${OUT_FILE}" >&2
      exit 1
    fi
    ;;

  auto-multiline)
    cat <<'INPUT' | "${HOLYC_BIN}" repl >"${OUT_FILE}"
I64 Sum3(I64 a,
         I64 b,
         I64 c)
{
  return a + b + c;
}
Sum3(2, 3, 4);
:quit
INPUT
    if ! grep -Eq '^9$' "${OUT_FILE}"; then
      echo "repl auto-multiline case failed: expected expression result 9" >&2
      cat "${OUT_FILE}" >&2
      exit 1
    fi
    ;;

  global)
    cat <<'INPUT' | "${HOLYC_BIN}" repl >"${OUT_FILE}"
I64 g = 3;
g = g + 4;
g;
:quit
INPUT
    if [[ "$(grep -Ec '^7$' "${OUT_FILE}")" -lt 2 ]]; then
      echo "repl global case failed: expected two result lines with value 7" >&2
      cat "${OUT_FILE}" >&2
      exit 1
    fi
    ;;

  main-call)
    cat <<'INPUT' | "${HOLYC_BIN}" repl >"${OUT_FILE}"
I64 Main()
{
  "Hello\n";
  return 0;
}
Main();
:quit
INPUT
    if ! grep -Eq '^Hello$' "${OUT_FILE}" || ! grep -Eq '^0$' "${OUT_FILE}"; then
      echo "repl main-call case failed: expected Main() output and result 0" >&2
      cat "${OUT_FILE}" >&2
      exit 1
    fi
    ;;

  *)
    echo "unknown repl test case: ${CASE_NAME}" >&2
    exit 2
    ;;
esac
