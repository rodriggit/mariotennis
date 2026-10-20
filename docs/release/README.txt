Mario Tennis Recompiled {VERSION} - Windows x64
================================================

A native PC port of Mario Tennis (Nintendo 64, USA), made by static recompilation
with N64Recomp, N64ModernRuntime and RT64.

Source code, issues and the build instructions:
https://github.com/rodriggit/mariotennis


YOU NEED YOUR OWN ROM
---------------------
This download contains no ROM. You need a dump of Mario Tennis (USA), game code
NM8E, revision 0, as a .z64, .n64 or .v64 file. Other regions and revisions are
not accepted.


FIRST RUN
---------
Drop the ROM file onto MarioTennisRecompiled.exe, or run it once from a command
prompt with the ROM's path:

    MarioTennisRecompiled.exe "C:\path\to\Mario Tennis (USA).z64"

The ROM is checked and a copy is kept with your saves, so later runs need no
argument: just open the executable.

Requirements: Windows 10 or 11, 64-bit, and a graphics card with Direct3D 12 or
Vulkan support.


CONTROLS (defaults)
-------------------
The keyboard and the first gamepad both control player 1; gamepads 2-4 are
players 2-4. Gamepad names are Xbox's; other controllers SDL recognises map the
same way by button position.

    N64            Keyboard         Gamepad
    Control stick  Arrow keys       Left stick
    A              X                A
    B              C                B or X
    Z              Z                Left trigger
    L              A                LB
    R              S                RB or right trigger
    Start          Enter            Start
    C buttons      I / J / K / L    Right stick
    D-pad          T / F / G / H    D-pad

    F11 or Alt+Enter: toggle fullscreen

Rumble works on gamepads that support it.


SETTINGS
--------
Controls and graphics options are in config.ini, created next to the executable
on the first run (or in the save folder below if that folder isn't writable).
Every option is explained in the file. Changes take effect on the next start;
delete the file to get the defaults back. If a setting isn't understood, a
message lists it at startup and it keeps its default.


SAVES
-----
Saves and the stored copy of the ROM are in:

    %APPDATA%\mariotennis-recomp\MarioTennisRecompiled\

To continue a saved tournament, load it from the game's save menu.


KNOWN LIMITATIONS
-----------------
- Transfer Pak (Game Boy connectivity) and Controller Pak are not supported.
- 4:3 only, original frame rate.


LICENSE
-------
This program is free software under the GNU General Public License v3.0
(LICENSE.txt). Its source code, including the patches applied to RT64, is at
the address above, under the tag {TAG}. It includes third-party components
listed in THIRD_PARTY_NOTICES.txt.

The executable contains the game's code, translated to C and compiled; the game
itself is (c) Nintendo / Camelot. This project is not affiliated with or
endorsed by Nintendo or Camelot.
