#include "fbx_instance_lod.h"

#include "../dep/tinygltf/tiny_gltf.h"

#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{
template <typename T>
void AppendBytes(std::vector<unsigned char>& out, const std::vector<T>& values)
{
    if (values.empty())
    {
        return;
    }
    const std::size_t oldSize = out.size();
    const std::size_t addSize = values.size() * sizeof(T);
    out.resize(oldSize + addSize);
    std::memcpy(out.data() + oldSize, values.data(), addSize);
}

void WriteRowMajorToGltfColumnMajor(const core::Transform4d& src, std::vector<double>& dst16)
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

double RowMajorDeterminant3x3(const core::Transform4d& m)
{
    const double a00 = m.m[0];
    const double a01 = m.m[1];
    const double a02 = m.m[2];
    const double a10 = m.m[4];
    const double a11 = m.m[5];
    const double a12 = m.m[6];
    const double a20 = m.m[8];
    const double a21 = m.m[9];
    const double a22 = m.m[10];

    return a00 * (a11 * a22 - a12 * a21) -
           a01 * (a10 * a22 - a12 * a20) +
           a02 * (a10 * a21 - a11 * a20);
}

double GltfColumnMajorDeterminant3x3(const std::vector<double>& m)
{
    if (m.size() < 16)
    {
        return 0.0;
    }

    const double l00 = m[0];
    const double l01 = m[4];
    const double l02 = m[8];
    const double l10 = m[1];
    const double l11 = m[5];
    const double l12 = m[9];
    const double l20 = m[2];
    const double l21 = m[6];
    const double l22 = m[10];

    return l00 * (l11 * l22 - l12 * l21) -
           l01 * (l10 * l22 - l12 * l20) +
           l02 * (l10 * l21 - l11 * l20);
}

bool IsFbxMatrixDebugEnabled()
{
    const char* env = std::getenv("MODEL2TILE_FBX_MATRIX_DEBUG");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
}

void EmitFbxMatrixDebug(const importers::FbxOccurrence& occ, const std::vector<double>& gltfMatrix)
{
    static int s_printed = 0;
    if (s_printed >= 20)
    {
        return;
    }
    ++s_printed;

    const double rowDet = RowMajorDeterminant3x3(occ.worldTransform);
    const double gltfDet = GltfColumnMajorDeterminant3x3(gltfMatrix);
    const double detDelta = std::abs(rowDet - gltfDet);

    const double rowTx = occ.worldTransform.m[3];
    const double rowTy = occ.worldTransform.m[7];
    const double rowTz = occ.worldTransform.m[11];
    const double gltfTx = (gltfMatrix.size() >= 16) ? gltfMatrix[12] : 0.0;
    const double gltfTy = (gltfMatrix.size() >= 16) ? gltfMatrix[13] : 0.0;
    const double gltfTz = (gltfMatrix.size() >= 16) ? gltfMatrix[14] : 0.0;

    std::cout << "[FbxMatrixDebug] occ=\"" << occ.sourceLabel
              << "\" rowDet=" << rowDet
              << " gltfDet=" << gltfDet
              << " detDelta=" << detDelta
              << " rowT=(" << rowTx << "," << rowTy << "," << rowTz << ")"
              << " gltfT=(" << gltfTx << "," << gltfTy << "," << gltfTz << ")"
              << " hasNormals=" << (!occ.meshPayload.normals.empty() ? 1 : 0)
              << "\n";
}

