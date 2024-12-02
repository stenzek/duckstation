#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
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

if [ "$#" -ne 4 ]; then
    echo "Syntax: $0 <path to duckstation directory> <path to build directory> <deps prefix> <output name>"
    exit 1
fi

ROOTDIR=$1
BUILDDIR=$2
DEPSDIR=$3
NAME=$4

BINARY=duckstation-qt
APPDIRNAME=DuckStation.AppDir
STRIP=strip

declare -a MANUAL_LIBS=(
	"libavcodec.so.61"
	"libavformat.so.61"
	"libavutil.so.59"
	"libswscale.so.8"
	"libswresample.so.5"
	"libdiscord-rpc.so"
	"libfreetype.so.6"
	"libshaderc_ds.so"
	"libspirv-cross-c-shared.so.0"
)

declare -a MANUAL_QT_LIBS=(
	"libQt6WaylandEglClientHwIntegration.so.6"
)

declare -a MANUAL_QT_PLUGINS=(
	"wayland-decoration-client"
	"wayland-graphics-integration-client"
	"wayland-shell-integration"
)

declare -a REMOVE_LIBS=(
	'libwayland-client.so*'
	'libwayland-cursor.so*'
	'libwayland-egl.so*'
)

set -e

LINUXDEPLOY=./linuxdeploy-x86_64
LINUXDEPLOY_PLUGIN_QT=./linuxdeploy-plugin-qt-x86_64
APPIMAGETOOL=./appimagetool-x86_64
APPIMAGERUNTIME=./runtime-x86_64
PATCHELF=patchelf

if [ ! -f "$LINUXDEPLOY" ]; then
	retry_command wget -O "$LINUXDEPLOY" https://github.com/stenzek/duckstation-ext-qt-minimal/releases/download/linux/linuxdeploy-x86_64.AppImage
	chmod +x "$LINUXDEPLOY"
fi

if [ ! -f "$LINUXDEPLOY_PLUGIN_QT" ]; then
	retry_command wget -O "$LINUXDEPLOY_PLUGIN_QT" https://github.com/stenzek/duckstation-ext-qt-minimal/releases/download/linux/linuxdeploy-plugin-qt-x86_64.AppImage
	chmod +x "$LINUXDEPLOY_PLUGIN_QT"
fi

if [ ! -f "$APPIMAGETOOL" ]; then
	retry_command wget -O "$APPIMAGETOOL" https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage
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
EXTRA_QT_PLUGINS="core;gui;svg;waylandclient;widgets;xcbqpa" \
EXTRA_PLATFORM_PLUGINS="libqwayland-egl.so;libqwayland-generic.so" \
DEPLOY_PLATFORM_THEMES="1" \
QMAKE="$DEPSDIR/bin/qmake" \
NO_STRIP="1" \
$LINUXDEPLOY --plugin qt --appdir="$OUTDIR" --executable="$BUILDDIR/bin/duckstation-qt" ${EXTRA_LIBS_ARGS[@]} \
--desktop-file="$ROOTDIR/scripts/packaging/org.duckstation.DuckStation.desktop" \
--icon-file="$ROOTDIR/scripts/packaging/org.duckstation.DuckStation.png" \

echo "Copying resources into AppDir..."
cp -a "$BUILDDIR/bin/resources" "$OUTDIR/usr/bin"

# LinuxDeploy's Qt plugin doesn't include Wayland support. So manually copy in the additional Wayland libraries.
echo "Copying Qt Wayland libraries..."
for lib in "${MANUAL_QT_LIBS[@]}"; do
	srcpath="$DEPSDIR/lib/$lib"
	dstpath="$OUTDIR/usr/lib/$lib"
	echo "  $srcpath -> $dstpath"
	cp "$srcpath" "$dstpath"
	$PATCHELF --set-rpath '$ORIGIN' "$dstpath"
done

# .. and plugins.
echo "Copying Qt Wayland plugins..."
for GROUP in "${MANUAL_QT_PLUGINS[@]}"; do
	srcpath="$DEPSDIR/plugins/$GROUP"
	dstpath="$OUTDIR/usr/plugins/$GROUP"
	echo "  $srcpath -> $dstpath"
	mkdir -p "$dstpath"

	for srcsopath in $(find "$DEPSDIR/plugins/$GROUP" -iname '*.so'); do
		# This is ../../ because it's usually plugins/group/name.so
		soname=$(basename "$srcsopath")
		dstsopath="$dstpath/$soname"
		echo "    $srcsopath -> $dstsopath"
		cp "$srcsopath" "$dstsopath"
		$PATCHELF --set-rpath '$ORIGIN/../../lib:$ORIGIN' "$dstsopath"
	done
done

# Why do we have to manually remove these libs? Because the linuxdeploy Qt plugin
# copies them, not the "main" linuxdeploy binary, and plugins don't inherit the
# include list...
for lib in "${REMOVE_LIBS[@]}"; do
	for libpath in $(find "$OUTDIR/usr/lib" -name "$lib"); do
		echo "    Removing problematic library ${libpath}."
		rm -f "$libpath"
	done
done

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
"$SCRIPTDIR/../generate-metainfo.sh" "$OUTDIR/usr/share/metainfo"

# Copy in AppRun hooks.
echo "Copying AppRun hooks..."
mkdir -p "$OUTDIR/apprun-hooks"
for hookpath in "$SCRIPTDIR/apprun-hooks"/*; do
	hookname=$(basename "$hookpath")
	cp -v "$hookpath" "$OUTDIR/apprun-hooks/$hookname"
	sed -i -e 's/exec /source "$this_dir"\/apprun-hooks\/"'"$hookname"'"\nexec /' "$OUTDIR/AppRun"
done

echo "Generating AppImage..."
rm -f "$NAME.AppImage"
"$APPIMAGETOOL" -v --runtime-file "$APPIMAGERUNTIME" "$OUTDIR" "$NAME.AppImage"
