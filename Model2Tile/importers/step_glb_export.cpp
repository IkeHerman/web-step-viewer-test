#include "step_glb_export.h"

#include <fstream>
#include <stdexcept>
#include <iostream>

#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_VisMaterialTool.hxx>

#include <BRep_Builder.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>
#include <TopLoc_Location.hxx>

#include <RWGltf_CafWriter.hxx>
#include <RWGltf_WriterTrsfFormat.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Face.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TDF_Tool.hxx>
#include <BRepTools.hxx>

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <TopoDS_Shape.hxx>
#include <Quantity_Color.hxx>
#include <cmath>
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>

#include "../dep/tinygltf/tiny_gltf.h"
#include "../tiler/node_scale_helpers.h"

namespace
{
std::filesystem::path g_fidelityArtifactDirectory;

/// Halve OCCT linear/angular deflections => ~2x denser meshes (temporary default).
constexpr double kTessellationDensityBoost = 0.5;

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

std::string GlbFilenameForStepInstanceLog(const std::string& glbPath)
{
    return std::filesystem::path(glbPath).filename().string();
}

void AppendFidelityJsonLine(const std::string& fileName, const std::string& payload)
{
    if (g_fidelityArtifactDirectory.empty())
    {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(g_fidelityArtifactDirectory, ec);
    std::ofstream out(g_fidelityArtifactDirectory / fileName, std::ios::app);
    if (!out)
    {
        return;
    }
    out << payload << "\n";
}
}

void ConfigureFidelityArtifactOutput(const std::string& directoryPath)
{
    if (directoryPath.empty())
    {
        g_fidelityArtifactDirectory.clear();
        return;
    }
    g_fidelityArtifactDirectory = std::filesystem::path(directoryPath);
}

Handle(TDocStd_Document) CreateEmptyXcafDocument()
{
    Handle(XCAFApp_Application) app = XCAFApp_Application::GetApplication();

    Handle(TDocStd_Document) doc;
    app->NewDocument("MDTV-XCAF", doc);

    return doc;
}

std::vector<std::uint8_t> ReadFileBytes(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};

    f.seekg(0, std::ios::end);
    std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);

    if (size <= 0)
        return {};

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    f.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

static Handle(XCAFDoc_ShapeTool) GetShapeTool(const Handle(TDocStd_Document)& doc)
{
    return XCAFDoc_DocumentTool::ShapeTool(doc->Main());
}

static Handle(XCAFDoc_ColorTool) GetColorTool(const Handle(TDocStd_Document)& doc)
{
    return XCAFDoc_DocumentTool::ColorTool(doc->Main());
}

static Handle(XCAFDoc_VisMaterialTool) GetVisMaterialTool(const Handle(TDocStd_Document)& doc)
{
    return XCAFDoc_DocumentTool::VisMaterialTool(doc->Main());
}

static void ApplyColorsToLabelOrShape(
    const Handle(XCAFDoc_ColorTool)& colorTool,
    const TDF_Label& targetLabel,
    const TopoDS_Shape& targetShape,
    const CachedColorSet& colors)
{
    if (colorTool.IsNull())
    {
        return;
    }

    if (colors.HasSurf)
    {
        if (!targetLabel.IsNull())
        {
            colorTool->SetColor(targetLabel, colors.Surf, XCAFDoc_ColorSurf);
        }
        if (!targetShape.IsNull())
        {
            colorTool->SetColor(targetShape, colors.Surf, XCAFDoc_ColorSurf);
        }
    }
    else if (colors.HasGen)
    {
        if (!targetLabel.IsNull())
        {
            colorTool->SetColor(targetLabel, colors.Gen, XCAFDoc_ColorGen);
        }
        if (!targetShape.IsNull())
        {
            colorTool->SetColor(targetShape, colors.Gen, XCAFDoc_ColorGen);
        }
    }
}

static void ApplyVisMaterialToLabelOrShape(
    const Handle(XCAFDoc_VisMaterialTool)& destinationVisMatTool,
    const TDF_Label& targetLabel,
    const TopoDS_Shape& targetShape,
    const Handle(XCAFDoc_VisMaterial)& sourceMaterial)
{
    if (destinationVisMatTool.IsNull() || sourceMaterial.IsNull())
    {
        return;
    }

    static std::size_t nextMaterialId = 0;
    const std::string materialName = "mat_uncached_" + std::to_string(nextMaterialId++);
    const TDF_Label destinationMaterialLabel = destinationVisMatTool->AddMaterial(
        sourceMaterial,
        TCollection_AsciiString(materialName.c_str()));

    if (destinationMaterialLabel.IsNull())
    {
        return;
    }

    if (!targetLabel.IsNull())
    {
        destinationVisMatTool->SetShapeMaterial(targetLabel, destinationMaterialLabel);
    }
    if (!targetShape.IsNull())
    {
        destinationVisMatTool->SetShapeMaterial(targetShape, destinationMaterialLabel);
    }
}

