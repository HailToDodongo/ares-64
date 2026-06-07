#!/bin/bash
set -e

# Paths and directories we've intentionally deleted.
# Conflicts in these paths are auto-resolved by accepting our deletion.
# Add new entries here when deleting more files from upstream.
DELETED=(
  hiro
  tools/genius
  tools/mame2bml
  scripts/update-arcade-rom-db.sh
  desktop-ui/tools/cheats.cpp
  desktop-ui/tools/graphics.cpp
  desktop-ui/tools/manifest.cpp
  desktop-ui/tools/memory.cpp
  desktop-ui/tools/properties.cpp
  desktop-ui/tools/streams.cpp
  desktop-ui/tools/tape.cpp
  desktop-ui/tools/tools.cpp
  desktop-ui/tools/tracer.cpp
  ruby/video/glx.cpp
  ruby/video/wgl.cpp
  ruby/video/cgl.cpp
  ruby/video/direct3d9.cpp
  ruby/video/metal
  ruby/audio/alsa.cpp
  ruby/audio/pulseaudio.cpp
  ruby/audio/oss.cpp
  ruby/audio/openal.cpp
  ruby/audio/asio.cpp
  ruby/audio/wasapi.cpp
  ruby/audio/directsound.cpp
  ruby/audio/waveout.cpp
  ruby/audio/xaudio2.cpp
  ruby/input/shared
  ruby/input/keyboard
  ruby/input/mouse
  ruby/input/joypad
  ruby/input/xlib.cpp
  ruby/input/udev.cpp
  ruby/input/uhid.cpp
  ruby/input/carbon.cpp
  ruby/input/quartz.cpp
  ruby/input/rawinput.cpp
  ruby/input/directinput.cpp
  ruby/input/iokit.cpp
  ares/a26
  ares/cv
  ares/fc
  ares/gb
  ares/gba
  ares/md
  ares/ms
  ares/msx
  ares/myvision
  ares/ng
  ares/ngp
  ares/pce
  ares/ps1
  ares/saturn
  ares/sfc
  ares/sg
  ares/spec
  ares/ws
  ares/System/ColecoVision
  ares/System/Famicom
  "ares/System/Game Boy"
  "ares/System/Game Boy Advance"
  "ares/System/Game Boy Color"
  "ares/System/Game Boy Player"
  "ares/System/Game Gear"
  "ares/System/Master System"
  "ares/System/Mega Drive"
  ares/System/MSX
  ares/System/MSX2
  "ares/System/Neo Geo Pocket"
  "ares/System/Neo Geo Pocket Color"
  "ares/System/PC Engine"
  "ares/System/PC Engine Duo"
  ares/System/PlayStation
  "ares/System/Pocket Challenge V2"
  ares/System/SC-3000
  ares/System/SG-1000
  "ares/System/Super Famicom"
  ares/System/SuperGrafx
  ares/System/SwanCrystal
  ares/System/WonderSwan
  "ares/System/WonderSwan Color"
  desktop-ui/emulator/atari-2600.cpp
  desktop-ui/emulator/colecovision.cpp
  desktop-ui/emulator/dendy.cpp
  desktop-ui/emulator/famicom.cpp
  desktop-ui/emulator/famicom-disk-system.cpp
  desktop-ui/emulator/game-boy.cpp
  desktop-ui/emulator/game-boy-color.cpp
  desktop-ui/emulator/game-boy-advance.cpp
  desktop-ui/emulator/game-gear.cpp
  desktop-ui/emulator/master-system.cpp
  desktop-ui/emulator/mega-32x.cpp
  desktop-ui/emulator/mega-cd.cpp
  desktop-ui/emulator/mega-cd-32x.cpp
  desktop-ui/emulator/mega-drive.cpp
  desktop-ui/emulator/mega-ld.cpp
  desktop-ui/emulator/msx.cpp
  desktop-ui/emulator/msx2.cpp
  desktop-ui/emulator/myvision.cpp
  desktop-ui/emulator/neo-geo-aes.cpp
  desktop-ui/emulator/neo-geo-mvs.cpp
  desktop-ui/emulator/neo-geo-pocket.cpp
  desktop-ui/emulator/neo-geo-pocket-color.cpp
  desktop-ui/emulator/pc-engine.cpp
  desktop-ui/emulator/pc-engine-cd.cpp
  desktop-ui/emulator/pc-engine-ld.cpp
  desktop-ui/emulator/playstation.cpp
  desktop-ui/emulator/pocket-challenge-v2.cpp
  desktop-ui/emulator/saturn.cpp
  desktop-ui/emulator/sc-3000.cpp
  desktop-ui/emulator/sg-1000.cpp
  desktop-ui/emulator/super-famicom.cpp
  desktop-ui/emulator/supergrafx.cpp
  desktop-ui/emulator/supergrafx-cd.cpp
  desktop-ui/emulator/wonderswan.cpp
  desktop-ui/emulator/wonderswan-color.cpp
  desktop-ui/emulator/zx-spectrum.cpp
  desktop-ui/emulator/zx-spectrum-128.cpp
  desktop-ui/presentation/presentation.cpp
)

REMOTE="${1:-origin}"
BRANCH="${2:-master}"

echo "Fetching $REMOTE..."
git fetch "$REMOTE"

echo "Merging $REMOTE/$BRANCH..."
if git merge "$REMOTE/$BRANCH"; then
  echo "Clean merge, done."
  exit 0
fi

# Directories whose conflicts are auto-resolved by keeping our version.
# Everything under ruby/ is our own SDL3 wrappers,upstream changes are ignored.
OURS=(
  ruby
  desktop-ui/presentation
)

echo "Merge conflicts: auto-resolving known deletions and ours-dirs..."

RESOLVED=()
while IFS= read -r file; do
  # 1) Known deletions: delete the file.
  for dir in "${DELETED[@]}"; do
    if [[ "$file" == "$dir" || "$file" == "$dir"/* ]]; then
      git rm -f "$file" 2>/dev/null && RESOLVED+=("$file") && continue 2
    fi
  done
  # 2) Ours-dirs: keep our version, discard upstream's.  Content conflicts
  #    are resolved with --ours; modify/delete conflicts (we deleted the
  #    file) are resolved by keeping the deletion.
  for dir in "${OURS[@]}"; do
    if [[ "$file" == "$dir" || "$file" == "$dir"/* ]]; then
      if git checkout --ours -- "$file" 2>/dev/null; then
        git add "$file" && RESOLVED+=("$file") && continue 2
      elif git rm -f "$file" 2>/dev/null; then
        RESOLVED+=("$file") && continue 2
      fi
    fi
  done
done < <(git diff --name-only --diff-filter=U)

if [ ${#RESOLVED[@]} -gt 0 ]; then
  echo "Auto-resolved:"
  printf '  %s\n' "${RESOLVED[@]}"
fi

REMAINING=$(git diff --name-only --diff-filter=U)
if [ -n "$REMAINING" ]; then
  echo ""
  echo "Manual resolution needed for:"
  echo "$REMAINING"
  echo ""
  echo "Resolve them, then:  git add -u && git commit --no-edit"
  exit 1
fi

echo "All conflicts resolved, finishing merge..."
git commit --no-edit
echo "Done."
