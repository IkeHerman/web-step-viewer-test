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

struct SceneOccurrence
{
    std::string sourceLabel;
    std::uint32_t triangleCount = 0;
    Aabb worldBounds;
};

struct SceneIR
{
    std::string sourcePath;
    std::string sourceFormat;
    std::vector<SceneOccurrence> occurrences;
    Aabb worldBounds;
    std::uint64_t totalTriangles = 0;
};
} // namespace core
