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

if [ "$1" == "-inject-libc" ]; then
	echo "Injecting libc/libstdc++"
	INJECT_LIBC=true
	shift
fi

if [ "$#" -ne 5 ]; then
    echo "Syntax: $0 [-inject-libc] <duckstation-qt|duckstation-mini> <target arch> <path to build directory> <deps prefix> <chroot dir>"
    exit 1
fi

BINARY=$1
ARCH=$2
BUILDDIR=$3
DEPSDIR=$4
CHROOTDIR=$5

STRIP=llvm-strip

if [ "$ARCH" == "arm64" ]; then
	DEBARCH="arm64"
	RUNTIMEARCH="aarch64"
	TRIPLE="aarch64-linux-gnu"
elif [ "$ARCH" == "armhf" ]; then
	DEBARCH="armhf"
	RUNTIMEARCH="armhf"
	TRIPLE="arm-linux-gnueabihf"
else
	echo "Unknown arch ${ARCH}."
	exit 1
fi

if [ "$BINARY" == "duckstation-qt" ]; then
	echo "Building Qt AppImage."
	ADD_QT_LIBS=true
	APPIMAGENAME="DuckStation-${ARCH}"
elif [ "$BINARY" == "duckstation-mini" ]; then
	echo "Building Mini AppImage."
	ADD_QT_LIBS=false
	APPIMAGENAME="DuckStation-Mini-${ARCH}"
else
	echo "Unknown binary $BINARY"
	exit 1
fi

APPDIRNAME="${APPIMAGENAME}.AppDir"

declare -a SYSLIBS=(
	"libbrotlicommon.so.1"
	"libbrotlidec.so.1"
	"libbsd.so.0"
	"libcom_err.so.2"
	"libcrypto.so.3"
	"libcurl.so.4"
	"libffi.so.8"
	"libgnutls.so.30"
	"libgssapi_krb5.so.2"
	"libhogweed.so.6"
	"libidn2.so.0"
	"libk5crypto.so.3"
	"libkeyutils.so.1"
	"libkrb5.so.3"
	"libkrb5support.so.0"
	"liblber-2.5.so.0"
	"libldap-2.5.so.0"
	"libnettle.so.8"
	"libnghttp2.so.14"
	"libp11-kit.so.0"
	"libpsl.so.5"
	"librtmp.so.1"
	"libsasl2.so.2"
	"libssh.so.4"
	"libssl.so.3"
	"libtasn1.so.6"
	"libudev.so.1"
	"libunistring.so.2"
)

declare -a DEPLIBS=(
	"libbacktrace.so.0"
	"libfreetype.so.6"
	"libharfbuzz.so"
	"libjpeg.so.62"
	"libpng16.so.16"
	"libSDL3.so.0"
	"libsharpyuv.so.0"
	"libwebpdemux.so.2"
	"libwebpmux.so.3"
	"libwebp.so.7"
	"libzip.so.5"
	"libzstd.so.1"

	"libcpuinfo.so"
	"libdiscord-rpc.so"
	"libplutosvg.so.0"
	"libshaderc_ds.so"
	"libsoundtouch.so.2"
	"libspirv-cross-c-shared.so.0"

	#"libavcodec.so.61"
	#"libavformat.so.61"
	#"libavutil.so.59"
	#"libswscale.so.8"
	#"libswresample.so.5"
	#"libva-drm.so.2"
	#"libva.so.2"
)

