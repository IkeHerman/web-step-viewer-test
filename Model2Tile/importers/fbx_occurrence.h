#pragma once

#include "../core/scene_ir.h"

#include <cstdint>
#include <array>
#include <string>
#include <vector>

namespace importers
{
struct FbxMeshPayload
{
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> texcoords0;
    std::vector<std::uint32_t> indices;
    std::array<float, 4> baseColor = {0.7f, 0.7f, 0.7f, 1.0f};
    std::string baseColorTextureSourcePath;
    std::vector<std::uint8_t> baseColorTextureBytes;
    std::string baseColorTextureMimeType;
};

struct FbxOccurrence
{
    std::string sourceLabel;
    core::Transform4d worldTransform;
    core::Aabb localBounds;
    core::Aabb worldBounds;
    std::uint32_t triangleCount = 0;
    std::string geometryKey;
    std::string materialKey;
    std::string qualifiedPrototypeKey;
    bool fromExplicitReference = false;
    FbxMeshPayload meshPayload;
};
} // namespace importers
