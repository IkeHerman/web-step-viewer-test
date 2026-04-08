import * as THREE from "three";
import { FlyControls } from "three/examples/jsm/controls/FlyControls.js";
import { RoomEnvironment } from "three/examples/jsm/environments/RoomEnvironment.js";
import { TilesRenderer } from "3d-tiles-renderer";

// ----------------------------------------------------
// CONFIG
// ----------------------------------------------------
const TILESET_URL = "/tileset.json";
const FORCE_NON_PROXY_CONTENT = false;
const TARGET_SCREEN_SPACE_ERROR = 14.0;
const MAX_TILE_DEPTH = 6;
const INITIAL_AXIS_FIX = "flip-x-180"; // none | flip-x-180 | z-up-to-y-up

// ----------------------------------------------------
// DOM
// ----------------------------------------------------
const app = document.getElementById("app");
const hud = document.getElementById("hud");

// ----------------------------------------------------
// Debug state
// ----------------------------------------------------
const debugState = {
  mode: "original", // original | wireframe | normals | unlit
  doubleSided: false,
  showBounds: false,
};

const meshOriginalMaterials = new WeakMap();
const meshDebugMaterials = new WeakMap();
const originalMaterialSides = new WeakMap();
let rootBoundsHelper = null;
let didFrameFromGeometry = false;
let needsGeometryRefit = false;

const _tmpBox = new THREE.Box3();
const _tmpSphere = new THREE.Sphere();
const _tmpCenter = new THREE.Vector3();
const _tmpOffset = new THREE.Vector3();
const _tmpUnionMin = new THREE.Vector3();
const _tmpUnionMax = new THREE.Vector3();
const _tmpWorldSphere = new THREE.Sphere();
const _tmpMatrixPos = new THREE.Vector3();
let lastDebugCompactLine = "";
let lastFrameStats = null;
let hasLoadedAnyTile = false;
const visibleProxyTiles = new Set();
const visibleLeafTiles = new Set();

