# MK64 course converter

Convert a course from your own n64decomp/mk64 source data with Python 3; no third-party Python packages are needed:

```sh
python tools/mk64_convert/mk64_convert.py --mk64 /path/to/mk64 --course luigi_raceway --out /path/to/data
python tools/mk64_convert/check.py /path/to/data/mk64/luigi_raceway
```

The default `--mk64` is the sibling `mk64` directory next to the main `wipeout-reworked` repository. The output is `<data>/mk64/<course>/`; to put it on an SD card, use the data root `apps/wiipeout`, producing `apps/wiipeout/mk64/<course>/`. Converted data is local and must not be committed.

The default `--scale 16` compares the roughly 216 MK64 units between Luigi Raceway's straight road edge vertices (x = -223 and -7 near z = -823) with the approximately 3,500-unit median retail track01 road width. Retail track01's median center spacing is about 1,744 units. The collision strip uses a fixed 3,500-unit width and this spacing, regardless of `--scale`. The scale can be overridden to experiment with other courses.

The path is resampled into a single closed loop. Each sample uses the nearest vertical mesh intersection to the path height, falling back to the path height if there is no intersection. Invisible collision faces form two road lanes and two walls; Scenery uses the triangles in each display-list array once, with inherited display-list texture state and vertex colours. The source path section IDs and alternate branches are not converted. This means Yoshi Valley's multiple routes, jumps, banked surfaces and courses with overlapping scenery need further work; path height alone can choose the wrong surface. Objects and animations outside the course display lists are not converted.

The engine still needs a circuit entry pointing to this folder and an appropriate `start_line_pos` in its existing circuit settings. The converter does not register a circuit or change the engine. Binary checks do not establish that collision, AI, lap counting, visual culling or gameplay work on hardware.

Scenery textures are enabled by default. `--no-textures` emits Gouraud geometry only. The converter reads the MK64 link-only generator's symbol table and course texture list, decodes 8-bit PNGs with the Python standard library, and writes deduplicated 16-bit TIMs. Alpha is reduced to transparent or opaque because the engine TIM loader does not preserve intermediate alpha. Missing symbols are reported and use Gouraud triangles.

The converter tracks image, render tile, mask, tile size and texture scale through display-list calls. It clips triangles at repeat and clamp boundaries, interpolates position and shade, and rebases UVs for the engine's clamped textures. Each source triangle is emitted once using its first encountered display-list state. Triangles spanning more than 256 texture cells fall back to Gouraud and are counted. Clipping can increase the triangle count and memory use considerably; conversion prints both counts and the checker validates texture indices and UV bounds.
