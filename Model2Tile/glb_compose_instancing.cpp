#include "glb_compose_instancing.h"

#include "dep/tinygltf/tiny_gltf.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
void RowMajorToGltfColumnMajor(const core::Transform4d& src, std::vector<double>& dst16)
{
    dst16.resize(16);
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            dst16[static_cast<std::size_t>(col * 4 + row)] =
                src.m[static_cast<std::size_t>(row * 4 + col)];
        }
    }
}

void MergeUnique(std::vector<std::string>& dest, const std::vector<std::string>& add)
{
    for (const std::string& s : add)
    {
        if (std::find(dest.begin(), dest.end(), s) == dest.end())
        {
            dest.push_back(s);
        }
    }
}

int FindPrimaryMeshIndex(const tinygltf::Model& model)
{
    if (model.scenes.empty() || model.nodes.empty())
    {
        return -1;
    }
    int sceneIndex = model.defaultScene;
    if (sceneIndex < 0 || sceneIndex >= static_cast<int>(model.scenes.size()))
    {
        sceneIndex = 0;
    }
    const tinygltf::Scene& sc = model.scenes[static_cast<std::size_t>(sceneIndex)];

    const std::function<int(int)> visit = [&](const int nodeIndex) -> int
    {
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size()))
        {
            return -1;
        }
        const tinygltf::Node& n = model.nodes[static_cast<std::size_t>(nodeIndex)];
        if (n.mesh >= 0)
        {
            return n.mesh;
        }
        for (const int child : n.children)
        {
            const int r = visit(child);
            if (r >= 0)
            {
                return r;
            }
        }
        return -1;
    };

    for (const int root : sc.nodes)
    {
        const int r = visit(root);
        if (r >= 0)
        {
            return r;
        }
    }
    return -1;
}

void BumpTextureInfo(tinygltf::TextureInfo& ti, const int delta)
{
    if (ti.index >= 0)
    {
        ti.index += delta;
    }
}

int ComponentCountForType(int type)
{
    switch (type)
    {
        case TINYGLTF_TYPE_SCALAR: return 1;
        case TINYGLTF_TYPE_VEC2: return 2;
        case TINYGLTF_TYPE_VEC3: return 3;
        case TINYGLTF_TYPE_VEC4: return 4;
        default: return 0;
    }
}

std::size_t ComponentSizeInBytes(int componentType)
{
    switch (componentType)
    {
        case TINYGLTF_COMPONENT_TYPE_BYTE: return 1;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: return 1;
        case TINYGLTF_COMPONENT_TYPE_SHORT: return 2;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return 2;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: return 4;
        case TINYGLTF_COMPONENT_TYPE_FLOAT: return 4;
        default: return 0;
    }
}

unsigned char* GetMutableAccessorPtr(
    tinygltf::Model& model,
    tinygltf::Accessor& accessor,
    std::size_t& outStride)
{
    if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(model.bufferViews.size()))
    {
        return nullptr;
    }

    tinygltf::BufferView& view = model.bufferViews[static_cast<std::size_t>(accessor.bufferView)];
    if (view.buffer < 0 || view.buffer >= static_cast<int>(model.buffers.size()))
    {
        return nullptr;
    }

    tinygltf::Buffer& buffer = model.buffers[static_cast<std::size_t>(view.buffer)];

    const int componentCount = ComponentCountForType(accessor.type);
    const std::size_t componentSize = ComponentSizeInBytes(accessor.componentType);
    if (componentCount <= 0 || componentSize == 0)
    {
        return nullptr;
    }
    const std::size_t packedSize = static_cast<std::size_t>(componentCount) * componentSize;
    if (packedSize == 0)
    {
        return nullptr;
    }

    outStride = accessor.ByteStride(view);
    if (outStride == 0)
    {
        outStride = packedSize;
    }
    if (outStride < packedSize)
    {
        return nullptr;
    }

    const std::size_t offset = static_cast<std::size_t>(view.byteOffset) + static_cast<std::size_t>(accessor.byteOffset);
    if (offset >= buffer.data.size())
    {
        return nullptr;
    }

    const std::size_t count = static_cast<std::size_t>(accessor.count);
    if (count > 0)
    {
        const std::size_t maxBeforeMul = std::numeric_limits<std::size_t>::max() / outStride;
        if ((count - 1u) > maxBeforeMul)
        {
            return nullptr;
        }
        const std::size_t lastOffset = (count - 1u) * outStride;
        if (lastOffset > (std::numeric_limits<std::size_t>::max() - packedSize))
        {
            return nullptr;
        }
        const std::size_t span = lastOffset + packedSize;
        if (span > (buffer.data.size() - offset))
        {
            return nullptr;
        }
    }

    return buffer.data.data() + offset;
}

