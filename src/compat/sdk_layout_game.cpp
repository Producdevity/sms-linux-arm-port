// The SDK layouts as the game compiles them (src/compat/sdk_layout_list.h).
#include <dolphin/ar.h>
#include <dolphin/arq.h>
#include <dolphin/card.h>
#include <dolphin/dvd.h>
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>
#include <THPPlayer/THPFile.h>
#include <THPPlayer/THPInfo.h>

#include <stddef.h>

#define SDK_SIZE(type) sizeof(::type),
#define SDK_OFFSET(type, field) offsetof(::type, field),
extern "C" const unsigned long sunshine_sdk_layout_game[] = {
#include "sdk_layout_list.h"
};
