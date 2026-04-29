#include "step_to_scene_ir.h"

#include <TDF_Tool.hxx>
#include <TCollection_AsciiString.hxx>
#include <TopAbs_Orientation.hxx>
#include <BRepBndLib.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>

#include <algorithm>
#include <limits>
#include <sstream>
#include <unordered_map>

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

core::Transform4d ToTransform4d(const gp_Trsf& trsf)
{
    core::Transform4d out;
    out.m[0] = trsf.Value(1, 1);  out.m[1] = trsf.Value(1, 2);  out.m[2] = trsf.Value(1, 3);  out.m[3] = trsf.Value(1, 4);
    out.m[4] = trsf.Value(2, 1);  out.m[5] = trsf.Value(2, 2);  out.m[6] = trsf.Value(2, 3);  out.m[7] = trsf.Value(2, 4);
    out.m[8] = trsf.Value(3, 1);  out.m[9] = trsf.Value(3, 2);  out.m[10] = trsf.Value(3, 3); out.m[11] = trsf.Value(3, 4);
    out.m[12] = 0.0;              out.m[13] = 0.0;              out.m[14] = 0.0;             out.m[15] = 1.0;
    return out;
}

Bnd_Box ComputeLocalBounds(const TopoDS_Shape& shape)
{
    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    return box;
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
    if (c.HasGen)
    {
        ss << "gen:" << c.Gen.Red() << "," << c.Gen.Green() << "," << c.Gen.Blue() << ";";
    }
    if (c.HasSurf)
    {
        ss << "surf:" << c.Surf.Red() << "," << c.Surf.Green() << "," << c.Surf.Blue() << ";";
    }
    if (c.HasCurv)
    {
        ss << "curv:" << c.Curv.Red() << "," << c.Curv.Green() << "," << c.Curv.Blue() << ";";
    }

    ss << "shapeMatPresent:" << (!appearance->ResolvedShapeMaterial.IsNull() ? 1 : 0) << ";";
    ss << "faceCount:" << appearance->Faces.size() << ";";
    for (const CachedFaceAppearance& face : appearance->Faces)
    {
        ss << "f(" << face.Colors.HasGen << "," << face.Colors.HasSurf << "," << face.Colors.HasCurv << ")";
        if (face.Colors.HasGen)
        {
            ss << "g:" << face.Colors.Gen.Red() << "," << face.Colors.Gen.Green() << "," << face.Colors.Gen.Blue();
        }
        if (face.Colors.HasSurf)
        {
            ss << "s:" << face.Colors.Surf.Red() << "," << face.Colors.Surf.Green() << "," << face.Colors.Surf.Blue();
        }
        if (face.Colors.HasCurv)
        {
            ss << "c:" << face.Colors.Curv.Red() << "," << face.Colors.Curv.Green() << "," << face.Colors.Curv.Blue();
        }
        ss << "mPresent:" << (!face.VisMaterial.IsNull() ? 1 : 0) << ";";
    }

    return ss.str();
}

std::string GeometrySignature(const Occurrence& occ)
{
    const TopoDS_Shape& shape = occ.Shape;
    Bnd_Box localBounds = ComputeLocalBounds(shape);
    core::Aabb aabb = ToAabb(localBounds);

    int faceCount = 0;
    int edgeCount = 0;
    int vertexCount = 0;
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) { ++faceCount; }
    for (TopExp_Explorer ex(shape, TopAbs_EDGE); ex.More(); ex.Next()) { ++edgeCount; }
    for (TopExp_Explorer ex(shape, TopAbs_VERTEX); ex.More(); ex.Next()) { ++vertexCount; }

    std::ostringstream ss;
    ss << "type:" << static_cast<int>(shape.ShapeType())
       << "|ori:" << static_cast<int>(shape.Orientation())
       << "|faces:" << faceCount
       << "|edges:" << edgeCount
       << "|verts:" << vertexCount
       << "|bbox:"
       << aabb.xmin << "," << aabb.ymin << "," << aabb.zmin << ","
       << aabb.xmax << "," << aabb.ymax << "," << aabb.zmax;
    return ss.str();
}



bool IsExplicitSourceReference(const Occurrence& occ)
{
    if (occ.Label.IsNull() || occ.EffectiveLabel.IsNull())
    {
        return false;
    }
    return occ.Label != occ.EffectiveLabel;
}

std::string QualifiedPrototypeKey(const Occurrence& occ)
{
    const std::string geo = GeometrySignature(occ);
    const std::string mat = MaterialSignature(occ.Appearance.get());
    return geo + "|mat:" + mat;
}

std::uint32_t CountTrianglesForShape(const TopoDS_Shape& shape);

std::string GeometrySignatureForShape(const TopoDS_Shape& shape)
{
    Bnd_Box localBounds = ComputeLocalBounds(shape);
    core::Aabb aabb = ToAabb(localBounds);

    int edgeCount = 0;
    int vertexCount = 0;
    for (TopExp_Explorer ex(shape, TopAbs_EDGE); ex.More(); ex.Next()) { ++edgeCount; }
    for (TopExp_Explorer ex(shape, TopAbs_VERTEX); ex.More(); ex.Next()) { ++vertexCount; }

    std::ostringstream ss;
    ss << "type:" << static_cast<int>(shape.ShapeType())
       << "|ori:" << static_cast<int>(shape.Orientation())
       << "|edges:" << edgeCount
       << "|verts:" << vertexCount
       << "|bbox:"
       << aabb.xmin << "," << aabb.ymin << "," << aabb.zmin << ","
       << aabb.xmax << "," << aabb.ymax << "," << aabb.zmax;
    return ss.str();
}