bool WriteOccurrenceGlb(const importers::FbxOccurrence& occ, const std::string& glbPath)
{
    const std::vector<std::uint32_t>& indices = occ.meshPayload.indices;
    const bool hasNormals = (occ.meshPayload.normals.size() == occ.meshPayload.positions.size());

    tinygltf::Model model;
    tinygltf::Scene scene;
    scene.nodes.push_back(0);
    model.scenes.push_back(scene);
    model.defaultScene = 0;

    tinygltf::Node node;
    node.mesh = 0;
    WriteRowMajorToGltfColumnMajor(occ.worldTransform, node.matrix);
    if (IsFbxMatrixDebugEnabled())
    {
        EmitFbxMatrixDebug(occ, node.matrix);
    }
    model.nodes.push_back(node);

    const bool hasTexcoords = (occ.meshPayload.texcoords0.size() == (occ.meshPayload.positions.size() / 3) * 2);
    tinygltf::Mesh mesh;
    tinygltf::Primitive primitive;
    primitive.mode = TINYGLTF_MODE_TRIANGLES;
    primitive.attributes["POSITION"] = 0;
    int accessorIndex = 1;
    if (hasNormals)
    {
        primitive.attributes["NORMAL"] = accessorIndex++;
    }
    if (hasTexcoords)
    {
        primitive.attributes["TEXCOORD_0"] = accessorIndex++;
    }
    primitive.indices = accessorIndex;
    primitive.material = 0;
    mesh.primitives.push_back(primitive);
    model.meshes.push_back(mesh);

    const bool hasTextureBytes = !occ.meshPayload.baseColorTextureBytes.empty();

    tinygltf::Material material;
    material.pbrMetallicRoughness.baseColorFactor = {
        occ.meshPayload.baseColor[0],
        occ.meshPayload.baseColor[1],
        occ.meshPayload.baseColor[2],
        occ.meshPayload.baseColor[3]};
    material.pbrMetallicRoughness.metallicFactor = 0.0;
    material.pbrMetallicRoughness.roughnessFactor = 1.0;
    if (hasTextureBytes)
    {
        tinygltf::Sampler sampler;
        sampler.minFilter = TINYGLTF_TEXTURE_FILTER_LINEAR_MIPMAP_LINEAR;
        sampler.magFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
        sampler.wrapS = TINYGLTF_TEXTURE_WRAP_REPEAT;
        sampler.wrapT = TINYGLTF_TEXTURE_WRAP_REPEAT;
        model.samplers.push_back(std::move(sampler));

        tinygltf::Texture texture;
        texture.source = 0;
        texture.sampler = 0;
        model.textures.push_back(std::move(texture));

        material.pbrMetallicRoughness.baseColorTexture.index = 0;
    }
    model.materials.push_back(material);

    std::vector<unsigned char> bufferData;
    AppendBytes(bufferData, occ.meshPayload.positions);
    const std::size_t normalOffset = bufferData.size();
    if (hasNormals)
    {
        AppendBytes(bufferData, occ.meshPayload.normals);
    }
    const std::size_t texcoordOffset = bufferData.size();
    if (hasTexcoords)
    {
        AppendBytes(bufferData, occ.meshPayload.texcoords0);
    }
    const std::size_t imageOffset = bufferData.size();
    if (hasTextureBytes)
    {
        AppendBytes(bufferData, occ.meshPayload.baseColorTextureBytes);
    }
    const std::size_t indexOffset = bufferData.size();
    AppendBytes(bufferData, indices);

    tinygltf::Buffer buffer;
    buffer.data = std::move(bufferData);
    model.buffers.push_back(std::move(buffer));

    tinygltf::BufferView posView;
    posView.buffer = 0;
    posView.byteOffset = 0;
    posView.byteLength = static_cast<int>(occ.meshPayload.positions.size() * sizeof(float));
    posView.target = TINYGLTF_TARGET_ARRAY_BUFFER;
    model.bufferViews.push_back(posView);

    int nextBufferViewIndex = 1;
    int normalBufferViewIndex = -1;
    if (hasNormals)
    {
        tinygltf::BufferView normalView;
        normalView.buffer = 0;
        normalView.byteOffset = static_cast<int>(normalOffset);
        normalView.byteLength = static_cast<int>(occ.meshPayload.normals.size() * sizeof(float));
        normalView.target = TINYGLTF_TARGET_ARRAY_BUFFER;
        normalBufferViewIndex = nextBufferViewIndex++;
        model.bufferViews.push_back(normalView);
    }
    int uvBufferViewIndex = -1;
    if (hasTexcoords)
    {
        tinygltf::BufferView uvView;
        uvView.buffer = 0;
        uvView.byteOffset = static_cast<int>(texcoordOffset);
        uvView.byteLength = static_cast<int>(occ.meshPayload.texcoords0.size() * sizeof(float));
        uvView.target = TINYGLTF_TARGET_ARRAY_BUFFER;
        uvBufferViewIndex = nextBufferViewIndex;
        model.bufferViews.push_back(uvView);
        ++nextBufferViewIndex;
    }
    int imageBufferViewIndex = -1;
    if (hasTextureBytes)
    {
        tinygltf::BufferView imgView;
        imgView.buffer = 0;
        imgView.byteOffset = static_cast<int>(imageOffset);
        imgView.byteLength = static_cast<int>(occ.meshPayload.baseColorTextureBytes.size());
        imgView.target = 0;
        imageBufferViewIndex = nextBufferViewIndex++;
        model.bufferViews.push_back(imgView);

        tinygltf::Image image;
        image.bufferView = imageBufferViewIndex;
        image.mimeType = occ.meshPayload.baseColorTextureMimeType.empty()
            ? std::string("application/octet-stream")
            : occ.meshPayload.baseColorTextureMimeType;
        model.images.push_back(std::move(image));
    }

    tinygltf::BufferView idxView;
    idxView.buffer = 0;
    idxView.byteOffset = static_cast<int>(indexOffset);
    idxView.byteLength = static_cast<int>(indices.size() * sizeof(std::uint32_t));
    idxView.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;
    const int indexBufferViewIndex = nextBufferViewIndex;
    model.bufferViews.push_back(idxView);

    tinygltf::Accessor posAccessor;
    posAccessor.bufferView = 0;
    posAccessor.byteOffset = 0;
    posAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    posAccessor.count = static_cast<int>(occ.meshPayload.positions.size() / 3);
    posAccessor.type = TINYGLTF_TYPE_VEC3;
    if (occ.localBounds.valid)
    {
        posAccessor.minValues = {occ.localBounds.xmin, occ.localBounds.ymin, occ.localBounds.zmin};
        posAccessor.maxValues = {occ.localBounds.xmax, occ.localBounds.ymax, occ.localBounds.zmax};
    }
    model.accessors.push_back(posAccessor);

    if (hasNormals)
    {
        tinygltf::Accessor normalAccessor;
        normalAccessor.bufferView = normalBufferViewIndex;
        normalAccessor.byteOffset = 0;
        normalAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
        normalAccessor.count = static_cast<int>(occ.meshPayload.normals.size() / 3);
        normalAccessor.type = TINYGLTF_TYPE_VEC3;
        model.accessors.push_back(normalAccessor);
    }

    if (hasTexcoords)
    {
        tinygltf::Accessor uvAccessor;
        uvAccessor.bufferView = uvBufferViewIndex;
        uvAccessor.byteOffset = 0;
        uvAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
        uvAccessor.count = static_cast<int>(occ.meshPayload.texcoords0.size() / 2);
        uvAccessor.type = TINYGLTF_TYPE_VEC2;
        model.accessors.push_back(uvAccessor);
    }

    tinygltf::Accessor idxAccessor;
    idxAccessor.bufferView = indexBufferViewIndex;
    idxAccessor.byteOffset = 0;
    idxAccessor.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
    idxAccessor.count = static_cast<int>(indices.size());
    idxAccessor.type = TINYGLTF_TYPE_SCALAR;
    model.accessors.push_back(idxAccessor);

    tinygltf::TinyGLTF writer;
    std::string err;
    std::string warn;
    const bool ok = writer.WriteGltfSceneToFile(&model, glbPath, true, true, true, true);
    if (!warn.empty())
    {
        std::cout << "[FbxInstanceLod] warning: " << warn << "\n";
    }
    if (!ok)
    {
        std::cerr << "[FbxInstanceLod] failed to write glb " << glbPath << " error: " << err << "\n";
    }
    return ok;
}
} // namespace

namespace importers
{
bool BakeFbxInstanceLods(
    const std::vector<FbxOccurrence>& occurrences,
    const std::string& outputDirectory,
    const std::string& outputUriPrefix,
    std::vector<std::string>& outHighGlbUris)
{
    std::error_code ec;
    std::filesystem::create_directories(outputDirectory, ec);
    if (ec)
    {
        std::cerr << "[FbxInstanceLod] failed to create directory: " << outputDirectory << "\n";
        return false;
    }

    outHighGlbUris.assign(occurrences.size(), std::string());

    for (std::size_t i = 0; i < occurrences.size(); ++i)
    {
        const std::string stem = "occ_" + std::to_string(i);
        const std::filesystem::path highPath = std::filesystem::path(outputDirectory) / (stem + "_high.glb");

        if (!WriteOccurrenceGlb(occurrences[i], highPath.string()))
        {
            return false;
        }

        outHighGlbUris[i] = outputUriPrefix + "/" + stem + "_high.glb";
    }

    std::cout << "[FbxInstanceLod] baked high GLBs for "
              << occurrences.size() << " occurrences under " << outputDirectory << "\n";
    return true;
}
} // namespace importers
