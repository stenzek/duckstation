#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
# SPDX-License-Identifier: CC-BY-NC-ND-4.0

set -e

if [ "$#" -lt 4 ]; then
    echo "Syntax: $0 [-system-freetype] [-system-harfbuzz] [-system-libjpeg] [-system-libpng] [-system-libwebp] [-system-libzip] [-system-zlib] [-system-zstd] [-system-qt] [-skip-download] [-skip-cleanup] [-only-download] <host directory> <cross architecture> <cross chroot> <output directory>"
    exit 1
fi

for arg in "$@"; do
	if [ "$arg" == "-system-freetype" ]; then
		echo "Skipping building FreeType."
		SKIP_FREETYPE=true
		shift
	elif [ "$arg" == "-system-harfbuzz" ]; then
		echo "Skipping building HarfBuzz."
		SKIP_HARFBUZZ=true
		shift
	elif [ "$arg" == "-system-libjpeg" ]; then
		echo "Skipping building libjpeg."
		SKIP_LIBJPEG=true
		shift
	elif [ "$arg" == "-system-libpng" ]; then
		echo "Skipping building libpng."
		SKIP_LIBPNG=true
		shift
	elif [ "$arg" == "-system-libwebp" ]; then
		echo "Skipping building libwebp."
		SKIP_LIBWEBP=true
		shift
	elif [ "$arg" == "-system-libzip" ]; then
		echo "Skipping building libzip."
		SKIP_LIBZIP=true
		shift
	elif [ "$arg" == "-system-zlib" ]; then
		echo "Skipping building zlib-ng."
		SKIP_ZLIBNG=true
		shift
	elif [ "$arg" == "-system-zstd" ]; then
		echo "Skipping building zstd."
		SKIP_ZSTD=true
		shift
	elif [ "$arg" == "-system-qt" ]; then
		echo "Skipping building Qt."
		SKIP_QT=true
		shift
	elif [ "$arg" == "-skip-download" ]; then
		echo "Not downloading sources."
		SKIP_DOWNLOAD=true
		shift
	elif [ "$arg" == "-skip-cleanup" ]; then
		echo "Not removing build directory."
		SKIP_CLEANUP=true
		shift
	elif [ "$arg" == "-only-download" ]; then
		echo "Only downloading sources."
		ONLY_DOWNLOAD=true
		shift
	fi
done

SCRIPTDIR=$(realpath $(dirname "${BASH_SOURCE[0]}"))
NPROCS="$(getconf _NPROCESSORS_ONLN)"
HOSTDIR="$1"
if [ "${HOSTDIR:0:1}" != "/" ]; then
	HOSTDIR="$PWD/$HOSTDIR"
fi
CROSSARCH="$2"
SYSROOTDIR="$3"
if [ "${SYSROOTDIR:0:1}" != "/" ]; then
	SYSROOTDIR="$PWD/$SYSROOTDIR"
fi
INSTALLDIR="$4"
if [ "${INSTALLDIR:0:1}" != "/" ]; then
	INSTALLDIR="$PWD/$INSTALLDIR"
fi
TOOLCHAINFILE="$INSTALLDIR/toolchain.cmake"
CMAKE_COMMON=(
	-DCMAKE_BUILD_TYPE=Release
	-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAINFILE"
	-DCMAKE_PREFIX_PATH="$INSTALLDIR"
	-DCMAKE_INSTALL_PREFIX="$INSTALLDIR"
)

# Determine architecture.
if [ "$CROSSARCH" == "arm64" ]; then
	CROSSSYSARCH="aarch64"
	CROSSTRIPLET="aarch64-linux-gnu"
	CMAKEPROCESSOR="aarch64"
elif [ "$CROSSARCH" == "armhf" ]; then
	CROSSSYSARCH="armhf"
	CROSSTRIPLET="arm-linux-gnueabihf"
	CMAKEPROCESSOR="armv7-a"
else
	echo "Unknown cross arch $CROSSARCH"
	exit 1
fi

source "$SCRIPTDIR/versions"

mkdir -p "${INSTALLDIR}"
mkdir -p deps-build
cd deps-build

