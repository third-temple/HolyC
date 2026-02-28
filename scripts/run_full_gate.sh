#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'EOF'
usage: run_full_gate.sh [options]

Runs required implementation gates:
1) macOS ARM64 lane (required): LLVM from third_party/llvm + LLVM-enabled build + tests
2) macOS ARM64 LLVM sanitizer lane (required): ASan+UBSan + stress/hardening
3) Linux x86_64 Docker lane (deferred): optional, non-blocking

Options:
  --macos-build-dir <path>   Host CMake build directory (default: build-<host-profile>)
  --build-type <type>        CMake build type (default: Release)
  --profile <name>           LLVM profile tag for host lane (default: autodetect host)
  --skip-llvm-build          Reuse prebuilt bundled LLVM without rebuilding
  --sanitizer-build-dir <p>  Sanitizer lane build directory (default: <macos-build-dir>-asan)
  --sanitizer-build-type <t> Sanitizer lane build type (default: RelWithDebInfo)
  --include-docker           Also run deferred docker lane
  --docker-build-dir <path>  Docker CMake build directory (default: build-linux-x86_64)
  --docker-image <name>      Docker image tag (default: holyc-linux-x86_64-builder)
  --skip-docker-image-build  Reuse existing docker image without building
  --rebuild-docker-image     Force rebuild docker image
  -h, --help                 Show this help message
EOF
}

RUN_DOCKER=0
HOST_BUILD_DIR=""
HOST_PROFILE=""
BUILD_TYPE="Release"
SKIP_LLVM_BUILD=0
SANITIZER_BUILD_DIR=""
SANITIZER_BUILD_TYPE="RelWithDebInfo"
DOCKER_BUILD_DIR="build-linux-x86_64"
DOCKER_IMAGE="holyc-linux-x86_64-builder"
SKIP_DOCKER_IMAGE_BUILD=0
REBUILD_DOCKER_IMAGE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --macos-build-dir)
      if [[ $# -lt 2 ]]; then
        echo "error: --macos-build-dir requires a value" >&2
        exit 2
      fi
      HOST_BUILD_DIR="$2"
      shift 2
      ;;
    --macos-build-dir=*)
      HOST_BUILD_DIR="${1#*=}"
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
      HOST_PROFILE="$2"
      shift 2
      ;;
    --profile=*)
      HOST_PROFILE="${1#*=}"
      shift
      ;;
    --skip-llvm-build)
      SKIP_LLVM_BUILD=1
      shift
      ;;
    --skip-sanitizer-lane)
      echo "error: --skip-sanitizer-lane is not allowed in the release gate" >&2
      exit 2
      ;;
    --sanitizer-build-dir)
      if [[ $# -lt 2 ]]; then
        echo "error: --sanitizer-build-dir requires a value" >&2
        exit 2
      fi
      SANITIZER_BUILD_DIR="$2"
      shift 2
      ;;
    --sanitizer-build-dir=*)
      SANITIZER_BUILD_DIR="${1#*=}"
      shift
      ;;
    --sanitizer-build-type)
      if [[ $# -lt 2 ]]; then
        echo "error: --sanitizer-build-type requires a value" >&2
        exit 2
      fi
      SANITIZER_BUILD_TYPE="$2"
      shift 2
      ;;
    --sanitizer-build-type=*)
      SANITIZER_BUILD_TYPE="${1#*=}"
      shift
      ;;
    --include-docker)
      RUN_DOCKER=1
      shift
      ;;
    --docker-build-dir)
      if [[ $# -lt 2 ]]; then
        echo "error: --docker-build-dir requires a value" >&2
        exit 2
      fi
      DOCKER_BUILD_DIR="$2"
      shift 2
      ;;
    --docker-build-dir=*)
      DOCKER_BUILD_DIR="${1#*=}"
      shift
      ;;
    --docker-image)
      if [[ $# -lt 2 ]]; then
        echo "error: --docker-image requires a value" >&2
        exit 2
      fi
      DOCKER_IMAGE="$2"
      shift 2
      ;;
    --docker-image=*)
      DOCKER_IMAGE="${1#*=}"
      shift
      ;;
    --skip-docker-image-build)
      SKIP_DOCKER_IMAGE_BUILD=1
      shift
      ;;
    --rebuild-docker-image)
      REBUILD_DOCKER_IMAGE=1
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

