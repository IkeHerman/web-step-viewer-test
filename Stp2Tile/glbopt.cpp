// glbopt.cpp
#include "glbopt.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <unordered_set>
#include <limits>

#include "dep/tinygltf/tiny_gltf.h"
#include "dep/meshoptimizer/src/meshoptimizer.h"

namespace GlbOpt
{
    // ------------------------------------------------------------
    // Debug helpers
    // ------------------------------------------------------------
    static size_t BufferBytes(const tinygltf::Model& model)
    {
        size_t total = 0;
        for (size_t i = 0; i < model.buffers.size(); ++i)
            total += model.buffers[i].data.size();
        return total;
    }

    static const char* ModeName(int mode)
    {
        switch (mode)
        {
            case TINYGLTF_MODE_POINTS:         return "POINTS";
            case TINYGLTF_MODE_LINE:           return "LINE";
            case TINYGLTF_MODE_LINE_LOOP:      return "LINE_LOOP";
            case TINYGLTF_MODE_LINE_STRIP:     return "LINE_STRIP";
            case TINYGLTF_MODE_TRIANGLES:      return "TRIANGLES";
            case TINYGLTF_MODE_TRIANGLE_STRIP: return "TRIANGLE_STRIP";
            case TINYGLTF_MODE_TRIANGLE_FAN:   return "TRIANGLE_FAN";
            default:                           return "UNKNOWN";
        }
    }


    static size_t GetComponentSize(int componentType)
    {
        switch (componentType)
        {
            case TINYGLTF_COMPONENT_TYPE_BYTE:           return 1;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  return 1;
            case TINYGLTF_COMPONENT_TYPE_SHORT:          return 2;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return 2;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:   return 4;
            case TINYGLTF_COMPONENT_TYPE_FLOAT:          return 4;
            default:                                     return 0;
        }
    }

    static int GetNumComponents(int type)
    {
        switch (type)
        {
            case TINYGLTF_TYPE_SCALAR: return 1;
            case TINYGLTF_TYPE_VEC2:   return 2;
            case TINYGLTF_TYPE_VEC3:   return 3;
            case TINYGLTF_TYPE_VEC4:   return 4;
            case TINYGLTF_TYPE_MAT2:   return 4;
            case TINYGLTF_TYPE_MAT3:   return 9;
            case TINYGLTF_TYPE_MAT4:   return 16;
            default:                   return 0;
        }
    }

    static const unsigned char* GetAccessorDataPtr(const tinygltf::Model& model, const tinygltf::Accessor& accessor)
    {
        if (accessor.bufferView < 0) return nullptr;
        const tinygltf::BufferView& bv = model.bufferViews[static_cast<size_t>(accessor.bufferView)];
        if (bv.buffer < 0) return nullptr;

        const tinygltf::Buffer& b = model.buffers[static_cast<size_t>(bv.buffer)];
        const size_t offset = static_cast<size_t>(bv.byteOffset) + static_cast<size_t>(accessor.byteOffset);
        if (offset > b.data.size()) return nullptr;

        return b.data.data() + offset;
    }

    static size_t GetAccessorStrideBytes(const tinygltf::Model& model, const tinygltf::Accessor& accessor)
    {
        const tinygltf::BufferView& bv = model.bufferViews[static_cast<size_t>(accessor.bufferView)];
        if (bv.byteStride != 0) return static_cast<size_t>(bv.byteStride);

        const size_t compSize = GetComponentSize(accessor.componentType);
        const int compCount = GetNumComponents(accessor.type);
        return compSize * static_cast<size_t>(compCount);
    }

    // ------------------------------------------------------------
    // Attribute + geometry containers
    // ------------------------------------------------------------
    struct AttributeStream
    {
        std::string name;
        int type = 0;
        int componentType = 0;
        bool normalized = false;

        size_t count = 0;
        size_t elemSizeBytes = 0;
        size_t srcStrideBytes = 0;
        std::vector<unsigned char> data; // tightly packed
    };

    static bool LoadAttributeTightlyPacked(
        const tinygltf::Model& model,
        const std::string& name,
        int accessorIndex,
        AttributeStream& out)
    {
        out = AttributeStream();
        out.name = name;

        if (accessorIndex < 0) return false;

        const tinygltf::Accessor& acc = model.accessors[static_cast<size_t>(accessorIndex)];
        const unsigned char* src = GetAccessorDataPtr(model, acc);
        if (src == nullptr) return false;

        const size_t compSize = GetComponentSize(acc.componentType);
        const int compCount = GetNumComponents(acc.type);
        if (compSize == 0 || compCount == 0) return false;

        out.type = acc.type;
        out.componentType = acc.componentType;
        out.normalized = acc.normalized;
        out.count = static_cast<size_t>(acc.count);
        out.elemSizeBytes = compSize * static_cast<size_t>(compCount);
        out.srcStrideBytes = GetAccessorStrideBytes(model, acc);

        out.data.resize(out.count * out.elemSizeBytes);

        for (size_t i = 0; i < out.count; ++i)
        {
            const unsigned char* srcElem = src + i * out.srcStrideBytes;
            unsigned char* dstElem = out.data.data() + i * out.elemSizeBytes;
            std::memcpy(dstElem, srcElem, out.elemSizeBytes);
        }

        return true;
    }

