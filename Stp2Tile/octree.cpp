// octree.cpp
#include "common.h"
#include "octree.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

TileOctree::TileOctree()
    : TileOctree(Config())
{
}

TileOctree::TileOctree(const Config& cfg)
    : m_cfg(cfg)
{
    m_root = std::make_unique<Node>();
    m_root->depth = 0;
    m_root->volume.SetVoid();
    m_root->totalTriangles = 0;
}

bool TileOctree::Node::IsLeaf() const
{
    for (const std::unique_ptr<Node>& c : children)
    {
        if (c)
            return false;
    }
    return true;
}

const TileOctree::Node& TileOctree::Root() const
{
    if (!m_root)
        throw std::runtime_error("TileOctree::Root() called before initialization.");

    return *m_root;
}

const Bnd_Box& TileOctree::GlobalBounds() const
{
    return m_global;
}

void TileOctree::GetMinMax(const Bnd_Box& box,
                           double& xmin, double& ymin, double& zmin,
                           double& xmax, double& ymax, double& zmax)
{
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
}

double TileOctree::MaxSideLength(const Bnd_Box& box)
{
    if (box.IsVoid())
        return 0.0;

    double xmin, ymin, zmin, xmax, ymax, zmax;
    GetMinMax(box, xmin, ymin, zmin, xmax, ymax, zmax);

    const double sx = xmax - xmin;
    const double sy = ymax - ymin;
    const double sz = zmax - zmin;
    return std::max(sx, std::max(sy, sz));
}

int TileOctree::ChooseChildByItemCenter(const Bnd_Box& parent, const Bnd_Box& itemBounds)
{
    if (parent.IsVoid())
        return 0;

    double px0, py0, pz0, px1, py1, pz1;
    parent.Get(px0, py0, pz0, px1, py1, pz1);

    const double pcx = (px0 + px1) * 0.5;
    const double pcy = (py0 + py1) * 0.5;
    const double pcz = (pz0 + pz1) * 0.5;

    if (itemBounds.IsVoid())
        return 0;

    double ix0, iy0, iz0, ix1, iy1, iz1;
    itemBounds.Get(ix0, iy0, iz0, ix1, iy1, iz1);

    const double icx = (ix0 + ix1) * 0.5;
    const double icy = (iy0 + iy1) * 0.5;
    const double icz = (iz0 + iz1) * 0.5;

    const int xi = (icx >= pcx) ? 1 : 0;
    const int yi = (icy >= pcy) ? 1 : 0;
    const int zi = (icz >= pcz) ? 1 : 0;

    return xi + (yi << 1) + (zi << 2);
}

std::array<Bnd_Box, 8> TileOctree::MakeChildVolumes(const Bnd_Box& parent, double looseFactor)
{
    std::array<Bnd_Box, 8> children;
    for (Bnd_Box& b : children)
        b.SetVoid();

    if (parent.IsVoid())
        return children;

    double xmin, ymin, zmin, xmax, ymax, zmax;
    GetMinMax(parent, xmin, ymin, zmin, xmax, ymax, zmax);

    const double cx = (xmin + xmax) * 0.5;
    const double cy = (ymin + ymax) * 0.5;
    const double cz = (zmin + zmax) * 0.5;

    const double hx = (xmax - xmin) * 0.25;
    const double hy = (ymax - ymin) * 0.25;
    const double hz = (zmax - zmin) * 0.25;

    const double lax = hx * looseFactor;
    const double lay = hy * looseFactor;
    const double laz = hz * looseFactor;

    auto makeBox = [&](double ox, double oy, double oz) -> Bnd_Box
    {
        const double ccx = cx + ox;
        const double ccy = cy + oy;
        const double ccz = cz + oz;

        Bnd_Box b;
        b.Update(ccx - lax, ccy - lay, ccz - laz,
                 ccx + lax, ccy + lay, ccz + laz);
        return b;
    };

    children[0] = makeBox(-hx, -hy, -hz);
    children[1] = makeBox(+hx, -hy, -hz);
    children[2] = makeBox(-hx, +hy, -hz);
    children[3] = makeBox(+hx, +hy, -hz);
    children[4] = makeBox(-hx, -hy, +hz);
    children[5] = makeBox(+hx, -hy, +hz);
    children[6] = makeBox(-hx, +hy, +hz);
    children[7] = makeBox(+hx, +hy, +hz);

    return children;
}

int TileOctree::FindContainingChild(const std::array<Bnd_Box, 8>& children, const Bnd_Box& itemBounds)
{
    if (itemBounds.IsVoid())
        return -1;

    double ix0, iy0, iz0, ix1, iy1, iz1;
    GetMinMax(itemBounds, ix0, iy0, iz0, ix1, iy1, iz1);

    for (int i = 0; i < 8; ++i)
    {
        const Bnd_Box& c = children[static_cast<std::size_t>(i)];
        if (c.IsVoid())
            continue;

        double cx0, cy0, cz0, cx1, cy1, cz1;
        GetMinMax(c, cx0, cy0, cz0, cx1, cy1, cz1);

        const bool contains =
            ix0 >= cx0 && iy0 >= cy0 && iz0 >= cz0 &&
            ix1 <= cx1 && iy1 <= cy1 && iz1 <= cz1;

        if (contains)
            return i;
    }

    return -1;
}

