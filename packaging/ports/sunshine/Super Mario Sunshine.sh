#!/bin/bash

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/sunshine"
cd "$GAMEDIR"
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1
# Testers send log.txt; this names the package and device even when the game never starts.
echo "Package $(cat VERSION), $DEVICE_NAME ($DEVICE_CPU, ${DEVICE_RAM} GB), $CFW_NAME $CFW_VERSION, kernel $(uname -r)"

# The first launch builds gamedata/ from the disc image in gamefiles/.
if [ ! -f gamedata/.prepared ]; then
  export PATCHER_FILE="$GAMEDIR/sunshine-prepare"
  export PATCHER_GAME="Super Mario Sunshine"
  export PATCHER_TIME="2 to 5 minutes"
  $ESUDO chmod +x "$PATCHER_FILE"
  source "$controlfolder/utils/patcher.txt"
  # The patcher returns after a failed setup too; the game cannot run without gamedata/.
  if [ ! -f gamedata/.prepared ]; then
    pm_message "Game setup did not finish. See sunshine/setup.log."
    sleep 5
    pm_finish
    exit 1
  fi
fi

export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
$ESUDO chmod +x "$GAMEDIR/sunshine.${DEVICE_ARCH}"
mkdir -p saves shadercache

$GPTOKEYB "sunshine.${DEVICE_ARCH}" &
pm_platform_helper "$GAMEDIR/sunshine.${DEVICE_ARCH}"
./sunshine.${DEVICE_ARCH} gamedata saves shadercache

pm_finish
