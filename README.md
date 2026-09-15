# suyu

<h1 align="center">
  <br>
  <img src="dist/suyu.svg" alt="suyu" height="128">
  <br>
  <b>suyu</b>
  <br>
</h1>

<h4 align="center">
Nintendo Switch emulator and native recompiler — based on <a href="https://git.eden-emu.dev/eden-emu/eden">Eden</a>, which itself descends from yuzu.
</h4>

<p align="center">
  <a href="#status">Status</a> |
  <a href="#static-recompilation">Static recompilation</a> |
  <a href="docs/releases/v0.0.10.md">Changes in v0.0.10</a> |
  <a href="#building">Building</a> |
  <a href="#license">License</a>
</p>

---

> **This is a continuation of suyu, which was archived upstream at v0.04.**
>
> [`suyu-emu/suyu-v0.0.4`](https://github.com/suyu-emu/suyu-v0.0.4) is a public
> archive and no further development was planned there. This fork picks it up
> from commit `d1d09321d7` and continues the numbering: **v0.0.10**.
>
> The name and version line are kept deliberately, so the lineage stays legible.
> `BUILD_FULLNAME` reads `suyu v0.0.10 (mk8-recomp)` — the suffix says *which*
> 0.0.10 a binary is, since the archived repository could in principle be picked
> up by others too. See [PROVENANCE.md](PROVENANCE.md).
>
> Work happens on the `mk8-recomp` branch, driven by
> [mk8-recomp](https://github.com/dougchansan/mk8-recomp) — a project statically
> recompiling Switch titles to native x86-64 using this recompiler. Fixes that
> are not recompiler-specific are listed below and are useful to anyone running
> suyu.

## About

suyu is a Nintendo Switch emulator and AArch64 native recompiler written in C++. It can run decrypted Switch titles using either:

- **HLE/emulation mode** — full hardware-level emulation via the suyu core (GPU, CPU, audio, services)
- **Recompiler mode** — ahead-of-time static recompilation of Switch AArch64 game code to native x86-64 executables, bundled with suyu's HLE backend

Based on [Eden](https://git.eden-emu.dev/eden-emu/eden), with suyu's own improvements to UI, recompiler, and platform support.

## Status

Current version: **v0.0.10**, continuing from the archived v0.04.

Upstream was inconsistent about its own version — the repository is named
`suyu-v0.0.4`, the tag reads `v0.04-latest`, and `BUILD_FULLNAME` was hardcoded
to `v0.04`. This fork normalises to the three-part form. Read literally, `v0.04`
means 0.4, which was evidently not the intent.

Platforms: Windows and Linux both build and run. macOS (arm64) builds and runs.
The GUI comes up, and games now boot under Vulkan/MoltenVK with the bundled
MoltenVK library; see [macOS](#macos).
Tests are off in that configuration. Android is inherited from upstream and
untested since the fork; iOS is not included.

Linux needs five things Windows does not, all handled by
[`scripts/build-suyu.sh`][bld] in the consuming project:

- CMake 3.31 (`CMakeModules/CPMUtil.cmake` requires it; Ubuntu 24.04 ships 3.28)
- `-Dfmt_FORCE_BUNDLED=ON` — the system fmt 9 has no `format_string::get()`, and
  suyu only forces the bundled one inside a branch that does not apply here
- Qt6 Charts, which Ubuntu packages separately
- system Boost
- skipping the `externals/ownfoil` submodule, whose own nested submodule no
  longer resolves; nothing in suyu's CMake references it

Building on Linux found two defects that MSVC had silently accepted: literal
carriage returns inside string literals, and a boost forwarding header that
resolved only where CPM had fetched boost.

[bld]: https://github.com/dougchansan/mk8-recomp/blob/main/scripts/build-suyu.sh

## Static recompilation

[v0.0.10 downloads](https://github.com/dougchansan/suyu-v0.0.4/releases/tag/v0.0.10) are an experimental static execution checkpoint. **Use Hybrid AOT + JIT for best performance.** Static can load and run more slowly; this is a compatibility milestone, with optimization still in progress.

| Mode | Purpose |
|---|---|
| suyu static (Experimental) | Ahead-of-time AArch64 code with suyu HLE; use the separate `no-jit` binaries for a host with Dynarmic entirely absent. |
| Dynarmic JIT (Baseline) | Dynamic compilation for comparison and general compatibility. |
| Hybrid AOT + JIT | Static code with JIT fallback; recommended for normal play and performance. |

The `no-jit` downloads are compiled with `-DSUYU_NO_JIT=ON` and audited for Dynarmic build inputs and executable symbols. Selecting static export mode in an ordinary host is a separate fallback policy; it does not remove the dynamic compiler from that host. No-JIT hosts require compiled coverage and cannot run unsupported AArch32 or runtime-generated code.

**Regenerate existing static modules for ABI 4.** Instruction side entries cover aligned addresses inside discovered blocks, and a bounded nonrecursive module loop reduces host dispatch. Automatic title bundles validate manifests, image hashes, ABI and instruction bytes. Hosted library launches use the current bundle rather than stale detached launchers.

Current local testing reaches controller prompts, menus, attract rendering and the race starting grid without fallback. The starting grid was verified during a bounded idle observation after the replay ended. Race transitions outlast the JIT fixture; matching timing and full-race validation remain open. Tested paths are evidence of compatibility, not a guarantee for all titles or instructions.

Recording and playback are armed at boot. Use separate functional fixtures when loading times differ, record screenshots at milestones, and retain a bounded idle observation after EOF. Keep exact EOF and later milestone verdicts separate. Compare performance only with identical work, interleaved arms and an idle machine.

Older speedup numbers used a retired title-screen input fixture and predate the current guarded emitter. They do not describe v0.0.10 gameplay performance. The current slowdown is being profiled; no new speedup is claimed.

See [release notes](docs/releases/v0.0.10.md) and the [campaign and regression safeguards](docs/static-campaign.md). Build/test scripts and synthetic instruction suites are maintained in [mk8-recomp](https://github.com/dougchansan/mk8-recomp).

## Changes in v0.0.5

Five of these are defects in suyu itself rather than recompiler work, and affect
ordinary emulation. Each is one commit.

### Fixes

- **Installed updates and DLC in NAND were never indexed.** `GetFileAtID` tried
  eight storage-layout variants but skipped every odd index except 7, so the
  `.cnmt.nca` suffix was only ever looked for at the cache root — never inside a
  `000000XX/` directory, which is exactly where meta NCAs are stored and what
  `InstallEntry` writes. Every meta NCA in NAND was therefore unreachable and no
  installed update or DLC ever entered the cache, silently: a miss is
  indistinguishable from nothing being installed, which is why the frontend's
  installed-title listing reported zero. A second defect behind it let an older
  update overwrite a newer one, because the metadata map is keyed by title id
  with no version comparison — now the higher `GetTitleVersion()` wins.

- **Service handler registration dropped most commands.** A
  `FunctionInfoTyped<T>` array was walked through a `FunctionInfoBase*` with a
  different member layout. `sizeof()` agrees, so a size assertion passes and
  tells you nothing, but every element after the first was read from the wrong
  offset. `IpcController` registered 2 of its 6 handlers;
  `QueryPointerBufferSize` was among the lost, and it is part of CMIF session
  setup — so titles stalled in early service initialisation.

- **RomFS registration was silently dropped.** `emplace` where
  `insert_or_assign` was meant, so re-registration kept the stale entry and the
  title panicked on boot.

- **AOT image dispatch resolved every PC to the wrong module.** Double base
  subtraction made every lookup underflow, and a four-entry module table
  mismapped any title with more than one subsdk.

- **The AOT exporter read the base ExeFS, not the update's.** `PatchManager`
  replaces the ExeFS wholesale when an update is present, so the exported image
  diverged from live execution on any updated title.

### Additions

- **AArch64 → C recompiler work.** Exclusives now route through
  `Core::ExclusiveMonitor` (previously a plain load/store with `STXR` always
  reporting success, which makes every compare-and-swap non-atomic under real
  threads); FPCR/FPSR are modelled; the counter and `CTR_EL0` are read from the
  emulator's own sources so the two engines cannot disagree across a transition.
  Plus EXTR/ROR, ADC/SBC, LDPSW, exclusive pair forms, PRFM, and the DC
  cache-maintenance family.

- **A build with no dynamic recompiler in it.** `-DSUYU_NO_JIT=ON` drops
  dynarmic from every target. The exclusive monitor, which every process builds
  regardless of engine and which dynarmic previously owned the only
  implementation of, now has a standalone one; `ArmRecomp` holds the
  `Core::ExclusiveMonitor` interface rather than dynarmic's implementation of
  it. See [Static recompilation](#static-recompilation).

- **Static and runtime coverage instrumentation** — per-module JSON of
  emitted/unhandled counts, and runtime histograms of blocks executed,
  transitions by cause, unimplemented opcodes and SVCs.

- **`suyu-cmd --probe-isa-list`** reports each title's CPU architecture without
  booting it, reading the update's NPDM as well as the base's. An update can
  change the answer: a title can ship a 32-bit base and a later 64-bit update.

- **Diagnostics** — the NPDM log line carries a content hash, because size is not
  an identity - two updates of one title can share a `main.npdm` size while
  differing in architecture - and `PatchExeFS` names which provider slot
  answered for an update.

Full change set:

```
git diff d1d09321d7ab84252291e05b3efbc8a8dfa57481..mk8-recomp
```

## Legal Notice

suyu is a GPLv3 program, which allows fully free redistribution of its source code and releases liability of its authors for how this software is used as stated in Section 15 and 16.

The suyu Emulator program does not circumvent Nintendo's technological protection measures (TPMs) as the user is required to provide both the Nintendo Switch software & the encryption keys for these games, and the suyu Emulator uses a mode of the Advanced Encryption Standard (AES), an open encryption standard established by the US NIST, along with the encryption keys that the user themselves must lawfully acquire, to decrypt the software. As the standard is public and available to use by all, it does not constitute as the Digital Market Copyright Act's (DMCA) definition of "circumventing a technological measure" as defined in Section 1201(a)(3).

The suyu Emulator also falls under the exemptions stated in Section 1201(f) of the DMCA as this software was created for the purposes of reverse engineering the Nintendo Switch software (known as Horizon OS) to create interoperability with Nintendo Switch games and software with the Windows, macOS, and GNU/Linux operating systems.

Any aggressive DMCA claims or takedown notices against projects that explicitly disclaim piracy support, require user-provided keys, and limit functionality to interoperability (such as suyu) could constitute overreach or misuse of the DMCA.

As derived from §512(f), if Nintendo (or an affiliated entity) knowingly materially misrepresents that a project like suyu is infringing (or circumvents TPMs) when it does not, especially if they fail to consider fair use, interoperability exemptions under §1201(f), or the fact that the emulator requires user-provided keys and does not itself contain proprietary Nintendo code, they can be made liable for any Damages against suyu.

## Building

All three platforms below are verified: the Linux instructions were run end to
end in a clean Ubuntu 24.04 container, the Windows ones from a fresh clone, and
the macOS ones from a clean checkout on an M4 Pro (AppleClang 21, macOS 26 SDK).
Nothing here fetches a game, keys or firmware. Those are yours to supply.

CMake **3.31 or newer** is required. `CMakeModules/CPMUtil.cmake` demands it and
Ubuntu 24.04 ships 3.28, so on most distributions it has to come from Kitware
rather than from the package manager.

### Linux

```sh
sudo apt-get install -y \
  build-essential git curl ca-certificates pkg-config ninja-build nasm autoconf \
  qt6-base-dev qt6-base-private-dev libqt6svg6-dev libqt6charts6-dev \
  qt6-multimedia-dev libqt6opengl6-dev glslang-tools \
  libboost-dev libboost-filesystem-dev libboost-context-dev \
  libusb-1.0-0-dev libssl-dev \
  libavcodec-dev libavformat-dev libavutil-dev libavfilter-dev \
  libswscale-dev libswresample-dev \
  libzstd-dev liblz4-dev libgl1-mesa-dev libasound2-dev libpulse-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev \
  libxkbcommon-dev libxss-dev libxtst-dev \
  libwayland-dev libwayland-egl1 wayland-protocols libdecor-0-dev \
  libegl-dev libdrm-dev libgbm-dev libvulkan-dev
```

Three of those are easy to miss and each stops the build outright:
`glslang-tools` provides `glslangValidator`, which the host shader step looks up
by name; the X11 and Wayland headers are what SDL3 refuses to configure without;
and `libavfilter-dev` is required by `FindFFmpeg` even though the emulator only
decodes.

If the distribution's CMake is older than 3.31:

```sh
V=3.31.6
curl -fsSL -o /tmp/cmake.tar.gz "https://github.com/Kitware/CMake/releases/download/v${V}/cmake-${V}-linux-x86_64.tar.gz"
sudo mkdir -p /opt/cmake && sudo tar xzf /tmp/cmake.tar.gz -C /opt/cmake --strip-components=1
export PATH=/opt/cmake/bin:$PATH
```

Then:

```sh
git clone --recursive -b mk8-recomp https://github.com/dougchansan/suyu-v0.0.4 suyu
cd suyu
cmake -B build -GNinja \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_QT=ON -DYUZU_USE_BUNDLED_QT=OFF \
  -DYUZU_TESTS=OFF -DENABLE_WEB_SERVICE=OFF \
  -Dfmt_FORCE_BUNDLED=ON
cmake --build build --target suyu suyu-cmd
```

`-Dfmt_FORCE_BUNDLED=ON` is not optional on a distribution shipping fmt 9:
`logging.h` calls `format_string::get()`, which only exists from fmt 10, and
suyu forces the bundled copy only inside a branch that does not apply to an
ordinary Linux build. Without it the build dies several hundred files in.

Binaries land in `build/bin`.

### Windows

Visual Studio 2022 with the **Desktop development with C++** workload, Qt 6.9.3
for MSVC 2022, and the Vulkan SDK, which supplies `glslangValidator`. Qt via
aqtinstall if you do not have it:

```
aqt install-qt windows desktop 6.9.3 win64_msvc2022_64 -m qtcharts qtmultimedia
```

From a **Developer Command Prompt for VS 2022**:

```bat
git clone --recursive -b mk8-recomp https://github.com/dougchansan/suyu-v0.0.4 suyu
cd suyu
cmake -B build -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DENABLE_QT=ON -DYUZU_USE_BUNDLED_QT=OFF ^
  -DYUZU_TESTS=OFF -DENABLE_WEB_SERVICE=OFF ^
  -DCMAKE_PREFIX_PATH="C:/Qt/6.9.3/msvc2022_64"
cmake --build build --target suyu suyu-cmd
```

Point `CMAKE_PREFIX_PATH` at wherever Qt actually is; forward slashes save a
quoting argument with CMake. glslang 16 renamed `glslangValidator` to `glslang`
and suyu's CMake still searches for the old name, so if configure stops with
*"Required program `glslangValidator` not found"*, add
`-DGLSLANGVALIDATOR="C:/path/to/glslang.exe"`.

`suyu.exe` needs the Qt runtime beside it to start — `windeployqt` on the built
executable copies it in.

### macOS

Apple Silicon (arm64), with the Xcode command line tools and Homebrew:

```sh
brew install cmake ninja pkgconf boost ffmpeg sdl3 libusb enet glslang nasm qt
```

Then the same configure as Linux, plus Homebrew's prefix so Qt, FFmpeg and SDL3
are found:

```sh
cmake -B build-macos -GNinja \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_QT=ON -DYUZU_USE_BUNDLED_QT=OFF \
  -DYUZU_TESTS=OFF -DENABLE_WEB_SERVICE=OFF \
  -Dfmt_FORCE_BUNDLED=ON \
  -DVulkanHeaders_FORCE_BUNDLED=ON \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build-macos --target suyu suyu-cmd
```

`-DVulkanHeaders_FORCE_BUNDLED=ON` is the macOS counterpart of Linux's
`fmt_FORCE_BUNDLED`. Homebrew's `vulkan-headers` is found while
`vulkan-utility-libraries` is not, and `AddDependentPackages` refuses that
mixture, so configure stops with *"Partial dependency installation detected"*
rather than pairing a system copy of one with a bundled copy of the other. On a
machine with neither installed the flag is not needed.

glslang 16 ships `glslang` with `glslangValidator` as a symlink to it, so the
host shader step still finds the program by the old name.

Binaries land in `build-macos/bin`: `suyu.app` and `suyu-cmd`.

MoltenVK comes from the bundled CPM package (`V380-Ori/Ryujinx.MoltenVK`,
`v1.4.1-ryujinx`), is copied into `suyu.app/Contents/Frameworks/`, and is the
copy that gets loaded: `Vulkan::OpenLibrary` tries the bundle's
`libvulkan.1.dylib` and `libMoltenVK.dylib` before anything on the loader's
search path. The app therefore does not need MoltenVK installed. Pass
`-DYUZU_USE_BUNDLED_MOLTENVK=OFF` to prefer an installed MoltenVK instead.

macOS does not have NCE support yet. `HAS_NCE` is enabled for Android and Linux
arm64 only, so the CPU runs on dynarmic's arm64 backend, whose Mach
exception handler builds and links here.

### Android

Removed for now. The Gradle build is inherited from upstream and nothing here
has verified it since the fork, so publishing instructions for it would be
guessing. It comes back when it has been built and run.

## License

GPL-3.0-or-later. See [LICENSE.txt](LICENSE.txt).
