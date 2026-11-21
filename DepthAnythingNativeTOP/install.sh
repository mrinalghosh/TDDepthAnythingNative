#!/bin/zsh
# Installs build/DepthAnythingNativeTOP.plugin where TouchDesigner looks for Custom OPs.
# Usage: ./install.sh [plugins-folder]
#   default:  ~/Library/Application Support/Derivative/TouchDesigner099/Plugins   (every project; the folder name is
#             still TouchDesigner099 in current TD builds)
#   or pass a project's own Plugins folder (next to its .toe) to install it for that project only.
# TouchDesigner only loads plugins at startup, so restart it after installing.
set -euo pipefail
dest="${1:-$HOME/Library/Application Support/Derivative/TouchDesigner099/Plugins}"
dest="${dest:A}"  # absolute, before changing directory
cd "$(dirname "$0")"
NAME=DepthAnythingNativeTOP
SRC="build/$NAME.plugin"

if [[ ! -d "$SRC" ]]; then
  echo "No $SRC yet. Run ./build.sh first." >&2
  exit 1
fi

mkdir -p "$dest"
# Replace the whole bundle (new files, not overwritten in place), so a TD that still has the old one loaded
# doesn't crash on a changed binary under it.
rm -rf "$dest/$NAME.plugin"
ditto "$SRC" "$dest/$NAME.plugin"
echo "Installed $dest/$NAME.plugin"

if pgrep -xq TouchDesigner; then
  echo "TouchDesigner is running: restart it to load this build."
fi
