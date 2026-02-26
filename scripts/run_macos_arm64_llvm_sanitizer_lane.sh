#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'USAGE'
usage: run_macos_arm64_llvm_sanitizer_lane.sh [options]

Runs the required sanitizer+stress+hardening lane on macOS ARM64 with bundled LLVM.

Options:
  --build-dir <path>    CMake build directory (default: build-macos-arm64-llvm-asan)
  --build-type <type>   CMake build type (default: RelWithDebInfo)
  --profile <name>      LLVM profile tag (default: autodetect host)
  -h, --help            Show this help message
USAGE
}

detect_profile() {
  local os arch
  os="$(uname -s | tr '[:upper:]' '[:lower:]')"
  arch="$(uname -m)"
  case "${arch}" in
    x86_64|amd64) arch="x86_64" ;;
    arm64|aarch64) arch="arm64" ;;
  esac
  echo "${os}-${arch}"
}

assert_macos_arm64() {
  local os arch
  os="$(uname -s)"
  arch="$(uname -m)"
  if [[ "${os}" != "Darwin" || "${arch}" != "arm64" ]]; then
    echo "error: this lane requires macOS arm64 (detected ${os}/${arch})" >&2
    exit 2
  fi
}

BUILD_DIR="build-macos-arm64-llvm-asan"
BUILD_TYPE="RelWithDebInfo"
PROFILE=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --build-dir=*)
      BUILD_DIR="${1#*=}"
      shift
      ;;
    --build-type)
      BUILD_TYPE="$2"
      shift 2
      ;;
    --build-type=*)
      BUILD_TYPE="${1#*=}"
      shift
      ;;
    --profile)
      PROFILE="$2"
      shift 2
      ;;
    --profile=*)
      PROFILE="${1#*=}"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

assert_macos_arm64
if [[ -z "${PROFILE}" ]]; then
  PROFILE="$(detect_profile)"
fi

if [[ "${BUILD_DIR}" = /* ]]; then
  BUILD_DIR_ABS="${BUILD_DIR}"
else
  BUILD_DIR_ABS="${ROOT_DIR}/${BUILD_DIR}"
fi

CXXFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-rtti" \
CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
LDFLAGS="-fsanitize=address,undefined" \
  "${ROOT_DIR}/scripts/configure_with_bundled_llvm.sh" \
    --profile "${PROFILE}" \
    --build-dir "${BUILD_DIR_ABS}" \
    --build-type "${BUILD_TYPE}"

ctest --test-dir "${BUILD_DIR_ABS}" \
  --output-on-failure \
  --build-config "${BUILD_TYPE}" \
  -R "holyc\\.stress\\.deep-include-graph|holyc\\.hardening\\.malformed-inputs"

echo "macOS ARM64 LLVM sanitizer lane passed"