if [ "${ADD_QT_LIBS}" == true ]; then
	SYSLIBS+=(
		"libatk-1.0.so.0"
		"libatk-bridge-2.0.so.0"
		"libatspi.so.0"
		"libblkid.so.1"
		"libcairo-gobject.so.2"
		"libcairo.so.2"
		"libcap.so.2"
		"libdatrie.so.1"
		"libdeflate.so.0"
		"libdbus-1.so.3"
		"libepoxy.so.0"
		"libgcrypt.so.20"
		"libgdk-3.so.0"
		"libgdk_pixbuf-2.0.so.0"
		"libgio-2.0.so.0"
		"libglib-2.0.so.0"
		"libgmodule-2.0.so.0"
		"libgobject-2.0.so.0"
		"libgraphite2.so.3"
		"libgtk-3.so.0"
		"libjbig.so.0"
		"liblz4.so.1"
		"liblzma.so.5"
		"libmd.so.0"
		"libmount.so.1"
		"libpango-1.0.so.0"
		"libpangocairo-1.0.so.0"
		"libpangoft2-1.0.so.0"
		"libpcre2-16.so.0"
		"libpcre2-8.so.0"
		"libpcre.so.3"
		"libpixman-1.so.0"
		"libselinux.so.1"
		"libsystemd.so.0"
		"libtiff.so.5"
		"libXau.so.6"
		"libxcb-cursor.so.0"
		"libxcb-glx.so.0"
		"libxcb-icccm.so.4"
		"libxcb-image.so.0"
		"libxcb-keysyms.so.1"
		"libxcb-randr.so.0"
		"libxcb-render.so.0"
		"libxcb-render-util.so.0"
		"libxcb-shape.so.0"
		"libxcb-shm.so.0"
		"libxcb-sync.so.1"
		"libxcb-util.so.1"
		"libxcb-xfixes.so.0"
		"libxcb-xkb.so.1"
		"libXcomposite.so.1"
		"libXcursor.so.1"
		"libXdamage.so.1"
		"libXdmcp.so.6"
		"libXext.so.6"
		"libXfixes.so.3"
		"libXinerama.so.1"
		"libXi.so.6"
		"libxkbcommon.so.0"
		"libxkbcommon-x11.so.0"
		"libXrandr.so.2"
		"libXrender.so.1"
	)

	declare -a QTLIBS=(
		"libQt6Core.so.6"
		"libQt6DBus.so.6"
		"libQt6Gui.so.6"
		"libQt6OpenGL.so.6"
		"libQt6Svg.so.6"
		"libQt6WaylandClient.so.6"
		"libQt6WaylandEglClientHwIntegration.so.6"
		"libQt6Widgets.so.6"
		"libQt6XcbQpa.so.6"
	)

	declare -a QTPLUGINS=(
		"plugins/iconengines"
		"plugins/imageformats"
		"plugins/platforminputcontexts"
		"plugins/platforms"
		"plugins/platformthemes"
		"plugins/wayland-decoration-client"
		"plugins/wayland-graphics-integration-client"
		"plugins/wayland-shell-integration"
		"plugins/xcbglintegrations"
	)
fi

set -e
IFS="
"

APPIMAGETOOL=./appimagetool-x86_64
APPIMAGERUNTIME=./runtime-${RUNTIMEARCH}
PATCHELF=patchelf

if [ ! -f "$APPIMAGETOOL" ]; then
	retry_command wget -O "$APPIMAGETOOL" https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage
	chmod +x "$APPIMAGETOOL"
fi

if [ ! -f "$APPIMAGERUNTIME" ]; then
	retry_command wget -O "$APPIMAGERUNTIME" https://github.com/stenzek/type2-runtime/releases/download/continuous/runtime-${RUNTIMEARCH}
fi

OUTDIR=$(realpath "./$APPDIRNAME")
rm -fr "$OUTDIR"
mkdir "$OUTDIR"
mkdir -p "$OUTDIR/usr/bin" "$OUTDIR/usr/lib"

echo "Copying binary and resources..."
cp -a "$BUILDDIR/bin/$BINARY" "$BUILDDIR/bin/resources" "$BUILDDIR/bin/translations" "$OUTDIR/usr/bin"

# Patch RPATH so the binary goes hunting for shared libraries in the AppDir instead of system.
echo "Patching RPATH in ${BINARY}..."
patchelf --set-rpath '$ORIGIN/../lib' "$OUTDIR/usr/bin/$BINARY"

# Libraries we pull in from the system.
echo "Copying system libraries..."
for lib in "${SYSLIBS[@]}"; do
	blib=$(basename "$lib")
	echo "$CHROOTDIR/lib/$TRIPLE/$lib"
	if [ -f "$CHROOTDIR/lib/$TRIPLE/$lib" ]; then
		cp "$CHROOTDIR/lib/$TRIPLE/$lib" "$OUTDIR/usr/lib/$blib"
	elif [ -f "$CHROOTDIR/usr/lib/$TRIPLE/$lib" ]; then
		cp "$CHROOTDIR/usr/lib/$TRIPLE/$lib" "$OUTDIR/usr/lib/$blib"
	elif [ -f "$CHROOTDIR/lib/$lib" ]; then
		cp "$CHROOTDIR/lib/$lib" "$OUTDIR/usr/lib/$blib"
	elif [ -f "$CHROOTDIR/usr/lib/$lib" ]; then
		cp "$CHROOTDIR/usr/lib/$lib" "$OUTDIR/usr/lib/$blib"
	else
		echo "*** Failed to find '$blib'"
		exit 1
	fi

	"$STRIP" $OUTDIR/usr/lib/$blib
