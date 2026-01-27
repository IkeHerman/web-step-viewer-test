#include <iostream>
#include <unordered_map>
#include <string>
#include <vector>
#include <iomanip>

#include <STEPCAFControl_Reader.hxx>

#include <TDocStd_Document.hxx>
#include <TDF_Label.hxx>
#include <TDF_Tool.hxx>
#include <TCollection_AsciiString.hxx>

#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include <TDataStd_Name.hxx>

#include <TopoDS_Shape.hxx>
#include <TopLoc_Location.hxx>
#include <gp_Trsf.hxx>
#include <gp_Mat.hxx>
#include <gp_XYZ.hxx>

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>

#include "common.h"
#include "octree.h"
#include "export_glb.h"
#include "b3dm.h"
#include "tileset_emit.h"

#include <filesystem>
#include <algorithm>

#include <TDataStd_Name.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>



typedef std::unordered_map<TDF_Label, Prototype> PrototypeLabelMap;

static std::string LabelToString(const TDF_Label& label)
{
    TCollection_AsciiString entry;
    TDF_Tool::Entry(label, entry);
    return std::string(entry.ToCString());
}

static std::string GetLabelName(const TDF_Label& label)
{
    Handle(TDataStd_Name) nameAttr;
    if (label.FindAttribute(TDataStd_Name::GetID(), nameAttr) && !nameAttr.IsNull())
    {
        // TCollection_ExtendedString -> convert to UTF-8-ish narrow for logging
        TCollection_AsciiString ascii(nameAttr->Get());
        return std::string(ascii.ToCString());
    }
    return std::string();
}

static Bnd_Box ComputeLocalBounds(const TopoDS_Shape& shape)
{
    Bnd_Box box;
    // You can tune triangulation usage etc later; this is fine for bounding boxes.
    BRepBndLib::Add(shape, box);
    return box;
}

static void PrintTransform(const gp_Trsf& trsf)
{
    // Print as: translation + 3x3 rotation/scale matrix.
    gp_XYZ t = trsf.TranslationPart();
    gp_Mat r = trsf.VectorialPart();

    std::cout << "  T=("
              << t.X() << "," << t.Y() << "," << t.Z() << ")";

    std::cout << "  R=["
              << r.Value(1,1) << "," << r.Value(1,2) << "," << r.Value(1,3) << ";"
              << r.Value(2,1) << "," << r.Value(2,2) << "," << r.Value(2,3) << ";"
              << r.Value(3,1) << "," << r.Value(3,2) << "," << r.Value(3,3) << "]";
}

static void PrintBounds(const Bnd_Box& box)
{
    if (box.IsVoid())
    {
        std::cout << "  AABB=(void)";
        return;
    }

    Standard_Real xmin, ymin, zmin, xmax, ymax, zmax;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);

    std::cout << "  AABB=("
              << xmin << "," << ymin << "," << zmin << ")-("
              << xmax << "," << ymax << "," << zmax << ")";
}

static void UpdateGlobalBounds(Bnd_Box& globalBox, const Bnd_Box& newBox )
{
    Standard_Real xmin, ymin, zmin, xmax, ymax, zmax;
    newBox.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    globalBox.Update(xmin, ymin, zmin, xmax, ymax, zmax);
}

static Prototype *GetOrCreatePrototype(
    const Handle(XCAFDoc_ShapeTool)& shapeTool,
    const TDF_Label& protoLabel,
    PrototypeLabelMap& prototypeByLabel)
{
    // Easy case, the proto label already exists meaning the author instanced it before
    auto it = prototypeByLabel.find(protoLabel);
    if (it != prototypeByLabel.end())
    {
        return &(it->second);
    }

    // Fetch prototype shape (definition geometry)
    TopoDS_Shape protoShape = shapeTool->GetShape(protoLabel);
    if (protoShape.IsNull())
    {
        // No shape found, we can probably ignore this but lets print a warning
        std::cerr << "Warning: Prototype shape is null for label "<< LabelToString(protoLabel) << "\n";

        // return 0 as invalid; caller should handle
        return nullptr;
    }

    // Assume this is a new prototype, create and stuff into our map
    Prototype p;
    p.Label = protoLabel;
    p.LocalBounds = ComputeLocalBounds(protoShape);
    p.Shape = protoShape;

    auto result = prototypeByLabel.emplace(protoLabel, p);
    return &(result.first->second);
}

