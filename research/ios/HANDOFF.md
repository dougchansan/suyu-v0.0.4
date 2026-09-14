# Local-agent handoff: separate Switch static-AOT iOS research app

## Task

Continue `research/ios-static-aot` in `dougchansan/suyu-v0.0.4`. Bring the existing
JIT-free static recompilation toward an iPhone 16 private development build on
the owner's networked MacBook with Xcode. **Do not add Switch to Lattice, change
Lattice's bundle ID, or modify its code or release settings.** Do not claim this
checkpoint boots a game: it does not.

## Preserve this base and evidence

The branch starts at Suyu `4f1b898a5eec6eff1d449049c8102d52ac356dd8`, containing
`718f1d79` (build-time no JIT) and `ee164653` (strict no fallback). The companion
pipeline inspected was `dougchansan/mk8-recomp` at
`0f69a2dee4ff6b94f07b65873bc242c535b43a4f`. Inspect newer commits before changing
any pins; do not blindly reset or overwrite an existing working tree.

The initial sandbox had working authenticated GitHub reads/writes, but no direct
Git/DNS access to clone the full repository. Therefore its executed validation
used the new standalone source overlay and original ABI-mirror modules, not a
complete Suyu build or the actual exporter. Treat this as an explicit evidence
boundary, not as a missing checkbox you may silently mark complete.

## Operating rules

Work in a new local worktree. Preserve the existing successful Linux/Windows
JIT-free build and exact export as a reference. Do not modify or force-push the
main development branch, change visibility, merge this research branch, publish
an IPA, or upload owner data. Never reintroduce Dynarmic, a CPU JIT, NCE guest
execution, writable/executable mappings or a dynamic game-module downloader to
make an iOS test pass. Do not replace real kernel/GPU/audio services with stubs
and call that compatibility.

Generated native game code must be compiled and linked on the Mac **before
signing**. User data on the phone is data, not executable modules to discover or
load. Keep credentials in the owner's local Xcode/keychain configuration. Do
not request them in chat or print them in shared logs.

## Stage 1 — reproduce the content-free gates

```sh
git fetch origin research/ios-static-aot
git worktree add ../suyu-ios-research -b work/ios-static-aot origin/research/ios-static-aot
cd ../suyu-ios-research
python3 research/ios/tests/test_portable.py
bash research/ios/scripts/test-real-emitter.sh /tmp/switch-aot-real-emitter-01
```

Use another new worktree/output suffix if either already exists; do not delete
someone else's work. Capture exact commit, compiler/CMake versions and test
outputs locally. The first gate uses a mirror fixture. The second MUST compile
the real `src/core/recompiler/arm64_to_c.h` and run only the original two-opcode
synthetic input. Fix any failures in the real exporter/static runtime path with
regression tests. In particular, check the three newer index symbols and any
other shared-runtime-to-module callbacks for static-link correctness.

The existing mitigation is target-scoped in
`research/ios/cmake/StaticModules.cmake`; do not duplicate definitions if moving
the correction into the emitter. Keep the negative multi-module link test.
Compare actual `GuestContext`/`RecompHostMem` headers against the core's asserted
offsets; do not weaken ABI assertions to get a link.

## Stage 2 — signed synthetic iPhone app, still no game

Build the diagnostic app for iPhoneOS, not macOS ARM64 or simulator ARM64:

```sh
bash research/ios/scripts/build-ios.sh iphoneos
```

Use the generated `iHorizon.xcodeproj` to select a local team and the
paired iPhone 16, then build/install. The bundle identity must remain separate
from Lattice. Record whether the app starts and displays the synthetic PASS.
Run a device build without a debugger attached as well as a development run.
Keep device identifiers and signing metadata out of the report.

Audit final executable architecture/platform and entitlements using local
`file`, `lipo`, `otool`/`vtool`, `nm`, and `codesign` tools. Confirm there are no
CPU-JIT/guest-NCE symbols or JIT entitlements. A symbol scan is supporting
evidence, not proof about arbitrary dependencies; inspect source/build graphs
and memory-allocation paths too. Simulator success does not prove device success.

## Stage 3 — compile the owner's exact export, without executing it

Ask the existing owner workspace/configuration for its export path rather than
assuming a path from a screenshot. Reuse only a consistent exact export of the
version that passed the JIT-free Linux replay. Required private inputs:

- complete `aot_cache/exefs` including registry and every module's generated C;
- exact base/update executable identities, module ordering/build IDs and hashes;
- emitter/runtime revision and `SUYU_AOT_TRANSLATE_ALL` / extra-root settings;
- local content path for later boot, supplied independently by the owner.

