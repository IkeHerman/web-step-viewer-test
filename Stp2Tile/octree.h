// octree.h
#pragma once

#include "common.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <Bnd_Box.hxx>

class TileOctree
{
public:
    struct Config
    {
        bool verbose = false;
        int maxDepth = 10;
        int maxItemsPerNode = 512;
        std::uint64_t maxTrianglesPerNode = 50000;
        double minNodeMaxSide = 1.0;
        double looseFactor = 1.0;
    };

    struct Node
    {
        Bnd_Box volume;
        int depth = 0;
        std::uint64_t totalTriangles = 0;
        std::vector<std::uint32_t> items;
        std::array<std::unique_ptr<Node>, 8> children;

        bool IsLeaf() const;
    };

public:
    TileOctree();
    explicit TileOctree(const Config& cfg);

    void Build(const std::vector<Occurrence>& occurrences, Bnd_Box& globalBounds);

    const Node& Root() const;
    const Bnd_Box& GlobalBounds() const;

    static void CollectSubtreeItems(const TileOctree::Node& node,
                                    std::vector<std::uint32_t>& out);

private:
    static void GetMinMax(const Bnd_Box& box,
                          double& xmin, double& ymin, double& zmin,
                          double& xmax, double& ymax, double& zmax);

    static double MaxSideLength(const Bnd_Box& box);

    static std::array<Bnd_Box, 8> MakeChildVolumes(const Bnd_Box& parent, double looseFactor);

    static int FindContainingChild(const std::array<Bnd_Box, 8>& children,
                                   const Bnd_Box& itemBounds);

    static int ChooseChildByItemCenter(const Bnd_Box& parent,
                                       const Bnd_Box& itemBounds);

    void BuildNode(Node& node,
                   const std::vector<Occurrence>& occurrences,
                   const std::vector<std::uint32_t>& inputItems,
                   std::uint64_t inputTotalTriangles);

private:
    Config m_cfg;
    std::unique_ptr<Node> m_root;
    Bnd_Box m_global;
};
