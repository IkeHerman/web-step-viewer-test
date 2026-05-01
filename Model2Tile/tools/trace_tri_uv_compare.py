#!/usr/bin/env python3
"""
Decode GLBs (BIN + JSON data: URI buffers) and compare one triangle’s UVs
between occ_*_high and a baked tile mesh (position-greedy corner matching).

Usage:
  python3 trace_tri_uv_compare.py occ_0_high.glb tile_0.glb [--tri N] [--eps 1e-4]
"""
from __future__ import annotations

import argparse
import base64
import json
import math
import pathlib
import struct
from typing import Any, Mapping, MutableMapping, Sequence, Tuple

Vec3 = Tuple[float, float, float]
Vec2 = Tuple[float, float]


def chunk_glb(buf: bytes) -> tuple[Mapping[str, Any], bytes]:
    o = 12
    j: MutableMapping[str, Any] | None = None
    bindata = b""
    while o < len(buf):
        cl = struct.unpack_from("<I", buf, o)[0]
        o += 4
        ct = struct.unpack_from("<I", buf, o)[0]
        o += 4
        ch = buf[o : o + cl]
        o += cl
        if ct == 0x4E4F534A:
            j = json.loads(ch)
        elif ct == 0x004E4942:
            bindata = ch
    assert j is not None
    return j, bindata


def decode_buffers(gl: Mapping[str, Any], bindata: bytes) -> list[bytes]:
    out = []
    c = 0
    for b in gl["buffers"]:
        uri = b.get("uri", "")
        blen = int(b["byteLength"])
        if uri.startswith("data:"):
            raw = base64.b64decode(uri.split(",", 1)[1])
        else:
            raw = bindata[c : c + blen]
            c += blen
        assert len(raw) == blen, (len(raw), blen)
        out.append(raw)
    return out


CSZ = {5126: 4, 5125: 4, 5123: 2}
COMPS = dict(SCALAR=1, VEC2=2, VEC3=3, VEC4=4)


def read_float_rows(gl: Mapping[str, Any], bufs: Sequence[bytes], ai: int) -> list[list[float]]:
    acc = gl["accessors"][ai]
    bv = gl["bufferViews"][acc["bufferView"]]
    buf = bufs[bv["buffer"]]
    base = int(bv.get("byteOffset", 0)) + int(acc.get("byteOffset", 0))
    stride = bv.get("byteStride")
    count = int(acc["count"])
    ct = acc["componentType"]
    comps = COMPS[acc["type"]]
    el = comps * CSZ[ct]

    assert ct == 5126

    st = stride if stride and stride != 0 else el
    out: list[list[float]] = []
    for i in range(count):
        row = []
        for k in range(comps):
            row.append(float(struct.unpack_from("<f", buf, base + i * st + k * 4)[0]))
        out.append(row)
    return out


def read_indices(gl: Mapping[str, Any], bufs: Sequence[bytes], ai: int) -> list[int]:
    acc = gl["accessors"][ai]
    bv = gl["bufferViews"][acc["bufferView"]]
    buf = bufs[bv["buffer"]]
    base = int(bv.get("byteOffset", 0)) + int(acc.get("byteOffset", 0))
    stride = bv.get("byteStride")
    count = int(acc["count"])
    ct = acc["componentType"]

    assert ct == 5125 or ct == 5123

    def rd(off: int) -> int:
        if ct == 5125:
            return int(struct.unpack_from("<I", buf, off)[0])
        return int(struct.unpack_from("<H", buf, off)[0])

    st = stride if stride and stride != 0 else CSZ[ct]
    return [rd(base + i * st) for i in range(count)]


def load_mesh(gl: Mapping[str, Any], bufs: Sequence[bytes]):
    prim = gl["meshes"][0]["primitives"][0]
    pos = read_float_rows(gl, bufs, prim["attributes"]["POSITION"])
    positions: list[Vec3] = [(r[0], r[1], r[2]) for r in pos]
    if "TEXCOORD_0" in prim["attributes"]:
        uv_rows = read_float_rows(gl, bufs, prim["attributes"]["TEXCOORD_0"])
        uvs = [(r[0], r[1]) for r in uv_rows]
    else:
        uvs = [(0.0, 0.0) for _ in positions]
    idx = read_indices(gl, bufs, prim["indices"])
    return positions, uvs, idx


