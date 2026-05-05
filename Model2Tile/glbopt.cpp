#include "glbopt_internal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace glbopt
{
    namespace
    {
        int EnsureDefaultMaterial(tinygltf::Model& model)
        {
            tinygltf::Material material;
            material.name = "glbopt-default";
            material.pbrMetallicRoughness.baseColorFactor = {0.8, 0.8, 0.8, 1.0};
            material.pbrMetallicRoughness.metallicFactor = 0.0;
            material.pbrMetallicRoughness.roughnessFactor = 1.0;
            material.doubleSided = true;

            model.materials.push_back(std::move(material));
            return static_cast<int>(model.materials.size() - 1);
        }

        void ForceAllMaterialsDoubleSided(tinygltf::Model& model)
        {
            for (tinygltf::Material& material : model.materials)
            {
                material.doubleSided = true;
            }
        }

        bool MaterialUsesAnyTexture(const tinygltf::Material& material)
        {
            if (material.pbrMetallicRoughness.baseColorTexture.index >= 0)
            {
                return true;
            }

            if (material.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0)
            {
                return true;
            }

            if (material.normalTexture.index >= 0)
            {
                return true;
            }

            if (material.occlusionTexture.index >= 0)
            {
                return true;
            }

            if (material.emissiveTexture.index >= 0)
            {
                return true;
            }

            return false;
        }

        bool ModelUsesAnyTexture(const tinygltf::Model& model)
        {
            for (const tinygltf::Material& material : model.materials)
            {
                if (MaterialUsesAnyTexture(material))
                {
                    return true;
                }
            }

            return false;
        }

        bool ModelUsesTexcoord0(const tinygltf::Model& model)
        {
            for (const tinygltf::Mesh& mesh : model.meshes)
            {
                for (const tinygltf::Primitive& prim : mesh.primitives)
                {
                    const auto it = prim.attributes.find("TEXCOORD_0");
                    if (it != prim.attributes.end() && it->second >= 0)
                    {
                        return true;
                    }
                }
            }
            return false;
        }
    }

    namespace
    {
        struct Mat4
        {
            double m[16] = {
                1.0, 0.0, 0.0, 0.0,
                0.0, 1.0, 0.0, 0.0,
                0.0, 0.0, 1.0, 0.0,
                0.0, 0.0, 0.0, 1.0
            };
        };

        static Mat4 Identity4()
        {
            return Mat4{};
        }

        static Mat4 Multiply4(const Mat4& a, const Mat4& b)
        {
            Mat4 out{};

            for (int c = 0; c < 4; ++c)
            {
                for (int r = 0; r < 4; ++r)
                {
                    out.m[c * 4 + r] =
                        a.m[0 * 4 + r] * b.m[c * 4 + 0] +
                        a.m[1 * 4 + r] * b.m[c * 4 + 1] +
                        a.m[2 * 4 + r] * b.m[c * 4 + 2] +
                        a.m[3 * 4 + r] * b.m[c * 4 + 3];
                }
            }

            return out;
        }

        static Mat4 Translation4(double tx, double ty, double tz)
        {
            Mat4 out = Identity4();
            out.m[12] = tx;
            out.m[13] = ty;
            out.m[14] = tz;
            return out;
        }

        static Mat4 Scale4(double sx, double sy, double sz)
        {
            Mat4 out = Identity4();
            out.m[0] = sx;
            out.m[5] = sy;
            out.m[10] = sz;
            return out;
        }

        static Mat4 RotationQuat4(double x, double y, double z, double w)
        {
            const double xx = x * x;
            const double yy = y * y;
            const double zz = z * z;
            const double xy = x * y;
            const double xz = x * z;
            const double yz = y * z;
            const double wx = w * x;
            const double wy = w * y;
            const double wz = w * z;

            Mat4 out = Identity4();
            out.m[0]  = 1.0 - 2.0 * (yy + zz);
            out.m[1]  = 2.0 * (xy + wz);
            out.m[2]  = 2.0 * (xz - wy);

            out.m[4]  = 2.0 * (xy - wz);
            out.m[5]  = 1.0 - 2.0 * (xx + zz);
            out.m[6]  = 2.0 * (yz + wx);

            out.m[8]  = 2.0 * (xz + wy);
            out.m[9]  = 2.0 * (yz - wx);
            out.m[10] = 1.0 - 2.0 * (xx + yy);
            return out;
        }

        static Mat4 NodeLocalTransform(const tinygltf::Node& node)
        {
            if (node.matrix.size() == 16)
            {
                Mat4 out{};
                for (int i = 0; i < 16; ++i)
                {
                    out.m[i] = node.matrix[static_cast<std::size_t>(i)];
                }
                return out;
            }

            double tx = 0.0;
            double ty = 0.0;
            double tz = 0.0;
            if (node.translation.size() == 3)
            {
                tx = node.translation[0];
                ty = node.translation[1];
                tz = node.translation[2];
            }

            double rx = 0.0;
            double ry = 0.0;
            double rz = 0.0;
            double rw = 1.0;
            if (node.rotation.size() == 4)
            {
                rx = node.rotation[0];
                ry = node.rotation[1];
                rz = node.rotation[2];
                rw = node.rotation[3];
            }

            double sx = 1.0;
            double sy = 1.0;
            double sz = 1.0;
            if (node.scale.size() == 3)
            {
                sx = node.scale[0];
                sy = node.scale[1];
                sz = node.scale[2];
            }

            const Mat4 t = Translation4(tx, ty, tz);
            const Mat4 r = RotationQuat4(rx, ry, rz, rw);
            const Mat4 s = Scale4(sx, sy, sz);
            return Multiply4(Multiply4(t, r), s);
        }

        static void TransformPoint(const Mat4& m, float& x, float& y, float& z)
        {
            const double ox = static_cast<double>(x);
            const double oy = static_cast<double>(y);
            const double oz = static_cast<double>(z);

            const double nx = m.m[0] * ox + m.m[4] * oy + m.m[8] * oz + m.m[12];
            const double ny = m.m[1] * ox + m.m[5] * oy + m.m[9] * oz + m.m[13];
            const double nz = m.m[2] * ox + m.m[6] * oy + m.m[10] * oz + m.m[14];

            x = static_cast<float>(nx);
            y = static_cast<float>(ny);
            z = static_cast<float>(nz);
        }

        static bool Invert3x3(const double a[9], double out[9])
        {
            const double det =
                a[0] * (a[4] * a[8] - a[5] * a[7]) -
                a[3] * (a[1] * a[8] - a[2] * a[7]) +
                a[6] * (a[1] * a[5] - a[2] * a[4]);

            if (std::abs(det) <= 1e-18)
            {
                return false;
            }

            const double invDet = 1.0 / det;

            out[0] =  (a[4] * a[8] - a[5] * a[7]) * invDet;
            out[1] = -(a[1] * a[8] - a[2] * a[7]) * invDet;
            out[2] =  (a[1] * a[5] - a[2] * a[4]) * invDet;

            out[3] = -(a[3] * a[8] - a[5] * a[6]) * invDet;
            out[4] =  (a[0] * a[8] - a[2] * a[6]) * invDet;
            out[5] = -(a[0] * a[5] - a[2] * a[3]) * invDet;

            out[6] =  (a[3] * a[7] - a[4] * a[6]) * invDet;
            out[7] = -(a[0] * a[7] - a[1] * a[6]) * invDet;
            out[8] =  (a[0] * a[4] - a[1] * a[3]) * invDet;
            return true;
        }

        static void TransformNormal(const Mat4& m, float& x, float& y, float& z)
        {
            // Build linear part in row-major form.
            const double l[9] = {
                m.m[0], m.m[4], m.m[8],
                m.m[1], m.m[5], m.m[9],
                m.m[2], m.m[6], m.m[10]
            };

            // Normals require inverse-transpose(linear).
            double invL[9];
            if (!Invert3x3(l, invL))
            {
                return;
            }

            const double ox = static_cast<double>(x);
            const double oy = static_cast<double>(y);
            const double oz = static_cast<double>(z);

            const double nx = invL[0] * ox + invL[3] * oy + invL[6] * oz;
            const double ny = invL[1] * ox + invL[4] * oy + invL[7] * oz;
            const double nz = invL[2] * ox + invL[5] * oy + invL[8] * oz;

            const double lenSq = nx * nx + ny * ny + nz * nz;
            if (lenSq <= 1e-20)
            {
                return;
            }

            const double invLen = 1.0 / std::sqrt(lenSq);
            x = static_cast<float>(nx * invLen);
            y = static_cast<float>(ny * invLen);
            z = static_cast<float>(nz * invLen);
        }

        static void ApplyTransformToPrimitive(internal::PrimitiveData& ioPrimitive, const Mat4& world)
        {
            for (internal::PackedVertex& v : ioPrimitive.Vertices)
            {
                TransformPoint(world, v.Position.X, v.Position.Y, v.Position.Z);

                if (v.HasNormal)
                {
                    TransformNormal(world, v.Normal.X, v.Normal.Y, v.Normal.Z);
                }
            }
        }

        static double LinearDeterminant(const Mat4& m)
        {
            const double l00 = m.m[0];
            const double l01 = m.m[4];
            const double l02 = m.m[8];
            const double l10 = m.m[1];
            const double l11 = m.m[5];
            const double l12 = m.m[9];
            const double l20 = m.m[2];
            const double l21 = m.m[6];
            const double l22 = m.m[10];
            return l00 * (l11 * l22 - l12 * l21) -
                   l01 * (l10 * l22 - l12 * l20) +
                   l02 * (l10 * l21 - l11 * l20);
        }

        static void FlipTriangleWinding(internal::PrimitiveData& ioPrimitive)
        {
            if (ioPrimitive.Mode != TINYGLTF_MODE_TRIANGLES)
            {
                return;
            }
            for (std::size_t i = 0; i + 2 < ioPrimitive.Indices.size(); i += 3)
            {
                std::swap(ioPrimitive.Indices[i + 1], ioPrimitive.Indices[i + 2]);
            }
        }

        static double QuantizeDouble(double value, double step)
        {
            if (step <= 0.0)
            {
                return value;
            }

            return std::round(value / step) * step;
        }

        static void QuantizeDoubleVector(std::vector<double>& values, double step)
        {
            for (double& value : values)
            {
                value = QuantizeDouble(value, step);
            }
        }

        static tinygltf::Material CanonicalizeMaterialForMerge(const tinygltf::Material& source)
        {
            tinygltf::Material canonical = source;

            // Ignore metadata-only names to avoid preventing merges.
            canonical.name.clear();

            // Quantize common numeric factors conservatively to collapse
            // numerically-close duplicates from export/serialization.
            QuantizeDoubleVector(canonical.pbrMetallicRoughness.baseColorFactor, 1.0 / 1024.0);
            canonical.pbrMetallicRoughness.metallicFactor =
                QuantizeDouble(canonical.pbrMetallicRoughness.metallicFactor, 1.0 / 1024.0);
            canonical.pbrMetallicRoughness.roughnessFactor =
                QuantizeDouble(canonical.pbrMetallicRoughness.roughnessFactor, 1.0 / 1024.0);

            QuantizeDoubleVector(canonical.emissiveFactor, 1.0 / 1024.0);
            canonical.alphaCutoff = QuantizeDouble(canonical.alphaCutoff, 1.0 / 1024.0);

            canonical.normalTexture.scale = QuantizeDouble(canonical.normalTexture.scale, 1.0 / 1024.0);
            canonical.occlusionTexture.strength = QuantizeDouble(canonical.occlusionTexture.strength, 1.0 / 1024.0);

            return canonical;
        }

        static bool AreMaterialsEquivalentForMerge(
            const tinygltf::Material& a,
            const tinygltf::Material& b)
        {
            const bool sensitiveAlpha =
                (a.alphaMode == "BLEND" || a.alphaMode == "MASK" ||
                 b.alphaMode == "BLEND" || b.alphaMode == "MASK");
            if (sensitiveAlpha && a.name != b.name)
            {
                // Keep explicit transparency authored materials isolated.
                return false;
            }
            // Intentionally ignore material name so semantically-identical
            // materials can collapse to a shared index and merge together.
            return (a.pbrMetallicRoughness == b.pbrMetallicRoughness) &&
                   (a.normalTexture == b.normalTexture) &&
                   (a.occlusionTexture == b.occlusionTexture) &&
                   (a.emissiveTexture == b.emissiveTexture) &&
                     (a.emissiveFactor == b.emissiveFactor) &&
                   (a.alphaMode == b.alphaMode) &&
                   TINYGLTF_DOUBLE_EQUAL(a.alphaCutoff, b.alphaCutoff) &&
                   (a.doubleSided == b.doubleSided) &&
                   (a.extensions == b.extensions) &&
                   (a.extras == b.extras) &&
                   (a.values == b.values) &&
                   (a.additionalValues == b.additionalValues);
        }

        static void DeduplicateMaterialSlots(
            tinygltf::Model& ioModel,
            Stats& ioStats)
        {
            const std::size_t materialCount = ioModel.materials.size();
            if (materialCount == 0)
            {
                return;
            }

            ioStats.MaterialCountInput += materialCount;

            std::vector<int> canonicalIndex(materialCount, -1);
            std::vector<tinygltf::Material> canonicalMaterials;
            canonicalMaterials.reserve(materialCount);
            for (std::size_t i = 0; i < materialCount; ++i)
            {
                canonicalMaterials.push_back(CanonicalizeMaterialForMerge(ioModel.materials[i]));
            }

            std::vector<int> uniqueMaterialIndices;
            uniqueMaterialIndices.reserve(materialCount);

            for (std::size_t i = 0; i < materialCount; ++i)
            {
                int canonical = -1;
                for (int uniqueIndex : uniqueMaterialIndices)
                {
                    if (AreMaterialsEquivalentForMerge(
                            canonicalMaterials[i],
                            canonicalMaterials[static_cast<std::size_t>(uniqueIndex)]))
                    {
                        canonical = uniqueIndex;
                        break;
                    }
                }

                if (canonical < 0)
                {
                    canonical = static_cast<int>(i);
                    uniqueMaterialIndices.push_back(canonical);
                }

                canonicalIndex[i] = canonical;
            }

            ioStats.MaterialCountCanonical += uniqueMaterialIndices.size();

            for (tinygltf::Mesh& mesh : ioModel.meshes)
            {
                for (tinygltf::Primitive& primitive : mesh.primitives)
                {
                    if (primitive.material < 0 ||
                        primitive.material >= static_cast<int>(canonicalIndex.size()))
                    {
                        continue;
                    }

                    const int oldIndex = primitive.material;
                    const int newIndex = canonicalIndex[static_cast<std::size_t>(oldIndex)];
                    if (newIndex != oldIndex)
                    {
                        primitive.material = newIndex;
                        ++ioStats.MaterialSlotsRemapped;
                    }
                }
            }
        }

        static bool AddPrimitiveToGroups(
            const tinygltf::Model& sourceModel,
            const tinygltf::Primitive& primitive,
            const Mat4& world,
            internal::DeepCopyContext& copyContext,
            std::unordered_map<internal::PrimitiveMergeKey, internal::PrimitiveData, internal::PrimitiveMergeKeyHasher>& ioGroups,
            Stats& ioStats)
        {
            ++ioStats.PrimitiveCountSeen;

            internal::PrimitiveData extracted;
            if (!internal::ExtractPrimitive(sourceModel, primitive, extracted, &ioStats))
            {
                return true;
            }

            ApplyTransformToPrimitive(extracted, world);
            if (LinearDeterminant(world) < 0.0)
            {
                FlipTriangleWinding(extracted);
            }

            if (primitive.material >= 0)
            {
                extracted.Material = internal::CopyMaterial(copyContext, primitive.material);
            }
            else
            {
                extracted.Material = -1;
            }

            ++ioStats.PrimitiveCountExtracted;
            ioStats.InputVertexCount += extracted.Vertices.size();
            ioStats.InputPrimitiveElementCount += extracted.Indices.size();

            const internal::PrimitiveMergeKey key = internal::MakeMergeKey(extracted);
            auto it = ioGroups.find(key);

            if (it == ioGroups.end())
            {
                ioGroups.emplace(key, std::move(extracted));
                return true;
            }

            if (internal::CanSafelyMerge(it->second, extracted))
            {
                internal::AppendPrimitiveIntoMerged(extracted, it->second);
            }
            else
            {
                internal::PrimitiveMergeKey splitKey{
                    extracted.Material,
                    extracted.Mode,
                    static_cast<std::uint8_t>(extracted.HasNormals ? 1 : 0),
                    static_cast<std::uint8_t>(extracted.HasTexcoord0 ? 1 : 0),
                    static_cast<std::uint8_t>(extracted.HasColor0 ? 1 : 0),
                    1u
                };

                while (ioGroups.find(splitKey) != ioGroups.end())
                {
                    ++splitKey.Bucket;
                }

                ioGroups.emplace(splitKey, std::move(extracted));
            }

            return true;
        }

        bool CollectIntoGroups(
            const tinygltf::Model& sourceModel,
            tinygltf::Model& destinationModel,
            std::unordered_map<internal::PrimitiveMergeKey, internal::PrimitiveData, internal::PrimitiveMergeKeyHasher>& ioGroups,
            Stats& ioStats)
        {
            internal::MergeUniqueStrings(destinationModel.extensionsUsed, sourceModel.extensionsUsed);
            internal::MergeUniqueStrings(destinationModel.extensionsRequired, sourceModel.extensionsRequired);

            internal::DeepCopyContext copyContext{
                sourceModel,
                destinationModel,
                {},
                {},
                {},
                {},
                {},
                {}
            };

            ioStats.MeshCount += sourceModel.meshes.size();

            auto processMeshPrimitives = [&](int meshIndex, const Mat4& world) -> bool
            {
                if (meshIndex < 0 || meshIndex >= static_cast<int>(sourceModel.meshes.size()))
                {
                    return true;
                }

                const tinygltf::Mesh& mesh = sourceModel.meshes[static_cast<std::size_t>(meshIndex)];
                for (const tinygltf::Primitive& primitive : mesh.primitives)
                {
                    if (!AddPrimitiveToGroups(
                            sourceModel,
                            primitive,
                            world,
                            copyContext,
                            ioGroups,
                            ioStats))
                    {
                        return false;
                    }
                }

                return true;
            };

            bool anySceneNodeProcessed = false;

            if (!sourceModel.scenes.empty() && !sourceModel.nodes.empty())
            {
                int sceneIndex = sourceModel.defaultScene;
                if (sceneIndex < 0 || sceneIndex >= static_cast<int>(sourceModel.scenes.size()))
                {
                    sceneIndex = 0;
                }

                const tinygltf::Scene& scene = sourceModel.scenes[static_cast<std::size_t>(sceneIndex)];

                std::function<bool(int, const Mat4&)> visitNode =
                    [&](int nodeIndex, const Mat4& parentWorld) -> bool
                {
                    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(sourceModel.nodes.size()))
                    {
                        return true;
                    }

                    const tinygltf::Node& node = sourceModel.nodes[static_cast<std::size_t>(nodeIndex)];
                    const Mat4 local = NodeLocalTransform(node);
                    const Mat4 world = Multiply4(parentWorld, local);

                    if (node.mesh >= 0)
                    {
                        anySceneNodeProcessed = true;
                        if (!processMeshPrimitives(node.mesh, world))
                        {
                            return false;
                        }
                    }

                    for (int childIndex : node.children)
                    {
                        if (!visitNode(childIndex, world))
                        {
                            return false;
                        }
                    }

                    return true;
                };

                const Mat4 identity = Identity4();
                for (int rootNodeIndex : scene.nodes)
                {
                    if (!visitNode(rootNodeIndex, identity))
                    {
                        return false;
                    }
                }
            }

            if (anySceneNodeProcessed)
            {
                return true;
            }

            for (const tinygltf::Mesh& mesh : sourceModel.meshes)
            {
                for (const tinygltf::Primitive& primitive : mesh.primitives)
                {
                    if (!AddPrimitiveToGroups(
                            sourceModel,
                            primitive,
                            Identity4(),
                            copyContext,
                            ioGroups,
                            ioStats))
                    {
                        return false;
                    }
                }
            }

            return true;
        }

        bool BuildMergedOutputModel(
            tinygltf::Model& ioModel,
            std::unordered_map<internal::PrimitiveMergeKey, internal::PrimitiveData, internal::PrimitiveMergeKeyHasher>& groups,
            const Options& options,
            Stats& ioStats)
        {
            if (groups.empty())
            {
                return false;
            }

            tinygltf::Mesh outMesh;
            outMesh.name = "merged-optimized";

            std::vector<internal::PrimitiveMergeKey> sortedKeys;
            sortedKeys.reserve(groups.size());
            for (const auto& kv : groups)
            {
                sortedKeys.push_back(kv.first);
            }
            std::sort(sortedKeys.begin(), sortedKeys.end());

            for (const internal::PrimitiveMergeKey& key : sortedKeys)
            {
                internal::PrimitiveData& merged = groups.at(key);

                if (options.ForceDefaultMaterialForMissing)
                {
                    if (merged.Material < 0 || merged.Material >= static_cast<int>(ioModel.materials.size()))
                    {
                        merged.Material = EnsureDefaultMaterial(ioModel);
                    }
                }

                internal::PreparePrimitiveData(merged, options, ioStats);
            }

            internal::ApplyGlobalMaxTriangles(groups, options, ioStats);

            for (const internal::PrimitiveMergeKey& key : sortedKeys)
            {
                internal::PrimitiveData& merged = groups.at(key);

                internal::FinalizePrimitiveData(merged, options, ioStats);

                tinygltf::Primitive outPrimitive;
                if (!internal::RewritePrimitive(ioModel, outPrimitive, merged))
                {
                    continue;
                }

                ioStats.OutputVertexCount += merged.Vertices.size();
                ioStats.OutputPrimitiveElementCount += merged.Indices.size();
                ++ioStats.PrimitiveCountMergedOut;

                outMesh.primitives.push_back(std::move(outPrimitive));
            }

            if (outMesh.primitives.empty())
            {
                return false;
            }

            ioModel.meshes.push_back(std::move(outMesh));

            tinygltf::Node outNode;
            outNode.name = "merged-root";
            outNode.mesh = static_cast<int>(ioModel.meshes.size() - 1);
            ioModel.nodes.push_back(std::move(outNode));

            tinygltf::Scene outScene;
            outScene.name = "default";
            outScene.nodes.push_back(static_cast<int>(ioModel.nodes.size() - 1));
            ioModel.scenes.push_back(std::move(outScene));
            ioModel.defaultScene = static_cast<int>(ioModel.scenes.size() - 1);

            return true;
        }
    }

    bool OptimizeGlbFile(
        const std::string& inputPath,
        const std::string& outputPath,
        const Options& options,
        Stats& outStats,
        const std::string& passTag)
    {
        outStats = Stats{};

        tinygltf::Model sourceModel;
        if (!internal::LoadGlb(inputPath, sourceModel))
        {
            return false;
        }

        Options effectiveOptions = options;
        if (!ModelUsesAnyTexture(sourceModel) && !ModelUsesTexcoord0(sourceModel))
        {
            effectiveOptions.WeldTexcoord0 = false;
        }

        if (effectiveOptions.DeduplicateMaterials)
        {
            DeduplicateMaterialSlots(sourceModel, outStats);
        }

        tinygltf::Model outputModel;
        outputModel.asset.version = "2.0";
        outputModel.asset.generator = "glbopt";

        std::unordered_map<internal::PrimitiveMergeKey, internal::PrimitiveData, internal::PrimitiveMergeKeyHasher> groups;

        if (!CollectIntoGroups(sourceModel, outputModel, groups, outStats))
        {
            return false;
        }

        if (effectiveOptions.ForceDoubleSidedMaterials)
        {
            ForceAllMaterialsDoubleSided(outputModel);
        }

        if (!BuildMergedOutputModel(outputModel, groups, effectiveOptions, outStats))
        {
            return false;
        }

        if (!internal::WriteGlb(outputModel, outputPath))
        {
            return false;
        }

        internal::PrintStats(outStats, inputPath, outputPath, passTag);
        return true;
    }

    bool OptimizeGlbFiles(
        const std::vector<std::string>& inputPaths,
        const std::string& outputPath,
        const Options& options,
        Stats& outStats,
        const std::string& passTag)
    {
        outStats = Stats{};

        if (inputPaths.empty())
        {
            return false;
        }

        if (inputPaths.size() == 1)
        {
            return OptimizeGlbFile(inputPaths.front(), outputPath, options, outStats, passTag);
        }

        // Merging N>2 inputs in one pass keeps all geometry in `groups` at once (O(sum inputs)),
        // which can OOM on large proxy merges. Merge pairwise via temp GLBs to cap peak memory.
        if (inputPaths.size() > 2)
        {
            namespace fs = std::filesystem;
            const fs::path outPathFs(outputPath);
            fs::path workDir = outPathFs.parent_path();
            if (workDir.empty())
            {
                workDir = fs::path(".");
            }

            std::string acc = inputPaths.front();
            bool accIsTemp = false;

            for (std::size_t i = 1; i < inputPaths.size(); ++i)
            {
                const bool last = (i + 1 == inputPaths.size());
                const std::string dest =
                    last ? outputPath
                         : (workDir / (std::string(".glbopt_chain_") + std::to_string(i) + ".glb"))
                               .string();

                if (!OptimizeGlbFiles(std::vector<std::string>{acc, inputPaths[i]}, dest, options, outStats, passTag))
                {
                    std::error_code ec;
                    if (accIsTemp)
                    {
                        fs::remove(acc, ec);
                    }
                    if (!last)
                    {
                        fs::remove(dest, ec);
                    }
                    return false;
                }

                if (accIsTemp)
                {
                    std::error_code ec;
                    fs::remove(acc, ec);
                }

                acc = dest;
                accIsTemp = !last;
            }

            return true;
        }

        Stats combinedStats{};
        bool anyTextureInInputs = false;
        bool anyTexcoordInInputs = false;

        tinygltf::Model outputModel;
        outputModel.asset.version = "2.0";
        outputModel.asset.generator = "glbopt merged";

        std::unordered_map<internal::PrimitiveMergeKey, internal::PrimitiveData, internal::PrimitiveMergeKeyHasher> groups;

        for (std::size_t inputIndex = 0; inputIndex < inputPaths.size(); ++inputIndex)
        {
            const std::string& inputPath = inputPaths[inputIndex];
            tinygltf::Model sourceModel;
            if (!internal::LoadGlb(inputPath, sourceModel))
            {
                return false;
            }

            anyTextureInInputs = anyTextureInInputs || ModelUsesAnyTexture(sourceModel);
            anyTexcoordInInputs =
                anyTexcoordInInputs || ModelUsesTexcoord0(sourceModel);

            if (options.DeduplicateMaterials)
            {
                DeduplicateMaterialSlots(sourceModel, combinedStats);
            }

            if (!CollectIntoGroups(sourceModel, outputModel, groups, combinedStats))
            {
                return false;
            }
        }

        Options effectiveOptions = options;
        if (!anyTextureInInputs && !anyTexcoordInInputs)
        {
            effectiveOptions.WeldTexcoord0 = false;
        }

        if (effectiveOptions.ForceDoubleSidedMaterials)
        {
            ForceAllMaterialsDoubleSided(outputModel);
        }

        if (!BuildMergedOutputModel(outputModel, groups, effectiveOptions, combinedStats))
        {
            return false;
        }

        if (!internal::WriteGlb(outputModel, outputPath))
        {
            return false;
        }

        outStats = combinedStats;
        internal::PrintStats(outStats, outputPath, outputPath, passTag);
        return true;
    }
}