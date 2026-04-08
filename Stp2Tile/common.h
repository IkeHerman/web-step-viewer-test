#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <TDF_Label.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>
#include <Bnd_Box.hxx>
#include <Quantity_Color.hxx>
#include <XCAFDoc_VisMaterial.hxx>

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
    TopoDS_Shape    Shape;
    gp_Trsf         WorldTransform;
    Bnd_Box         WorldBounds;
    std::uint32_t   TriangleCount = 0;
    std::shared_ptr<const CachedOccurrenceAppearance> Appearance;
};
