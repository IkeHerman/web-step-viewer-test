#include <iostream>
#include <string>
#include <vector>
#include <iomanip>
#include <filesystem>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <exception>
#include <fstream>
#include <memory>
#include <unordered_set>
#include <unordered_map>

#include <STEPCAFControl_Reader.hxx>

#include <TDocStd_Document.hxx>
#include <TDF_Label.hxx>
#include <TDF_Tool.hxx>
#include <TCollection_AsciiString.hxx>
#include <TCollection_ExtendedString.hxx>

#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_VisMaterialTool.hxx>
#include <XCAFDoc_VisMaterial.hxx>
#include <XCAFApp_Application.hxx>

#include <TDataStd_Name.hxx>

#include <TopoDS_Shape.hxx>
#include <TopoDS.hxx>
#include <TopLoc_Location.hxx>
#include <gp_Trsf.hxx>

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>

#include <TopExp_Explorer.hxx>
#include <TopAbs_ShapeEnum.hxx>

#include <IFSelect_ReturnStatus.hxx>

#include <BRepMesh_IncrementalMesh.hxx>
#include <TopoDS_Face.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <Quantity_Color.hxx>

#include "common.h"
#include "octree.h"
#include "export_glb.h"
#include "b3dm.h"
#include "glbopt.h"
#include "tileset_emit.h"
#include "stpsani.h"

static bool g_verboseLogging = false;

static std::string LabelToString(const TDF_Label& label);

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
        TCollection_AsciiString ascii(nameAttr->Get());
        return std::string(ascii.ToCString());
    }
    return std::string();
}

static Bnd_Box ComputeLocalBounds(const TopoDS_Shape& shape)
{
    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    return box;
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

static void UpdateGlobalBounds(Bnd_Box& globalBox, const Bnd_Box& newBox)
{
    if (newBox.IsVoid())
        return;

    Standard_Real xmin, ymin, zmin, xmax, ymax, zmax;
    newBox.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    globalBox.Update(xmin, ymin, zmin, xmax, ymax, zmax);
}

static double BoundsMaxSideLength(const Bnd_Box& box)
{
    if (box.IsVoid())
    {
        return 0.0;
    }

    Standard_Real xmin, ymin, zmin, xmax, ymax, zmax;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    const double sx = static_cast<double>(xmax - xmin);
    const double sy = static_cast<double>(ymax - ymin);
    const double sz = static_cast<double>(zmax - zmin);
    return std::max(sx, std::max(sy, sz));
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

struct CliOptions
{
    std::filesystem::path inputPath;
    std::filesystem::path outDir = "../Tile-Viewer/public";
    std::string contentSubdir = "tiles";
    std::string tilePrefix = "tile_";
    bool keepGlb = true;
    bool useTightBounds = true;
    bool contentOnlyAtLeaves = false;
    bool verbose = false;
};

static void PrintUsage(const char* programName)
{
    const std::string exe = (programName && *programName) ? programName : "stp2tile";

    std::cout
        << "Usage:\n"
        << "  " << exe << " [options] <input.step|input_directory>\n\n"
        << "Options:\n"
        << "  -o, --out-dir <path>         Output directory for tileset.json and tile content\n"
        << "                               (default: ../Tile-Viewer/public)\n"
        << "      --content-subdir <name>  Subdirectory under out-dir for tile content\n"
        << "                               (default: tiles)\n"
        << "      --tile-prefix <prefix>   Tile filename prefix (default: tile_)\n"
        << "      --keep-glb               Keep intermediate .glb files\n"
        << "      --discard-glb            Delete intermediate .glb files after wrap\n"
        << "      --tight-bounds           Enable tight tile bounds (default)\n"
        << "      --no-tight-bounds        Disable tight tile bounds\n"
        << "      --content-only-leaves    Emit content only at leaves\n"
        << "      --content-all-levels     Allow content at internal levels (default)\n"
        << "  -v, --verbose                Enable verbose debug output\n"
        << "  -h, --help                   Show this help\n";
}

static bool ParseCli(int argc, char** argv, CliOptions& out, int& outExitCode)
{
    outExitCode = 0;

    bool hasInputPath = false;

    auto requireValue = [&](int& index, const std::string& optName, std::string& outValue) -> bool
    {
        if (index + 1 >= argc)
        {
            std::cerr << "Missing value for option: " << optName << "\n";
            return false;
        }

        ++index;
        outValue = argv[index];
        return true;
    };

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];

        if (arg == "-h" || arg == "--help")
        {
            PrintUsage(argv[0]);
            outExitCode = 0;
            return false;
        }
        else if (arg == "-o" || arg == "--out-dir")
        {
            std::string value;
            if (!requireValue(i, arg, value))
            {
                outExitCode = 2;
                return false;
            }
            out.outDir = value;
        }
        else if (arg == "--content-subdir")
        {
            std::string value;
            if (!requireValue(i, arg, value))
            {
                outExitCode = 2;
                return false;
            }
            out.contentSubdir = value;
        }
        else if (arg == "--tile-prefix")
        {
            std::string value;
            if (!requireValue(i, arg, value))
            {
                outExitCode = 2;
                return false;
            }
            out.tilePrefix = value;
        }
        else if (arg == "--keep-glb")
        {
            out.keepGlb = true;
        }
        else if (arg == "--discard-glb")
        {
            out.keepGlb = false;
        }
        else if (arg == "--tight-bounds")
        {
            out.useTightBounds = true;
        }
        else if (arg == "--no-tight-bounds")
        {
            out.useTightBounds = false;
        }
        else if (arg == "--content-only-leaves")
        {
            out.contentOnlyAtLeaves = true;
        }
        else if (arg == "--content-all-levels")
        {
            out.contentOnlyAtLeaves = false;
        }
        else if (arg == "-v" || arg == "--verbose")
        {
            out.verbose = true;
        }
        else if (!arg.empty() && arg[0] == '-')
        {
            std::cerr << "Unknown option: " << arg << "\n";
            PrintUsage(argv[0]);
            outExitCode = 2;
            return false;
        }
        else
        {
            if (hasInputPath)
            {
                std::cerr << "Multiple input paths provided. Only one input path is allowed.\n";
                outExitCode = 2;
                return false;
            }

            out.inputPath = arg;
            hasInputPath = true;
        }
    }

    if (!hasInputPath)
    {
        std::cerr << "Missing input path.\n";
        PrintUsage(argv[0]);
        outExitCode = 2;
        return false;
    }

    if (out.contentSubdir.empty())
    {
        std::cerr << "--content-subdir must not be empty.\n";
        outExitCode = 2;
        return false;
    }

    if (out.tilePrefix.empty())
    {
        std::cerr << "--tile-prefix must not be empty.\n";
        outExitCode = 2;
        return false;
    }

    return true;
}

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

