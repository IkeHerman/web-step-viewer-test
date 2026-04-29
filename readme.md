on Mac:
## Summary

`Model2Tile` ingests STEP assemblies via Open CASCADE, extracts occurrence metadata (prototypes, transforms, bounding boxes), and writes the geometric/occurrence data into an intermediate tile-friendly structure. `Tile-Viewer` is a lightweight web app that loads the exported tileset, renders it in `three.js`, and lets you fly through the scene.

The converter pipeline now includes importer routing (`--input-format auto|step|fbx`) and modular boundaries for import, scene IR, tiling, and tile content baking. FBX routing is scaffolded and currently reports a clear not-yet-implemented message.

## Installation Notes

- Build `Model2Tile` on macOS with Homebrew’s Open CASCADE: `brew install opencascade` (or otherwise provide the library headers/libraries and point the Makefile’s `PREFIX`).
- `Tile-Viewer` is a Vite + Three.js project. Install Node.js 18+, run `npm install` from the `Tile-Viewer` folder, then `npm run dev` or `npm run build` as needed.
- The `.vscode/c_cpp_properties.json` file already points IntelliSense at `/opt/homebrew/opt/opencascade/include/opencascade`; adjust if your prefix differs.
