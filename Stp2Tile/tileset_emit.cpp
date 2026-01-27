#include "tileset_emit.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <Bnd_Box.hxx>
#include <Message_ProgressRange.hxx>

#include "export_glb.h"
#include "b3dm.h"
#include "glbopt.h"

namespace
{
    static void GetMinMax(const Bnd_Box& b,
                          double& xmin, double& ymin, double& zmin,
                          double& xmax, double& ymax, double& zmax)
    {
        b.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    }

    static double MaxSideLength(const Bnd_Box& b)
    {
        if (b.IsVoid())
            return 0.0;

        double xmin, ymin, zmin, xmax, ymax, zmax;
        GetMinMax(b, xmin, ymin, zmin, xmax, ymax, zmax);

        double sx = xmax - xmin;
        double sy = ymax - ymin;
        double sz = zmax - zmin;
        return std::max(sx, std::max(sy, sz));
    }

    static gp_Pnt ToGltfSpace(const gp_Pnt& p)
    {
        return gp_Pnt(p.X(), -p.Z(), p.Y());
    }

    static gp_Vec ToGltfSpace(const gp_Vec& v)
    {
        return gp_Vec(v.X(), -v.Z(), v.Y());
    }

    static std::array<double, 12> ToTilesBoxGltfSpace(const Bnd_Box& b)
    {
        double xmin, ymin, zmin, xmax, ymax, zmax;
        GetMinMax(b, xmin, ymin, zmin, xmax, ymax, zmax);

        // Center + half-lengths in OCCT space
        gp_Pnt cOcct(
            (xmin + xmax) * 0.5,
            (ymin + ymax) * 0.5,
            (zmin + zmax) * 0.5
        );

        double hxLen = (xmax - xmin) * 0.5;
        double hyLen = (ymax - ymin) * 0.5;
        double hzLen = (zmax - zmin) * 0.5;

        gp_Vec hxOcct(hxLen, 0.0, 0.0);
        gp_Vec hyOcct(0.0, hyLen, 0.0);
        gp_Vec hzOcct(0.0, 0.0, hzLen);

        // Convert into glTF space (must match your RWGltf_CafWriter axis conversion)
        gp_Pnt c = ToGltfSpace(cOcct);
        gp_Vec hx = ToGltfSpace(hxOcct);
        gp_Vec hy = ToGltfSpace(hyOcct);
        gp_Vec hz = ToGltfSpace(hzOcct);

        return {
            c.X(),  c.Y(),  c.Z(),
            hx.X(), hx.Y(), hx.Z(),
            hy.X(), hy.Y(), hy.Z(),
            hz.X(), hz.Y(), hz.Z()
        };
    }

    static double GeometricErrorFromBoundsHeuristic(const Bnd_Box& b)
    {
        double size = MaxSideLength(b); // full length of the largest side
        if (size <= 0.0)
            return 0.0;

        return size / 16.0;
    }


    static bool HasAnyChild(const TileOctree::Node& node)
    {
        for (const auto& c : node.children)
        {
            if (c) return true;
        }
        return false;
    }

    static void CollectSubtreeItems(const TileOctree::Node& node, std::vector<std::uint32_t>& out)
    {
        out.insert(out.end(), node.items.begin(), node.items.end());
        for (const auto& c : node.children)
        {
            if (!c) continue;
            CollectSubtreeItems(*c, out);
        }
    }

    static Bnd_Box ComputeTightBounds(const TileOctree::Node& node, const std::vector<Occurrence>& occs)
    {
        Bnd_Box b;
        b.SetVoid();

        std::vector<std::uint32_t> all;
        all.reserve(node.items.size());
        CollectSubtreeItems(node, all);

        for (std::uint32_t idx : all)
        {
            if (idx >= occs.size()) continue;
            const Bnd_Box& ob = occs[static_cast<std::size_t>(idx)].WorldBounds;
            if (ob.IsVoid()) continue;
            b.Add(ob);
        }
        return b;
    }

    static std::string JoinUri(const std::string& subdir, const std::string& name)
    {
        if (subdir.empty())
            return name;
        if (subdir.back() == '/' || subdir.back() == '\\')
            return subdir + name;
        return subdir + "/" + name;
    }

