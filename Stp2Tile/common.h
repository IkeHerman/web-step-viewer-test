#pragma once

#include <TDF_Label.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>
#include <Bnd_Box.hxx>

struct Prototype
{
    TDF_Label Label;
    TopoDS_Shape Shape;   // typically a TopoDS_Solid
    Bnd_Box LocalBounds;
};

struct Occurrence
{
    TDF_Label       Label;           // occurrence label
    TopoDS_Shape    Shape;          // full shape at this occurrence
    gp_Trsf         WorldTransform;
    Bnd_Box         WorldBounds;
};