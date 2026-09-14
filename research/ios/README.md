# iHorizon - separate iOS bring-up

Latest bring-up: the actual no-JIT core now builds for iPhoneOS and passes two
Core::System Initialize/destruction cycles on the physical phone, together with
100 timing reset cycles and a small software-backed memory check on 16 KiB host
pages. The optional `SUYU_BUILD_IOS_CORE_PROBE` target uses the companion iHorizon
checkout via `IHORIZON_APP_SOURCE`; its scripts build the pinned dependencies.
This is not process startup, service execution, rendering, or game boot.
The historical prototype description below remains the standalone default.

This is an **additive, content-free research prototype**, not a working Switch
emulator app or a Mario Kart 8 Deluxe iOS compatibility claim. It does not modify
Lattice. The application identity is `iHorizon`, with bundle identifier
`org.ihorizon.app`.

## Checkpoint

Based on Suyu `4f1b898a5eec6eff1d449049c8102d52ac356dd8` on `mk8-recomp`.
That includes the build-time `SUYU_NO_JIT` change (`718f1d79`) and the strict
no-fallback change (`ee164653`). The companion `dougchansan/mk8-recomp` pipeline
and Lattice's documented separation of interpreter/private-AOT lanes informed
the design. No private Lattice implementation was copied here.

| Piece | Status at this checkpoint |
| --- | --- |
| Three-module static link, lookup, ADD/SVC boundary, failure guards | Tested on Linux with an original ABI-mirror fixture |
| Index-symbol collision mitigation | Reproduced in the fixture; target-scoped fix tested |
| Actual current exporter integration test | Script provided; not executed in the original Linux sandbox |
| UIKit diagnostic app / Xcode generation | Source provided; Apple SDK build not yet verified |
| Suyu `ArmRecomp` adapter | Uses real API names; not compiled against the full core here |
| Full Suyu iOS core, renderer, audio, input, game boot | Not implemented/validated by this checkpoint |

Read [TEST_REPORT.md](TEST_REPORT.md) for the exact evidence, and
[HANDOFF.md](HANDOFF.md) before extending this branch.

## What the prototype does

The default build creates three tiny original C modules with the existing
Suyu static-registration shape. They are linked into the host executable, not
loaded with `dlopen`. It exercises absolute-PC lookup, a synthetic ADD/SVC
boundary, incomplete module sets, missing blocks and load-order failures.

The CMake wrapper reuses the real exporter's `recomp_static_<module>` and
`recomp_runtime_shared` targets. It compiles ABI assertions against **every
input module's own header**, including the current SIMD/TLS/FP and host-memory
fields, and rejects mixed copies of the generated runtime/header.

At the pinned revision, the static target renames the older symbols but omits
`recomp_build_index`, `_recomp_index_view`, and `recomp_image_index`. The wrapper
adds these three renames to each module target, without editing an owner's
export. The negative test deliberately removes that mitigation and verifies a
multi-module link failure. The fixture reproduces the relevant symbol shape;
it is not a substitute for the real-exporter integration test.

With a private export, the probe only links and validates descriptors. It does
**not** call game blocks, set module bases, load a title, or initialize Suyu.
Linking is not semantic, content-identity, or whole-title coverage validation.

## Linux / native portable tests

From the repository root, with Python 3.9+, CMake 3.24+, Ninja, a C/C++20 compiler
and `nm`:

```sh
python3 research/ios/tests/test_portable.py
cmake -S research/ios -B /tmp/switch-aot-probe -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/switch-aot-probe --parallel 2
ctest --test-dir /tmp/switch-aot-probe --output-on-failure
```

The first suite is independent of the full emulator and its submodules. All
checks use runtime failures or unittest assertions, not C/C++ `assert` that
would disappear in Release builds.

In a complete checkout, test the **actual emitter** on original AArch64
instructions, without a game or any keys:

```sh
bash research/ios/scripts/test-real-emitter.sh /tmp/switch-aot-real-emitter-01
```

Use a new output directory each time. The driver calls the real `EmitProject`
for `rtld`, `main` and `sdk` using only `ADD x0,x0,#1; SVC #1`, then statically
links the generated C. It will expose exporter/runtime issues that a mirror
fixture cannot detect. Do not replace this gate with a fixture pass.

## Mac / iPhone diagnostic app

On an Apple Silicon Mac with Xcode selected and CMake/Ninja/Python installed:

```sh
bash research/ios/scripts/build-ios.sh iphoneos
# Or the separately built simulator diagnostic app:
bash research/ios/scripts/build-ios.sh iphonesimulator
```

The default is **unsigned**. For a device, use a locally configured Apple team
through `SWITCH_AOT_TEAM_ID`, or select the team in the generated Xcode project.
Never commit signing identities, profiles, certificates, device identifiers or
unredacted build logs. No JIT entitlement is requested. The default deployment
target is iOS 18.0, configurable with `SWITCH_AOT_MIN_IOS`.

This app displays diagnostic results; it does not expose a fake game-launch
button. A successful build or on-device diagnostic is not a Suyu port.

To compile private, owner-generated modules for iPhoneOS instead of the fixture:

```sh
export SWITCH_AOT_EXEFS=/absolute/private/export/aot_cache/exefs
export SWITCH_AOT_BUILD_DIR=/absolute/private/build/switch-aot-iphoneos
bash research/ios/scripts/build-ios.sh iphoneos
```

The directory must contain `recomp_registration.c` and all module projects.
A Linux `.so` or x86-64 `.a` cannot serve as an iPhoneOS library; recompile the
portable C with the iPhoneOS SDK. Do not mix macOS, simulator and device objects.
The wrapper resets the export path each invocation to avoid silently retaining
a previous private export in CMake's cache. Use separate build trees regardless.
Generated C/CMake is trusted owner-local **build input**, never an on-device
import format or an untrusted download.

## Full-core integration boundary

`src/core_bridge.cpp` is a small real-API adapter, deliberately excluded from the
diagnostic app. `switch_aot_attach_suyu(target)` requires an actual `core` target
and a `SUYU_NO_JIT` build. It does not manufacture a substitute HLE implementation.

1. Verify exact executable/module identity, then install the static callbacks
   **before** process loading / `KProcess::InitializeInterfaces`.
2. Route each real module load index/base through `SetRecompBaseSetter`.
3. Bind and seal within the load lifecycle, before publishing the process to
   the applet manager. The current lazy setter runs too late; merely calling
   `FinalizeStaticImages` after System::Load returns is unsafe. This binding
   integration remains unimplemented and is not used by the initialization probe.
4. Join every guest thread before clearing/replacing the registry.

The registry passes the **absolute** guest PC to the generated lookup, which
already subtracts its module base. It must not subtract that base a second time.
The generated guest context is still serviced by Suyu's real memory/kernel/GPU
stack. A standalone SVC stub is not sufficient.

The adapter's lifecycle assumptions must be verified against the actual core
load/start paths before using it. There is no thread-safe hot replacement.

## Content boundary

Keep all title data, generated game C, native game objects, keys, firmware,
saves, signing material and device logs outside this public repository. There
is no automatic fetching of any of those inputs, release upload, App Store
submission, or executable download facility here. Research scope is not a
claim of legal clearance for redistribution. Preserve upstream licensing and
provenance when distributing any applicable software.
