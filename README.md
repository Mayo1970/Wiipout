# Wiipout

A native Wii port of wipEout, built on wipeout-reworked (an SDL3 fork of [wipeout-rewrite](https://github.com/phoboslab/wipeout-rewrite)).

### Working

- [x] Native libogc platform layer (`src/platform_wii.c`): video, input, audio, SD/USB files, timer, exit to the Homebrew Channel.
- [x] Native GX/TEV renderer (`src/render_gx.c`).
- [x] NTSC 480i/480p, PAL 576i and EuRGB60. 16:9 and 4:3.
- [x] HUD and menus stay inside the TV safe area.
- [x] Wii Remote (sideways), Wii Remote + Nunchuk, Classic Controller and GameCube controller.
- [x] Tilt air brakes on the sideways Wii Remote.
- [x] Rumble on the Wii Remote and the GameCube controller.
- [x] Sound effects, QOA music and the MPEG1 intro video.
- [x] Save data (`save.json`) on SD or USB.
- [x] SD I/O and audio mixing on separate threads, so music loads and saves do not stall the game.
- [x] View-frustum culling of the track, scene objects and ships.
- [x] Ship shadows without flicker.
- [x] Local 2-player split-screen (main menu > 2 PLAYERS).

## Install and build

See [INSTALLATION.md](INSTALLATION.md).

Short version:

1. Build with `tools/docker-make.sh PLATFORM=WII dist` (or `tools/docker-make.ps1` on Windows). The build runs in the devkitPPC Docker image.
2. Copy `dist/apps/wiipeout/` to `apps/wiipout/` on the SD card or USB drive.
3. Add your own wipEout game data to `apps/wiipout/wipeout/`.

The game data is not included. It is copyrighted. You must supply it.

CI builds `boot.dol` on every push and uploads it as an artifact.

## Wii controls

| Action | Wiimote sideways | Wiimote + Nunchuk | Classic Controller | GameCube pad |
| --- | --- | --- | --- | --- |
| Steer | D-pad left/right | Stick or D-pad left/right | Left stick or D-pad left/right | Main stick or D-pad left/right |
| Pitch | D-pad up/down | Stick or D-pad up/down | Left stick or D-pad up/down | Main stick or D-pad up/down |
| Thrust | 2 | A | B (bottom) | A |
| Brake left | Tilt left | Z | L or ZL | L |
| Brake right | Tilt right | C | R or ZR | R |
| Fire | 1 | B | Y (left) | B |
| Change view | Minus | Minus | X (top) | Y |
| Look | — | — | Right stick | C-stick |
| Menu select | 2 | A | B (bottom) | A |
| Menu back | 1 | B | Y (left) or A (right) | B or X |
| Pause/start | Plus | Plus | Plus | Start |
| Exit to Homebrew Channel | HOME | HOME | HOME | None |

Stick steering is analog. On Wii the main stick also drives the D-pad, so it steers without rebinding. You can rebind controls in Options > Controls.

## Tools

- `tools/docker-make.*`: run any `make` target in the devkitPPC container.
- `tools/dolphin_smoke/`, `tools/dolphin_docker/`: smoke tests in Dolphin.
- `tools/endian_test/`: host-side check of the big-endian asset loaders.
- `tools/mem_budget/`: memory budget for MEM1 and MEM2.

## Credits

- Dominic Szablewski (phoboslab): [wipeout-rewrite](https://github.com/phoboslab/wipeout-rewrite), qoa, pl_mpeg.
- Dave Gamble: cJSON.
- unofficial-fish: ship alpha bleed and mine light fixes.
- devkitPro: devkitPPC and libogc.

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the licences.

## AI Disclosure
This project has been made with the assistance of AI.

## License

This project inherits the licence status of wipeout-rewrite.
