#pragma once

#include "scene_ir.h"

namespace core
{
/// Row-major 4x4 multiply: `out = a * b` (column vectors: `v' = a * b * v`).
inline Transform4d MultiplyTransforms(const Transform4d& a, const Transform4d& b)
{
    Transform4d r;
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            double s = 0.0;
            for (int k = 0; k < 4; ++k)
            {
                s += a.m[i * 4 + k] * b.m[k * 4 + j];
            }
            r.m[i * 4 + j] = s;
        }
    }
    return r;
}

/// Translation only; matches `ToTransform4d` / row-major layout used across exporters.
inline Transform4d TranslationTransform(const double tx, const double ty, const double tz)
{
    Transform4d t;
    t.m[0] = 1.0;
    t.m[1] = 0.0;
    t.m[2] = 0.0;
    t.m[3] = tx;
    t.m[4] = 0.0;
    t.m[5] = 1.0;
    t.m[6] = 0.0;
    t.m[7] = ty;
    t.m[8] = 0.0;
    t.m[9] = 0.0;
    t.m[10] = 1.0;
    t.m[11] = tz;
    t.m[12] = 0.0;
    t.m[13] = 0.0;
    t.m[14] = 0.0;
    t.m[15] = 1.0;
    return t;
}
} // namespace core
