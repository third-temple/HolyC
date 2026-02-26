#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

require_tool() {
  local tool="$1"
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "error: required tool '${tool}' is not installed or not in PATH" >&2
    exit 1
  fi
}

usage() {
  cat <<'EOF'
usage: configure_with_bundled_llvm.sh [options]

Options:
  --profile <name>         LLVM build profile tag (default: autodetect host)
  --build-dir <path>       CMake build directory (default: <repo>/build)
  --build-type <type>      CMake build type (default: Release)
  --llvm-cmake-dir <path>  Explicit LLVM cmake config directory
  --print-profile          Print detected host profile and exit
  -h, --help               Show this help message
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

PROFILE=""
BUILD_DIR="${ROOT_DIR}/build"
BUILD_TYPE="Release"
LLVM_CMAKE_DIR=""

while [[ $# -gt 0 ]]; do
  case "$1" in
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
    --llvm-cmake-dir)
      if [[ $# -lt 2 ]]; then
        echo "error: --llvm-cmake-dir requires a value" >&2
        exit 2
      fi
      LLVM_CMAKE_DIR="$2"
      shift 2
      ;;
    --llvm-cmake-dir=*)
      LLVM_CMAKE_DIR="${1#*=}"
      shift
      ;;
    --print-profile)
      detect_profile
      exit 0
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

if [[ -z "${PROFILE}" ]]; then
  PROFILE="$(detect_profile)"
fi

require_tool cmake

if [[ -z "${LLVM_CMAKE_DIR}" ]]; then
  LLVM_CMAKE_DIR="${ROOT_DIR}/third_party/llvm/build-${PROFILE}/lib/cmake/llvm"
  if [[ ! -f "${LLVM_CMAKE_DIR}/LLVMConfig.cmake" ]]; then
    LEGACY_LLVM_CMAKE_DIR="${ROOT_DIR}/third_party/llvm/build/lib/cmake/llvm"
    if [[ -f "${LEGACY_LLVM_CMAKE_DIR}/LLVMConfig.cmake" ]]; then
      LLVM_CMAKE_DIR="${LEGACY_LLVM_CMAKE_DIR}"
    fi
  fi
fi

if [[ ! -f "${LLVM_CMAKE_DIR}/LLVMConfig.cmake" ]]; then
  echo "error: missing ${LLVM_CMAKE_DIR}/LLVMConfig.cmake" >&2
  echo "run scripts/build_bundled_llvm.sh --profile ${PROFILE} first" >&2
  exit 1
fi

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DHOLYC_BUNDLED_LLVM_CONFIG_DIR="${LLVM_CMAKE_DIR}"

cmake --build "${BUILD_DIR}" --parallel

echo "Configured and built in ${BUILD_DIR}"
echo "Using LLVM config from ${LLVM_CMAKE_DIR}"
