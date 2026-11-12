#pragma once

#include "starlet/disc/install.hpp"

namespace sunshine::setup {

// Super Mario Sunshine (USA): GMSE01 disc 0 revision 0, with the mario.dol SHA-1 the pinned decomp builds.
// With a manifest, the extracted gamedata files must match it.
starlet::disc::Game supported_game(const starlet::disc::Manifest* manifest);

} // namespace sunshine::setup
