#include "glbopt_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>

namespace glbopt
{
    namespace internal
    {
        static int ComponentCountForType(int type)
        {
            switch (type)
            {
                case TINYGLTF_TYPE_SCALAR: return 1;
                case TINYGLTF_TYPE_VEC2: return 2;
                case TINYGLTF_TYPE_VEC3: return 3;
                case TINYGLTF_TYPE_VEC4: return 4;
                default: return 0;
            }
        }

        static std::size_t ComponentSizeInBytes(int componentType)
        {
            switch (componentType)
            {
                case TINYGLTF_COMPONENT_TYPE_BYTE: return 1;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: return 1;
                case TINYGLTF_COMPONENT_TYPE_SHORT: return 2;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return 2;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: return 4;
                case TINYGLTF_COMPONENT_TYPE_FLOAT: return 4;
                default: return 0;
            }
        }

        static const unsigned char* GetAccessorDataPtr(
            const tinygltf::Model& model,
            const tinygltf::Accessor& accessor,
            std::size_t& outStride)
        {
            if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(model.bufferViews.size()))
            {
                return nullptr;
            }

            const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
            if (view.buffer < 0 || view.buffer >= static_cast<int>(model.buffers.size()))
            {
                return nullptr;
            }

            const tinygltf::Buffer& buffer = model.buffers[view.buffer];

            const int componentCount = ComponentCountForType(accessor.type);
            const std::size_t componentSize = ComponentSizeInBytes(accessor.componentType);
            const std::size_t packedSize = static_cast<std::size_t>(componentCount) * componentSize;

            outStride = accessor.ByteStride(view);
            if (outStride == 0)
            {
                outStride = packedSize;
            }

            const std::size_t offset = view.byteOffset + accessor.byteOffset;
            if (offset >= buffer.data.size())
            {
                return nullptr;
            }

            return buffer.data.data() + offset;
        }