if [[ "$SKIP_DOWNLOAD" != true && ! -f "libbacktrace-$LIBBACKTRACE.tar.gz" ]]; then
	curl -C - -L \
		-o "libbacktrace-$LIBBACKTRACE.tar.gz" "https://github.com/ianlancetaylor/libbacktrace/archive/$LIBBACKTRACE.tar.gz" \
		-O "https://github.com/libsdl-org/SDL/releases/download/release-$SDL3/SDL3-$SDL3.tar.gz" \
		-o "cpuinfo-$CPUINFO.tar.gz" "https://github.com/stenzek/cpuinfo/archive/$CPUINFO.tar.gz" \
		-o "discord-rpc-$DISCORD_RPC.tar.gz" "https://github.com/stenzek/discord-rpc/archive/$DISCORD_RPC.tar.gz" \
		-o "plutosvg-$PLUTOSVG.tar.gz" "https://github.com/stenzek/plutosvg/archive/$PLUTOSVG.tar.gz" \
		-o "shaderc-$SHADERC.tar.gz" "https://github.com/stenzek/shaderc/archive/$SHADERC.tar.gz" \
		-o "soundtouch-$SOUNDTOUCH.tar.gz" "https://github.com/stenzek/soundtouch/archive/$SOUNDTOUCH.tar.gz"
fi

cat > SHASUMS <<EOF
$LIBBACKTRACE_GZ_HASH  libbacktrace-$LIBBACKTRACE.tar.gz
$SDL3_GZ_HASH  SDL3-$SDL3.tar.gz
$CPUINFO_GZ_HASH  cpuinfo-$CPUINFO.tar.gz
$DISCORD_RPC_GZ_HASH  discord-rpc-$DISCORD_RPC.tar.gz
$PLUTOSVG_GZ_HASH  plutosvg-$PLUTOSVG.tar.gz
$SHADERC_GZ_HASH  shaderc-$SHADERC.tar.gz
$SOUNDTOUCH_GZ_HASH  soundtouch-$SOUNDTOUCH.tar.gz
EOF

if [ "$SKIP_FREETYPE" != true ]; then
	if [[ "$SKIP_DOWNLOAD" != true && ! -f "freetype-$FREETYPE.tar.gz" ]]; then
		curl -C - -L -o "freetype-$FREETYPE.tar.gz" "https://sourceforge.net/projects/freetype/files/freetype2/$FREETYPE/freetype-$FREETYPE.tar.gz/download"
	fi
	cat >> SHASUMS <<EOF
$FREETYPE_GZ_HASH  freetype-$FREETYPE.tar.gz
EOF
fi
if [ "$SKIP_HARFBUZZ" != true ]; then
	if [[ "$SKIP_DOWNLOAD" != true && ! -f "harfbuzz-$HARFBUZZ.tar.gz" ]]; then
		curl -C - -L -o "harfbuzz-$HARFBUZZ.tar.gz" "https://github.com/harfbuzz/harfbuzz/archive/refs/tags/$HARFBUZZ.tar.gz"
	fi
	cat >> SHASUMS <<EOF
$HARFBUZZ_GZ_HASH  harfbuzz-$HARFBUZZ.tar.gz
EOF
fi
if [ "$SKIP_LIBJPEG" != true ]; then
	if [[ "$SKIP_DOWNLOAD" != true && ! -f "libjpeg-turbo-$LIBJPEGTURBO.tar.gz" ]]; then
		curl -C - -L -O "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$LIBJPEGTURBO/libjpeg-turbo-$LIBJPEGTURBO.tar.gz"
	fi
	cat >> SHASUMS <<EOF
$LIBJPEGTURBO_GZ_HASH  libjpeg-turbo-$LIBJPEGTURBO.tar.gz
EOF
fi
if [ "$SKIP_LIBPNG" != true ]; then
	if [[ "$SKIP_DOWNLOAD" != true && ! -f "libpng-$LIBPNG.tar.gz" ]]; then
		curl -C - -L -O "https://downloads.sourceforge.net/project/libpng/libpng16/$LIBPNG/libpng-$LIBPNG.tar.gz"
	fi
	cat >> SHASUMS <<EOF
