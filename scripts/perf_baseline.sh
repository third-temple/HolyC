#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
usage: perf_baseline.sh <holyc-bin> [out-file] [options]

Positional arguments:
  <holyc-bin>               Path to holyc binary to benchmark.
  [out-file]                Optional markdown output path (legacy alias for --out-md).

Options:
  --out-md <path>           Markdown report path (default: .holyc-artifacts/perf-baseline.md)
  --out-json <path>         JSON report path (default: .holyc-artifacts/perf-baseline.json)
  --runs <count>            Timed runs per benchmark (default: 7)
  --warmup <count>          Warmup runs per benchmark (default: 2)
  --suite <name>            Benchmark suite: all|compile|runtime (default: all)
  --opt-level <level>       Optimization level for jit/build/run: 0|1|2|3|s|z (default: 2)
  -h, --help                Show this help.
EOF
}

HOLYC_BIN=""
OUT_MD=".holyc-artifacts/perf-baseline.md"
OUT_JSON=".holyc-artifacts/perf-baseline.json"
RUNS="7"
WARMUP="2"
SUITE="all"
OPT_LEVEL="2"
POSITIONAL_OUT_SEEN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --out-md)
      if [[ $# -lt 2 ]]; then
        echo "error: --out-md requires a value" >&2
        exit 2
      fi
      OUT_MD="$2"
      shift 2
      ;;
    --out-md=*)
      OUT_MD="${1#*=}"
      shift
      ;;
    --out-json)
      if [[ $# -lt 2 ]]; then
        echo "error: --out-json requires a value" >&2
        exit 2
      fi
      OUT_JSON="$2"
      shift 2
      ;;
    --out-json=*)
      OUT_JSON="${1#*=}"
      shift
      ;;
    --runs)
      if [[ $# -lt 2 ]]; then
        echo "error: --runs requires a value" >&2
        exit 2
      fi
      RUNS="$2"
      shift 2
      ;;
    --runs=*)
      RUNS="${1#*=}"
      shift
      ;;
    --warmup)
      if [[ $# -lt 2 ]]; then
        echo "error: --warmup requires a value" >&2
        exit 2
      fi
      WARMUP="$2"
      shift 2
      ;;
    --warmup=*)
      WARMUP="${1#*=}"
      shift
      ;;
    --suite)
      if [[ $# -lt 2 ]]; then
        echo "error: --suite requires a value" >&2
        exit 2
      fi
      SUITE="$2"
      shift 2
      ;;
    --suite=*)
      SUITE="${1#*=}"
      shift
      ;;
    --opt-level)
      if [[ $# -lt 2 ]]; then
        echo "error: --opt-level requires a value" >&2
        exit 2
      fi
      OPT_LEVEL="$2"
      shift 2
      ;;
    --opt-level=*)
      OPT_LEVEL="${1#*=}"
      shift
      ;;
    --)
      shift
      break
      ;;
    -*)
      echo "error: unknown option: $1" >&2
      usage
      exit 2
      ;;
    *)
      if [[ -z "${HOLYC_BIN}" ]]; then
        HOLYC_BIN="$1"
      elif [[ ${POSITIONAL_OUT_SEEN} -eq 0 ]]; then
        OUT_MD="$1"
        POSITIONAL_OUT_SEEN=1
      else
        echo "error: unexpected argument: $1" >&2
        usage
        exit 2
      fi
      shift
      ;;
  esac
done