function isProxyTile(tile) {
  const tileUri =
    tile?.content?.uri ||
    tile?.content?.url ||
    tile?.contentUrl ||
    "";

  return /_proxy\.b3dm(?:$|[?#])/i.test(`${tileUri}`);
}

const geometryDebug = {
  meshSeen: 0,
  meshWithGeometry: 0,
  meshWithPosition: 0,
  matrixValid: 0,
  matrixInvalid: 0,
  sphereValid: 0,
  sphereInvalid: 0,
  boxValid: 0,
  boxInvalid: 0,
  attrFallbackUsed: 0,
  attrFallbackInvalid: 0,
  sampleFinite: 0,
  sampleInvalid: 0,
  pointFallbackUsed: 0,
  totalPositionCount: 0,
  totalSamplesTried: 0,
};

function collectGeometrySnapshot(maxMeshes = 10) {
  const summary = {
    meshCount: 0,
    withGeometry: 0,
    withPosition: 0,
    totalPositionCount: 0,
    samplesTried: 0,
    finiteSamples: 0,
    invalidSamples: 0,
    examples: [],
  };

  tiles.group.updateMatrixWorld(true);

  tiles.group.traverse((obj) => {
    if (!obj.isMesh) return;
    summary.meshCount++;
    if (!obj.geometry) return;
    summary.withGeometry++;

    const position = obj.geometry.attributes?.position;
    if (!position) return;
    summary.withPosition++;
    summary.totalPositionCount += position.count;

    const sampleCount = Math.min(position.count, 16);
    let meshFinite = 0;
    let meshInvalid = 0;

    for (let i = 0; i < sampleCount; i++) {
      _tmpMatrixPos.set(
        position.getX(i),
        position.getY(i),
        position.itemSize >= 3 ? position.getZ(i) : 0
      ).applyMatrix4(obj.matrixWorld);

      summary.samplesTried++;
      if (isFinite(_tmpMatrixPos.x) && isFinite(_tmpMatrixPos.y) && isFinite(_tmpMatrixPos.z)) {
        summary.finiteSamples++;
        meshFinite++;
      } else {
        summary.invalidSamples++;
        meshInvalid++;
      }
    }

    if (summary.examples.length < maxMeshes) {
      summary.examples.push({
        name: obj.name || "(unnamed)",
        positionCount: position.count,
        sampled: sampleCount,
        finite: meshFinite,
        invalid: meshInvalid,
      });
    }
  });

  return summary;
}

// ----------------------------------------------------
// Renderer
// ----------------------------------------------------
const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
renderer.setSize(window.innerWidth, window.innerHeight);
renderer.outputColorSpace = THREE.SRGBColorSpace;
renderer.toneMapping = THREE.ACESFilmicToneMapping;
renderer.toneMappingExposure = 1.1;
app.appendChild(renderer.domElement);

// ----------------------------------------------------
// Scene
// ----------------------------------------------------
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x111111);
const pmremGenerator = new THREE.PMREMGenerator(renderer);
scene.environment = pmremGenerator.fromScene(new RoomEnvironment(), 0.04).texture;
pmremGenerator.dispose();

// ----------------------------------------------------
// Camera
// ----------------------------------------------------
const camera = new THREE.PerspectiveCamera(
  50,
  window.innerWidth / window.innerHeight,
  0.01,
  5000
);

camera.position.set(1, 1, 1);
camera.up.set(0, 1, 0);

const clock = new THREE.Clock();

// ----------------------------------------------------
// Fly Controls
// ----------------------------------------------------
const controls = new FlyControls(camera, renderer.domElement);
controls.movementSpeed = 0; // set after framing
controls.rollSpeed = Math.PI / 10;
controls.dragToLook = true;
controls.autoForward = false;

// ----------------------------------------------------
// Lights
// ----------------------------------------------------
scene.add(new THREE.HemisphereLight(0xffffff, 0x222233, 0.8));
scene.add(new THREE.AmbientLight(0xffffff, 0.35));

const key = new THREE.DirectionalLight(0xffffff, 1.8);
key.position.set(5, 8, 3);
scene.add(key);

const fill = new THREE.DirectionalLight(0xffffff, 0.9);
fill.position.set(-6, 3, -4);
scene.add(fill);

// ----------------------------------------------------
// 3D Tiles
// ----------------------------------------------------
const tiles = new TilesRenderer(TILESET_URL);

function applyInitialAxisFix(target) {
  if (!target) return;

  target.rotation.set(0, 0, 0);

  if (INITIAL_AXIS_FIX === "flip-x-180") {
    target.rotation.x = Math.PI;
  } else if (INITIAL_AXIS_FIX === "z-up-to-y-up") {
    target.rotation.x = -Math.PI * 0.5;
  }

  target.updateMatrixWorld(true);
}

if (FORCE_NON_PROXY_CONTENT) {
  const manager = tiles.manager || tiles.loadingManager;
  if (manager?.setURLModifier) {
    manager.setURLModifier((url) => {
      if (/_proxy\.b3dm$/i.test(url)) {
        return url.replace(/_proxy\.b3dm$/i, ".b3dm");
      }
      return url;
    });
  }
}

tiles.addEventListener?.("load-model", (e) => {
  hasLoadedAnyTile = true;

  const tileScene = e?.scene || e?.tile?.cached?.scene;
  if (tileScene) {
    tileScene.traverse((obj) => {
      if (!obj.isMesh || !obj.material) return;

      if (!meshOriginalMaterials.has(obj)) {
        meshOriginalMaterials.set(obj, obj.material);
      }

      const materials = Array.isArray(obj.material) ? obj.material : [obj.material];
      for (const material of materials) {
        if (!material) continue;

        if (material.isMeshStandardMaterial || material.isMeshPhysicalMaterial) {
          material.envMapIntensity = Math.max(material.envMapIntensity ?? 1.0, 1.0);

          const hasPbrInputs =
            !!material.map ||
            !!material.normalMap ||
            !!material.roughnessMap ||
            !!material.metalnessMap ||
            !!material.emissiveMap;

          if (!hasPbrInputs && material.metalness >= 0.95) {
            material.metalness = 0.1;
            material.roughness = Math.max(material.roughness ?? 1.0, 0.8);
          }
        }

        material.needsUpdate = true;
      }

      applyDebugMaterialToMesh(obj);
    });
  }
});

tiles.addEventListener?.("load-error", (e) => {
  console.error("load-error", e);
});

tiles.addEventListener?.("dispose-model", (e) => {
  const tile = e?.tile;
  if (!tile) return;

  visibleLeafTiles.delete(tile);
  visibleProxyTiles.delete(tile);
});

tiles.addEventListener?.("tile-visibility-change", (e) => {
  const tile = e?.tile;
  if (!tile) return;

  if (e?.visible) {
    if (isProxyTile(tile)) {
      visibleProxyTiles.add(tile);
      visibleLeafTiles.delete(tile);
    } else {
      visibleLeafTiles.add(tile);
      visibleProxyTiles.delete(tile);
    }
  } else {
    visibleLeafTiles.delete(tile);
    visibleProxyTiles.delete(tile);
  }
});

tiles.addEventListener?.("load-tileset", () => {

    if (!didFrame) {
    didFrame = frameTileset();
    if (didFrame && hud) {
      hud.textContent =
        "Loaded tileset — WASD/RF to fly, mouse drag to look";

      //showRootBox();
    }
  }
  console.log("tileset loaded");

  if (debugState.showBounds) {
    updateRootBoundsHelper();
  }
});

tiles.setCamera(camera);
tiles.setResolutionFromRenderer(camera, renderer);

tiles.errorTarget = TARGET_SCREEN_SPACE_ERROR;
tiles.maxDepth = MAX_TILE_DEPTH;

// Ensure base path is correct
try {
  const u = new URL(TILESET_URL, window.location.href);
  tiles.setBasePath(u.href.substring(0, u.href.lastIndexOf("/") + 1));
} catch (_) {}

// Add tiles to scene
applyInitialAxisFix(tiles.group);
scene.add(tiles.group);

// ----------------------------------------------------
// Tileset bounding helpers
// ----------------------------------------------------
function getTilesetRootSphere(tiles) {
  const root = tiles.root;
  if (!root || !root.boundingVolume) return null;

  const bv = root.boundingVolume;

  // boundingVolume.sphere = [cx, cy, cz, r]
  if (bv.sphere) {
    return new THREE.Sphere(
      new THREE.Vector3(bv.sphere[0], bv.sphere[1], bv.sphere[2]),
      bv.sphere[3]
    );
  }

  // boundingVolume.box = center + 3 half-axis vectors (12 numbers)
  if (bv.box) {
    const b = bv.box;

    const center = new THREE.Vector3(b[0], b[1], b[2]);
    const hx = new THREE.Vector3(b[3],  b[4],  b[5]);
    const hy = new THREE.Vector3(b[6],  b[7],  b[8]);
    const hz = new THREE.Vector3(b[9],  b[10], b[11]);

    const radius = Math.sqrt(hx.lengthSq() + hy.lengthSq() + hz.lengthSq());

    return new THREE.Sphere(center, radius);
  }

  // region bounds require geospatial conversion
  return null;
}

// ----------------------------------------------------
// Frame tileset once root bounds are available
// ----------------------------------------------------
let didFrame = false;

function frameTileset() {
  
  const sphere = getTilesetRootSphere(tiles);
  if (
    !sphere ||
    !isFinite(sphere.center.x) ||
    !isFinite(sphere.radius) ||
    sphere.radius <= 0
  ) {
    return false;
  }

  const viewDir = new THREE.Vector3(1, 0.6, 1).normalize();
  const fov = THREE.MathUtils.degToRad(camera.fov);
  const fitDist = sphere.radius / Math.sin(fov / 2);

  camera.position
    .copy(sphere.center)
    .addScaledVector(viewDir, fitDist * 1.2);

  camera.lookAt(sphere.center);

  camera.near = Math.max(fitDist / 1000, 0.01);
  camera.far  = fitDist * 20.0;
  camera.updateProjectionMatrix();

  controls.movementSpeed = Math.max(sphere.radius * 0.5, 1.0);

  return true;
}

function frameFromSphere(sphere, viewDirection = null) {
  if (
    !sphere ||
    !isFinite(sphere.center.x) ||
    !isFinite(sphere.center.y) ||
    !isFinite(sphere.center.z) ||
    !isFinite(sphere.radius) ||
    sphere.radius <= 0
  ) {
    return false;
  }

  const viewDir = (viewDirection || new THREE.Vector3(1, 0.6, 1)).clone().normalize();
  const fov = THREE.MathUtils.degToRad(camera.fov);
  const fitDist = sphere.radius / Math.sin(fov / 2);

  camera.position
    .copy(sphere.center)
    .addScaledVector(viewDir, fitDist * 1.2);

  camera.lookAt(sphere.center);

  camera.near = Math.max(fitDist / 1000, 0.01);
  camera.far = fitDist * 40.0;
  camera.updateProjectionMatrix();

  controls.movementSpeed = Math.max(sphere.radius * 0.5, 1.0);
  return true;
}

function estimateLoadedGeometrySphere(outSphere) {
  geometryDebug.meshSeen = 0;
  geometryDebug.meshWithGeometry = 0;
  geometryDebug.meshWithPosition = 0;
  geometryDebug.matrixValid = 0;
  geometryDebug.matrixInvalid = 0;
  geometryDebug.sphereValid = 0;
  geometryDebug.sphereInvalid = 0;
  geometryDebug.boxValid = 0;
  geometryDebug.boxInvalid = 0;
  geometryDebug.attrFallbackUsed = 0;
  geometryDebug.attrFallbackInvalid = 0;
  geometryDebug.sampleFinite = 0;
  geometryDebug.sampleInvalid = 0;
  geometryDebug.pointFallbackUsed = 0;
  geometryDebug.totalPositionCount = 0;
  geometryDebug.totalSamplesTried = 0;

  let hasAnyMeshBounds = false;
  let hasAnyPointBounds = false;
  _tmpUnionMin.set(Infinity, Infinity, Infinity);
  _tmpUnionMax.set(-Infinity, -Infinity, -Infinity);

  tiles.group.updateMatrixWorld(true);

  tiles.group.traverse((obj) => {
    if (!obj.isMesh) return;
    geometryDebug.meshSeen++;
    if (!obj.geometry) return;
    geometryDebug.meshWithGeometry++;

    const g = obj.geometry;
    if (g.attributes?.position) {
      geometryDebug.meshWithPosition++;
      geometryDebug.totalPositionCount += g.attributes.position.count;
    }

    const worldMatrix = obj.matrixWorld;
    let matrixIsFinite = true;
    for (let i = 0; i < 16; i++) {
      if (!isFinite(worldMatrix.elements[i])) {
        matrixIsFinite = false;
        break;
      }
    }

    if (matrixIsFinite) geometryDebug.matrixValid++;
    else geometryDebug.matrixInvalid++;

    let useSphere = false;

    if (!g.boundingSphere) {
      g.computeBoundingSphere();
    }
    if (g.boundingSphere) {
      _tmpSphere.copy(g.boundingSphere);
      if (matrixIsFinite) {
        _tmpSphere.applyMatrix4(worldMatrix);
      }

      if (isFinite(_tmpSphere.center.x) && isFinite(_tmpSphere.center.y) && isFinite(_tmpSphere.center.z) && isFinite(_tmpSphere.radius) && _tmpSphere.radius > 0) {
        useSphere = true;
        geometryDebug.sphereValid++;
      } else {
        geometryDebug.sphereInvalid++;
      }
    }

    if (!useSphere) {
      if (!g.boundingBox) {
        g.computeBoundingBox();
      }

      if (g.boundingBox && isFinite(g.boundingBox.min.x) && isFinite(g.boundingBox.max.x)) {
        _tmpBox.copy(g.boundingBox);
        if (matrixIsFinite) {
          _tmpBox.applyMatrix4(worldMatrix);
        }
        _tmpBox.getBoundingSphere(_tmpSphere);

        if (isFinite(_tmpSphere.center.x) && isFinite(_tmpSphere.radius) && _tmpSphere.radius > 0) {
          useSphere = true;
          geometryDebug.boxValid++;
        } else {
          geometryDebug.boxInvalid++;
        }
      }
    }

    if (!useSphere && g.attributes?.position) {
      const position = g.attributes.position;
      const count = position.count;
      const maxSamples = 3000;
      const step = Math.max(1, Math.ceil(count / maxSamples));
      let foundFinitePoint = false;
      let minX = Infinity;
      let minY = Infinity;
      let minZ = Infinity;
      let maxX = -Infinity;
      let maxY = -Infinity;
      let maxZ = -Infinity;

      for (let i = 0; i < count; i += step) {
        geometryDebug.totalSamplesTried++;
        const x = position.getX(i);
        const y = position.getY(i);
        const z = position.itemSize >= 3 ? position.getZ(i) : 0;
        _tmpMatrixPos.set(x, y, z);
        if (matrixIsFinite) {
          _tmpMatrixPos.applyMatrix4(worldMatrix);
        }

        if (!isFinite(_tmpMatrixPos.x) || !isFinite(_tmpMatrixPos.y) || !isFinite(_tmpMatrixPos.z)) {
          geometryDebug.sampleInvalid++;
          continue;
        }

        geometryDebug.sampleFinite++;
        foundFinitePoint = true;
        minX = Math.min(minX, _tmpMatrixPos.x);
        minY = Math.min(minY, _tmpMatrixPos.y);
        minZ = Math.min(minZ, _tmpMatrixPos.z);
        maxX = Math.max(maxX, _tmpMatrixPos.x);
        maxY = Math.max(maxY, _tmpMatrixPos.y);
        maxZ = Math.max(maxZ, _tmpMatrixPos.z);

        hasAnyPointBounds = true;
        _tmpUnionMin.min(_tmpMatrixPos);
        _tmpUnionMax.max(_tmpMatrixPos);
      }

      if (foundFinitePoint) {
        _tmpBox.min.set(minX, minY, minZ);
        _tmpBox.max.set(maxX, maxY, maxZ);
        _tmpBox.getBoundingSphere(_tmpSphere);

        if (isFinite(_tmpSphere.center.x) && isFinite(_tmpSphere.radius) && _tmpSphere.radius > 0) {
          useSphere = true;
          geometryDebug.attrFallbackUsed++;
        } else {
          geometryDebug.attrFallbackInvalid++;
        }
      } else {
        geometryDebug.attrFallbackInvalid++;
      }
    }

    if (!useSphere) {
      return;
    }

    hasAnyMeshBounds = true;
    _tmpOffset.set(_tmpSphere.radius, _tmpSphere.radius, _tmpSphere.radius);
    _tmpUnionMin.min(_tmpCenter.copy(_tmpSphere.center).sub(_tmpOffset));
    _tmpUnionMax.max(_tmpCenter.copy(_tmpSphere.center).add(_tmpOffset));
  });

  if (!hasAnyMeshBounds && hasAnyPointBounds) {
    _tmpBox.min.copy(_tmpUnionMin);
    _tmpBox.max.copy(_tmpUnionMax);
    _tmpBox.getBoundingSphere(outSphere);

    if (!isFinite(outSphere.radius) || outSphere.radius <= 0) {
      _tmpBox.getCenter(outSphere.center);
      outSphere.radius = 1.0;
    }

    geometryDebug.pointFallbackUsed = 1;
    return true;
  }

  if (!hasAnyMeshBounds) return false;

  _tmpBox.min.copy(_tmpUnionMin);
  _tmpBox.max.copy(_tmpUnionMax);
  _tmpBox.getBoundingSphere(outSphere);

  if (!isFinite(outSphere.radius) || outSphere.radius <= 0) {
    _tmpBox.getCenter(outSphere.center);
    outSphere.radius = 1.0;
  }

  return true;
}

function getMaterialList(materialOrList) {
  return Array.isArray(materialOrList) ? materialOrList : [materialOrList];
}

function disposeMaterialList(materialOrList) {
  for (const material of getMaterialList(materialOrList)) {
    material?.dispose?.();
  }
}

function createDebugMaterial(sourceMaterial) {
  const side = debugState.doubleSided ? THREE.DoubleSide : sourceMaterial?.side ?? THREE.FrontSide;

  if (debugState.mode === "wireframe") {
    return new THREE.MeshBasicMaterial({
      color: 0x7de3ff,
      wireframe: true,
      side,
    });
  }

  if (debugState.mode === "normals") {
    return new THREE.MeshNormalMaterial({ side });
  }

  // Unlit but still uses base map/color so albedo can be inspected without lighting.
  return new THREE.MeshBasicMaterial({
    color: sourceMaterial?.color ? sourceMaterial.color.clone() : new THREE.Color(0xdddddd),
    map: sourceMaterial?.map ?? null,
    vertexColors: !!sourceMaterial?.vertexColors,
    side,
  });
}

function applyOriginalMaterialSide(material) {
  if (!material) return;

  if (debugState.doubleSided) {
    if (!originalMaterialSides.has(material)) {
      originalMaterialSides.set(material, material.side);
    }
    material.side = THREE.DoubleSide;
    material.needsUpdate = true;
  } else if (originalMaterialSides.has(material)) {
    material.side = originalMaterialSides.get(material);
    originalMaterialSides.delete(material);
    material.needsUpdate = true;
  }
}

function applyDebugMaterialToMesh(mesh) {
  if (!mesh?.isMesh || !mesh.material) return;

  const originalMaterial = meshOriginalMaterials.get(mesh) ?? mesh.material;
  if (!meshOriginalMaterials.has(mesh)) {
    meshOriginalMaterials.set(mesh, originalMaterial);
  }

  const previousDebugMaterial = meshDebugMaterials.get(mesh);
  if (previousDebugMaterial) {
    disposeMaterialList(previousDebugMaterial);
    meshDebugMaterials.delete(mesh);
  }

  if (debugState.mode === "original") {
    mesh.material = originalMaterial;
    for (const material of getMaterialList(originalMaterial)) {
      applyOriginalMaterialSide(material);
    }
    return;
  }

  const originalMaterials = getMaterialList(originalMaterial);
  const debugMaterials = originalMaterials.map((material) => createDebugMaterial(material));

  mesh.material = Array.isArray(originalMaterial) ? debugMaterials : debugMaterials[0];
  meshDebugMaterials.set(mesh, debugMaterials);
}

function applyDebugModeToLoadedMeshes() {
  tiles.group.traverse((obj) => {
    if (!obj.isMesh || !obj.material) return;
    applyDebugMaterialToMesh(obj);
  });
}

function updateRootBoundsHelper() {
  if (rootBoundsHelper) {
    scene.remove(rootBoundsHelper);
    rootBoundsHelper.geometry?.dispose?.();
    rootBoundsHelper.material?.dispose?.();
    rootBoundsHelper = null;
  }

  if (!debugState.showBounds) return;

  const sphere = getTilesetRootSphere(tiles);
  if (!sphere || !isFinite(sphere.radius) || sphere.radius <= 0) return;

  rootBoundsHelper = new THREE.Mesh(
    new THREE.SphereGeometry(sphere.radius, 24, 16),
    new THREE.MeshBasicMaterial({
      color: 0xffcc33,
      wireframe: true,
      transparent: true,
      opacity: 0.8,
      depthTest: false,
    })
  );
  rootBoundsHelper.position.copy(sphere.center);
  scene.add(rootBoundsHelper);
}

window.addEventListener("keydown", (event) => {
  if (event.repeat) return;
  if (event.target && (event.target.tagName === "INPUT" || event.target.tagName === "TEXTAREA")) {
    return;
  }

  switch (event.code) {
    case "Digit1":
      debugState.mode = "original";
      applyDebugModeToLoadedMeshes();
      break;
    case "Digit2":
      debugState.mode = "wireframe";
      applyDebugModeToLoadedMeshes();
      break;
    case "Digit3":
      debugState.mode = "normals";
      applyDebugModeToLoadedMeshes();
      break;
    case "Digit4":
      debugState.mode = "unlit";
      applyDebugModeToLoadedMeshes();
      break;
    case "KeyD":
      debugState.doubleSided = !debugState.doubleSided;
      applyDebugModeToLoadedMeshes();
      break;
    case "KeyB":
      debugState.showBounds = !debugState.showBounds;
      updateRootBoundsHelper();
      break;
    case "KeyF":
      needsGeometryRefit = true;
      console.log(`F_DEBUG ${lastDebugCompactLine || "no-metrics-yet"}`);
      console.log("F_DEBUG_FRAME", lastFrameStats || {});
      {
        const snapshot = collectGeometrySnapshot(12);
        console.log("F_DEBUG_GEOM", snapshot);
      }
      break;
    default:
      return;
  }
});

// ----------------------------------------------------
// Animation loop
// ----------------------------------------------------
function animate() {
  requestAnimationFrame(animate);

 // frameToGeometry();
  const delta = clock.getDelta();

  controls.update(delta);
  camera.updateMatrixWorld(true);
  tiles.update();

  renderer.render(scene, camera);


  let meshCount = 0;
  let triCount = 0;
  let hiddenMeshCount = 0;
  let blackMaterialCount = 0;

  tiles.group.traverse((o) => {
    if (o.isMesh) {
      meshCount++;

      if (!o.visible) {
        hiddenMeshCount++;
      }

      const g = o.geometry;
      if (g && g.index) triCount += g.index.count / 3;
      else if (g && g.attributes?.position) triCount += g.attributes.position.count / 3;

      if (debugState.mode === "original" && o.material) {
        for (const material of getMaterialList(o.material)) {
          if (!material || !material.color) continue;
          const colorMagnitude = material.color.r + material.color.g + material.color.b;
          const hasColorMap = !!material.map;
          if (!hasColorMap && colorMagnitude < 0.12) {
            blackMaterialCount++;
          }
        }
      }
    }
  });

  const hasGeometrySphere = estimateLoadedGeometrySphere(_tmpWorldSphere);

  if (hasGeometrySphere && (!didFrameFromGeometry || needsGeometryRefit)) {
    const framed = frameFromSphere(_tmpWorldSphere);
    if (framed) {
      didFrameFromGeometry = true;
      needsGeometryRefit = false;
    }
  }

  const cameraToGeom = hasGeometrySphere
    ? camera.position.distanceTo(_tmpWorldSphere.center)
    : NaN;
  const geomRadius = hasGeometrySphere ? _tmpWorldSphere.radius : NaN;

  const debugCompact =
    `m=${geometryDebug.meshSeen}/${geometryDebug.meshWithGeometry}/${geometryDebug.meshWithPosition} ` +
    `mw=${geometryDebug.matrixValid}-${geometryDebug.matrixInvalid} ` +
    `s=${geometryDebug.sphereValid}-${geometryDebug.sphereInvalid} ` +
    `b=${geometryDebug.boxValid}-${geometryDebug.boxInvalid} ` +
    `a=${geometryDebug.attrFallbackUsed}-${geometryDebug.attrFallbackInvalid} ` +
    `p=${geometryDebug.sampleFinite}-${geometryDebug.sampleInvalid} ` +
    `pt=${geometryDebug.totalPositionCount}/${geometryDebug.totalSamplesTried} ` +
    `u=${visibleLeafTiles.size}/${visibleProxyTiles.size} ` +
    `pf=${geometryDebug.pointFallbackUsed}`;
  lastDebugCompactLine =
    `geomRadius=${isFinite(geomRadius) ? geomRadius.toFixed(2) : "n/a"} ` +
    `camDist=${isFinite(cameraToGeom) ? cameraToGeom.toFixed(2) : "n/a"} ` +
    debugCompact;
  lastFrameStats = {
    geomRadius,
    cameraToGeom,
    debugCompact,
  };

  const loadingLine = hasLoadedAnyTile ? "Tiles loaded" : "Tiles are loading...";
  hud.textContent =
    `${loadingLine}\n` +
    `Meshes: ${meshCount} | Triangles: ~${Math.floor(triCount)}\n` +
    `Leaf tiles: ${visibleLeafTiles.size} | Proxy tiles: ${visibleProxyTiles.size}`;

  //updateGeometryBounds();
}

animate();

// ----------------------------------------------------
// Resize handling
// ----------------------------------------------------
window.addEventListener("resize", () => {
  const w = window.innerWidth;
  const h = window.innerHeight;

  camera.aspect = w / h;
  camera.updateProjectionMatrix();

  renderer.setSize(w, h);
  tiles.setResolutionFromRenderer(camera, renderer);

});
