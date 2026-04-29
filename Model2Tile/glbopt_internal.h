#pragma once

#include "glbopt.h"
#include "dep/tinygltf/tiny_gltf.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace glbopt
{
    namespace internal
    {
        struct Vec2
        {
            float X = 0.0f;
            float Y = 0.0f;
        };

        struct Vec3
        {
            float X = 0.0f;
            float Y = 0.0f;
            float Z = 0.0f;
        };

        struct Vec4
        {
            float X = 1.0f;
            float Y = 1.0f;
            float Z = 1.0f;
            float W = 1.0f;
        };

        struct PackedVertex
        {
            Vec3 Position;
            Vec3 Normal;
            Vec2 Texcoord0;
            Vec4 Color0;

            std::uint8_t HasNormal = 0;
            std::uint8_t HasTexcoord0 = 0;
            std::uint8_t HasColor0 = 0;
        };

        struct PrimitiveData
        {
            std::vector<PackedVertex> Vertices;
            std::vector<std::uint32_t> Indices;

            int Material = -1;
            int Mode = TINYGLTF_MODE_TRIANGLES;

            bool HasNormals = false;
            bool HasTexcoord0 = false;
            bool HasColor0 = false;
        };

        struct PrimitiveMergeKey
        {
            int Material = -1;
            int Mode = TINYGLTF_MODE_TRIANGLES;
            std::uint8_t HasNormals = 0;
            std::uint8_t HasTexcoord0 = 0;
            std::uint8_t HasColor0 = 0;
            std::uint32_t Bucket = 0;

            bool operator==(const PrimitiveMergeKey& other) const
            {
                return Material == other.Material &&
                       Mode == other.Mode &&
                       HasNormals == other.HasNormals &&
                       HasTexcoord0 == other.HasTexcoord0 &&
                       HasColor0 == other.HasColor0 &&
                       Bucket == other.Bucket;
            }
        };

        struct PrimitiveMergeKeyHasher
        {
            std::size_t operator()(const PrimitiveMergeKey& key) const noexcept;
        };

        struct QuantizedKey
        {
            std::vector<std::int64_t> Values;

            bool operator==(const QuantizedKey& other) const
            {
                return Values == other.Values;
            }
        };

        struct QuantizedKeyHasher
        {
            std::size_t operator()(const QuantizedKey& key) const noexcept;
        };

        struct DeepCopyContext
        {
            const tinygltf::Model& Source;
            tinygltf::Model& Destination;

            std::unordered_map<int, int> BufferMap;
            std::unordered_map<int, int> BufferViewMap;
            std::unordered_map<int, int> ImageMap;
            std::unordered_map<int, int> SamplerMap;
            std::unordered_map<int, int> TextureMap;
            std::unordered_map<int, int> MaterialMap;
        };

        bool LoadGlb(const std::string& inputPath, tinygltf::Model& outModel);
        bool WriteGlb(tinygltf::Model& model, const std::string& outputPath);
        void PrintStats(const Stats& stats, const std::string& label);

        bool ExtractPrimitive(
            const tinygltf::Model& model,
            const tinygltf::Primitive& primitive,
            PrimitiveData& outData);

        PrimitiveMergeKey MakeMergeKey(const PrimitiveData& primitive);
        bool CanSafelyMerge(const PrimitiveData& a, const PrimitiveData& b);
        void AppendPrimitiveIntoMerged(const PrimitiveData& source, PrimitiveData& destination);

        void OptimizePrimitiveData(
            PrimitiveData& ioData,
            const Options& options,
            Stats& stats);

        bool RewritePrimitive(
            tinygltf::Model& model,
            tinygltf::Primitive& primitive,
            const PrimitiveData& data);

        int CopyMaterial(DeepCopyContext& context, int sourceMaterialIndex);
        void MergeUniqueStrings(
            std::vector<std::string>& destination,
            const std::vector<std::string>& source);
    }
}