$LIBPNG_GZ_HASH  libpng-$LIBPNG.tar.gz
EOF
fi
if [ "$SKIP_LIBWEBP" != true ]; then
	if [[ "$SKIP_DOWNLOAD" != true && ! -f "libwebp-$LIBWEBP.tar.gz" ]]; then
		curl -C - -L -O "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$LIBWEBP.tar.gz"
	fi
	cat >> SHASUMS <<EOF
$LIBWEBP_GZ_HASH  libwebp-$LIBWEBP.tar.gz
EOF
fi
if [ "$SKIP_LIBZIP" != true ]; then
	if [[ "$SKIP_DOWNLOAD" != true && ! -f "libzip-$LIBZIP.tar.gz" ]]; then
		curl -C - -L -O "https://github.com/nih-at/libzip/releases/download/v$LIBZIP/libzip-$LIBZIP.tar.gz"
	fi
	cat >> SHASUMS <<EOF
$LIBZIP_GZ_HASH  libzip-$LIBZIP.tar.gz
EOF
fi
if [ "$SKIP_ZLIBNG" != true ]; then
	if [[ "$SKIP_DOWNLOAD" != true && ! -f "zlib-ng-$ZLIBNG.tar.gz" ]]; then
		curl -C - -L -o "zlib-ng-$ZLIBNG.tar.gz" "https://github.com/zlib-ng/zlib-ng/archive/refs/tags/$ZLIBNG.tar.gz"
	fi
	cat >> SHASUMS <<EOF
$ZLIBNG_GZ_HASH  zlib-ng-$ZLIBNG.tar.gz
EOF
fi
if [ "$SKIP_ZSTD" != true ]; then
	if [[ "$SKIP_DOWNLOAD" != true && ! -f "zstd-$ZSTD.tar.gz" ]]; then
		curl -C - -L -O "https://github.com/facebook/zstd/releases/download/v$ZSTD/zstd-$ZSTD.tar.gz"
	fi
	cat >> SHASUMS <<EOF
$ZSTD_GZ_HASH  zstd-$ZSTD.tar.gz
EOF
fi
if [ "$SKIP_QT" != true ]; then
	if [[ "$SKIP_DOWNLOAD" != true && ! -f "qtbase-everywhere-src-$QT.tar.xz" ]]; then
		curl -C - -L \
			-O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qtbase-everywhere-src-$QT.tar.xz" \
			-O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qtimageformats-everywhere-src-$QT.tar.xz" \
			-O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qtsvg-everywhere-src-$QT.tar.xz" \
			-O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qttools-everywhere-src-$QT.tar.xz" \
			-O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qttranslations-everywhere-src-$QT.tar.xz" \
			-O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qtwayland-everywhere-src-$QT.tar.xz" 
	fi
	cat >> SHASUMS <<EOF
$QTBASE_XZ_HASH  qtbase-everywhere-src-$QT.tar.xz
$QTIMAGEFORMATS_XZ_HASH  qtimageformats-everywhere-src-$QT.tar.xz
$QTSVG_XZ_HASH  qtsvg-everywhere-src-$QT.tar.xz
$QTTOOLS_XZ_HASH  qttools-everywhere-src-$QT.tar.xz
$QTTRANSLATIONS_XZ_HASH  qttranslations-everywhere-src-$QT.tar.xz
$QTWAYLAND_XZ_HASH  qtwayland-everywhere-src-$QT.tar.xz
EOF
fi

shasum -a 256 --check SHASUMS

# Have to clone with git, because it does version detection.
if [[ "$SKIP_DOWNLOAD" != true && ! -d "SPIRV-Cross" ]]; then
	git clone https://github.com/KhronosGroup/SPIRV-Cross/ -b $SPIRV_CROSS --depth 1
	if [ "$(git --git-dir=SPIRV-Cross/.git rev-parse HEAD)" != "$SPIRV_CROSS_SHA" ]; then
		echo "SPIRV-Cross version mismatch, expected $SPIRV_CROSS_SHA, got $(git rev-parse HEAD)"
		exit 1
	fi