if [[ $# -gt 0 ]]; then
  echo "error: unexpected trailing arguments: $*" >&2
  usage
  exit 2
fi

if [[ -z "${HOLYC_BIN}" ]]; then
  usage
  exit 2
fi

if ! [[ "${RUNS}" =~ ^[1-9][0-9]*$ ]]; then
  echo "error: --runs must be a positive integer" >&2
  exit 2
fi
if ! [[ "${WARMUP}" =~ ^[0-9]+$ ]]; then
  echo "error: --warmup must be a non-negative integer" >&2
  exit 2
fi
if ! [[ "${SUITE}" =~ ^(all|compile|runtime)$ ]]; then
  echo "error: --suite must be one of: all, compile, runtime" >&2
  exit 2
fi
if ! [[ "${OPT_LEVEL}" =~ ^(0|1|2|3|s|z)$ ]]; then
  echo "error: --opt-level must be one of: 0, 1, 2, 3, s, z" >&2
  exit 2
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "error: python3 is required for perf statistics output" >&2
  exit 2
fi

mkdir -p "$(dirname "${OUT_MD}")"
mkdir -p "$(dirname "${OUT_JSON}")"

python3 - "${HOLYC_BIN}" "${OUT_MD}" "${OUT_JSON}" "${RUNS}" "${WARMUP}" "${SUITE}" "${OPT_LEVEL}" <<'PY'
import datetime
import json
import math
import platform
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

holyc_bin = sys.argv[1]
out_md = Path(sys.argv[2])
out_json = Path(sys.argv[3])
runs = int(sys.argv[4])
warmup = int(sys.argv[5])
suite = sys.argv[6]
opt_level = sys.argv[7]


def build_benchmarks(tmp_root: Path, selected_suite: str, selected_opt_level: str) -> list[dict]:
    opt_flag = f"--opt-level={selected_opt_level}"
    all_benchmarks = [
        {
            "suite": "compile",
            "operation": "check",
            "name": "check.features",
            "sample": "tests/samples/features.HC",
            "command": [holyc_bin, "check", "tests/samples/features.HC"],
        },
        {
            "suite": "compile",
            "operation": "check",
            "name": "check.semantics",
            "sample": "tests/samples/semantics.HC",
            "command": [holyc_bin, "check", "tests/samples/semantics.HC"],
        },
        {
            "suite": "compile",
            "operation": "emit-llvm",
            "name": "emit-llvm.llvm",
            "sample": "tests/samples/llvm.HC",
            "command": [holyc_bin, "emit-llvm", "tests/samples/llvm.HC"],
        },
        {
            "suite": "compile",
            "operation": "emit-llvm",
            "name": "emit-llvm.control-flow",
            "sample": "tests/samples/control_flow.HC",
            "command": [holyc_bin, "emit-llvm", "tests/samples/control_flow.HC"],
        },
        {
            "suite": "compile",
            "operation": "build",
            "name": "build.llvm",
            "sample": "tests/samples/llvm.HC",
            "command": [
                holyc_bin,
                "build",
                "tests/samples/llvm.HC",
                "-o",
                str(tmp_root / "build-llvm"),
                f"--artifact-dir={tmp_root / 'artifacts-build-llvm'}",
                opt_flag,
            ],
        },
        {
            "suite": "compile",
            "operation": "build",
            "name": "build.runtime-print",
            "sample": "tests/samples/runtime_print.HC",
            "command": [
                holyc_bin,
                "build",
                "tests/samples/runtime_print.HC",
                "-o",
                str(tmp_root / "build-runtime-print"),
                f"--artifact-dir={tmp_root / 'artifacts-build-runtime-print'}",
                opt_flag,
            ],
        },
        {
            "suite": "runtime",
            "operation": "jit",
            "name": "jit.jit",
            "sample": "tests/samples/jit.HC",
            "command": [holyc_bin, "jit", "tests/samples/jit.HC", opt_flag],
        },
        {
            "suite": "runtime",
            "operation": "jit",
            "name": "jit.control-flow",
            "sample": "tests/samples/control_flow.HC",
            "command": [holyc_bin, "jit", "tests/samples/control_flow.HC", opt_flag],
        },
        {
            "suite": "runtime",
            "operation": "jit",
            "name": "jit.runtime-print",
            "sample": "tests/samples/runtime_print.HC",
            "command": [holyc_bin, "jit", "tests/samples/runtime_print.HC", opt_flag],
        },
        {
            "suite": "runtime",
            "operation": "jit",
            "name": "jit.switch-edge-cases",
            "sample": "tests/samples/switch_edge_cases.HC",
            "command": [holyc_bin, "jit", "tests/samples/switch_edge_cases.HC", opt_flag],
        },
        {
            "suite": "runtime",
            "operation": "run",
            "name": "run.main-with-params",
            "sample": "tests/samples/main_with_params.HC",
            "command": [
                holyc_bin,
                "run",
                "tests/samples/main_with_params.HC",
                f"--artifact-dir={tmp_root / 'artifacts-run-main-with-params'}",
                opt_flag,
            ],
        },
        {
            "suite": "runtime",
            "operation": "run",
            "name": "run.main-with-extra-params",
            "sample": "tests/samples/main_with_extra_params.HC",
            "command": [
                holyc_bin,
                "run",
                "tests/samples/main_with_extra_params.HC",
                f"--artifact-dir={tmp_root / 'artifacts-run-main-with-extra-params'}",
                opt_flag,
            ],
        },
    ]

    if selected_suite == "all":
        return all_benchmarks
    return [entry for entry in all_benchmarks if entry["suite"] == selected_suite]


def p95(values: list[float]) -> float:
    sorted_values = sorted(values)
    idx = max(0, math.ceil(0.95 * len(sorted_values)) - 1)
    return sorted_values[idx]


def summarize(times: list[float]) -> dict:
    return {
        "runs": len(times),
        "min_sec": min(times),
        "max_sec": max(times),
        "mean_sec": statistics.fmean(times),
        "median_sec": statistics.median(times),
        "p95_sec": p95(times),
        "stdev_sec": statistics.pstdev(times) if len(times) > 1 else 0.0,
    }


def run_once(cmd: list[str]) -> float:
    start_ns = time.perf_counter_ns()
    proc = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    elapsed = (time.perf_counter_ns() - start_ns) / 1_000_000_000.0
    if proc.returncode != 0:
        raise RuntimeError(
            f"benchmark command failed with exit code {proc.returncode}: {' '.join(cmd)}"
        )
    return elapsed


results = []
with tempfile.TemporaryDirectory(prefix="holyc-perf-") as temp_dir:
    benchmarks = build_benchmarks(Path(temp_dir), suite, opt_level)
    for benchmark in benchmarks:
        for _ in range(warmup):
            run_once(benchmark["command"])
        times = [run_once(benchmark["command"]) for _ in range(runs)]
        results.append(
            {
                "suite": benchmark["suite"],
                "operation": benchmark["operation"],
                "name": benchmark["name"],
                "sample": benchmark["sample"],
                "command": benchmark["command"],
                "summary": summarize(times),
                "times_sec": times,
            }
        )

generated_at = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

json_payload = {
    "tool": "scripts/perf_baseline.sh",
    "generated_at_utc": generated_at,
    "host": {
        "platform": platform.platform(),
        "machine": platform.machine(),
        "python": platform.python_version(),
    },
    "config": {
        "holyc_bin": holyc_bin,
        "runs": runs,
        "warmup": warmup,
        "suite": suite,
        "opt_level": opt_level,
    },
    "benchmarks": results,
}

out_json.write_text(json.dumps(json_payload, indent=2) + "\n", encoding="utf-8")

md_lines = [
    "# Performance Baseline",
    "",
    f"Generated by scripts/perf_baseline.sh on {generated_at}.",
    "",
    "All timings are wall-clock seconds measured by Python monotonic high-resolution clock.",
    f"Runs per benchmark: {runs}. Warmup runs: {warmup}.",
    f"Suite: {suite}. Opt level for jit/build/run: {opt_level}.",
    "",
    "| Suite | Benchmark | Operation | Sample | Median (s) | P95 (s) | Mean (s) | Stdev (s) | Min (s) | Max (s) |",
    "| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
]

for entry in results:
    s = entry["summary"]
    md_lines.append(
        "| {suite_name} | {name} | {op} | {sample} | {median:.6f} | {p95:.6f} | {mean:.6f} | {stdev:.6f} | {minv:.6f} | {maxv:.6f} |".format(
            suite_name=entry["suite"],
            name=entry["name"],
            op=entry["operation"],
            sample=entry["sample"],
            median=s["median_sec"],
            p95=s["p95_sec"],
            mean=s["mean_sec"],
            stdev=s["stdev_sec"],
            minv=s["min_sec"],
            maxv=s["max_sec"],
        )
    )

md_lines.extend(
    [
        "",
        f"JSON details: `{out_json}`",
    ]
)

out_md.write_text("\n".join(md_lines) + "\n", encoding="utf-8")

print(f"wrote {out_md}")
print(f"wrote {out_json}")
PY
