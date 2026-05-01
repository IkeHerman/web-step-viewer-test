#include "../dep/tinygltf/tiny_gltf.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{
int ComponentCountForType(const int type)
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

std::size_t ComponentSizeInBytes(const int componentType)
{
    switch (componentType)
    {
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        case TINYGLTF_COMPONENT_TYPE_BYTE:
            return 1;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        case TINYGLTF_COMPONENT_TYPE_SHORT:
            return 2;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        case TINYGLTF_COMPONENT_TYPE_FLOAT:
            return 4;
        default:
            return 0;
    }
}

bool ReadFloatAccessor(
    const tinygltf::Model& model,
    const int accessorIndex,
    std::vector<float>& outValues)
{
    if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size()))
    {
        return false;
    }
    const tinygltf::Accessor& accessor = model.accessors[static_cast<std::size_t>(accessorIndex)];
    if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(model.bufferViews.size()))
    {
        return false;
    }
    const tinygltf::BufferView& view = model.bufferViews[static_cast<std::size_t>(accessor.bufferView)];
    if (view.buffer < 0 || view.buffer >= static_cast<int>(model.buffers.size()))
    {
        return false;
    }
    const tinygltf::Buffer& buffer = model.buffers[static_cast<std::size_t>(view.buffer)];
    const int comps = ComponentCountForType(accessor.type);
    if (comps <= 0 || accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
    {
        return false;
    }
    const std::size_t compSize = ComponentSizeInBytes(accessor.componentType);
    std::size_t stride = accessor.ByteStride(view);
    if (stride == 0)
    {
        stride = static_cast<std::size_t>(comps) * compSize;
    }
    const std::size_t baseOffset = static_cast<std::size_t>(view.byteOffset + accessor.byteOffset);
    outValues.resize(static_cast<std::size_t>(accessor.count) * static_cast<std::size_t>(comps));

    for (std::size_t i = 0; i < static_cast<std::size_t>(accessor.count); ++i)
    {
        const std::size_t off = baseOffset + i * stride;
        if (off + static_cast<std::size_t>(comps) * compSize > buffer.data.size())
        {
            return false;
        }
        const float* src = reinterpret_cast<const float*>(buffer.data.data() + off);
        for (int c = 0; c < comps; ++c)
        {
            outValues[i * static_cast<std::size_t>(comps) + static_cast<std::size_t>(c)] = src[c];
        }
    }
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: glb_uv_probe <path.glb>\n";
        return 2;
    }

    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string warn;
    std::string err;
    if (!loader.LoadBinaryFromFile(&model, &err, &warn, argv[1]))
    {
        std::cerr << "load failed: " << err << "\n";
        return 1;
    }

    double minU = std::numeric_limits<double>::max();
    double maxU = -std::numeric_limits<double>::max();
    double minV = std::numeric_limits<double>::max();
    double maxV = -std::numeric_limits<double>::max();
    std::size_t uvCount = 0;
    std::size_t outOf01 = 0;

    for (const tinygltf::Mesh& mesh : model.meshes)
    {
        for (const tinygltf::Primitive& prim : mesh.primitives)
        {
            const auto uvIt = prim.attributes.find("TEXCOORD_0");
            if (uvIt == prim.attributes.end())
            {
                continue;
            }
            std::vector<float> uv;
            if (!ReadFloatAccessor(model, uvIt->second, uv))
            {
                continue;
            }
            for (std::size_t i = 0; i + 1 < uv.size(); i += 2)
            {
                const double u = uv[i];
                const double v = uv[i + 1];
                minU = std::min(minU, u);
                maxU = std::max(maxU, u);
                minV = std::min(minV, v);
                maxV = std::max(maxV, v);
                ++uvCount;
                if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0)
                {
                    ++outOf01;
                }
            }
        }
    }

    std::cout << "uvCount=" << uvCount
              << " uRange=[" << minU << "," << maxU << "]"
              << " vRange=[" << minV << "," << maxV << "]"
              << " outOf01=" << outOf01 << "\n";
    return 0;
}
