#include "glbopt_internal.h"

#include <cstring>
#include <limits>
#include <vector>

namespace glbopt
{
    namespace internal
    {
        static int EnsureDefaultMaterial(tinygltf::Model& model)
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

        template<typename T>
        static void AppendBytes(std::vector<unsigned char>& dst, const std::vector<T>& src)
        {
            const std::size_t oldSize = dst.size();
            dst.resize(oldSize + src.size() * sizeof(T));
            std::memcpy(dst.data() + oldSize, src.data(), src.size() * sizeof(T));
        }

        static int AddBuffer(
            tinygltf::Model& model,
            std::vector<unsigned char>&& bytes,
            const std::string& name)
        {
            tinygltf::Buffer buffer;
            buffer.name = name;
            buffer.data = std::move(bytes);
            model.buffers.push_back(std::move(buffer));
            return static_cast<int>(model.buffers.size() - 1);
        }

        static int AddBufferView(
            tinygltf::Model& model,
            int bufferIndex,
            std::size_t byteOffset,
            std::size_t byteLength,
            int target,
            const std::string& name)
        {
            tinygltf::BufferView view;
            view.buffer = bufferIndex;
            view.byteOffset = byteOffset;
            view.byteLength = byteLength;
            view.target = target;
            view.name = name;
            model.bufferViews.push_back(std::move(view));
            return static_cast<int>(model.bufferViews.size() - 1);
        }

        static int AddAccessor(
            tinygltf::Model& model,
            int bufferView,
            int componentType,
            int type,
            std::size_t count,
            bool normalized,
            const std::vector<double>* minVals = nullptr,
            const std::vector<double>* maxVals = nullptr,
            const std::string& name = "")
        {
            tinygltf::Accessor accessor;
            accessor.bufferView = bufferView;
            accessor.byteOffset = 0;
            accessor.componentType = componentType;
            accessor.type = type;
            accessor.count = count;
            accessor.normalized = normalized;
            accessor.name = name;

            if (minVals != nullptr)
            {
                accessor.minValues = *minVals;
            }

            if (maxVals != nullptr)
            {
                accessor.maxValues = *maxVals;
            }

            model.accessors.push_back(std::move(accessor));
            return static_cast<int>(model.accessors.size() - 1);
        }

        static void ComputePositionMinMax(
            const std::vector<PackedVertex>& vertices,
            std::vector<double>& outMin,
            std::vector<double>& outMax)
        {
            outMin = {
                std::numeric_limits<double>::max(),
                std::numeric_limits<double>::max(),
                std::numeric_limits<double>::max()
            };

            outMax = {
                -std::numeric_limits<double>::max(),
                -std::numeric_limits<double>::max(),
                -std::numeric_limits<double>::max()
            };

            for (const PackedVertex& v : vertices)
            {
                outMin[0] = std::min(outMin[0], static_cast<double>(v.Position.X));
                outMin[1] = std::min(outMin[1], static_cast<double>(v.Position.Y));
                outMin[2] = std::min(outMin[2], static_cast<double>(v.Position.Z));

                outMax[0] = std::max(outMax[0], static_cast<double>(v.Position.X));
                outMax[1] = std::max(outMax[1], static_cast<double>(v.Position.Y));
                outMax[2] = std::max(outMax[2], static_cast<double>(v.Position.Z));
            }
        }