static std::uint32_t CountExistingTrianglesOnly(const TopoDS_Shape& shape)
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

    if (triangleCount > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        return std::numeric_limits<std::uint32_t>::max();

    return static_cast<std::uint32_t>(triangleCount);
}

static std::uint32_t SafeEstimateTriangles(
    const TopoDS_Shape& shape,
    const TDF_Label& sourceLabel)
{
    try
    {
        std::uint32_t existing = CountExistingTrianglesOnly(shape);
        if (existing > 0)
            return existing;

        const int faceCount = CountSubshapes(shape, TopAbs_FACE);
        if (faceCount <= 0)
            return 0;

        const std::uint64_t estimate = static_cast<std::uint64_t>(faceCount) * 24ull;
        if (estimate > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))
            return std::numeric_limits<std::uint32_t>::max();

        return static_cast<std::uint32_t>(estimate);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Warning: triangle estimate failed for label "
                  << LabelToString(sourceLabel)
                  << " error=\"" << e.what() << "\"\n";
        return 0;
    }
    catch (...)
    {
        std::cerr << "Warning: triangle estimate failed for label "
                  << LabelToString(sourceLabel)
                  << " with unknown exception\n";
        return 0;
    }
}

static CachedColorSet ResolveColorsForOccurrence(
    const Handle(XCAFDoc_ColorTool)& colorTool,
    const TDF_Label& sourceLabel,
    const TDF_Label& effectiveLabel,
    const TopoDS_Shape& parentShape,
    const TopoDS_Shape& sourceShape)
{
    CachedColorSet colors;

    if (colorTool.IsNull())
    {
        return colors;
    }

    Quantity_Color c;

    if (!sourceLabel.IsNull())
    {
        if (colorTool->GetColor(sourceLabel, XCAFDoc_ColorGen, c)) { colors.HasGen = true; colors.Gen = c; }
        if (colorTool->GetColor(sourceLabel, XCAFDoc_ColorSurf, c)) { colors.HasSurf = true; colors.Surf = c; }
        if (colorTool->GetColor(sourceLabel, XCAFDoc_ColorCurv, c)) { colors.HasCurv = true; colors.Curv = c; }
    }

    if (!effectiveLabel.IsNull())
    {
        if (colorTool->GetColor(effectiveLabel, XCAFDoc_ColorGen, c)) { colors.HasGen = true; colors.Gen = c; }
        if (colorTool->GetColor(effectiveLabel, XCAFDoc_ColorSurf, c)) { colors.HasSurf = true; colors.Surf = c; }
        if (colorTool->GetColor(effectiveLabel, XCAFDoc_ColorCurv, c)) { colors.HasCurv = true; colors.Curv = c; }
    }

    if (!sourceShape.IsNull())
    {
        if (colorTool->GetColor(sourceShape, XCAFDoc_ColorGen, c)) { colors.HasGen = true; colors.Gen = c; }
        if (colorTool->GetColor(sourceShape, XCAFDoc_ColorSurf, c)) { colors.HasSurf = true; colors.Surf = c; }
        if (colorTool->GetColor(sourceShape, XCAFDoc_ColorCurv, c)) { colors.HasCurv = true; colors.Curv = c; }
    }

    if (!parentShape.IsNull())
    {
        if (colorTool->GetColor(parentShape, XCAFDoc_ColorGen, c)) { colors.HasGen = true; colors.Gen = c; }
        if (colorTool->GetColor(parentShape, XCAFDoc_ColorSurf, c)) { colors.HasSurf = true; colors.Surf = c; }
        if (colorTool->GetColor(parentShape, XCAFDoc_ColorCurv, c)) { colors.HasCurv = true; colors.Curv = c; }
    }

    return colors;
}