Do not request a ROM, encryption keys, firmware, Apple password or certificate
upload into chat. Do not infer whole-game coverage from one recorded replay.
An uncovered path must remain a diagnostic failure, not invoke a hidden engine.

Set `SWITCH_AOT_EXEFS` and a new private `SWITCH_AOT_BUILD_DIR`, then build with
`build-ios.sh iphoneos`. This is a **private static link probe only**. Its app
must report descriptor count without executing game blocks. Keep generated
sources and native archives outside Git. Audit every archive for the iPhoneOS
platform slice. Limit compile parallelism initially; the real generated C can
be large. Benchmark Apple Clang optimization options only after correctness.

## Stage 4 — port the actual Suyu core as a separate library

Inspect the full source and dependency graph before choosing a frontend.
Create an additive iOS core target, without Qt/desktop SDL window assumptions,
web updater/telemetry, desktop executable discovery or game-module dlopen.
Keep original desktop targets working. Build with `SUYU_NO_JIT=ON`; explicitly
audit/disable any guest NCE path as well. Trace indirect uses of Dynarmic,
executable memory, assembler caches and platform-only dependencies.

Audit these source/dependency areas, reporting actual errors rather than
assuming they are already portable:

- memory reservation/mirroring, mapping protection, guest page tables versus
  host page sizes, memory pressure and address-space requirements;
- fibers/context switching, TLS, atomics/exclusives and real multicore ordering;
- Apple framework/link dependencies, filesystem/sandbox paths and process APIs;
- renderer capabilities, GPU memory tracking and shader compiler dependencies;
- audio output, controllers/touch, main-thread UI ownership and lifecycle.

The prototype adapter uses real `SetRecompLookup` and `SetRecompBaseSetter`
interfaces, but was not compiled against the core in the initial sandbox.
Include `core_bridge.h` in the actual frontend and call install before process
initialization, seal after all bases are assigned, run only after seal, and
clear only with all guest threads joined. Verify this ordering in the real core
rather than assuming `System::Load` has no thread-start side effects.

Before installing callbacks, verify the loaded executable/module set against a
build-time identity manifest. A title name or title ID alone is insufficient:
base/update revisions and module content matter. Refuse mismatches. Matching
generated runtime/header hashes checks ABI consistency, NOT game identity.

The registry currently assumes non-overlapping modules ordered by ascending
load base. Validate that against the loader; extend with executable ranges and
manifest identity where necessary. Surface errors through the frontend rather
than leaving a black screen on a suspended guest thread.

## Stage 5 — graphics, sound and lifecycle before commercial boot

Investigate both any existing native Metal backend and Vulkan portability via
MoltenVK. Do not assume either backend is complete merely because its files or
Apple conditionals exist. Start with a renderer-only synthetic clear/present on
a real `CAMetalLayer`, then representative shader/pipeline capabilities. Keep
CPU recompilation separate from normal graphics-driver shader compilation.

Use app-owned Documents for imported owner data/saves and Application Support
or Caches for regenerable caches. No directory scan should load native code.
Use GameController and an original touch overlay for input, and a platform audio
sink. Handle backgrounding, interruptions, memory warnings, rotation and stopping
without racing UI/guest/render threads. Never permanently block UIKit's main
thread with the emulator loop.

## Stage 6 — first private title boot and measurable acceptance

Only after the actual core links and synthetic device tests pass, boot the
owner's exact matched MK8 Deluxe content privately. Keep all real Suyu HLE/GPU
services active. Capture an ordered progression: load, CPU dispatch, SVC/service
initialization, first present, menu, input, audio, race. On an uncovered address
record module-relative PC / opcode and repair/re-export off-device. The phone
must not generate new native CPU code.

Acceptance must identify: source/export revisions; tested scene/replay; no-JIT
build and runtime audit; crash/coverage logs; wall time and memory high-water
mark; thermal state; device class and OS version; audio/input correctness;
foreground/background behavior. Separate boot success from playable speed and
from complete compatibility. Compare identical work rather than unrelated FPS
screens. Do not extrapolate the x86-64 benchmark to iPhone 16.

## Deliver at each real boundary

Commit source-only changes on this research worktree/branch. Retain sanitized
build logs and exact reproduction commands. Report what passed, what failed,
what remains unimplemented, and the first concrete blocker. Stop before any
public release or App Store operation. Do not alter Lattice.
