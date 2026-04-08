# Stp2Tile

Convert STEP (`.stp` / `.step`) input into a 3D Tiles tileset (`tileset.json` + `.b3dm` content).

## Quick Start

```bash
make
./stp2tile -o ../Tile-Viewer/public "../source_data/Downie Small"
```

This writes:

- `../Tile-Viewer/public/tileset.json`
- `../Tile-Viewer/public/tiles/*.b3dm`

## Build

From the project root:

```bash
make
```

This builds the `stp2tile` executable.

## Command Line Usage

```bash
./stp2tile [options] <input.step|input_directory>
```

- `<input.step|input_directory>`: Required. Either a single STEP file or a directory containing STEP files.

## Options

- `-o, --out-dir <path>`  
  Output directory for `tileset.json` and tile content.  
  Default: `../Tile-Viewer/public`

- `--content-subdir <name>`  
  Subdirectory under `out-dir` for tile content (`.b3dm`, optional debug `.glb`).  
  Default: `tiles`

- `--tile-prefix <prefix>`  
  Prefix for tile filenames.  
  Default: `tile_`

- `--keep-glb`  
  Keep intermediate `.glb` files (debug-friendly).

- `--discard-glb`  
  Remove intermediate `.glb` files after wrapping to `.b3dm`.

- `--tight-bounds` / `--no-tight-bounds`  
  Enable/disable tighter per-tile bounds.

- `--content-only-leaves` / `--content-all-levels`  
  Control whether tile content is emitted only at leaves or also at internal levels.

- `-v, --verbose`  
  Enable verbose logging (detailed pipeline/debug output).

- `-h, --help`  
  Show help.

## Examples

### 1) Single STEP file (default output location)

```bash
./stp2tile ../source_data/model.step
```

### 2) Single STEP file with explicit output directory

```bash
./stp2tile -o ../Tile-Viewer/public ../source_data/model.step
```

### 3) Process all STEP files in a directory

```bash
./stp2tile -o ./out/tileset ../source_data/Downie\ Small
```

### 4) Custom content subdirectory + tile prefix

```bash
./stp2tile \
  --out-dir ./out/tileset \
  --content-subdir content \
  --tile-prefix part_ \
  ../source_data/model.step
```

### 5) Verbose run with intermediate GLBs kept

```bash
./stp2tile \
  --verbose \
  --keep-glb \
  --out-dir ./out/debug_tiles \
  ../source_data/model.step
```

## Typical Output Structure

For `--out-dir ./out/tileset` and default `--content-subdir tiles`:

```text
out/tileset/
  tileset.json
  tiles/
    tile_0.b3dm
    tile_1.b3dm
    ...
```

(If `--keep-glb` is enabled, intermediate `.glb` files are also kept.)
