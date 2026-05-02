#pragma once

#include "core/scene_ir.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace glb_compose
{
/// Populated when exporting a baked leaf GLB (optional).
struct InstancedLeafComposeStats
{
    std::size_t instances = 0;           ///< Root nodes in the output scene (one mesh copy each)
    std::size_t uniquePrototypes = 0;   ///< Distinct prototype ids referenced by this tile
    std::size_t materials = 0;          ///< tinygltf material count in the combined model
};

/// Builds one GLB by duplicating prototype geometry per instance and baking each SceneIR world transform
/// into POSITION / NORMAL / TANGENT (identity node transforms). No shared mesh buffers between instances.
/// If `outStats` is non-null, it is filled with counts for the written GLB.
bool ComposeInstancedLeafGlb(
    const std::vector<std::pair<std::uint32_t, core::Transform4d>>& instancesInDrawOrder,
    const core::SceneIR& sceneIr,
    const std::string& tilesetOutDir,
    const std::string& outputGlbPath,
    std::string& outError,
    InstancedLeafComposeStats* outStats = nullptr);
} // namespace glb_compose
