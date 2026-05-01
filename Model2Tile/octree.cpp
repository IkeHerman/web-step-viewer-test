// octree.cpp
#include "octree.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
namespace {

const tiler::TileItem* FindTileItemById(
    const std::vector<tiler::TileItem>& items,
    const std::uint32_t id)
{
    if (id < items.size())
    {
        const tiler::TileItem& candidate = items[static_cast<std::size_t>(id)];
        if (candidate.id == id)
        {
            return &candidate;
        }
    }

    for (const tiler::TileItem& item : items)
    {
        if (item.id == id)
        {
            return &item;
        }
    }

    return nullptr;
}
} // namespace


TileOctree::TileOctree()
    : TileOctree(Config())
{
}

TileOctree::TileOctree(const Config& cfg)
    : m_cfg(cfg)
{
    m_root = std::make_unique<Node>();
    m_root->depth = 0;
    m_root->volume.valid = false;
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

const core::Aabb& TileOctree::GlobalBounds() const
{
    return m_global;
}

void TileOctree::GetMinMax(const core::Aabb& box,
                           double& xmin, double& ymin, double& zmin,
                           double& xmax, double& ymax, double& zmax)
{
    xmin = box.xmin;
    ymin = box.ymin;
    zmin = box.zmin;
    xmax = box.xmax;
    ymax = box.ymax;
    zmax = box.zmax;
}

double TileOctree::MaxSideLength(const core::Aabb& box)
{
    if (!box.valid)
        return 0.0;

    double xmin, ymin, zmin, xmax, ymax, zmax;
    GetMinMax(box, xmin, ymin, zmin, xmax, ymax, zmax);

    const double sx = xmax - xmin;
    const double sy = ymax - ymin;
    const double sz = zmax - zmin;
    return std::max(sx, std::max(sy, sz));
}

int TileOctree::ChooseChildByItemCenter(const core::Aabb& parent, const core::Aabb& itemBounds)
{
    if (!parent.valid)
        return 0;

    double px0, py0, pz0, px1, py1, pz1;
    GetMinMax(parent, px0, py0, pz0, px1, py1, pz1);

    const double pcx = (px0 + px1) * 0.5;
    const double pcy = (py0 + py1) * 0.5;
    const double pcz = (pz0 + pz1) * 0.5;

    if (!itemBounds.valid)
        return 0;

    double ix0, iy0, iz0, ix1, iy1, iz1;
    GetMinMax(itemBounds, ix0, iy0, iz0, ix1, iy1, iz1);

    const double icx = (ix0 + ix1) * 0.5;
    const double icy = (iy0 + iy1) * 0.5;
    const double icz = (iz0 + iz1) * 0.5;

    const int xi = (icx >= pcx) ? 1 : 0;
    const int yi = (icy >= pcy) ? 1 : 0;
    const int zi = (icz >= pcz) ? 1 : 0;

    return xi + (yi << 1) + (zi << 2);
}

std::array<core::Aabb, 8> TileOctree::MakeChildVolumes(const core::Aabb& parent, double looseFactor)
{
    std::array<core::Aabb, 8> children{};

    if (!parent.valid)
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

    auto makeBox = [&](double ox, double oy, double oz) -> core::Aabb
    {
        const double ccx = cx + ox;
        const double ccy = cy + oy;
        const double ccz = cz + oz;

        core::Aabb b;
        b.xmin = ccx - lax;
        b.ymin = ccy - lay;
        b.zmin = ccz - laz;
        b.xmax = ccx + lax;
        b.ymax = ccy + lay;
        b.zmax = ccz + laz;
        b.valid = true;
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

int TileOctree::FindContainingChild(const std::array<core::Aabb, 8>& children, const core::Aabb& itemBounds)
{
    if (!itemBounds.valid)
    {
        return -1;
    }

    double ix0, iy0, iz0, ix1, iy1, iz1;
    GetMinMax(itemBounds, ix0, iy0, iz0, ix1, iy1, iz1);

    int uniqueIndex = -1;
    int matchCount = 0;

    for (int i = 0; i < 8; ++i)
    {
        const core::Aabb& c = children[static_cast<std::size_t>(i)];
        if (!c.valid)
        {
            continue;
        }

        double cx0, cy0, cz0, cx1, cy1, cz1;
        GetMinMax(c, cx0, cy0, cz0, cx1, cy1, cz1);

        const bool contains =
            ix0 >= cx0 && iy0 >= cy0 && iz0 >= cz0 &&
            ix1 <= cx1 && iy1 <= cy1 && iz1 <= cz1;

        if (contains)
        {
            ++matchCount;
            uniqueIndex = i;
        }
    }

    if (matchCount == 1)
    {
        return uniqueIndex;
    }

    return -1;
}

void TileOctree::Build(const std::vector<tiler::TileItem>& items, const core::Aabb& globalBounds)
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
    all.reserve(items.size());

    for (const tiler::TileItem& item : items)
    {
        all.push_back(item.id);
        m_root->totalTriangles += item.triangleCount;
    }

    if (all.empty() || !m_global.valid)
        return;

    BuildNode(*m_root, items, all, m_root->totalTriangles);
}

void TileOctree::BuildNode(Node& node,
                           const std::vector<tiler::TileItem>& items,
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

    const std::array<core::Aabb, 8> childVolumes = MakeChildVolumes(node.volume, m_cfg.looseFactor);
    std::array<std::vector<std::uint32_t>, 8> buckets;
    std::array<std::uint64_t, 8> bucketTriangles{};
    bucketTriangles.fill(0);

    for (std::uint32_t idx : inputItems)
    {
        const tiler::TileItem* item = FindTileItemById(items, idx);
        if (!item)
        {
            continue;
        }
        const core::Aabb& itemBox = item->worldBounds;

        int childIndex = FindContainingChild(childVolumes, itemBox);
        if (childIndex < 0)
        {
            childIndex = ChooseChildByItemCenter(node.volume, itemBox);
        }

        buckets[static_cast<std::size_t>(childIndex)].push_back(idx);
        bucketTriangles[static_cast<std::size_t>(childIndex)] += item->triangleCount;
    }

    bool anyChildHasItems = false;
    for (int i = 0; i < 8; ++i)
    {
        if (!buckets[static_cast<std::size_t>(i)].empty())
        {
            anyChildHasItems = true;
            break;
        }
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

        BuildNode(*child, items, bucket, child->totalTriangles);
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