static Handle(XCAFDoc_VisMaterial) ResolveVisMaterialForOccurrence(
    const Handle(XCAFDoc_VisMaterialTool)& visMatTool,
    const TDF_Label& sourceLabel,
    const TDF_Label& effectiveLabel,
    const TopoDS_Shape& parentShape,
    const TopoDS_Shape& sourceShape)
{
    if (visMatTool.IsNull())
    {
        return Handle(XCAFDoc_VisMaterial)();
    }

    TDF_Label matLabel;
    Handle(XCAFDoc_VisMaterial) resolved;

    if (!sourceLabel.IsNull() && XCAFDoc_VisMaterialTool::GetShapeMaterial(sourceLabel, matLabel))
    {
        resolved = XCAFDoc_VisMaterialTool::GetMaterial(matLabel);
    }

    if (!effectiveLabel.IsNull() && XCAFDoc_VisMaterialTool::GetShapeMaterial(effectiveLabel, matLabel))
    {
        resolved = XCAFDoc_VisMaterialTool::GetMaterial(matLabel);
    }

    if (!sourceShape.IsNull() && visMatTool->GetShapeMaterial(sourceShape, matLabel))
    {
        resolved = XCAFDoc_VisMaterialTool::GetMaterial(matLabel);
    }

    if (!parentShape.IsNull() && visMatTool->GetShapeMaterial(parentShape, matLabel))
    {
        resolved = XCAFDoc_VisMaterialTool::GetMaterial(matLabel);
    }

    return resolved;
}

static std::vector<CachedFaceAppearance> ExtractFaceAppearance(
    const Handle(XCAFDoc_ColorTool)& colorTool,
    const Handle(XCAFDoc_VisMaterialTool)& visMatTool,
    const TopoDS_Shape& sourceShape)
{
    std::vector<CachedFaceAppearance> faces;

    for (TopExp_Explorer exp(sourceShape, TopAbs_FACE); exp.More(); exp.Next())
    {
        const TopoDS_Face sourceFace = TopoDS::Face(exp.Current());
        CachedFaceAppearance faceAppearance;

        if (!colorTool.IsNull())
        {
            Quantity_Color c;
            if (colorTool->GetColor(sourceFace, XCAFDoc_ColorGen, c)) { faceAppearance.Colors.HasGen = true; faceAppearance.Colors.Gen = c; }
            if (colorTool->GetColor(sourceFace, XCAFDoc_ColorSurf, c)) { faceAppearance.Colors.HasSurf = true; faceAppearance.Colors.Surf = c; }
            if (colorTool->GetColor(sourceFace, XCAFDoc_ColorCurv, c)) { faceAppearance.Colors.HasCurv = true; faceAppearance.Colors.Curv = c; }
        }

        if (!visMatTool.IsNull())
        {
            TDF_Label matLabel;
            if (visMatTool->GetShapeMaterial(sourceFace, matLabel))
            {
                faceAppearance.VisMaterial = XCAFDoc_VisMaterialTool::GetMaterial(matLabel);
            }
        }

        faces.push_back(std::move(faceAppearance));
    }

    return faces;
}

struct SolidCacheKey
{
    const void* TShapePtr = nullptr;
    TopAbs_Orientation Orientation = TopAbs_FORWARD;

    bool operator==(const SolidCacheKey& other) const
    {
        return TShapePtr == other.TShapePtr && Orientation == other.Orientation;
    }
};

struct SolidCacheKeyHasher
{
    std::size_t operator()(const SolidCacheKey& key) const
    {
        const std::size_t h1 = std::hash<const void*>{}(key.TShapePtr);
        const std::size_t h2 = std::hash<int>{}(static_cast<int>(key.Orientation));
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ull + (h1 << 6) + (h1 >> 2));
    }
};

