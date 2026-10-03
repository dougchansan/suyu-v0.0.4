# Windows Startup Crash and Portable Hotfix

Date: 2026-09-23

Local hotfix version: **0.0.4.1**

## Summary

The downloaded Windows v0.04 / 0.0.4 build crashed while applying an interface
profile. The failure was traced to access through an uninitialized optional
MicroProfile dialog pointer. Rebuilding source containing the existing pointer
initialization fix, adding a compile-time guard, and packaging the runtime
dependencies produced a working local portable app.

The initial local delivery was `build/local-0.0.4.1/`, tested before any GitHub
publication. The original installed emulator was left in place. The user later
requested publication of the verified Windows hotfix as `v0.0.4.1`.

## Symptoms and Evidence

- On first launch, choosing Gamer caused the application to freeze and close.
- Later launches showed a window frame, waited about 30 seconds, then closed.
- The installed binary logged its build as `main-9403b8399c-main`, with Qt 6.7.0.
- Windows recorded repeated access violations (`0xc0000005`) at
  `suyu.exe + 0x374919`.
- A local crash dump showed an invalid pointer read during the mode-switching
  code's debugger-widget visibility updates. The failing instruction read from
  `0x0000002004000400`.
- A Technical/Programmer-mode launch reproduced the same failure, ruling out a
  problem confined to the Gamer library view.

The installed build's revision was not present in the cloned Git history, and
the download did not include matching debugging symbols. The diagnosis used
the dump's exception, registers, and disassembly together with the corresponding
source control flow, rather than a symbolized stack naming the original line.

Startup log messages about missing keys, firmware, and the compatibility list
were not the cause of this access violation. A replacement build successfully
started with an empty portable data directory and no installed firmware or keys.

## Root Cause

[`GMainWindow::InitializeDebugWidgets`](../src/suyu/main.cpp) creates the
MicroProfile dialog only when `MICROPROFILE_ENABLED` is enabled. However,
`GMainWindow::ApplyAppMode` also used that dialog while showing or hiding debug
panes after a profile selection.

In the affected binary, the pointer could contain uninitialized memory when the
dialog was not created. A check such as `if (microProfileDialog)` is insufficient:
an uninitialized pointer can be nonzero without pointing to a valid widget.
Calling `setVisible()` through it then causes an access violation.

The profile selector saves the selected mode before returning to the main
window. On subsequent launches, that saved mode is applied again, reaching the
same failing code without displaying the selector. This explains why selecting
Gamer once appeared to break every later launch. Clearing settings alone would
not fix the invalid pointer access.

## Source Changes

1. **Retain the existing initialization fix.** The cloned source already included
   commit `4a1cb058d` (`frontend: zero-initialize the debugger pane pointers`). In
   [`main.h`](../src/suyu/main.h), `MicroProfileDialog* microProfileDialog{};`
   initializes the optional pointer to null. This was an existing upstream fix,
   not a new change made during this investigation.
2. **Guard access when profiling is disabled.** In
   [`main.cpp`](../src/suyu/main.cpp), the MicroProfile `setVisible()` call in
   `ApplyAppMode` is now inside `#if MICROPROFILE_ENABLED`, matching the dialog's
   conditional construction and other profiler UI accesses.
3. **Make Windows linking work with MinGW.** The local GCC build exposed an
   MSVC-only `#pragma comment(lib, "Dbghelp.lib")`. It was removed and replaced
   with `target_link_libraries(suyu PRIVATE dbghelp)` in the Windows branch of
   [`CMakeLists.txt`](../src/suyu/CMakeLists.txt). This preserves the Windows
   stack-trace dependency for both toolchains; it is separate from the original
   startup crash.
4. **Identify the rebuilt app.**
   [`GenerateSCMRev.cmake`](../CMakeModules/GenerateSCMRev.cmake) sets the displayed
   version to `suyu v0.0.4.1`.

The initial hotfix preparation is local commit `98a5fda2e`. The subsequent
MinGW linking changes and startup-test improvements were made after that commit;
the tested executable includes those working-tree changes as well.

## Local Build and Packaging

No native C++ build toolchain was initially available. An isolated MSYS2 UCRT64
environment was installed under `A:/suyu-local-build/`, without a system-wide
compiler installation. The successful build used GCC 16.2.0, Qt 6.11.2,
CMake 4.4.3, and Ninja 1.13.2.

