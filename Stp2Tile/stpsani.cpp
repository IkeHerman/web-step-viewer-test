#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct GroupInfo
{
    int Id = 0;
    std::string Name;
    std::string Description;
};

enum class StepRecordKind
{
    Unknown,
    NonEntity,
    Group,
    AppliedGroupAssignment,
    GroupRelationship,
    ItemDefinedTransformation,
    ProductDefinitionShape,
    SurfaceStyleRenderingWithProperties,
    SurfaceSideStyle,
    ComplexTriangulatedFace,
    TessellatedSolid
};

struct StepRecord
{
    std::string OriginalText;
    std::string NormalizedText;

    int EntityId = 0;
    bool HasEntityId = false;
    StepRecordKind Kind = StepRecordKind::Unknown;

    std::unordered_set<std::string> EntityTypes;
    std::vector<int> ReferencedIds;

    bool Remove = false;
};

struct SanitizeStats
{
    int RemovedEmptyAssignments = 0;
    int PatchedGroupRelationships = 0;
    int PatchedItemDefinedTransformations = 0;
    int PatchedRepresentationRelationships = 0;
    int PatchedProductDefinitionShapes = 0;
    int RemovedComplexTriangulatedFaces = 0;
    int RemovedInvalidSurfaceStyleRenderingWithProperties = 0;

    int FilteredSurfaceSideStyles = 0;
    int RemovedSurfaceSideStyles = 0;
    int SkippedRemovedSurfaceSideStyleRefs = 0;
    int SkippedUnresolvedSurfaceSideStyleRefs = 0;
    int SkippedIllegalTypeSurfaceSideStyleRefs = 0;

    int FilteredTessellatedSolids = 0;
    int RemovedTessellatedSolids = 0;
    int SkippedRemovedTessellatedSolidRefs = 0;
    int SkippedUnresolvedTessellatedSolidRefs = 0;
    int SkippedIllegalTypeTessellatedSolidRefs = 0;

    int TotalEntitiesProcessed = 0;
    int TotalRemovedEntityIdsTracked = 0;

    int ColorEntityTypeHits = 0;
    int MaterialEntityTypeHits = 0;
    bool HasColorData = false;
    bool HasMaterialData = false;
};

static bool ContainsToken(const std::string& value, const char* token)
{
    return value.find(token) != std::string::npos;
}

static bool IsColorEntityTypeName(const std::string& typeName)
{
    return ContainsToken(typeName, "COLOUR") || ContainsToken(typeName, "COLOR");
}

static bool IsMaterialEntityTypeName(const std::string& typeName)
{
    return ContainsToken(typeName, "MATERIAL");
}

