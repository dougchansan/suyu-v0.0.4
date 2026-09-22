# JIT-free AOT: execution architecture and measurement contract

## Scope and status

This change implements the **session-evidence and performance-validation layer** on top of the
existing Suyu static recompiler. It is not a new CPU translator, interpreter, iOS application,
Metal backend, or demonstration of game performance parity.

The baseline inspected is `mk8-recomp` at `8644cb009d355beed656f721e08ef7c69d3369de`.
`src/core/recompiler/arm64_to_c.h` is the existing exporter, and
`src/core/arm/recomp/arm_recomp.cpp` is the existing execution/HLE bridge. The bridge already
has guest-PC lookup, module-base registration, register/context transfer, SVC handoff,
exclusive-monitor access, generated-code guard negotiation, and optional desktop JIT fallback.
This PR does not change those execution semantics.

| Component | Status in this change |
| --- | --- |
| Static export, native compilation, ArmRecomp dispatch | Existing code; not rebuilt or exercised end-to-end here |
| Lightweight shared telemetry API | Implemented by extracting existing declarations without changing their layout |
| Session counter baselines and conservative strict-workload gate | Implemented and component-tested |
| Optional JSONL system-frame trace, wired into production PerfStats | Implemented and component-tested |
| Fixed-work trace validation and median/p95/p99 regression checks | Implemented and tested |
| Lost frame-count race, zero-frame rate, short-session CSV fixes | Implemented and regression-tested |
| Complete AArch64 interpreter fallback | Not implemented; no interpreter is silently substituted |
| Native iOS linking/signing, on-device run, gameplay correctness | Outstanding validation gates |
| CPU-only, HLE-only and GPU timestamp scopes, new graphical HUD | Follow-on work; not represented by invented values |

## Execution architecture

```text
BUILD MACHINE (before app signing)
  locally supplied, permitted guest modules and metadata
       -> existing static control-flow analysis / ARM64-to-C export
       -> native host compilation and optimization
       -> link generated object code into the application
       -> sign the application and its executable components

DEVICE (no runtime CPU code generation in the strict configuration)
  guest PC -> existing registered block lookup -> compiled native block
                     |                              |
              missing/changed code            guest state + exit reason
                     |                              |
            strict failure/diagnostic       existing ArmRecomp bridge
                                                    |
                                       memory / kernel / SVC / services
                                                    |
                                         existing GPU / audio / input

OBSERVABILITY (new)
  existing cumulative CPU counters + PerfStats system-frame boundaries
       -> per-session baseline and latched validity/policy flags
       -> PerfStatsResults.aot (frontend-readable) and optional JSONL trace
       -> offline identity/completeness/no-JIT gates
       -> fixed-work median / p95 / p99 comparison
```

Guest and host both using AArch64 does **not** permit arbitrary copying of guest instructions
into an iOS executable. Translation still must preserve guest address spaces, TLS, ABI,
exception/SVC behavior, FP control/status, atomics, scheduling and instruction-cache semantics.
Host page size, alignment, register conventions and signing remain separate constraints.

