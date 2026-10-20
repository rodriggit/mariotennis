# Building

You need your own Mario Tennis (USA) ROM (NM8E, rev 0). Nothing from the game is
in this repository; everything game-derived is generated into `build/`.

## Layout

The pinned upstream checkouts (docs/versions.md) sit next to this repository:

```
repos/
  mariotennis/          this repository
  N64Recomp/            a13e5cf, built: N64Recomp.exe, RSPRecomp.exe
  N64ModernRuntime/     df7e820, with submodules
  rt64/                 23cab60, with submodules
```

Other locations work too: pass `-DMT_RUNTIME_DIR=...` and `-DMT_RT64_DIR=...` to CMake.

RT64 needs the patches in `patches/rt64/` (see docs/versions.md). CMake applies them
to the RT64 checkout when it configures, skipping any that are already applied, and
stops with an error if one doesn't apply (checkout not at the pinned commit).

## Requirements (Windows)

- Visual Studio with "Desktop development with C++" and **C++ Clang tools for
  Windows** (the runtime and the generated code need clang-cl).
- A native CMake first in `PATH` (not MSYS2/devkitPro's).
- Python 3.11+ with a virtual environment in `.venv` (`pip install rabbitizer==1.16.2 spimdisasm==1.42.4`).

## 1. Generate the recompiler inputs

From `tools/`:

```
..\.venv\Scripts\python.exe build_recomp.py <ROM> ..\build
..\.venv\Scripts\python.exe check_syms.py ..\build
..\.venv\Scripts\python.exe gen_config.py ..\build
```

This writes `build/recomp.z64` (ROM + decompressed code), `build/syms.toml`,
`build/recomp.toml`, `build/aspMain.toml` and `build/mt_files.inl`.
`check_syms.py` must report 0 functions without a terminator and 0 bad jal targets.

Optional check that the port will link (must report 0 undefined functions):

```
..\.venv\Scripts\python.exe scan_unsupported.py ..\build ..\..\N64Recomp\src\symbol_lists.cpp ..\..\N64ModernRuntime ..\port
```

## 2. Recompile

Delete `build/RecompiledFuncs` if it exists, then from N64Recomp's output folder:

```
.\N64Recomp.exe <repos>\mariotennis\build\recomp.toml
.\RSPRecomp.exe <repos>\mariotennis\build\aspMain.toml
```

Output: `build/RecompiledFuncs/` and `build/rsp/aspMain.cpp`.

## 3. Build the executable

From this repository, in a Developer PowerShell for Visual Studio:

```
cmake -S . -B cmake-build -G "Visual Studio 18 2026" -A x64 -T ClangCL
cmake --build cmake-build --config Release --target MarioTennisRecompiled
```

CMake downloads the SDL2 2.30.3 development package from the official SDL
releases. Use `--config RelWithDebInfo` to keep a console with the runtime's logs.

## 4. Run

The first time, pass the ROM (or drop it onto the executable):

```
.\cmake-build\Release\MarioTennisRecompiled.exe <ROM>
```

The runtime keeps a copy of the ROM and the EEPROM save in
`%APPDATA%\mariotennis-recomp\MarioTennisRecompiled\`, so later runs need no argument.
Controls are in the [README](../README.md#controls).

## 5. Package a release

A release zip is built from a **Release** build (no console) of a tagged commit:

1. Set the version in `port/src/main.cpp` (`Version::from_string`) and commit.
2. Tag that commit: `git tag v0.1.0`.
3. Build the Release configuration. With the Visual Studio generator, `--config Release`
   as in step 3. With Ninja, use a separate build folder and name Visual Studio's LLVM
   tools explicitly, so that no other `ld`/`ar` in `PATH` (MSYS2, devkitPro) is picked
   up (adjust `$llvm` to your Visual Studio edition):

   ```
   $llvm = "C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/Llvm/x64/bin"
   cmake -S . -B cmake-build-release -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=$llvm/clang-cl.exe" "-DCMAKE_CXX_COMPILER=$llvm/clang-cl.exe" "-DCMAKE_LINKER=$llvm/lld-link.exe" "-DCMAKE_AR=$llvm/llvm-lib.exe" "-DCMAKE_MT=$llvm/llvm-mt.exe"
   cmake --build cmake-build-release --target MarioTennisRecompiled
   ```

4. Package it:

   ```
   .\tools\package_release.ps1 -Version 0.1.0 -BuildDir cmake-build-release
   ```

   `-BuildDir` is the build folder (default `cmake-build-release`; a Visual Studio
   build's `Release\` subfolder is found too). `-RuntimeDir` and `-Rt64Dir` default to
   the checkouts next to this repository.

The script checks that the executable has no console, that the version matches
`main.cpp`, and warns if the working tree is dirty or the tag is missing or not on the
current commit. It writes `dist/MarioTennisRecompiled-v<version>-windows-x64.zip` with:

- the executable, `SDL2.dll`, `dxil.dll` and `dxcompiler.dll` (RT64 compiles shaders
  while the game runs);
- `README.txt` for players (from [docs/release/README.txt](release/README.txt));
- `LICENSE.txt` (GPL-3.0) and `THIRD_PARTY_NOTICES.txt`, the licenses of everything
  linked into the executable or shipped next to it.

It prints the zip's SHA-256 for the release notes. Push the tag, then create the GitHub
release from it and attach the zip.

Releases are built locally, not in CI: every step from `build_recomp.py` onwards needs
the ROM, and putting the ROM or the code generated from it in the repository or a CI
service would distribute the game itself.

## Debugging aids

Environment variables read at startup (use a RelWithDebInfo build: console and symbols):

| Variable | Effect |
|---|---|
| `MT_TRACE=1` | log file loads, TLB mappings and `osViSetXScale` changes |
| `MT_TRACE_EEPROM=1` | print every EEPROM read and write the game makes (offset, size, result, data) |
| `MT_DUMP_STACKS=<s>` | after that many seconds, print every thread's call stack |
| `MT_RT64_DEV=1` | RT64 developer mode: F1 opens RT64's inspector (pause, browse the frame's draw calls), RDP warnings on the console |
| `MT_WATCH=<addr>:<size>,...` | RDRAM watchpoints (hex physical addresses): every 2 s, which code (game, runtime, RT64) read or wrote those 4 KB pages, grouped by call stack |
| (key) F9 | write RDRAM (8 MB) to `rdram_<n>.bin` in the current folder; N64 byte address `a` is at file offset `a ^ 3` |
| (key) F10 | print the framebuffers RT64 tracks (address range, width, height, last write) |
| `MT_RT64_RDRAM_COPIES=1` | sample framebuffers used as textures (crowd sprites, shadows, hit effects) from RDRAM instead of RT64's GPU copies: native resolution and slower; for comparing against the GPU copy path |
| `MT_PROFILE=<s>` | sampling profiler: every that many seconds, one line on the console (display lists/s, VI/s, busiest threads) and a report in `mt_profile.txt` in the current folder (hottest functions per busy thread) |
