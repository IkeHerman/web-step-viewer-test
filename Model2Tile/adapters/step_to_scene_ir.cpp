#include "step_to_scene_ir.h"

#include <TDF_Tool.hxx>
#include <TCollection_AsciiString.hxx>

namespace
{
core::Aabb ToAabb(const Bnd_Box& box)
{
    core::Aabb out;
    if (box.IsVoid())
    {
        out.valid = false;
        return out;
    }

    box.Get(out.xmin, out.ymin, out.zmin, out.xmax, out.ymax, out.zmax);
    out.valid = true;
    return out;
}

std::string LabelToStringSafe(const TDF_Label& label)
{
    if (label.IsNull())
    {
        return {};
    }

    TCollection_AsciiString entry;
    TDF_Tool::Entry(label, entry);
    return std::string(entry.ToCString());
}
} // namespace

namespace adapters
{
core::SceneIR BuildSceneIRFromStepOccurrences(
    const std::string& sourcePath,
    const std::vector<Occurrence>& occurrences,
    const Bnd_Box& globalBounds)
{
    core::SceneIR out;
    out.sourcePath = sourcePath;
    out.sourceFormat = "step";
    out.worldBounds = ToAabb(globalBounds);
    out.occurrences.reserve(occurrences.size());

    for (const Occurrence& occ : occurrences)
    {
        core::SceneOccurrence item;
        item.sourceLabel = LabelToStringSafe(occ.EffectiveLabel.IsNull() ? occ.Label : occ.EffectiveLabel);
        item.triangleCount = occ.TriangleCount;
        item.worldBounds = ToAabb(occ.WorldBounds);
        out.totalTriangles += static_cast<std::uint64_t>(occ.TriangleCount);
        out.occurrences.push_back(std::move(item));
    }

    return out;
}
} // namespace adapters
