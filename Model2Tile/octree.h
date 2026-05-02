// octree.h
#pragma once

#include "tiler/scene_ir_tile_items.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

class TileOctree
{
public:
    struct Config
    {
        int maxDepth = 10;
        int maxItemsPerNode = 512;
        std::uint64_t maxTrianglesPerNode = 50000;
        double minNodeMaxSide = 1.0;
        double looseFactor = 1.0;
    };

    struct Node
    {
        core::Aabb volume;
        int depth = 0;
        std::uint64_t totalTriangles = 0;
        std::vector<std::uint32_t> items;
        std::array<std::unique_ptr<Node>, 8> children;

        bool IsLeaf() const;
    };

public:
    TileOctree();
    explicit TileOctree(const Config& cfg);

    void Build(const std::vector<tiler::TileItem>& items, const core::Aabb& globalBounds);

    const Node& Root() const;
    const core::Aabb& GlobalBounds() const;

    static void CollectSubtreeItems(const TileOctree::Node& node,
                                    std::vector<std::uint32_t>& out);

private:
    static void GetMinMax(const core::Aabb& box,
                          double& xmin, double& ymin, double& zmin,
                          double& xmax, double& ymax, double& zmax);

    static double MaxSideLength(const core::Aabb& box);

    static std::array<core::Aabb, 8> MakeChildVolumes(const core::Aabb& parent, double looseFactor);

    /// Returns child index only if that one child strictly contains itemBounds; otherwise -1.
    /// With loose overlapping octants, multiple children often contain the same small AABB; returning
    /// the first match would pack almost everything into bucket 0 and abort subdivision as "ineffective".
    static int FindContainingChild(const std::array<core::Aabb, 8>& children,
                                   const core::Aabb& itemBounds);

    static int ChooseChildByItemCenter(const core::Aabb& parent,
                                       const core::Aabb& itemBounds);

    void BuildNode(Node& node,
                   const std::vector<tiler::TileItem>& items,
                   const std::vector<std::uint32_t>& inputItems,
                   std::uint64_t inputTotalTriangles);

private:
    Config m_cfg;
    std::unique_ptr<Node> m_root;
    core::Aabb m_global;
};
