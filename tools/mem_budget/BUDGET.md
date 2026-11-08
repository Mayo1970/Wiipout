# Wii memory budget

Source: `tools/mem_budget/run.sh` with the retail wipEout data (7 circuits, Venom and Rapier) and the `PLATFORM=WII` boot.elf.
The host run is 32-bit (`gcc -m32`), so pointer and struct sizes match the Wii.

## Measured

| Item | Bytes | Where it lives |
| --- | --- | --- |
| Hunk size | 16,777,216 | MEM2 |
| Hunk use at main menu (bump) | 2,156,832 | MEM2 hunk |
| Hunk peak, any race (bump + temp) | 3,202,168 (circuit 6) | MEM2 hunk |
| Temp peak (during loads) | 510,528 | MEM2 hunk |
| SFX samples (decoded `wipeout.vb`) | 1,650,488 | MEM2 hunk (in the bump figures) |
| Music decoder buffers | 45,096 | MEM2 hunk (in the bump figures) |
| Textures at menu, RGBA8888 / 16 bpp | 3,472,120 / 1,736,060 (145 textures) | renderer, MEM1 heap (WPO-11) |
| Textures, largest race, RGBA8888 / 16 bpp | 5,297,376 / 2,648,688 (circuit 5) | renderer, MEM1 heap (WPO-11) |
| malloc peak (cJSON save write) | 9,086 | MEM1 heap |
| boot.elf text / data / bss | 261,338 / 56,484 / 645,408 | MEM1 |
| Main stack (`__ppc_main_sp`, in bss) | 262,144 | MEM1 |

The intro (pl_mpeg) scene is not measured. Its buffers go to the hunk through `mem_bump`.

## Placement

- MEM2: the 16 MB hunk, cut from the top of arena 2 before any init. It holds models, tracks, SFX samples, music and intro buffers.
- MEM1: code, data, bss, the 256 KB main stack and the newlib heap. The heap holds the hot data: GX textures (16 bpp, about 2.6 MB at peak), two XFBs (about 1.5 MB for PAL), the GX FIFO and the audio mix buffers.

## Headroom

- Hunk: the peak is 3.2 MB of 16 MB. About 13.5 MB is free. A smaller hunk is possible later.
- MEM1: static use is about 1 MB of 24 MB. With textures, XFBs and the FIFO, about 19 MB stays free.
- Nothing is over budget.

## Open points

- SFX samples are in the MEM2 hunk. The audio task must check that ASND can DMA from MEM2, or copy them to MEM1.
- The Wii logs real values at boot, menu and race with `platform_wii_mem_log` once the main loop runs (WPO-9).
