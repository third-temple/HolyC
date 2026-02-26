#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <holyc-bin>" >&2
  exit 2
fi

HOLYC_BIN="$1"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

"${HOLYC_BIN}" check "${SCRIPT_DIR}/holyc_docs/NullCase.HC" >/dev/null
"${HOLYC_BIN}" check "${SCRIPT_DIR}/holyc_docs/SubSwitch.HC" >/dev/null
"${HOLYC_BIN}" check "${SCRIPT_DIR}/holyc_docs/ClassMeta.HC" >/dev/null
"${HOLYC_BIN}" check "${SCRIPT_DIR}/holyc_docs/Directives.HC" >/dev/null
"${HOLYC_BIN}" check "${SCRIPT_DIR}/holyc_docs/Exceptions.HC" >/dev/null
"${HOLYC_BIN}" check "${SCRIPT_DIR}/holyc_docs/LastClass.HC" >/dev/null
"${HOLYC_BIN}" check "${SCRIPT_DIR}/holyc_docs/Lock.HC" >/dev/null
"${HOLYC_BIN}" check "${SCRIPT_DIR}/holyc_docs/StkGrow.HC" >/dev/null
"${HOLYC_BIN}" check "${SCRIPT_DIR}/holyc_docs/SubIntAccess.HC" >/dev/null