def dsq(a: Vec3, b: Vec3) -> float:
    return sum((float(x) - float(y)) ** 2 for x, y in zip(a, b))


def tri_corners(pos: Sequence[Vec3], uv: Sequence[Vec2], ix: list[int], tri: int):
    i0 = int(ix[tri * 3])
    i1 = int(ix[tri * 3 + 1])
    i2 = int(ix[tri * 3 + 2])
    out = [(pos[j], uv[j], j) for j in (i0, i1, i2)]
    return i0, i1, i2, out


def pair_triangles(oc: Sequence[Tuple[Vec3, Vec2]], tg: Sequence[Tuple[Vec3, Vec2]], eps: float):
    """Greedy min-distance bipartite pairing (exact for clean matches)."""

    eps2 = eps * eps
    oc = list(oc)
    tg = list(tg)
    oc_use = [False] * 3
    tg_use = [False] * 3
    matched: list[Tuple[float, Tuple[Vec3, Vec2], Tuple[Vec3, Vec2], float]] = []

    for _step in range(3):
        best = (math.inf, -1, -1)
        for i in range(3):
            if oc_use[i]:
                continue
            for j in range(3):
                if tg_use[j]:
                    continue
                d = dsq(oc[i][0], tg[j][0])
                if d < best[0]:
                    best = (d, i, j)
        bd, bi, bj = best
        if bi < 0 or bd > eps2:
            return None
        oc_use[bi] = True
        tg_use[bj] = True
        _, uo = oc[bi]
        _, ut = tg[bj]
        duv = math.hypot(float(uo[0] - ut[0]), float(uo[1] - ut[1]))
        matched.append((float(math.sqrt(float(bd))), oc[bi], tg[bj], duv))

    return matched