        static float ReadNormalizedInt(const void* src, int componentType, bool normalized)
        {
            if (componentType == TINYGLTF_COMPONENT_TYPE_FLOAT)
            {
                return *reinterpret_cast<const float*>(src);
            }

            if (componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
            {
                const std::uint8_t v = *reinterpret_cast<const std::uint8_t*>(src);
                return normalized ? static_cast<float>(v) / 255.0f : static_cast<float>(v);
            }

            if (componentType == TINYGLTF_COMPONENT_TYPE_BYTE)
            {
                const std::int8_t v = *reinterpret_cast<const std::int8_t*>(src);
                return normalized ? std::max(-1.0f, static_cast<float>(v) / 127.0f) : static_cast<float>(v);
            }

            if (componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
            {
                const std::uint16_t v = *reinterpret_cast<const std::uint16_t*>(src);
                return normalized ? static_cast<float>(v) / 65535.0f : static_cast<float>(v);
            }

            if (componentType == TINYGLTF_COMPONENT_TYPE_SHORT)
            {
                const std::int16_t v = *reinterpret_cast<const std::int16_t*>(src);
                return normalized ? std::max(-1.0f, static_cast<float>(v) / 32767.0f) : static_cast<float>(v);
            }

            if (componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
            {
                const std::uint32_t v = *reinterpret_cast<const std::uint32_t*>(src);
                return normalized ? static_cast<float>(static_cast<double>(v) / 4294967295.0) : static_cast<float>(v);
            }

            return 0.0f;
        }

        static bool ReadFloatAccessor(
            const tinygltf::Model& model,
            int accessorIndex,
            std::vector<float>& outValues,
            int expectedComponentsMin)
        {
            if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size()))
            {
                return false;
            }

            const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
            const int comps = ComponentCountForType(accessor.type);
            if (comps < expectedComponentsMin)
            {
                return false;
            }

            std::size_t stride = 0;
            const unsigned char* data = GetAccessorDataPtr(model, accessor, stride);
            if (data == nullptr)
            {
                return false;
            }

            outValues.resize(static_cast<std::size_t>(accessor.count) * static_cast<std::size_t>(comps));
            const std::size_t componentSize = ComponentSizeInBytes(accessor.componentType);

            for (std::size_t i = 0; i < static_cast<std::size_t>(accessor.count); ++i)
            {
                const unsigned char* src = data + i * stride;
                for (int c = 0; c < comps; ++c)
                {
                    outValues[i * static_cast<std::size_t>(comps) + static_cast<std::size_t>(c)] =
                        ReadNormalizedInt(
                            src + static_cast<std::size_t>(c) * componentSize,
                            accessor.componentType,
                            accessor.normalized);
                }
            }

            return true;
        }

        static bool ReadIndicesOrGenerateSequential(
            const tinygltf::Model& model,
            const tinygltf::Primitive& primitive,
            std::size_t vertexCount,
            std::vector<std::uint32_t>& outIndices)
        {
            if (primitive.indices < 0)
            {
                outIndices.resize(vertexCount);
                for (std::size_t i = 0; i < vertexCount; ++i)
                {
                    outIndices[i] = static_cast<std::uint32_t>(i);
                }
                return true;
            }

            if (primitive.indices >= static_cast<int>(model.accessors.size()))
            {
                return false;
            }

            const tinygltf::Accessor& accessor = model.accessors[primitive.indices];
            std::size_t stride = 0;
            const unsigned char* data = GetAccessorDataPtr(model, accessor, stride);
            if (data == nullptr)
            {
                return false;
            }

            outIndices.resize(static_cast<std::size_t>(accessor.count));

            for (std::size_t i = 0; i < static_cast<std::size_t>(accessor.count); ++i)
            {
                const unsigned char* src = data + i * stride;

                switch (accessor.componentType)
                {
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                        outIndices[i] = *reinterpret_cast<const std::uint8_t*>(src);
                        break;
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                        outIndices[i] = *reinterpret_cast<const std::uint16_t*>(src);
                        break;
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                        outIndices[i] = *reinterpret_cast<const std::uint32_t*>(src);
                        break;
                    default:
                        return false;
                }
            }

            return true;
        }

        bool ExtractPrimitive(
            const tinygltf::Model& model,
            const tinygltf::Primitive& primitive,
            PrimitiveData& outData)
        {
            const auto posIt = primitive.attributes.find("POSITION");
            if (posIt == primitive.attributes.end())
            {
                return false;
            }

            std::vector<float> positions;
            if (!ReadFloatAccessor(model, posIt->second, positions, 3))
            {
                return false;
            }

            const std::size_t vertexCount = positions.size() / 3;

            std::vector<float> normals;
            std::vector<float> texcoords;
            std::vector<float> colors;

            outData.HasNormals = false;
            outData.HasTexcoord0 = false;
            outData.HasColor0 = false;

            const auto normalIt = primitive.attributes.find("NORMAL");
            if (normalIt != primitive.attributes.end())
            {
                outData.HasNormals =
                    ReadFloatAccessor(model, normalIt->second, normals, 3) &&
                    (normals.size() / 3 == vertexCount);
            }

            const auto uvIt = primitive.attributes.find("TEXCOORD_0");
            if (uvIt != primitive.attributes.end())
            {
                outData.HasTexcoord0 =
                    ReadFloatAccessor(model, uvIt->second, texcoords, 2) &&
                    (texcoords.size() / 2 == vertexCount);
            }

            const auto colorIt = primitive.attributes.find("COLOR_0");
            if (colorIt != primitive.attributes.end())
            {
                const bool ok = ReadFloatAccessor(model, colorIt->second, colors, 3);
                const std::size_t count3 = colors.size() / 3;
                const std::size_t count4 = colors.size() / 4;
                outData.HasColor0 = ok && (count3 == vertexCount || count4 == vertexCount);
            }

            if (!ReadIndicesOrGenerateSequential(model, primitive, vertexCount, outData.Indices))
            {
                return false;
            }

            outData.Vertices.resize(vertexCount);
            outData.Material = primitive.material;

            // Some sources omit mode (tinygltf uses -1). glTF default is TRIANGLES.
            // Normalize here so downstream optimization/rewrite never emits invalid mode.
            outData.Mode = (primitive.mode >= 0) ? primitive.mode : TINYGLTF_MODE_TRIANGLES;

            for (std::size_t i = 0; i < vertexCount; ++i)
            {
                PackedVertex v{};

                v.Position.X = positions[i * 3 + 0];
                v.Position.Y = positions[i * 3 + 1];
                v.Position.Z = positions[i * 3 + 2];

                if (outData.HasNormals)
                {
                    v.HasNormal = 1;
                    v.Normal.X = normals[i * 3 + 0];
                    v.Normal.Y = normals[i * 3 + 1];
                    v.Normal.Z = normals[i * 3 + 2];
                }

                if (outData.HasTexcoord0)
                {
                    v.HasTexcoord0 = 1;
                    v.Texcoord0.X = texcoords[i * 2 + 0];
                    v.Texcoord0.Y = texcoords[i * 2 + 1];
                }

                if (outData.HasColor0)
                {
                    v.HasColor0 = 1;

                    if (colors.size() == vertexCount * 3)
                    {
                        v.Color0.X = colors[i * 3 + 0];
                        v.Color0.Y = colors[i * 3 + 1];
                        v.Color0.Z = colors[i * 3 + 2];
                        v.Color0.W = 1.0f;
                    }
                    else
                    {
                        v.Color0.X = colors[i * 4 + 0];
                        v.Color0.Y = colors[i * 4 + 1];
                        v.Color0.Z = colors[i * 4 + 2];
                        v.Color0.W = colors[i * 4 + 3];
                    }
                }

                outData.Vertices[i] = v;
            }

            return true;
        }

        bool LoadGlb(const std::string& inputPath, tinygltf::Model& outModel)
        {
            tinygltf::TinyGLTF gltf;
            std::string err;
            std::string warn;

            const bool ok = gltf.LoadBinaryFromFile(&outModel, &err, &warn, inputPath);

            if (!warn.empty() && glbopt::IsVerboseLogging())
            {
                std::cout << "[glbopt] warn: " << warn << "\n";
            }

            if (!err.empty())
            {
                std::cout << "[glbopt] err: " << err << "\n";
            }

            return ok;
        }

        bool WriteGlb(tinygltf::Model& model, const std::string& outputPath)
        {
            tinygltf::TinyGLTF gltf;
            return gltf.WriteGltfSceneToFile(&model, outputPath, true, true, false, true);
        }

        void PrintStats(const Stats& stats, const std::string& label)
        {
            if (!glbopt::IsVerboseLogging())
            {
                return;
            }

            const std::string fileName = std::filesystem::path(label).filename().string();
            const std::uint64_t trisIn = stats.InputPrimitiveElementCount / 3;
            const std::uint64_t trisOut = stats.OutputPrimitiveElementCount / 3;

            const double inCount = static_cast<double>(trisIn);
            const double outCount = static_cast<double>(trisOut);
            const double changePct =
                (inCount > 0.0) ? ((inCount - outCount) * 100.0 / inCount) : 0.0;

            const long long roundedPct = static_cast<long long>(std::llround(std::abs(changePct)));
            const char* changeWord = (changePct >= 0.0) ? "Reduction" : "Increase";

            std::cout << "[glbopt] file=" << fileName
                      << " matsIn=" << stats.MaterialCountInput
                      << " matsCanon=" << stats.MaterialCountCanonical
                      << " matRemap=" << stats.MaterialSlotsRemapped
                      << " trisIn=" << trisIn
                      << " trisOut=" << trisOut
                      << " " << roundedPct << "% " << changeWord
                      << "\n";
        }
    }
}