The Windows GUI was built as `RelWithDebInfo` with
`-DCMAKE_CXX_FLAGS=-DMICROPROFILE_ENABLED=0`, explicitly exercising the configuration
involved in the crash. It used system Qt and the project's matched bundled
Vulkan headers/utility libraries. Optional Qt WebEngine, Qt Multimedia, LLVM,
unit tests, and the separate command-line executable were disabled for this
local build. This is not an otherwise identical rebuild of the original MSVC /
Qt 6.7.0 download.

The portable package contains the executable, 97 resolved runtime DLLs, Qt
plugins, and a `user/` directory. Debug information was stripped from the
packaged executable; the debug build was retained separately. Windows system
DLLs were excluded from the package.

An initial packaged launch could not find Qt's Windows platform plugin, despite
`plugins/platforms/qwindows.dll` being present. A `qt.conf` beside the executable
fixes the relative plugin lookup:

```ini
[Paths]
Plugins = plugins
```

The initial smoke-test launcher also reported no responsive main window when
inheriting the task console. A normal Windows desktop launch was responsive and
rendered Gamer mode correctly. The test now uses `UseShellExecute = $true` to
match that launch path. This was a local test-harness issue, not the diagnosis
of the original downloaded binary's access violation.

## Verification

[`Test-Startup.ps1`](../tools-local/Test-Startup.ps1) checks that the launched
process survives for 45 seconds and has a responsive main window. It closes only
the process it started and restores the saved `AppMode` and `RememberMode`
registry values. The `saved` option launches without a mode override.

| Check | Result |
| --- | --- |
| Full local Windows GUI compile and link, profiling disabled | Passed |
| Gamer startup | Responsive after 45 seconds |
| Technical/Programmer startup | Responsive after 45 seconds |
| Hacker startup | Responsive after 45 seconds |
| Normal launch with saved Gamer mode | Responsive after 45 seconds |
| Standalone packaged launch without toolchain paths | Passed |
| Copy entire folder to a different path containing spaces | Passed |
| Launch relocated copy with `PATH` limited to Windows system directories | Responsive after 45 seconds |
| User follow-up | Reported the app was working |

The temporary relocation copy was removed after verification. The delivered
portable folder was retained and opened in Explorer.

To repeat a packaged startup check from the repository root in PowerShell:

```powershell
& ([scriptblock]::Create((Get-Content -LiteralPath './tools-local/Test-Startup.ps1' -Raw))) `
    -ExecutablePath './build/local-0.0.4.1/suyu.exe' -Mode saved -ObservationSeconds 45
```

Close other suyu instances first and let the test close its own window. For a
direct mode test, replace `saved` with `gamer`, `programmer`, or `hacker`. The
saved-mode check assumes a mode has already been remembered. The local four-mode
runner temporarily suppressed the separate first-run checklist and restored its
setting afterward; it did not automate clicking through every first-run dialog.

These results establish startup and same-PC relocation, not game compatibility,
performance, every application feature, or operation on a different computer.
No gameplay or first-run profile-button automation was performed as part of the
recorded verification. These are local UCRT64 results, not GitHub Actions or
MSVC release-build results.

## Using the Portable App

Move the **entire** `build/local-0.0.4.1/` folder to the desired location, then
launch `suyu.exe` inside it. Keep all DLLs, `plugins/`, `qt.conf`, and `user/`
together. The app does not need the source checkout, VS Code, or the build tools
to launch.

The `user/` directory keeps the emulator's core data and configuration beside the
app. Some Qt preferences, including the remembered interface mode and first-run
checklist state, still use the Windows user registry. Thus the executable folder
is movable, but not every preference is stored inside it.

The portable build started with separate test data. Existing saves, firmware,
keys, and library configuration were not migrated from the original installation.

## Release Status

The initial request was completed as a local portable folder. Following the
user's confirmation that it worked, they requested a GitHub update. The
[v0.0.4.1 release page](https://github.com/Valdrrak/suyu-v0.0.4/releases/tag/v0.0.4.1)
records the published assets and their checksums. Release packaging uses a new
staging directory and excludes the local `user/` contents, logs, crash dumps,
keys, firmware, and games.

[`release.yml`](../.github/workflows/release.yml) includes Windows startup tests
and a successful-build gate for automated publication. It does not delete old
releases or replace an existing `v0.0.4.1` release. The Windows portable hotfix
is based on the locally tested UCRT64 build; other platform artifacts are not
claimed as part of that local validation.