    static bool ReadIndicesU32(
        const tinygltf::Model& model,
        const tinygltf::Primitive& prim,
        std::vector<unsigned int>& out)
    {
        out.clear();
        if (prim.indices < 0) return false;

        const tinygltf::Accessor& acc = model.accessors[static_cast<size_t>(prim.indices)];
        const unsigned char* data = GetAccessorDataPtr(model, acc);
        if (data == nullptr) return false;

        const size_t count = static_cast<size_t>(acc.count);
        out.resize(count);

        if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
        {
            const std::uint16_t* src = reinterpret_cast<const std::uint16_t*>(data);
            for (size_t i = 0; i < count; ++i) out[i] = static_cast<unsigned int>(src[i]);
            return true;
        }

        if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
        {
            const std::uint32_t* src = reinterpret_cast<const std::uint32_t*>(data);
            for (size_t i = 0; i < count; ++i) out[i] = static_cast<unsigned int>(src[i]);
            return true;
        }

        return false;
    }

    static bool BuildSequentialIndicesForNonIndexed(
        const tinygltf::Model& model,
        const tinygltf::Primitive& prim,
        std::vector<unsigned int>& out,
        size_t& outVertexCount)
    {
        out.clear();
        outVertexCount = 0;

        std::map<std::string, int>::const_iterator itPos = prim.attributes.find("POSITION");
        if (itPos == prim.attributes.end()) return false;

        const tinygltf::Accessor& posAcc = model.accessors[static_cast<size_t>(itPos->second)];
        outVertexCount = static_cast<size_t>(posAcc.count);

        if (prim.mode != TINYGLTF_MODE_TRIANGLES) return false;
        if ((outVertexCount % 3) != 0) return false;

        out.resize(outVertexCount);
        for (size_t i = 0; i < outVertexCount; ++i) out[i] = static_cast<unsigned int>(i);
        return true;
    }

    struct PrimitiveGeom
    {
        int mode = TINYGLTF_MODE_TRIANGLES;
        std::vector<unsigned int> indices;       // u32
        std::vector<AttributeStream> attributes; // POSITION first (enforced)
    };

    // ------------------------------------------------------------
    // GLB rebuild helpers (compaction!)
    // ------------------------------------------------------------
    struct NewBufferBuilder
    {
        tinygltf::Buffer buffer;

        size_t Append(const void* bytes, size_t byteCount, size_t alignment)
        {
            size_t offset = buffer.data.size();
            size_t pad = 0;
            if (alignment != 0)
            {
                const size_t mod = offset % alignment;
                pad = (mod == 0) ? 0 : (alignment - mod);
            }
            if (pad != 0) buffer.data.resize(offset + pad);

            size_t start = buffer.data.size();
            buffer.data.resize(start + byteCount);
            std::memcpy(buffer.data.data() + start, bytes, byteCount);
            return start;
        }
    };

    static int AddBufferView(tinygltf::Model& model, int bufferIndex, size_t byteOffset, size_t byteLength, int target)
    {
        tinygltf::BufferView bv;
        bv.buffer = bufferIndex;
        bv.byteOffset = byteOffset;
        bv.byteLength = byteLength;
        bv.byteStride = 0;
        bv.target = target;
        model.bufferViews.push_back(bv);
        return static_cast<int>(model.bufferViews.size() - 1);
    }

    static int AddAccessor(
        tinygltf::Model& model,
        int bufferViewIndex,
        int componentType,
        int type,
        int count,
        bool normalized)
    {
        tinygltf::Accessor acc;
        acc.bufferView = bufferViewIndex;
        acc.byteOffset = 0;
        acc.componentType = componentType;
        acc.type = type;
        acc.count = count;
        acc.normalized = normalized;
        model.accessors.push_back(acc);
        return static_cast<int>(model.accessors.size() - 1);
    }

    static void SetMinMaxForPositionAccessor(tinygltf::Accessor& acc, const float* positions, size_t vertexCount)
    {
        if (vertexCount == 0) return;

        float minx = positions[0], miny = positions[1], minz = positions[2];
        float maxx = positions[0], maxy = positions[1], maxz = positions[2];

        for (size_t i = 0; i < vertexCount; ++i)
        {
            const float x = positions[i * 3 + 0];
            const float y = positions[i * 3 + 1];
            const float z = positions[i * 3 + 2];

            minx = std::min(minx, x);
            miny = std::min(miny, y);
            minz = std::min(minz, z);

            maxx = std::max(maxx, x);
            maxy = std::max(maxy, y);
            maxz = std::max(maxz, z);
        }

        acc.minValues = { minx, miny, minz };
        acc.maxValues = { maxx, maxy, maxz };
    }