bool Inverse3x3RowMajor(double invOut[9], double a, double b, double c, double d, double e, double f, double g, double h, double i)
{
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::abs(det) < 1e-30)
    {
        return false;
    }
    const double id = 1.0 / det;
    invOut[0] = (e * i - f * h) * id;
    invOut[1] = (c * h - b * i) * id;
    invOut[2] = (b * f - c * e) * id;
    invOut[3] = (f * g - d * i) * id;
    invOut[4] = (a * i - c * g) * id;
    invOut[5] = (c * d - a * f) * id;
    invOut[6] = (d * h - e * g) * id;
    invOut[7] = (b * g - a * h) * id;
    invOut[8] = (a * e - b * d) * id;
    return true;
}

void TransformPointWithColumnMajor4x4(const std::vector<double>& cm, float& x, float& y, float& z)
{
    const double px = static_cast<double>(x);
    const double py = static_cast<double>(y);
    const double pz = static_cast<double>(z);
    const double nx = cm[0] * px + cm[4] * py + cm[8] * pz + cm[12];
    const double ny = cm[1] * px + cm[5] * py + cm[9] * pz + cm[13];
    const double nz = cm[2] * px + cm[6] * py + cm[10] * pz + cm[14];
    const double nw = cm[3] * px + cm[7] * py + cm[11] * pz + cm[15];
    if (std::abs(nw - 1.0) > 1e-8 && std::abs(nw) > 1e-12)
    {
        const double invW = 1.0 / nw;
        x = static_cast<float>(nx * invW);
        y = static_cast<float>(ny * invW);
        z = static_cast<float>(nz * invW);
    }
    else
    {
        x = static_cast<float>(nx);
        y = static_cast<float>(ny);
        z = static_cast<float>(nz);
    }
}

void TransformDirectionWithColumnMajorLinear3x3(const std::vector<double>& cm, float& x, float& y, float& z)
{
    const double px = static_cast<double>(x);
    const double py = static_cast<double>(y);
    const double pz = static_cast<double>(z);
    const double nx = cm[0] * px + cm[4] * py + cm[8] * pz;
    const double ny = cm[1] * px + cm[5] * py + cm[9] * pz;
    const double nz = cm[2] * px + cm[6] * py + cm[10] * pz;
    x = static_cast<float>(nx);
    y = static_cast<float>(ny);
    z = static_cast<float>(nz);
}

void TransformNormalWithColumnMajor4x4(const std::vector<double>& cm, float& x, float& y, float& z)
{
    // Upper-left 3x3 of column-major 4x4: columns are (cm[0-2], cm[4-6], cm[8-10]).
    const double a = cm[0], b = cm[4], c = cm[8];
    const double d = cm[1], e = cm[5], f = cm[9];
    const double g = cm[2], h = cm[6], i = cm[10];
    double invR[9];
    if (!Inverse3x3RowMajor(invR, a, b, c, d, e, f, g, h, i))
    {
        return;
    }
    // n' = inv(R)^T * n; first row of inv(R)^T is first column of inv(R).
    const double nx = static_cast<double>(x);
    const double ny = static_cast<double>(y);
    const double nz = static_cast<double>(z);
    const double ox = invR[0] * nx + invR[3] * ny + invR[6] * nz;
    const double oy = invR[1] * nx + invR[4] * ny + invR[7] * nz;
    const double oz = invR[2] * nx + invR[5] * ny + invR[8] * nz;
    x = static_cast<float>(ox);
    y = static_cast<float>(oy);
    z = static_cast<float>(oz);
    const float len = std::sqrt(x * x + y * y + z * z);
    if (len > 1e-20f)
    {
        const float inv = 1.0f / len;
        x *= inv;
        y *= inv;
        z *= inv;
    }
}

