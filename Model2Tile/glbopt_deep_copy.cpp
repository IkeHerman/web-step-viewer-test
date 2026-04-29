#include "glbopt_internal.h"

#include <algorithm>
#include <cmath>

namespace glbopt
{
    namespace internal
    {
        static void ApplyCadPbrFallback(tinygltf::Material& material)
        {
            tinygltf::PbrMetallicRoughness& pbr = material.pbrMetallicRoughness;

            // CAD exports commonly provide only a base color. In glTF, missing
            // metallic/roughness factors default to metallic=1, roughness=1,
            // which can appear near-black without environment lighting.
            if (pbr.metallicRoughnessTexture.index < 0)
            {
                pbr.metallicFactor = 0.0;
            }

            if (!std::isfinite(pbr.roughnessFactor))
            {
                pbr.roughnessFactor = 1.0;
            }
        }

        void MergeUniqueStrings(
            std::vector<std::string>& destination,
            const std::vector<std::string>& source)
        {
            for (const std::string& s : source)
            {
                if (std::find(destination.begin(), destination.end(), s) == destination.end())
                {
                    destination.push_back(s);
                }
            }
        }

        static int CopyBuffer(DeepCopyContext& context, int sourceBufferIndex)
        {
            if (sourceBufferIndex < 0)
            {
                return -1;
            }

            const auto it = context.BufferMap.find(sourceBufferIndex);
            if (it != context.BufferMap.end())
            {
                return it->second;
            }

            tinygltf::Buffer copy = context.Source.buffers[static_cast<std::size_t>(sourceBufferIndex)];
            context.Destination.buffers.push_back(std::move(copy));

            const int destinationIndex = static_cast<int>(context.Destination.buffers.size() - 1);
            context.BufferMap.emplace(sourceBufferIndex, destinationIndex);
            return destinationIndex;
        }

        static int CopyBufferView(DeepCopyContext& context, int sourceBufferViewIndex)
        {
            if (sourceBufferViewIndex < 0)
            {
                return -1;
            }

            const auto it = context.BufferViewMap.find(sourceBufferViewIndex);
            if (it != context.BufferViewMap.end())
            {
                return it->second;
            }

            tinygltf::BufferView copy = context.Source.bufferViews[static_cast<std::size_t>(sourceBufferViewIndex)];
            copy.buffer = CopyBuffer(context, copy.buffer);

            context.Destination.bufferViews.push_back(std::move(copy));

            const int destinationIndex = static_cast<int>(context.Destination.bufferViews.size() - 1);
            context.BufferViewMap.emplace(sourceBufferViewIndex, destinationIndex);
            return destinationIndex;
        }

        static int CopyImage(DeepCopyContext& context, int sourceImageIndex)
        {
            if (sourceImageIndex < 0)
            {
                return -1;
            }

            const auto it = context.ImageMap.find(sourceImageIndex);
            if (it != context.ImageMap.end())
            {
                return it->second;
            }

            tinygltf::Image copy = context.Source.images[static_cast<std::size_t>(sourceImageIndex)];
            if (copy.bufferView >= 0)
            {
                copy.bufferView = CopyBufferView(context, copy.bufferView);
            }

            context.Destination.images.push_back(std::move(copy));

            const int destinationIndex = static_cast<int>(context.Destination.images.size() - 1);
            context.ImageMap.emplace(sourceImageIndex, destinationIndex);
            return destinationIndex;
        }

        static int CopySampler(DeepCopyContext& context, int sourceSamplerIndex)
        {
            if (sourceSamplerIndex < 0)
            {
                return -1;
            }

            const auto it = context.SamplerMap.find(sourceSamplerIndex);
            if (it != context.SamplerMap.end())
            {
                return it->second;
            }

            tinygltf::Sampler copy = context.Source.samplers[static_cast<std::size_t>(sourceSamplerIndex)];
            context.Destination.samplers.push_back(std::move(copy));

            const int destinationIndex = static_cast<int>(context.Destination.samplers.size() - 1);
            context.SamplerMap.emplace(sourceSamplerIndex, destinationIndex);
            return destinationIndex;
        }

        static int CopyTexture(DeepCopyContext& context, int sourceTextureIndex)
        {
            if (sourceTextureIndex < 0)
            {
                return -1;
            }

            const auto it = context.TextureMap.find(sourceTextureIndex);
            if (it != context.TextureMap.end())
            {
                return it->second;
            }

            tinygltf::Texture copy = context.Source.textures[static_cast<std::size_t>(sourceTextureIndex)];
            copy.source = CopyImage(context, copy.source);
            copy.sampler = CopySampler(context, copy.sampler);

            context.Destination.textures.push_back(std::move(copy));

            const int destinationIndex = static_cast<int>(context.Destination.textures.size() - 1);
            context.TextureMap.emplace(sourceTextureIndex, destinationIndex);
            return destinationIndex;
        }

        static void RemapTextureInfo(DeepCopyContext& context, tinygltf::TextureInfo& info)
        {
            if (info.index >= 0)
            {
                info.index = CopyTexture(context, info.index);
            }
        }

        static void RemapNormalTextureInfo(DeepCopyContext& context, tinygltf::NormalTextureInfo& info)
        {
            if (info.index >= 0)
            {
                info.index = CopyTexture(context, info.index);
            }
        }

        static void RemapOcclusionTextureInfo(DeepCopyContext& context, tinygltf::OcclusionTextureInfo& info)
        {
            if (info.index >= 0)
            {
                info.index = CopyTexture(context, info.index);
            }
        }

        int CopyMaterial(DeepCopyContext& context, int sourceMaterialIndex)
        {
            if (sourceMaterialIndex < 0)
            {
                return -1;
            }

            const auto it = context.MaterialMap.find(sourceMaterialIndex);
            if (it != context.MaterialMap.end())
            {
                return it->second;
            }

            tinygltf::Material copy = context.Source.materials[static_cast<std::size_t>(sourceMaterialIndex)];

            RemapTextureInfo(context, copy.pbrMetallicRoughness.baseColorTexture);
            RemapTextureInfo(context, copy.pbrMetallicRoughness.metallicRoughnessTexture);
            RemapNormalTextureInfo(context, copy.normalTexture);
            RemapOcclusionTextureInfo(context, copy.occlusionTexture);
            RemapTextureInfo(context, copy.emissiveTexture);

            ApplyCadPbrFallback(copy);

            context.Destination.materials.push_back(std::move(copy));

            const int destinationIndex = static_cast<int>(context.Destination.materials.size() - 1);
            context.MaterialMap.emplace(sourceMaterialIndex, destinationIndex);
            return destinationIndex;
        }
    }
}