def histogram_uv_delta(
    oc_pos: Sequence[Vec3],
    oc_uv: Sequence[Vec2],
    oc_ix: Sequence[int],
    tg_pos: Sequence[Vec3],
    tg_uv: Sequence[Vec2],
    tg_ix: Sequence[int],
    eps: float,
    bucket: float,
) -> None:
    """For every occ triangle whose position multiset exists in tgt, record max Δuv."""

    def pos_key_tri(tri_idx: int) -> tuple[tuple[float, float, float], tuple[float, float, float], tuple[float, float, float]]:
        _, _, _, corners = tri_corners(oc_pos, oc_uv, list(oc_ix), tri_idx)
        pts = tuple(sorted(tuple(round(x, 9) for x in c[0]) for c in corners))
        return pts  # type: ignore[return-value]

    oc_ntri = len(oc_ix) // 3

    tgt_map: dict[
        Tuple[Tuple[float, float, float], Tuple[float, float, float], Tuple[float, float, float]],
        int,
    ] = {}

    tg_ntri = len(tg_ix) // 3
    for t in range(tg_ntri):
        _, _, _, c = tri_corners(tg_pos, tg_uv, list(tg_ix), t)
        key = tuple(sorted(tuple(round(x, 9) for x in x_[0]) for x_ in c))
        tgt_map.setdefault(key, t)

    bins: dict[float, int] = {}
    first_bad: Tuple[int, list, list] | None = None

    for tri in range(oc_ntri):
        _, _, _, oc_corn = tri_corners(oc_pos, oc_uv, list(oc_ix), tri)
        oc_key = tuple(sorted(tuple(round(x, 9) for x in c[0]) for c in oc_corn))

        tt = tgt_map.get(oc_key)
        if tt is None:
            continue

        _, _, _, tg_corn = tri_corners(tg_pos, tg_uv, list(tg_ix), tt)
        pr = pair_triangles(
            [(c[0], c[1]) for c in oc_corn],
            [(c[0], c[1]) for c in tg_corn],
            eps,
        )
        if pr is None:
            continue
        max_duv = max(p[3] for p in pr)
        b = math.floor(max_duv / bucket) * bucket
        bins[b] = bins.get(b, 0) + 1
        if first_bad is None and max_duv > 1e-6:
            first_bad = (tri, list(pr), tt)

    print(f"\n[Δuv histogram] bucket={bucket:g} (triangles with position-key match in tile)")
    for k in sorted(bins):
        print(f"  Δuv in [{k:g}, {k+bucket:g}): {bins[k]}")

    if first_bad:
        tri, pr, tt = first_bad
        print(
            f"\n[first occ tri with |Δuv|>1e-6] occ_tri={tri} matched_tile_tri={tt}"
        )
        for i, (pd, o, t, duv) in enumerate(pr):
            print(
                f"  pair{i}: posDist={pd:.6g} Δuv={duv:.6g}\n"
                f"    occ pos={tuple(round(x,6) for x in o[0])} uv={tuple(round(x,8) for x in o[1])}\n"
                f"    tgt pos={tuple(round(x,6) for x in t[0])} uv={tuple(round(x,8) for x in t[1])}"
            )


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("occ_glb", type=pathlib.Path)
    ap.add_argument("tile_glb", type=pathlib.Path)
    ap.add_argument("--tri", type=int, default=0, help="occ triangle index to narrate")
    ap.add_argument("--eps", type=float, default=1e-4)
    ap.add_argument("--hist-bucket", type=float, default=1e-5)
    args = ap.parse_args()

    j0, b0 = chunk_glb(args.occ_glb.read_bytes())
    bu0 = decode_buffers(j0, b0)
    op, ou, oi = load_mesh(j0, bu0)

    j1, b1 = chunk_glb(args.tile_glb.read_bytes())
    bu1 = decode_buffers(j1, b1)
    tp, tu, ti = load_mesh(j1, bu1)

    print("occ:", args.occ_glb.name, "verts", len(op), "tris", len(oi) // 3)
    print("tile:", args.tile_glb.name, "verts", len(tp), "tris", len(ti) // 3)

    tri = args.tri
    if tri < 0 or tri >= len(oi) // 3:
        raise SystemExit("bad --tri")

    i0, i1, i2, occ_c = tri_corners(op, ou, oi, tri)
    print(f"\n=== Occ triangle {tri} (winding = index order) ===")
    print(f"  indices (i0,i1,i2) = ({i0}, {i1}, {i2})")
    for k, (p, u, vix) in enumerate(occ_c):
        print(
            f"  corner[{k}] vidx={vix} pos=({p[0]:.6f},{p[1]:.6f},{p[2]:.6f})"
            f" uv=({u[0]:.10f},{u[1]:.10f})"
        )

    print("\n=== Search tile for same three positions (ε={:g}) ===".format(args.eps))
    target_pts = [occ_c[k][0] for k in range(3)]
    found: list[int] = []
    for tt in range(len(ti) // 3):
        _, _, _, tc = tri_corners(tp, tu, ti, tt)
        tpts = [tc[k][0] for k in range(3)]
        ok = True
        for P in target_pts:
            if min(dsq(P, Q) for Q in tpts) > args.eps * args.eps:
                ok = False
                break
        if not ok:
            continue
        pr = pair_triangles(
            [(c[0], c[1]) for c in occ_c],
            [(c[0], c[1]) for c in tc],
            args.eps,
        )
        if pr is None:
            continue
        found.append(tt)
        print(f"  candidate tile tri {tt}: maxPosPairDist={max(p[0] for p in pr):.6g}")
        for i, (pd, o, t, duv) in enumerate(pr):
            print(
                f"    pair{i}: Δuv={duv:.6g} occUV={tuple(round(x,8) for x in o[1])}"
                f" tileUV={tuple(round(x,8) for x in t[1])}"
            )
        break

    if not found:
        print(
            "  No single tile triangle matched occ tri positions within ε "
            "(mesh may be merged with other instances or coordinates changed)."
        )

    histogram_uv_delta(op, ou, oi, tp, tu, ti, args.eps, args.hist_bucket)


if __name__ == "__main__":
    main()
