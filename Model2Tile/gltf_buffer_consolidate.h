#pragma once

#include <string>

namespace tinygltf
{
class Model;
}

namespace model2tile
{
/// Concatenate all buffers in `model` into a single buffer at index 0 so GLB writers put
/// geometry (and any bufferView-backed images) in the BIN chunk instead of extra data URIs.
bool ConsolidateGltfBuffersToSingle(tinygltf::Model& model, std::string& outError);
} // namespace model2tile
