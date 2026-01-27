// octree.h
#pragma once

#include "common.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <Bnd_Box.hxx>

/// Octree for 3D Tiles preprocessing built from Occurrence.WorldBounds (Bnd_Box).
/// - Partitions space into octants (AABB volumes).
/// - Attempts to assign each occurrence index to a child that FULLY CONTAINS its WorldBounds.
/// - If an occurrence does not fully fit any child, it is FORCED into a child (currently by item AABB center).
///
/// IMPORTANT: Items are stored ONLY in LEAF nodes. Internal nodes keep no items.
///
/// Output: Node.items = indices into the original occurrences vector (LEAVES ONLY).
class TileOctree
{
public:
    struct Config
    {
        int maxDepth = 10;
        int maxItemsPerNode = 512;
        double minNodeMaxSide = 1.0;  // stop subdividing if node max side <= this

        // Expands child volumes slightly around their centers to reduce boundary misses.
        // 1.0 = strict. Typical: 1.1–1.5.
        double looseFactor = 1.0;
    };

    struct Node
    {
        Bnd_Box volume;
        int depth = 0;

        // Indices into occurrences. With current rules, ONLY populated for leaf nodes.
        std::vector<std::uint32_t> items;

        std::array<std::unique_ptr<Node>, 8> children;

        bool IsLeaf() const;
    };

public:
    TileOctree();
    explicit TileOctree(const Config& cfg);

    /// Build an octree over occurrences[i].WorldBounds.
    /// globalBounds should be the world-space bounds of the entire set.
    void Build(const std::vector<Occurrence>& occurrences, Bnd_Box& globalBounds);

    const Node& Root() const;
    const Bnd_Box& GlobalBounds() const;

    /// Collect all leaf items in a subtree.
    static void CollectSubtreeItems(const TileOctree::Node& node,
                                    std::vector<std::uint32_t>& out);

private:
    static void GetMinMax(const Bnd_Box& box,
                          double& xmin, double& ymin, double& zmin,
                          double& xmax, double& ymax, double& zmax);

    static double MaxSideLength(const Bnd_Box& box);

    static std::array<Bnd_Box, 8> MakeChildVolumes(const Bnd_Box& parent, double looseFactor);

    // Returns [0..7] if itemBounds is fully contained in one child; otherwise -1.
    static int FindContainingChild(const std::array<Bnd_Box, 8>& children,
                                   const Bnd_Box& itemBounds);

    // Forced placement rule when no child fully contains the item.
    // Current strategy: choose octant based on the item's AABB center vs parent center.
    static int ChooseChildByItemCenter(const Bnd_Box& parent,
                                       const Bnd_Box& itemBounds);

    void BuildNode(Node& node,
                   const std::vector<Occurrence>& occurrences,
                   const std::vector<std::uint32_t>& inputItems);

private:
    Config m_cfg;
    std::unique_ptr<Node> m_root;
    Bnd_Box m_global;
};