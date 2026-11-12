#!/bin/bash
# Builds the PortMaster port folder (the layout a ports/sunshine pull request uses)
# and an installable sunshine.zip from an aarch64 build directory. PROGRAM_LIST, a
# program list recorded on devices, ships as sunshine/programs.list so the game builds
# those shaders in the background before they are first drawn.
# Usage: [PROGRAM_LIST=FILE] tools/package-portmaster.sh BUILD_DIR OUTPUT_DIR
set -euo pipefail

if [ $# -ne 2 ]; then
    echo "usage: [PROGRAM_LIST=FILE] $0 BUILD_DIR OUTPUT_DIR" >&2
    exit 2
fi
if [ -n "${PROGRAM_LIST:-}" ] && ! head -1 "$PROGRAM_LIST" | grep -qx '# starlet program list 1'; then
    echo "PROGRAM_LIST is not a program list: $PROGRAM_LIST" >&2
    exit 1
fi
root=$(cd "$(dirname "$0")/.." && pwd)
build=$(cd "$1" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
if grep -q '^SMS_DEV_TOOLS:BOOL=ON' "$build/CMakeCache.txt"; then
    echo "$build has SMS_DEV_TOOLS on: development hooks never ship" >&2
    exit 1
fi
strip=$(sed -n 's/^CMAKE_STRIP:FILEPATH=//p' "$build/CMakeCache.txt")
port="$out/sunshine"

for binary in sunshine sunshine-setup; do
    [ -f "$build/$binary" ] || { echo "missing $build/$binary; build the sunshine and sunshine-setup targets" >&2; exit 1; }
done
# The catalogue images are device captures of the game, kept out of the repository.
for image in screenshot.jpg cover.jpg; do
    [ -f "$root/packaging/ports/sunshine/$image" ] || { echo "missing packaging/ports/sunshine/$image (640x480 JPEG from a device capture)" >&2; exit 1; }
done

rm -rf "${port:?}" "${out:?}/sunshine.zip"
cp -R "$root/packaging/ports/sunshine" "$port"
# PortMaster ships scripts without the executable bit; the launcher sets it at run time.
chmod 644 "$port/Super Mario Sunshine.sh" "$port/sunshine/sunshine-prepare"
# The symbol table stays: crash reports in log.txt name the functions.
"$strip" --strip-debug -o "$port/sunshine/sunshine.aarch64" "$build/sunshine"
"$strip" -o "$port/sunshine/sunshine-setup" "$build/sunshine-setup"
cp "$root/tools/assets/GMSE01-gamedata.sha1" "$port/sunshine/sunshine-gamedata.sha1"
# The version and commit the game prints, so a log names the package even when the game never ran.
sed -n 's/^extern const char sunshine_port_version\[\] = "\(.*\)";$/\1/p' "$build/sunshine_version.cpp" > "$port/sunshine/VERSION"
[ -s "$port/sunshine/VERSION" ] || { echo "no version in $build/sunshine_version.cpp" >&2; exit 1; }
[ -z "${PROGRAM_LIST:-}" ] || cp "$PROGRAM_LIST" "$port/sunshine/programs.list"
# .gitignore excludes gamefiles/, so the folder and its note come from here.
mkdir "$port/sunshine/gamefiles" "$port/sunshine/licenses"
cp "$root/tools/assets/gamefiles-README.txt" "$port/sunshine/gamefiles/README.txt"
cp "$root/LICENSE" "$port/sunshine/licenses/LICENSE.sunshine.txt"
cp "$root/third_party/sms/LICENSE" "$port/sunshine/licenses/LICENSE.sms-english.txt"
cp "$root/third_party/sms-pc-port/LICENSE" "$port/sunshine/licenses/LICENSE.sms-pc-port.txt"
starlet=$(sed -n 's/^SMS_STARLET_DIR:PATH=//p' "$build/CMakeCache.txt")
[ -f "$starlet/LICENSE" ] || { echo "no Starlet LICENSE in $starlet" >&2; exit 1; }
cp "$starlet/LICENSE" "$port/sunshine/licenses/LICENSE.starlet.txt"
cp "$starlet/third_party/zstd/LICENSE" "$port/sunshine/licenses/LICENSE.zstd.txt"

# PortMaster's release zips keep the launcher at the root and move the catalogue
# files into the port folder, with the README renamed after the port.
stage=$(mktemp -d)
trap 'rm -rf "${stage:?}"' EXIT
cp "$port/Super Mario Sunshine.sh" "$stage/"
cp -R "$port/sunshine" "$stage/sunshine"
cp "$port/port.json" "$port/gameinfo.xml" "$port/screenshot.jpg" "$port/cover.jpg" "$stage/sunshine/"
cp "$port/README.md" "$stage/sunshine/sunshine.md"
(cd "$stage" && COPYFILE_DISABLE=1 zip -qrX9 "$out/sunshine.zip" "Super Mario Sunshine.sh" sunshine)
echo "$port"
echo "$out/sunshine.zip"
