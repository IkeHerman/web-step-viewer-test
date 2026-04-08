#include "export_glb.h"

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

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <TopoDS_Shape.hxx>
#include <Quantity_Color.hxx>
#include <cmath>
#include <algorithm>
#include <cstddef>
#include <TopoDS.hxx>

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

    // For shaded mesh export, prioritize surface color and then general color.
    // Curve color is typically intended for wireframe edges and can mute shading.
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

static float LerpClamped(float a, float b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return a + (b - a) * t;
}

static float ClampDecimation(float decimationFactor)
{
    return std::clamp(decimationFactor, 0.0f, 1.0f);
}

bool ExportTileToGlbFile(
    const std::vector<Occurrence>& occurrences,
    const std::vector<std::uint32_t>& itemIndices,
    const std::string& glbPath,
    const float decimationFactor,
    const bool debugAppearance)
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

        TopLoc_Location loc(occ.WorldTransform);
        TopoDS_Shape placed = occ.Shape.Located(loc);

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
        }

        exportedLabels.push_back(exportedLabel);
        ++addedCount;
    }

    if (addedCount == 0 || exportedLabels.empty())
    {
        std::cerr << "[ExportTile] no shapes to export path=" << glbPath << "\n";
        return false;
    }

    const float t = ClampDecimation(decimationFactor);

    // Do the simplification here by changing OCCT tessellation quality.
    // Lower t => finer mesh. Higher t => coarser proxy mesh.
    const float linearDeflection = LerpClamped(0.05f, 25.0f, t);
    const float angularDeflectionDeg = LerpClamped(0.75f, 12.0f, t);

    std::size_t triCount = 0;
    for (const TDF_Label& exportedLabel : exportedLabels)
    {
        const TopoDS_Shape exportedShape = tileShapeTool->GetShape(exportedLabel);
        if (exportedShape.IsNull())
        {
            continue;
        }

        BRepMesh_IncrementalMesh mesh(
            exportedShape,
            linearDeflection,
            Standard_False,
            angularDeflectionDeg,
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

    if (debugAppearance)
    {
        std::cout << "[AppearanceProbe:Export] glb=" << glbPath
                  << " occ=" << addedCount
                  << " shapeColorApplied=" << dbgShapeColorApplied
                  << " shapeMaterialApplied=" << dbgShapeMaterialApplied
                  << " occWithFaceLoop=" << dbgOccurrencesWithFaceLoop
                  << " faceColorApplied=" << dbgFaceColorApplied
                  << " faceMaterialApplied=" << dbgFaceMaterialApplied
                  << "\n";
    }

    if (ok)
    {
        std::cout << "[ExportTile] wrote glb path=" << glbPath
                  << " items=" << itemIndices.size()
                  << " triangles=" << triCount
                  << " status=ok\n";
    }
    else
    {
        std::cerr << "[ExportTile] failed glb path=" << glbPath
                  << " items=" << itemIndices.size()
                  << " status=error\n";
    }

    return ok;
}