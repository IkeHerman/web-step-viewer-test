on Mac:
## Summary

`Stp2Tile` ingests STEP assemblies via Open CASCADE, extracts occurrence metadata (prototypes, transforms, bounding boxes), and writes the geometric/occurrence data into an intermediate tile-friendly structure. `Tile-Viewer` is a lightweight web app that loads the exported tileset, renders it in `three.js`, and lets you fly through the scene.

## Installation Notes

- Build `Stp2Tile` on macOS with Homebrew’s Open CASCADE: `brew install opencascade` (or otherwise provide the library headers/libraries and point the Makefile’s `PREFIX`).
- `Tile-Viewer` is a Vite + Three.js project. Install Node.js 18+, run `npm install` from the `Tile-Viewer` folder, then `npm run dev` or `npm run build` as needed.
- The `.vscode/c_cpp_properties.json` file already points IntelliSense at `/opt/homebrew/opt/opencascade/include/opencascade`; adjust if your prefix differs.