struct SolidCacheEntry
{
    Bnd_Box LocalBounds;
    std::uint32_t TriangleCount = 0;
};

struct NonRenderableLeafInfo
{
    std::string Label;
    std::string Name;
    TopAbs_ShapeEnum Type = TopAbs_SHAPE;
    int ShellCount = 0;
    int FaceCount = 0;
};

static void PromoteFaceColorToShapeFallback(CachedOccurrenceAppearance& appearance)
{
    CachedColorSet& shape = appearance.ResolvedShapeColors;
    if (shape.HasGen || shape.HasSurf || shape.HasCurv)
    {
        return;
    }

    for (const CachedFaceAppearance& face : appearance.Faces)
    {
        if (face.Colors.HasSurf)
        {
            shape.HasSurf = true;
            shape.Surf = face.Colors.Surf;
            return;
        }
        if (face.Colors.HasGen)
        {
            shape.HasGen = true;
            shape.Gen = face.Colors.Gen;
            return;
        }
        if (face.Colors.HasCurv)
        {
            shape.HasCurv = true;
            shape.Curv = face.Colors.Curv;
            return;
        }
    }
}

static SolidCacheKey MakeSolidCacheKey(const TopoDS_Shape& localSolid)
{
    Handle(TopoDS_TShape) tshape = localSolid.TShape();
    return SolidCacheKey{ static_cast<const void*>(tshape.operator->()), localSolid.Orientation() };
}

static std::size_t EmitSolidOccurrencesFromShape(
    const TDF_Label& sourceLabel,
    const TDF_Label& effectiveLabel,
    const TopoDS_Shape& sourceShape,
    const Handle(XCAFDoc_ColorTool)& colorTool,
    const Handle(XCAFDoc_VisMaterialTool)& visMatTool,
    const TopLoc_Location& parentWorldLoc,
    std::unordered_map<SolidCacheKey, SolidCacheEntry, SolidCacheKeyHasher>& solidCache,
    std::vector<Occurrence>& occurrences,
    Bnd_Box& globalBounds,
    std::uint64_t& totalTriangles,
    TopAbs_ShapeEnum targetType)
{
    std::size_t emitted = 0;

    for (TopExp_Explorer ex(sourceShape, targetType); ex.More(); ex.Next())
    {
        const TopoDS_Shape emittedShape = ex.Current();
        if (emittedShape.IsNull())
            continue;

        const TopLoc_Location emittedLocalLoc = emittedShape.Location();
        const TopLoc_Location emittedWorldLoc = parentWorldLoc * emittedLocalLoc;
        const gp_Trsf emittedWorldTrsf = emittedWorldLoc.Transformation();
        const TopoDS_Shape emittedShapeAtLocalOrigin = emittedShape.Located(TopLoc_Location());

        const SolidCacheKey cacheKey = MakeSolidCacheKey(emittedShapeAtLocalOrigin);
        std::unordered_map<SolidCacheKey, SolidCacheEntry, SolidCacheKeyHasher>::iterator it =
            solidCache.find(cacheKey);

        if (it == solidCache.end())
        {
            SolidCacheEntry entry;
            entry.LocalBounds = ComputeLocalBounds(emittedShapeAtLocalOrigin);
            entry.TriangleCount = SafeEstimateTriangles(emittedShapeAtLocalOrigin, sourceLabel);

            it = solidCache.emplace(cacheKey, std::move(entry)).first;
        }

        const SolidCacheEntry& cached = it->second;

        std::shared_ptr<CachedOccurrenceAppearance> appearance =
            std::make_shared<CachedOccurrenceAppearance>();
        appearance->ResolvedShapeColors = ResolveColorsForOccurrence(
            colorTool,
            sourceLabel,
            effectiveLabel,
            sourceShape,
            emittedShapeAtLocalOrigin);
        appearance->ResolvedShapeMaterial = ResolveVisMaterialForOccurrence(
            visMatTool,
            sourceLabel,
            effectiveLabel,
            sourceShape,
            emittedShapeAtLocalOrigin);
        appearance->Faces = ExtractFaceAppearance(
            colorTool,
            visMatTool,
            emittedShapeAtLocalOrigin);
        PromoteFaceColorToShapeFallback(*appearance);

        const Bnd_Box emittedWorldBounds = cached.LocalBounds.Transformed(emittedWorldTrsf);
        UpdateGlobalBounds(globalBounds, emittedWorldBounds);
        const std::uint32_t triangleCount = cached.TriangleCount;

        try
        {
            Occurrence occ;
            occ.Label = sourceLabel;
            occ.EffectiveLabel = effectiveLabel;
            occ.Shape = emittedShapeAtLocalOrigin;
            occ.WorldTransform = emittedWorldTrsf;
            occ.WorldBounds = emittedWorldBounds;
            occ.TriangleCount = triangleCount;
            occ.Appearance = appearance;

            occurrences.push_back(occ);
        }
        catch (const std::exception& e)
        {
            std::cerr << "Failure while appending occurrence"
                      << " label=" << LabelToString(sourceLabel)
                      << " emitted_so_far=" << emitted
                      << " total_occurrences=" << occurrences.size()
                      << " error=\"" << e.what() << "\"\n";
            throw;
        }

        totalTriangles += triangleCount;
        ++emitted;

        if (g_verboseLogging && (occurrences.size() % 10000u) == 0u)
        {
            std::cout << "[Debug] occurrences=" << occurrences.size()
                      << " tris=" << totalTriangles << "\n";
        }
    }

    return emitted;
}

