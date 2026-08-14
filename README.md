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

The default pipeline canonicalizes SSA and conservatively removes dead
instructions. `-opt` enables LocalDAG/InstCombine, SCCP, CFG simplification,
DCE, dominator-scoped GVN, in-loop global scalar promotion (globals are loaded
once in the preheader and stored back on exit edges), conservative LICM, compare-branch fusion, copy-aware
linear scan, spilled-constant rematerialization, immediate-aware `addi`/`slli`
selection, zero-register comparisons, pre-RA machine combine/DCE, post-RA
physical-register and spill-slot peepholes, and adjacent-block fall-through.
Dumps and statistics are
written to stderr, leaving stdout as valid assembly:

```sh
./build-wsl/compiler --dump-cfg --dump-ir --dump-mir \
  --dump-mir-after-ra --verify-each < input.tc > output.s
```

Inspect and verify the optimized pipeline with:

```sh
./build-wsl/compiler -opt --verify-each \
  --dump-ir-before-opt --dump-ir-after-each --dump-ir \
  --print-pass-stats < input.tc > output.s 2> optimization.log
```

`--dump-ir-before-opt` shows freshly constructed SSA, `--dump-ir-after-each`
adds a stable boundary after every pass, and `--dump-ir` shows final SSA.
`--dump-mir` is after phi-copy resolution and pre-RA machine optimization;
`--dump-mir-after-ra` is after register allocation and the post-RA peephole but
before frame lowering. Machine pass hit counts are included in
`--print-pass-stats` output. The default mode leaves all new machine-level
transformations and assembly fall-through emission disabled for stable A/B
comparison.

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
ELF text size, median QEMU wall time, and the course-compatible percentage and
points (`min(1, GCC_O2_Time / ToyC_Time) * 100 / 12`). The displayed percentage
is truncated and points are rounded to two decimal places, matching the remote
runner. By default each executable calls `main` once to match the course runner.
Increase the sample count when a benchmark is too noisy:

```sh
TOYC_BENCH_ITERATIONS=1 TOYC_BENCH_SAMPLES=3 \
  ./scripts/benchmark.sh ./build-wsl/compiler tests/cases/bench/*.tc
```

QEMU wall time is useful for regression comparisons on the same machine, but it
is not a target CPU cycle count. Final performance claims should be confirmed on
the course runner or real RISC-V hardware.

The supplied remote result snapshot and its inferred GCC `-O2` baselines can be
replayed without the RISC-V toolchain:

```sh
./scripts/check_remote_benchmark_formula.sh
```

This verifies all 12 displayed percentages and points, including the remote
total of 28.09/100. Inferred GCC milliseconds are approximate because the remote
page exposes only points rounded to two decimal places.

Before submitting, the optimization-mode sentinel can independently verify that
the command-line `-opt` path is active and removes both dead pure calls and an
exact hot loop:

```sh
./scripts/check_opt_mode.sh ./build-wsl/compiler
```

`optimization_robustness_tests` also exercises equivalent CFG, wrapper-call,
comparison-orientation, stride, and safety-barrier variants. These tests guard
optimization generality; the local `p01`-`p12` sources remain approximation
benchmarks and must not be treated as copies of hidden course inputs.
