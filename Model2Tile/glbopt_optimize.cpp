#include "glbopt_internal.h"
#include "dep/meshoptimizer/src/meshoptimizer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace glbopt
{
    namespace internal
    {
        std::size_t PrimitiveMergeKeyHasher::operator()(const PrimitiveMergeKey& key) const noexcept
        {
            std::size_t h = 1469598103934665603ull;

            auto mix = [&](std::size_t v)
            {
                h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            };

            mix(static_cast<std::size_t>(key.Material));
            mix(static_cast<std::size_t>(key.Mode));
            mix(static_cast<std::size_t>(key.HasNormals));
            mix(static_cast<std::size_t>(key.HasTexcoord0));
            mix(static_cast<std::size_t>(key.HasColor0));
            mix(static_cast<std::size_t>(key.Bucket));
            return h;
        }

        std::size_t QuantizedKeyHasher::operator()(const QuantizedKey& key) const noexcept
        {
            std::size_t h = 1469598103934665603ull;

            for (std::int64_t v : key.Values)
            {
                const std::size_t p = static_cast<std::size_t>(v ^ (v >> 32));
                h ^= p + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            }

            return h;
        }

        static std::int64_t QuantizeFloat(float value, float step)
        {
            if (step <= 0.0f)
            {
                std::uint32_t bits = 0;
                static_assert(sizeof(bits) == sizeof(value), "float size must match uint32_t");
                std::memcpy(&bits, &value, sizeof(float));
                return static_cast<std::int64_t>(bits);
            }

            return static_cast<std::int64_t>(std::llround(static_cast<double>(value / step)));
        }

        static QuantizedKey BuildVertexKey(const PackedVertex& v, const Options& options)
        {
            QuantizedKey key;
            key.Values.reserve(24);

            if (options.WeldPositions)
            {
                key.Values.push_back(QuantizeFloat(v.Position.X, options.PositionStep));
                key.Values.push_back(QuantizeFloat(v.Position.Y, options.PositionStep));
                key.Values.push_back(QuantizeFloat(v.Position.Z, options.PositionStep));
            }

            if (options.WeldNormals)
            {
                key.Values.push_back(v.HasNormal ? 1 : 0);
                if (v.HasNormal)
                {
                    key.Values.push_back(QuantizeFloat(v.Normal.X, options.NormalStep));
                    key.Values.push_back(QuantizeFloat(v.Normal.Y, options.NormalStep));
                    key.Values.push_back(QuantizeFloat(v.Normal.Z, options.NormalStep));
                }
            }

            if (options.WeldTexcoord0)
            {
                key.Values.push_back(v.HasTexcoord0 ? 1 : 0);
                if (v.HasTexcoord0)
                {
                    key.Values.push_back(QuantizeFloat(v.Texcoord0.X, options.TexcoordStep));
                    key.Values.push_back(QuantizeFloat(v.Texcoord0.Y, options.TexcoordStep));
                }
            }

            if (options.WeldColor0)
            {
                key.Values.push_back(v.HasColor0 ? 1 : 0);
                if (v.HasColor0)
                {
                    key.Values.push_back(QuantizeFloat(v.Color0.X, options.ColorStep));
                    key.Values.push_back(QuantizeFloat(v.Color0.Y, options.ColorStep));
                    key.Values.push_back(QuantizeFloat(v.Color0.Z, options.ColorStep));
                    key.Values.push_back(QuantizeFloat(v.Color0.W, options.ColorStep));
                }
            }

            return key;
        }

        static Vec3 Sub(const Vec3& a, const Vec3& b)
        {
            Vec3 out;
            out.X = a.X - b.X;
            out.Y = a.Y - b.Y;
            out.Z = a.Z - b.Z;
            return out;
        }

        static Vec3 Cross(const Vec3& a, const Vec3& b)
        {
            Vec3 out;
            out.X = a.Y * b.Z - a.Z * b.Y;
            out.Y = a.Z * b.X - a.X * b.Z;
            out.Z = a.X * b.Y - a.Y * b.X;
            return out;
        }

        static float LengthSquared(const Vec3& v)
        {
            return v.X * v.X + v.Y * v.Y + v.Z * v.Z;
        }

        static bool IsZeroAreaTriangle(
            const PackedVertex& a,
            const PackedVertex& b,
            const PackedVertex& c,
            float epsilonSq)
        {
            const Vec3 ab = Sub(b.Position, a.Position);
            const Vec3 ac = Sub(c.Position, a.Position);
            const Vec3 cross = Cross(ab, ac);
            return LengthSquared(cross) <= epsilonSq;
        }

        PrimitiveMergeKey MakeMergeKey(const PrimitiveData& primitive)
        {
            PrimitiveMergeKey key;
            key.Material = primitive.Material;
            key.Mode = primitive.Mode;
            key.HasNormals = primitive.HasNormals ? 1 : 0;
            key.HasTexcoord0 = primitive.HasTexcoord0 ? 1 : 0;
            key.HasColor0 = primitive.HasColor0 ? 1 : 0;
            return key;
        }

        static bool IsConcatenationSafeMode(int mode)
        {
            return mode == TINYGLTF_MODE_POINTS ||
                   mode == TINYGLTF_MODE_LINE ||
                   mode == TINYGLTF_MODE_TRIANGLES;
        }

        bool CanSafelyMerge(const PrimitiveData& a, const PrimitiveData& b)
        {
            if (!IsConcatenationSafeMode(a.Mode) || !IsConcatenationSafeMode(b.Mode))
            {
                return false;
            }

            return a.Material == b.Material &&
                   a.Mode == b.Mode &&
                   a.HasNormals == b.HasNormals &&
                   a.HasTexcoord0 == b.HasTexcoord0 &&
                   a.HasColor0 == b.HasColor0;
        }

        void AppendPrimitiveIntoMerged(const PrimitiveData& source, PrimitiveData& destination)
        {
            const std::uint32_t baseVertex = static_cast<std::uint32_t>(destination.Vertices.size());

            destination.Vertices.insert(
                destination.Vertices.end(),
                source.Vertices.begin(),
                source.Vertices.end());

            destination.Indices.reserve(destination.Indices.size() + source.Indices.size());
            for (std::uint32_t idx : source.Indices)
            {
                destination.Indices.push_back(baseVertex + idx);
            }
        }

        static bool IsTriangleMode(int mode)
        {
            return mode == TINYGLTF_MODE_TRIANGLES;
        }

        static void CullInvalidIndexReferences(PrimitiveData& ioData, Stats& stats)
        {
            if (ioData.Indices.empty())
            {
                return;
            }

            const std::size_t vertexCount = ioData.Vertices.size();
            if (vertexCount == 0)
            {
                ioData.Indices.clear();
                return;
            }

            if (ioData.Mode == TINYGLTF_MODE_TRIANGLES)
            {
                std::vector<std::uint32_t> filtered;
                filtered.reserve(ioData.Indices.size());
                for (std::size_t i = 0; i + 2 < ioData.Indices.size(); i += 3)
                {
                    const std::uint32_t a = ioData.Indices[i + 0];
                    const std::uint32_t b = ioData.Indices[i + 1];
                    const std::uint32_t c = ioData.Indices[i + 2];
                    if (a >= vertexCount || b >= vertexCount || c >= vertexCount)
                    {
                        ++stats.DroppedTrianglesInvalidIndices;
                        continue;
                    }
                    filtered.push_back(a);
                    filtered.push_back(b);
                    filtered.push_back(c);
                }
                ioData.Indices.swap(filtered);
                return;
            }

            if (ioData.Mode == TINYGLTF_MODE_LINE)
            {
                std::vector<std::uint32_t> filtered;
                filtered.reserve(ioData.Indices.size());
                for (std::size_t i = 0; i + 1 < ioData.Indices.size(); i += 2)
                {
                    const std::uint32_t a = ioData.Indices[i + 0];
                    const std::uint32_t b = ioData.Indices[i + 1];
                    if (a >= vertexCount || b >= vertexCount)
                    {
                        continue;
                    }
                    filtered.push_back(a);
                    filtered.push_back(b);
                }
                ioData.Indices.swap(filtered);
                return;
            }

            std::vector<std::uint32_t> filtered;
            filtered.reserve(ioData.Indices.size());
            for (const std::uint32_t idx : ioData.Indices)
            {
                if (idx < vertexCount)
                {
                    filtered.push_back(idx);
                }
            }
            ioData.Indices.swap(filtered);
        }

        static void WeldVertices(
            PrimitiveData& ioData,
            const Options& options,
            Stats& stats)
        {
            if (!options.WeldPositions &&
                !options.WeldNormals &&
                !options.WeldTexcoord0 &&
                !options.WeldColor0)
            {
                return;
            }

            std::unordered_map<QuantizedKey, std::uint32_t, QuantizedKeyHasher> lookup;
            lookup.reserve(ioData.Vertices.size() * 2);

            std::vector<PackedVertex> weldedVertices;
            weldedVertices.reserve(ioData.Vertices.size());

            std::vector<std::uint32_t> remap(
                ioData.Vertices.size(),
                std::numeric_limits<std::uint32_t>::max());

            for (std::size_t i = 0; i < ioData.Vertices.size(); ++i)
            {
                const QuantizedKey key = BuildVertexKey(ioData.Vertices[i], options);
                const auto it = lookup.find(key);

                if (it != lookup.end())
                {
                    remap[i] = it->second;
                    continue;
                }

                const std::uint32_t newIndex = static_cast<std::uint32_t>(weldedVertices.size());
                weldedVertices.push_back(ioData.Vertices[i]);
                remap[i] = newIndex;
                lookup.emplace(key, newIndex);
            }

            for (std::uint32_t& idx : ioData.Indices)
            {
                if (idx >= remap.size() || remap[idx] == std::numeric_limits<std::uint32_t>::max())
                {
                    ++stats.DroppedIndicesInvalidRemap;
                    idx = 0;
                    continue;
                }
                idx = remap[idx];
            }

            stats.MergedVertexCount += ioData.Vertices.size() - weldedVertices.size();
            ioData.Vertices.swap(weldedVertices);
        }

        static void DropAllBlackVertexColor0(PrimitiveData& ioData)
        {
            if (!ioData.HasColor0)
            {
                return;
            }

            bool allBlack = true;
            constexpr float kEps = 1e-6f;

            for (const PackedVertex& v : ioData.Vertices)
            {
                if (!v.HasColor0)
                {
                    allBlack = false;
                    break;
                }

                if (std::fabs(v.Color0.X) > kEps ||
                    std::fabs(v.Color0.Y) > kEps ||
                    std::fabs(v.Color0.Z) > kEps)
                {
                    allBlack = false;
                    break;
                }
            }

            if (!allBlack)
            {
                return;
            }

            ioData.HasColor0 = false;
            for (PackedVertex& v : ioData.Vertices)
            {
                v.HasColor0 = 0;
                v.Color0.X = 1.0f;
                v.Color0.Y = 1.0f;
                v.Color0.Z = 1.0f;
                v.Color0.W = 1.0f;
            }
        }

        static void StripVertexColor0(PrimitiveData& ioData)
        {
            if (!ioData.HasColor0)
            {
                return;
            }

            ioData.HasColor0 = false;
            for (PackedVertex& v : ioData.Vertices)
            {
                v.HasColor0 = 0;
                v.Color0.X = 1.0f;
                v.Color0.Y = 1.0f;
                v.Color0.Z = 1.0f;
                v.Color0.W = 1.0f;
            }
        }

        static void CullDegenerates(
            PrimitiveData& ioData,
            const Options& options,
            Stats& stats)
        {
            if (ioData.Indices.empty())
            {
                return;
            }

            if (ioData.Mode == TINYGLTF_MODE_POINTS)
            {
                return;
            }

            if (ioData.Mode == TINYGLTF_MODE_LINE)
            {
                std::vector<std::uint32_t> newIndices;
                newIndices.reserve(ioData.Indices.size());

                for (std::size_t i = 0; i + 1 < ioData.Indices.size(); i += 2)
                {
                    const std::uint32_t a = ioData.Indices[i + 0];
                    const std::uint32_t b = ioData.Indices[i + 1];

                    if (options.RemoveDegenerateByIndex && a == b)
                    {
                        ++stats.DroppedDegenerateById;
                        continue;
                    }

                    newIndices.push_back(a);
                    newIndices.push_back(b);
                }

                ioData.Indices.swap(newIndices);
                return;
            }

            if (!IsTriangleMode(ioData.Mode))
            {
                return;
            }

            std::vector<std::uint32_t> newIndices;
            newIndices.reserve(ioData.Indices.size());

            for (std::size_t i = 0; i + 2 < ioData.Indices.size(); i += 3)
            {
                const std::uint32_t a = ioData.Indices[i + 0];
                const std::uint32_t b = ioData.Indices[i + 1];
                const std::uint32_t c = ioData.Indices[i + 2];

                if (options.RemoveDegenerateByIndex &&
                    (a == b || b == c || c == a))
                {
                    ++stats.DroppedDegenerateById;
                    continue;
                }

                if (options.RemoveDegenerateByArea &&
                    IsZeroAreaTriangle(
                        ioData.Vertices[a],
                        ioData.Vertices[b],
                        ioData.Vertices[c],
                        options.DegenerateAreaEpsilonSq))
                {
                    ++stats.DroppedDegenerateByArea;
                    continue;
                }

                newIndices.push_back(a);
                newIndices.push_back(b);
                newIndices.push_back(c);
            }

            ioData.Indices.swap(newIndices);
        }

        static void RunModeSafeOptimizer(
            PrimitiveData& ioData,
            const Options& options,
            Stats& stats)
        {
            if (ioData.Vertices.empty() || ioData.Indices.empty())
            {
                return;
            }

            if (!IsTriangleMode(ioData.Mode))
            {
                if (options.OptimizeVertexFetch)
                {
                    std::vector<PackedVertex> fetched(ioData.Vertices.size());
                    const std::size_t newVertexCount = meshopt_optimizeVertexFetch(
                        fetched.data(),
                        ioData.Indices.data(),
                        ioData.Indices.size(),
                        ioData.Vertices.data(),
                        ioData.Vertices.size(),
                        sizeof(PackedVertex));

                    fetched.resize(newVertexCount);
                    ioData.Vertices.swap(fetched);
                }

                return;
            }

            if (options.Simplify && options.SimplifyRatio < 1.0f)
            {
                const std::size_t currentPrimitiveCount = ioData.Indices.size() / 3;
                const std::size_t targetPrimitiveCount = std::max<std::size_t>(
                    1,
                    static_cast<std::size_t>(std::floor(
                        static_cast<double>(currentPrimitiveCount) * options.SimplifyRatio)));

                const std::size_t targetIndexCount = targetPrimitiveCount * 3;
                std::vector<std::uint32_t> simplified(ioData.Indices.size());
                const std::size_t resultCount = meshopt_simplify(
                    simplified.data(),
                    ioData.Indices.data(),
                    ioData.Indices.size(),
                    &ioData.Vertices[0].Position.X,
                    ioData.Vertices.size(),
                    sizeof(PackedVertex),
                    targetIndexCount,
                    options.SimplifyError,
                    0,
                    nullptr);

                simplified.resize(resultCount);
                ioData.Indices.swap(simplified);
                stats.SimplifiedIndexCount += resultCount;
            }

            if (options.OptimizeVertexCache)
            {
                std::vector<std::uint32_t> reordered(ioData.Indices.size());
                meshopt_optimizeVertexCache(
                    reordered.data(),
                    ioData.Indices.data(),
                    ioData.Indices.size(),
                    ioData.Vertices.size());
                ioData.Indices.swap(reordered);
            }

            if (options.OptimizeOverdraw)
            {
                std::vector<std::uint32_t> reordered(ioData.Indices.size());
                meshopt_optimizeOverdraw(
                    reordered.data(),
                    ioData.Indices.data(),
                    ioData.Indices.size(),
                    &ioData.Vertices[0].Position.X,
                    ioData.Vertices.size(),
                    sizeof(PackedVertex),
                    options.OverdrawThreshold);
                ioData.Indices.swap(reordered);
            }

            if (options.OptimizeVertexFetch)
            {
                std::vector<PackedVertex> fetched(ioData.Vertices.size());
                const std::size_t newVertexCount = meshopt_optimizeVertexFetch(
                    fetched.data(),
                    ioData.Indices.data(),
                    ioData.Indices.size(),
                    ioData.Vertices.data(),
                    ioData.Vertices.size(),
                    sizeof(PackedVertex));

                fetched.resize(newVertexCount);
                ioData.Vertices.swap(fetched);
            }
        }

        void OptimizePrimitiveData(
            PrimitiveData& ioData,
            const Options& options,
            Stats& stats)
        {
            CullInvalidIndexReferences(ioData, stats);
            if (options.StripColor0Always)
            {
                StripVertexColor0(ioData);
            }

            if (options.DropAllBlackColor0)
            {
                DropAllBlackVertexColor0(ioData);
            }

            WeldVertices(ioData, options, stats);
            CullDegenerates(ioData, options, stats);
            RunModeSafeOptimizer(ioData, options, stats);
        }
    }
}