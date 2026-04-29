#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace exporters
{
struct TileBakeRequest
{
    std::vector<std::uint32_t> itemIndices;
    std::string outputBasename;
    double nodeBoundsDiagonal = -1.0;
    bool debugAppearance = false;
    bool keepGlb = false;
};

struct TileBakeResult
{
    bool ok = false;
    std::string glbPath;
    std::string b3dmPath;
    std::string error;
};

class ITileContentBaker
{
public:
    virtual ~ITileContentBaker() = default;
    virtual TileBakeResult Bake(const TileBakeRequest& request) = 0;
};
} // namespace exporters
