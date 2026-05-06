#include "gltf_buffer_consolidate.h"

#include "dep/tinygltf/tiny_gltf.h"

#include <cstring>
#include <vector>

namespace model2tile
{
namespace
{
std::size_t AlignUpSize(std::size_t value, std::size_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }
    const std::size_t rem = value % alignment;
    return rem == 0 ? value : value + (alignment - rem);
}
} // namespace

bool ConsolidateGltfBuffersToSingle(tinygltf::Model& model, std::string& outError)
{
    outError.clear();
    if (model.buffers.size() <= 1)
    {
        return true;
    }
    constexpr std::size_t kAlign = 8;
    const std::size_t bufCount = model.buffers.size();
    std::vector<std::size_t> base(bufCount, 0);
    std::size_t total = 0;
    for (std::size_t i = 0; i < bufCount; ++i)
    {
        if (i > 0)
        {
            total = AlignUpSize(total, kAlign);
        }
        base[i] = total;
        const std::size_t chunk = model.buffers[i].data.size();
        total += chunk;
        if (chunk == 0 && i + 1 < bufCount)
        {
            total += kAlign;
        }
    }

    std::vector<unsigned char> combined(total, 0);
    for (std::size_t i = 0; i < bufCount; ++i)
    {
        const std::vector<unsigned char>& src = model.buffers[i].data;
        if (!src.empty())
        {
            if (base[i] + src.size() > combined.size())
            {
                outError = "consolidate internal size mismatch";
                return false;
            }
            std::memcpy(combined.data() + base[i], src.data(), src.size());
        }
    }
    for (tinygltf::BufferView& bv : model.bufferViews)
    {
        if (bv.buffer < 0 || static_cast<std::size_t>(bv.buffer) >= bufCount)
        {
            outError = "bufferView references invalid buffer index";
            return false;
        }
        const std::size_t bidx = static_cast<std::size_t>(bv.buffer);
        bv.byteOffset = base[bidx] + bv.byteOffset;
        bv.buffer = 0;
    }
    tinygltf::Buffer one;
    one.data = std::move(combined);
    one.uri.clear();
    one.name.clear();
    model.buffers.clear();
    model.buffers.push_back(std::move(one));

    return true;
}
} // namespace model2tile
