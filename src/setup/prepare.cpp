#include "setup/prepare.hpp"

#ifndef SUNSHINE_GMSE01_DOL_SHA1
#error "SUNSHINE_GMSE01_DOL_SHA1 must name the decomp's main.dol SHA-1"
#endif

namespace sunshine::setup {

starlet::disc::Game supported_game(const starlet::disc::Manifest* manifest) {
    return {"Super Mario Sunshine (USA)", {{"GMSE01", 0, 0, SUNSHINE_GMSE01_DOL_SHA1, manifest}}};
}

} // namespace sunshine::setup