if [[ -z "${HOST_PROFILE}" ]]; then
  HOST_PROFILE="$("${ROOT_DIR}/scripts/build_bundled_llvm.sh" --print-profile)"
fi
if [[ -z "${HOST_BUILD_DIR}" ]]; then
  HOST_BUILD_DIR="build-${HOST_PROFILE}"
fi
if [[ -z "${SANITIZER_BUILD_DIR}" ]]; then
  SANITIZER_BUILD_DIR="${HOST_BUILD_DIR}-asan"
fi

if [[ "${HOST_BUILD_DIR}" = /* ]]; then
  HOST_BUILD_DIR_ABS="${HOST_BUILD_DIR}"
else
  HOST_BUILD_DIR_ABS="${ROOT_DIR}/${HOST_BUILD_DIR}"
fi

if [[ "${SKIP_LLVM_BUILD}" -eq 0 ]]; then
  "${ROOT_DIR}/scripts/build_bundled_llvm.sh" \
    --profile "${HOST_PROFILE}" \
    --build-type "${BUILD_TYPE}" \
    --jobs 2
fi

"${ROOT_DIR}/scripts/run_macos_arm64_lane.sh" \
  --with-bundled-llvm \
  --profile "${HOST_PROFILE}" \
  --build-dir "${HOST_BUILD_DIR}" \
  --build-type "${BUILD_TYPE}"

"${ROOT_DIR}/scripts/run_macos_arm64_llvm_sanitizer_lane.sh" \
  --profile "${HOST_PROFILE}" \
  --build-dir "${SANITIZER_BUILD_DIR}" \
  --build-type "${SANITIZER_BUILD_TYPE}"

# T42 criterion checks: enforce release invariants explicitly.
ctest --test-dir "${HOST_BUILD_DIR_ABS}" \
  --output-on-failure \
  --build-config "${BUILD_TYPE}" \
  -R "holyc\\.corpus\\.holyc-docs\\.full-repo|runtime\\.jit_backend\\.conformance|holyc\\.emit-llvm\\.invalid-comma-expr-lowering|holyc\\.emit-llvm\\.invalid-expression-space-lowering|holyc\\.emit-llvm\\.invalid-string-literal|holyc\\.jit\\.invalid-comma-expr-lowering|holyc\\.jit\\.expression-space-lowering"

if rg -n "not-yet" "${ROOT_DIR}/CMakeLists.txt" >/dev/null 2>&1; then
  echo "error: release gate forbids lingering 'not-yet' tests in CMakeLists.txt" >&2
  exit 1
fi

if ! awk '
  /std::int64_t[[:space:]]+hc_task_spawn[[:space:]]*\(/ { in_fn = 1 }
  in_fn && /std::abort[[:space:]]*\(/ { found_abort = 1 }
  in_fn && /^[[:space:]]*}/ {
    if (found_abort) {
      exit 1
    }
    exit 0
  }
  END {
    if (!in_fn) {
      exit 2
    }
  }
' "${ROOT_DIR}/runtime/hc_runtime.cpp"; then
  echo "error: hc_task_spawn must not be an abort-only runtime stub" >&2
  exit 1
fi

if [[ "${RUN_DOCKER}" -eq 1 ]]; then
  docker_args=(
    --image "${DOCKER_IMAGE}"
    --build-dir "${DOCKER_BUILD_DIR}"
  )
  if [[ "${SKIP_DOCKER_IMAGE_BUILD}" -eq 1 ]]; then
    docker_args+=(--skip-image-build)
  fi
  if [[ "${REBUILD_DOCKER_IMAGE}" -eq 1 ]]; then
    docker_args+=(--rebuild-image)
  fi
  "${ROOT_DIR}/scripts/run_docker_linux_x86_64.sh" "${docker_args[@]}"
fi

echo "full gate passed (required macOS ARM64 release criteria)"