    // ------------------------------------------------------------
    // Primitive extraction + GLOBAL weld (squash materials)
    // ------------------------------------------------------------
    static bool ExtractPrimitiveGeom(
        const tinygltf::Model& model,
        const tinygltf::Primitive& prim,
        PrimitiveGeom& out,
        bool verbose)
    {
        out = PrimitiveGeom();
        out.mode = prim.mode;

        if (prim.mode != TINYGLTF_MODE_TRIANGLES)
        {
            if (verbose)
                std::cout << "GlbOpt: unsupported mode " << ModeName(prim.mode) << "\n";
            return false;
        }

        std::map<std::string, int>::const_iterator itPos = prim.attributes.find("POSITION");
        if (itPos == prim.attributes.end()) return false;

        AttributeStream pos;
        if (!LoadAttributeTightlyPacked(model, "POSITION", itPos->second, pos)) return false;

        // keep float3 POSITION for now
        if (pos.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT || pos.type != TINYGLTF_TYPE_VEC3) return false;

        out.attributes.push_back(pos);

        // load other float attributes; stable ordering by name
        std::vector<std::pair<std::string, int>> attrsSorted;
        attrsSorted.reserve(prim.attributes.size());
        for (std::map<std::string, int>::const_iterator it = prim.attributes.begin(); it != prim.attributes.end(); ++it)
        {
            if (it->first == "POSITION") continue;
            attrsSorted.push_back(std::make_pair(it->first, it->second));
        }
        std::sort(attrsSorted.begin(), attrsSorted.end(),
                  [](const std::pair<std::string,int>& a, const std::pair<std::string,int>& b){ return a.first < b.first; });

        for (size_t i = 0; i < attrsSorted.size(); ++i)
        {
            AttributeStream a;
            if (LoadAttributeTightlyPacked(model, attrsSorted[i].first, attrsSorted[i].second, a))
            {
                if (a.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT && a.count == out.attributes[0].count)
                {
                    out.attributes.push_back(a);
                }
            }
        }

        const size_t vertexCount = out.attributes[0].count;

        // indices
        std::vector<unsigned int> indices;
        bool hasIndices = ReadIndicesU32(model, prim, indices);
        if (!hasIndices)
        {
            size_t inferredVertexCount = 0;
            if (!BuildSequentialIndicesForNonIndexed(model, prim, indices, inferredVertexCount)) return false;
            if (inferredVertexCount != vertexCount) return false;
        }

        if ((indices.size() % 3) != 0) return false;

        out.indices.swap(indices);
        return true;
    }

    static PrimitiveGeom WeldAllPrimitives(const std::vector<PrimitiveGeom>& prims)
    {
        PrimitiveGeom out;
        out.mode = TINYGLTF_MODE_TRIANGLES;
        if (prims.empty()) return out;

        // Use attribute layout from first primitive.
        // Since you said "ok to squash materials", we still MUST keep consistent attribute layout.
        // For primitives that don't match the first layout, we'll simply skip them (with debug elsewhere).
        out.attributes = prims[0].attributes;

        const size_t attrCount = out.attributes.size();

        size_t totalVertices = 0;
        size_t totalIndices = 0;

        for (size_t i = 0; i < prims.size(); ++i)
        {
            totalVertices += prims[i].attributes[0].count;
            totalIndices += prims[i].indices.size();
        }

        // merged indices
        out.indices.resize(totalIndices);

        size_t idxCursor = 0;
        size_t vBase = 0;

        for (size_t pi = 0; pi < prims.size(); ++pi)
        {
            const PrimitiveGeom& g = prims[pi];
            for (size_t i = 0; i < g.indices.size(); ++i)
                out.indices[idxCursor++] = static_cast<unsigned int>(g.indices[i] + static_cast<unsigned int>(vBase));

            vBase += g.attributes[0].count;
        }

        // merged attributes
        std::vector<AttributeStream> merged;
        merged.resize(attrCount);

        for (size_t ai = 0; ai < attrCount; ++ai)
        {
            const AttributeStream& ref = out.attributes[ai];

            AttributeStream m;
            m.name = ref.name;
            m.type = ref.type;
            m.componentType = ref.componentType;
            m.normalized = ref.normalized;
            m.elemSizeBytes = ref.elemSizeBytes;
            m.count = totalVertices;
            m.data.resize(totalVertices * m.elemSizeBytes);

            size_t vCursor = 0;
            for (size_t pi = 0; pi < prims.size(); ++pi)
            {
                const AttributeStream& src = prims[pi].attributes[ai];
                const size_t bytes = src.count * src.elemSizeBytes;
                std::memcpy(m.data.data() + vCursor * m.elemSizeBytes, src.data.data(), bytes);
                vCursor += src.count;
            }

            merged[ai] = std::move(m);
        }

        out.attributes.swap(merged);
        return out;
    }

