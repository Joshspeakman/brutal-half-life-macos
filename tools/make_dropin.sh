#!/bin/bash
# tools/make_dropin.sh <Half-Life folder with bhl> <version tag>: the drag-and-drop app ("Brutal Half-Life.app", disk image
# and zip in ~/ProjectBuilds/brutal-half-life-macos/release) for players without Xcode tools, after the other ports'
# recomp setup (setup/: the same files as diablo-2-macos recomp/setup).
#
# The app holds the launch window (with the setup window linked in), the Xash3D engine, the bridges, ANGLE, SDL and the
# Vorbis libraries, and the split runtime (libbhl.dylib, tools/build_runtime.py --split), all built here for
# MACOSX_DEPLOYMENT_TARGET (14.0) from pinned sources; the bundled clang, ld64.lld and Python, UPX, pefile's wheel and this
# project's tools, which set the game up on the player's Mac (tools/dropin_build.py): the mod's modules translated into
# libgame.dylib with their images, and the data folder. No game code, images or data are in the app (check_release.py).
# The Half-Life folder given here (bhl/ beside valve/, as on a PC) is read only, to check the app against.
set -euo pipefail
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-14.0}"
cd "$(dirname "$0")/.."; cd "$(pwd -P)"; ROOT="$PWD"; S="$ROOT/setup"   # (the real path: the tools record paths relative to it)
GAME="$(cd "${1:?usage: tools/make_dropin.sh <Half-Life folder with bhl> <version tag>}" && pwd)"; TAG="${2:?usage: tools/make_dropin.sh <folder> <version tag>}"
NAME="Brutal Half-Life"; BIN=BrutalHalfLife; SLUG=Brutal-Half-Life; REPO=Joshspeakman/brutal-half-life-macos
REL="$HOME/ProjectBuilds/brutal-half-life-macos/release"; mkdir -p "$REL"
PY="$ROOT/.venv/bin/python3"; [ -x "$PY" ] || PY=python3
TK="$HOME/ProjectBuilds/_toolkit/toolkit.tar.xz"; [ -f "$TK" ] || "$S/make_toolkit.sh" "$TK"
DEPS="$(tools/build_deps.sh | tail -1)"; export BHL_DEPS="$DEPS" PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig"
echo "== the engine for macOS $MACOSX_DEPLOYMENT_TARGET"
BHL_ENGINE_OUT=build-dropin "$PY" tools/fetch_dependencies.py --build-client-engine > "$REL/engine.log" 2>&1 || { tail -20 "$REL/engine.log"; exit 1; }
echo "== the split runtime and the bridges"
N="$ROOT/work/native-dropin"; B="$ROOT/work/bridge-dropin"; mkdir -p "$N" "$B"
"$PY" tools/build_runtime.py hl.dll client.dll fmod.dll vgui.dll --split --out "$N" > "$REL/runtime.log" 2>&1 || { tail -20 "$REL/runtime.log"; exit 1; }
BHL_NATIVE_DIR="$N" BHL_BRIDGE_DIR="$B" "$PY" tools/build_bridge.py > "$REL/bridge.log" 2>&1 || { tail -20 "$REL/bridge.log"; exit 1; }
BHL_NATIVE_DIR="$N" BHL_BRIDGE_DIR="$B" "$PY" tools/build_client_bridge.py >> "$REL/bridge.log" 2>&1 || { tail -20 "$REL/bridge.log"; exit 1; }
echo "== the app"
A="$REL/$NAME.app"; rm -rf "${A:?}"; mkdir -p "$A/Contents/MacOS" "$A/Contents/Resources/Runtime" "$A/Contents/Resources/setup" "$A/Contents/Resources/bin"
"$PY" -c 'import sys; sys.path.insert(0, "tools"); from pathlib import Path; import package_app as p
deps = Path(sys.argv[4]) / "lib"
extra = {n: deps / n for n in ("libSDL2-2.0.0.dylib", "libSDL3.0.dylib", "libogg.0.dylib", "libvorbis.0.4.9.dylib", "libvorbisfile.3.3.8.dylib")}
p.bundle_runtime(Path(sys.argv[1]), native=Path(sys.argv[2]), bridge=Path(sys.argv[3]), external={"libgame.dylib"}, extra=extra,
                 engine=Path("work/upstream/xash3d-fwgs/build-dropin").resolve())' \
  "$A/Contents/Resources/Runtime" "$N" "$B" "$DEPS" > /dev/null