static std::size_t EmitSolidOccurrencesFromShape(
    const TDF_Label& sourceLabel,
    const TDF_Label& effectiveLabel,
    const TopoDS_Shape& sourceShape,
    const Handle(XCAFDoc_ColorTool)& colorTool,
    const Handle(XCAFDoc_VisMaterialTool)& visMatTool,
    const TopLoc_Location& parentWorldLoc,
    std::unordered_map<SolidCacheKey, SolidCacheEntry, SolidCacheKeyHasher>& solidCache,
    std::vector<Occurrence>& occurrences,
    Bnd_Box& globalBounds,
    std::uint64_t& totalTriangles)
{
    return EmitSolidOccurrencesFromShape(
        sourceLabel,
        effectiveLabel,
        sourceShape,
        colorTool,
        visMatTool,
        parentWorldLoc,
        solidCache,
        occurrences,
        globalBounds,
        totalTriangles,
        TopAbs_SOLID);
}

static std::size_t EmitShellOccurrencesFromShape(
    const TDF_Label& sourceLabel,
    const TDF_Label& effectiveLabel,
    const TopoDS_Shape& sourceShape,
    const Handle(XCAFDoc_ColorTool)& colorTool,
    const Handle(XCAFDoc_VisMaterialTool)& visMatTool,
    const TopLoc_Location& parentWorldLoc,
    std::unordered_map<SolidCacheKey, SolidCacheEntry, SolidCacheKeyHasher>& solidCache,
    std::vector<Occurrence>& occurrences,
    Bnd_Box& globalBounds,
    std::uint64_t& totalTriangles)
{
    return EmitSolidOccurrencesFromShape(
        sourceLabel,
        effectiveLabel,
        sourceShape,
        colorTool,
        visMatTool,
        parentWorldLoc,
        solidCache,
        occurrences,
        globalBounds,
        totalTriangles,
        TopAbs_SHELL);
}