static bool HasAnyFaceAppearanceData(const CachedOccurrenceAppearance& appearance)
{
    for (const CachedFaceAppearance& face : appearance.Faces)
    {
        if (face.Colors.HasGen || face.Colors.HasSurf || face.Colors.HasCurv || !face.VisMaterial.IsNull())
        {
            return true;
        }
    }

    return false;
}

static std::size_t CountTriangles(const TopoDS_Shape& shape)
{
    std::size_t triangleCount = 0;

    for (TopExp_Explorer exp(shape, TopAbs_FACE); exp.More(); exp.Next())
    {
        const TopoDS_Face& face = TopoDS::Face(exp.Current());

        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);

        if (tri.IsNull())
            continue;

        triangleCount += static_cast<std::size_t>(tri->NbTriangles());
    }

    return triangleCount;
}

static std::size_t CountTrianglesInGltfModel(const tinygltf::Model& gltfModel)
{
    std::size_t triangleCount = 0;
    for (const tinygltf::Mesh& mesh : gltfModel.meshes)
    {
        for (const tinygltf::Primitive& primitive : mesh.primitives)
        {
            const int mode = (primitive.mode == -1) ? TINYGLTF_MODE_TRIANGLES : primitive.mode;
            if (mode != TINYGLTF_MODE_TRIANGLES)
            {
                continue;
            }

            if (primitive.indices >= 0 &&
                static_cast<std::size_t>(primitive.indices) < gltfModel.accessors.size())
            {
                const tinygltf::Accessor& indexAccessor =
                    gltfModel.accessors[static_cast<std::size_t>(primitive.indices)];
                triangleCount += static_cast<std::size_t>(indexAccessor.count / 3);
                continue;
            }

            const auto positionIt = primitive.attributes.find("POSITION");
            if (positionIt != primitive.attributes.end())
            {
                const int accessorIndex = positionIt->second;
                if (accessorIndex >= 0 &&
                    static_cast<std::size_t>(accessorIndex) < gltfModel.accessors.size())
                {
                    const tinygltf::Accessor& posAccessor =
                        gltfModel.accessors[static_cast<std::size_t>(accessorIndex)];
                    triangleCount += static_cast<std::size_t>(posAccessor.count / 3);
                }
            }
        }
    }
    return triangleCount;
}

ExportResolvedTessellation ResolveExportTessellation(
    const ExportTessellationPolicy& policy)
{
    ExportResolvedTessellation out;
    out.chosenSse = std::max(1.0, policy.viewerTargetSse);

    const double diag = std::max(1e-9, policy.nodeBoundsDiagonal);
    const double geomErr = std::max(0.0, policy.tileGeometricError);
    const double relativeError = geomErr / std::max(1e-9, diag);
    const double classBias = (policy.tileClass == ExportTileClass::Proxy) ? 2.4 : 0.55;
    const double sseScale = model2tile::ClampViewerSseScaleTileDefault(policy.viewerTargetSse);

    double linear = diag * std::max(5e-4, relativeError * classBias * policy.qualityBias * sseScale);
    if (geomErr <= 1e-12)
    {
        linear = diag * ((policy.tileClass == ExportTileClass::Proxy) ? 3.0e-2 : 2.0e-3);
    }
    const double linearMinAbs = std::max(1e-12, diag * std::max(1e-9, policy.linearMinFraction));
    const double linearMaxAbs = std::max(linearMinAbs, diag * std::max(policy.linearMinFraction, policy.linearMaxFraction));
    linear = std::clamp(linear, linearMinAbs, linearMaxAbs);

    double angular = (policy.tileClass == ExportTileClass::Proxy) ? 1.6 : 0.5;
    angular += std::clamp(std::log10(std::max(1e-6, linear / std::max(1e-9, diag))), -2.0, 1.0) * 0.7;
    angular = std::clamp(angular, policy.angularMinDeg, policy.angularMaxDeg);

    out.linearDeflection = std::max(1e-15, linear * kTessellationDensityBoost);
    out.angularDeflectionDeg = std::max(1e-6, angular * kTessellationDensityBoost);
    return out;
}

