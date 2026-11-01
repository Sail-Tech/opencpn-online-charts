#!/usr/bin/env bash
# Quick local macOS build of the plugin library, for testing against an installed OpenCPN.
#
# Compiles against the wxWidgets 3.2 headers of OpenCPN's macOS dependency bundle and links against
# the wxWidgets libraries inside OpenCPN.app, so the plugin uses the same wx copy as OpenCPN.
# The release tarballs are still produced by the CMake build described in README.md.
#
# Usage:
#   WXDEPS=/path/to/macos_deps_universal ci/build-macos-local.sh [--install]
#
# WXDEPS is the unpacked https://dl.cloudsmith.io/public/nohal/opencpn-plugins/raw/files/macos_deps_universal.tar.xz
# ARCHS defaults to the host architecture; use ARCHS="x86_64 arm64" for a universal library.
# --install copies the library and data into ~/Library/Application Support/OpenCPN, keeping a
# backup of the previous library next to it.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OPENCPN_APP="${OPENCPN_APP:-/Applications/OpenCPN.app}"
WXDEPS="${WXDEPS:?Set WXDEPS to the unpacked OpenCPN macOS dependency bundle}"
ARCHS="${ARCHS:-$(uname -m)}"
OUT="${OUT:-$ROOT/build-local}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

WXCONFIG="$WXDEPS/lib/wx/config/osx_cocoa-unicode-3.2"
FRAMEWORKS="$OPENCPN_APP/Contents/Frameworks"
[ -x "$WXCONFIG" ] || { echo "wx-config not found at $WXCONFIG" >&2; exit 1; }
[ -d "$FRAMEWORKS" ] || { echo "OpenCPN not found at $OPENCPN_APP" >&2; exit 1; }

VERSION="$(sed -n 's/^set(PKG_VERSION \([0-9.]*\)).*/\1/p' "$ROOT/Plugin.cmake")"
IFS=. read -r MAJOR MINOR PATCH <<<"$VERSION"
mkdir -p "$OUT/obj"
sed -e "s/@PKG_NAME@/online_charts_pi/" -e "s/@API_VERSION@/1.18/" \
    -e "s/@PROJECT_VERSION_MAJOR@/$MAJOR/" -e "s/@PROJECT_VERSION_MINOR@/$MINOR/" \
    -e "s/@PROJECT_VERSION_PATCH@/$PATCH/" -e "s/@PROJECT_VERSION_TWEAK@/0/" \
    -e "s/@PKG_PRERELEASE@//" -e "s/@pkg_vers_build_info@/local/" \
    -e "s/@PKG_SUMMARY@/Online vector bathymetry and weather for OpenCPN/" \
    -e "s/@PLUGIN_API_NAME@/OnlineCharts/" -e "s/@PKG_DESCRIPTION@/Local build/" \
    "$ROOT/config.h.in" >"$OUT/config.h"

ARCH_FLAGS=()
for arch in $ARCHS; do ARCH_FLAGS+=(-arch "$arch"); done
# shellcheck disable=SC2206
COMMON=("${ARCH_FLAGS[@]}" -mmacosx-version-min=11.0 -O2 -fPIC -fvisibility=hidden ${EXTRA_CFLAGS:-}
        -DNDEBUG -DocpnUSE_GL -DGL_SILENCE_DEPRECATION
        -I"$OUT" -I"$ROOT/src" -I"$ROOT/opencpn-libs/api-18"
        -I"$ROOT/opencpn-libs/plugin_dc/dc_utils/include" -I"$ROOT/libs/civetweb")
# shellcheck disable=SC2207
WXFLAGS=($("$WXCONFIG" --prefix="$WXDEPS" --cxxflags))

