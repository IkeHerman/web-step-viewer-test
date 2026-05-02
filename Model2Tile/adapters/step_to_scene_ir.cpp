#include "step_to_scene_ir.h"

#include "../core/transform_ops.h"

#include <string>
#include <unordered_map>

namespace adapters
{
core::SceneIR BuildSceneIRFromStepOccurrences(
    const std::string& sourcePath,
    const std::vector<Occurrence>& occurrences,
    const core::Aabb& globalBounds,
    const std::unordered_map<std::string, std::string>* prototypeHighLodUrisByQualifiedKey,
    const std::vector<std::string>* lowLodGlbUris)
{
    core::SceneIR out;
    out.sourcePath = sourcePath;
    out.sourceFormat = "step";
    out.worldBounds = globalBounds;
    out.instances.reserve(occurrences.size());

    std::unordered_map<std::string, std::uint32_t> prototypeByKey;

    // First occurrence with appearance per qualified key matches the bake reference in `BakeStepInstanceLods`.
    std::unordered_map<std::string, std::size_t> prototypeBakeRefOccIndex;
    prototypeBakeRefOccIndex.reserve(occurrences.size());
    for (std::size_t ri = 0; ri < occurrences.size(); ++ri)
    {
        const Occurrence& ro = occurrences[ri];
        if (!ro.Appearance)
        {
            continue;
        }
        const std::string& rq = ro.QualifiedPrototypeKey;
        if (prototypeBakeRefOccIndex.find(rq) == prototypeBakeRefOccIndex.end())
        {
            prototypeBakeRefOccIndex[rq] = ri;
        }
    }

    for (std::size_t occIndex = 0; occIndex < occurrences.size(); ++occIndex)
    {
        const Occurrence& occ = occurrences[occIndex];
        const std::string sourceLabel = occ.SourceLabelEntry;
        const std::string& geometryKey = occ.GeometryKey;
        const std::string& materialKey = occ.MaterialKey;
        const std::string& qualifiedKey = occ.QualifiedPrototypeKey;
        const bool fromExplicitReference = occ.FromExplicitReference;

        std::uint32_t prototypeId = 0;
        const std::unordered_map<std::string, std::uint32_t>::const_iterator existing =
            prototypeByKey.find(qualifiedKey);
        bool reusedExistingPrototype = false;
        if (existing == prototypeByKey.end())
        {
            prototypeId = static_cast<std::uint32_t>(out.prototypes.size());
            core::ScenePrototype prototype;
            prototype.id = prototypeId;
            prototype.sourceLabel = sourceLabel;
            prototype.geometryKey = geometryKey;
            prototype.materialKey = materialKey;
            prototype.triangleCount = occ.TriangleCount;
            prototype.localBounds = occ.LocalBoundsAabb;
            if (prototypeHighLodUrisByQualifiedKey)
            {
                const auto glbIt = prototypeHighLodUrisByQualifiedKey->find(qualifiedKey);
                if (glbIt != prototypeHighLodUrisByQualifiedKey->end())
                {
                    prototype.highLodGlbUri = glbIt->second;
                }
            }
            out.prototypes.push_back(std::move(prototype));
            prototypeByKey.emplace(qualifiedKey, prototypeId);
        }
        else
        {
            prototypeId = existing->second;
            reusedExistingPrototype = true;
        }
        core::SceneInstance instance;
        instance.id = static_cast<std::uint32_t>(out.instances.size());
        instance.sourceLabel = sourceLabel;
        instance.prototypeId = prototypeId;
        instance.fromExplicitReference = fromExplicitReference;

        core::Transform4d prototypeMeshToOccurrenceLocal = core::Transform4d{};
        core::Transform4d worldTransformOut = occ.WorldTransformMatrix;

        std::size_t bakeRefRi = static_cast<std::size_t>(-1);
        const auto refIt = prototypeBakeRefOccIndex.find(qualifiedKey);
        if (refIt != prototypeBakeRefOccIndex.end())
        {
            bakeRefRi = refIt->second;
        }

        if (occ.Appearance && bakeRefRi != static_cast<std::size_t>(-1) &&
            bakeRefRi != occIndex &&
            occ.LocalBoundsAabb.valid &&
            occurrences[bakeRefRi].LocalBoundsAabb.valid)
        {
            const core::Aabb& ra = occurrences[bakeRefRi].LocalBoundsAabb;
            const core::Aabb& oa = occ.LocalBoundsAabb;
            const double tcx = 0.5 * (oa.xmin + oa.xmax - ra.xmin - ra.xmax);
            const double tcy = 0.5 * (oa.ymin + oa.ymax - ra.ymin - ra.ymax);
            const double tcz = 0.5 * (oa.zmin + oa.zmax - ra.zmin - ra.zmax);
            prototypeMeshToOccurrenceLocal = core::TranslationTransform(tcx, tcy, tcz);
            worldTransformOut =
                core::MultiplyTransforms(occ.WorldTransformMatrix, prototypeMeshToOccurrenceLocal);
        }
        instance.prototypeMeshToOccurrenceLocal = prototypeMeshToOccurrenceLocal;
        instance.worldTransform = worldTransformOut;
        instance.worldBounds = occ.WorldBoundsAabb;
        if (prototypeId < out.prototypes.size())
        {
            instance.highLodGlbUri = out.prototypes[prototypeId].highLodGlbUri;
        }
        if (lowLodGlbUris &&
            instance.id < lowLodGlbUris->size())
        {
            instance.lowLodGlbUri =
                (*lowLodGlbUris)[instance.id];
        }
        out.instances.push_back(std::move(instance));
        if (fromExplicitReference)
        {
            ++out.explicitReferenceInstances;
        }
        if (reusedExistingPrototype && !fromExplicitReference)
        {
            ++out.qualifiedDedupInstances;
        }

        for (const Occurrence::FacePrototypeSeed& seed : occ.FacePrototypeSeeds)
        {
            const std::string& childGeometryKey = seed.GeometryKey;
            const std::string& childMaterialKey = seed.MaterialKey;
            const std::string childQualifiedKey = "face|" + childGeometryKey + "|mat:" + childMaterialKey;

            const auto childExisting = prototypeByKey.find(childQualifiedKey);
            if (childExisting == prototypeByKey.end())
            {
                core::ScenePrototype childPrototype;
                childPrototype.id = static_cast<std::uint32_t>(out.prototypes.size());
                childPrototype.sourceLabel = sourceLabel;
                childPrototype.geometryKey = childGeometryKey;
                childPrototype.materialKey = childMaterialKey;
                childPrototype.triangleCount = seed.TriangleCount;
                childPrototype.localBounds = seed.LocalBounds;
                out.prototypes.push_back(std::move(childPrototype));
                prototypeByKey.emplace(childQualifiedKey, out.prototypes.back().id);
            }
        }

        bool hasShapeColor = false;
        bool hasShapeMaterial = false;
        std::uint32_t faceColorEntries = 0;
        std::uint32_t faceMaterialEntries = 0;
        if (occ.Appearance)
        {
            const CachedColorSet& shapeColors = occ.Appearance->ResolvedShapeColors;
            hasShapeColor = shapeColors.HasGen || shapeColors.HasSurf || shapeColors.HasCurv;
            hasShapeMaterial = !occ.Appearance->ResolvedShapeMaterial.IsNull();
            for (const CachedFaceAppearance& face : occ.Appearance->Faces)
            {
                if (face.Colors.HasGen || face.Colors.HasSurf || face.Colors.HasCurv)
                {
                    ++faceColorEntries;
                }
                if (!face.VisMaterial.IsNull())
                {
                    ++faceMaterialEntries;
                }
            }
        }
        if (hasShapeColor)
        {
            ++out.shapeColorOccurrences;
        }
        if (hasShapeMaterial)
        {
            ++out.shapeMaterialOccurrences;
        }
        out.faceColorEntries += faceColorEntries;
        out.faceMaterialEntries += faceMaterialEntries;
        if (occ.HasAnyMetadata)
        {
            ++out.metadataOccurrences;
        }
        out.totalTriangles += static_cast<std::uint64_t>(occ.TriangleCount);
    }

    return out;
}
} // namespace adapters