fi

# Only downloading sources?
if [ "$ONLY_DOWNLOAD" == true ]; then
	exit 0
fi

# Stop pkg-config picking up host files.
export PKG_CONFIG_PATH=${SYSROOTDIR}/usr/lib/${CROSSTRIPLET}/pkgconfig:${SYSROOTDIR}/usr/lib/pkgconfig:${SYSROOTDIR}/usr/share/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=${SYSROOTDIR}

# Generate cmake toolchain file.
cat > "$TOOLCHAINFILE" << EOF
set(CMAKE_CROSSCOMPILING TRUE)
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR ${CMAKEPROCESSOR})

set(CMAKE_C_COMPILER "/usr/bin/${CROSSTRIPLET}-gcc")
set(CMAKE_C_COMPILER_TARGET "${CROSSTRIPLET}")
set(CMAKE_C_COMPILER_AR "/usr/bin/${CROSSTRIPLET}-ar")
set(CMAKE_C_COMPILER_RANLIB "/usr/bin/${CROSSTRIPLET}-ranlib")

set(CMAKE_CXX_COMPILER "/usr/bin/${CROSSTRIPLET}-g++")
set(CMAKE_CXX_COMPILER_TARGET "${CROSSTRIPLET}")
set(CMAKE_CXX_COMPILER_AR "/usr/bin/${CROSSTRIPLET}-ar")
set(CMAKE_CXX_COMPILER_RANLIB "/usr/bin/${CROSSTRIPLET}-ranlib")

set(CMAKE_FIND_ROOT_PATH "${INSTALLDIR};${SYSROOTDIR}")
set(CMAKE_SYSROOT "${SYSROOTDIR}")