bool BakeWorldTransformIntoMeshVertices(
    tinygltf::Model& model,
    int meshIndex,
    const core::Transform4d& worldRowMajor)
{
    if (meshIndex < 0 || meshIndex >= static_cast<int>(model.meshes.size()))
    {
        return false;
    }

    std::vector<double> cm;
    RowMajorToGltfColumnMajor(worldRowMajor, cm);

    tinygltf::Mesh& mesh = model.meshes[static_cast<std::size_t>(meshIndex)];
    for (tinygltf::Primitive& prim : mesh.primitives)
    {
        auto transformAttr = [&](const char* name, bool isPosition, bool isNormal, bool isTangent)
        {
            const auto it = prim.attributes.find(name);
            if (it == prim.attributes.end())
            {
                return;
            }
            const int accIndex = it->second;
            if (accIndex < 0 || accIndex >= static_cast<int>(model.accessors.size()))
            {
                return;
            }
            tinygltf::Accessor& accessor = model.accessors[static_cast<std::size_t>(accIndex)];
            if (accessor.sparse.isSparse || accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
            {
                std::cerr << "[glb_compose] skip attribute " << name << " (sparse or non-float)\n";
                return;
            }
            if (isPosition && (accessor.type != TINYGLTF_TYPE_VEC3))
            {
                return;
            }
            if (isNormal && (accessor.type != TINYGLTF_TYPE_VEC3))
            {
                return;
            }
            if (isTangent && (accessor.type != TINYGLTF_TYPE_VEC4))
            {
                return;
            }

            std::size_t stride = 0;
            unsigned char* base = GetMutableAccessorPtr(model, accessor, stride);
            if (!base)
            {
                return;
            }

            const std::size_t count = static_cast<std::size_t>(accessor.count);
            for (std::size_t vi = 0; vi < count; ++vi)
            {
                float* v = reinterpret_cast<float*>(base + vi * stride);
                if (isPosition)
                {
                    TransformPointWithColumnMajor4x4(cm, v[0], v[1], v[2]);
                }
                else if (isNormal)
                {
                    TransformNormalWithColumnMajor4x4(cm, v[0], v[1], v[2]);
                }
                else if (isTangent)
                {
                    TransformDirectionWithColumnMajorLinear3x3(cm, v[0], v[1], v[2]);
                    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                    if (len > 1e-20f)
                    {
                        const float inv = 1.0f / len;
                        v[0] *= inv;
                        v[1] *= inv;
                        v[2] *= inv;
                    }
                }
            }

            accessor.minValues.clear();
            accessor.maxValues.clear();
        };

        transformAttr("POSITION", true, false, false);
        transformAttr("NORMAL", false, true, false);
        transformAttr("TANGENT", false, false, true);
    }

    return true;
}

/// Appends all resources from `src` into `dest` and returns the dest mesh index for the primary
/// mesh used in the default scene of `src` (or -1).
bool AppendGltfModel(tinygltf::Model& dest, const tinygltf::Model& src, int& outPrimaryMeshIndex)
{
    outPrimaryMeshIndex = -1;
    const int srcPrimary = FindPrimaryMeshIndex(src);

    const std::size_t bufBase = dest.buffers.size();
    for (const tinygltf::Buffer& b : src.buffers)
    {
        dest.buffers.push_back(b);
    }

    const std::size_t bvBase = dest.bufferViews.size();
    for (tinygltf::BufferView bv : src.bufferViews)
    {
        if (bv.buffer >= 0)
        {
            bv.buffer += static_cast<int>(bufBase);
        }
        dest.bufferViews.push_back(std::move(bv));
    }

    const std::size_t accBase = dest.accessors.size();
    for (tinygltf::Accessor a : src.accessors)
    {
        if (a.bufferView >= 0)
        {
            a.bufferView += static_cast<int>(bvBase);
        }
        dest.accessors.push_back(std::move(a));
    }

    const std::size_t sampBase = dest.samplers.size();
    for (const tinygltf::Sampler& s : src.samplers)
    {
        dest.samplers.push_back(s);
    }

    const std::size_t imgBase = dest.images.size();
    for (tinygltf::Image im : src.images)
    {
        if (im.bufferView >= 0)
        {
            im.bufferView += static_cast<int>(bvBase);
        }
        dest.images.push_back(std::move(im));
    }

    const std::size_t texBase = dest.textures.size();
    for (tinygltf::Texture t : src.textures)
    {
        if (t.sampler >= 0)
        {
            t.sampler += static_cast<int>(sampBase);
        }
        if (t.source >= 0)
        {
            t.source += static_cast<int>(imgBase);
        }
        dest.textures.push_back(std::move(t));
    }

    const std::size_t matBase = dest.materials.size();
    const int texDelta = static_cast<int>(texBase);
    for (tinygltf::Material m : src.materials)
    {
        if (m.normalTexture.index >= 0)
        {
            m.normalTexture.index += texDelta;
        }
        if (m.occlusionTexture.index >= 0)
        {
            m.occlusionTexture.index += texDelta;
        }
        BumpTextureInfo(m.emissiveTexture, texDelta);
        BumpTextureInfo(m.pbrMetallicRoughness.baseColorTexture, texDelta);
        BumpTextureInfo(m.pbrMetallicRoughness.metallicRoughnessTexture, texDelta);
        dest.materials.push_back(std::move(m));
    }

    const std::size_t meshBase = dest.meshes.size();
    for (tinygltf::Mesh mesh : src.meshes)
    {
        for (tinygltf::Primitive& prim : mesh.primitives)
        {
            for (auto& attr : prim.attributes)
            {
                if (attr.second >= 0)
                {
                    attr.second += static_cast<int>(accBase);
                }
            }
            if (prim.indices >= 0)
            {
                prim.indices += static_cast<int>(accBase);
            }
            if (prim.material >= 0)
            {
                prim.material += static_cast<int>(matBase);
            }
        }
        dest.meshes.push_back(std::move(mesh));
    }

    // Do not append cameras/nodes/skins from prototype files — tile roots are built explicitly below.

    MergeUnique(dest.extensionsUsed, src.extensionsUsed);
    MergeUnique(dest.extensionsRequired, src.extensionsRequired);

    if (srcPrimary >= 0)
    {
        outPrimaryMeshIndex = static_cast<int>(meshBase) + srcPrimary;
    }
    else if (!src.meshes.empty())
    {
        outPrimaryMeshIndex = static_cast<int>(meshBase);
    }
    return true;
}

static std::size_t AlignUpSize(std::size_t value, std::size_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }
    const std::size_t rem = value % alignment;
    return rem == 0 ? value : value + (alignment - rem);
}

