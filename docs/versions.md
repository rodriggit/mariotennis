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
- rt64 `0004-shader-description-list.patch`: the raster shader cache keeps the
  descriptions of the shaders it was asked to compile (`submittedDescriptions()`).
  The port saves them to `shaders.bin` (AppData folder) and submits them again at
  startup, so they compile in the background during the intro instead of the first
  time a serve or an effect needs them (RT64 draws with its ubershader meanwhile,
  which made those moments stutter on weaker GPUs). RT64 main has no shader cache on
  disk either.
- rt64 `0005-reserve-native-buffers.patch`: the buffers RT64 uses to copy framebuffers
  to RDRAM and back are reserved at the next power of two. The size each copy needs
  depends on the rows it covers, so with exact sizes a buffer was recreated whenever
  the next copy was slightly bigger: hundreds of D3D12 buffer creations during the
  effects of strong shots, which switch framebuffers dozens of times per frame.
  Those switches still cost a full GPU synchronization each: every shadow is drawn
  at one address first at 16 and then at 8 bits, and RT64 can't reinterpret a
  framebuffer's format on the GPU yet.
- rt64 `0006-center-on-vi-visible-width.patch`: with `aspect_ratio = expand`, the
  original picture is centered on the framebuffer columns the VI shows rather than
  on the whole framebuffer. The menus draw into a 384-wide image of which the VI
  shows the left ~304 columns (osViSetXScale); RT64 centered the 2D on column 192,
  while the 3D (centered on the game's viewport, column 152) and the visible area
  were centered on 152: the 2D sat 40 * (factor - 1) columns too far right, off the
  middle of the screen and away from the 3D coins of the character select.
- rt64 `0007-forced-linear-tile-edges.patch`: with `rect.forceLinearFiltering` (the game
  asked for point sampling), the filter's extra texels stop at the tile's last texel
  (`lrs`/`lrt`) on axes without a mask. RT64 already clamps those axes when it decodes
  from TMEM, but not when it samples a decoded texture with a hardware sampler, which
  clamps at the texture's edge instead. Needed for 0008, whose shorter `lrt` would
  otherwise be ignored on that path.
- rt64 `0008-forced-linear-rect-texel-range.patch`: with `rect.forceLinearFiltering`, the
  tile the shader sees for a rect ends at the last texel the rect draws (from the
  texcoords RT64 already tracks per call; rects only, axes without a mask). The intro's
  Nintendo logo is 80 rows loaded in 6-row strips (LoadTile): the last one loads rows
  78-83 and draws 78-79, and the filter blended row 79 with rows past the image, a
  line under the logo.

SDL2 2.30.3 (official release package, fetched by CMake on Windows; same version as Zelda64Recomp v1.2.2).

Python tools (PyPI): rabbitizer 1.16.2, spimdisasm 1.42.4, splat64 0.50.0 (when needed).
