# Initial Linux validation report

## iHorizon takeover validation

The original eight portable tests and the actual emitter integration were rerun
successfully in WSL Ubuntu using GCC 15.2.0, CMake 4.2.3 and Linux Python 3.14.4.
The actual `arm64_to_c.h` emitted all three original ADD/SVC modules; compilation,
static link and CTest passed before and after the Apple identity rename.
Generated-runtime compiler warnings remain for unchecked fread results and a
potentially truncated save-directory buffer; these paths were not exercised by
the two-instruction probe. Apple compilation/signing/device tests remain unrun
because the networked MacBook was offline. The older report below is retained
as historical evidence, not the latest emitter status.

Checkpoint: September 13, 2026 (Pacific/Honolulu).
Base: `4f1b898a5eec6eff1d449049c8102d52ac356dd8`.
Environment: x86-64 Linux; CMake 3.31.6; Ninja; GCC Release and Clang Debug
with AddressSanitizer + UndefinedBehaviorSanitizer.

## Executed

`python3 research/ios/tests/test_portable.py`: **8/8 passed**.

| Test | Observed result |
| --- | --- |
| Release synthetic static link / ADD-SVC dispatch / symbol screen | Passed |
| `SUYU_NO_JIT=OFF` | Correctly rejected at configure |
| Remove three index-symbol renames | Correctly failed multi-module link |
| Mix generated runtime/header revisions | Correctly rejected at configure |
| Uniform but wrong `GuestContext` offset | Correctly failed compilation |
| Export path with spaces and square brackets | Built and passed |
| Private-shaped export with aborting block bodies | Metadata probe passed without executing blocks |
| Empty entitlement file and separate app display identity | Parsed and passed |

Separate Clang ASan/UBSan build: **1/1 CTest passed**, no sanitizer diagnostic
in this synthetic execution. This tests the registry/probe, not the Suyu core.
The symbol screen is scoped to that diagnostic executable; it is not evidence
that an unbuilt full iOS runtime has been audited.

## Not executed

- Real current exporter integration script (requires full upstream header).
- Full Suyu build, actual-core bridge compilation, or any game execution.
- Apple SDK configuration/compilation, codesigning, simulator or device run.
- Renderer, audio, controller, memory-pressure, power or performance tests.

The new source was built as a standalone overlay because the sandbox could
read/write GitHub through the connector but could not directly clone the full
repository. The original fixture mirrors the inspected upstream registration
and ABI/symbol shape; it is deliberately not described as real exporter output.
Run `scripts/test-real-emitter.sh` in the complete checkout before claiming the
actual emitter/static-runtime path is verified.

## Reproduce the sanitizer gate

```sh
cmake -S research/ios -B /tmp/switch-aot-sanitized -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build /tmp/switch-aot-sanitized --parallel 2
ctest --test-dir /tmp/switch-aot-sanitized --output-on-failure
```

The reported desktop MK8 replay and timing numbers belong to the existing
project documentation; they were not reproduced by this checkpoint.