ExportTessellationPolicy MakeInstanceHighTessellationPolicy(
    const double viewerTargetSse,
    const double occurrenceBoundsDiagonal)
{
    const double d = std::max(1e-9, occurrenceBoundsDiagonal);
    ExportTessellationPolicy p;
    p.viewerTargetSse = viewerTargetSse;
    p.tileGeometricError = 0.0;
    p.nodeBoundsDiagonal = d;
    p.linearMinFraction = 2.0e-4;
    p.linearMaxFraction = 1.5e-2;
    p.tileClass = ExportTileClass::Leaf;
    p.qualityBias = 0.75;
    p.angularMinDeg = 3.0;
    p.angularMaxDeg = 9.0;
    return p;
}

ExportTessellationPolicy MakeInstanceLowTessellationPolicy(
    const double viewerTargetSse,
    const double occurrenceBoundsDiagonal)
{
    const double nodeDiag = std::max(0.0, occurrenceBoundsDiagonal);
    ExportTessellationPolicy p;
    p.viewerTargetSse = viewerTargetSse;
    p.nodeBoundsDiagonal = std::max(1e-9, nodeDiag);
    p.linearMinFraction = 5.0e-3;
    p.linearMaxFraction = 8.5e-1;
    p.tileClass = ExportTileClass::Proxy;
    p.qualityBias = 4.0;
    p.angularMinDeg = 35.0;
    p.angularMaxDeg = 120.0;

    constexpr double kGeomErrFraction = 0.65;
    constexpr double kGeomErrMinFraction = 0.25;
    constexpr double kGeomErrMaxFraction = 0.95;
    const double sseScale = model2tile::ClampViewerSseScale(viewerTargetSse, 0.75, 3.0);
    p.tileGeometricError = model2tile::ClampDiagonalGeometricError(
        nodeDiag,
        sseScale,
        kGeomErrFraction,
        kGeomErrMinFraction,
        kGeomErrMaxFraction);

    return p;
}

