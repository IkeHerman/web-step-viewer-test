#include "../glbopt.h"
#include "../dep/tinygltf/tiny_gltf.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{
template <typename T>
void AppendBytes(std::vector<unsigned char>& dst, const std::vector<T>& src)
{
    const std::size_t oldSize = dst.size();
    dst.resize(oldSize + src.size() * sizeof(T));
    std::memcpy(dst.data() + oldSize, src.data(), src.size() * sizeof(T));
}

bool WriteSinglePrimitiveGlb(
    const std::filesystem::path& path,
    const std::vector<float>& positions,
    const std::vector<std::uint32_t>& indices,
    const std::array<double, 3>& scale)
{
    tinygltf::Model model;
    model.asset.version = "2.0";

    tinygltf::Buffer payload;
    AppendBytes(payload.data, positions);
    const std::size_t indexOffset = payload.data.size();
    AppendBytes(payload.data, indices);
    model.buffers.push_back(std::move(payload));

    tinygltf::BufferView posView;
    posView.buffer = 0;
    posView.byteOffset = 0;
    posView.byteLength = positions.size() * sizeof(float);
    posView.target = TINYGLTF_TARGET_ARRAY_BUFFER;
    model.bufferViews.push_back(std::move(posView));

    tinygltf::BufferView idxView;
    idxView.buffer = 0;
    idxView.byteOffset = indexOffset;
    idxView.byteLength = indices.size() * sizeof(std::uint32_t);
    idxView.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;
    model.bufferViews.push_back(std::move(idxView));

    tinygltf::Accessor posAccessor;
    posAccessor.bufferView = 0;
    posAccessor.byteOffset = 0;
    posAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    posAccessor.count = positions.size() / 3;
    posAccessor.type = TINYGLTF_TYPE_VEC3;
    model.accessors.push_back(std::move(posAccessor));

    tinygltf::Accessor idxAccessor;
    idxAccessor.bufferView = 1;
    idxAccessor.byteOffset = 0;
    idxAccessor.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
    idxAccessor.count = indices.size();
    idxAccessor.type = TINYGLTF_TYPE_SCALAR;
    model.accessors.push_back(std::move(idxAccessor));

    tinygltf::Primitive primitive;
    primitive.mode = TINYGLTF_MODE_TRIANGLES;
    primitive.attributes["POSITION"] = 0;
    primitive.indices = 1;

    tinygltf::Mesh mesh;
    mesh.primitives.push_back(std::move(primitive));
    model.meshes.push_back(std::move(mesh));

    tinygltf::Node node;
    node.mesh = 0;
    node.scale = {scale[0], scale[1], scale[2]};
    model.nodes.push_back(std::move(node));

    tinygltf::Scene scene;
    scene.nodes.push_back(0);
    model.scenes.push_back(std::move(scene));
    model.defaultScene = 0;

    tinygltf::TinyGLTF writer;
    std::string err;
    std::string warn;
    const bool ok = writer.WriteGltfSceneToFile(&model, path.string(), true, true, true, true);
    if (!warn.empty())
    {
        std::cout << "[probe] write warning: " << warn << "\n";
    }
    if (!ok && !err.empty())
    {
        std::cout << "[probe] write error: " << err << "\n";
    }
    return ok;
}

