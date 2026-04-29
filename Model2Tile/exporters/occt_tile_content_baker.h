#pragma once

#include "i_tile_content_baker.h"

#include "../common.h"

#include <string>
#include <vector>

namespace exporters
{
class OcctTileContentBaker final : public ITileContentBaker
{
public:
    explicit OcctTileContentBaker(const std::vector<Occurrence>& occurrences);
    TileBakeResult Bake(const TileBakeRequest& request) override;

private:
    const std::vector<Occurrence>& m_occurrences;
};
} // namespace exporters
