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
- rt64 `0002-tile-copy-fixes.patch`: two fixes to the GPU copies RT64 makes when
  a game samples one of its framebuffers as a texture.
  - New tile copy textures are cleared once before their first copy. Without the
    clear, Intel Iris Xe on D3D12 kept showing stale pixels (boxes around the
    crowd and line-judge sprites) even though the copy writes every pixel.
    RT64 main already does this with `needsDiscard`/`discardTexture`, so this
    part goes away when the RT64 pin moves past it.
  - A load that runs past the framebuffer's last row gets no tile copy and reads
    RDRAM. Mario Tennis fills 4 KB at 16 bits, draws a shadow into the first
    38-51 rows as a 64-wide I8 image, and loads all 64 rows; the copy covered
    only the drawn rows, so the rest came out dark (rectangular shadows).
- rt64 `0003-rect-linear-filtering.patch`: an enhancement option,
  `rect.forceLinearFiltering`, that samples the textures of rectangles (the game's
  2D: menus, scoreboards, title screens) with linear filtering even when the game
  asks for point sampling. config.ini's `filter_2d = linear` turns it on; it's
  off by default because the game builds big images (title logo, splash) from blocks
  loaded one at a time, and the filter has no neighbor texels at the joins, which
  show as cuts.

SDL2 2.30.3 (official release package, fetched by CMake on Windows; same version as Zelda64Recomp v1.2.2).

Python tools (PyPI): rabbitizer 1.16.2, spimdisasm 1.42.4, splat64 0.50.0 (when needed).
