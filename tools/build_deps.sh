#!/bin/bash
# tools/build_deps.sh [prefix]: the libraries the engine and the runtime link, built from their pinned release sources for
# the macOS the drag-and-drop app supports (MACOSX_DEPLOYMENT_TARGET, default 14.0; Homebrew's bottles target the Mac that
# installed them): SDL3 and sdl2-compat (zlib licence), libogg and libvorbis (BSD). The same SDL as the Homeworld port's.
# Each tarball is kept in ~/ProjectBuilds/_toolkit/src and checked against its pinned SHA-256; the result goes to the prefix
# (default ~/ProjectBuilds/_toolkit/bhl-deps-<target>), with pkg-config files: PKG_CONFIG_LIBDIR=<prefix>/lib/pkgconfig
# makes the engine (./waf --sdl-use-pkgconfig) and tools/build_runtime.py use them. Needs cmake and ninja.
set -euo pipefail
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-14.0}"
P="${1:-$HOME/ProjectBuilds/_toolkit/bhl-deps-$MACOSX_DEPLOYMENT_TARGET}"; SRC="$HOME/ProjectBuilds/_toolkit/src"; mkdir -p "$SRC"
[ -f "$P/.done" ] && { echo "$P"; exit 0; }
UPX_SHA=a7d457be4ef942e46844ee8f301206b111394cbcbde3599747a6904c54ff116b
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
fetch() {   # fetch <name> <url> <sha256 or ->: the tarball, checked; unpacked into $W/<name>
  local f="$SRC/$1.tar.gz"
  [ -f "$f" ] || { curl -fsSL -o "$f.part" "$2" && mv "$f.part" "$f"; }
  local got; got="$(shasum -a 256 "$f" | cut -d' ' -f1)"
  if [ "$3" != - ] && [ "$got" != "$3" ]; then echo "build_deps: $1's tarball isn't the pinned one ($got)"; rm -f "$f"; exit 1; fi
  [ "$3" != - ] || echo "build_deps: $1 sha256 $got (pin it)"
  mkdir -p "$W/$1"; tar -xzf "$f" -C "$W/$1" --strip-components 1
}
CM=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" -DCMAKE_PREFIX_PATH="$P" -DCMAKE_OSX_ARCHITECTURES=arm64
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" -DCMAKE_INSTALL_NAME_DIR="@rpath" -DCMAKE_INSTALL_RPATH="@loader_path")
build() { cmake -S "$W/$1" -B "$W/$1/build" "${CM[@]}" "${@:2}" > "$W/$1.log" 2>&1 && cmake --build "$W/$1/build" >> "$W/$1.log" 2>&1 &&
          cmake --install "$W/$1/build" >> "$W/$1.log" 2>&1 || { tail -30 "$W/$1.log"; exit 1; }; echo "built $1"; }
fetch SDL3 https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz 9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
build SDL3 -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
fetch sdl2-compat https://github.com/libsdl-org/sdl2-compat/releases/download/release-2.32.74/sdl2-compat-2.32.74.tar.gz ec68abde77e2e459c8abc4f9587b8976b5d2a657b2bfda2fb3a079b0f8924588
build sdl2-compat -DSDL2COMPAT_TESTS=OFF
fetch libogg https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.gz 0eb4b4b9420a0f51db142ba3f9c64b333f826532dc0f48c6410ae51f4799b664
build libogg -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=ON -DINSTALL_DOCS=OFF -DBUILD_TESTING=OFF
fetch libvorbis https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.gz 0e982409a9c3fc82ee06e08205b1355e5c6aa4c36bca58146ef399621b0ce5ab
build libvorbis -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=ON
# UPX (GPL-2.0 with its exception; run as a separate tool): setup unpacks the mod's FMOD DLL with it (tools/unpack_audio.py
# checks the result's hash, so the version is the one that hash was taken with)
f="$SRC/upx-5.2.1-src.tar.xz"
[ -f "$f" ] || { curl -fsSL -o "$f.part" https://github.com/upx/upx/releases/download/v5.2.1/upx-5.2.1-src.tar.xz && mv "$f.part" "$f"; }
got="$(shasum -a 256 "$f" | cut -d' ' -f1)"; [ "$UPX_SHA" = - ] && echo "build_deps: upx sha256 $got (pin it)" || [ "$got" = "$UPX_SHA" ] || { echo "build_deps: upx's tarball isn't the pinned one ($got)"; rm -f "$f"; exit 1; }
mkdir -p "$W/upx"; tar -xJf "$f" -C "$W/upx" --strip-components 1
cmake -S "$W/upx" -B "$W/upx/build" "${CM[@]}" -DUPX_CONFIG_DISABLE_GITREV=ON -DUPX_CONFIG_DISABLE_SELF_PACK_TEST=ON > "$W/upx.log" 2>&1 &&
  cmake --build "$W/upx/build" --target upx >> "$W/upx.log" 2>&1 && mkdir -p "$P/bin" && cp "$W/upx/build/upx" "$P/bin/upx" || { tail -30 "$W/upx.log"; exit 1; }
echo "built upx"
ln -sf sdl2-compat.pc "$P/lib/pkgconfig/sdl2.pc"   # (the name the engine's configure and build_runtime.py ask pkg-config for)
touch "$P/.done"; echo "$P"