bool ReadU32Indices(const std::filesystem::path& path, std::vector<std::uint32_t>& outIndices)
{
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err;
    std::string warn;
    if (!loader.LoadBinaryFromFile(&model, &err, &warn, path.string()))
    {
        std::cout << "[probe] read error: " << err << "\n";
        return false;
    }
    if (model.meshes.empty() || model.meshes[0].primitives.empty())
    {
        return false;
    }

    const tinygltf::Primitive& primitive = model.meshes[0].primitives[0];
    if (primitive.indices < 0 || primitive.indices >= static_cast<int>(model.accessors.size()))
    {
        return false;
    }
    const tinygltf::Accessor& accessor = model.accessors[primitive.indices];
    if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT ||
        accessor.bufferView < 0 ||
        accessor.bufferView >= static_cast<int>(model.bufferViews.size()))
    {
        return false;
    }
    const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
    if (view.buffer < 0 || view.buffer >= static_cast<int>(model.buffers.size()))
    {
        return false;
    }
    const tinygltf::Buffer& buffer = model.buffers[view.buffer];
    const std::size_t stride = accessor.ByteStride(view) == 0 ? sizeof(std::uint32_t) : accessor.ByteStride(view);
    const std::size_t base = view.byteOffset + accessor.byteOffset;
    outIndices.clear();
    outIndices.reserve(accessor.count);
    for (std::size_t i = 0; i < accessor.count; ++i)
    {
        const std::size_t off = base + i * stride;
        if (off + sizeof(std::uint32_t) > buffer.data.size())
        {
            return false;
        }
        outIndices.push_back(*reinterpret_cast<const std::uint32_t*>(buffer.data.data() + off));
    }
    return true;
}
} // namespace

int main()
{
    const std::filesystem::path root = std::filesystem::current_path();
    const std::filesystem::path outDir = root / "build" / "tools";
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec)
    {
        std::cerr << "[probe] failed to create output dir: " << outDir << "\n";
        return 1;
    }

    const std::vector<float> triPos = {
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f
    };

    // Case 1: invalid index must be rejected by glbopt extraction.
    const std::filesystem::path invalidIn = outDir / "edge_invalid_indices_in.glb";
    const std::filesystem::path invalidOut = outDir / "edge_invalid_indices_out.glb";
    if (!WriteSinglePrimitiveGlb(invalidIn, triPos, {0, 1, 99}, {1.0, 1.0, 1.0}))
    {
        std::cerr << "[probe] failed to write invalid-index input\n";
        return 1;
    }
    glbopt::Stats invalidStats;
    const bool invalidOk = glbopt::OptimizeGlbFile(
        invalidIn.string(),
        invalidOut.string(),
        glbopt::Options{},
        invalidStats);
    if (invalidOk || invalidStats.DroppedPrimitivesInvalidIndices == 0)
    {
        std::cerr << "[probe] expected invalid-index primitive rejection\n";
        return 1;
    }

    // Case 2: mirrored transform should flip triangle winding in triangle mode.
    const std::filesystem::path mirrorIn = outDir / "edge_mirror_in.glb";
    const std::filesystem::path mirrorOut = outDir / "edge_mirror_out.glb";
    if (!WriteSinglePrimitiveGlb(mirrorIn, triPos, {0, 1, 2}, {-1.0, 1.0, 1.0}))
    {
        std::cerr << "[probe] failed to write mirrored input\n";
        return 1;
    }
    glbopt::Options mirrorOptions;
    mirrorOptions.WeldPositions = false;
    mirrorOptions.WeldNormals = false;
    mirrorOptions.WeldTexcoord0 = false;
    mirrorOptions.WeldColor0 = false;
    mirrorOptions.RemoveDegenerateByIndex = false;
    mirrorOptions.RemoveDegenerateByArea = false;
    mirrorOptions.OptimizeVertexCache = false;
    mirrorOptions.OptimizeOverdraw = false;
    mirrorOptions.OptimizeVertexFetch = false;
    mirrorOptions.Simplify = false;
    if (!glbopt::OptimizeGlbFile(mirrorIn.string(), mirrorOut.string(), mirrorOptions))
    {
        std::cerr << "[probe] mirrored transform optimize failed\n";
        return 1;
    }
    std::vector<std::uint32_t> mirrorIndices;
    if (!ReadU32Indices(mirrorOut, mirrorIndices))
    {
        std::cerr << "[probe] failed to read mirrored output indices\n";
        return 1;
    }
    if (mirrorIndices.size() < 3 || mirrorIndices[0] != 0u || mirrorIndices[1] != 2u || mirrorIndices[2] != 1u)
    {
        std::cerr << "[probe] mirrored triangle winding was not flipped as expected\n";
        return 1;
    }

    std::cout << "glbopt_edge_case_probe: PASS\n";
    return 0;
}
