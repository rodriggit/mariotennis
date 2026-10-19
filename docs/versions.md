# Pinned upstream versions

Upstream projects have no releases for the recompiler/runtime, so we pin the
exact commits used by Zelda64Recomp v1.2.2 (2025-08-27), the reference
N64Recomp project. Source is read and built only at these commits.

| Project | Commit | Date | Source of the pin |
|---|---|---|---|
| N64Recomp/N64Recomp | a13e5cff96686776b0e03baf23923e3c1927b770 | 2025-08-11 | Zelda64Recomp v1.2.2 `.github/workflows/validate.yml` (N64RECOMP_COMMIT default) |
| N64Recomp/N64ModernRuntime | df7e820d8c55e4fcb4616c210cbb2c01b25cd48c | 2025-08-12 | Zelda64Recomp v1.2.2 submodule `lib/N64ModernRuntime` |
| rt64/rt64 | 23cab603c4f9f4a8b369b38e036f1aa484603878 | 2025-08-25 | Zelda64Recomp v1.2.2 submodule `lib/rt64` |

Local patches on top of these commits (`patches/`):
- rt64 `0001-vi-honor-x-scale.patch`: the VI renderer shows only the framebuffer
  columns the VI X scale covers between H start and H end, instead of the whole
  framebuffer width. Mario Tennis uses `osViSetXScale(0.79)` to magnify the left
  ~304 columns of its 384-wide framebuffer (title/splash screens); unpatched,
  RT64 shows all 384 and the picture sits in the left part of the screen.

SDL2 2.30.3 (official release package, fetched by CMake on Windows; same version as Zelda64Recomp v1.2.2).

Python tools (PyPI): rabbitizer 1.16.2, spimdisasm 1.42.4, splat64 0.50.0 (when needed).
