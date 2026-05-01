#include "fbx_traversal.h"

#if __has_include(<assimp/Importer.hpp>)
#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#define MODEL2TILE_HAS_ASSIMP 1
#else
#define MODEL2TILE_HAS_ASSIMP 0
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
#if MODEL2TILE_HAS_ASSIMP
core::Aabb InvalidAabb()
{
    return core::Aabb{};
}

void ExpandAabb(core::Aabb& box, const double x, const double y, const double z)
{
    if (!box.valid)
    {
        box.xmin = box.xmax = x;
        box.ymin = box.ymax = y;
        box.zmin = box.zmax = z;
        box.valid = true;
        return;
    }
    box.xmin = std::min(box.xmin, x);
    box.ymin = std::min(box.ymin, y);
    box.zmin = std::min(box.zmin, z);
    box.xmax = std::max(box.xmax, x);
    box.ymax = std::max(box.ymax, y);
    box.zmax = std::max(box.zmax, z);
}

core::Transform4d ToTransform(const aiMatrix4x4& matrix)
{
    core::Transform4d out;
    out.m[0] = matrix.a1; out.m[1] = matrix.a2; out.m[2] = matrix.a3; out.m[3] = matrix.a4;
    out.m[4] = matrix.b1; out.m[5] = matrix.b2; out.m[6] = matrix.b3; out.m[7] = matrix.b4;
    out.m[8] = matrix.c1; out.m[9] = matrix.c2; out.m[10] = matrix.c3; out.m[11] = matrix.c4;
    out.m[12] = matrix.d1; out.m[13] = matrix.d2; out.m[14] = matrix.d3; out.m[15] = matrix.d4;
    return out;
}

std::array<double, 3> TransformPoint(const aiMatrix4x4& matrix, const aiVector3D& point)
{
    const aiVector3D t = matrix * point;
    return {static_cast<double>(t.x), static_cast<double>(t.y), static_cast<double>(t.z)};
}

std::string BuildGeometryKey(const aiMesh& mesh, const core::Aabb& localBounds)
{
    std::ostringstream ss;
    ss << "mesh:"
       << mesh.mNumVertices << ":"
       << mesh.mNumFaces << ":"
       << mesh.mPrimitiveTypes << ":";
    if (localBounds.valid)
    {
        ss << std::round(localBounds.xmin * 100000.0) << ":"
           << std::round(localBounds.ymin * 100000.0) << ":"
           << std::round(localBounds.zmin * 100000.0) << ":"
           << std::round(localBounds.xmax * 100000.0) << ":"
           << std::round(localBounds.ymax * 100000.0) << ":"
           << std::round(localBounds.zmax * 100000.0);
    }
    else
    {
        ss << "void";
    }
    return ss.str();
}

std::string BuildMaterialKey(const aiMaterial* material, const unsigned int materialIndex)
{
    if (material == nullptr)
    {
        return "material:null";
    }

    aiColor4D color(0.7f, 0.7f, 0.7f, 1.0f);
    material->Get(AI_MATKEY_BASE_COLOR, color);
    std::ostringstream ss;
    ss << "material:" << materialIndex
       << ":rgba:"
       << std::round(color.r * 1000.0f) << ":"
       << std::round(color.g * 1000.0f) << ":"
       << std::round(color.b * 1000.0f) << ":"
       << std::round(color.a * 1000.0f);

    aiString texturePath;
    if (AI_SUCCESS == material->GetTexture(aiTextureType_BASE_COLOR, 0, &texturePath) ||
        AI_SUCCESS == material->GetTexture(aiTextureType_DIFFUSE, 0, &texturePath))
    {
        ss << ":tex:" << texturePath.C_Str();
    }
    return ss.str();
}

std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& filePath)
{
    std::ifstream in(filePath, std::ios::binary);
    if (!in)
    {
        return {};
    }
    in.seekg(0, std::ios::end);
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size <= 0)
    {
        return {};
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(out.data()), size);
    if (!in)
    {
        return {};
    }
    return out;
}

