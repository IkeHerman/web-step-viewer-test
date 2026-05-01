#if __has_include(<assimp/Importer.hpp>)
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#define MODEL2TILE_HAS_ASSIMP 1
#else
#define MODEL2TILE_HAS_ASSIMP 0
#endif

#include "../dep/tinygltf/tiny_gltf.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
struct PositionKey
{
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;
    bool operator==(const PositionKey& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct PositionKeyHash
{
    std::size_t operator()(const PositionKey& k) const noexcept
    {
        std::size_t h = 1469598103934665603ull;
        auto mix = [&](std::size_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
        mix(static_cast<std::size_t>(k.x ^ (k.x >> 32)));
        mix(static_cast<std::size_t>(k.y ^ (k.y >> 32)));
        mix(static_cast<std::size_t>(k.z ^ (k.z >> 32)));
        return h;
    }
};

struct NormalKey
{
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    bool operator==(const NormalKey& o) const { return x == o.x && y == o.y && z == o.z; }
};

NormalKey MakeNormalKey(float nx, float ny, float nz)
{
    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len > 1e-12f)
    {
        nx /= len;
        ny /= len;
        nz /= len;
    }
    constexpr float q = 10000.0f;
    return {
        static_cast<std::int32_t>(std::lround(nx * q)),
        static_cast<std::int32_t>(std::lround(ny * q)),
        static_cast<std::int32_t>(std::lround(nz * q))
    };
}

PositionKey MakePositionKey(float x, float y, float z)
{
    constexpr double q = 1e6;
    return {
        static_cast<std::int64_t>(std::llround(static_cast<double>(x) * q)),
        static_cast<std::int64_t>(std::llround(static_cast<double>(y) * q)),
        static_cast<std::int64_t>(std::llround(static_cast<double>(z) * q))
    };
}

struct SmoothStats
{
    std::size_t vertices = 0;
    std::size_t uniquePositions = 0;
    std::size_t positionsWithMultipleNormals = 0;
    std::size_t maxNormalsPerPosition = 0;
    std::size_t cornerCount = 0;
    double meanCornerAngleDeg = 0.0;
    double p95CornerAngleDeg = 0.0;
    double maxCornerAngleDeg = 0.0;
};

void PrintStats(const std::string& label, const SmoothStats& s)
{
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "[" << label << "] vertices=" << s.vertices
              << " uniquePositions=" << s.uniquePositions
              << " positionsWithMultipleNormals=" << s.positionsWithMultipleNormals
              << " maxNormalsPerPosition=" << s.maxNormalsPerPosition
              << " cornerCount=" << s.cornerCount
              << " meanCornerAngleDeg=" << s.meanCornerAngleDeg
              << " p95CornerAngleDeg=" << s.p95CornerAngleDeg
              << " maxCornerAngleDeg=" << s.maxCornerAngleDeg
              << "\n";
}

struct Vec3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

double Dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

double Length(const Vec3& v)
{
    return std::sqrt(Dot(v, v));
}

Vec3 Normalize(const Vec3& v)
{
    const double len = Length(v);
    if (len <= 1e-18)
    {
        return {0.0, 0.0, 0.0};
    }
    return {v.x / len, v.y / len, v.z / len};
}

double AngleDeg(const Vec3& a, const Vec3& b)
{
    const Vec3 na = Normalize(a);
    const Vec3 nb = Normalize(b);
    const double d = std::clamp(Dot(na, nb), -1.0, 1.0);
    return std::acos(d) * (180.0 / M_PI);
}

void FinalizeCornerAngles(const std::vector<double>& angles, SmoothStats& out)
{
    if (angles.empty())
    {
        out.cornerCount = 0;
        out.meanCornerAngleDeg = 0.0;
        out.p95CornerAngleDeg = 0.0;
        out.maxCornerAngleDeg = 0.0;
        return;
    }

    out.cornerCount = angles.size();
    double sum = 0.0;
    out.maxCornerAngleDeg = 0.0;
    for (double a : angles)
    {
        sum += a;
        out.maxCornerAngleDeg = std::max(out.maxCornerAngleDeg, a);
    }
    out.meanCornerAngleDeg = sum / static_cast<double>(angles.size());

    std::vector<double> sorted = angles;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t p95Index = static_cast<std::size_t>(
        std::floor(0.95 * static_cast<double>(sorted.size() - 1)));
    out.p95CornerAngleDeg = sorted[p95Index];
}

#if MODEL2TILE_HAS_ASSIMP
bool AnalyzeFbx(const std::string& fbxPath, SmoothStats& out)
{
    Assimp::Importer importer;
    const unsigned int flags = aiProcess_Triangulate | aiProcess_ImproveCacheLocality |
        aiProcess_SortByPType | aiProcess_TransformUVCoords | aiProcess_FlipUVs;
    const aiScene* scene = importer.ReadFile(fbxPath, flags);
    if (scene == nullptr)
    {
        std::cerr << "failed to read fbx: " << importer.GetErrorString() << "\n";
        return false;
    }

    std::unordered_map<PositionKey, std::vector<NormalKey>, PositionKeyHash> map;
    std::vector<double> cornerAngles;
    for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi)
    {
        const aiMesh* mesh = scene->mMeshes[mi];
        if (!mesh || !mesh->HasNormals())
        {
            continue;
        }
        for (unsigned int v = 0; v < mesh->mNumVertices; ++v)
        {
            const aiVector3D& p = mesh->mVertices[v];
            const aiVector3D& n = mesh->mNormals[v];
            const PositionKey pk = MakePositionKey(p.x, p.y, p.z);
            const NormalKey nk = MakeNormalKey(n.x, n.y, n.z);
            auto& bucket = map[pk];
            if (std::find(bucket.begin(), bucket.end(), nk) == bucket.end())
            {
                bucket.push_back(nk);
            }
            ++out.vertices;
        }

        for (unsigned int fi = 0; fi < mesh->mNumFaces; ++fi)
        {
            const aiFace& f = mesh->mFaces[fi];
            if (f.mNumIndices != 3)
            {
                continue;
            }
            const unsigned int ia = f.mIndices[0];
            const unsigned int ib = f.mIndices[1];
            const unsigned int ic = f.mIndices[2];
            if (ia >= mesh->mNumVertices || ib >= mesh->mNumVertices || ic >= mesh->mNumVertices)
            {
                continue;
            }

            const aiVector3D& ap = mesh->mVertices[ia];
            const aiVector3D& bp = mesh->mVertices[ib];
            const aiVector3D& cp = mesh->mVertices[ic];

            const Vec3 e1{bp.x - ap.x, bp.y - ap.y, bp.z - ap.z};
            const Vec3 e2{cp.x - ap.x, cp.y - ap.y, cp.z - ap.z};
            const Vec3 fn = Normalize(Cross(e1, e2));
            if (Length(fn) <= 1e-18)
            {
                continue;
            }

            const aiVector3D& an = mesh->mNormals[ia];
            const aiVector3D& bn = mesh->mNormals[ib];
            const aiVector3D& cn = mesh->mNormals[ic];
            cornerAngles.push_back(AngleDeg(fn, Vec3{an.x, an.y, an.z}));
            cornerAngles.push_back(AngleDeg(fn, Vec3{bn.x, bn.y, bn.z}));
            cornerAngles.push_back(AngleDeg(fn, Vec3{cn.x, cn.y, cn.z}));
        }
    }

    out.uniquePositions = map.size();
    for (const auto& kv : map)
    {
        const std::size_t cnt = kv.second.size();
        if (cnt > 1)
        {
            ++out.positionsWithMultipleNormals;
        }
        out.maxNormalsPerPosition = std::max(out.maxNormalsPerPosition, cnt);
    }
    FinalizeCornerAngles(cornerAngles, out);
    return true;
}
#endif

bool AnalyzeGlb(const std::string& glbPath, SmoothStats& out)
{
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err;
    std::string warn;
    if (!loader.LoadBinaryFromFile(&model, &err, &warn, glbPath))
    {
        std::cerr << "failed to read glb: " << err << "\n";
        return false;
    }

    std::unordered_map<PositionKey, std::vector<NormalKey>, PositionKeyHash> map;
    std::vector<double> cornerAngles;
    for (const tinygltf::Mesh& mesh : model.meshes)
    {
        for (const tinygltf::Primitive& prim : mesh.primitives)
        {
            const auto pIt = prim.attributes.find("POSITION");
            const auto nIt = prim.attributes.find("NORMAL");
            if (pIt == prim.attributes.end() || nIt == prim.attributes.end())
            {
                continue;
            }
            if (prim.indices < 0 || prim.indices >= static_cast<int>(model.accessors.size()))
            {
                continue;
            }

            const tinygltf::Accessor& pAcc = model.accessors[pIt->second];
            const tinygltf::Accessor& nAcc = model.accessors[nIt->second];
            const tinygltf::Accessor& iAcc = model.accessors[prim.indices];
            if (pAcc.count != nAcc.count || pAcc.bufferView < 0 || nAcc.bufferView < 0)
            {
                continue;
            }
            if (iAcc.bufferView < 0 || iAcc.bufferView >= static_cast<int>(model.bufferViews.size()))
            {
                continue;
            }
            const tinygltf::BufferView& pView = model.bufferViews[pAcc.bufferView];
            const tinygltf::BufferView& nView = model.bufferViews[nAcc.bufferView];
            const tinygltf::BufferView& iView = model.bufferViews[iAcc.bufferView];
            const tinygltf::Buffer& pBuf = model.buffers[pView.buffer];
            const tinygltf::Buffer& nBuf = model.buffers[nView.buffer];
            const tinygltf::Buffer& iBuf = model.buffers[iView.buffer];
            const std::size_t pStride = pAcc.ByteStride(pView) ? pAcc.ByteStride(pView) : sizeof(float) * 3;
            const std::size_t nStride = nAcc.ByteStride(nView) ? nAcc.ByteStride(nView) : sizeof(float) * 3;
            const std::size_t iStride = iAcc.ByteStride(iView)
                ? iAcc.ByteStride(iView)
                : (iAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT ? sizeof(std::uint16_t)
                   : iAcc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ? sizeof(std::uint8_t)
                   : sizeof(std::uint32_t));
            const std::size_t pBase = pView.byteOffset + pAcc.byteOffset;
            const std::size_t nBase = nView.byteOffset + nAcc.byteOffset;
            const std::size_t iBase = iView.byteOffset + iAcc.byteOffset;

            for (std::size_t i = 0; i < pAcc.count; ++i)
            {
                const float* p = reinterpret_cast<const float*>(pBuf.data.data() + pBase + i * pStride);
                const float* n = reinterpret_cast<const float*>(nBuf.data.data() + nBase + i * nStride);
                const PositionKey pk = MakePositionKey(p[0], p[1], p[2]);
                const NormalKey nk = MakeNormalKey(n[0], n[1], n[2]);
                auto& bucket = map[pk];
                if (std::find(bucket.begin(), bucket.end(), nk) == bucket.end())
                {
                    bucket.push_back(nk);
                }
                ++out.vertices;
            }

            auto readIndex = [&](std::size_t i) -> std::uint32_t
            {
                const unsigned char* src = iBuf.data.data() + iBase + i * iStride;
                switch (iAcc.componentType)
                {
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                        return *reinterpret_cast<const std::uint8_t*>(src);
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                        return *reinterpret_cast<const std::uint16_t*>(src);
                    default:
                        return *reinterpret_cast<const std::uint32_t*>(src);
                }
            };

            const std::size_t triIndexCount = iAcc.count - (iAcc.count % 3u);
            for (std::size_t i = 0; i < triIndexCount; i += 3)
            {
                const std::uint32_t ia = readIndex(i + 0);
                const std::uint32_t ib = readIndex(i + 1);
                const std::uint32_t ic = readIndex(i + 2);
                if (ia >= pAcc.count || ib >= pAcc.count || ic >= pAcc.count)
                {
                    continue;
                }

                const float* ap = reinterpret_cast<const float*>(pBuf.data.data() + pBase + ia * pStride);
                const float* bp = reinterpret_cast<const float*>(pBuf.data.data() + pBase + ib * pStride);
                const float* cp = reinterpret_cast<const float*>(pBuf.data.data() + pBase + ic * pStride);
                const Vec3 e1{bp[0] - ap[0], bp[1] - ap[1], bp[2] - ap[2]};
                const Vec3 e2{cp[0] - ap[0], cp[1] - ap[1], cp[2] - ap[2]};
                const Vec3 fn = Normalize(Cross(e1, e2));
                if (Length(fn) <= 1e-18)
                {
                    continue;
                }

                const float* an = reinterpret_cast<const float*>(nBuf.data.data() + nBase + ia * nStride);
                const float* bn = reinterpret_cast<const float*>(nBuf.data.data() + nBase + ib * nStride);
                const float* cn = reinterpret_cast<const float*>(nBuf.data.data() + nBase + ic * nStride);
                cornerAngles.push_back(AngleDeg(fn, Vec3{an[0], an[1], an[2]}));
                cornerAngles.push_back(AngleDeg(fn, Vec3{bn[0], bn[1], bn[2]}));
                cornerAngles.push_back(AngleDeg(fn, Vec3{cn[0], cn[1], cn[2]}));
            }
        }
    }

    out.uniquePositions = map.size();
    for (const auto& kv : map)
    {
        const std::size_t cnt = kv.second.size();
        if (cnt > 1)
        {
            ++out.positionsWithMultipleNormals;
        }
        out.maxNormalsPerPosition = std::max(out.maxNormalsPerPosition, cnt);
    }
    FinalizeCornerAngles(cornerAngles, out);
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: fbx_normals_probe <source.fbx> <occ_high.glb>\n";
        return 2;
    }

#if !MODEL2TILE_HAS_ASSIMP
    std::cerr << "assimp headers unavailable\n";
    return 2;
#else
    SmoothStats fbxStats;
    SmoothStats glbStats;
    if (!AnalyzeFbx(argv[1], fbxStats))
    {
        return 1;
    }
    if (!AnalyzeGlb(argv[2], glbStats))
    {
        return 1;
    }

    PrintStats("FBX", fbxStats);
    PrintStats("GLB", glbStats);
    return 0;
#endif
}