static void TraverseLabel(
    const Handle(XCAFDoc_ShapeTool)& shapeTool,
    const TDF_Label& label,
    const TopLoc_Location& parentLoc,
    PrototypeLabelMap& prototypeByLabel,
    std::vector<Occurrence>& occurrences,
    Bnd_Box& globalBounds)
{
    if (label.IsNull())
        return;

    // Resolve reference -> prototype definition + local placement
    TDF_Label effectiveLabel = label;
    TopLoc_Location instanceLoc; // identity

    if (shapeTool->IsReference(label))
    {
        shapeTool->GetReferredShape(label, effectiveLabel);
        instanceLoc = shapeTool->GetLocation(label);
    }

    TopLoc_Location worldLoc = parentLoc * instanceLoc;

    // IMPORTANT: test assembly on the effective label (proto), not the instance label
    if (shapeTool->IsAssembly(effectiveLabel))
    {
        TDF_LabelSequence components;
        shapeTool->GetComponents(effectiveLabel, components);

        for (Standard_Integer i = 1; i <= components.Length(); ++i)
        {
            TraverseLabel(shapeTool, components.Value(i), worldLoc,
                          prototypeByLabel, occurrences, globalBounds);
        }
        return;
    }

    // Create/get prototype (only for non-assemblies)
    Prototype* prototypeptr = GetOrCreatePrototype(shapeTool, effectiveLabel, prototypeByLabel);
    if (prototypeptr == nullptr)
        return;

    const Prototype& proto = *prototypeptr;

    // Leaf / shape node
    TopoDS_Shape shape = shapeTool->GetShape(effectiveLabel);
    if (shape.IsNull())
        return;

    const gp_Trsf worldTrsf = worldLoc.Transformation();

    Bnd_Box worldBounds = proto.LocalBounds.Transformed(worldTrsf);
    UpdateGlobalBounds(globalBounds, worldBounds);

    Occurrence occ;
    occ.Label = label;            // keep original label for debugging
    occ.Shape = shape;
    occ.WorldTransform = worldTrsf;
    occ.WorldBounds = worldBounds;

    occurrences.push_back(occ);
}

static bool IsStepPath(const std::filesystem::path& p)
{
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return (ext == ".stp" || ext == ".step");
}