std::string GuessMimeType(const std::string& texturePath)
{
    const std::filesystem::path path(texturePath);
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".jpg" || ext == ".jpeg")
    {
        return "image/jpeg";
    }
    if (ext == ".png")
    {
        return "image/png";
    }
    if (ext == ".webp")
    {
        return "image/webp";
    }
    if (ext == ".bmp")
    {
        return "image/bmp";
    }
    return "application/octet-stream";
}

std::filesystem::path ResolveTexturePath(const std::filesystem::path& sourceFile, const std::string& texturePath)
{
    std::string normalized = texturePath;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::filesystem::path path(normalized);
    if (path.is_absolute() && std::filesystem::exists(path))
    {
        return path;
    }
    const std::filesystem::path relative = sourceFile.parent_path() / path;
    if (std::filesystem::exists(relative))
    {
        return relative;
    }
    return {};
}

void ExtractTexturePayload(
    const aiScene& scene,
    const std::filesystem::path& sourceFile,
    const aiMaterial* material,
    importers::FbxMeshPayload& outPayload)
{
    if (material == nullptr)
    {
        return;
    }
    aiString texturePath;
    if (AI_SUCCESS != material->GetTexture(aiTextureType_BASE_COLOR, 0, &texturePath) &&
        AI_SUCCESS != material->GetTexture(aiTextureType_DIFFUSE, 0, &texturePath))
    {
        return;
    }

    const std::string textureRef = texturePath.C_Str();
    if (textureRef.empty())
    {
        return;
    }

    if (const aiTexture* embedded = scene.GetEmbeddedTexture(textureRef.c_str()))
    {
        if (embedded->mHeight == 0 && embedded->pcData != nullptr && embedded->mWidth > 0)
        {
            const std::uint8_t* begin = reinterpret_cast<const std::uint8_t*>(embedded->pcData);
            outPayload.baseColorTextureBytes.assign(begin, begin + embedded->mWidth);
            const std::string hint = embedded->achFormatHint;
            if (!hint.empty())
            {
                outPayload.baseColorTextureMimeType = GuessMimeType(std::string("x.") + hint);
            }
            else
            {
                outPayload.baseColorTextureMimeType = "application/octet-stream";
            }
        }
        return;
    }

    const std::filesystem::path resolvedPath = ResolveTexturePath(sourceFile, textureRef);
    if (resolvedPath.empty())
    {
        return;
    }

    std::vector<std::uint8_t> bytes = ReadFileBytes(resolvedPath);
    if (bytes.empty())
    {
        return;
    }
    outPayload.baseColorTextureBytes = std::move(bytes);
    outPayload.baseColorTextureMimeType = GuessMimeType(resolvedPath.string());
}

/// UV channel referenced by base color map (usually same as Blender/FBX "UVSet" binding).
unsigned int ResolveAlbedoUvChannel(const aiMaterial* material, const aiMesh* mesh)
{
    constexpr unsigned int kFallback = 0u;
    if (material == nullptr || mesh == nullptr)
    {
        return kFallback;
    }

    int uvwSrc = static_cast<int>(kFallback);
    aiReturn r = material->Get(AI_MATKEY_UVWSRC(aiTextureType_BASE_COLOR, 0), uvwSrc);
    if (r != AI_SUCCESS)
    {
        r = material->Get(AI_MATKEY_UVWSRC(aiTextureType_DIFFUSE, 0), uvwSrc);
    }
    if (r != AI_SUCCESS)
    {
        return kFallback;
    }

    if (uvwSrc < 0 || uvwSrc >= AI_MAX_NUMBER_OF_TEXTURECOORDS)
    {
        return kFallback;
    }

    const unsigned int ch = static_cast<unsigned int>(uvwSrc);
    return mesh->HasTextureCoords(ch) ? ch : kFallback;
}

