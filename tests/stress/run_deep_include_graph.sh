#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: run_deep_include_graph.sh <holyc-bin>" >&2
  exit 2
fi

HOLYC_BIN="$1"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

DEPTH=60
for i in $(seq 1 "${DEPTH}"); do
  next=$((i + 1))
  file="${TMP_DIR}/inc_${i}.HC"
  if [[ "${i}" -eq "${DEPTH}" ]]; then
    cat >"${file}" <<FINAL
#define HOLYC_DEEP_VALUE ${DEPTH}
FINAL
  else
    cat >"${file}" <<INC
#include "inc_${next}.HC"
INC
  fi
done

cat >"${TMP_DIR}/main.HC" <<MAIN
#include "inc_1.HC"
I64 Main()
{
  return HOLYC_DEEP_VALUE;
}
MAIN

"${HOLYC_BIN}" check "${TMP_DIR}/main.HC" >/dev/null
