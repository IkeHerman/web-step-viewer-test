#include "step_traversal.h"

#include <STEPCAFControl_Reader.hxx>
#include <IFSelect_PrintCount.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <STEPControl_Reader.hxx>

#include <TCollection_AsciiString.hxx>
#include <TDataStd_Name.hxx>
#include <TDF_Tool.hxx>
#include <TDocStd_Document.hxx>

#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>

#include <BRepBndLib.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <ShapeUpgrade_RemoveLocations.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFDoc_VisMaterial.hxx>
#include <XCAFDoc_VisMaterialTool.hxx>

#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <unordered_map>

namespace
{
std::string LabelToString(const TDF_Label& label)
{
    TCollection_AsciiString entry;
    TDF_Tool::Entry(label, entry);
    return std::string(entry.ToCString());
}

std::string GetLabelName(const TDF_Label& label)
{
    Handle(TDataStd_Name) nameAttr;
    if (label.FindAttribute(TDataStd_Name::GetID(), nameAttr) && !nameAttr.IsNull())
    {
        TCollection_AsciiString ascii(nameAttr->Get());
        return std::string(ascii.ToCString());
    }
    return std::string();
}

Bnd_Box ComputeLocalBounds(const TopoDS_Shape& shape)
{
    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    return box;
}

void UpdateGlobalBounds(Bnd_Box& globalBox, const Bnd_Box& newBox)
{
    if (newBox.IsVoid())
    {
        return;
    }
    Standard_Real xmin, ymin, zmin, xmax, ymax, zmax;
    newBox.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    globalBox.Update(xmin, ymin, zmin, xmax, ymax, zmax);
}

int CountSubshapes(const TopoDS_Shape& s, TopAbs_ShapeEnum what)
{
    int count = 0;
    for (TopExp_Explorer ex(s, what); ex.More(); ex.Next())
    {
        ++count;
    }
    return count;
}

std::uint32_t CountFaces(const TopoDS_Shape& shape)
{
    std::size_t count = 0;
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next())
    {
        ++count;
    }
    if (count > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
    {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(count);
}

std::uint32_t CountExistingTrianglesOnly(const TopoDS_Shape& shape)
{
    std::size_t triangleCount = 0;

    for (TopExp_Explorer exp(shape, TopAbs_FACE); exp.More(); exp.Next())
    {
        const TopoDS_Face& face = TopoDS::Face(exp.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
        if (!tri.IsNull())
        {
            triangleCount += static_cast<std::size_t>(tri->NbTriangles());
        }
    }

    if (triangleCount > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
    {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(triangleCount);
}

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

core::Transform4d ToTransform4d(const gp_Trsf& trsf)
{
    core::Transform4d out;
    out.m[0] = trsf.Value(1, 1);  out.m[1] = trsf.Value(1, 2);  out.m[2] = trsf.Value(1, 3);  out.m[3] = trsf.Value(1, 4);
    out.m[4] = trsf.Value(2, 1);  out.m[5] = trsf.Value(2, 2);  out.m[6] = trsf.Value(2, 3);  out.m[7] = trsf.Value(2, 4);
    out.m[8] = trsf.Value(3, 1);  out.m[9] = trsf.Value(3, 2);  out.m[10] = trsf.Value(3, 3); out.m[11] = trsf.Value(3, 4);
    out.m[12] = 0.0;              out.m[13] = 0.0;              out.m[14] = 0.0;             out.m[15] = 1.0;
    return out;
}

std::string MaterialSignature(const CachedOccurrenceAppearance* appearance)
{
    if (appearance == nullptr)
    {
        return "no_appearance";
    }

    std::ostringstream ss;
    const CachedColorSet& c = appearance->ResolvedShapeColors;
    ss << "shapeColors:" << c.HasGen << ":" << c.HasSurf << ":" << c.HasCurv << ";";
    if (c.HasGen) { ss << "gen:" << c.Gen.Red() << "," << c.Gen.Green() << "," << c.Gen.Blue() << ";"; }
    if (c.HasSurf) { ss << "surf:" << c.Surf.Red() << "," << c.Surf.Green() << "," << c.Surf.Blue() << ";"; }
    if (c.HasCurv) { ss << "curv:" << c.Curv.Red() << "," << c.Curv.Green() << "," << c.Curv.Blue() << ";"; }
    ss << "shapeMatPresent:" << (!appearance->ResolvedShapeMaterial.IsNull() ? 1 : 0) << ";";
    ss << "faceCount:" << appearance->Faces.size() << ";";
    for (const CachedFaceAppearance& face : appearance->Faces)
    {
        ss << "f(" << face.Colors.HasGen << "," << face.Colors.HasSurf << "," << face.Colors.HasCurv << ")";
        if (face.Colors.HasGen) { ss << "g:" << face.Colors.Gen.Red() << "," << face.Colors.Gen.Green() << "," << face.Colors.Gen.Blue(); }
        if (face.Colors.HasSurf) { ss << "s:" << face.Colors.Surf.Red() << "," << face.Colors.Surf.Green() << "," << face.Colors.Surf.Blue(); }
        if (face.Colors.HasCurv) { ss << "c:" << face.Colors.Curv.Red() << "," << face.Colors.Curv.Green() << "," << face.Colors.Curv.Blue(); }
        ss << "mPresent:" << (!face.VisMaterial.IsNull() ? 1 : 0) << ";";
    }
    return ss.str();
}

/// Flattens nested `TopLoc_Location` into geometry for more stable bounds/topology when keying.
/// Export paths still use the original shape; this is only for prototype deduplication signatures.
TopoDS_Shape ShapeWithLocationsRemovedForKeying(const TopoDS_Shape& shape)
{
    if (shape.IsNull())
    {
        return shape;
    }
    ShapeUpgrade_RemoveLocations remover;
    remover.SetRemoveLevel(TopAbs_SHAPE);
    if (!remover.Remove(shape))
    {
        return shape;
    }
    const TopoDS_Shape out = remover.GetResult();
    return out.IsNull() ? shape : out;
}

/// Coarse geometry signature for prototype keys and face seeds.
/// Translation-invariant: uses axis-aligned **extents** (size) instead of absolute bbox corners.
std::string CoarseGeometrySignatureForShape(const TopoDS_Shape& shape)
{
    const TopoDS_Shape sigShape = ShapeWithLocationsRemovedForKeying(shape);
    const core::Aabb aabb = ToAabb(ComputeLocalBounds(sigShape));
    int edgeCount = 0;
    int vertexCount = 0;
    int faceCount = 0;
    for (TopExp_Explorer ex(sigShape, TopAbs_FACE); ex.More(); ex.Next()) { ++faceCount; }
    for (TopExp_Explorer ex(sigShape, TopAbs_EDGE); ex.More(); ex.Next()) { ++edgeCount; }
    for (TopExp_Explorer ex(sigShape, TopAbs_VERTEX); ex.More(); ex.Next()) { ++vertexCount; }

    std::ostringstream ss;
    ss << std::setprecision(17) << std::scientific;
    ss << "type:" << static_cast<int>(sigShape.ShapeType())
       << "|ori:" << static_cast<int>(sigShape.Orientation())
       << "|faces:" << faceCount
       << "|edges:" << edgeCount
       << "|verts:" << vertexCount;
    if (!aabb.valid)
    {
        ss << "|extents:void";
    }
    else
    {
        const double dx = aabb.xmax - aabb.xmin;
        const double dy = aabb.ymax - aabb.ymin;
        const double dz = aabb.zmax - aabb.zmin;
        ss << "|extents:" << dx << "," << dy << "," << dz;
    }
    return ss.str();
}

std::string FaceMaterialSignature(const CachedOccurrenceAppearance* appearance, const std::size_t faceIndex)
{
    if (appearance == nullptr)
    {
        return "no_appearance";
    }

    std::ostringstream ss;
    if (faceIndex < appearance->Faces.size())
    {
        const CachedFaceAppearance& fa = appearance->Faces[faceIndex];
        ss << "face(" << fa.Colors.HasGen << "," << fa.Colors.HasSurf << "," << fa.Colors.HasCurv << ")";
        if (fa.Colors.HasGen) { ss << "|gen:" << fa.Colors.Gen.Red() << "," << fa.Colors.Gen.Green() << "," << fa.Colors.Gen.Blue(); }
        if (fa.Colors.HasSurf) { ss << "|surf:" << fa.Colors.Surf.Red() << "," << fa.Colors.Surf.Green() << "," << fa.Colors.Surf.Blue(); }
        if (fa.Colors.HasCurv) { ss << "|curv:" << fa.Colors.Curv.Red() << "," << fa.Colors.Curv.Green() << "," << fa.Colors.Curv.Blue(); }
        ss << "|matPresent:" << (!fa.VisMaterial.IsNull() ? 1 : 0);
        return ss.str();
    }

    const CachedColorSet& c = appearance->ResolvedShapeColors;
    ss << "shapeFallback(" << c.HasGen << "," << c.HasSurf << "," << c.HasCurv << ")";
    ss << "|shapeMatPresent:" << (!appearance->ResolvedShapeMaterial.IsNull() ? 1 : 0);
    return ss.str();
}

std::uint32_t SafeEstimateTriangles(
    const TopoDS_Shape& shape,
    const TDF_Label& sourceLabel)
{
    try
    {
        const std::uint32_t existing = CountExistingTrianglesOnly(shape);
        if (existing > 0)
        {
            return existing;
        }

        const int faceCount = CountSubshapes(shape, TopAbs_FACE);
        if (faceCount <= 0)
        {
            return 0;
        }
        const std::uint64_t estimate = static_cast<std::uint64_t>(faceCount) * 24ull;
        if (estimate > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))
        {
            return std::numeric_limits<std::uint32_t>::max();
        }
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

CachedColorSet ResolveColorsForOccurrence(
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

Handle(XCAFDoc_VisMaterial) ResolveVisMaterialForOccurrence(
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

std::vector<CachedFaceAppearance> ExtractFaceAppearance(
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

void PromoteFaceColorToShapeFallback(CachedOccurrenceAppearance& appearance)
{
    CachedColorSet& shape = appearance.ResolvedShapeColors;
    if (shape.HasGen || shape.HasSurf || shape.HasCurv)
    {
        return;
    }
    for (const CachedFaceAppearance& face : appearance.Faces)
    {
        if (face.Colors.HasSurf) { shape.HasSurf = true; shape.Surf = face.Colors.Surf; return; }
        if (face.Colors.HasGen) { shape.HasGen = true; shape.Gen = face.Colors.Gen; return; }
        if (face.Colors.HasCurv) { shape.HasCurv = true; shape.Curv = face.Colors.Curv; return; }
    }
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

SolidCacheKey MakeSolidCacheKey(const TopoDS_Shape& localSolid)
{
    Handle(TopoDS_TShape) tshape = localSolid.TShape();
    return SolidCacheKey{ static_cast<const void*>(tshape.operator->()), localSolid.Orientation() };
}

std::size_t EmitSolidOccurrencesFromShape(
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
        {
            continue;
        }

        const TopLoc_Location emittedWorldLoc = parentWorldLoc * emittedShape.Location();
        const gp_Trsf emittedWorldTrsf = emittedWorldLoc.Transformation();
        const TopoDS_Shape emittedShapeAtLocalOrigin = emittedShape.Located(TopLoc_Location());

        const SolidCacheKey cacheKey = MakeSolidCacheKey(emittedShapeAtLocalOrigin);
        auto it = solidCache.find(cacheKey);
        if (it == solidCache.end())
        {
            SolidCacheEntry entry;
            entry.LocalBounds = ComputeLocalBounds(emittedShapeAtLocalOrigin);
            entry.TriangleCount = SafeEstimateTriangles(emittedShapeAtLocalOrigin, sourceLabel);
            it = solidCache.emplace(cacheKey, std::move(entry)).first;
        }

        const SolidCacheEntry& cached = it->second;
        std::shared_ptr<CachedOccurrenceAppearance> appearance = std::make_shared<CachedOccurrenceAppearance>();
        appearance->ResolvedShapeColors = ResolveColorsForOccurrence(
            colorTool, sourceLabel, effectiveLabel, sourceShape, emittedShapeAtLocalOrigin);
        appearance->ResolvedShapeMaterial = ResolveVisMaterialForOccurrence(
            visMatTool, sourceLabel, effectiveLabel, sourceShape, emittedShapeAtLocalOrigin);
        appearance->Faces = ExtractFaceAppearance(colorTool, visMatTool, emittedShapeAtLocalOrigin);
        PromoteFaceColorToShapeFallback(*appearance);

        const Bnd_Box emittedWorldBounds = cached.LocalBounds.Transformed(emittedWorldTrsf);
        UpdateGlobalBounds(globalBounds, emittedWorldBounds);

        Occurrence occ;
        occ.Label = sourceLabel;
        occ.EffectiveLabel = effectiveLabel;
        occ.SourceLabelName = GetLabelName(effectiveLabel);
        occ.Shape = emittedShapeAtLocalOrigin;
        occ.WorldTransform = emittedWorldTrsf;
        occ.WorldBounds = emittedWorldBounds;
        occ.TriangleCount = cached.TriangleCount;
        occ.SourceFaceCount = CountFaces(emittedShapeAtLocalOrigin);
        occ.SourceLabelEntry = LabelToString(effectiveLabel);
        occ.HasAnyMetadata = (!occ.SourceLabelName.empty() || !occ.SourceLabelEntry.empty());
        occ.WorldTransformMatrix = ToTransform4d(emittedWorldTrsf);
        occ.LocalBoundsAabb = ToAabb(cached.LocalBounds);
        occ.WorldBoundsAabb = ToAabb(emittedWorldBounds);
        occ.GeometryKey = CoarseGeometrySignatureForShape(emittedShapeAtLocalOrigin);
        occ.MaterialKey = MaterialSignature(appearance.get());
        occ.QualifiedPrototypeKey = occ.GeometryKey + "|mat:" + occ.MaterialKey;
        occ.FromExplicitReference = (!sourceLabel.IsNull() && !effectiveLabel.IsNull() && sourceLabel != effectiveLabel);
        occ.FacePrototypeSeeds.clear();
        std::size_t faceIndex = 0;
        for (TopExp_Explorer faceExp(emittedShapeAtLocalOrigin, TopAbs_FACE); faceExp.More(); faceExp.Next(), ++faceIndex)
        {
            const TopoDS_Shape childShape = faceExp.Current().Located(TopLoc_Location());
            if (childShape.IsNull())
            {
                continue;
            }

            Occurrence::FacePrototypeSeed seed;
            seed.GeometryKey = CoarseGeometrySignatureForShape(childShape);
            seed.MaterialKey = FaceMaterialSignature(appearance.get(), faceIndex);
            seed.TriangleCount = CountExistingTrianglesOnly(childShape);
            seed.LocalBounds = ToAabb(ComputeLocalBounds(childShape));
            occ.FacePrototypeSeeds.push_back(std::move(seed));
        }
        occ.Appearance = appearance;
        occurrences.push_back(occ);

        totalTriangles += cached.TriangleCount;
        ++emitted;

    }
    return emitted;
}

void TraverseLabelToSolids(
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
    std::vector<importers::NonRenderableLeafInfo>& nonRenderableLeaves,
    std::uint64_t& totalTriangles)
{
    if (label.IsNull())
    {
        return;
    }

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
                shapeTool, colorTool, visMatTool, components.Value(i), worldLoc, solidCache,
                occurrences, globalBounds, traversedLeafLabels, labelsWithNoRenderableGeometry,
                labelsWithShellFallback, nonAssemblyLabelsWithComponents, nonRenderableLeaves,
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
    }

    ++traversedLeafLabels;
    const std::size_t beforeCount = occurrences.size();

    const std::size_t emittedSolids = EmitSolidOccurrencesFromShape(
        label, effectiveLabel, shape, colorTool, visMatTool, worldLoc, solidCache,
        occurrences, globalBounds, totalTriangles, TopAbs_SOLID);

    std::size_t emitted = emittedSolids;
    if (emittedSolids == 0)
    {
        emitted = EmitSolidOccurrencesFromShape(
            label, effectiveLabel, shape, colorTool, visMatTool, worldLoc, solidCache,
            occurrences, globalBounds, totalTriangles, TopAbs_SHELL);
    }

    if (emitted == 0)
    {
        importers::NonRenderableLeafInfo info;
        info.Label = LabelToString(effectiveLabel);
        info.Name = GetLabelName(effectiveLabel);
        info.Type = shape.ShapeType();
        info.ShellCount = CountSubshapes(shape, TopAbs_SHELL);
        info.FaceCount = CountSubshapes(shape, TopAbs_FACE);
        nonRenderableLeaves.push_back(std::move(info));
        ++labelsWithNoRenderableGeometry;
        return;
    }

    if (emittedSolids == 0)
    {
        ++labelsWithShellFallback;
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

bool LoadStepFileIntoDoc(
    const std::filesystem::path& stepPath,
    const Handle(TDocStd_Document)& targetDoc,
    const bool occtVerbose)
{
    if (targetDoc.IsNull())
    {
        std::cerr << "Target document is null\n";
        return false;
    }

    STEPCAFControl_Reader reader;
    reader.SetColorMode(Standard_True);
    reader.SetNameMode(Standard_True);
    reader.SetLayerMode(Standard_True);
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

    if (occtVerbose)
    {
        baseReader.PrintCheckTransfer(Standard_False, IFSelect_ItemsByEntity);
    }

    return true;
}
}

namespace importers
{
bool CollectStepOccurrencesFromFiles(
    const std::vector<std::filesystem::path>& stepFiles,
    const bool occtVerbose,
    std::vector<Occurrence>& occurrences,
    Bnd_Box& globalBounds,
    std::size_t& traversedLeafLabels,
    std::size_t& labelsWithNoRenderableGeometry,
    std::size_t& labelsWithShellFallback,
    std::size_t& nonAssemblyLabelsWithComponents,
    std::vector<NonRenderableLeafInfo>& nonRenderableLeaves,
    std::uint64_t& totalTriangles,
    std::size_t& totalRoots)
{
    Handle(XCAFApp_Application) app = XCAFApp_Application::GetApplication();
    if (app.IsNull())
    {
        std::cerr << "Failed to get XCAF application\n";
        return false;
    }

    std::vector<Handle(XCAFDoc_ShapeTool)> shapeTools;
    std::vector<Handle(XCAFDoc_ColorTool)> colorTools;
    std::vector<Handle(XCAFDoc_VisMaterialTool)> visMatTools;
    shapeTools.reserve(stepFiles.size());
    colorTools.reserve(stepFiles.size());
    visMatTools.reserve(stepFiles.size());

    for (const std::filesystem::path& p : stepFiles)
    {
        std::cout << "Opening STEP: " << p << "\n";
        Handle(TDocStd_Document) doc;
        app->NewDocument("MDTV-XCAF", doc);
        if (doc.IsNull() || !LoadStepFileIntoDoc(p, doc, occtVerbose))
        {
            std::cerr << "Load failed for: " << p << "\n";
            return false;
        }

        Handle(XCAFDoc_ShapeTool) shapeTool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
        Handle(XCAFDoc_ColorTool) colorTool = XCAFDoc_DocumentTool::ColorTool(doc->Main());
        Handle(XCAFDoc_VisMaterialTool) visMatTool = XCAFDoc_DocumentTool::VisMaterialTool(doc->Main());
        if (shapeTool.IsNull())
        {
            std::cerr << "Failed to get XCAFDoc_ShapeTool for: " << p << "\n";
            return false;
        }
        shapeTools.push_back(shapeTool);
        colorTools.push_back(colorTool);
        visMatTools.push_back(visMatTool);
    }

    std::unordered_map<SolidCacheKey, SolidCacheEntry, SolidCacheKeyHasher> solidCache;
    const TopLoc_Location identity;
    for (std::size_t docIndex = 0; docIndex < shapeTools.size(); ++docIndex)
    {
        TDF_LabelSequence roots;
        shapeTools[docIndex]->GetFreeShapes(roots);
        totalRoots += static_cast<std::size_t>(roots.Length());
        for (Standard_Integer i = 1; i <= roots.Length(); ++i)
        {
            TraverseLabelToSolids(
                shapeTools[docIndex], colorTools[docIndex], visMatTools[docIndex], roots.Value(i),
                identity, solidCache, occurrences, globalBounds, traversedLeafLabels,
                labelsWithNoRenderableGeometry, labelsWithShellFallback, nonAssemblyLabelsWithComponents,
                nonRenderableLeaves, totalTriangles);
        }
    }

    return true;
}
} // namespace importers
