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

struct SceneOccurrence
{
    std::string sourceLabel;
    std::uint32_t triangleCount = 0;
    Aabb worldBounds;
};

struct ScenePrototype
{
    std::uint32_t id = 0;
    std::string sourceLabel;
    std::string geometryKey;
    std::string materialKey;
    std::uint32_t triangleCount = 0;
    Aabb localBounds;
};

struct SceneInstance
{
    std::string sourceLabel;
    std::uint32_t prototypeId = 0;
    std::uint32_t occurrenceIndex = 0;
    bool fromExplicitReference = false;
    Transform4d worldTransform;
    Aabb worldBounds;
};

struct SceneIR
{
    std::string sourcePath;
    std::string sourceFormat;
    std::vector<ScenePrototype> prototypes;
    std::vector<SceneInstance> instances;
    // Legacy flattened representation retained for compatibility during migration.
    std::vector<SceneOccurrence> occurrences;
    Aabb worldBounds;
    std::uint64_t totalTriangles = 0;
    std::uint64_t explicitReferenceInstances = 0;
    std::uint64_t qualifiedDedupInstances = 0;
};
} // namespace core
