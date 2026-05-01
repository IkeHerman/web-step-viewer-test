#include "fbx_to_scene_ir.h"

#include <cstdint>
#include <unordered_map>

namespace adapters
{
core::SceneIR BuildSceneIRFromFbxOccurrences(
    const std::string& sourcePath,
    const std::vector<importers::FbxOccurrence>& occurrences,
    const core::Aabb& globalBounds,
    const std::vector<std::string>* highLodGlbUris,
    const std::vector<std::string>* lowLodGlbUris)
{
    core::SceneIR out;
    out.sourcePath = sourcePath;
    out.sourceFormat = "fbx";
    out.worldBounds = globalBounds;
    out.instances.reserve(occurrences.size());

    std::unordered_map<std::string, std::uint32_t> prototypeByKey;
    for (const importers::FbxOccurrence& occ : occurrences)
    {
        std::uint32_t prototypeId = 0;
        bool reusedExistingPrototype = false;
        const auto existing = prototypeByKey.find(occ.qualifiedPrototypeKey);
        if (existing == prototypeByKey.end())
        {
            prototypeId = static_cast<std::uint32_t>(out.prototypes.size());
            core::ScenePrototype prototype;
            prototype.id = prototypeId;
            prototype.sourceLabel = occ.sourceLabel;
            prototype.geometryKey = occ.geometryKey;
            prototype.materialKey = occ.materialKey;
            prototype.triangleCount = occ.triangleCount;
            prototype.localBounds = occ.localBounds;
            out.prototypes.push_back(std::move(prototype));
            prototypeByKey.emplace(occ.qualifiedPrototypeKey, prototypeId);
        }
        else
        {
            prototypeId = existing->second;
            reusedExistingPrototype = true;
        }

        core::SceneInstance instance;
        instance.id = static_cast<std::uint32_t>(out.instances.size());
        instance.sourceLabel = occ.sourceLabel;
        instance.prototypeId = prototypeId;
        instance.fromExplicitReference = occ.fromExplicitReference;
        instance.worldTransform = occ.worldTransform;
        instance.worldBounds = occ.worldBounds;
        if (highLodGlbUris && instance.id < highLodGlbUris->size())
        {
            instance.highLodGlbUri = (*highLodGlbUris)[instance.id];
        }
        if (lowLodGlbUris && instance.id < lowLodGlbUris->size())
        {
            instance.lowLodGlbUri = (*lowLodGlbUris)[instance.id];
        }
        out.instances.push_back(std::move(instance));
        if (occ.fromExplicitReference)
        {
            ++out.explicitReferenceInstances;
        }
        if (reusedExistingPrototype && !occ.fromExplicitReference)
        {
            ++out.qualifiedDedupInstances;
        }
        out.totalTriangles += static_cast<std::uint64_t>(occ.triangleCount);
    }
    return out;
}
} // namespace adapters
