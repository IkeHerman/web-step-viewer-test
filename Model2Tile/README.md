# Model2Tile

Convert supported CAD input formats (STEP and static-mesh FBX)
into a 3D Tiles tileset (`tileset.json` + `.b3dm` content).

## Quick Start

```bash
make
./model2tile -o ../Tile-Viewer/public "../source_data/Downie Small"
```

This writes:

- `../Tile-Viewer/public/tileset.json`
- `../Tile-Viewer/public/tiles/*.b3dm`

## Build

From the project root:

```bash
make
```

This builds the `model2tile` executable.

### FBX build dependency

FBX import uses Assimp. If Assimp headers/libs are available at `ASSIMP_PREFIX`
(default `/opt/homebrew/opt/assimp`), the FBX traversal path is compiled with parsing support.
Without Assimp, the binary still builds but FBX traversal reports a dependency error at runtime.

## Command Line Usage

```bash
./model2tile [options] <input_model|input_directory>
```

- `<input_model|input_directory>`: Required. Either a single model file (for the selected/auto-detected importer) or a directory containing importer-supported files.

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

- `--input-format <auto|step|fbx>`  
  Select importer routing behavior.  
  Default: `auto` (chooses by input extension when possible)

- `--fidelity-artifacts-dir <path>`  
  Emit import/SceneIR/export evidence files for fidelity validation.

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
./model2tile ../source_data/model.step
```

### 2) Single STEP file with explicit output directory

```bash
./model2tile -o ../Tile-Viewer/public ../source_data/model.step
```

### 3) Process all STEP files in a directory

```bash
./model2tile -o ./out/tileset ../source_data/Downie\ Small
```

### 4) Custom content subdirectory + tile prefix

```bash
./model2tile \
  --out-dir ./out/tileset \
  --content-subdir content \
  --tile-prefix part_ \
  ../source_data/model.step
```

### 5) Verbose run with intermediate GLBs kept

```bash
./model2tile \
  --verbose \
  --keep-glb \
  --out-dir ./out/debug_tiles \
  ../source_data/model.step
```

### 6) Single FBX file

```bash
./model2tile --input-format fbx ../source_data/model.fbx
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

For both STEP and FBX paths, per-instance intermediate LOD files are emitted under:

```text
out/tileset/instance_lods/
  occ_0_high.glb
  occ_0_low.glb
  occ_1_high.glb
  occ_1_low.glb
  ...
```

## FBX Scope (first pass)

- Preserves FBX node instances (shared meshes produce multiple SceneIR instances with unique transforms).
- Performs conservative prototype dedup using geometry+material signatures.
- Generates the same `instance_lods/occ_<i>_{high|low}.glb` intermediate naming used by STEP.
- Current scope is static meshes and transforms only (no animation/skinning pipeline yet).

## Fidelity Regression Gate

Run the full fixture gate:

```bash
./tools/ci_fidelity_gate.sh
```

Run only the fixture assertions (requires existing `model2tile` build):

```bash
python3 tools/run_fidelity_suite.py
```

## Geometric Error Rationale

`geometricError` is derived from tile/node bounding-box diagonal (linear scale), not a fixed
multiplier between hierarchy levels. For octree partitioning, node diagonal naturally trends
toward half each level, so diagonal-based error tends to produce parent/child error near 2x
in regular splits. This gives smoother runtime refinement behavior than aggressive fixed ladders
(for example 4x per level), while remaining stable across model units and SSE targets.