bool ExportTileToGlbFile(
    const std::vector<Occurrence>& occurrences,
    const std::vector<std::uint32_t>& itemIndices,
    const std::string& glbPath,
    const ExportTessellationPolicy& tessellationPolicy,
    const bool placeShapeInWorldSpace)
{
    Handle(TDocStd_Document) tileDoc = CreateEmptyXcafDocument();
    Handle(XCAFDoc_ShapeTool) tileShapeTool = GetShapeTool(tileDoc);
    Handle(XCAFDoc_ColorTool) tileColorTool = GetColorTool(tileDoc);
    Handle(XCAFDoc_VisMaterialTool) tileVisMatTool = GetVisMaterialTool(tileDoc);

    std::vector<TDF_Label> exportedLabels;
    exportedLabels.reserve(itemIndices.size());

    std::size_t dbgShapeColorApplied = 0;
    std::size_t dbgShapeMaterialApplied = 0;
    std::size_t dbgFaceColorApplied = 0;
    std::size_t dbgFaceMaterialApplied = 0;
    std::size_t dbgOccurrencesWithFaceLoop = 0;
    std::size_t dbgFaceMappingMismatches = 0;

    std::size_t addedCount = 0;
    for (std::uint32_t idx : itemIndices)
    {
        if (idx >= occurrences.size())
            continue;

        const Occurrence& occ = occurrences[static_cast<std::size_t>(idx)];
        if (!occ.Appearance)
        {
            continue;
        }
        const CachedOccurrenceAppearance& appearance = *occ.Appearance;

        const TopoDS_Shape placed = placeShapeInWorldSpace
            ? occ.Shape.Located(TopLoc_Location(occ.WorldTransform))
            : occ.Shape;

        const TDF_Label exportedLabel = tileShapeTool->AddShape(placed, Standard_False);
        const TopoDS_Shape exportedShape = tileShapeTool->GetShape(exportedLabel);

        ApplyColorsToLabelOrShape(
            tileColorTool,
            exportedLabel,
            exportedShape,
            appearance.ResolvedShapeColors);
        if (appearance.ResolvedShapeColors.HasGen ||
            appearance.ResolvedShapeColors.HasSurf ||
            appearance.ResolvedShapeColors.HasCurv)
        {
            ++dbgShapeColorApplied;
        }

        ApplyVisMaterialToLabelOrShape(
            tileVisMatTool,
            exportedLabel,
            exportedShape,
            appearance.ResolvedShapeMaterial);
        if (!appearance.ResolvedShapeMaterial.IsNull())
        {
            ++dbgShapeMaterialApplied;
        }

        if (!appearance.Faces.empty() && HasAnyFaceAppearanceData(appearance))
        {
            ++dbgOccurrencesWithFaceLoop;
            TopExp_Explorer destinationFaceExplorer(exportedShape, TopAbs_FACE);
            std::size_t faceIndex = 0;
            for (; destinationFaceExplorer.More() && faceIndex < appearance.Faces.size();
                 destinationFaceExplorer.Next(), ++faceIndex)
            {
                const TopoDS_Face destinationFace = TopoDS::Face(destinationFaceExplorer.Current());
                const CachedFaceAppearance& faceAppearance = appearance.Faces[faceIndex];

                ApplyColorsToLabelOrShape(
                    tileColorTool,
                    TDF_Label(),
                    destinationFace,
                    faceAppearance.Colors);
                if (faceAppearance.Colors.HasGen || faceAppearance.Colors.HasSurf || faceAppearance.Colors.HasCurv)
                {
                    ++dbgFaceColorApplied;
                }

                ApplyVisMaterialToLabelOrShape(
                    tileVisMatTool,
                    TDF_Label(),
                    destinationFace,
                    faceAppearance.VisMaterial);
                if (!faceAppearance.VisMaterial.IsNull())
                {
                    ++dbgFaceMaterialApplied;
                }
            }
            std::size_t destinationFaceCount = 0;
            for (TopExp_Explorer countFaces(exportedShape, TopAbs_FACE); countFaces.More(); countFaces.Next())
            {
                ++destinationFaceCount;
            }
            if (destinationFaceCount != appearance.Faces.size())
            {
                ++dbgFaceMappingMismatches;
            }
        }

        exportedLabels.push_back(exportedLabel);
        ++addedCount;
    }

    if (addedCount == 0 || exportedLabels.empty())
    {
        std::cerr << "[ExportStepPrototype] no shapes to export "
                  << GlbFilenameForStepInstanceLog(glbPath) << "\n";
        return false;
    }

    const ExportResolvedTessellation resolved = ResolveExportTessellation(tessellationPolicy);

    std::size_t triCount = 0;
    for (const TDF_Label& exportedLabel : exportedLabels)
    {
        const TopoDS_Shape exportedShape = tileShapeTool->GetShape(exportedLabel);
        if (exportedShape.IsNull())
        {
            continue;
        }

        // Force fresh tessellation for this export policy; otherwise prior
        // triangulation can be reused and high/low LODs collapse to similar output.
        BRepTools::Clean(exportedShape);

        BRepMesh_IncrementalMesh mesh(
            exportedShape,
            resolved.linearDeflection,
            Standard_False,
            resolved.angularDeflectionDeg,
            Standard_True
        );
        mesh.Perform();

        triCount += CountTriangles(exportedShape);
    }

    RWGltf_CafWriter writer(TCollection_AsciiString(glbPath.c_str()), Standard_True);
    writer.SetTransformationFormat(RWGltf_WriterTrsfFormat_Compact);

    TDF_LabelSequence rootLabels;
    for (const TDF_Label& exportedLabel : exportedLabels)
    {
        rootLabels.Append(exportedLabel);
    }

    const TColStd_MapOfAsciiString* labelFilter = nullptr;
    TColStd_IndexedDataMapOfStringString fileInfo;
    Message_ProgressRange progress;

    const bool ok = writer.Perform(tileDoc, rootLabels, labelFilter, fileInfo, progress);

    std::size_t exportedTriangleCount = 0;
    std::size_t exportedMaterialCount = 0;
    std::size_t exportedAlphaBlendCount = 0;
    std::size_t exportedAlphaMaskCount = 0;
    if (ok)
    {
        tinygltf::TinyGLTF loader;
        tinygltf::Model gltfModel;
        std::string warn;
        std::string err;
        const bool loaded = loader.LoadBinaryFromFile(&gltfModel, &err, &warn, glbPath);
        if (loaded)
        {
            exportedTriangleCount = CountTrianglesInGltfModel(gltfModel);
            exportedMaterialCount = gltfModel.materials.size();
            for (const tinygltf::Material& material : gltfModel.materials)
            {
                if (material.alphaMode == "BLEND")
                {
                    ++exportedAlphaBlendCount;
                }
                else if (material.alphaMode == "MASK")
                {
                    ++exportedAlphaMaskCount;
                }
            }
        }
    }

    AppendFidelityJsonLine(
        "export_evidence.jsonl",
        std::string("{\"glb\":\"") + JsonEscape(glbPath) +
        "\",\"occurrences\":" + std::to_string(addedCount) +
        ",\"sourceTriangles\":" + std::to_string(triCount) +
        ",\"glbTriangles\":" + std::to_string(exportedTriangleCount) +
        ",\"shapeColorApplied\":" + std::to_string(dbgShapeColorApplied) +
        ",\"shapeMaterialApplied\":" + std::to_string(dbgShapeMaterialApplied) +
        ",\"occWithFaceLoop\":" + std::to_string(dbgOccurrencesWithFaceLoop) +
        ",\"faceColorApplied\":" + std::to_string(dbgFaceColorApplied) +
        ",\"faceMaterialApplied\":" + std::to_string(dbgFaceMaterialApplied) +
        ",\"faceMappingMismatches\":" + std::to_string(dbgFaceMappingMismatches) +
        ",\"exportedMaterials\":" + std::to_string(exportedMaterialCount) +
        ",\"alphaBlendMaterials\":" + std::to_string(exportedAlphaBlendCount) +
        ",\"alphaMaskMaterials\":" + std::to_string(exportedAlphaMaskCount) +
        ",\"chosenSSE\":" + std::to_string(resolved.chosenSse) +
        ",\"linearDeflection\":" + std::to_string(resolved.linearDeflection) +
        ",\"angularDeflectionDeg\":" + std::to_string(resolved.angularDeflectionDeg) +
        ",\"status\":\"" + (ok ? std::string("ok") : std::string("error")) + "\"}");

    if (ok)
    {
        const std::size_t triangles =
            exportedTriangleCount > 0 ? exportedTriangleCount : triCount;
        std::cout << "[ExportStepPrototype] wrote glb "
                  << GlbFilenameForStepInstanceLog(glbPath)
                  << " Triangles=" << triangles
                  << " status=ok\n";
    }
    else
    {
        std::cerr << "[ExportStepPrototype] failed glb "
                  << GlbFilenameForStepInstanceLog(glbPath)
                  << " status=error\n";
    }

    return ok;
}