std::string FaceMaterialSignature(const Occurrence& occ, const std::size_t faceIndex)
{
    if (!occ.Appearance)
    {
        return "no_appearance";
    }

    std::ostringstream ss;
    const CachedOccurrenceAppearance& app = *occ.Appearance;
    if (faceIndex < app.Faces.size())
    {
        const CachedFaceAppearance& fa = app.Faces[faceIndex];
        ss << "face(" << fa.Colors.HasGen << "," << fa.Colors.HasSurf << "," << fa.Colors.HasCurv << ")";
        if (fa.Colors.HasGen)
        {
            ss << "|gen:" << fa.Colors.Gen.Red() << "," << fa.Colors.Gen.Green() << "," << fa.Colors.Gen.Blue();
        }
        if (fa.Colors.HasSurf)
        {
            ss << "|surf:" << fa.Colors.Surf.Red() << "," << fa.Colors.Surf.Green() << "," << fa.Colors.Surf.Blue();
        }
        if (fa.Colors.HasCurv)
        {
            ss << "|curv:" << fa.Colors.Curv.Red() << "," << fa.Colors.Curv.Green() << "," << fa.Colors.Curv.Blue();
        }
        ss << "|matPresent:" << (!fa.VisMaterial.IsNull() ? 1 : 0);
        return ss.str();
    }

    const CachedColorSet& c = app.ResolvedShapeColors;
    ss << "shapeFallback(" << c.HasGen << "," << c.HasSurf << "," << c.HasCurv << ")";
    ss << "|shapeMatPresent:" << (!app.ResolvedShapeMaterial.IsNull() ? 1 : 0);
    return ss.str();
}


std::uint32_t CountTrianglesForShape(const TopoDS_Shape& shape)
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
    out.instances.reserve(occurrences.size());

    std::unordered_map<std::string, std::uint32_t> prototypeByKey;
    for (const Occurrence& occ : occurrences)
    {
        const std::string sourceLabel = LabelToStringSafe(occ.EffectiveLabel.IsNull() ? occ.Label : occ.EffectiveLabel);
        const std::string geometryKey = GeometrySignature(occ);
        const std::string materialKey = MaterialSignature(occ.Appearance.get());
        const std::string qualifiedKey = QualifiedPrototypeKey(occ);
        const bool fromExplicitReference = IsExplicitSourceReference(occ);

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
            prototype.localBounds = ToAabb(ComputeLocalBounds(occ.Shape));
            out.prototypes.push_back(std::move(prototype));
            prototypeByKey.emplace(qualifiedKey, prototypeId);
        }
        else
        {
            prototypeId = existing->second;
            reusedExistingPrototype = true;
        }
        core::SceneInstance instance;
        instance.sourceLabel = sourceLabel;
        instance.prototypeId = prototypeId;
        instance.occurrenceIndex = static_cast<std::uint32_t>(out.occurrences.size());
        instance.fromExplicitReference = fromExplicitReference;
        instance.worldTransform = ToTransform4d(occ.WorldTransform);
        instance.worldBounds = ToAabb(occ.WorldBounds);
        out.instances.push_back(std::move(instance));
        if (fromExplicitReference)
        {
            ++out.explicitReferenceInstances;
        }
        if (reusedExistingPrototype && !fromExplicitReference)
        {
            ++out.qualifiedDedupInstances;
        }

        std::size_t faceIndex = 0;
        for (TopExp_Explorer childExp(occ.Shape, TopAbs_FACE); childExp.More(); childExp.Next(), ++faceIndex)
        {
            const TopoDS_Shape childShape = childExp.Current().Located(TopLoc_Location());
            if (childShape.IsNull())
            {
                continue;
            }

            const std::string childGeometryKey = GeometrySignatureForShape(childShape);
            const std::string childMaterialKey = FaceMaterialSignature(occ, faceIndex);
            const std::string childQualifiedKey = "face|" + childGeometryKey + "|mat:" + childMaterialKey;

            std::uint32_t childPrototypeId = 0;
            const auto childExisting = prototypeByKey.find(childQualifiedKey);
            if (childExisting == prototypeByKey.end())
            {
                childPrototypeId = static_cast<std::uint32_t>(out.prototypes.size());
                core::ScenePrototype childPrototype;
                childPrototype.id = childPrototypeId;
                childPrototype.sourceLabel = sourceLabel;
                childPrototype.geometryKey = childGeometryKey;
                childPrototype.materialKey = childMaterialKey;
                childPrototype.triangleCount = CountTrianglesForShape(childShape);
                childPrototype.localBounds = ToAabb(ComputeLocalBounds(childShape));
                out.prototypes.push_back(std::move(childPrototype));
                prototypeByKey.emplace(childQualifiedKey, childPrototypeId);
            }
            else
            {
                childPrototypeId = childExisting->second;
            }

        }

        core::SceneOccurrence item;
        item.sourceLabel = sourceLabel;
        item.triangleCount = occ.TriangleCount;
        item.worldBounds = ToAabb(occ.WorldBounds);
        out.totalTriangles += static_cast<std::uint64_t>(occ.TriangleCount);
        out.occurrences.push_back(std::move(item));
    }

    return out;
}
} // namespace adapters
