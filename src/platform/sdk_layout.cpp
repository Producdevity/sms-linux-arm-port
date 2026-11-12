// Compares the SDK layouts the game compiled with Starlet's (src/compat/sdk_layout_list.h).
#define STARLET_SDK_TYPES_ONLY
#include "starlet/sdk/ar.h"
#include "starlet/sdk/card.h"
#include "starlet/sdk/dvd.h"
#include "starlet/sdk/gx.h"
#include "starlet/sdk/os.h"
#include "starlet/sdk/pad.h"
#include "starlet/sdk/thp.h"
#include "starlet/sdk/vi.h"

#include "platform/sdk_layout.hpp"

#include <cstddef>
#include <cstdio>
#include <stdexcept>

extern "C" const unsigned long sunshine_sdk_layout_game[];

namespace sms::platform {
namespace {
#define SDK_SIZE(type) sizeof(starlet::sdk::type),
#define SDK_OFFSET(type, field) offsetof(starlet::sdk::type, field),
const unsigned long starlet_layout[] = {
#include "compat/sdk_layout_list.h"
};
#undef SDK_SIZE
#undef SDK_OFFSET
#define SDK_SIZE(type) "sizeof(" #type ")",
#define SDK_OFFSET(type, field) "offsetof(" #type ", " #field ")",
const char* const names[] = {
#include "compat/sdk_layout_list.h"
};
}

void check_sdk_layout() {
    bool same = true;
    for (std::size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        if (sunshine_sdk_layout_game[i] == starlet_layout[i]) continue;
        std::printf("SDK layout: %s is %lu in the game, %lu in Starlet\n", names[i], sunshine_sdk_layout_game[i],
                    starlet_layout[i]);
        same = false;
    }
    if (!same) throw std::runtime_error("the game's SDK structures differ from Starlet's");
}

}