void CountMeshReferences(
    const aiNode* node,
    std::unordered_map<unsigned int, std::size_t>& outCounts)
{
    for (unsigned int i = 0; i < node->mNumMeshes; ++i)
    {
        ++outCounts[node->mMeshes[i]];
    }
    for (unsigned int i = 0; i < node->mNumChildren; ++i)
    {
        CountMeshReferences(node->mChildren[i], outCounts);
    }
}

void TraverseNode(
    const aiScene& scene,
    const std::filesystem::path& sourceFile,
    const aiNode* node,
    const aiMatrix4x4& parentTransform,
    const std::unordered_map<unsigned int, std::size_t>& meshUseCount,
    const std::string& parentPath,
    std::vector<importers::FbxOccurrence>& outOccurrences,
    core::Aabb& outWorldBounds)
{
    const std::string nodeName = node->mName.C_Str();
    const std::string nodePath = parentPath.empty() ? nodeName : (parentPath + "/" + nodeName);
    const aiMatrix4x4 worldTransform = parentTransform * node->mTransformation;

    for (unsigned int meshSlot = 0; meshSlot < node->mNumMeshes; ++meshSlot)
    {
        const unsigned int meshIndex = node->mMeshes[meshSlot];
        if (meshIndex >= scene.mNumMeshes)
        {
            continue;
        }
        const aiMesh* mesh = scene.mMeshes[meshIndex];
        if (mesh == nullptr || mesh->mNumVertices == 0)
        {
            continue;
        }

        importers::FbxOccurrence occ;
        occ.sourceLabel = nodePath + "#mesh_" + std::to_string(meshSlot);
        occ.worldTransform = ToTransform(worldTransform);
        occ.localBounds = InvalidAabb();
        occ.worldBounds = InvalidAabb();
        occ.triangleCount = 0;

        const aiMaterial* material = nullptr;
        if (mesh->mMaterialIndex < scene.mNumMaterials)
        {
            material = scene.mMaterials[mesh->mMaterialIndex];
        }
        const unsigned int uvChannel = ResolveAlbedoUvChannel(material, mesh);

        occ.meshPayload.positions.reserve(static_cast<std::size_t>(mesh->mNumVertices) * 3);
        if (mesh->HasNormals())
        {
            occ.meshPayload.normals.reserve(static_cast<std::size_t>(mesh->mNumVertices) * 3);
        }
        if (mesh->HasTextureCoords(uvChannel))
        {
            occ.meshPayload.texcoords0.reserve(static_cast<std::size_t>(mesh->mNumVertices) * 2);
        }
        for (unsigned int v = 0; v < mesh->mNumVertices; ++v)
        {
            const aiVector3D& p = mesh->mVertices[v];
            occ.meshPayload.positions.push_back(p.x);
            occ.meshPayload.positions.push_back(p.y);
            occ.meshPayload.positions.push_back(p.z);
            if (mesh->HasNormals())
            {
                const aiVector3D& n = mesh->mNormals[v];
                occ.meshPayload.normals.push_back(n.x);
                occ.meshPayload.normals.push_back(n.y);
                occ.meshPayload.normals.push_back(n.z);
            }
            if (mesh->HasTextureCoords(uvChannel))
            {
                const aiVector3D& uv = mesh->mTextureCoords[uvChannel][v];
                occ.meshPayload.texcoords0.push_back(uv.x);
                occ.meshPayload.texcoords0.push_back(uv.y);
            }
            ExpandAabb(occ.localBounds, p.x, p.y, p.z);

            const std::array<double, 3> wp = TransformPoint(worldTransform, p);
            ExpandAabb(occ.worldBounds, wp[0], wp[1], wp[2]);
            ExpandAabb(outWorldBounds, wp[0], wp[1], wp[2]);
        }

        occ.meshPayload.indices.reserve(static_cast<std::size_t>(mesh->mNumFaces) * 3);
        for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex)
        {
            const aiFace& face = mesh->mFaces[faceIndex];
            if (face.mNumIndices != 3)
            {
                continue;
            }
            if (face.mIndices[0] >= mesh->mNumVertices ||
                face.mIndices[1] >= mesh->mNumVertices ||
                face.mIndices[2] >= mesh->mNumVertices)
            {
                continue;
            }
            occ.meshPayload.indices.push_back(face.mIndices[0]);
            occ.meshPayload.indices.push_back(face.mIndices[1]);
            occ.meshPayload.indices.push_back(face.mIndices[2]);
            ++occ.triangleCount;
        }

        if (material)
        {
            aiColor4D color(0.7f, 0.7f, 0.7f, 1.0f);
            if (AI_SUCCESS == material->Get(AI_MATKEY_BASE_COLOR, color))
            {
                occ.meshPayload.baseColor = {color.r, color.g, color.b, color.a};
            }
            else if (AI_SUCCESS == material->Get(AI_MATKEY_COLOR_DIFFUSE, color))
            {
                occ.meshPayload.baseColor = {color.r, color.g, color.b, color.a};
            }
        }
        ExtractTexturePayload(scene, sourceFile, material, occ.meshPayload);

        occ.geometryKey = BuildGeometryKey(*mesh, occ.localBounds);
        occ.materialKey = BuildMaterialKey(material, mesh->mMaterialIndex);
        occ.qualifiedPrototypeKey = occ.geometryKey + "|mat:" + occ.materialKey;
        const auto it = meshUseCount.find(meshIndex);
        occ.fromExplicitReference = (it != meshUseCount.end() && it->second > 1);

        if (!occ.meshPayload.indices.empty())
        {
            outOccurrences.push_back(std::move(occ));
        }
    }

    for (unsigned int i = 0; i < node->mNumChildren; ++i)
    {
        TraverseNode(scene, sourceFile, node->mChildren[i], worldTransform, meshUseCount, nodePath, outOccurrences, outWorldBounds);
    }
}
#endif
} // namespace

