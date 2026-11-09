# MK64 course converter

Converts a Mario Kart 64 course from your own n64decomp/mk64 data into a Wiipeout track folder. You need Python 3. No other packages are necessary.

## Usage

```sh
python tools/mk64_convert/mk64_convert.py --mk64 /path/to/mk64 --course luigi_raceway --out /path/to/data
python tools/mk64_convert/check.py /path/to/data/mk64/luigi_raceway
```

- `--mk64`: the mk64 repository, with its assets extracted. The default is the `mk64` folder next to this repository.
- `--course`: a race course folder name from `mk64/courses/`, for example `luigi_raceway`.
- `--out`: the data root. The tool writes `<out>/mk64/<course>/`.
- `--scale`: world scale. The default is 16, which makes the MK64 road width (about 216 units) match the retail Wipeout road width (about 3500 units).
- `--no-textures`: writes untextured geometry only.

## Install on the Wii

Copy `mk64/<course>/` to `apps/wiipeout/mk64/<course>/` on the SD card or USB drive. The game shows the **MK64 CIRCUITS** option on the circuit select page only when at least one course folder holds all of these files:

`track.trv`, `track.trf`, `track.trs`, `track.trc`, `library.ttf`, `library.cmp`, `scene.prm`, `scene.cmp`, `sky.prm`, `sky.cmp`.

Do not commit converted data.

## What the converter writes

- **Road strip** (`track.*`): sections sampled along the MK64 track path, with independently measured edges and floor heights from the MK64 collision mesh. Faces are collision-only (`track.trc`) except for visible procedural weapon pickup pads mapped from MK64 item boxes. Physics, AI and lap logic use them. The converter fits cross-slope near the path and stops each edge at walls, floor ends, or floor departures from that plane. It prints edge widths and sources, cross-slope, and the number of sampled rows with nearby overhead geometry. Ceiling counts are approximate: samples span each strip row and may include scenery such as banks or walls.
- **Junctions**: alternate MK64 paths (Yoshi Valley, Koopa Troopa Beach) become junction branches. The converter skips branches that overlap another branch and prints the reason.
- **Scenery** (`scene.*`): the original course mesh, with the extracted textures; duplicate position-and-winding triangles are resolved before clipping. The game draws MK64 scenery textures with GX repeat. The converter clips triangles only where their UVs leave the 8-bit PRM range, and bakes mirrored axes into the texture.
- **Sky** (`sky.*`): a simple gradient dome.

## Limits

- The sampled strip approximates collision geometry between sections and does not reproduce jumps.
- Two of the Yoshi Valley routes are not converted.
- Partial alpha becomes fully transparent or fully opaque.
- Course objects and animations outside the course display lists are not converted.
