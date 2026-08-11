# ToyC compiler

The recommended development and validation environment is Ubuntu under WSL 2.
The compiler itself also builds on Windows, but the repository emits RV32IM/ILP32
assembly and uses the Linux RISC-V cross toolchain and QEMU for executable tests.

## WSL setup

From PowerShell, enter Ubuntu and install the tools if they are missing:

```powershell
wsl -d Ubuntu
```

```sh
sudo apt update
sudo apt install -y build-essential cmake gcc-riscv64-linux-gnu qemu-user
cd /mnt/d/Repo/compiler
sh scripts/check_wsl_env.sh
```

Ubuntu's `gcc-riscv64-linux-gnu` package can assemble RV32 code but does not ship
RV32 libc or libgcc. The test scripts therefore link the generated assembly with
the small freestanding Linux entry point in `tests/runtime/rv32_linux_start.S`.
This is intentional: the assignment observes the low 8 bits returned by `main`
and does not require libc.

## Build and correctness tests

```sh
cmake --preset wsl-release
cmake --build --preset wsl-release -j2
ctest --preset wsl-release
```

When the RISC-V tools are installed, CTest automatically includes `e2e_tests`.
That test generates assembly for every `tests/cases/e2e/*.tc` input, statically
links both normal and `-opt` RV32 executables, runs them with `qemu-riscv32`, and
checks their exit codes.

The stage-two pipeline is `CFG -> SSA IR -> RISC-V MIR -> linear-scan register
allocation -> frame lowering -> assembly`. Dumps are written to stderr, leaving
stdout as valid assembly:

```sh
./build-wsl/compiler --dump-cfg --dump-ir --dump-mir \
  --dump-mir-after-ra --verify-each < input.tc > output.s
```

Deterministic differential testing against GCC is available with:

```sh
./scripts/differential_test.sh \
  --compiler ./build-wsl/compiler --seed-start 0 --count 100
```

## Relative performance check

```sh
./scripts/benchmark.sh ./build-wsl/compiler tests/cases/bench/*.tc
```

The benchmark compares normal ToyC output, `-opt` output, and GCC `-O2`. It reports
ELF text size and median QEMU wall time. Each executable calls `main` repeatedly in
one QEMU process so startup overhead is amortized. Tune the run length when needed:

```sh
TOYC_BENCH_ITERATIONS=500 TOYC_BENCH_SAMPLES=9 \
  ./scripts/benchmark.sh ./build-wsl/compiler tests/cases/bench/*.tc
```

QEMU wall time is useful for regression comparisons on the same machine, but it
is not a target CPU cycle count. Final performance claims should be confirmed on
the course runner or real RISC-V hardware.