    static void Indent(std::ostringstream& ss, int depth)
    {
        for (int i = 0; i < depth; ++i) ss << "  ";
    }

    struct EmitState
    {
        std::uint32_t nextNodeId = 0;

        // Optional debugging: map tileId -> occurrence indices
        std::ostringstream manifest; // writes a JSON array
        bool manifestStarted = false;
    };

    static void ManifestBegin(EmitState& st)
    {
        if (st.manifestStarted) return;
        st.manifestStarted = true;
        st.manifest << "[\n";
    }

    static void ManifestAdd(EmitState& st, std::uint32_t nodeId, const std::vector<std::uint32_t>& items)
    {
        ManifestBegin(st);

        st.manifest << "  {\"tileId\":" << nodeId << ",\"occurrenceIndices\":[";
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            if (i) st.manifest << ",";
            st.manifest << items[i];
        }
        st.manifest << "]}";
        st.manifest << ",\n";
    }

    static void ManifestEnd(EmitState& st)
    {
        if (!st.manifestStarted) return;

        std::string s = st.manifest.str();
        // Remove trailing ",\n" if present
        if (s.size() >= 2)
        {
            if (s.rfind(",\n") == s.size() - 2)
            {
                s.erase(s.size() - 2);
            }
        }
        st.manifest.str("");
        st.manifest.clear();
        st.manifest << s << "\n]\n";
    }



    static void EmitTileNode(
        std::ostringstream& ss,
        const TileOctree::Node& node,
        const Handle(XCAFDoc_ShapeTool)& sourceShapeTool,
        const std::vector<Occurrence>& occurrences,
        const TilesetEmit::Options& opt,
        EmitState& st,
        int depth)
    {
        const std::uint32_t nodeId = st.nextNodeId++;

        const bool isLeaf = !HasAnyChild(node);

        // Content policy
        const bool isEmptyTile = node.items.empty();

        bool shouldWriteContent = !isEmptyTile;
        if (opt.contentOnlyAtLeaves && !isLeaf)
            shouldWriteContent = false;

        // Override: empty tiles should still write placeholder cube content
        if (isEmptyTile)
            shouldWriteContent = true;

        // Bounding volume
        Bnd_Box bv = node.volume;
        if (opt.useTightBounds)
        {
            Bnd_Box tight = ComputeTightBounds(node, occurrences);
            if (!tight.IsVoid())
                bv = tight;
        }

        // Export content now (so nodeId matches file name)
        std::string contentUri;
        if (shouldWriteContent)
        {
            const std::string fileName = opt.tileFilePrefix + std::to_string(nodeId);

            const std::filesystem::path tilesDir = std::filesystem::path(opt.tilesetOutDir) / opt.contentSubdir;
            std::filesystem::create_directories(tilesDir);

            const std::filesystem::path glbPath = tilesDir / (fileName + ".glb");
            const std::filesystem::path glbRawPath = tilesDir / (fileName + "_raw.glb");
            const std::filesystem::path b3dmPath = tilesDir / (fileName + ".b3dm");


            bool okGlb = false;

            if (!isEmptyTile)
            {
                okGlb = ExportTileToGlbFile(sourceShapeTool, occurrences, node.items, glbRawPath.string(),0.0f);

                GlbOpt::Options opt;
        
                opt.maxTriangleCountTotal = 0; 
                opt.verbose = true;

                GlbOpt::GlbOptimize(glbRawPath.string(), glbPath.string(), opt);
            }
            else
            {
                std::vector<std::uint32_t> allItems; 
                
                CollectSubtreeItems(node, allItems);
                
                okGlb = ExportTileToGlbFile(sourceShapeTool, occurrences, allItems, glbRawPath.string(),1.0f);

                GlbOpt::Options opt;
        
                opt.maxTriangleCountTotal = 0; 
                opt.verbose = true;

                GlbOpt::GlbOptimize(glbRawPath.string(), glbPath.string(), opt);
//                okGlb = ExportBoxToGlbFile(bv, glbPath.string());
            }

            if (okGlb)
            {
                bool okB3dm = B3dm::WrapGlbFileToB3dmFile(glbPath.string(), b3dmPath.string());
                if (okB3dm)
                {
                    contentUri = JoinUri(opt.contentSubdir, fileName + ".b3dm");

                    if (!opt.keepGlbFilesForDebug)
                    {
                        std::error_code ec;
                        std::filesystem::remove(glbPath, ec);
                    }

                    // Debug manifest only for real content tiles (optional)
                    if (!isEmptyTile)
                    {
                        ManifestAdd(st, nodeId, node.items);
                    }
                }
            }
        }

        // Emit JSON node
        Indent(ss, depth);
        ss << "{\n";

        Indent(ss, depth + 1);
        ss << "\"boundingVolume\":{\"box\":[";
        std::array<double, 12> box = ToTilesBoxGltfSpace(bv);
        for (int i = 0; i < 12; ++i)
        {
            if (i) ss << ",";
            ss << std::setprecision(15) << box[static_cast<std::size_t>(i)];
        }
        ss << "]}," << "\n";

        Indent(ss, depth + 1);
        double geometricError = isLeaf ? 0 : GeometricErrorFromBoundsHeuristic(bv);
        ss << "\"geometricError\":" << std::setprecision(15) << geometricError << ",\n";

        Indent(ss, depth + 1);
        const char* refineMode = "REPLACE";
        ss << "\"refine\":\"" << refineMode << "\"";

        if (!contentUri.empty())
        {
            ss << ",\n";
            Indent(ss, depth + 1);
            ss << "\"content\":{\"uri\":\"" << contentUri << "\"}";
        }

        // Children
        std::vector<const TileOctree::Node*> children;
        children.reserve(8);
        for (const auto& c : node.children)
        {
            if (c) children.push_back(c.get());
        }

        if (!children.empty())
        {
            ss << ",\n";
            Indent(ss, depth + 1);
            ss << "\"children\":[\n";

            for (std::size_t i = 0; i < children.size(); ++i)
            {
                EmitTileNode(ss, *children[i], sourceShapeTool, occurrences, opt, st, depth + 2);
                if (i + 1 < children.size()) ss << ",";
                ss << "\n";
            }

            Indent(ss, depth + 1);
            ss << "]";
        }

        ss << "\n";
        Indent(ss, depth);
        ss << "}";
    }
}