static void TraverseLabelToSolids(
    const Handle(XCAFDoc_ShapeTool)& shapeTool,
    const Handle(XCAFDoc_ColorTool)& colorTool,
    const Handle(XCAFDoc_VisMaterialTool)& visMatTool,
    const TDF_Label& label,
    const TopLoc_Location& parentLoc,
    std::unordered_map<SolidCacheKey, SolidCacheEntry, SolidCacheKeyHasher>& solidCache,
    std::vector<Occurrence>& occurrences,
    Bnd_Box& globalBounds,
    std::size_t& traversedLeafLabels,
    std::size_t& labelsWithNoRenderableGeometry,
    std::size_t& labelsWithShellFallback,
    std::size_t& nonAssemblyLabelsWithComponents,
    std::vector<NonRenderableLeafInfo>& nonRenderableLeaves,
    std::uint64_t& totalTriangles)
{
    if (label.IsNull())
        return;

    TDF_Label effectiveLabel = label;
    TopLoc_Location instanceLoc;

    if (shapeTool->IsReference(label))
    {
        shapeTool->GetReferredShape(label, effectiveLabel);
        instanceLoc = shapeTool->GetLocation(label);
    }

    const TopLoc_Location worldLoc = parentLoc * instanceLoc;

    if (shapeTool->IsAssembly(effectiveLabel))
    {
        TDF_LabelSequence components;
        shapeTool->GetComponents(effectiveLabel, components);

        for (Standard_Integer i = 1; i <= components.Length(); ++i)
        {
            TraverseLabelToSolids(
                shapeTool,
                colorTool,
                visMatTool,
                components.Value(i),
                worldLoc,
                solidCache,
                occurrences,
                globalBounds,
                traversedLeafLabels,
                labelsWithNoRenderableGeometry,
                labelsWithShellFallback,
                nonAssemblyLabelsWithComponents,
                nonRenderableLeaves,
                totalTriangles);
        }
        return;
    }

    const TopoDS_Shape shape = shapeTool->GetShape(effectiveLabel);
    if (shape.IsNull())
    {
        std::cerr << "Warning: null shape for label " << LabelToString(effectiveLabel) << "\n";
        return;
    }

    TDF_LabelSequence probeComponents;
    shapeTool->GetComponents(effectiveLabel, probeComponents);
    if (probeComponents.Length() > 0)
    {
        ++nonAssemblyLabelsWithComponents;
        if (g_verboseLogging)
        {
            std::cerr << "Warning: non-assembly label has components"
                      << " label=" << LabelToString(effectiveLabel)
                      << " type=" << ShapeTypeName(shape.ShapeType())
                      << " componentCount=" << probeComponents.Length()
                      << "\n";
        }
    }

    ++traversedLeafLabels;

    if (g_verboseLogging && (traversedLeafLabels % 1000u) == 0u)
    {
        std::cout << "[Debug] traversedLeafLabels=" << traversedLeafLabels
                  << " occurrences=" << occurrences.size() << "\n";
    }

    const std::size_t beforeCount = occurrences.size();
    const std::size_t emittedSolids = EmitSolidOccurrencesFromShape(
        label,
        effectiveLabel,
        shape,
        colorTool,
        visMatTool,
        worldLoc,
        solidCache,
        occurrences,
        globalBounds,
        totalTriangles);

    std::size_t emitted = emittedSolids;
    if (emittedSolids == 0)
    {
        emitted = EmitShellOccurrencesFromShape(
            label,
            effectiveLabel,
            shape,
            colorTool,
            visMatTool,
            worldLoc,
            solidCache,
            occurrences,
            globalBounds,
            totalTriangles);
    }

    if (emitted == 0)
    {
        const int shellCount = CountSubshapes(shape, TopAbs_SHELL);
        const int faceCount = CountSubshapes(shape, TopAbs_FACE);

        NonRenderableLeafInfo info;
        info.Label = LabelToString(effectiveLabel);
        info.Name = GetLabelName(effectiveLabel);
        info.Type = shape.ShapeType();
        info.ShellCount = shellCount;
        info.FaceCount = faceCount;
        nonRenderableLeaves.push_back(std::move(info));

        if (g_verboseLogging)
        {
            ++labelsWithNoRenderableGeometry;

            std::cerr << "Warning: no solids found for leaf label "
                      << LabelToString(effectiveLabel);

            const std::string name = GetLabelName(effectiveLabel);
            if (!name.empty())
                std::cerr << " name=\"" << name << "\"";

            std::cerr << " type=" << ShapeTypeName(shape.ShapeType())
                      << " shells=" << shellCount
                      << " faces=" << faceCount
                      << "\n";
        }
    }
    else
    {
        if (emittedSolids == 0)
        {
            ++labelsWithShellFallback;

            if (g_verboseLogging)
            {
                std::cerr << "Info: emitted shell fallback for leaf label "
                          << LabelToString(effectiveLabel)
                          << " shells=" << emitted
                          << "\n";
            }
        }

        const std::size_t added = occurrences.size() - beforeCount;
        if (added != emitted)
        {
            std::cerr << "Warning: emitted solid mismatch for label "
                      << LabelToString(effectiveLabel)
                      << " emitted=" << emitted
                      << " added=" << added
                      << "\n";
        }
    }
}

static bool LoadStepFileIntoDoc(
    const std::filesystem::path& stepPath,
    const Handle(TDocStd_Document)& targetDoc)
{
    if (targetDoc.IsNull())
    {
        std::cerr << "Target document is null\n";
        return false;
    }

    STEPCAFControl_Reader reader;

    reader.SetColorMode(Standard_True);
    reader.SetNameMode(Standard_True);
    reader.SetLayerMode(Standard_False);
    reader.SetPropsMode(Standard_True);
    reader.SetMatMode(Standard_True);
    reader.SetGDTMode(Standard_False);
    reader.SetSHUOMode(Standard_True);

    IFSelect_ReturnStatus status = reader.ReadFile(stepPath.string().c_str());
    if (status != IFSelect_RetDone)
    {
        std::cerr << "Failed to read STEP file: " << stepPath << "\n";
        return false;
    }

    STEPControl_Reader& baseReader = reader.ChangeReader();

    if (!reader.Transfer(targetDoc))
    {
        std::cerr << "STEP transfer failed for: " << stepPath << "\n";
        baseReader.PrintCheckTransfer(Standard_False, IFSelect_ItemsByEntity);
        return false;
    }

    return true;
}

