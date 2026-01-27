import * as THREE from "three";
import { FlyControls } from "three/examples/jsm/controls/FlyControls.js";
import { TilesRenderer } from "3d-tiles-renderer";

// ----------------------------------------------------
// CONFIG
// ----------------------------------------------------
const TILESET_URL = "/tileset.json";

// ----------------------------------------------------
// DOM
// ----------------------------------------------------
const app = document.getElementById("app");
const hud = document.getElementById("hud");

// ----------------------------------------------------
// Renderer
// ----------------------------------------------------
const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
renderer.setSize(window.innerWidth, window.innerHeight);
renderer.outputColorSpace = THREE.SRGBColorSpace;
app.appendChild(renderer.domElement);

// ----------------------------------------------------
// Scene
// ----------------------------------------------------
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x111111);

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

const key = new THREE.DirectionalLight(0xffffff, 1.2);
key.position.set(5, 8, 3);
scene.add(key);

const fill = new THREE.DirectionalLight(0xffffff, 0.6);
fill.position.set(-6, 3, -4);
scene.add(fill);

// ----------------------------------------------------
// 3D Tiles
// ----------------------------------------------------
const tiles = new TilesRenderer(TILESET_URL);

tiles.addEventListener?.("tile-load", (e) => {
  console.log("tile-load", e?.tile?.content?.uri || e?.tile?.contentUrl || e);
});

tiles.addEventListener?.("tile-error", (e) => {
  console.error("tile-error", e);
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
});

tiles.setCamera(camera);
tiles.setResolutionFromRenderer(camera, renderer);

tiles.errorTarget = 64;
tiles.maxDepth = Infinity;

// Ensure base path is correct
try {
  const u = new URL(TILESET_URL, window.location.href);
  tiles.setBasePath(u.href.substring(0, u.href.lastIndexOf("/") + 1));
} catch (_) {}

// Add tiles to scene
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

  tiles.group.traverse((o) => {
    if (o.isMesh) {
      meshCount++;

      const g = o.geometry;
      if (g && g.index) triCount += g.index.count / 3;
      else if (g && g.attributes?.position) triCount += g.attributes.position.count / 3;
    }
  });

  hud.textContent = "Loaded tileset — WASD/RF to fly, mouse drag to look \n" + `meshes=${meshCount} tris≈${Math.floor(triCount)}`;

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