namespace importers
{
bool CollectFbxOccurrences(
    const std::filesystem::path& filePath,
    std::vector<FbxOccurrence>& outOccurrences,
    core::Aabb& outWorldBounds,
    const bool verbose)
{
#if !MODEL2TILE_HAS_ASSIMP
    (void)filePath;
    (void)outOccurrences;
    (void)outWorldBounds;
    (void)verbose;
    std::cerr << "[FbxTraversal] assimp headers are unavailable. Install assimp and rebuild.\n";
    return false;
#else
    Assimp::Importer importer;
    // TransformUVCoords: bake FBX tiling/rotation/offset (AI_MATKEY_UVTRANSFORM) into vertex TEXCOORD.
    // FlipUVs: map Assimp/OpenGL-ish V to upper-left-origin UV space used by WebGL/glTF sampling.
    const unsigned int flags = aiProcess_Triangulate | aiProcess_ImproveCacheLocality |
        aiProcess_SortByPType | aiProcess_TransformUVCoords | aiProcess_FlipUVs;
    const aiScene* scene = importer.ReadFile(filePath.string(), flags);
    if (scene == nullptr || scene->mRootNode == nullptr)
    {
        std::cerr << "[FbxTraversal] failed to read: " << filePath << "\n";
        std::cerr << "[FbxTraversal] assimp error: " << importer.GetErrorString() << "\n";
        return false;
    }

    outOccurrences.clear();
    outWorldBounds = InvalidAabb();
    std::unordered_map<unsigned int, std::size_t> meshUseCount;
    CountMeshReferences(scene->mRootNode, meshUseCount);
    TraverseNode(*scene, filePath, scene->mRootNode, aiMatrix4x4(), meshUseCount, "", outOccurrences, outWorldBounds);

    if (verbose)
    {
        std::cout << "[FbxTraversal] occurrences=" << outOccurrences.size() << "\n";
    }
    return true;
#endif
}
} // namespace importers