bool ExportBoxToGlbFile(
    const Bnd_Box& bounds,
    const std::string& glbPath)
{
    if (bounds.IsVoid())
    {
        std::cerr << "[ExportBox] invalid bounds path=" << glbPath << " status=void\n";
        return false;
    }

    double xmin = 0.0;
    double ymin = 0.0;
    double zmin = 0.0;
    double xmax = 0.0;
    double ymax = 0.0;
    double zmax = 0.0;
    bounds.Get(xmin, ymin, zmin, xmax, ymax, zmax);

    constexpr double kMinExtent = 1e-6;
    if ((xmax - xmin) < kMinExtent)
    {
        const double c = 0.5 * (xmin + xmax);
        xmin = c - 0.5 * kMinExtent;
        xmax = c + 0.5 * kMinExtent;
    }
    if ((ymax - ymin) < kMinExtent)
    {
        const double c = 0.5 * (ymin + ymax);
        ymin = c - 0.5 * kMinExtent;
        ymax = c + 0.5 * kMinExtent;
    }
    if ((zmax - zmin) < kMinExtent)
    {
        const double c = 0.5 * (zmin + zmax);
        zmin = c - 0.5 * kMinExtent;
        zmax = c + 0.5 * kMinExtent;
    }

    Handle(TDocStd_Document) tileDoc = CreateEmptyXcafDocument();
    Handle(XCAFDoc_ShapeTool) tileShapeTool = GetShapeTool(tileDoc);

    const TopoDS_Shape boxShape = BRepPrimAPI_MakeBox(
        gp_Pnt(xmin, ymin, zmin),
        gp_Pnt(xmax, ymax, zmax)).Shape();

    const TDF_Label boxLabel = tileShapeTool->AddShape(boxShape, Standard_False);

    BRepMesh_IncrementalMesh mesh(
        boxShape,
        0.25 * kTessellationDensityBoost,
        Standard_False,
        8.0 * kTessellationDensityBoost,
        Standard_True);
    mesh.Perform();

    RWGltf_CafWriter writer(TCollection_AsciiString(glbPath.c_str()), Standard_True);
    writer.SetTransformationFormat(RWGltf_WriterTrsfFormat_Compact);

    TDF_LabelSequence rootLabels;
    rootLabels.Append(boxLabel);

    const TColStd_MapOfAsciiString* labelFilter = nullptr;
    TColStd_IndexedDataMapOfStringString fileInfo;
    Message_ProgressRange progress;

    const bool ok = writer.Perform(tileDoc, rootLabels, labelFilter, fileInfo, progress);
    if (ok)
    {
        std::cout << "[ExportBox] wrote glb path=" << glbPath << " status=ok\n";
    }
    else
    {
        std::cerr << "[ExportBox] failed glb path=" << glbPath << " status=error\n";
    }

    return ok;
}
