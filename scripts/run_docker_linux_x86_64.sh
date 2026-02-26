#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<'EOF'
usage: run_docker_linux_x86_64.sh [options]

Runs HolyC build/test in a linux/amd64 Docker container.

Options:
  --image <name>         Docker image tag (default: holyc-linux-x86_64-builder)
  --dockerfile <path>    Dockerfile path (default: <repo>/Dockerfile.linux-x86_64)
  --platform <value>     Docker platform (default: linux/amd64)
  --profile <name>       LLVM profile used inside container (default: linux-x86_64)
  --build-dir <path>     CMake build dir used inside container (default: build-linux-x86_64)
  --cmd <command>        Command to run inside container
  --skip-image-build     Do not build image before running
  --rebuild-image        Force rebuild image before running
  -h, --help             Show this help message

Default container command:
  scripts/build_bundled_llvm.sh --profile linux-x86_64 &&
  scripts/configure_with_bundled_llvm.sh --profile linux-x86_64 --build-dir build-linux-x86_64 &&
  ctest --test-dir build-linux-x86_64 --output-on-failure --build-config Release
EOF
}

if ! command -v docker >/dev/null 2>&1; then
  echo "error: docker is required but was not found in PATH" >&2
  exit 2
fi

IMAGE_NAME="holyc-linux-x86_64-builder"
DOCKERFILE="${ROOT_DIR}/Dockerfile.linux-x86_64"
PLATFORM="linux/amd64"
PROFILE="linux-x86_64"
BUILD_DIR="build-linux-x86_64"
CONTAINER_CMD=""
SKIP_IMAGE_BUILD=0
REBUILD_IMAGE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --image)
      if [[ $# -lt 2 ]]; then
        echo "error: --image requires a value" >&2
        exit 2
      fi
      IMAGE_NAME="$2"
      shift 2
      ;;
    --image=*)
      IMAGE_NAME="${1#*=}"
      shift
      ;;
    --dockerfile)
      if [[ $# -lt 2 ]]; then
        echo "error: --dockerfile requires a value" >&2
        exit 2
      fi
      DOCKERFILE="$2"
      shift 2
      ;;
    --dockerfile=*)
      DOCKERFILE="${1#*=}"
      shift
      ;;
    --platform)
      if [[ $# -lt 2 ]]; then
        echo "error: --platform requires a value" >&2
        exit 2
      fi
      PLATFORM="$2"
      shift 2
      ;;
    --platform=*)
      PLATFORM="${1#*=}"
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
    --cmd)
      if [[ $# -lt 2 ]]; then
        echo "error: --cmd requires a value" >&2
        exit 2
      fi
      CONTAINER_CMD="$2"
      shift 2
      ;;
    --cmd=*)
      CONTAINER_CMD="${1#*=}"
      shift
      ;;
    --skip-image-build)
      SKIP_IMAGE_BUILD=1
      shift
      ;;
    --rebuild-image)
      REBUILD_IMAGE=1
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

if [[ -z "${CONTAINER_CMD}" ]]; then
  CONTAINER_CMD="scripts/build_bundled_llvm.sh --profile ${PROFILE} && scripts/configure_with_bundled_llvm.sh --profile ${PROFILE} --build-dir ${BUILD_DIR} && ctest --test-dir ${BUILD_DIR} --output-on-failure --build-config Release"
fi

if [[ "${SKIP_IMAGE_BUILD}" -eq 0 ]]; then
  if [[ "${REBUILD_IMAGE}" -eq 1 ]] || ! docker image inspect "${IMAGE_NAME}" >/dev/null 2>&1; then
    docker build \
      --platform "${PLATFORM}" \
      -f "${DOCKERFILE}" \
      -t "${IMAGE_NAME}" \
      "${ROOT_DIR}"
  fi
fi

docker run --rm \
  --platform "${PLATFORM}" \
  --user "$(id -u):$(id -g)" \
  -e HOME=/tmp \
  -v "${ROOT_DIR}:/work" \
  -w /work \
  "${IMAGE_NAME}" \
  /bin/bash -lc "${CONTAINER_CMD}"