    static void WeldInPlace(PrimitiveGeom& prim)
    {
        if (prim.attributes.empty())
            return;

        const size_t vertexCount = prim.attributes[0].count;
        if (vertexCount == 0 || prim.indices.empty())
            return;

        // Build an interleaved stream of ALL attributes for correct dedupe
        const size_t attrCount = prim.attributes.size();

        std::vector<size_t> offsets(attrCount, 0);
        size_t stride = 0;
        for (size_t a = 0; a < attrCount; ++a)
        {
            offsets[a] = stride;
            stride += prim.attributes[a].elemSizeBytes;
        }

        std::vector<unsigned char> interleaved(vertexCount * stride);

        for (size_t v = 0; v < vertexCount; ++v)
        {
            unsigned char* dst = interleaved.data() + v * stride;

            for (size_t a = 0; a < attrCount; ++a)
            {
                const AttributeStream& s = prim.attributes[a];
                const unsigned char* src = s.data.data() + v * s.elemSizeBytes;
                std::memcpy(dst + offsets[a], src, s.elemSizeBytes);
            }
        }

        // Remap table
        std::vector<unsigned int> remap(vertexCount);

        const size_t newVertexCount = meshopt_generateVertexRemap(
            remap.data(),
            prim.indices.data(),
            prim.indices.size(),
            interleaved.data(),
            vertexCount,
            stride);

        if (newVertexCount == vertexCount)
            return; // nothing to do

        // Remap indices
        std::vector<unsigned int> newIndices(prim.indices.size());
        meshopt_remapIndexBuffer(
            newIndices.data(),
            prim.indices.data(),
            prim.indices.size(),
            remap.data());
        prim.indices.swap(newIndices);

        // Remap all attributes
        for (size_t a = 0; a < attrCount; ++a)
        {
            AttributeStream& stream = prim.attributes[a];

            std::vector<unsigned char> newData(newVertexCount * stream.elemSizeBytes);

            meshopt_remapVertexBuffer(
                newData.data(),
                stream.data.data(),
                stream.count,
                stream.elemSizeBytes,
                remap.data());

            stream.data.swap(newData);
            stream.count = newVertexCount;
        }
    }

 // Drop-in replacement for OptimizeGeomInPlace + required helper.
// Assumes:
// - prim.attributes[0] is POSITION, float3 tightly packed (elemSizeBytes == sizeof(float)*3)
// - prim.indices is a u32 triangle list
// - AttributeStream has: name, count, elemSizeBytes, data
// - PrimitiveGeom has: mode, indices, attributes
// - Options has: maxTriangleCountTotal, sloppySimplification, simplifyMaxError, overdrawThreshold
// - WeldInPlace(PrimitiveGeom&) already exists in your file (the multi-attribute weld you wrote)

static void OptimizeVertexFetchInPlace(PrimitiveGeom& prim)
{
    if (prim.attributes.empty())
        return;

    const size_t vertexCount = prim.attributes[0].count;
    if (vertexCount == 0 || prim.indices.empty())
        return;

    std::vector<unsigned int> remap(vertexCount);

    const size_t newVertexCount = meshopt_optimizeVertexFetchRemap(
        remap.data(),
        prim.indices.data(),
        prim.indices.size(),
        vertexCount);

    if (newVertexCount == vertexCount)
        return;

    // Remap indices
    {
        std::vector<unsigned int> newIndices(prim.indices.size());
        meshopt_remapIndexBuffer(
            newIndices.data(),
            prim.indices.data(),
            prim.indices.size(),
            remap.data());
        prim.indices.swap(newIndices);
    }

    // Remap all attributes and shrink
    for (size_t a = 0; a < prim.attributes.size(); ++a)
    {
        AttributeStream& stream = prim.attributes[a];

        std::vector<unsigned char> newData(newVertexCount * stream.elemSizeBytes);

        meshopt_remapVertexBuffer(
            newData.data(),
            stream.data.data(),
            stream.count, // old count
            stream.elemSizeBytes,
            remap.data());

        stream.data.swap(newData);
        stream.count = newVertexCount;
    }
}

static void OptimizeGeomInPlace(
    PrimitiveGeom& prim,
    const Options& options,
    bool verbose)
{
    if (prim.mode != TINYGLTF_MODE_TRIANGLES)
        return;

    if (prim.attributes.empty())
        return;

    if (prim.indices.empty() || (prim.indices.size() % 3) != 0)
        return;

    // POSITION must be float3 tightly packed
    AttributeStream& pos = prim.attributes[0];
    if (pos.name != "POSITION" ||
        pos.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT ||
        pos.type != TINYGLTF_TYPE_VEC3 ||
        pos.elemSizeBytes != sizeof(float) * 3)
    {
        return;
    }

    const size_t vertexCountBefore = prim.attributes[0].count;
    const size_t triBefore = prim.indices.size() / 3;

    // 1) Weld first (best for simplify stability; reduces holes/cracks)
    WeldInPlace(prim);

    // 2) Simplify (meshoptimizer 1.0 signatures)
    const size_t triAfterWeld = prim.indices.size() / 3;
    if (options.maxTriangleCountTotal > 0 &&
        triAfterWeld > static_cast<size_t>(options.maxTriangleCountTotal))
    {
        const size_t targetIndexCount = static_cast<size_t>(options.maxTriangleCountTotal) * 3;

        const float* positionsF = reinterpret_cast<const float*>(prim.attributes[0].data.data());

        std::vector<unsigned int> simplified(prim.indices.size());

        float lodError = 0.0f;
        size_t newIndexCount = 0;

        // Sloppy simplify: vertex_lock is optional (NULL ok)
        newIndexCount = meshopt_simplifySloppy(
            simplified.data(),
            prim.indices.data(),
            prim.indices.size(),
            positionsF,
            prim.attributes[0].count,
            sizeof(float) * 3,
            (const unsigned char*)NULL, // vertex_lock
            targetIndexCount,
            options.simplifyMaxError,
            &lodError);

        simplified.resize(newIndexCount);
        prim.indices.swap(simplified);

        // Optional: weld again after simplify (can create new duplicates)
        WeldInPlace(prim);
    }

    // 3) Vertex cache optimize (index reorder)
    meshopt_optimizeVertexCache(
        prim.indices.data(),
        prim.indices.data(),
        prim.indices.size(),
        prim.attributes[0].count);

    // 4) Overdraw optimize (index reorder using positions)
    {
        const float* positionsF = reinterpret_cast<const float*>(prim.attributes[0].data.data());
        meshopt_optimizeOverdraw(
            prim.indices.data(),
            prim.indices.data(),
            prim.indices.size(),
            positionsF,
            prim.attributes[0].count,
            sizeof(float) * 3,
            options.overdrawThreshold);
    }

    // 5) Vertex fetch optimize (remap indices + reorder/shrink all attribute streams)
    OptimizeVertexFetchInPlace(prim);

    const size_t vertexCountAfter = prim.attributes[0].count;
    const size_t triAfter = prim.indices.size() / 3;

    if (verbose)
    {
        std::cout << "GlbOpt: optimize"
                  << " verts " << vertexCountBefore << " -> " << vertexCountAfter
                  << " tris " << triBefore << " -> " << triAfter
                  << " (budget=" << options.maxTriangleCountTotal << ")\n";
    }
}