static std::vector<std::filesystem::path> CollectStepFiles(const std::filesystem::path& input)
{
    std::vector<std::filesystem::path> files;

    if (std::filesystem::is_regular_file(input))
    {
        if (IsStepPath(input))
            files.push_back(input);
        return files;
    }

    if (std::filesystem::is_directory(input))
    {
        for (const std::filesystem::directory_entry& e : std::filesystem::directory_iterator(input))
        {
            if (!e.is_regular_file())
                continue;

            const std::filesystem::path p = e.path();
            if (IsStepPath(p))
                files.push_back(p);
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

#include <TopExp_Explorer.hxx>
#include <TopAbs_ShapeEnum.hxx>

static const char* ShapeTypeName(TopAbs_ShapeEnum t)
{
    switch (t)
    {
        case TopAbs_COMPOUND:  return "COMPOUND";
        case TopAbs_COMPSOLID: return "COMPSOLID";
        case TopAbs_SOLID:     return "SOLID";
        case TopAbs_SHELL:     return "SHELL";
        case TopAbs_FACE:      return "FACE";
        default:               return "OTHER";
    }
}

static int CountSubshapes(const TopoDS_Shape& s, TopAbs_ShapeEnum what)
{
    int count = 0;
    for (TopExp_Explorer ex(s, what); ex.More(); ex.Next())
        ++count;
    return count;
}


static bool MergeStepFileIntoMasterDoc(
    const std::filesystem::path& stepPath,
    const Handle(TDocStd_Document)& masterDoc)
{
    // Read STEP into a temporary doc
    Handle(TDocStd_Document) tempDoc = new TDocStd_Document("MDTV-XCAF");

    STEPCAFControl_Reader reader;
    IFSelect_ReturnStatus status = reader.ReadFile(stepPath.string().c_str());
    if (status != IFSelect_RetDone)
    {
        std::cerr << "Failed to read STEP file: " << stepPath << "\n";
        return false;
    }

    if (!reader.Transfer(tempDoc))
    {
        std::cerr << "STEP transfer failed for: " << stepPath << "\n";
        return false;
    }

    Handle(XCAFDoc_ShapeTool) tempShapeTool = XCAFDoc_DocumentTool::ShapeTool(tempDoc->Main());
    Handle(XCAFDoc_ShapeTool) masterShapeTool = XCAFDoc_DocumentTool::ShapeTool(masterDoc->Main());
    if (tempShapeTool.IsNull() || masterShapeTool.IsNull())
    {
        std::cerr << "Failed to get ShapeTool.\n";
        return false;
    }

    // Optional: create an assembly "folder" per file in master
    const bool groupByFile = true;
    TDF_Label fileRoot;
    if (groupByFile)
    {
        fileRoot = masterShapeTool->NewShape();     // creates a label

        // name it for debugging
        TCollection_ExtendedString nm(stepPath.filename().string().c_str());
        TDataStd_Name::Set(fileRoot, nm);
    }

    // Copy free shapes from temp into master
    TDF_LabelSequence tempRoots;
    tempShapeTool->GetFreeShapes(tempRoots);

    for (Standard_Integer i = 1; i <= tempRoots.Length(); ++i)
    {
        const TDF_Label tempRootLabel = tempRoots.Value(i);
        const TopoDS_Shape tempRootShape = tempShapeTool->GetShape(tempRootLabel);
        if (tempRootShape.IsNull())
            continue;

        std::cout << "STEP root " << i
          << " type=" << ShapeTypeName(tempRootShape.ShapeType())
          << " solids=" << CountSubshapes(tempRootShape, TopAbs_SOLID)
          << " shells=" << CountSubshapes(tempRootShape, TopAbs_SHELL)
          << " faces=" << CountSubshapes(tempRootShape, TopAbs_FACE)
          << "\n";


        // AddShape copies the shape into master and returns its label
        const bool makeAssemblyIfCompound = false;
        const TDF_Label newLabel = masterShapeTool->AddShape(tempRootShape, makeAssemblyIfCompound);

        if (groupByFile)
        {
            // Add as a component of the file root assembly with identity location
            const TopLoc_Location identityLoc;
            masterShapeTool->AddComponent(fileRoot, newLabel, identityLoc);
        }
    }

    return true;
}


int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "Usage: step_occurrences <file.step | directory>\n";
        return 2;
    }

    std::filesystem::path inputPath(argv[1]);
    std::vector<std::filesystem::path> stepFiles = CollectStepFiles(inputPath);

    if (stepFiles.empty())
    {
        std::cerr << "No .stp/.step files found at: " << inputPath << "\n";
        return 2;
    }

    // Master XDE document
    std::cout << "Created master XDE document.\n";
    Handle(TDocStd_Document) doc = new TDocStd_Document("MDTV-XCAF");

    // Merge all STEP files into master doc
    for (const std::filesystem::path& p : stepFiles)
    {
        std::cout << "Merging STEP: " << p << "\n";
        if (!MergeStepFileIntoMasterDoc(p, doc))
        {
            std::cerr << "Merge failed for: " << p << "\n";
            return 1;
        }
    }

    Handle(XCAFDoc_ShapeTool) shapeTool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    if (shapeTool.IsNull())
    {
        std::cerr << "Failed to get XCAFDoc_ShapeTool.\n";
        return 1;
    }

    // Traverse from free shapes (roots)
    TDF_LabelSequence roots;
    shapeTool->GetFreeShapes(roots);

    PrototypeLabelMap prototypeByLabel;
    std::vector<Occurrence> occurrences;
    Bnd_Box globalBounds;
    globalBounds.SetVoid();

    TopLoc_Location identity;

    for (Standard_Integer i = 1; i <= roots.Length(); ++i)
    {
        TraverseLabel(shapeTool, roots.Value(i), identity, prototypeByLabel, occurrences, globalBounds);
    }

    // Document ingestion
    std::cout << "Found " << occurrences.size() << " occurrences\n";
    std::cout << "Found " << prototypeByLabel.size() << " prototypes\n";
    std::cout << "Global bounds: ";
    PrintBounds(globalBounds);
    std::cout << "\n\n";


    // Insert into Octree
    std::cout << "Inserting occurrences into Octree...\n";

    TileOctree::Config cfg;
    cfg.maxDepth = 9;
    cfg.maxItemsPerNode = 5;
    cfg.minNodeMaxSide = 1.0;
    cfg.looseFactor = 1.25;
    

    TileOctree tree(cfg);
    tree.Build(occurrences, globalBounds);

    std::cout << "Octree built\n";

    // Export octree nodes to GLB tiles

    std::cout << "Exporting Tiles...\n";

    TilesetEmit::Options opt;
    opt.tilesetOutDir = "../Tile-Viewer/public";
    opt.contentSubdir = "tiles";
    opt.tileFilePrefix = "tile_";
    opt.keepGlbFilesForDebug = true; // start true while validating
    opt.useTightBounds = true;
    opt.contentOnlyAtLeaves = false;

    TilesetEmit::EmitTilesetAndB3dm(tree, shapeTool, occurrences, opt);

    return 0;
}

