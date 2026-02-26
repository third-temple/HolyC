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
./scripts/perf_baseline.sh ./build/holyc
```

## Some commands

```bash
./build/holyc preprocess tests/samples/preprocess.HC
./build/holyc ast-dump tests/samples/features.HC
./build/holyc ast-dump tests/samples/semantics.HC
./build/holyc ast-dump tests/samples/preprocess.HC
./build/holyc emit-llvm tests/samples/llvm.HC
./build/holyc build tests/samples/llvm.HC -o ./build/llvm_app
./build/holyc jit tests/samples/jit.HC
./build/holyc jit tests/samples/runtime_try.HC
./build/holyc jit tests/samples/switch_compat.HC
```

## TODO
- [ ] Test on platforms aside from macOS ARM64
- [ ] REPL