        bool RewritePrimitive(
            tinygltf::Model& model,
            tinygltf::Primitive& primitive,
            const PrimitiveData& data)
        {
            if (data.Vertices.empty() || data.Indices.empty())
            {
                return false;
            }

            std::vector<float> positions;
            std::vector<float> normals;
            std::vector<float> texcoords;
            std::vector<float> colors;

            positions.reserve(data.Vertices.size() * 3);

            for (const PackedVertex& v : data.Vertices)
            {
                positions.push_back(v.Position.X);
                positions.push_back(v.Position.Y);
                positions.push_back(v.Position.Z);
            }

            if (data.HasNormals)
            {
                normals.reserve(data.Vertices.size() * 3);
                for (const PackedVertex& v : data.Vertices)
                {
                    normals.push_back(v.HasNormal ? v.Normal.X : 0.0f);
                    normals.push_back(v.HasNormal ? v.Normal.Y : 0.0f);
                    normals.push_back(v.HasNormal ? v.Normal.Z : 1.0f);
                }
            }

            if (data.HasTexcoord0)
            {
                texcoords.reserve(data.Vertices.size() * 2);
                for (const PackedVertex& v : data.Vertices)
                {
                    texcoords.push_back(v.HasTexcoord0 ? v.Texcoord0.X : 0.0f);
                    texcoords.push_back(v.HasTexcoord0 ? v.Texcoord0.Y : 0.0f);
                }
            }

            if (data.HasColor0)
            {
                colors.reserve(data.Vertices.size() * 4);
                for (const PackedVertex& v : data.Vertices)
                {
                    colors.push_back(v.HasColor0 ? v.Color0.X : 1.0f);
                    colors.push_back(v.HasColor0 ? v.Color0.Y : 1.0f);
                    colors.push_back(v.HasColor0 ? v.Color0.Z : 1.0f);
                    colors.push_back(v.HasColor0 ? v.Color0.W : 1.0f);
                }
            }

            std::vector<unsigned char> vertexBytes;
            const std::size_t posOffset = 0;
            AppendBytes(vertexBytes, positions);

            std::size_t normalOffset = 0;
            std::size_t uvOffset = 0;
            std::size_t colorOffset = 0;

            if (data.HasNormals)
            {
                normalOffset = vertexBytes.size();
                AppendBytes(vertexBytes, normals);
            }

            if (data.HasTexcoord0)
            {
                uvOffset = vertexBytes.size();
                AppendBytes(vertexBytes, texcoords);
            }

            if (data.HasColor0)
            {
                colorOffset = vertexBytes.size();
                AppendBytes(vertexBytes, colors);
            }

            std::vector<unsigned char> indexBytes;
            AppendBytes(indexBytes, data.Indices);

            const int vb = AddBuffer(model, std::move(vertexBytes), "optimized-vb");
            const int ib = AddBuffer(model, std::move(indexBytes), "optimized-ib");

            const int posView = AddBufferView(
                model,
                vb,
                posOffset,
                positions.size() * sizeof(float),
                TINYGLTF_TARGET_ARRAY_BUFFER,
                "POSITION");

            int normalView = -1;
            int uvView = -1;
            int colorView = -1;

            if (data.HasNormals)
            {
                normalView = AddBufferView(
                    model,
                    vb,
                    normalOffset,
                    normals.size() * sizeof(float),
                    TINYGLTF_TARGET_ARRAY_BUFFER,
                    "NORMAL");
            }

            if (data.HasTexcoord0)
            {
                uvView = AddBufferView(
                    model,
                    vb,
                    uvOffset,
                    texcoords.size() * sizeof(float),
                    TINYGLTF_TARGET_ARRAY_BUFFER,
                    "TEXCOORD_0");
            }

            if (data.HasColor0)
            {
                colorView = AddBufferView(
                    model,
                    vb,
                    colorOffset,
                    colors.size() * sizeof(float),
                    TINYGLTF_TARGET_ARRAY_BUFFER,
                    "COLOR_0");
            }

            const int idxView = AddBufferView(
                model,
                ib,
                0,
                data.Indices.size() * sizeof(std::uint32_t),
                TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER,
                "indices");

            std::vector<double> minVals;
            std::vector<double> maxVals;
            ComputePositionMinMax(data.Vertices, minVals, maxVals);

            primitive.attributes.clear();

            primitive.attributes["POSITION"] = AddAccessor(
                model,
                posView,
                TINYGLTF_COMPONENT_TYPE_FLOAT,
                TINYGLTF_TYPE_VEC3,
                data.Vertices.size(),
                false,
                &minVals,
                &maxVals,
                "POSITION");

            if (data.HasNormals)
            {
                primitive.attributes["NORMAL"] = AddAccessor(
                    model,
                    normalView,
                    TINYGLTF_COMPONENT_TYPE_FLOAT,
                    TINYGLTF_TYPE_VEC3,
                    data.Vertices.size(),
                    false,
                    nullptr,
                    nullptr,
                    "NORMAL");
            }

            if (data.HasTexcoord0)
            {
                primitive.attributes["TEXCOORD_0"] = AddAccessor(
                    model,
                    uvView,
                    TINYGLTF_COMPONENT_TYPE_FLOAT,
                    TINYGLTF_TYPE_VEC2,
                    data.Vertices.size(),
                    false,
                    nullptr,
                    nullptr,
                    "TEXCOORD_0");
            }

            if (data.HasColor0)
            {
                primitive.attributes["COLOR_0"] = AddAccessor(
                    model,
                    colorView,
                    TINYGLTF_COMPONENT_TYPE_FLOAT,
                    TINYGLTF_TYPE_VEC4,
                    data.Vertices.size(),
                    false,
                    nullptr,
                    nullptr,
                    "COLOR_0");
            }

            primitive.indices = AddAccessor(
                model,
                idxView,
                TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
                TINYGLTF_TYPE_SCALAR,
                data.Indices.size(),
                false,
                nullptr,
                nullptr,
                "indices");

            int materialIndex = data.Material;
            if (materialIndex < 0 || materialIndex >= static_cast<int>(model.materials.size()))
            {
                materialIndex = EnsureDefaultMaterial(model);
            }

            primitive.material = materialIndex;
            primitive.mode = (data.Mode >= 0) ? data.Mode : TINYGLTF_MODE_TRIANGLES;
            return true;
        }
    }
}