for lib in libogg.0.dylib libvorbis.0.4.9.dylib libvorbisfile.3.3.8.dylib; do   # (the engine links these by name)
  [ -e "$A/Contents/Resources/Runtime/$lib" ] || { cp "$DEPS/lib/$lib" "$A/Contents/Resources/Runtime/"; codesign --force -s - "$A/Contents/Resources/Runtime/$lib" 2>/dev/null; }
done
cp work/upstream/xash3d-fwgs/build-dropin/3rdparty/extras/extras.pk3 "$A/Contents/Resources/extras.pk3"
cp "$DEPS/bin/upx" "$A/Contents/Resources/bin/upx"
O="$ROOT/build/dropin"; rm -rf "${O:?}"; mkdir -p "$O"
clang -c -arch arm64 -O2 -fobjc-arc -w -I "$S" -o "$O/setup_ui.o" "$S/setup_ui.m"
swiftc -O -D DROPIN -target "arm64-apple-macos$MACOSX_DEPLOYMENT_TARGET" -framework AppKit -framework CoreImage launcher/*.swift "$O/setup_ui.o" -o "$A/Contents/MacOS/$BIN"
sed -e "s|@REPO@|$REPO|; s|@ASSET@|$SLUG.zip|" "$S/update.sh" > "$A/Contents/MacOS/update"; chmod +x "$A/Contents/MacOS/update"
/usr/libexec/PlistBuddy -c "Add :branch string release" "$A/Contents/Resources/Update.plist" > /dev/null
printf '{\n  "revision": "%s",\n  "architecture": "arm64",\n  "renderer": "Metal (ANGLE)",\n  "drag_and_drop": true\n}\n' "$TAG" > "$A/Contents/Resources/Build.json"
cp NOTICE.txt "$A/Contents/Resources/NOTICE.txt"; mkdir -p "$A/Contents/Resources/Licenses"
for n in LICENSE LICENSES.chromium.html manifest.json; do cp "work/angle/$n" "$A/Contents/Resources/Licenses/ANGLE-$n"; done
echo "== setup files"
R="$A/Contents/Resources/setup"; mkdir -p "$R/tools" "$R/wheels" "$R/bhl/tools" "$R/bhl/config" "$R/bhl/runtime"
cp "$S/setup.py" "$R/"; cp -R "$S/include" "$R/include"; cp "$TK" "$R/toolkit.tar.xz"
cp tools/dropin_build.py tools/bhl_art.py "$R/tools/"
cp tools/*.py "$R/bhl/tools/"; cp config/*.json "$R/bhl/config/"; cp -R vendor "$R/bhl/vendor"; cp runtime/*.h runtime/*.inc "$R/bhl/runtime/"
"$PY" -m pip download -q --disable-pip-version-check --only-binary=:all: --no-deps pefile==2024.8.26 -d "$R/wheels"
CODE="$(cat tools/*.py config/*.json runtime/*.h runtime/*.inc "$S"/include/* $(find vendor -type f -name '*.py' | sort) | shasum -a 256 | cut -d' ' -f1)"
ZIP_SHA="$("$PY" -c 'import json; print(json.load(open("config/modules.json"))["archive_sha256"])')"
python3 - "$R/game.json" "$CODE" "$TAG" "$ZIP_SHA" <<'PY'
import json, sys
mods = json.load(open("config/modules.json"))["modules"]
c = {"name": "Brutal Half-Life", "folder": "Brutal Half-Life", "version": "v3", "title": "BRUTAL HALF-LIFE", "style": "iron",
     "where": "Your Half-Life folder (Steam) with Brutal Half-Life v3 extracted into it (its bhl folder beside valve), or bhl_v3.zip",
     "minutes": 15, "space_gb": 4, "build_gb": 2, "exe": "bhl/dlls/hl.dll",
     "exe_sha256": {mods["hl.dll"]["sha256"]: "v3"},
     "required": ["bhl/cl_dlls/client.dll", "bhl/cl_dlls/fmod.dll", "bhl/maps/start.bsp"],
     "libs": [{"file": "bhl/cl_dlls/client.dll"}, {"file": "bhl/cl_dlls/fmod.dll"}],
     "package": {"name": "bhl_v3.zip", "sha256": sys.argv[4], "kind": "zip", "into": "bhl", "payload": "", "version": "Brutal Half-Life v3's zip"},
     "addons": [{"name": "Half-Life", "into": "valve", "look": ["~/Library/Application Support/Steam/steamapps/common/Half-Life/valve"]}],
     "pipeline": ["dropin_build.py", "--game", "{game}", "--out", "{out}", "--work", "{work}", "--setup", "{setup}", "--llvm", "{llvm}", "--macos", "{macos}"],
     "art": ["bhl_art.py", "{game}/bhl", "{art}"], "log": "Setup", "bound_symbol": "", "code": sys.argv[2], "release": sys.argv[3]}
json.dump(c, open(sys.argv[1], "w"), indent=1)
PY
echo "== icon and bundle"
I="$(mktemp -d)"; "$A/Contents/MacOS/$BIN" --write-iconset "$I/i.iconset" >/dev/null 2>&1 && iconutil -c icns "$I/i.iconset" -o "$A/Contents/Resources/BrutalHalfLife.icns" || true
clang -fobjc-arc -framework AppKit -framework CoreImage "$S/make_art.m" -I "$S" -o "$I/mk"
"$I/mk" dmg iron "BRUTAL HALF-LIFE" "$REL/bg.png" 1; "$I/mk" dmg iron "BRUTAL HALF-LIFE" "$REL/bg@2x.png" 2; tiffutil -cathidpicheck "$REL/bg.png" "$REL/bg@2x.png" -out "$REL/background.tiff" 2>/dev/null; rm -f "${REL:?}/bg.png" "${REL:?}/bg@2x.png"; rm -rf "${I:?}"
python3 - "$A/Contents/Info.plist" "$TAG" "$MACOSX_DEPLOYMENT_TARGET" <<'PY'
import plistlib, sys
info = dict(CFBundleExecutable='BrutalHalfLife', CFBundleIdentifier='io.github.brutal-half-life-macos', CFBundleName='Brutal Half-Life',
            CFBundleDisplayName='Brutal Half-Life', CFBundlePackageType='APPL', CFBundleShortVersionString=sys.argv[2].lstrip('v'),
            CFBundleVersion=sys.argv[2].lstrip('v'), LSMinimumSystemVersion=sys.argv[3], NSHighResolutionCapable=True,
            LSApplicationCategoryType='public.app-category.games', GCSupportsGameMode=True, LSSupportsGameMode=True,
            NSPrincipalClass='NSApplication', CFBundleIconFile='BrutalHalfLife',
            CFBundleDocumentTypes=[dict(CFBundleTypeName='Game Folder', CFBundleTypeRole='Viewer', LSHandlerRank='Alternate',
                                        LSItemContentTypes=['public.folder', 'public.zip-archive'])])
plistlib.dump(info, open(sys.argv[1], 'wb'))
PY
python3 "$S/check_release.py" "$A" "$GAME" "$R/game.json"
codesign --force --deep -s - "$A"; codesign -v "$A"
echo "== packaging"
"$PY" -c 'import dmgbuild' 2>/dev/null || "$PY" -m pip install -q dmgbuild
D="$(mktemp -d)"; sed "s/@NAME@/$NAME/g" "$S/ReadMe.txt" > "$D/Read Me First.txt"
rm -f "${REL:?}/$SLUG.dmg" "${REL:?}/$SLUG.zip"
cat > "$D/settings.py" <<PY
files = ["$A", "$D/Read Me First.txt"]; symlinks = {"Applications": "/Applications"}; format = "UDZO"; filesystem = "HFS+"
badge_icon = None; icon = "$A/Contents/Resources/BrutalHalfLife.icns"; background = "$REL/background.tiff"; show_status_bar = False; show_tab_view = False; show_toolbar = False; show_pathbar = False; show_sidebar = False
window_rect = ((200, 120), (660, 420)); default_view = "icon-view"; icon_size = 112; text_size = 13
icon_locations = {"$NAME.app": (170, 230), "Applications": (490, 230), "Read Me First.txt": (330, 372)}
PY
"$(dirname "$PY")/dmgbuild" -s "$D/settings.py" "$NAME" "$REL/$SLUG.dmg" 2>&1 | tail -1; rm -rf "${D:?}"
(cd "$REL"; ditto -c -k --keepParent "$NAME.app" "$SLUG.zip")
du -sh "$A" "$REL/$SLUG.dmg" | sed "s|$REL/||"
