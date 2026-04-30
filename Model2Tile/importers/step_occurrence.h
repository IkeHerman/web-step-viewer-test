#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <TDF_Label.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>
#include <Bnd_Box.hxx>
#include <Quantity_Color.hxx>
#include <XCAFDoc_VisMaterial.hxx>

#include "../core/scene_ir.h"

struct CachedColorSet
{
    bool HasGen = false;
    bool HasSurf = false;
    bool HasCurv = false;
    Quantity_Color Gen;
    Quantity_Color Surf;
    Quantity_Color Curv;
};

struct CachedFaceAppearance
{
    CachedColorSet Colors;
    Handle(XCAFDoc_VisMaterial) VisMaterial;
};

struct CachedOccurrenceAppearance
{
    CachedColorSet ResolvedShapeColors;
    Handle(XCAFDoc_VisMaterial) ResolvedShapeMaterial;
    std::vector<CachedFaceAppearance> Faces;
};

struct Prototype
{
    TDF_Label Label;
    TopoDS_Shape Shape;
    Bnd_Box LocalBounds;
};

struct Occurrence
{
    TDF_Label       Label;
    TDF_Label       EffectiveLabel;
    std::string     SourceLabelName;
    TopoDS_Shape    Shape;
    gp_Trsf         WorldTransform;
    Bnd_Box         WorldBounds;
    std::uint32_t   TriangleCount = 0;
    std::uint32_t   SourceFaceCount = 0;
    bool            HasAnyMetadata = false;

    core::Transform4d WorldTransformMatrix;
    core::Aabb LocalBoundsAabb;
    core::Aabb WorldBoundsAabb;
    std::string SourceLabelEntry;
    std::string GeometryKey;
    std::string MaterialKey;
    std::string QualifiedPrototypeKey;
    bool FromExplicitReference = false;

    struct FacePrototypeSeed
    {
        std::string GeometryKey;
        std::string MaterialKey;
        std::uint32_t TriangleCount = 0;
        core::Aabb LocalBounds;
    };
    std::vector<FacePrototypeSeed> FacePrototypeSeeds;

    std::shared_ptr<const CachedOccurrenceAppearance> Appearance;
};