namespace TilesetEmit
{
    bool EmitTilesetAndB3dm(
        const TileOctree& tree,
        const Handle(XCAFDoc_ShapeTool)& sourceShapeTool,
        const std::vector<Occurrence>& occurrences,
        const Options& opt)
    {
        std::filesystem::create_directories(opt.tilesetOutDir);

        Bnd_Box rootBv = tree.Root().volume;
        if (opt.useTightBounds)
        {
            Bnd_Box tight = ComputeTightBounds(tree.Root(), occurrences);
            if (!tight.IsVoid())
                rootBv = tight;
        }

        EmitState st;
        std::ostringstream ss;

        const double rootGeometricError = GeometricErrorFromBoundsHeuristic(rootBv);

        ss << "{\n";
        ss << "  \"asset\":{\"version\":\"1.0\"},\n";
        ss << "  \"geometricError\":" << std::setprecision(15) << rootGeometricError << ",\n";
        ss << "  \"root\":\n";

        EmitTileNode(ss, tree.Root(), sourceShapeTool, occurrences, opt, st, 2);

        ss << "\n}\n";

        // Write tileset.json
        {
            const std::filesystem::path tilesetPath = std::filesystem::path(opt.tilesetOutDir) / "tileset.json";
            std::ofstream f(tilesetPath);
            if (!f) return false;
            f << ss.str();
        }

        // Write optional manifest (helps you confirm “which occurrences in which tile”)
        ManifestEnd(st);
        if (st.manifestStarted)
        {
            const std::filesystem::path manifestPath = std::filesystem::path(opt.tilesetOutDir) / "tile_manifest.json";
            std::ofstream mf(manifestPath);
            if (mf)
                mf << st.manifest.str();
        }

        return true;
    }
}