/// Concatenate all buffers into index 0 so GLB output uses one BIN chunk and stays within
/// uint32 size limits (tinygltf embeds extra buffers as base64 in JSON otherwise).
static bool ConsolidateGltfBuffersToSingle(tinygltf::Model& model, std::string& outError)
{
    outError.clear();
    if (model.buffers.size() <= 1)
    {
        return true;
    }
    constexpr std::size_t kAlign = 8;
    const std::size_t bufCount = model.buffers.size();
    std::vector<std::size_t> base(bufCount, 0);
    std::size_t total = 0;
    for (std::size_t i = 0; i < bufCount; ++i)
    {
        if (i > 0)
        {
            total = AlignUpSize(total, kAlign);
        }
        base[i] = total;
        const std::size_t chunk = model.buffers[i].data.size();
        total += chunk;
        if (chunk == 0 && i + 1 < bufCount)
        {
            total += kAlign;
        }
    }

    std::vector<unsigned char> combined(total, 0);
    for (std::size_t i = 0; i < bufCount; ++i)
    {
        const std::vector<unsigned char>& src = model.buffers[i].data;
        if (!src.empty())
        {
            if (base[i] + src.size() > combined.size())
            {
                outError = "consolidate internal size mismatch";
                return false;
            }
            std::memcpy(combined.data() + base[i], src.data(), src.size());
        }
    }
    for (tinygltf::BufferView& bv : model.bufferViews)
    {
        if (bv.buffer < 0 || static_cast<std::size_t>(bv.buffer) >= bufCount)
        {
            outError = "bufferView references invalid buffer index";
            return false;
        }
        const std::size_t bidx = static_cast<std::size_t>(bv.buffer);
        bv.byteOffset = base[bidx] + bv.byteOffset;
        bv.buffer = 0;
    }
    tinygltf::Buffer one;
    one.data = std::move(combined);
    model.buffers.clear();
    model.buffers.push_back(std::move(one));
    return true;
}
} // namespace

