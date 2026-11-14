# SKEV+ 1.0.0

Fixes and options for **SENRAN KAGURA ESTIVAL VERSUS** on PC (Steam): high frame rates with the right game speed, sharp textures, cleaner shadows, rendering bug fixes, 4K output and a faster boot.

Author: DarkSpartan

## Requirements

- SENRAN KAGURA ESTIVAL VERSUS on Steam (current version, 1.06).
- Windows 10 or 11.

## Install

1. Copy `dinput8.dll` and `skmod.ini` into the game folder, next to `SKEstivalVersus.exe`.
2. Start the game from Steam.

### Steam Deck and Linux (Proton)

Not tested yet; reports welcome. Proton only loads `dinput8.dll` when told to:

1. Copy `dinput8.dll` and `skmod.ini` into the game folder as above.
2. In Steam, right-click the game > **Properties** > **Launch Options**, and enter:
   `WINEDLLOVERRIDES="dinput8=n,b" %command%`

## What it does

All fixes are switched in `skmod.ini`: close the game, edit, start the game again. `0` turns a fix off and gives the original behaviour. Each option is explained in the file itself. After every start, `skmod.log` lists each fix as `on`, `off` or `SKIPPED`.

| Area | Options |
| --- | --- |
| Frame rate | `FrameRate` (60 to 360). Game speed stays right: animations, finishers, transformations, cutscenes, the sea and sky, menus, the lottery and controller menu scrolling all run at their normal speed. `FastMenus` (optional faster menu animations) |
| Textures | `MipFix` (no shimmering or sparkling textures in the distance or in motion), `Anisotropy` |
| Shadows | `ShadowResolution` (sharper shadows), `ShadowNoiseFix` (no bright speckles flickering inside shadows), `AOBlur` (smoother ambient occlusion) |
| Camera | `NearClip` (no skin flickering through swimsuits in zoomed-in shots; close shots, depth of field and shadows unchanged) |
| Resolution | `MaxResolution` (fullscreen up to 4K with DSR/VSR; never more than the display supports) |
| Boot | `SkipIntro` (skips the logos and, optionally, the opening movie) |

The game files on disk are never changed: everything is patched in memory while the game runs. Every fix checks the game code before patching it; on an unknown game version that fix is skipped and the rest still work.

## Uninstall

Delete `dinput8.dll`, `skmod.ini` and `skmod.log` from the game folder.

## License

Copyright (c) 2026 DarkSpartan

SKEV+ is free. You may use, copy, modify and share it, in whole or in part, including in other mods, as long as this notice stays with it. It may not be sold or offered only behind a paywall.

THE MOD IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, INCLUDING LOSS OF SAVE DATA, ARISING FROM OR IN CONNECTION WITH THE MOD OR ITS USE. Back up your saves; use at your own risk.

SKEV+ is an unofficial fan project. It is not affiliated with, endorsed or supported by Tamsoft, Marvelous, XSEED Games, or any other developer or publisher of SENRAN KAGURA ESTIVAL VERSUS. Do not contact them for support with the mod.

SENRAN KAGURA ESTIVAL VERSUS, its characters, artwork, movies and all other game content belong to their respective owners.
