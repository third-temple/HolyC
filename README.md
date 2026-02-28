# HolyC

HolyC compiler with an LLVM backend.

## Quick Start

```bash
git clone --recursive https://github.com/third-temple/HolyC
cd HolyC

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

This outputs the `holyc` compiler binary to the `build/` directory.

## Usage

```bash
./build/holyc --help
./build/holyc check tests/samples/features.HC
./build/holyc jit tests/samples/jit.HC
./build/holyc build tests/samples/llvm.HC -o ./build/llvm_app
./build/holyc run tests/samples/runtime_print.HC
./build/holyc repl
```

## Tutorial

### 1) Your first program

```hc
I64 Main()
{
  "Hello, World!\n";
  return 0;
}
```

Run it with JIT:

```bash
./build/holyc jit examples/hello.HC
```

Or build a native executable:

```bash
./build/holyc build examples/hello.HC -o ./build/hello
./build/hello
```

### 2) Try the REPL

```bash
./build/holyc repl
```

Inside REPL:

```hc
I64 X() { return 42; }
"X=%d\n", X();
```

### 3) Useful example programs

- `examples/hello.HC`
- `examples/fizzbuzz.HC`
- `examples/language/` (organized by language area)

## HolyC vs C

- Entry point is `Main`, not `main`.
- Common type names are HolyC-style aliases like `I64`, `U64`, `F64`, and `U0` (void-like).
- Printing uses HolyC print statements:
  - `"Hello\n";`
  - `"%d\n", value;`
- Default function arguments are supported.
- JIT execution is a first-class workflow (`holyc jit ...`, `holyc repl`).

## Notes

- TempleOS-specific OS/runtime APIs are out of scope.
- LLVM is required and built from the bundled `third_party/llvm` source tree.

## License

AGPL-3.0. See [LICENSE](LICENSE).
