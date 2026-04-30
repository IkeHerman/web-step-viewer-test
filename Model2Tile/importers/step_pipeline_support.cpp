#include "step_pipeline_support.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>

#include <TCollection_AsciiString.hxx>
#include <TDF_Tool.hxx>

namespace
{
std::string JsonEscape(const std::string& input)
{
    std::string out;
    out.reserve(input.size() + 8);
    for (const char ch : input)
    {
        switch (ch)
        {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string LabelToString(const TDF_Label& label)
{
    TCollection_AsciiString entry;
    TDF_Tool::Entry(label, entry);
    return std::string(entry.ToCString());
}

bool IsStepPath(const std::filesystem::path& p)
{
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return (ext == ".stp" || ext == ".step");
}
}

namespace importers
{
bool ValidateSceneIrInstanceIds(const core::SceneIR& sceneIr, bool verbose, bool requireLodUris)
{
    std::unordered_set<std::uint32_t> ids;
    ids.reserve(sceneIr.instances.size());
    for (const core::SceneInstance& instance : sceneIr.instances)
    {
        if (!ids.insert(instance.id).second)
        {
            std::cerr << "[SceneIR] duplicate instance id detected: " << instance.id << "\n";
            return false;
        }
    }

    std::size_t missing = 0;
    for (const core::SceneInstance& instance : sceneIr.instances)
    {
        if (instance.highLodGlbUri.empty() || instance.lowLodGlbUri.empty())
        {
            ++missing;
        }
    }

    if (verbose)
    {
        std::cout << "[SceneIR] instanceIdCheck unique=" << ids.size()
                  << " instances=" << sceneIr.instances.size()
                  << " missingLodUris=" << missing << "\n";
    }
    if (requireLodUris && missing > 0)
    {
        std::cerr << "[SceneIR] missing high/low LOD URI pairs for "
                  << missing << " instances\n";
        return false;
    }
    return true;
}

std::vector<std::filesystem::path> CollectStepFiles(const std::filesystem::path& input)
{
    std::vector<std::filesystem::path> files;

    if (std::filesystem::is_regular_file(input))
    {
        if (IsStepPath(input))
        {
            files.push_back(input);
        }
        return files;
    }

    if (std::filesystem::is_directory(input))
    {
        for (const std::filesystem::directory_entry& e : std::filesystem::directory_iterator(input))
        {
            if (!e.is_regular_file())
            {
                continue;
            }

            const std::filesystem::path p = e.path();
            if (IsStepPath(p))
            {
                files.push_back(p);
            }
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

void WriteStepFidelityArtifacts(
    const std::filesystem::path& artifactDir,
    const std::vector<Occurrence>& occurrences,
    const core::SceneIR& sceneIr)
{
    if (artifactDir.empty())
    {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(artifactDir, ec);
    if (ec)
    {
        std::cerr << "Warning: failed to create fidelity artifact directory: "
                  << artifactDir << "\n";
        return;
    }

    std::size_t shapeColorCount = 0;
    std::size_t shapeMaterialCount = 0;
    std::size_t faceColorCount = 0;
    std::size_t faceMaterialCount = 0;
    std::size_t metadataCount = 0;
    std::size_t sourceFaceCount = 0;
    for (const Occurrence& occ : occurrences)
    {
        sourceFaceCount += occ.SourceFaceCount;
        if (occ.HasAnyMetadata)
        {
            ++metadataCount;
        }
        if (!occ.Appearance)
        {
            continue;
        }
        const CachedColorSet& sc = occ.Appearance->ResolvedShapeColors;
        if (sc.HasGen || sc.HasSurf || sc.HasCurv)
        {
            ++shapeColorCount;
        }
        if (!occ.Appearance->ResolvedShapeMaterial.IsNull())
        {
            ++shapeMaterialCount;
        }
        for (const CachedFaceAppearance& fa : occ.Appearance->Faces)
        {
            if (fa.Colors.HasGen || fa.Colors.HasSurf || fa.Colors.HasCurv)
            {
                ++faceColorCount;
            }
            if (!fa.VisMaterial.IsNull())
            {
                ++faceMaterialCount;
            }
        }
    }

    std::ofstream importOut(artifactDir / "import_evidence.json");
    if (importOut)
    {
        importOut
            << "{\n"
            << "  \"occurrences\": " << occurrences.size() << ",\n"
            << "  \"shapeColorOccurrences\": " << shapeColorCount << ",\n"
            << "  \"shapeMaterialOccurrences\": " << shapeMaterialCount << ",\n"
            << "  \"faceColorEntries\": " << faceColorCount << ",\n"
            << "  \"faceMaterialEntries\": " << faceMaterialCount << ",\n"
            << "  \"sourceFaceCount\": " << sourceFaceCount << ",\n"
            << "  \"metadataOccurrences\": " << metadataCount << "\n"
            << "}\n";
    }

    std::ofstream sceneOut(artifactDir / "scene_ir_evidence.json");
    if (sceneOut)
    {
        sceneOut
            << "{\n"
            << "  \"instances\": " << sceneIr.instances.size() << ",\n"
            << "  \"prototypes\": " << sceneIr.prototypes.size() << ",\n"
            << "  \"shapeColorOccurrences\": " << sceneIr.shapeColorOccurrences << ",\n"
            << "  \"shapeMaterialOccurrences\": " << sceneIr.shapeMaterialOccurrences << ",\n"
            << "  \"faceColorEntries\": " << sceneIr.faceColorEntries << ",\n"
            << "  \"faceMaterialEntries\": " << sceneIr.faceMaterialEntries << ",\n"
            << "  \"metadataOccurrences\": " << sceneIr.metadataOccurrences << "\n"
            << "}\n";
    }

    std::ofstream occOut(artifactDir / "occurrence_evidence.jsonl");
    if (occOut)
    {
        for (const Occurrence& occ : occurrences)
        {
            occOut
                << "{\"label\":\"" << JsonEscape(LabelToString(occ.EffectiveLabel.IsNull() ? occ.Label : occ.EffectiveLabel))
                << "\",\"name\":\"" << JsonEscape(occ.SourceLabelName)
                << "\",\"sourceFaceCount\":" << occ.SourceFaceCount
                << ",\"hasMetadata\":" << (occ.HasAnyMetadata ? "true" : "false")
                << ",\"hasAppearance\":" << (occ.Appearance ? "true" : "false")
                << "}\n";
        }
    }
}
} // namespace importers