namespace glb_compose
{
bool ComposeInstancedLeafGlb(
    const std::vector<std::pair<std::uint32_t, core::Transform4d>>& instancesInDrawOrder,
    const core::SceneIR& sceneIr,
    const std::string& tilesetOutDir,
    const std::string& outputGlbPath,
    std::string& outError,
    InstancedLeafComposeStats* outStats)
{
    outError.clear();
    if (instancesInDrawOrder.empty())
    {
        outError = "no instances";
        return false;
    }

    tinygltf::Model combined;
    combined.asset.version = "2.0";
    combined.asset.generator = "model2tile baked leaf (per-instance geometry)";

    std::unordered_set<std::uint32_t> uniqueProtoIds;
    tinygltf::TinyGLTF loader;

    tinygltf::Scene sceneOut;
    sceneOut.name = "tile";

    for (const auto& pr : instancesInDrawOrder)
    {
        const std::uint32_t pid = pr.first;
        uniqueProtoIds.insert(pid);

        if (pid >= sceneIr.prototypes.size())
        {
            outError = "prototype id out of range";
            return false;
        }
        const std::string& relUri = sceneIr.prototypes[pid].highLodGlbUri;
        if (relUri.empty())
        {
            outError = "empty prototype highLodGlbUri for id=" + std::to_string(pid);
            return false;
        }
        const std::filesystem::path glbPath = std::filesystem::path(tilesetOutDir) / relUri;
        tinygltf::Model proto;
        std::string err;
        std::string warn;
        if (!loader.LoadBinaryFromFile(&proto, &err, &warn, glbPath.string()))
        {
            outError = "failed to load prototype glb " + glbPath.string() + ": " + err;
            return false;
        }
        if (!warn.empty())
        {
            std::cerr << "[glb_compose] warn loading " << glbPath << ": " << warn << "\n";
        }

        int meshIndex = -1;
        if (!AppendGltfModel(combined, proto, meshIndex))
        {
            outError = "append model failed";
            return false;
        }
        if (meshIndex < 0)
        {
            outError = "could not resolve mesh for prototype id=" + std::to_string(pid);
            return false;
        }

        if (!BakeWorldTransformIntoMeshVertices(combined, meshIndex, pr.second))
        {
            outError = "bake transform into mesh failed for prototype id=" + std::to_string(pid);
            return false;
        }

        tinygltf::Node node;
        node.mesh = meshIndex;
        const int nodeIndex = static_cast<int>(combined.nodes.size());
        combined.nodes.push_back(std::move(node));
        sceneOut.nodes.push_back(nodeIndex);
    }

    combined.scenes.push_back(std::move(sceneOut));
    combined.defaultScene = 0;

    if (outStats)
    {
        outStats->instances = instancesInDrawOrder.size();
        outStats->uniquePrototypes = uniqueProtoIds.size();
        outStats->materials = combined.materials.size();
    }

    if (!ConsolidateGltfBuffersToSingle(combined, outError))
    {
        outError = "buffer consolidation failed: " + outError;
        return false;
    }

    tinygltf::TinyGLTF writer;
    if (!writer.WriteGltfSceneToFile(&combined, outputGlbPath, true, true, true, true))
    {
        outError = "WriteGltfSceneToFile failed: " + outputGlbPath;
        return false;
    }

    return true;
}
} // namespace glb_compose
