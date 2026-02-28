# HolyC

Cross-platform HolyC compiler. Faithfully implements all HolyC language features, except for TempleOS-specific semantics.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Quality And Perf

```bash
./scripts/lint.sh
./scripts/static_analysis.sh
./scripts/perf_baseline.sh ./build/holyc --suite all --opt-level 2 --runs 7 --warmup 2
./scripts/perf_opt_sweep.sh ./build/holyc --suite all --runs 3 --warmup 1
```

Perf reports are written to `.holyc-artifacts/perf-baseline.md` and `.holyc-artifacts/perf-baseline.json` by default.

## Some commands

```bash
./build/holyc preprocess tests/samples/preprocess.HC
./build/holyc ast-dump tests/samples/features.HC
./build/holyc ast-dump tests/samples/semantics.HC
./build/holyc ast-dump tests/samples/preprocess.HC
./build/holyc emit-llvm tests/samples/llvm.HC
./build/holyc emit-llvm tests/samples/llvm.HC --time-phases --time-phases-json=.holyc-artifacts/emit-llvm-phases.json
./build/holyc build tests/samples/llvm.HC -o ./build/llvm_app
./build/holyc build tests/samples/llvm.HC -o ./build/llvm_app --opt-level=3
./build/holyc jit tests/samples/jit.HC
./build/holyc jit tests/samples/jit.HC --opt-level=2
./build/holyc jit tests/samples/runtime_try.HC
./build/holyc jit tests/samples/switch_compat.HC
```

## TODO
- [ ] Test on platforms aside from macOS ARM64
- [x] REPL