static std::string Trim(const std::string& value)
{
    size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
    {
        return "";
    }

    size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

static std::string CollapseWhitespace(const std::string& value)
{
    std::string result;
    result.reserve(value.size());

    bool previousWasWhitespace = false;
    for (char ch : value)
    {
        bool isWhitespace = (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n');

        if (isWhitespace)
        {
            if (!previousWasWhitespace)
            {
                result.push_back(' ');
                previousWasWhitespace = true;
            }
        }
        else
        {
            result.push_back(ch);
            previousWasWhitespace = false;
        }
    }

    return Trim(result);
}

static bool StartsStepEntity(const std::string& line)
{
    std::string trimmed = Trim(line);
    return !trimmed.empty() && trimmed[0] == '#';
}

static bool EndsStepEntity(const std::string& text)
{
    return text.find(';') != std::string::npos;
}

static bool IsStepEnumerationToken(const std::string& token)
{
    std::string trimmed = Trim(token);
    return trimmed.size() >= 3 &&
           trimmed.front() == '.' &&
           trimmed.back() == '.';
}

static bool TryGetEntityId(const std::string& normalizedEntity, int& outEntityId)
{
    static const std::regex idRegex(R"(^#(\d+)\s*=)");
    std::smatch match;
    if (!std::regex_search(normalizedEntity, match, idRegex))
    {
        return false;
    }

    outEntityId = std::stoi(match[1].str());
    return true;
}

static std::unordered_set<std::string> ExtractEntityTypeNames(const std::string& normalizedEntity)
{
    std::unordered_set<std::string> types;

    static const std::regex simpleTypeRegex(
        R"(^#\d+\s*=\s*([A-Z0-9_]+)\s*\()");

    std::smatch simpleMatch;
    if (std::regex_search(normalizedEntity, simpleMatch, simpleTypeRegex))
    {
        types.insert(simpleMatch[1].str());
        return types;
    }

    static const std::regex complexInnerRegex(
        R"(^#\d+\s*=\s*\((.*)\)\s*;\s*$)");

    std::smatch complexMatch;
    if (std::regex_match(normalizedEntity, complexMatch, complexInnerRegex))
    {
        std::string inner = complexMatch[1].str();

        static const std::regex typeInComplexRegex(
            R"(([A-Z0-9_]+)\s*\()");

        for (std::sregex_iterator it(inner.begin(), inner.end(), typeInComplexRegex), end;
             it != end;
             ++it)
        {
            types.insert((*it)[1].str());
        }
    }

    return types;
}

static std::vector<int> ParseEntityRefList(const std::string& listText)
{
    std::vector<int> refs;

    static const std::regex refRegex(R"(#(\d+))");
    for (std::sregex_iterator it(listText.begin(), listText.end(), refRegex), end;
         it != end;
         ++it)
    {
        refs.push_back(std::stoi((*it)[1].str()));
    }

    return refs;
}

static std::string JoinEntityRefs(const std::vector<int>& refs)
{
    std::string result;
    for (size_t i = 0; i < refs.size(); ++i)
    {
        if (i > 0)
        {
            result += ",";
        }

        result += "#";
        result += std::to_string(refs[i]);
    }

    return result;
}

static bool IsAllowedSurfaceSideStyleElementType(const std::unordered_set<std::string>& entityTypes)
{
    static const std::unordered_set<std::string> allowedTypes =
    {
        "SURFACE_STYLE_FILL_AREA",
        "SURFACE_STYLE_BOUNDARY",
        "SURFACE_STYLE_SILHOUETTE",
        "SURFACE_STYLE_SEGMENTATION_CURVE",
        "SURFACE_STYLE_CONTROL_GRID",
        "SURFACE_STYLE_PARAMETER_LINE",
        "SURFACE_STYLE_RENDERING",
        "SURFACE_STYLE_RENDERING_WITH_PROPERTIES"
    };

    for (const std::string& type : entityTypes)
    {
        if (allowedTypes.find(type) != allowedTypes.end())
        {
            return true;
        }
    }

    return false;
}

static bool IsAllowedTessellatedStructuredItemType(const std::unordered_set<std::string>& entityTypes)
{
    static const std::unordered_set<std::string> allowedTypes =
    {
        "TESSELLATED_VERTEX",
        "TESSELLATED_EDGE",
        "TESSELLATED_FACE",
        "TRIANGULATED_FACE",
        "COMPLEX_TRIANGULATED_FACE",
        "CUBIC_BEZIER_TRIANGULATED_FACE"
    };

    for (const std::string& type : entityTypes)
    {
        if (allowedTypes.find(type) != allowedTypes.end())
        {
            return true;
        }
    }

    return false;
}

static std::vector<StepRecord> ReadStepRecords(const std::string& inputPath, bool& outOk)
{
    outOk = false;

    std::ifstream in(inputPath);
    if (!in.is_open())
    {
        return {};
    }

    std::vector<StepRecord> records;
    std::string line;
    std::string currentEntity;
    bool accumulatingEntity = false;

    while (std::getline(in, line))
    {
        if (!accumulatingEntity)
        {
            if (StartsStepEntity(line))
            {
                currentEntity = line;
                accumulatingEntity = true;

                if (EndsStepEntity(currentEntity))
                {
                    StepRecord record;
                    record.OriginalText = currentEntity;
                    records.push_back(std::move(record));
                    currentEntity.clear();
                    accumulatingEntity = false;
                }
            }
            else
            {
                StepRecord record;
                record.OriginalText = line;
                record.Kind = StepRecordKind::NonEntity;
                records.push_back(std::move(record));
            }
        }
        else
        {
            currentEntity += "\n";
            currentEntity += line;

            if (EndsStepEntity(currentEntity))
            {
                StepRecord record;
                record.OriginalText = currentEntity;
                records.push_back(std::move(record));
                currentEntity.clear();
                accumulatingEntity = false;
            }
        }
    }

    if (accumulatingEntity)
    {
        StepRecord record;
        record.OriginalText = currentEntity;
        records.push_back(std::move(record));
    }

    outOk = true;
    return records;
}

static void ClassifyRecord(StepRecord& record)
{
    if (record.Kind == StepRecordKind::NonEntity)
    {
        return;
    }

    record.NormalizedText = CollapseWhitespace(record.OriginalText);
    record.HasEntityId = TryGetEntityId(record.NormalizedText, record.EntityId);

    if (!record.HasEntityId)
    {
        record.Kind = StepRecordKind::NonEntity;
        return;
    }

    record.EntityTypes = ExtractEntityTypeNames(record.NormalizedText);

    static const std::regex groupRegex(
        R"(^#\d+\s*=\s*GROUP\s*\()");
    static const std::regex appliedGroupAssignmentRegex(
        R"(^#\d+\s*=\s*APPLIED_GROUP_ASSIGNMENT\s*\()");
    static const std::regex groupRelationshipRegex(
        R"(^#\d+\s*=\s*GROUP_RELATIONSHIP\s*\()");
    static const std::regex itemDefinedTransformationRegex(
        R"(^#\d+\s*=\s*ITEM_DEFINED_TRANSFORMATION\s*\()");
    static const std::regex productDefinitionShapeRegex(
        R"(^#\d+\s*=\s*PRODUCT_DEFINITION_SHAPE\s*\()");
    static const std::regex surfaceStyleRenderingWithPropertiesRegex(
        R"(^#\d+\s*=\s*SURFACE_STYLE_RENDERING_WITH_PROPERTIES\s*\()");
    static const std::regex surfaceSideStyleRegex(
        R"(^#\d+\s*=\s*SURFACE_SIDE_STYLE\s*\()");
    static const std::regex complexTriangulatedFaceRegex(
        R"(^#\d+\s*=\s*COMPLEX_TRIANGULATED_FACE\s*\()");
    static const std::regex tessellatedSolidRegex(
        R"(^#\d+\s*=\s*TESSELLATED_SOLID\s*\()");

    if (std::regex_search(record.NormalizedText, groupRegex))
    {
        record.Kind = StepRecordKind::Group;
    }
    else if (std::regex_search(record.NormalizedText, appliedGroupAssignmentRegex))
    {
        record.Kind = StepRecordKind::AppliedGroupAssignment;
    }
    else if (std::regex_search(record.NormalizedText, groupRelationshipRegex))
    {
        record.Kind = StepRecordKind::GroupRelationship;
    }
    else if (std::regex_search(record.NormalizedText, itemDefinedTransformationRegex))
    {
        record.Kind = StepRecordKind::ItemDefinedTransformation;
    }
    else if (std::regex_search(record.NormalizedText, productDefinitionShapeRegex))
    {
        record.Kind = StepRecordKind::ProductDefinitionShape;
    }
    else if (std::regex_search(record.NormalizedText, surfaceStyleRenderingWithPropertiesRegex))
    {
        record.Kind = StepRecordKind::SurfaceStyleRenderingWithProperties;
    }
    else if (std::regex_search(record.NormalizedText, surfaceSideStyleRegex))
    {
        record.Kind = StepRecordKind::SurfaceSideStyle;
    }
    else if (std::regex_search(record.NormalizedText, complexTriangulatedFaceRegex))
    {
        record.Kind = StepRecordKind::ComplexTriangulatedFace;
    }
    else if (std::regex_search(record.NormalizedText, tessellatedSolidRegex))
    {
        record.Kind = StepRecordKind::TessellatedSolid;
    }
    else
    {
        record.Kind = StepRecordKind::Unknown;
    }

    if (record.Kind == StepRecordKind::SurfaceSideStyle)
    {
        static const std::regex surfaceSideStyleRegexFull(
            R"(^#\d+\s*=\s*SURFACE_SIDE_STYLE\s*\(\s*(\$|'(?:[^']|'')*')\s*,\s*\((.*)\)\s*\)\s*;\s*$)");
        std::smatch match;
        if (std::regex_match(record.NormalizedText, match, surfaceSideStyleRegexFull))
        {
            record.ReferencedIds = ParseEntityRefList(match[2].str());
        }
    }
    else if (record.Kind == StepRecordKind::TessellatedSolid)
    {
        static const std::regex tessellatedSolidRegexFull(
            R"(^#\d+\s*=\s*TESSELLATED_SOLID\s*\(\s*(\$|'(?:[^']|'')*')\s*,\s*\((.*)\)\s*\)\s*;\s*$)");
        std::smatch match;
        if (std::regex_match(record.NormalizedText, match, tessellatedSolidRegexFull))
        {
            record.ReferencedIds = ParseEntityRefList(match[2].str());
        }
    }
}

static std::vector<int> FilterSurfaceSideStyleRefs(
    const StepRecord& record,
    const std::unordered_set<int>& removedEntityIds,
    const std::unordered_map<int, std::unordered_set<std::string>>& entityTypesById,
    int* outSkippedRemoved,
    int* outSkippedUnresolved,
    int* outSkippedIllegal)
{
    std::vector<int> filtered;
    filtered.reserve(record.ReferencedIds.size());

    for (int refId : record.ReferencedIds)
    {
        if (removedEntityIds.find(refId) != removedEntityIds.end())
        {
            if (outSkippedRemoved != nullptr) { (*outSkippedRemoved)++; }
            continue;
        }

        auto found = entityTypesById.find(refId);
        if (found == entityTypesById.end())
        {
            if (outSkippedUnresolved != nullptr) { (*outSkippedUnresolved)++; }
            continue;
        }

        if (IsAllowedSurfaceSideStyleElementType(found->second))
        {
            filtered.push_back(refId);
        }
        else
        {
            if (outSkippedIllegal != nullptr) { (*outSkippedIllegal)++; }
        }
    }

    return filtered;
}

static std::vector<int> FilterTessellatedSolidRefs(
    const StepRecord& record,
    const std::unordered_set<int>& removedEntityIds,
    const std::unordered_map<int, std::unordered_set<std::string>>& entityTypesById,
    int* outSkippedRemoved,
    int* outSkippedUnresolved,
    int* outSkippedIllegal)
{
    std::vector<int> filtered;
    filtered.reserve(record.ReferencedIds.size());

    for (int refId : record.ReferencedIds)
    {
        if (removedEntityIds.find(refId) != removedEntityIds.end())
        {
            if (outSkippedRemoved != nullptr) { (*outSkippedRemoved)++; }
            continue;
        }

        auto found = entityTypesById.find(refId);
        if (found == entityTypesById.end())
        {
            if (outSkippedUnresolved != nullptr) { (*outSkippedUnresolved)++; }
            continue;
        }

        if (IsAllowedTessellatedStructuredItemType(found->second))
        {
            filtered.push_back(refId);
        }
        else
        {
            if (outSkippedIllegal != nullptr) { (*outSkippedIllegal)++; }
        }
    }

    return filtered;
}

bool SanitizeStepFile(const std::string& inputPath, const std::string& outputPath)
{
    bool readOk = false;
    std::vector<StepRecord> records = ReadStepRecords(inputPath, readOk);
    if (!readOk)
    {
        std::cerr << "[Sanitizer] Failed to open input STEP file\n";
        return false;
    }

    std::cout << "[Sanitizer] Starting STEP sanitization...\n";

    std::unordered_map<int, std::unordered_set<std::string>> entityTypesById;
    std::unordered_set<int> removedEntityIds;
    SanitizeStats stats;

    for (StepRecord& record : records)
    {
        ClassifyRecord(record);

        for (const std::string& typeName : record.EntityTypes)
        {
            if (IsColorEntityTypeName(typeName))
            {
                stats.ColorEntityTypeHits++;
                stats.HasColorData = true;
            }

            if (IsMaterialEntityTypeName(typeName))
            {
                stats.MaterialEntityTypeHits++;
                stats.HasMaterialData = true;
            }
        }

        if (record.HasEntityId)
        {
            entityTypesById[record.EntityId] = record.EntityTypes;
        }
    }
    stats.TotalEntitiesProcessed = static_cast<int>(records.size());

    // Pass 1: mark intrinsically bad leaf entities
    static const std::regex emptyAssignmentRegex(
        R"(^#(\d+)\s*=\s*APPLIED_GROUP_ASSIGNMENT\s*\(\s*#(\d+)\s*,\s*\$\s*\)\s*;\s*$)");

    static const std::regex complexTriangulatedFaceEmptyStripRegex(
        R"(^#(\d+)\s*=\s*COMPLEX_TRIANGULATED_FACE\s*\(.*,\s*\(\s*\)\s*,.*\)\s*;\s*$)");

    static const std::regex surfaceStyleRenderingWithPropertiesRegex(
        R"(^#(\d+)\s*=\s*SURFACE_STYLE_RENDERING_WITH_PROPERTIES\s*\(\s*([^,]+)\s*,(.*)\)\s*;\s*$)");

    for (StepRecord& record : records)
    {
        if (!record.HasEntityId)
        {
            continue;
        }

        std::smatch match;

        if (record.Kind == StepRecordKind::AppliedGroupAssignment)
        {
            if (std::regex_match(record.NormalizedText, match, emptyAssignmentRegex))
            {
                record.Remove = true;
                removedEntityIds.insert(record.EntityId);
                stats.RemovedEmptyAssignments++;
                continue;
            }
        }

        if (record.Kind == StepRecordKind::ComplexTriangulatedFace)
        {
            if (std::regex_match(record.NormalizedText, match, complexTriangulatedFaceEmptyStripRegex))
            {
                record.Remove = true;
                removedEntityIds.insert(record.EntityId);
                stats.RemovedComplexTriangulatedFaces++;
                continue;
            }
        }

        if (record.Kind == StepRecordKind::SurfaceStyleRenderingWithProperties)
        {
            if (std::regex_match(record.NormalizedText, match, surfaceStyleRenderingWithPropertiesRegex))
            {
                std::string renderingMethodToken = match[2].str();
                if (!IsStepEnumerationToken(renderingMethodToken))
                {
                    record.Remove = true;
                    removedEntityIds.insert(record.EntityId);
                    stats.RemovedInvalidSurfaceStyleRenderingWithProperties++;
                    continue;
                }
            }
        }
    }

    // Passes until stable: remove containers that become empty after filtering
    bool changed = true;
    while (changed)
    {
        changed = false;

        for (StepRecord& record : records)
        {
            if (!record.HasEntityId || record.Remove)
            {
                continue;
            }

            if (record.Kind == StepRecordKind::SurfaceSideStyle)
            {
                std::vector<int> filtered = FilterSurfaceSideStyleRefs(
                    record,
                    removedEntityIds,
                    entityTypesById,
                    nullptr,
                    nullptr,
                    nullptr);

                if (filtered.empty())
                {
                    record.Remove = true;
                    removedEntityIds.insert(record.EntityId);
                    changed = true;
                }
            }
            else if (record.Kind == StepRecordKind::TessellatedSolid)
            {
                std::vector<int> filtered = FilterTessellatedSolidRefs(
                    record,
                    removedEntityIds,
                    entityTypesById,
                    nullptr,
                    nullptr,
                    nullptr);

                if (filtered.empty())
                {
                    record.Remove = true;
                    removedEntityIds.insert(record.EntityId);
                    changed = true;
                }
            }
        }
    }

    std::ofstream out(outputPath);
    if (!out.is_open())
    {
        std::cerr << "[Sanitizer] Failed to open output STEP file\n";
        return false;
    }

    // Final emit pass
    static const std::regex groupRelationshipPatchRegex(
        R"(^(\s*#(\d+)\s*=\s*GROUP_RELATIONSHIP\s*\()\s*\$\s*,\s*\$\s*,(.*\)\s*;\s*)$)");

    static const std::regex itemDefinedTransformationPatchRegex(
        R"(^(\s*#(\d+)\s*=\s*ITEM_DEFINED_TRANSFORMATION\s*\()\s*\$\s*,\s*(.*\)\s*;\s*)$)");

    static const std::regex representationRelationshipNamePatchRegex(
        R"(REPRESENTATION_RELATIONSHIP\s*\(\s*\$\s*,)");

    static const std::regex productDefinitionShapePatchRegex(
        R"(^(\s*#(\d+)\s*=\s*PRODUCT_DEFINITION_SHAPE\s*\()\s*\$\s*,\s*(.*\)\s*;\s*)$)");

    static const std::regex surfaceSideStyleRegexFull(
        R"(^#(\d+)\s*=\s*SURFACE_SIDE_STYLE\s*\(\s*(\$|'(?:[^']|'')*')\s*,\s*\((.*)\)\s*\)\s*;\s*$)");

    static const std::regex tessellatedSolidRegexFull(
        R"(^#(\d+)\s*=\s*TESSELLATED_SOLID\s*\(\s*(\$|'(?:[^']|'')*')\s*,\s*\((.*)\)\s*\)\s*;\s*$)");

    for (const StepRecord& record : records)
    {
        if (record.Kind == StepRecordKind::NonEntity)
        {
            out << record.OriginalText << "\n";
            continue;
        }

        if (record.Remove)
        {
            continue;
        }

        std::smatch match;

        if (record.Kind == StepRecordKind::GroupRelationship)
        {
            if (std::regex_match(record.NormalizedText, match, groupRelationshipPatchRegex))
            {
                std::string patchedNormalized =
                    match[1].str() + "'',''," + match[3].str();

                out << patchedNormalized << "\n";
                stats.PatchedGroupRelationships++;
                continue;
            }
        }

        if (record.Kind == StepRecordKind::ItemDefinedTransformation)
        {
            if (std::regex_match(record.NormalizedText, match, itemDefinedTransformationPatchRegex))
            {
                std::string patchedNormalized =
                    match[1].str() + "''," + match[3].str();

                out << patchedNormalized << "\n";
                stats.PatchedItemDefinedTransformations++;
                continue;
            }
        }

        if (record.NormalizedText.find("REPRESENTATION_RELATIONSHIP") != std::string::npos &&
            record.NormalizedText.find("($,") != std::string::npos)
        {
            std::string patchedNormalized = std::regex_replace(
                record.NormalizedText,
                representationRelationshipNamePatchRegex,
                "REPRESENTATION_RELATIONSHIP('',");

            if (patchedNormalized != record.NormalizedText)
            {
                out << patchedNormalized << "\n";
                stats.PatchedRepresentationRelationships++;
                continue;
            }
        }

        if (record.Kind == StepRecordKind::ProductDefinitionShape)
        {
            if (std::regex_match(record.NormalizedText, match, productDefinitionShapePatchRegex))
            {
                std::string patchedNormalized =
                    match[1].str() + "''," + match[3].str();

                out << patchedNormalized << "\n";
                stats.PatchedProductDefinitionShapes++;
                continue;
            }
        }

        if (record.Kind == StepRecordKind::SurfaceSideStyle)
        {
            if (std::regex_match(record.NormalizedText, match, surfaceSideStyleRegexFull))
            {
                std::string nameToken = match[1].str();

                int skippedRemoved = 0;
                int skippedUnresolved = 0;
                int skippedIllegal = 0;

                std::vector<int> filteredRefs = FilterSurfaceSideStyleRefs(
                    record,
                    removedEntityIds,
                    entityTypesById,
                    &skippedRemoved,
                    &skippedUnresolved,
                    &skippedIllegal);

                stats.SkippedRemovedSurfaceSideStyleRefs += skippedRemoved;
                stats.SkippedUnresolvedSurfaceSideStyleRefs += skippedUnresolved;
                stats.SkippedIllegalTypeSurfaceSideStyleRefs += skippedIllegal;

                if (filteredRefs.size() != record.ReferencedIds.size())
                {
                    std::string rewritten =
                        "#" + std::to_string(record.EntityId) +
                        "=SURFACE_SIDE_STYLE(" +
                        nameToken +
                        ",(" +
                        JoinEntityRefs(filteredRefs) +
                        "));";

                    out << rewritten << "\n";
                    stats.FilteredSurfaceSideStyles++;
                }
                else
                {
                    out << record.NormalizedText << "\n";
                }

                continue;
            }
        }

        if (record.Kind == StepRecordKind::TessellatedSolid)
        {
            if (std::regex_match(record.NormalizedText, match, tessellatedSolidRegexFull))
            {
                std::string nameToken = match[1].str();

                int skippedRemoved = 0;
                int skippedUnresolved = 0;
                int skippedIllegal = 0;

                std::vector<int> filteredRefs = FilterTessellatedSolidRefs(
                    record,
                    removedEntityIds,
                    entityTypesById,
                    &skippedRemoved,
                    &skippedUnresolved,
                    &skippedIllegal);

                stats.SkippedRemovedTessellatedSolidRefs += skippedRemoved;
                stats.SkippedUnresolvedTessellatedSolidRefs += skippedUnresolved;
                stats.SkippedIllegalTypeTessellatedSolidRefs += skippedIllegal;

                if (filteredRefs.size() != record.ReferencedIds.size())
                {
                    std::string rewritten =
                        "#" + std::to_string(record.EntityId) +
                        "=TESSELLATED_SOLID(" +
                        nameToken +
                        ",(" +
                        JoinEntityRefs(filteredRefs) +
                        "));";

                    out << rewritten << "\n";
                    stats.FilteredTessellatedSolids++;
                }
                else
                {
                    out << record.NormalizedText << "\n";
                }

                continue;
            }
        }

        out << record.OriginalText << "\n";
    }

    // Count removed container types after stabilization
    for (const StepRecord& record : records)
    {
        if (!record.Remove)
        {
            continue;
        }

        if (record.Kind == StepRecordKind::SurfaceSideStyle)
        {
            stats.RemovedSurfaceSideStyles++;
        }
        else if (record.Kind == StepRecordKind::TessellatedSolid)
        {
            stats.RemovedTessellatedSolids++;
        }
    }

    stats.TotalRemovedEntityIdsTracked = static_cast<int>(removedEntityIds.size());

    std::cout << "\n[SANITIZER SUMMARY]\n";
    std::cout << "  Removed empty APPLIED_GROUP_ASSIGNMENT:                " << stats.RemovedEmptyAssignments << "\n";
    std::cout << "  Patched GROUP_RELATIONSHIP strings:                    " << stats.PatchedGroupRelationships << "\n";
    std::cout << "  Patched ITEM_DEFINED_TRANSFORMATION name:              " << stats.PatchedItemDefinedTransformations << "\n";
    std::cout << "  Patched REPRESENTATION_RELATIONSHIP name:              " << stats.PatchedRepresentationRelationships << "\n";
    std::cout << "  Patched PRODUCT_DEFINITION_SHAPE name:                 " << stats.PatchedProductDefinitionShapes << "\n";
    std::cout << "  Removed COMPLEX_TRIANGULATED_FACE with empty triangle_strips: "
              << stats.RemovedComplexTriangulatedFaces << "\n";
    std::cout << "  Removed invalid SURFACE_STYLE_RENDERING_WITH_PROPERTIES: "
              << stats.RemovedInvalidSurfaceStyleRenderingWithProperties << "\n";
    std::cout << "  Filtered SURFACE_SIDE_STYLE style refs:                " << stats.FilteredSurfaceSideStyles << "\n";
    std::cout << "  Removed SURFACE_SIDE_STYLE with no valid refs:         " << stats.RemovedSurfaceSideStyles << "\n";
    std::cout << "  Skipped removed SURFACE_SIDE_STYLE refs:               " << stats.SkippedRemovedSurfaceSideStyleRefs << "\n";
    std::cout << "  Skipped unresolved SURFACE_SIDE_STYLE refs:            " << stats.SkippedUnresolvedSurfaceSideStyleRefs << "\n";
    std::cout << "  Skipped illegal-type SURFACE_SIDE_STYLE refs:          " << stats.SkippedIllegalTypeSurfaceSideStyleRefs << "\n";
    std::cout << "  Filtered TESSELLATED_SOLID item refs:                  " << stats.FilteredTessellatedSolids << "\n";
    std::cout << "  Removed TESSELLATED_SOLID with no valid refs:          " << stats.RemovedTessellatedSolids << "\n";
    std::cout << "  Skipped removed TESSELLATED_SOLID refs:                " << stats.SkippedRemovedTessellatedSolidRefs << "\n";
    std::cout << "  Skipped unresolved TESSELLATED_SOLID refs:             " << stats.SkippedUnresolvedTessellatedSolidRefs << "\n";
    std::cout << "  Skipped illegal-type TESSELLATED_SOLID refs:           " << stats.SkippedIllegalTypeTessellatedSolidRefs << "\n";
    std::cout << "  Total removed entity ids tracked:                      " << stats.TotalRemovedEntityIdsTracked << "\n";
    std::cout << "  Total STEP records processed:                          " << stats.TotalEntitiesProcessed << "\n\n";

    std::cout << "[STEP APPEARANCE VALIDATION]\n";
    std::cout << "  Color-related entity types found:                      "
              << (stats.HasColorData ? "yes" : "no")
              << " (hits=" << stats.ColorEntityTypeHits << ")\n";
    std::cout << "  Material-related entity types found:                   "
              << (stats.HasMaterialData ? "yes" : "no")
              << " (hits=" << stats.MaterialEntityTypeHits << ")\n";

    if (!stats.HasColorData && !stats.HasMaterialData)
    {
        std::cout << "  Warning: no color/material entity types detected in STEP input.\n";
    }

    std::cout << "\n";

    return true;
}