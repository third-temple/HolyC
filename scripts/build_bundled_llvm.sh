#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LLVM_SRC_DIR="${ROOT_DIR}/third_party/llvm/llvm"

require_tool() {
  local tool="$1"
  if ! command -v "${tool}" >/dev/null 2>&1; then
    echo "error: required tool '${tool}' is not installed or not in PATH" >&2
    exit 1
  fi
}

usage() {
  cat <<'EOF'
usage: build_bundled_llvm.sh [options]

Options:
  --profile <name>       Build profile tag (default: autodetect host, e.g. macos-arm64)
  --build-type <type>    CMake build type (default: Release)
  --print-profile        Print the detected host profile and exit
  -h, --help             Show this help message
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
BUILD_TYPE="Release"

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

LLVM_BUILD_DIR="${ROOT_DIR}/third_party/llvm/build-${PROFILE}"
INSTALL_DIR="${ROOT_DIR}/third_party/llvm/install-${PROFILE}"

require_tool cmake
require_tool ninja

if [[ ! -d "${LLVM_SRC_DIR}" ]]; then
  echo "error: expected LLVM sources at ${LLVM_SRC_DIR}" >&2
  echo "hint: run 'git submodule update --init --recursive third_party/llvm'" >&2
  exit 1
fi

if [[ -f "${LLVM_BUILD_DIR}/CMakeCache.txt" ]]; then
  current_generator="$(
    awk -F= '/^CMAKE_GENERATOR:INTERNAL=/{print $2; exit}' \
      "${LLVM_BUILD_DIR}/CMakeCache.txt" || true
  )"
  if [[ -n "${current_generator}" && "${current_generator}" != "Ninja" ]]; then
    echo "info: resetting ${LLVM_BUILD_DIR} cache (generator '${current_generator}' -> 'Ninja')"
    rm -rf "${LLVM_BUILD_DIR}/CMakeCache.txt" "${LLVM_BUILD_DIR}/CMakeFiles"
  fi
fi

cmake -S "${LLVM_SRC_DIR}" -B "${LLVM_BUILD_DIR}" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
  -DLLVM_ENABLE_PROJECTS="" \
  -DLLVM_TARGETS_TO_BUILD="AArch64;X86" \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_BUILD_TOOLS=OFF \
  -DLLVM_ENABLE_BINDINGS=OFF \
  -DLLVM_ENABLE_ZSTD=OFF \
  -DBUILD_SHARED_LIBS=OFF \
  -DLLVM_BUILD_LLVM_DYLIB=OFF \
  -DLLVM_LINK_LLVM_DYLIB=OFF

cmake --build "${LLVM_BUILD_DIR}" --parallel
cmake --install "${LLVM_BUILD_DIR}"

echo "LLVM build complete."
echo "Profile: ${PROFILE}"
echo "Use HOLYC_BUNDLED_LLVM_CONFIG_DIR=${LLVM_BUILD_DIR}/lib/cmake/llvm"
