# Open Source Notices

This project bundles or consumes the following open-source components. Refer to the listed licenses (or the referenced files) if you need their text for redistribution.

## C / C++ dependencies

- **Open CASCADE Technology 7.9.3** (Homebrew prefix `/opt/homebrew/opt/opencascade`)
  - License: GNU Lesser General Public License v2.1 (LGPL-2.1-only). Some Open CASCADE modules are additionally covered by the Open CASCADE Technology Public License (OCPPL); consult https://dev.opencascade.org/index.php/occt_sourcecode for the exact distribution and license notices.  When distributing binaries that link to these libraries, honor the LGPL-2.1 runtime obligations (provide the LGPL text, enable relinking, and document the third-party usage).

- **meshoptimizer (Arseny Kapoulkine)**
  - Source tree under `Stp2Tile/dep/meshoptimizer` (including `gltf` and `js` subdirectories).
  - License: MIT (see `Stp2Tile/dep/meshoptimizer/LICENSE.md`).

## JavaScript dependencies (Tile-Viewer)

- **three** (Three.js) – MIT (see https://github.com/mrdoob/three.js/blob/dev/LICENSE).
- **3d-tiles-renderer** – MIT (see https://github.com/3d-tiles-renderer/3d-tiles-renderer/blob/main/LICENSE).
- **vite** – MIT (see https://github.com/vitejs/vite/blob/main/LICENSE).
- **esbuild** – MIT (see https://github.com/evanw/esbuild/blob/main/LICENSE).

## Shared MIT notice (three, 3d-tiles-renderer, vite, esbuild, meshoptimizer)

```
MIT License

Copyright (c) [copyright holders]

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Keep this notice (or the individual license files) next to any distribution of this project so that recipients can see the open-source licenses governing these dependencies.