set(CMAKE_PKG_CONFIG_PC_PATH "${PKG_CONFIG_PATH}")
set(CMAKE_PKG_CONFIG_SYSROOT_DIR "${PKG_CONFIG_SYSROOT_DIR}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF

# Build zlib first because of the things that depend on it.
# Disabled because it currently causes crashes on armhf.
#if [ "$SKIP_ZLIBNG" != true ]; then
#	echo "Building zlib-ng..."
#	rm -fr "zlib-ng-$ZLIBNG"
#	tar xf "zlib-ng-$ZLIBNG.tar.gz"
#	cd "zlib-ng-$ZLIBNG"
#	cmake "${CMAKE_COMMON[@]}" -DBUILD_SHARED_LIBS=ON -DZLIB_COMPAT=ON -DZLIBNG_ENABLE_TESTS=OFF -DZLIB_ENABLE_TESTS=OFF -DWITH_GTEST=OFF -B build -G Ninja
#	cmake --build build --parallel
#	ninja -C build install
#	cd ..
#fi

# NOTE: Must be a shared library because otherwise aarch64 libgcc symbols are missing when building with clang.
echo "Building libbacktrace..."
rm -fr "libbacktrace-$LIBBACKTRACE"
tar xf "libbacktrace-$LIBBACKTRACE.tar.gz"
cd "libbacktrace-$LIBBACKTRACE"
./configure --prefix="$INSTALLDIR" --build=x86_64-linux-gnu --host="${CROSSTRIPLET}" --with-pic --enable-shared --disable-static
make
make install
cd ..

if [ "$SKIP_LIBPNG" != true ]; then
	echo "Building libpng..."
	rm -fr "libpng-$LIBPNG"
	tar xf "libpng-$LIBPNG.tar.gz"
	cd "libpng-$LIBPNG"
	cmake "${CMAKE_COMMON[@]}" -DBUILD_SHARED_LIBS=ON -DPNG_TESTS=OFF -DPNG_STATIC=OFF -DPNG_SHARED=ON -DPNG_TOOLS=OFF -B build -G Ninja
	cmake --build build --parallel
	ninja -C build install
	cd ..
fi

if [ "$SKIP_LIBJPEG" != true ]; then
	echo "Building libjpeg..."
	rm -fr "libjpeg-turbo-$LIBJPEGTURBO"
	tar xf "libjpeg-turbo-$LIBJPEGTURBO.tar.gz"
	cd "libjpeg-turbo-$LIBJPEGTURBO"
	cmake "${CMAKE_COMMON[@]}" -DENABLE_STATIC=OFF -DENABLE_SHARED=ON -B build -G Ninja
	cmake --build build --parallel
	ninja -C build install
	cd ..
fi

if [ "$SKIP_ZSTD" != true ]; then
	echo "Building Zstandard..."
	rm -fr "zstd-$ZSTD"
	tar xf "zstd-$ZSTD.tar.gz"
	cd "zstd-$ZSTD"
	cmake "${CMAKE_COMMON[@]}" -DBUILD_SHARED_LIBS=ON -DZSTD_BUILD_SHARED=ON -DZSTD_BUILD_STATIC=OFF -DZSTD_BUILD_PROGRAMS=OFF -B build -G Ninja build/cmake
	cmake --build build --parallel
	ninja -C build install
	cd ..
fi

if [ "$SKIP_LIBWEBP" != true ]; then
	echo "Building WebP..."
	rm -fr "libwebp-$LIBWEBP"
	tar xf "libwebp-$LIBWEBP.tar.gz"
	cd "libwebp-$LIBWEBP"
	cmake "${CMAKE_COMMON[@]}" -B build -G Ninja \
		-DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF \
		-DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF -DBUILD_SHARED_LIBS=ON
	cmake --build build --parallel
	ninja -C build install
	cd ..
fi

if [ "$SKIP_LIBZIP" != true ]; then
	echo "Building libzip..."
	rm -fr "libzip-$LIBZIP"
	tar xf "libzip-$LIBZIP.tar.gz"
	cd "libzip-$LIBZIP"
	cmake "${CMAKE_COMMON[@]}" -B build -G Ninja \
		-DENABLE_COMMONCRYPTO=OFF -DENABLE_GNUTLS=OFF -DENABLE_MBEDTLS=OFF -DENABLE_OPENSSL=OFF -DENABLE_WINDOWS_CRYPTO=OFF \
		-DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=ON -DBUILD_SHARED_LIBS=ON -DLIBZIP_DO_INSTALL=ON \
		-DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_OSSFUZZ=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF
	cmake --build build --parallel
	ninja -C build install
	cd ..
fi

if [ "$SKIP_FREETYPE" != true ]; then
	if [ "$SKIP_HARFBUZZ" != true ]; then
		echo "Building FreeType without HarfBuzz..."
		rm -fr "freetype-$FREETYPE"
		tar xf "freetype-$FREETYPE.tar.gz"
		cd "freetype-$FREETYPE"
		cmake "${CMAKE_COMMON[@]}" -DBUILD_SHARED_LIBS=ON -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON -DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_BROTLI=TRUE -DFT_DISABLE_HARFBUZZ=TRUE -B build -G Ninja
		cmake --build build --parallel
		ninja -C build install
		cd ..

		echo "Building HarfBuzz..."
		rm -fr "harfbuzz-$HARFBUZZ"
		tar xf "harfbuzz-$HARFBUZZ.tar.gz"
		cd "harfbuzz-$HARFBUZZ"
		cmake "${CMAKE_COMMON[@]}" -DBUILD_SHARED_LIBS=ON -DHB_BUILD_UTILS=OFF -B build -G Ninja
		cmake --build build --parallel
		ninja -C build install
		cd ..
	fi

	echo "Building FreeType with HarfBuzz..."
	rm -fr "freetype-$FREETYPE"
	tar xf "freetype-$FREETYPE.tar.gz"
	cd "freetype-$FREETYPE"
	cmake "${CMAKE_COMMON[@]}" -DBUILD_SHARED_LIBS=ON -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON -DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_BROTLI=TRUE -DFT_REQUIRE_HARFBUZZ=TRUE -B build -G Ninja
	cmake --build build --parallel
	ninja -C build install
	cd ..
fi

echo "Building SDL..."
rm -fr "SDL3-$SDL3"
tar xf "SDL3-$SDL3.tar.gz"
cd "SDL3-$SDL3"
cmake -B build "${CMAKE_COMMON[@]}" -DBUILD_SHARED_LIBS=ON -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

if [ "$SKIP_QT" != true ]; then
	# Couple notes:
	# -fontconfig is needed otherwise Qt Widgets render only boxes.
	# -qt-doubleconversion avoids a dependency on libdouble-conversion.
	# ICU avoids pulling in a bunch of large libraries, and hopefully we can get away without it.
	# OpenGL is needed to render window decorations in Wayland, apparently.
	# dbus-runtime and linked off to avoid a relocation error (different to host.. probably should change that).
	echo "Building Qt Base..."
	rm -fr "qtbase-everywhere-src-$QT"
	tar xf "qtbase-everywhere-src-$QT.tar.xz"
	cd "qtbase-everywhere-src-$QT"
	mkdir build
	cd build
	../configure -prefix "$INSTALLDIR" -extprefix "$INSTALLDIR" -qt-host-path "$HOSTDIR" -release -dbus runtime -fontconfig -qt-doubleconversion -ssl -openssl-runtime -opengl desktop -qpa xcb,wayland -xkbcommon -xcb -gtk -- -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAINFILE" -DQT_GENERATE_SBOM=OFF -DFEATURE_cups=OFF -DFEATURE_dbus=ON -DFEATURE_dbus_linked=OFF -DFEATURE_icu=OFF -DFEATURE_sql=OFF -DFEATURE_system_png=ON -DFEATURE_system_jpeg=ON -DFEATURE_system_zlib=ON -DFEATURE_system_freetype=ON -DFEATURE_system_harfbuzz=ON
	cmake --build . --parallel
	ninja install
	cd ../../

	echo "Building Qt SVG..."
	rm -fr "qtsvg-everywhere-src-$QT"
	tar xf "qtsvg-everywhere-src-$QT.tar.xz"
	cd "qtsvg-everywhere-src-$QT"
	mkdir build
	cd build
	"$INSTALLDIR/bin/qt-configure-module" .. -- -DCMAKE_PREFIX_PATH="$INSTALLDIR"
	cmake --build . --parallel
	ninja install
	cd ../../

	echo "Building Qt Image Formats..."
	rm -fr "qtimageformats-everywhere-src-$QT"
	tar xf "qtimageformats-everywhere-src-$QT.tar.xz"
	cd "qtimageformats-everywhere-src-$QT"
	mkdir build
	cd build
	"$INSTALLDIR/bin/qt-configure-module" .. -- -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DFEATURE_system_webp=ON
	cmake --build . --parallel
	ninja install
	cd ../../

	echo "Building Qt Wayland..."
	rm -fr "qtwayland-everywhere-src-$QT"
	tar xf "qtwayland-everywhere-src-$QT.tar.xz"
	cd "qtwayland-everywhere-src-$QT"
	mkdir build
	cd build
	"$INSTALLDIR/bin/qt-configure-module" .. -- -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DFEATURE_wayland_server=OFF
	cmake --build . --parallel
	ninja install
	cd ../../

	echo "Installing Qt Tools..."
	rm -fr "qttools-everywhere-src-$QT"
	tar xf "qttools-everywhere-src-$QT.tar.xz"
	cd "qttools-everywhere-src-$QT"

	# Force disable clang scanning, it gets very confused.
	patch -u configure.cmake <<EOF
--- configure.cmake
+++ configure.cmake
@@ -3,11 +3,11 @@
 
 #### Tests
 
-qt_find_package(WrapLibClang 8 PROVIDED_TARGETS WrapLibClang::WrapLibClang)
+#qt_find_package(WrapLibClang 8 PROVIDED_TARGETS WrapLibClang::WrapLibClang)
 
-if(TARGET WrapLibClang::WrapLibClang)
-    set(TEST_libclang "ON" CACHE BOOL "Required libclang version found." FORCE)
-endif()
+#if(TARGET WrapLibClang::WrapLibClang)
+#    set(TEST_libclang "ON" CACHE BOOL "Required libclang version found." FORCE)
+#endif()
 
 
 

EOF

	mkdir build
	cd build
	"$INSTALLDIR/bin/qt-configure-module" .. -- -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DFEATURE_assistant=OFF -DFEATURE_clang=OFF -DFEATURE_designer=ON -DFEATURE_kmap2qmap=OFF -DFEATURE_pixeltool=OFF -DFEATURE_pkg_config=OFF -DFEATURE_qev=OFF -DFEATURE_qtattributionsscanner=OFF -DFEATURE_qtdiag=OFF -DFEATURE_qtplugininfo=OFF
	cmake --build . --parallel
	ninja install
	cd ../../

	echo "Installing Qt Translations..."
	rm -fr "qttranslations-everywhere-src-$QT"
	tar xf "qttranslations-everywhere-src-$QT.tar.xz"
	cd "qttranslations-everywhere-src-$QT"
	mkdir build
	cd build
	"$INSTALLDIR/bin/qt-configure-module" .. -- -DCMAKE_PREFIX_PATH="$INSTALLDIR"
	cmake --build . --parallel
	ninja install
	cd ../../
fi

echo "Building shaderc..."
rm -fr "shaderc-$SHADERC"
tar xf "shaderc-$SHADERC.tar.gz"
cd "shaderc-$SHADERC"
cmake "${CMAKE_COMMON[@]}" -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -B build -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building SPIRV-Cross..."
cd SPIRV-Cross
rm -fr build
cmake "${CMAKE_COMMON[@]}" -DSPIRV_CROSS_SHARED=ON -DSPIRV_CROSS_STATIC=OFF -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_ENABLE_GLSL=ON -DSPIRV_CROSS_ENABLE_HLSL=OFF -DSPIRV_CROSS_ENABLE_MSL=OFF -DSPIRV_CROSS_ENABLE_CPP=OFF -DSPIRV_CROSS_ENABLE_REFLECT=OFF -DSPIRV_CROSS_ENABLE_C_API=ON -DSPIRV_CROSS_ENABLE_UTIL=ON -B build -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building cpuinfo..."
rm -fr "cpuinfo-$CPUINFO"
tar xf "cpuinfo-$CPUINFO.tar.gz"
cd "cpuinfo-$CPUINFO"
cmake "${CMAKE_COMMON[@]}" -DCPUINFO_LIBRARY_TYPE=shared -DCPUINFO_RUNTIME_TYPE=shared -DCPUINFO_LOG_LEVEL=error -DCPUINFO_LOG_TO_STDIO=ON -DCPUINFO_BUILD_TOOLS=OFF -DCPUINFO_BUILD_UNIT_TESTS=OFF -DCPUINFO_BUILD_MOCK_TESTS=OFF -DCPUINFO_BUILD_BENCHMARKS=OFF -DUSE_SYSTEM_LIBS=ON -B build -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building discord-rpc..."
rm -fr "discord-rpc-$DISCORD_RPC"
tar xf "discord-rpc-$DISCORD_RPC.tar.gz"
cd "discord-rpc-$DISCORD_RPC"
cmake "${CMAKE_COMMON[@]}" -DBUILD_SHARED_LIBS=ON -B build -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building plutosvg..."
rm -fr "plutosvg-$PLUTOSVG"
tar xf "plutosvg-$PLUTOSVG.tar.gz"
cd "plutosvg-$PLUTOSVG"
cmake "${CMAKE_COMMON[@]}" -DBUILD_SHARED_LIBS=ON -DPLUTOSVG_ENABLE_FREETYPE=ON -DPLUTOSVG_BUILD_EXAMPLES=OFF -B build -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building soundtouch..."
rm -fr "soundtouch-$SOUNDTOUCH"
tar xf "soundtouch-$SOUNDTOUCH.tar.gz"
cd "soundtouch-$SOUNDTOUCH"
cmake "${CMAKE_COMMON[@]}" -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON -B build -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

if [ "$SKIP_CLEANUP" != true ]; then
	echo "Cleaning up..."
	cd ..
	rm -fr deps-build
fi