Apple documents the executable-signing boundary in
[App code signing process](https://support.apple.com/guide/security/app-code-signing-process-sec7c917bf14/web).
Compile and link native executable code before signing; do not turn downloaded guest bytes
into executable pages at runtime. This design needs no runtime JIT-enabling step for that
precompiled code. It does not establish App Store eligibility or entitlements for unrestricted
runtime code generation. No signing entitlement changes are included here.

## Strict and development policies

For the strict target, build the existing core with `SUYU_NO_JIT=ON` and use
`SUYU_RECOMP_STRICT=1` for the existing fail-closed execution policy. Confirm both against the
actual executable, rather than relying only on build-directory names. The default comparator
requires the runtime to report that JIT was compiled out.

A desktop development build may retain the existing hybrid AOT/JIT behavior to investigate
missing coverage. Such a build is **not** an acceptable strict candidate, even when no fallback
attempt happened in one run. `--allow-runtime-strict` explicitly relaxes the compiled-out
requirement for a runtime-strict desktop measurement; it does not turn that binary into an
iOS-validated or JIT-free artifact.

An interpreter could eventually provide a separate, explicit `AOT + interpreter` policy, but
it needs a complete architectural state contract, validated instruction semantics, bounded
scheduling slices, memory/atomic behavior, cache invalidation, and per-engine accounting.
An unsupported instruction must stop, not be skipped or replaced with a guessed result.
This PR intentionally does not add a partial interpreter or weaken the strict policy.

## Counter semantics: avoid false coverage percentages

The new `recomp_stats.h` exposes the existing producer API without pulling in ArmInterface.
`Aot::Session` samples it, subtracts a session baseline and never resets shared CPU counters.
Repeated HUD queries therefore cannot consume another observer's CPU counts. Counter decreases
invalidate the session permanently; unsigned subtraction cannot manufacture a huge count.
Hybrid policy, missing code guards and diagnostic cutoffs are likewise latched.

The producer uses independent atomic loads, not a globally synchronized snapshot. This is
observational telemetry, not an attestation or an execution-policy enforcement mechanism.
`PerfStats` is currently created after application-process publication in `core.cpp`: the
baseline does **not** cover guest activity preceding that construction. The strict gate applies
to the observed session window, not a claim about every instruction since process launch.

| Field | Meaning and limitation |
| --- | --- |
| `static_blocks` | Delta of the existing static-block counter, including producer-accounted chains; not guest instructions or elapsed CPU time |
| `jit_transitions` | Preserved legacy name. At the inspected revision the producer sums lookup-miss and unhandled-instruction fallback attempts; it does not count retired JIT instructions or necessarily successful JIT entries |
| `svc_calls` | SVC handoff count, not service execution duration |
| `lookup_misses`, `unhandled`, `no_fallback` | Coverage/unsupported/blocked-path diagnostics, not a denominator for percentage coverage |
| `jit_compiled_out` | Runtime build-configuration evidence, not an executable-memory audit |
| `backend_at_shutdown` | Last observed backend; `non_aot` can mean teardown, NCE or another backend, not necessarily JIT |
| `strict_workload_observed` | Conservative observed-window gate, not game correctness, security or performance proof |

The strict gate requires positive static-block activity, monotonic counters, guarded AOT,
strict policy on all AOT samples, no non-AOT completed frame, and zero fallback attempts,
misses, unhandled/blocked paths or forced cutoffs. Normal inactive teardown does not erase
valid earlier observations. An empty session cannot pass. The comparator recomputes this
gate rather than trusting the serialized boolean.

**Do not calculate `static_blocks / (static_blocks + jit_transitions)` as AOT coverage.**
Those quantities have incompatible units. Instruction coverage, CPU time coverage and GPU
time are explicitly `null` in comparison output until measured with valid instrumentation.

## Frame statistics and tracing

`PerfStatsResults` retains its existing fields and adds the raw atomically consumed game-frame
count and `aot` report. The game-frame counter now uses `exchange(0)` rather than a load followed
by a separate reset, preventing increments between those operations from disappearing.
Zero-frame/invalid-duration observations produce finite rates. CSV shutdown no longer constructs
an invalid iterator range when five or fewer frames were recorded.

Tracing is disabled by default. Set the following variables **before launch**; do not mutate
process environment variables concurrently with emulation. Each trace must have a fresh path.
The parent directory must already exist. Paths are opened in append mode to avoid destroying
previous evidence, and the comparator rejects multiple concatenated sessions.

```sh
export SUYU_AOT_TRACE=/absolute/existing/directory/candidate.jsonl
export SUYU_AOT_BUILD_ID=<commit-and-build-configuration>
export SUYU_AOT_WORKLOAD_ID=<fixed-replay-or-fixture-identifier>
export SUYU_AOT_CONTENT_ID=<locally-computed-module-and-update-identity>
export SUYU_AOT_CONFIG_ID=<non-CPU-settings-identity>
export SUYU_AOT_MACHINE_ID=<same-device-OS-and-driver-identity>
export SUYU_RECOMP_STRICT=1
# Launch the existing frontend with your locally prepared static image.
# Finish the fixed workload and close emulation normally to flush the footer.
```

Angle-bracket values are placeholders, not literal shell values. Content and machine identities
are caller-supplied labels/hashes, not cryptographic attestation by the runtime. Do not include
personal identifiers, keys, credentials, full local paths or game data in metadata. The title ID
and host architecture are recorded automatically. For an iOS app, the frontend must eventually
provide an equivalent configuration/lifecycle interface; this PR does not add that UI.

JSONL schema 1 has one `session`, contiguous `frame` records and one orderly-shutdown `summary`.
A crash, killed process, missing footer, I/O failure, non-finite sample, unsupported schema,
invalid types, duplicate keys, counter reset, code error or concatenated run is not valid evidence.
The trace is diagnostic data, not a trusted or signed report.

`active_ms` is the existing **system-frame active duration**, excluding the limiter/waits as
measured by PerfStats. It is not CPU-only time, GPU time or game-present time. `period_ms` is
the system-frame end-to-end interval including limiting and intervening work. GPU work can be
asynchronous; neither measurement identifies the bottleneck by itself. Per-frame sampling/I/O
can perturb performance: instrument both builds equally and compare against uninstrumented
controls before treating a small difference as an optimization result.

## Reproducible comparisons

Use the same actual hardware, OS, workload/content and non-CPU settings for baseline and
candidate. Record complete matching fixed work, not equal wall-clock durations. The validator
requires equal system-frame counts; that is a necessary check, not proof that the game performed
the same work. External replay/game-state verification is still required. A non-AOT baseline is
not automatically identified as Dynarmic; independently record and verify the baseline backend.

```sh
python3 tools/aot/compare_runs.py baseline.jsonl candidate.jsonl \
  --warmup-frames 120 --min-frames 300 --max-regression-percent 5 \
  --output comparison.json
```

The defaults require at least 420 recorded frames in each run and discard the first 120 timing
samples. The strict execution gate still considers the whole observed session, including warmup.
Exit 0 means median, p95 and p99 active system-frame times all meet the configured threshold;
exit 1 means a measured regression; exit 2 means invalid/incompatible/incomplete evidence.
Percentiles use linear interpolation. Period statistics are reported separately, not mislabeled
as CPU execution time. A pass establishes only this instrumented workload's threshold result,
not universal JIT parity. Repeat paired runs and inspect thermal state, variance and frame
correctness; this comparator is not a statistical significance test.

## Local contract tests

These tests have no emulator submodule/download requirements:

```sh
cmake -S tests/aot -B build-aot -DCMAKE_BUILD_TYPE=Release
cmake --build build-aot --parallel 2
ctest --test-dir build-aot --output-on-failure

cmake -S tests/aot -B build-aot-sanitized -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++ -DAOT_TEST_SANITIZERS=ON
cmake --build build-aot-sanitized --parallel 2
ctest --test-dir build-aot-sanitized --output-on-failure
```

C++ tests exercise the new metrics/trace headers. A separate component target compiles the
**actual modified `src/core/perf_stats.cpp`** with explicit filesystem/settings/fmt test doubles
and simulated CPU-counter providers. Python tests round-trip the emitted files and exercise
malformed/adversarial traces and CLI exit codes. The generated test support is local build output,
not replacement production headers. All fixtures are synthetic; no guest game is executed.

## Remaining implementation and validation gates

1. Build the entire Qt/SDL core and affected frontends against real dependencies on each target;
   verify aggregate/API consumers and telemetry lifecycle under actual loads. No full-core build
   was possible in the restricted partial-checkout sandbox.
2. Build and link locally generated native AOT modules for Apple ARM64 into a signed iHorizon
   application. Validate generated ABI offsets, relocations, instruction guards, TLS, FP/SIMD,
   atomics, memory maps and code invalidation on hardware; do not infer these from x86-64 tests.
3. Launch without a debugger/JIT helper, verify strict policy and executable-memory behavior,
   and demonstrate correct frames, input, audio and gameplay. Validate both restart and teardown.
4. Collect matched on-device baseline/candidate evidence where the baseline execution mechanism
   is available and verified. Do not extrapolate Linux timings to iPhone performance.
5. Add distinct CPU engine timers or instruction-retirement accounting, HLE scope timing and
   real GPU timestamp queries before drawing a CPU/GPU bottleneck conclusion. Expose those
   through the frontend HUD with explicit units and unavailable states.
6. Only after correctness and measured hot paths justify it, optimize direct chaining, lookup
   caches, profile-guided build-time optimization, memory paths and generated code. Retain the
   strict failure path while unresolved coverage is being repaired.

The GPU/service subsystem remains necessary regardless of CPU translation strategy. This PR
neither removes it nor claims that replacing CPU JIT alone makes a Switch title an iPhone app.