void TileOctree::Build(const std::vector<Occurrence>& occurrences, Bnd_Box& globalBounds)
{
    if (!m_root)
        m_root = std::make_unique<Node>();

    m_root->items.clear();
    for (std::unique_ptr<Node>& c : m_root->children)
        c.reset();

    m_root->depth = 0;
    m_root->volume = globalBounds;
    m_root->totalTriangles = 0;
    m_global = globalBounds;

    std::vector<std::uint32_t> all;
    all.reserve(occurrences.size());

    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(occurrences.size()); ++i)
    {
        all.push_back(i);
        m_root->totalTriangles += occurrences[static_cast<std::size_t>(i)].TriangleCount;
    }

    if (all.empty() || m_global.IsVoid())
        return;

    BuildNode(*m_root, occurrences, all, m_root->totalTriangles);
}

void TileOctree::BuildNode(Node& node,
                           const std::vector<Occurrence>& occurrences,
                           const std::vector<std::uint32_t>& inputItems,
                           std::uint64_t inputTotalTriangles)
{
    const std::uint64_t totalTriangles = inputTotalTriangles;
    node.totalTriangles = totalTriangles;

    if (node.depth >= m_cfg.maxDepth ||
        static_cast<int>(inputItems.size()) <= m_cfg.maxItemsPerNode ||
        totalTriangles <= m_cfg.maxTrianglesPerNode ||
        MaxSideLength(node.volume) <= m_cfg.minNodeMaxSide)
    {
        if (m_cfg.verbose)
        {
            std::cout << "Stopping at depth " << node.depth
                      << " with " << inputItems.size()
                      << " items and " << totalTriangles
                      << " triangles.\n";
        }
        node.items = inputItems;
        return;
    }

    node.items.clear();

    const std::array<Bnd_Box, 8> childVolumes = MakeChildVolumes(node.volume, m_cfg.looseFactor);
    std::array<std::vector<std::uint32_t>, 8> buckets;
    std::array<std::uint64_t, 8> bucketTriangles{};
    bucketTriangles.fill(0);

    int containedCount = 0;
    int forcedCount = 0;

    for (std::uint32_t idx : inputItems)
    {
        const Occurrence& occ = occurrences[static_cast<std::size_t>(idx)];
        const Bnd_Box& itemBox = occ.WorldBounds;

        int childIndex = FindContainingChild(childVolumes, itemBox);
        if (childIndex >= 0)
        {
            ++containedCount;
        }
        else
        {
            childIndex = ChooseChildByItemCenter(node.volume, itemBox);
            ++forcedCount;
        }

        buckets[static_cast<std::size_t>(childIndex)].push_back(idx);
        bucketTriangles[static_cast<std::size_t>(childIndex)] += occ.TriangleCount;
    }

    bool anyChildHasItems = false;
    int nonEmptyChildCount = 0;
    std::size_t largestBucketSize = 0;
    std::uint64_t largestBucketTriangles = 0;

    for (int i = 0; i < 8; ++i)
    {
        const std::size_t bucketSize = buckets[static_cast<std::size_t>(i)].size();
        if (bucketSize == 0)
            continue;

        anyChildHasItems = true;
        ++nonEmptyChildCount;
        largestBucketSize = std::max(largestBucketSize, bucketSize);
        largestBucketTriangles = std::max(largestBucketTriangles, bucketTriangles[static_cast<std::size_t>(i)]);
    }

    if (!anyChildHasItems)
    {
        if (m_cfg.verbose)
        {
            std::cout << "No child buckets populated at depth " << node.depth
                      << " - forcing leaf.\n";
        }
        node.items = inputItems;
        for (std::unique_ptr<Node>& c : node.children)
            c.reset();
        return;
    }

    const bool oneBucketGotEverything = (largestBucketSize == inputItems.size());
    const bool poorItemSplit = (largestBucketSize >= (inputItems.size() * 95) / 100);
    const bool poorTriangleSplit =
        (totalTriangles > 0) &&
        (largestBucketTriangles >= (totalTriangles * 95) / 100);
    const bool allForced = (forcedCount == static_cast<int>(inputItems.size()));

    if (oneBucketGotEverything || poorItemSplit || poorTriangleSplit || allForced)
    {
        if (m_cfg.verbose)
        {
            std::cout << "Stopping at depth " << node.depth
                      << " due to ineffective split"
                      << " (items=" << inputItems.size()
                      << ", triangles=" << totalTriangles
                      << ", children=" << nonEmptyChildCount
                      << ", contained=" << containedCount
                      << ", forced=" << forcedCount
                      << ", largestBucketItems=" << largestBucketSize
                      << ", largestBucketTriangles=" << largestBucketTriangles
                      << ")\n";
        }

        node.items = inputItems;
        for (std::unique_ptr<Node>& c : node.children)
            c.reset();
        return;
    }

    for (int i = 0; i < 8; ++i)
    {
        const std::vector<std::uint32_t>& bucket = buckets[static_cast<std::size_t>(i)];
        if (bucket.empty())
        {
            node.children[static_cast<std::size_t>(i)].reset();
            continue;
        }

        std::unique_ptr<Node> child = std::make_unique<Node>();
        child->volume = childVolumes[static_cast<std::size_t>(i)];
        child->depth = node.depth + 1;
        child->totalTriangles = bucketTriangles[static_cast<std::size_t>(i)];

        BuildNode(*child, occurrences, bucket, child->totalTriangles);
        node.children[static_cast<std::size_t>(i)] = std::move(child);
    }
}

void TileOctree::CollectSubtreeItems(const TileOctree::Node& node,
                                     std::vector<std::uint32_t>& out)
{
    if (node.IsLeaf())
    {
        out.insert(out.end(), node.items.begin(), node.items.end());
        return;
    }

    for (const auto& child : node.children)
    {
        if (!child)
            continue;

        CollectSubtreeItems(*child, out);
    }
}
