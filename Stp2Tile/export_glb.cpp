#include "export_glb.h"

#include <fstream>
#include <stdexcept>

#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>

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

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <TopoDS_Shape.hxx>
#include <cmath>
#include <algorithm>
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

struct MeshTolerances
{
    double LinearDeflectionAbs;
    double AngularDeflectionDeg;
};


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

float LerpClamped(float a, float b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return a + (b - a) * t;
}

bool ExportTileToGlbFile(
    const Handle(XCAFDoc_ShapeTool)& sourceShapeTool,
    const std::vector<Occurrence>& occurrences,
    const std::vector<std::uint32_t>& itemIndices,
    const std::string& glbPath,
    const float decimationFactor)
{
    if (sourceShapeTool.IsNull())
        throw std::runtime_error("ExportTileToGlbFile: sourceShapeTool is null.");

    Handle(TDocStd_Document) tileDoc = CreateEmptyXcafDocument();
    Handle(XCAFDoc_ShapeTool) tileShapeTool = GetShapeTool(tileDoc);

    // Build a single compound that contains all shapes placed in world space.
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);

    for (std::uint32_t idx : itemIndices)
    {
        if (idx >= occurrences.size())
            continue;

        const Occurrence& occ = occurrences[static_cast<std::size_t>(idx)];

        // Apply world transform as a location (fast, doesn’t copy geometry).
        TopLoc_Location loc(occ.WorldTransform);
        TopoDS_Shape placed = occ.Shape.Located(loc);

        builder.Add(compound, placed);
    }

    if (compound.IsNull())
    {
        std::cerr << "ExportTileToGlbFile: compound is null, no shapes to export.\n";

        return false;
    }

    std::cout << "Tessellating " << glbPath << "... \n";

    float linearDeflection = LerpClamped(0.1f, 10.0f, decimationFactor);
    float angularDeflection = LerpClamped(1.5f, 6.0f, decimationFactor);

    BRepMesh_IncrementalMesh mesh(
        compound,
        linearDeflection,
        Standard_False,                 // absolute
        angularDeflection,
        Standard_True                   // parallel
    );
    mesh.Perform();

    // linear deflection 0.05f for high fidelity mesh

    // Angular deflection 1.5f for high fidelity mesh

    std::size_t triCount = CountTriangles(compound);

    std::cout << "done. " << "(triangles=" << triCount << ")\n";

    // Add to tile document and CAPTURE the returned label.
    // AddShape returns the label of the added shape in the tile doc.
    TDF_Label tileRootLabel = tileShapeTool->AddShape(compound, Standard_False);

    // Now export: explicitly tell writer which root(s) to export
    RWGltf_CafWriter writer(TCollection_AsciiString(glbPath.c_str()), Standard_True);
    writer.SetTransformationFormat(RWGltf_WriterTrsfFormat_Compact);

    TDF_LabelSequence rootLabels;
    rootLabels.Append(tileRootLabel);

    const TColStd_MapOfAsciiString* labelFilter = nullptr;
    TColStd_IndexedDataMapOfStringString fileInfo;
    Message_ProgressRange progress;

    bool ok = writer.Perform(tileDoc, rootLabels, labelFilter, fileInfo, progress);

    if(ok)
        std::cout << "Exported GLB file: " << glbPath << " with " << itemIndices.size() << " items.\n";
    else
        std::cerr << "Failed to export GLB file: " << glbPath << "\n";

    return ok;
}

bool ExportBoxToGlbFile(
    const Bnd_Box& bounds,
    const std::string& glbPath)
{
    // Reject invalid bounds early
    if (bounds.IsVoid())
        return false;

    // Reject open / infinite bounds
    if (bounds.IsOpenXmin() || bounds.IsOpenXmax() ||
        bounds.IsOpenYmin() || bounds.IsOpenYmax() ||
        bounds.IsOpenZmin() || bounds.IsOpenZmax())
        return false;

    double xmin, ymin, zmin, xmax, ymax, zmax;
    bounds.Get(xmin, ymin, zmin, xmax, ymax, zmax);

    // Enforce minimum thickness to avoid degenerate boxes
    constexpr double eps = 1e-3;

    auto fixDegenerate = [&](double& lo, double& hi)
    {
        if ((hi - lo) < eps)
        {
            double c = 0.5 * (lo + hi);
            lo = c - eps * 0.5;
            hi = c + eps * 0.5;
        }
    };

    fixDegenerate(xmin, xmax);
    fixDegenerate(ymin, ymax);
    fixDegenerate(zmin, zmax);

    // Build axis-aligned box in OCCT space
    TopoDS_Shape boxShape =
        BRepPrimAPI_MakeBox(
            gp_Pnt(xmin, ymin, zmin),
            gp_Pnt(xmax, ymax, zmax)
        ).Shape();

    // Explicitly mesh the shape (important for reliable GLTF export)
    constexpr double deflection = 0.1; // adjust for your scene scale
    BRepMesh_IncrementalMesh mesher(boxShape, deflection);
    mesher.Perform();

    // Create a fresh XCAF document
    Handle(XCAFApp_Application) app = XCAFApp_Application::GetApplication();
    Handle(TDocStd_Document) doc;
    app->NewDocument("MDTV-XCAF", doc);

    Handle(XCAFDoc_ShapeTool) shapeTool =
        XCAFDoc_DocumentTool::ShapeTool(doc->Main());

    // Add shape as a root
    TDF_Label tileRootLabel =
        shapeTool->AddShape(boxShape, Standard_True);

    // Export GLB
    RWGltf_CafWriter writer(
        TCollection_AsciiString(glbPath.c_str()),
        Standard_True /* binary GLB */
    );

    writer.SetTransformationFormat(RWGltf_WriterTrsfFormat_Compact);

    TDF_LabelSequence rootLabels;
    rootLabels.Append(tileRootLabel);

    const TColStd_MapOfAsciiString* labelFilter = nullptr;
    TColStd_IndexedDataMapOfStringString fileInfo;
    Message_ProgressRange progress;

    const Standard_Boolean ok =
        writer.Perform(doc, rootLabels, labelFilter, fileInfo, progress);

    return ok == Standard_True;
}