int main(int argc, char** argv)
{
    try
    {
        CliOptions cli;
        int cliExitCode = 0;
        if (!ParseCli(argc, argv, cli, cliExitCode))
        {
            return cliExitCode;
        }

        g_verboseLogging = cli.verbose;
        glbopt::SetVerboseLogging(cli.verbose);

        const std::filesystem::path inputPath = cli.inputPath;
        std::vector<std::filesystem::path> stepFiles = CollectStepFiles(inputPath);

        if (stepFiles.empty())
        {
            std::cerr << "No .stp/.step files found at: " << inputPath << "\n";
            return 2;
        }

        Handle(XCAFApp_Application) app = XCAFApp_Application::GetApplication();
        if (app.IsNull())
        {
            std::cerr << "Failed to get XCAF application\n";
            return 1;
        }

        std::vector<Handle(TDocStd_Document)> docs;
        docs.reserve(stepFiles.size());

        std::vector<Handle(XCAFDoc_ShapeTool)> shapeTools;
        shapeTools.reserve(stepFiles.size());

        std::vector<Handle(XCAFDoc_ColorTool)> colorTools;
        colorTools.reserve(stepFiles.size());

        std::vector<Handle(XCAFDoc_VisMaterialTool)> visMatTools;
        visMatTools.reserve(stepFiles.size());

        for (const std::filesystem::path& p : stepFiles)
        {
            std::cout << "Opening STEP: " << p << "\n";
            Handle(TDocStd_Document) doc;
            app->NewDocument("MDTV-XCAF", doc);
            if (doc.IsNull())
            {
                std::cerr << "Failed to create XCAF document for: " << p << "\n";
                return 1;
            }

            if (!LoadStepFileIntoDoc(p, doc))
            {
                std::cerr << "Load failed for: " << p << "\n";
                return 1;
            }

            Handle(XCAFDoc_ShapeTool) shapeTool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
            Handle(XCAFDoc_ColorTool) colorTool = XCAFDoc_DocumentTool::ColorTool(doc->Main());
            Handle(XCAFDoc_VisMaterialTool) visMatTool = XCAFDoc_DocumentTool::VisMaterialTool(doc->Main());
            if (shapeTool.IsNull())
            {
                std::cerr << "Failed to get XCAFDoc_ShapeTool for: " << p << "\n";
                return 1;
            }

            docs.push_back(doc);
            shapeTools.push_back(shapeTool);
            colorTools.push_back(colorTool);
            visMatTools.push_back(visMatTool);
        }

        if (g_verboseLogging)
        {
            std::cout << "[Stage] Begin solid traversal\n";
        }

        std::vector<Occurrence> occurrences;
        Bnd_Box globalBounds;
        globalBounds.SetVoid();

        std::size_t traversedLeafLabels = 0;
        std::size_t labelsWithNoRenderableGeometry = 0;
        std::size_t labelsWithShellFallback = 0;
        std::size_t nonAssemblyLabelsWithComponents = 0;
        std::vector<NonRenderableLeafInfo> nonRenderableLeaves;
        std::uint64_t totalTriangles = 0;
        std::size_t totalRoots = 0;
        std::unordered_map<SolidCacheKey, SolidCacheEntry, SolidCacheKeyHasher> solidCache;

        const TopLoc_Location identity;
        for (std::size_t docIndex = 0; docIndex < shapeTools.size(); ++docIndex)
        {
            TDF_LabelSequence roots;
            shapeTools[docIndex]->GetFreeShapes(roots);
            totalRoots += static_cast<std::size_t>(roots.Length());

            for (Standard_Integer i = 1; i <= roots.Length(); ++i)
            {
                if (g_verboseLogging)
                {
                    std::cout << "[Debug] traversing doc " << (docIndex + 1)
                              << " root " << i << " of " << roots.Length() << "\n";
                }
                TraverseLabelToSolids(
                    shapeTools[docIndex],
                    colorTools[docIndex],
                    visMatTools[docIndex],
                    roots.Value(i),
                    identity,
                    solidCache,
                    occurrences,
                    globalBounds,
                    traversedLeafLabels,
                    labelsWithNoRenderableGeometry,
                    labelsWithShellFallback,
                    nonAssemblyLabelsWithComponents,
                    nonRenderableLeaves,
                    totalTriangles);
            }
        }

        if (g_verboseLogging)
        {
            std::cout << "[Stage] Solid traversal complete\n";
            std::cout << "Found " << totalRoots << " free roots\n";
            std::cout << "Traversed " << traversedLeafLabels << " non-assembly labels\n";
            std::cout << "Built " << occurrences.size() << " occurrences\n";
            std::cout << "Estimated solids tris=" << totalTriangles << "\n";
            std::cout << "Labels with shell fallback: " << labelsWithShellFallback << "\n";
            std::cout << "Labels with no renderable geometry: " << labelsWithNoRenderableGeometry << "\n";
            std::cout << "Non-assembly labels with components (probe): "
                      << nonAssemblyLabelsWithComponents << "\n";
            if (!nonRenderableLeaves.empty())
            {
                std::cout << "Non-renderable leaves:\n";
                const std::size_t maxToPrint = 20;
                const std::size_t printCount = std::min(maxToPrint, nonRenderableLeaves.size());

                for (std::size_t i = 0; i < printCount; ++i)
                {
                    const NonRenderableLeafInfo& info = nonRenderableLeaves[i];
                    std::cout << "  - label=" << info.Label;
                    if (!info.Name.empty())
                    {
                        std::cout << " name=\"" << info.Name << "\"";
                    }
                    std::cout << " type=" << ShapeTypeName(info.Type)
                              << " shells=" << info.ShellCount
                              << " faces=" << info.FaceCount
                              << "\n";
                }

                if (nonRenderableLeaves.size() > printCount)
                {
                    std::cout << "  ... (" << (nonRenderableLeaves.size() - printCount)
                              << " more)\n";
                }
            }
            std::cout << "Global bounds: ";
            PrintBounds(globalBounds);
            std::cout << "\n\n";

            std::size_t shapeColorCount = 0;
            std::size_t shapeMaterialCount = 0;
            std::size_t faceColorCount = 0;
            std::size_t faceMaterialCount = 0;
            std::size_t occurrencesWithAnyFaceStyle = 0;
            std::size_t totalFaceSlots = 0;

            for (const Occurrence& occ : occurrences)
            {
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

                bool hasAnyFaceStyle = false;
                totalFaceSlots += occ.Appearance->Faces.size();
                for (const CachedFaceAppearance& fa : occ.Appearance->Faces)
                {
                    if (fa.Colors.HasGen || fa.Colors.HasSurf || fa.Colors.HasCurv)
                    {
                        ++faceColorCount;
                        hasAnyFaceStyle = true;
                    }
                    if (!fa.VisMaterial.IsNull())
                    {
                        ++faceMaterialCount;
                        hasAnyFaceStyle = true;
                    }
                }

                if (hasAnyFaceStyle)
                {
                    ++occurrencesWithAnyFaceStyle;
                }
            }

            std::cout << "[AppearanceProbe] occurrences=" << occurrences.size()
                      << " shapeColor=" << shapeColorCount
                      << " shapeMaterial=" << shapeMaterialCount
                      << " faceColorEntries=" << faceColorCount
                      << " faceMaterialEntries=" << faceMaterialCount
                      << " occWithFaceStyle=" << occurrencesWithAnyFaceStyle
                      << " faceSlots=" << totalFaceSlots
                      << "\n\n";

        }
 
        std::cout << "[Stage] Begin octree build\n";

        const double rootMaxSide = BoundsMaxSideLength(globalBounds);

        TileOctree::Config cfg;
        cfg.maxDepth = 10;
        cfg.maxItemsPerNode = 96;
        cfg.maxTrianglesPerNode = 30000;
        cfg.minNodeMaxSide = std::max(1e-6, rootMaxSide * 1e-3);
        cfg.looseFactor = 1.8;
        cfg.verbose = cli.verbose;

        TileOctree tree(cfg);
        tree.Build(occurrences, globalBounds);

        std::cout << "[Stage] Begin tile export\n";

        TilesetEmit::Options opt;
        opt.tilesetOutDir = cli.outDir.string();
        opt.contentSubdir = cli.contentSubdir;
        opt.tileFilePrefix = cli.tilePrefix;
        opt.keepGlbFilesForDebug = cli.keepGlb;
        opt.debugAppearance = cli.verbose;
        opt.useTightBounds = cli.useTightBounds;
        opt.contentOnlyAtLeaves = cli.contentOnlyAtLeaves;

        if (g_verboseLogging)
        {
            std::cout << "[Config] outDir=" << std::filesystem::absolute(cli.outDir)
                      << " contentSubdir=" << opt.contentSubdir
                      << " tilePrefix=" << opt.tileFilePrefix
                      << " keepGlb=" << (opt.keepGlbFilesForDebug ? "true" : "false")
                      << "\n";
        }

        const bool emitOk = TilesetEmit::EmitTilesetAndB3dm(tree, occurrences, opt);
        if (!emitOk)
        {
            std::cerr << "[Error] Tile export failed\n";
            return 1;
        }

        std::cout << "[Stage] Tile export complete\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Fatal exception: " << e.what() << "\n";
        return 1;
    }
    catch (...)
    {
        std::cerr << "Fatal exception: unknown\n";
        return 1;
    }
}
