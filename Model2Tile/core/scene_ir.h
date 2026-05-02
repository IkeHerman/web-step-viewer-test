#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace core
{
struct Aabb
{
    double xmin = 0.0;
    double ymin = 0.0;
    double zmin = 0.0;
    double xmax = 0.0;
    double ymax = 0.0;
    double zmax = 0.0;
    bool valid = false;
};

struct Transform4d
{
    // Row-major 4x4 transform matrix.
    double m[16] = {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0
    };
};

struct ScenePrototype
{
    std::uint32_t id = 0;
    std::string sourceLabel;
    std::string geometryKey;
    std::string materialKey;
    std::uint32_t triangleCount = 0;
    Aabb localBounds;
    std::string highLodGlbUri;
};

struct SceneInstance
{
    std::uint32_t id = 0;
    std::string sourceLabel;
    std::uint32_t prototypeId = 0;
    bool fromExplicitReference = false;
    Transform4d worldTransform;
    /// STEP only (identity elsewhere): maps prototype GLB vertex coords (first baked occurrence for this
    /// qualified key) into this occurrence's intrinsic local solid frame when instances share a key but
    /// differ in intrinsic placement (translation by bbox-center delta). Tile composition uses
    /// `worldTransform`, which includes this mapping as `WorldOcc * prototypeMeshToOccurrenceLocal`.
    Transform4d prototypeMeshToOccurrenceLocal;
    Aabb worldBounds;
    std::string highLodGlbUri;
    std::string lowLodGlbUri;
};

struct SceneIR
{
    std::string sourcePath;
    std::string sourceFormat;
    std::vector<ScenePrototype> prototypes;
    std::vector<SceneInstance> instances;
    Aabb worldBounds;
    std::uint64_t totalTriangles = 0;
    std::uint64_t explicitReferenceInstances = 0;
    std::uint64_t qualifiedDedupInstances = 0;
    std::uint64_t shapeColorOccurrences = 0;
    std::uint64_t shapeMaterialOccurrences = 0;
    std::uint64_t faceColorEntries = 0;
    std::uint64_t faceMaterialEntries = 0;
    std::uint64_t metadataOccurrences = 0;
};
} // namespace core