done

echo "Copying local libraries..."
for lib in "${DEPLIBS[@]}"; do
	blib=$(basename "$lib")
	echo "$DEPSDIR/lib/$lib"
	if [ -f "$DEPSDIR/lib/$lib" ]; then
		cp "$DEPSDIR/lib/$lib" "$OUTDIR/usr/lib/$blib"
	else
		echo "*** Failed to find '$blib'"
		exit 1
	fi

	"$STRIP" "$OUTDIR/usr/lib/$blib"
done

if [ "${ADD_QT_LIBS}" == true ]; then
	echo "Copying Qt libraries..."
	for lib in "${QTLIBS[@]}"; do
		cp -avL "$DEPSDIR/lib/$lib" "$OUTDIR/usr/lib"
		"$STRIP" "$OUTDIR/usr/lib/$lib"
	done

	echo "Copying Qt plugins..."
	mkdir -p $OUTDIR/usr/lib/plugins
	for plugin in "${QTPLUGINS[@]}"; do
		mkdir -p "$OUTDIR/usr/lib/$plugin"
		cp -avL "$DEPSDIR/$plugin/"*.so "$OUTDIR/usr/lib/$plugin/"
	done

	for so in $(find $OUTDIR/usr/lib/plugins -iname '*.so'); do
		# This is ../../ because it's usually plugins/group/name.so
		echo "Patching RPATH in ${so}..."
		patchelf --set-rpath '$ORIGIN/../..' "$so"
		"$STRIP" "$so"
	done

	echo "Creating qt.conf..."
cat > "$OUTDIR/usr/bin/qt.conf" << EOF
[Paths]
Plugins = ../lib/plugins
EOF

fi

for so in $(find $OUTDIR/usr/lib -maxdepth 1); do
	if [ -f "$so" ]; then
		echo "Patching RPATH in ${so}"
		patchelf --set-rpath '$ORIGIN' "$so"
	fi
done

# Copy desktop/icon
echo "Copying desktop/icon..."
mkdir -p "$OUTDIR/usr/share/applications"
mkdir -p "$OUTDIR/usr/share/icons/hicolor/512x512/apps"
cp -v "$SCRIPTDIR/../org.duckstation.DuckStation.desktop" "$OUTDIR/usr/share/applications"
cp -v "$SCRIPTDIR/../org.duckstation.DuckStation.png" "$OUTDIR/usr/share/icons/hicolor/512x512/apps"
ln -s "usr/share/applications/org.duckstation.DuckStation.desktop" "$OUTDIR"
ln -s "usr/share/icons/hicolor/512x512/apps/org.duckstation.DuckStation.png" "$OUTDIR"

# Generate AppStream meta-info.
echo "Generating AppStream metainfo..."
mkdir -p "$OUTDIR/usr/share/metainfo"
"$SCRIPTDIR/../generate-metainfo.sh" "$OUTDIR/usr/share/metainfo"

# Copy AppRun
cp "$SCRIPTDIR/apprun-cross.sh" "$OUTDIR/AppRun"
chmod +x "$OUTDIR/AppRun"
ln -s "usr/bin/$BINARY" "$OUTDIR/AppRun.wrapped"

# Optionally inject libc
if [ "$INJECT_LIBC" == true ]; then
	echo "Injecting libc/libc++..."
	"$SCRIPTDIR/inject-libc.sh" "$OUTDIR" "$DEBARCH" "$TRIPLE" "$BINARY"
fi

echo "Generating AppImage..."
rm -f "$APPIMAGENAME.AppImage"

# Can't run the appimage in a docker container because no fuse, so extract it first.
"$APPIMAGETOOL" --appimage-extract
"$PWD/squashfs-root/AppRun" -v --runtime-file "$APPIMAGERUNTIME" "$OUTDIR" "$APPIMAGENAME.AppImage"