SOURCES=("$ROOT"/src/*.cpp
         "$ROOT"/opencpn-libs/plugin_dc/dc_utils/src/{pidc,TexFont,pi_shaders,qtstylesheet}.cpp)

compile() {
  local src="$1" obj
  obj="$OUT/obj/$(basename "${src%.*}").o"
  if [ "$src" -nt "$obj" ] || [ ! -f "$obj" ] || find "$ROOT/src" -name '*.h' -newer "$obj" | grep -q .; then
    case "$src" in
      *.c) clang "${COMMON[@]}" -DUSE_WEBSOCKET -DNO_SSL -DNO_CGI -c "$src" -o "$obj" ;;
      *) clang++ -std=c++17 "${COMMON[@]}" "${WXFLAGS[@]}" -Wall -Wno-deprecated-declarations \
           -Wno-unused-private-field -Wno-unknown-pragmas -c "$src" -o "$obj" ;;
    esac
  fi
}
pids=()
for src in "${SOURCES[@]}" "$ROOT/libs/civetweb/civetweb.c"; do
  compile "$src" &
  pids+=($!)
  if [ "${#pids[@]}" -ge "$JOBS" ]; then wait "${pids[0]}"; pids=("${pids[@]:1}"); fi
done
for pid in "${pids[@]}"; do wait "$pid"; done

WXLIBS=()
for lib in baseu baseu_net baseu_xml osx_cocoau_core osx_cocoau_html osx_cocoau_aui osx_cocoau_gl; do
  WXLIBS+=("$FRAMEWORKS/libwx_$lib-3.2.dylib")
done
clang++ "${ARCH_FLAGS[@]}" -mmacosx-version-min=11.0 -dynamiclib -o "$OUT/libonline_charts_pi.dylib" \
  -install_name @rpath/libonline_charts_pi.dylib "$OUT"/obj/*.o "${WXLIBS[@]}" \
  -framework OpenGL -framework Cocoa -framework IOKit -framework Carbon -framework QuartzCore \
  -framework AudioToolbox -Wl,-undefined,dynamic_lookup
# Refer to OpenCPN's wx libraries by their generic 3.2 names, as release
# builds do, so the plugin also loads with other OpenCPN 5.x patch versions.
for lib in "${WXLIBS[@]}"; do
  actual="$(otool -D "$lib" | tail -1)"
  install_name_tool -change "$actual" "@executable_path/../Frameworks/$(basename "$lib")" \
    "$OUT/libonline_charts_pi.dylib"
done
codesign --force --sign - "$OUT/libonline_charts_pi.dylib" >/dev/null 2>&1 || true
echo "Built $OUT/libonline_charts_pi.dylib ($VERSION, $ARCHS)"

if [ "${1:-}" = "--install" ]; then
  DEST="$HOME/Library/Application Support/OpenCPN/Contents"
  mkdir -p "$DEST/PlugIns" "$DEST/SharedSupport/plugins/online_charts_pi/data"
  if [ -f "$DEST/PlugIns/libonline_charts_pi.dylib" ] && [ ! -f "$DEST/PlugIns/libonline_charts_pi.dylib.backup" ]; then
    cp "$DEST/PlugIns/libonline_charts_pi.dylib" "$DEST/PlugIns/libonline_charts_pi.dylib.backup"
  fi
  if pgrep -x OpenCPN >/dev/null; then
    echo "OpenCPN is running: quit it first. Overwriting a loaded plugin makes macOS kill OpenCPN." >&2
    exit 1
  fi
  # Copy beside the old file and rename over it, so the replacement is atomic.
  cp "$OUT/libonline_charts_pi.dylib" "$DEST/PlugIns/libonline_charts_pi.dylib.new"
  mv -f "$DEST/PlugIns/libonline_charts_pi.dylib.new" "$DEST/PlugIns/libonline_charts_pi.dylib"
  cp "$ROOT"/data/*.svg "$DEST/SharedSupport/plugins/online_charts_pi/data/"
  # The auxiliary browser dashboard served by the plugin.
  rsync -a "$ROOT/data/online_charts/" "$DEST/SharedSupport/plugins/online_charts_pi/data/online_charts/"
  echo "Installed into $DEST/PlugIns (start OpenCPN)"
fi
