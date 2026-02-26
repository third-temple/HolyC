#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'EOF'
usage: run_macos_arm64_lane.sh [options]

Runs the canonical required local lane on macOS ARM64:
configure -> build -> test

Options:
  --build-dir <path>      CMake build directory (default: build-macos-arm64)
  --build-type <type>     CMake build type (default: Release)
  --profile <name>        LLVM profile tag when using bundled LLVM (default: autodetect host)
  --with-bundled-llvm     Configure via scripts/configure_with_bundled_llvm.sh
  --clean                 Remove build directory before configuring
  --ctest-args "<args>"   Extra arguments appended to ctest
  --skip-tests            Stop after configure + build
  -h, --help              Show this help message
EOF
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

require_tool() {
  local tool="$1"
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "error: required tool '${tool}' is not installed or not in PATH" >&2
    exit 1
  fi
}

assert_macos_arm64() {
  local os arch
  os="$(uname -s)"
  arch="$(uname -m)"
  if [[ "${os}" != "Darwin" || "${arch}" != "arm64" ]]; then
    echo "error: this lane is required for macOS ARM64 only (detected: ${os}/${arch})" >&2
    echo "hint: run this script on a macOS arm64 host or use a platform-specific lane script" >&2
    exit 2
  fi
}

BUILD_DIR="build-macos-arm64"
BUILD_TYPE="Release"
PROFILE=""
USE_BUNDLED_LLVM=0
CLEAN=0
SKIP_TESTS=0
CTEST_ARGS=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      if [[ $# -lt 2 ]]; then
        echo "error: --build-dir requires a value" >&2
        exit 2
      fi
      BUILD_DIR="$2"
      shift 2
      ;;
    --build-dir=*)
      BUILD_DIR="${1#*=}"
      shift
      ;;
    --build-type)
      if [[ $# -lt 2 ]]; then
        echo "error: --build-type requires a value" >&2
        exit 2
      fi
      BUILD_TYPE="$2"
      shift 2
      ;;
    --build-type=*)
      BUILD_TYPE="${1#*=}"
      shift
      ;;
    --profile)
      if [[ $# -lt 2 ]]; then
        echo "error: --profile requires a value" >&2
        exit 2
      fi
      PROFILE="$2"
      shift 2
      ;;
    --profile=*)
      PROFILE="${1#*=}"
      shift
      ;;
    --with-bundled-llvm)
      USE_BUNDLED_LLVM=1
      shift
      ;;
    --clean)
      CLEAN=1
      shift
      ;;
    --ctest-args)
      if [[ $# -lt 2 ]]; then
        echo "error: --ctest-args requires a value" >&2
        exit 2
      fi
      CTEST_ARGS="$2"
      shift 2
      ;;
    --ctest-args=*)
      CTEST_ARGS="${1#*=}"
      shift
      ;;
    --skip-tests)
      SKIP_TESTS=1
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
require_tool cmake
require_tool ctest

if [[ "${BUILD_DIR}" = /* ]]; then
  BUILD_DIR_ABS="${BUILD_DIR}"
else
  BUILD_DIR_ABS="${ROOT_DIR}/${BUILD_DIR}"
fi

if [[ "${CLEAN}" -eq 1 ]]; then
  rm -rf "${BUILD_DIR_ABS}"
fi

if [[ "${USE_BUNDLED_LLVM}" -eq 1 ]]; then
  if [[ -z "${PROFILE}" ]]; then
    PROFILE="$(detect_profile)"
  fi
  "${ROOT_DIR}/scripts/configure_with_bundled_llvm.sh" \
    --profile "${PROFILE}" \
    --build-dir "${BUILD_DIR_ABS}" \
    --build-type "${BUILD_TYPE}"
else
  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR_ABS}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DHOLYC_ENABLE_LLVM=ON
  cmake --build "${BUILD_DIR_ABS}" --parallel
fi

if [[ "${SKIP_TESTS}" -eq 1 ]]; then
  echo "macOS ARM64 lane configure/build complete (tests skipped)"
  exit 0
fi

TEST_JOBS="$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"
if [[ -n "${CTEST_ARGS}" ]]; then
  # shellcheck disable=SC2086
  ctest --test-dir "${BUILD_DIR_ABS}" \
    --output-on-failure \
    --build-config "${BUILD_TYPE}" \
    --parallel "${TEST_JOBS}" \
    ${CTEST_ARGS}
else
  ctest --test-dir "${BUILD_DIR_ABS}" \
    --output-on-failure \
    --build-config "${BUILD_TYPE}" \
    --parallel "${TEST_JOBS}"
fi

echo "macOS ARM64 lane passed"