    // ------------------------------------------------------------
    // Rebuild optimized geometry back into a compact model
    // We create ONE mesh with ONE primitive (squashed materials ok).
    // ------------------------------------------------------------
    static bool RebuildModelGeometryCompactSingleMesh(
        tinygltf::Model& model,
        const PrimitiveGeom& g,
        bool verbose)
    {
        // Clear old geometry
        model.buffers.clear();
        model.bufferViews.clear();
        model.accessors.clear();

        // Ensure at least one buffer
        model.buffers.push_back(tinygltf::Buffer());

        NewBufferBuilder builder;
        builder.buffer = tinygltf::Buffer();

        // Build a single mesh with a single primitive
        model.meshes.clear();
        model.meshes.push_back(tinygltf::Mesh());
        tinygltf::Mesh& mesh = model.meshes[0];
        mesh.name = "GlbOpt_WeldedMesh";

        tinygltf::Primitive p;
        p.mode = g.mode;
        p.material = -1; // squashed

        // indices: use u16 if possible
        unsigned int maxIndex = 0;
        for (size_t i = 0; i < g.indices.size(); ++i)
            maxIndex = std::max(maxIndex, g.indices[i]);

        const bool useU16 = (maxIndex <= 65535);

        if (useU16)
        {
            std::vector<std::uint16_t> idx16;
            idx16.resize(g.indices.size());
            for (size_t i = 0; i < g.indices.size(); ++i)
                idx16[i] = static_cast<std::uint16_t>(g.indices[i]);

            const size_t off = builder.Append(idx16.data(), idx16.size() * sizeof(std::uint16_t), 2);
            const int bv = AddBufferView(model, 0, off, idx16.size() * sizeof(std::uint16_t), TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
            const int acc = AddAccessor(model, bv, TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, TINYGLTF_TYPE_SCALAR, static_cast<int>(idx16.size()), false);
            p.indices = acc;
        }
        else
        {
            std::vector<std::uint32_t> idx32;
            idx32.resize(g.indices.size());
            for (size_t i = 0; i < g.indices.size(); ++i)
                idx32[i] = static_cast<std::uint32_t>(g.indices[i]);

            const size_t off = builder.Append(idx32.data(), idx32.size() * sizeof(std::uint32_t), 4);
            const int bv = AddBufferView(model, 0, off, idx32.size() * sizeof(std::uint32_t), TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
            const int acc = AddAccessor(model, bv, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, TINYGLTF_TYPE_SCALAR, static_cast<int>(idx32.size()), false);
            p.indices = acc;
        }

        // attributes
        for (size_t ai = 0; ai < g.attributes.size(); ++ai)
        {
            const AttributeStream& a = g.attributes[ai];

            const size_t off = builder.Append(a.data.data(), a.data.size(), 4);
            const int bv = AddBufferView(model, 0, off, a.data.size(), TINYGLTF_TARGET_ARRAY_BUFFER);

            const int acc = AddAccessor(model, bv, a.componentType, a.type, static_cast<int>(a.count), a.normalized);
            p.attributes[a.name] = acc;

            if (a.name == "POSITION" && a.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT && a.type == TINYGLTF_TYPE_VEC3)
            {
                tinygltf::Accessor& posAcc = model.accessors[static_cast<size_t>(acc)];
                const float* pf = reinterpret_cast<const float*>(a.data.data());
                SetMinMaxForPositionAccessor(posAcc, pf, a.count);
            }
        }

        mesh.primitives.clear();
        mesh.primitives.push_back(p);

        // Finalize buffer
        model.buffers[0] = std::move(builder.buffer);

        // Ensure nodes reference mesh 0 (we'll keep the first node if present, else create)
        if (model.nodes.empty())
        {
            model.nodes.push_back(tinygltf::Node());
            model.nodes[0].mesh = 0;
            model.scenes.clear();
            model.scenes.push_back(tinygltf::Scene());
            model.scenes[0].nodes = { 0 };
            model.defaultScene = 0;
        }
        else
        {
            // Set first node to mesh 0; clear others to avoid duplicates (simple)
            model.nodes[0].mesh = 0;
            for (size_t i = 1; i < model.nodes.size(); ++i)
                model.nodes[i].mesh = -1;
            if (!model.scenes.empty())
            {
                if (model.scenes[static_cast<size_t>(model.defaultScene >= 0 ? model.defaultScene : 0)].nodes.empty())
                    model.scenes[static_cast<size_t>(model.defaultScene >= 0 ? model.defaultScene : 0)].nodes = { 0 };
            }
        }

        return true;
    }


    // Add these helpers somewhere in glbopt.cpp (inside namespace GlbOpt).
// They assume POSITION is float3 in attributes[0] (as in your extractor).

    static const float* GetPositionsF32(const PrimitiveGeom& prim)
    {
        if (prim.attributes.empty()) return nullptr;

        const AttributeStream& pos = prim.attributes[0];
        if (pos.name != "POSITION") return nullptr;
        if (pos.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) return nullptr;
        if (pos.type != TINYGLTF_TYPE_VEC3) return nullptr;
        if (pos.elemSizeBytes != sizeof(float) * 3) return nullptr;

        return reinterpret_cast<const float*>(pos.data.data());
    }

    // Removes:
    //  1) Index-degenerate tris: i0==i1 || i1==i2 || i2==i0
    //  2) Geometric degenerates: area^2 <= areaEpsilonSq (optional; set epsilon <= 0 to disable)
    //
    // areaEpsilon is in the same units as POSITION (e.g. meters). Typical values:
    //  - 0: only index-degenerates
    //  - 1e-12 .. 1e-8 depending on your scale
    //
    // Returns number of triangles removed.
    static size_t RemoveDegenerateTriangles(PrimitiveGeom& prim, float areaEpsilon)
    {
        if (prim.mode != TINYGLTF_MODE_TRIANGLES) return 0;
        if ((prim.indices.size() % 3) != 0) return 0;

        const float* positions = GetPositionsF32(prim);
        if (positions == nullptr) return 0;

        const size_t vertexCount = prim.attributes[0].count;
        if (vertexCount == 0) return 0;

        const float eps = (areaEpsilon > 0.0f) ? areaEpsilon : 0.0f;
        const float epsSq = eps * eps;

        std::vector<unsigned int> out;
        out.reserve(prim.indices.size());

        size_t removed = 0;

        for (size_t i = 0; i < prim.indices.size(); i += 3)
        {
            unsigned int i0 = prim.indices[i + 0];
            unsigned int i1 = prim.indices[i + 1];
            unsigned int i2 = prim.indices[i + 2];

            // Bounds check (defensive)
            if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
            {
                removed += 1;
                continue;
            }

            // Index-degenerate
            if (i0 == i1 || i1 == i2 || i2 == i0)
            {
                removed += 1;
                continue;
            }

            // Optional geometric degeneracy test
            if (epsSq > 0.0f)
            {
                const float ax = positions[i0 * 3 + 0];
                const float ay = positions[i0 * 3 + 1];
                const float az = positions[i0 * 3 + 2];

                const float bx = positions[i1 * 3 + 0];
                const float by = positions[i1 * 3 + 1];
                const float bz = positions[i1 * 3 + 2];

                const float cx = positions[i2 * 3 + 0];
                const float cy = positions[i2 * 3 + 1];
                const float cz = positions[i2 * 3 + 2];

                // Compute cross((b-a),(c-a)) length^2
                const float abx = bx - ax;
                const float aby = by - ay;
                const float abz = bz - az;

                const float acx = cx - ax;
                const float acy = cy - ay;
                const float acz = cz - az;

                const float cxp = (aby * acz) - (abz * acy);
                const float cyp = (abz * acx) - (abx * acz);
                const float czp = (abx * acy) - (aby * acx);

                const float area2Sq = cxp * cxp + cyp * cyp + czp * czp; // (2*area)^2

                // Compare against (2*epsArea)^2: using epsSq directly is fine as a heuristic.
                if (area2Sq <= epsSq)
                {
                    removed += 1;
                    continue;
                }
            }

            out.push_back(i0);
            out.push_back(i1);
            out.push_back(i2);
        }

        prim.indices.swap(out);
        return removed;
    }

    // Removes duplicate triangles by treating each triangle as an unordered set of 3 vertex indices.
    // (i.e., (a,b,c) == (b,c,a) == (c,b,a), and also matches reversed winding.)
    //
    // Returns number of triangles removed.
    static size_t RemoveDuplicateTriangles(PrimitiveGeom& prim)
    {
        if (prim.mode != TINYGLTF_MODE_TRIANGLES) return 0;
        if ((prim.indices.size() % 3) != 0) return 0;

        struct TriKey
        {
            unsigned int a;
            unsigned int b;
            unsigned int c;

            bool operator==(const TriKey& o) const
            {
                return a == o.a && b == o.b && c == o.c;
            }
        };

        struct TriKeyHash
        {
            size_t operator()(const TriKey& k) const
            {
                // Simple 64-bit mix; good enough for triangle keys.
                size_t h = static_cast<size_t>(k.a) * 73856093u;
                h ^= static_cast<size_t>(k.b) * 19349663u;
                h ^= static_cast<size_t>(k.c) * 83492791u;
                return h;
            }
        };

        auto sort3 = [](unsigned int& x, unsigned int& y, unsigned int& z)
        {
            if (x > y) std::swap(x, y);
            if (y > z) std::swap(y, z);
            if (x > y) std::swap(x, y);
        };

        std::unordered_set<TriKey, TriKeyHash> seen;
        seen.reserve(prim.indices.size() / 3);

        std::vector<unsigned int> out;
        out.reserve(prim.indices.size());

        size_t removed = 0;

        for (size_t i = 0; i < prim.indices.size(); i += 3)
        {
            unsigned int i0 = prim.indices[i + 0];
            unsigned int i1 = prim.indices[i + 1];
            unsigned int i2 = prim.indices[i + 2];

            // Canonicalize as unordered triple
            unsigned int a = i0;
            unsigned int b = i1;
            unsigned int c = i2;
            sort3(a, b, c);

            TriKey key{ a, b, c };

            if (seen.find(key) != seen.end())
            {
                removed += 1;
                continue;
            }

            seen.insert(key);

            out.push_back(i0);
            out.push_back(i1);
            out.push_back(i2);
        }

        prim.indices.swap(out);
        return removed;
    }



    // ------------------------------------------------------------
    // Main optimize: weld EVERYTHING into one primitive, then optimize once
    // ------------------------------------------------------------
    static bool OptimizeModelWeldAllInPlace(tinygltf::Model& model, const Options& options)
    {
        // Extract all supported primitives, but only those that match the FIRST primitive's attribute layout.
        std::vector<PrimitiveGeom> extractedAll;
        extractedAll.reserve(64);

        bool firstLayoutSet = false;
        std::vector<std::pair<std::string, int>> firstAttrOrder;
        std::vector<int> firstAttrType;
        std::vector<int> firstAttrComp;
        std::vector<bool> firstAttrNorm;

        auto layoutMatchesFirst = [&](const PrimitiveGeom& g) -> bool
        {
            if (!firstLayoutSet) return true;
            if (g.attributes.size() != firstAttrOrder.size()) return false;

            for (size_t i = 0; i < g.attributes.size(); ++i)
            {
                const AttributeStream& a = g.attributes[i];
                if (a.name != firstAttrOrder[i].first) return false;
                if (a.type != firstAttrType[i]) return false;
                if (a.componentType != firstAttrComp[i]) return false;
                if (a.normalized != firstAttrNorm[i]) return false;
            }
            return true;
        };

        size_t skippedUnsupported = 0;
        size_t skippedLayout = 0;

        for (size_t mi = 0; mi < model.meshes.size(); ++mi)
        {
            const tinygltf::Mesh& mesh = model.meshes[mi];

            for (size_t pi = 0; pi < mesh.primitives.size(); ++pi)
            {
                PrimitiveGeom g;
                bool ok = ExtractPrimitiveGeom(model, mesh.primitives[pi], g, options.verbose);
                if (!ok)
                {
                    ++skippedUnsupported;
                    if (!options.skipUnsupportedPrimitives)
                        return false;
                    continue;
                }

                // Set first layout signature after first extracted primitive
                if (!firstLayoutSet)
                {
                    firstLayoutSet = true;
                    firstAttrOrder.clear();
                    firstAttrType.clear();
                    firstAttrComp.clear();
                    firstAttrNorm.clear();

                    for (size_t ai = 0; ai < g.attributes.size(); ++ai)
                    {
                        firstAttrOrder.push_back(std::make_pair(g.attributes[ai].name, 0));
                        firstAttrType.push_back(g.attributes[ai].type);
                        firstAttrComp.push_back(g.attributes[ai].componentType);
                        firstAttrNorm.push_back(g.attributes[ai].normalized);
                    }
                }

                if (!layoutMatchesFirst(g))
                {
                    ++skippedLayout;
                    if (options.verbose)
                    {
                        std::cout << "GlbOpt: skipping primitive (layout mismatch) mesh=" << mi << " prim=" << pi << "\n";
                    }
                    continue;
                }

                extractedAll.push_back(std::move(g));
            }
        }

        if (extractedAll.empty())
        {
            if (options.verbose)
            {
                std::cout << "GlbOpt: no supported primitives found. skippedUnsupported=" << skippedUnsupported
                          << " skippedLayout=" << skippedLayout << "\n";
            }
            return options.skipUnsupportedPrimitives;
        }

        if (options.verbose)
        {
            size_t totalTris = 0;
            size_t totalVerts = 0;
            for (size_t i = 0; i < extractedAll.size(); ++i)
            {
                totalTris += extractedAll[i].indices.size() / 3;
                totalVerts += extractedAll[i].attributes[0].count;
            }

            std::cout << "GlbOpt: extracted primitives=" << extractedAll.size()
                      << " totalVerts=" << totalVerts
                      << " totalTris=" << totalTris
                      << " skippedUnsupported=" << skippedUnsupported
                      << " skippedLayout=" << skippedLayout
                      << "\n";
        }

        // Weld into a single PrimitiveGeom
        PrimitiveGeom welded = WeldAllPrimitives(extractedAll);

        // Optimize once (global budget only)
        OptimizeGeomInPlace(welded, options, options.verbose);

        // Rebuild compact model to actually shrink
        if (!RebuildModelGeometryCompactSingleMesh(model, welded, options.verbose))
            return false;

        return true;
    }

    // ------------------------------------------------------------
    // Public API
    // ------------------------------------------------------------
    bool GlbOptimize(const std::filesystem::path& inGlbPath,
                     const std::filesystem::path& outGlbPath,
                     const Options& options)
    {
        tinygltf::TinyGLTF gltf;
        tinygltf::Model model;
        std::string err;
        std::string warn;

        const bool loadOk = gltf.LoadBinaryFromFile(&model, &err, &warn, inGlbPath.string().c_str());
        if (!warn.empty()) std::cout << "tinygltf warn: " << warn << "\n";
        if (!err.empty())  std::cerr << "tinygltf err: " << err << "\n";
        if (!loadOk) return false;

        const size_t inBytes = BufferBytes(model);
        if (options.verbose)
        {
            std::cout << "GlbOpt: input buffers total bytes=" << inBytes
                      << " meshes=" << model.meshes.size()
                      << " nodes=" << model.nodes.size()
                      << "\n";
        }

        const bool optimizeOk = OptimizeModelWeldAllInPlace(model, options);
        if (!optimizeOk) return false;

        const size_t outBytes = BufferBytes(model);
        if (options.verbose)
        {
            std::cout << "GlbOpt: after rebuild, buffers total bytes=" << outBytes
                      << " (delta=" << static_cast<long long>(outBytes) - static_cast<long long>(inBytes) << ")\n";
        }

        const bool writeOk = gltf.WriteGltfSceneToFile(
            &model,
            outGlbPath.string(),
            true,  // embedImages
            true,  // embedBuffers
            true,  // prettyPrint
            true); // writeBinary

        if (options.verbose)
        {
            std::cout << "GlbOpt: wrote " << outGlbPath.string()
                      << " ok=" << (writeOk ? "true" : "false") << "\n";
        }

        return writeOk;
    }

    bool GlbOptimize(const std::filesystem::path& inGlbPath,
                     const std::filesystem::path& outGlbPath,
                     std::uint32_t maxTriangleCount)
    {
        Options opt;
        opt.maxTriangleCountTotal = maxTriangleCount; // NOTE: now treated as GLOBAL
        opt.verbose = true;
        return GlbOptimize(inGlbPath, outGlbPath, opt);
    }

    bool GlbOptimizeLods(const std::filesystem::path& inGlbPath,
                         const std::filesystem::path& outDir,
                         const std::vector<std::uint32_t>& lodTriangleCounts,
                         const Options& baseOptions)
    {
        if (lodTriangleCounts.empty()) return false;

        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);

        const std::string baseName = inGlbPath.stem().string();

        for (size_t i = 0; i < lodTriangleCounts.size(); ++i)
        {
            Options opt = baseOptions;
            opt.maxTriangleCountTotal = lodTriangleCounts[i];

            const std::filesystem::path outPath =
                outDir / (baseName + "_LOD" + std::to_string(i) + ".glb");

            const bool ok = GlbOptimize(inGlbPath, outPath, opt);
            if (!ok) return false;
        }

        return true;
    }

} // namespace GlbOpt