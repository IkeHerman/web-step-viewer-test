#!/usr/bin/env python3
import argparse
import json
import subprocess
from pathlib import Path


def run_fixture(model2tile_bin: Path, repo_root: Path, fixture: dict, out_root: Path):
    fixture_id = fixture["id"]
    fixture_path = repo_root / fixture["path"]
    artifact_dir = out_root / fixture_id / "artifacts"
    out_dir = out_root / fixture_id / "tiles"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)

    cmd = [
        str(model2tile_bin),
        "--fidelity-artifacts-dir",
        str(artifact_dir),
        "--out-dir",
        str(out_dir),
        "--content-subdir",
        "tiles",
        "--disable-glbopt",
        "--viewer-target-sse",
        "80",
        "--discard-glb",
        str(fixture_path),
    ]
    proc = subprocess.run(cmd, cwd=repo_root, capture_output=True, text=True)
    return proc, artifact_dir


def read_json(path: Path):
    if not path.exists():
        return {}
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def read_jsonl(path: Path):
    if not path.exists():
        return []
    rows = []
    with path.open("r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rows.append(json.loads(line))
    return rows


def assert_fixture(fixture: dict, import_ev: dict, scene_ev: dict, export_rows: list):
    a = fixture.get("assertions", {})
    checks = [
        ("minOccurrences", import_ev.get("occurrences", 0)),
        ("minShapeColorOccurrences", import_ev.get("shapeColorOccurrences", 0)),
        ("minFaceColorEntries", import_ev.get("faceColorEntries", 0)),
        ("minMetadataOccurrences", import_ev.get("metadataOccurrences", 0)),
    ]
    failures = []
    for key, actual in checks:
        expected = int(a.get(key, 0))
        if actual < expected:
            failures.append(f"{key}: expected >= {expected}, got {actual}")
    # SceneIR preservation guard
    if scene_ev:
        if scene_ev.get("faceColorEntries", 0) < import_ev.get("faceColorEntries", 0):
            failures.append("SceneIR faceColorEntries dropped from import evidence")
        if scene_ev.get("metadataOccurrences", 0) < import_ev.get("metadataOccurrences", 0):
            failures.append("SceneIR metadataOccurrences dropped from import evidence")
    for row in export_rows:
        if row.get("status") != "ok":
            failures.append("export evidence contains non-ok status")
            break
        if "chosenSSE" not in row or "linearDeflection" not in row or "angularDeflectionDeg" not in row:
            failures.append("export evidence missing SSE tessellation fields")
            break
    return failures


def main():
    parser = argparse.ArgumentParser(description="Run STEP fidelity regression fixtures.")
    parser.add_argument("--contract", default="Model2Tile/fidelity/fixtures_contract.json")
    parser.add_argument("--model2tile-bin", default="Model2Tile/model2tile")
    parser.add_argument("--out-root", default="Model2Tile/out/fidelity")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[2]
    contract = read_json(repo_root / args.contract)
    fixtures = contract.get("fixtures", [])
    model2tile_bin = (repo_root / args.model2tile_bin).resolve()
    out_root = (repo_root / args.out_root).resolve()
    out_root.mkdir(parents=True, exist_ok=True)

    summary = {"passed": [], "failed": []}
    for fixture in fixtures:
        proc, artifact_dir = run_fixture(model2tile_bin, repo_root, fixture, out_root)
        if proc.returncode != 0:
            summary["failed"].append({
                "fixture": fixture["id"],
                "reason": "pipeline failed",
                "stderr": proc.stderr[-2000:],
            })
            continue
        import_ev = read_json(artifact_dir / "import_evidence.json")
        scene_ev = read_json(artifact_dir / "scene_ir_evidence.json")
        export_rows = read_jsonl(artifact_dir / "export_evidence.jsonl")
        failures = assert_fixture(fixture, import_ev, scene_ev, export_rows)
        if failures:
            summary["failed"].append({"fixture": fixture["id"], "reason": "; ".join(failures)})
        else:
            summary["passed"].append(fixture["id"])

    summary_path = out_root / "summary.json"
    with summary_path.open("w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)
    print(json.dumps(summary, indent=2))
    raise SystemExit(1 if summary["failed"] else 0)


if __name__ == "__main__":
    main()
