#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
# SPDX-License-Identifier: CC-BY-NC-ND-4.0

SCRIPTDIR=$(dirname "${BASH_SOURCE[0]}")

function retry_command {
  # Package servers tend to be unreliable at times..
  # Retry a bunch of times.
  local RETRIES=10

  for i in $(seq 1 "$RETRIES"); do
    "$@" && break
    if [ "$i" == "$RETRIES" ]; then
      echo "Command \"$@\" failed after ${RETRIES} retries."
      exit 1
    fi
  done
}

if [ "$#" -ne 3 ]; then
    echo "Syntax: $0 <path to duckstation directory> <path to build directory> <output name>"
    exit 1
fi

ROOTDIR=$1
BUILDDIR=$2
ASSETNAME=$3

BINARY=duckstation-qt
APPDIRNAME=DuckStation.AppDir
STRIP=strip

declare -a MANUAL_LIBS=(
	"libz.so.1"
	"libavcodec.so.62"
	"libavformat.so.62"
	"libavutil.so.60"
	"libswscale.so.9"
	"libswresample.so.6"
	"libdiscord-rpc.so"
	"libharfbuzz.so"
	"libfreetype.so.6"
	"libshaderc_shared.so"
	"libspirv-cross-c-shared.so.0"
)

set -e

DEPSDIR=$(realpath "$SCRIPTDIR/../../dep/prebuilt/linux-x64")
LINUXDEPLOY=./linuxdeploy-x86_64
LINUXDEPLOY_PLUGIN_QT=./linuxdeploy-plugin-qt-x86_64
APPIMAGETOOL=./appimagetool-x86_64
APPIMAGERUNTIME=./runtime-x86_64
PATCHELF=patchelf

if [ ! -f "$LINUXDEPLOY" ]; then
	retry_command wget -O "$LINUXDEPLOY" https://github.com/duckstation/dependencies/releases/download/appimage-tools/linuxdeploy-x86_64.AppImage
	chmod +x "$LINUXDEPLOY"
fi

if [ ! -f "$LINUXDEPLOY_PLUGIN_QT" ]; then
	retry_command wget -O "$LINUXDEPLOY_PLUGIN_QT" https://github.com/duckstation/dependencies/releases/download/appimage-tools/linuxdeploy-plugin-qt-x86_64.AppImage
	chmod +x "$LINUXDEPLOY_PLUGIN_QT"
fi

if [ ! -f "$APPIMAGETOOL" ]; then
	retry_command wget -O "$APPIMAGETOOL" https://github.com/duckstation/dependencies/releases/download/appimage-tools/appimagetool-x86_64.AppImage
	chmod +x "$APPIMAGETOOL"
fi

if [ ! -f "$APPIMAGERUNTIME" ]; then
	retry_command wget -O "$APPIMAGERUNTIME" https://github.com/stenzek/type2-runtime/releases/download/continuous/runtime-x86_64
fi

OUTDIR=$(realpath "./$APPDIRNAME")
rm -fr "$OUTDIR"

echo "Locating extra libraries..."
EXTRA_LIBS_ARGS=()
for lib in "${MANUAL_LIBS[@]}"; do
	srcpath=$(find "$DEPSDIR" -name "$lib")
	if [ ! -f "$srcpath" ]; then
		echo "Missinge extra library $lib. Exiting."
		exit 1
	fi

	echo "Found $lib at $srcpath."
	EXTRA_LIBS_ARGS+=("--library=$srcpath")
done

# Why the nastyness? linuxdeploy strips our main binary, and there's no option to turn it off.
# It also doesn't strip the Qt libs. We can't strip them after running linuxdeploy, because
# patchelf corrupts the libraries (but they still work), but patchelf+strip makes them crash
# on load. So, make a backup copy, strip the original (since that's where linuxdeploy finds
# the libs to copy), then swap them back after we're done.
# Isn't Linux packaging amazing?

rm -fr "$DEPSDIR.bak"
cp -a "$DEPSDIR" "$DEPSDIR.bak"
IFS="
"
for i in $(find "$DEPSDIR" -iname '*.so'); do
  echo "Stripping deps library ${i}"
  strip "$i"
done

echo "Running linuxdeploy to create AppDir..."
EXTRA_QT_MODULES="core;gui;svg;widgets;xcbqpa;waylandcompositor" \
EXTRA_PLATFORM_PLUGINS="libqwayland.so" \
DEPLOY_PLATFORM_THEMES="1" \
LINUXDEPLOY_EXCLUDED_LIBRARIES="libwayland-cursor*;libwayland-egl*" \
QMAKE="$DEPSDIR/bin/qmake" \
NO_STRIP="1" \
$LINUXDEPLOY --plugin qt --appdir="$OUTDIR" --executable="$BUILDDIR/bin/duckstation-qt" ${EXTRA_LIBS_ARGS[@]} \
--desktop-file="$ROOTDIR/scripts/appimage/org.duckstation.DuckStation.desktop" \
--icon-file="$ROOTDIR/scripts/appimage/org.duckstation.DuckStation.png" \

echo "Copying resources into AppDir..."
cp -a "$BUILDDIR/bin/resources" "$OUTDIR/usr/bin"

# Restore unstripped deps (for cache).
rm -fr "$DEPSDIR"
mv "$DEPSDIR.bak" "$DEPSDIR"

# Fix up translations.
rm -fr "$OUTDIR/usr/bin/translations"
mv "$OUTDIR/usr/translations" "$OUTDIR/usr/bin"
cp -a "$BUILDDIR/bin/translations" "$OUTDIR/usr/bin"

# Generate AppStream meta-info.
echo "Generating AppStream metainfo..."
mkdir -p "$OUTDIR/usr/share/metainfo"
"$SCRIPTDIR/generate-metainfo.sh" "$OUTDIR/usr/share/metainfo"

echo "Generating AppImage..."
rm -f "$ASSETNAME"
"$APPIMAGETOOL" -v --runtime-file "$APPIMAGERUNTIME" "$OUTDIR" "$ASSETNAME"
