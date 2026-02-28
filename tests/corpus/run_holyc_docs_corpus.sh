#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <holyc-bin>" >&2
  exit 2
fi

HOLYC_BIN="$1"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CORPUS_DIR="${SCRIPT_DIR}/holyc_docs"

corpus_files=()
while IFS= read -r src; do
  corpus_files+=("${src}")
done < <(find "${CORPUS_DIR}" -type f -name '*.HC' | LC_ALL=C sort)

if [[ ${#corpus_files[@]} -eq 0 ]]; then
  echo "error: no HolyC corpus files found under ${CORPUS_DIR}" >&2
  exit 1
fi

for src in "${corpus_files[@]}"; do
  "${HOLYC_BIN}" check "${src}" >/dev/null
done

echo "checked ${#corpus_files[@]} corpus files"
