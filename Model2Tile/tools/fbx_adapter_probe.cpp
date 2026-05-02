#include "../adapters/fbx_to_scene_ir.h"
#include "../importers/fbx_occurrence.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
importers::FbxOccurrence MakeOccurrence(
    const std::string& sourceLabel,
    const std::string& geometryKey,
    const std::string& materialKey,
    const bool fromExplicitReference,
    const std::uint32_t triangleCount)
{
    importers::FbxOccurrence occ;
    occ.sourceLabel = sourceLabel;
    occ.geometryKey = geometryKey;
    occ.materialKey = materialKey;
    occ.qualifiedPrototypeKey = geometryKey + "|mat:" + materialKey;
    occ.fromExplicitReference = fromExplicitReference;
    occ.triangleCount = triangleCount;
    occ.localBounds = {0.0, 0.0, 0.0, 1.0, 1.0, 1.0, true};
    occ.worldBounds = {0.0, 0.0, 0.0, 1.0, 1.0, 1.0, true};
    occ.meshPayload.positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    occ.meshPayload.indices = {0, 1, 2};
    return occ;
}
} // namespace

int main()
{
    std::vector<importers::FbxOccurrence> occurrences;
    occurrences.push_back(MakeOccurrence("inst_a", "geom_shared", "mat_shared", false, 10));
    occurrences.push_back(MakeOccurrence("inst_b", "geom_shared", "mat_shared", false, 10));
    occurrences.push_back(MakeOccurrence("inst_c", "geom_unique", "mat_other", true, 5));

    std::unordered_map<std::string, std::string> protoUris;
    protoUris["geom_shared|mat:mat_shared"] = "instance_lods/proto_0_high.glb";
    protoUris["geom_unique|mat:mat_other"] = "instance_lods/proto_1_high.glb";

    const core::Aabb worldBounds = {-1.0, -1.0, -1.0, 2.0, 2.0, 2.0, true};

    const core::SceneIR scene = adapters::BuildSceneIRFromFbxOccurrences(
        "probe.fbx",
        occurrences,
        worldBounds,
        &protoUris,
        nullptr);

    if (scene.instances.size() != 3)
    {
        std::cerr << "Expected 3 instances, got " << scene.instances.size() << "\n";
        return 1;
    }
    if (scene.prototypes.size() != 2)
    {
        std::cerr << "Expected 2 prototypes, got " << scene.prototypes.size() << "\n";
        return 1;
    }
    if (scene.qualifiedDedupInstances != 1)
    {
        std::cerr << "Expected qualifiedDedupInstances=1, got " << scene.qualifiedDedupInstances << "\n";
        return 1;
    }
    if (scene.explicitReferenceInstances != 1)
    {
        std::cerr << "Expected explicitReferenceInstances=1, got " << scene.explicitReferenceInstances << "\n";
        return 1;
    }
    if (scene.instances[1].prototypeId != scene.instances[0].prototypeId)
    {
        std::cerr << "Shared instances did not reuse prototype\n";
        return 1;
    }
    if (scene.instances[2].prototypeId == scene.instances[0].prototypeId)
    {
        std::cerr << "Unique occurrence unexpectedly deduped\n";
        return 1;
    }
    if (scene.instances[0].highLodGlbUri.empty())
    {
        std::cerr << "Missing high LOD URI in SceneIR instances\n";
        return 1;
    }
    if (scene.prototypes[0].highLodGlbUri != "instance_lods/proto_0_high.glb")
    {
        std::cerr << "Unexpected prototype 0 URI\n";
        return 1;
    }

    std::cout << "fbx_adapter_probe: PASS\n";
    return 0;
}
