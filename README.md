# Mario Tennis Recompiled

A native PC port of **Mario Tennis** (Nintendo 64, USA) made by static recompilation:
the game's MIPS code is translated to C with [N64Recomp] and runs on [N64ModernRuntime],
with graphics by [RT64] (Direct3D 12 or Vulkan) and window, audio and input through SDL2.

> **Status: work in progress.** The whole game recompiles and the executable builds,
> but it has not been played through yet. Expect crashes and graphical or audio bugs.

This is not a decompilation: there is no game source code here. The repository holds
the tools that analyse your ROM and drive the recompiler, plus the port's own code.

## You need your own ROM

No game data is included or distributed. You need a dump of **Mario Tennis (USA)**
(game code NM8E, revision 0) in `.z64`, `.n64` or `.v64` format. Other regions and
revisions are not supported.

## Building

There are no prebuilt releases yet. See [docs/building.md](docs/building.md) for the
full process: generating the recompiler inputs from your ROM, recompiling, and
building the executable.

## Running

The first time, pass the path to your ROM, or drop the ROM file onto the executable:

```
MarioTennisRecompiled.exe "path\to\Mario Tennis (USA).z64"
```

The ROM is checked and a copy is kept with your save, so later runs need no argument.
Saves (EEPROM) and the stored ROM are in
`%APPDATA%\mariotennis-recomp\MarioTennisRecompiled\`.

## Controls

The keyboard and the first gamepad both control player 1. Gamepads 2–4 are players 2–4.
These are the defaults; they can be changed in `config.ini` (see [Settings](#settings)).
Gamepads use SDL's standard layout (Xbox names below); PlayStation, Switch and other
controllers SDL recognises map the same way by button position.

| N64 | Keyboard | Gamepad |
|---|---|---|
| Control stick | Arrow keys | Left stick |
| A | X | A |
| B | C | B or X |
| Z | Z | Left trigger |
| L | A | LB |
| R | S | RB or right trigger |
| Start | Enter | Start |
| C buttons | I / J / K / L | Right stick |
| D-pad | T / F / G / H | D-pad |

| Key | Action |
|---|---|
| F11 or Alt+Enter | Toggle fullscreen |

Rumble works on gamepads that support it, as a Rumble Pak.

## Settings

Controls and graphics options are in `config.ini`, next to the executable. It's created
with the defaults on the first run, and each option is explained in the file itself.
If that folder isn't writable, the file goes in the save folder above instead. Changes
take effect on the next start; delete the file to get the defaults back.

- `[keyboard]` and `[gamepad]`: which keys or gamepad inputs press each N64 button or
  push the stick. Several per button are allowed, separated by commas.
- `[graphics]`:
  - `resolution`;
  - `fullscreen`;
  - `antialiasing` (MSAA);
  - `api` (D3D12 or Vulkan);
  - `upscale_2d`: which 2D images (menus, scoreboards, title screens) are drawn at the
    high resolution;
  - `filter_2d`: whether those 2D images are smooth or have sharp pixels (the default).
    It only has an effect with `upscale_2d = all`, because the game draws most of its
    2D at 1:1. Smoothing shows cuts where the game joins the blocks of its big images.

If a setting isn't understood, a message lists it at startup and it keeps its default.

## Known limitations

- **Transfer Pak** (Game Boy connectivity) and **Controller Pak** are not supported;
  the game sees no pak connected.
- 4:3 only, original frame rate.

## How it works

- `tools/`: Python scripts that read the ROM, decompress the code and overlays,
  find function boundaries, and generate N64Recomp's symbol file and config.
- `port/`: the executable and the glue the game needs on PC: overlay loading,
  the game's TLB-mapped code, pak access, rendering, audio and input.
- `docs/`: research notes and design. [docs/recomp_design.md](docs/recomp_design.md)
  covers how the game's overlays, TLB windows and boot code are handled.

## Credits

- [N64Recomp] and [N64ModernRuntime] by Wiseguy and contributors.
- [RT64] by the RT64 contributors.
- [Zelda64Recomp], the reference for how a port is put together; the RT64 renderer glue
  is adapted from it.
- [SDL2].

Mario Tennis is © Nintendo / Camelot. This project is not affiliated with or endorsed by them.

## License

[GPL-3.0](LICENSE), the same as N64ModernRuntime, which the port links.

[N64Recomp]: https://github.com/N64Recomp/N64Recomp
[N64ModernRuntime]: https://github.com/N64Recomp/N64ModernRuntime
[RT64]: https://github.com/rt64/rt64
[Zelda64Recomp]: https://github.com/Zelda64Recomp/Zelda64Recomp
[SDL2]: https://www.libsdl.org/
