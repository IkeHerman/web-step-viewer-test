// octree.cpp
#include "common.h"
#include "octree.h"

#include <algorithm>
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
}

bool TileOctree::Node::IsLeaf() const
{
    for (const std::unique_ptr<Node>& c : children)
    {
        if (c) return false;
    }
    return true;
}

const TileOctree::Node& TileOctree::Root() const
{
    if (!m_root)
    {
        throw std::runtime_error("TileOctree::Root() called before initialization.");
    }
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

    double sx = xmax - xmin;
    double sy = ymax - ymin;
    double sz = zmax - zmin;
    return std::max(sx, std::max(sy, sz));
}

int TileOctree::ChooseChildByItemCenter(const Bnd_Box& parent, const Bnd_Box& itemBounds)
{
    if (parent.IsVoid())
        return 0;

    double px0, py0, pz0, px1, py1, pz1;
    parent.Get(px0, py0, pz0, px1, py1, pz1);

    double pcx = (px0 + px1) * 0.5;
    double pcy = (py0 + py1) * 0.5;
    double pcz = (pz0 + pz1) * 0.5;

    // If item bounds are void, place deterministically.
    if (itemBounds.IsVoid())
        return 0;

    double ix0, iy0, iz0, ix1, iy1, iz1;
    itemBounds.Get(ix0, iy0, iz0, ix1, iy1, iz1);

    double icx = (ix0 + ix1) * 0.5;
    double icy = (iy0 + iy1) * 0.5;
    double icz = (iz0 + iz1) * 0.5;

    int xi = (icx >= pcx) ? 1 : 0;
    int yi = (icy >= pcy) ? 1 : 0;
    int zi = (icz >= pcz) ? 1 : 0;

    // Child ordering:
    // 0..3 = -z, 4..7 = +z
    // x toggles fastest, then y, then z.
    int index = xi + (yi << 1) + (zi << 2);
    return index;
}

std::array<Bnd_Box, 8> TileOctree::MakeChildVolumes(const Bnd_Box& parent, double looseFactor)
{
    std::array<Bnd_Box, 8> children;
    for (Bnd_Box& b : children) b.SetVoid();

    if (parent.IsVoid())
    {
        return children;
    }

    double xmin, ymin, zmin, xmax, ymax, zmax;
    GetMinMax(parent, xmin, ymin, zmin, xmax, ymax, zmax);

    // Parent center
    double cx = (xmin + xmax) * 0.5;
    double cy = (ymin + ymax) * 0.5;
    double cz = (zmin + zmax) * 0.5;

    // Strict child half-sizes are quarter of parent side length
    double hx = (xmax - xmin) * 0.25;
    double hy = (ymax - ymin) * 0.25;
    double hz = (zmax - zmin) * 0.25;

    // "Loose" expansion about child center
    double lax = hx * looseFactor;
    double lay = hy * looseFactor;
    double laz = hz * looseFactor;

    auto makeBox = [&](double ox, double oy, double oz) -> Bnd_Box
    {
        double ccx = cx + ox;
        double ccy = cy + oy;
        double ccz = cz + oz;

        Bnd_Box b;
        b.Update(ccx - lax, ccy - lay, ccz - laz,
                 ccx + lax, ccy + lay, ccz + laz);
        return b;
    };

    // Offsets based on strict child half-size to keep the octants centered correctly
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
        if (c.IsVoid()) continue;

        double cx0, cy0, cz0, cx1, cy1, cz1;
        GetMinMax(c, cx0, cy0, cz0, cx1, cy1, cz1);

        bool contains =
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
    {
        m_root = std::make_unique<Node>();
    }

    // Reset root
    m_root->items.clear();
    for (std::unique_ptr<Node>& c : m_root->children) c.reset();
    m_root->depth = 0;
    m_root->volume = globalBounds;
    m_global = globalBounds;

    std::vector<std::uint32_t> all;
    all.reserve(occurrences.size());

    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(occurrences.size()); ++i)
        all.push_back(i);

    if (all.empty() || m_global.IsVoid())
    {
        return;
    }

    BuildNode(*m_root, occurrences, all);
}

void TileOctree::BuildNode(Node& node,
                           const std::vector<Occurrence>& occurrences,
                           const std::vector<std::uint32_t>& inputItems)
{
    // Leaf / stop condition => store items here (LEAVES ONLY).
    if (node.depth >= m_cfg.maxDepth ||
        static_cast<int>(inputItems.size()) <= m_cfg.maxItemsPerNode ||
        MaxSideLength(node.volume) <= m_cfg.minNodeMaxSide)
    {
        std::cout << "Stopping at depth " << node.depth
                  << " with " << inputItems.size() << " items.\n";
        node.items = inputItems;
        return;
    }

    // Internal node => it will NOT keep items anymore.
    node.items.clear();

    std::array<Bnd_Box, 8> childVolumes = MakeChildVolumes(node.volume, m_cfg.looseFactor);
    std::array<std::vector<std::uint32_t>, 8> buckets;

    // Bucket everything into children. If it doesn't fit cleanly, force it down by center.
    for (std::uint32_t idx : inputItems)
    {
        const Bnd_Box& itemBox = occurrences[static_cast<std::size_t>(idx)].WorldBounds;

        int childIndex = FindContainingChild(childVolumes, itemBox);
        if (childIndex < 0)
        {
            childIndex = ChooseChildByItemCenter(node.volume, itemBox);
        }

        buckets[static_cast<std::size_t>(childIndex)].push_back(idx);
    }

    // If, for some reason, nothing ended up in buckets, stop here as a leaf.
    // (Shouldn't happen, but keeps things robust.)
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
        std::cout << "No child buckets populated at depth " << node.depth
                  << " - forcing leaf.\n";
        node.items = inputItems;
        for (std::unique_ptr<Node>& c : node.children) c.reset();
        return;
    }

    // Recurse into children
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

        BuildNode(*child, occurrences, bucket);
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
