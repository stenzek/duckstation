#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
# SPDX-License-Identifier: CC-BY-NC-ND-4.0

set -e

if [ "$#" -lt 1 ]; then
    echo "Syntax: $0 [-system-freetype] [-system-harfbuzz] [-system-libjpeg] [-system-libpng] [-system-libwebp] [-system-libzip] [-system-zlib] [-system-zstd] [-system-qt] [-skip-download] [-skip-cleanup] [-only-download] <output directory>"
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
INSTALLDIR="$1"
if [ "${INSTALLDIR:0:1}" != "/" ]; then
	INSTALLDIR="$PWD/$INSTALLDIR"
fi

FREETYPE=2.13.3
HARFBUZZ=11.3.2
LIBBACKTRACE=793921876c981ce49759114d7bb89bb89b2d3a2d
LIBJPEGTURBO=3.1.1
LIBPNG=1.6.50
LIBWEBP=1.6.0
LIBZIP=1.11.4
SDL3=3.2.18
QT=6.9.1
ZLIBNG=2.2.4
ZSTD=1.5.7

CPUINFO=3ebbfd45645650c4940bf0f3b4d25ab913466bb0
DISCORD_RPC=cc59d26d1d628fbd6527aac0ac1d6301f4978b92
PLUTOSVG=bc845bb6b6511e392f9e1097b26f70cf0b3c33be
SHADERC=4daf9d466ad00897f755163dd26f528d14e1db44
SOUNDTOUCH=463ade388f3a51da078dc9ed062bf28e4ba29da7
SPIRV_CROSS=vulkan-sdk-1.4.321.0
SPIRV_CROSS_SHA=d8e3e2b141b8c8a167b2e3984736a6baacff316c

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
858b1225351052234f7e71f0f36622fc9ad33aac947db7816d0b443ae0dd33ce  libbacktrace-$LIBBACKTRACE.tar.gz
1a775bde924397a8e0c08bfda198926c17be859d0288ad0dec1dea1b2ee04f8f  SDL3-$SDL3.tar.gz
b60832071919220d2fe692151fb420fa9ea489aa4c7a2eb0e01c830cbe469858  cpuinfo-$CPUINFO.tar.gz
297cd48a287a9113eec44902574084c6ab3b6a8b28d02606765a7fded431d7d8  discord-rpc-$DISCORD_RPC.tar.gz
cc8eed38daf68aaaaa96e904f68f5524c02f10b5d42062b91cdc93f93445f68a  plutosvg-$PLUTOSVG.tar.gz
167109d52b65f6eedd66103971b869a71632fe27a63efc2ba5b0e5a1912a094c  shaderc-$SHADERC.tar.gz
fe45c2af99f6102d2704277d392c1c83b55180a70bfd17fb888cc84a54b70573  soundtouch-$SOUNDTOUCH.tar.gz
EOF

if [ "$SKIP_FREETYPE" != true ]; then
	if [[ "$SKIP_DOWNLOAD" != true && ! -f "freetype-$FREETYPE.tar.xz" ]]; then
		curl -C - -L -o "freetype-$FREETYPE.tar.xz" "https://sourceforge.net/projects/freetype/files/freetype2/$FREETYPE/freetype-$FREETYPE.tar.xz/download"
	fi
	cat >> SHASUMS <<EOF
0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289  freetype-$FREETYPE.tar.xz
EOF
fi
if [ "$SKIP_HARFBUZZ" != true ]; then
	if [[ "$SKIP_DOWNLOAD" != true && ! -f "harfbuzz-$HARFBUZZ.tar.gz" ]]; then
		curl -C - -L -o "harfbuzz-$HARFBUZZ.tar.gz" "https://github.com/harfbuzz/harfbuzz/archive/refs/tags/$HARFBUZZ.tar.gz"
	fi
	cat >> SHASUMS <<EOF
b6120ebc56238474f4030b2fbcfd235912b6adaf1477c088f4a399a942dd0ab0  harfbuzz-$HARFBUZZ.tar.gz
EOF
fi
if [ "$SKIP_LIBJPEG" != true ]; then
	if [[ "$SKIP_DOWNLOAD" != true && ! -f "libjpeg-turbo-$LIBJPEGTURBO.tar.gz" ]]; then
		curl -C - -L -O "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$LIBJPEGTURBO/libjpeg-turbo-$LIBJPEGTURBO.tar.gz"
	fi
	cat >> SHASUMS <<EOF
aadc97ea91f6ef078b0ae3a62bba69e008d9a7db19b34e4ac973b19b71b4217c  libjpeg-turbo-$LIBJPEGTURBO.tar.gz
EOF
fi
if [ "$SKIP_LIBPNG" != true ]; then
	if [[ "$SKIP_DOWNLOAD" != true && ! -f "libpng-$LIBPNG.tar.xz" ]]; then
		curl -C - -L -O "https://downloads.sourceforge.net/project/libpng/libpng16/$LIBPNG/libpng-$LIBPNG.tar.xz"
	fi
	cat >> SHASUMS <<EOF
4df396518620a7aa3651443e87d1b2862e4e88cad135a8b93423e01706232307  libpng-$LIBPNG.tar.xz
EOF
fi
if [ "$SKIP_LIBWEBP" != true ]; then
	if [[ "$SKIP_DOWNLOAD" != true && ! -f "libwebp-$LIBWEBP.tar.gz" ]]; then
		curl -C - -L -O "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$LIBWEBP.tar.gz"
	fi
	cat >> SHASUMS <<EOF
e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564  libwebp-$LIBWEBP.tar.gz
EOF
fi
if [ "$SKIP_LIBZIP" != true ]; then
	if [[ "$SKIP_DOWNLOAD" != true && ! -f "libzip-$LIBZIP.tar.xz" ]]; then
		curl -C - -L -O "https://github.com/nih-at/libzip/releases/download/v$LIBZIP/libzip-$LIBZIP.tar.xz"
	fi
	cat >> SHASUMS <<EOF
8a247f57d1e3e6f6d11413b12a6f28a9d388de110adc0ec608d893180ed7097b  libzip-$LIBZIP.tar.xz
EOF
fi
if [ "$SKIP_ZLIBNG" != true ]; then
	if [[ "$SKIP_DOWNLOAD" != true && ! -f "zlib-ng-$ZLIBNG.tar.gz" ]]; then
		curl -C - -L -o "zlib-ng-$ZLIBNG.tar.gz" "https://github.com/zlib-ng/zlib-ng/archive/refs/tags/$ZLIBNG.tar.gz"
	fi
	cat >> SHASUMS <<EOF
a73343c3093e5cdc50d9377997c3815b878fd110bf6511c2c7759f2afb90f5a3  zlib-ng-$ZLIBNG.tar.gz
EOF
fi
if [ "$SKIP_ZSTD" != true ]; then
	if [[ "$SKIP_DOWNLOAD" != true && ! -f "zstd-$ZSTD.tar.gz" ]]; then
		curl -C - -L -O "https://github.com/facebook/zstd/releases/download/v$ZSTD/zstd-$ZSTD.tar.gz"
	fi
	cat >> SHASUMS <<EOF
eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3  zstd-$ZSTD.tar.gz
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
40caedbf83cc9a1959610830563565889878bc95f115868bbf545d1914acf28e  qtbase-everywhere-src-$QT.tar.xz
ebe9f238daaf9bb752c7233edadf4af33fc4fa30d914936812b6410d3af1577c  qtimageformats-everywhere-src-$QT.tar.xz
2dfc5de5fd891ff2afd9861e519bf1a26e6deb729b3133f68a28ba763c9abbd5  qtsvg-everywhere-src-$QT.tar.xz
90c4a562f4ccfd043fd99f34c600853e0b5ba9babc6ec616c0f306f2ce3f4b4c  qttools-everywhere-src-$QT.tar.xz
9761a1a555f447cdeba79fdec6a705dee8a7882ac10c12e85f49467ddd01a741  qttranslations-everywhere-src-$QT.tar.xz
7d21ea0e687180ebb19b9a1f86ae9cfa7a25b4f02d5db05ec834164409932e3e  qtwayland-everywhere-src-$QT.tar.xz
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

# Build zlib first because of the things that depend on it.
if [ "$SKIP_ZLIBNG" != true ]; then
	echo "Building zlib-ng..."
	rm -fr "zlib-ng-$ZLIBNG"
	tar xf "zlib-ng-$ZLIBNG.tar.gz"
	cd "zlib-ng-$ZLIBNG"
	cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DBUILD_SHARED_LIBS=ON -DZLIB_COMPAT=ON -DZLIBNG_ENABLE_TESTS=OFF -DZLIB_ENABLE_TESTS=OFF -DWITH_GTEST=OFF -B build -G Ninja
	cmake --build build --parallel
	ninja -C build install
	cd ..
fi

echo "Building libbacktrace..."
rm -fr "libbacktrace-$LIBBACKTRACE"
tar xf "libbacktrace-$LIBBACKTRACE.tar.gz"
cd "libbacktrace-$LIBBACKTRACE"
./configure --prefix="$INSTALLDIR" --with-pic
make
make install
cd ..

if [ "$SKIP_LIBPNG" != true ]; then
	echo "Building libpng..."
	rm -fr "libpng-$LIBPNG"
	tar xf "libpng-$LIBPNG.tar.xz"
	cd "libpng-$LIBPNG"
	cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DBUILD_SHARED_LIBS=ON -DPNG_TESTS=OFF -DPNG_STATIC=OFF -DPNG_SHARED=ON -DPNG_TOOLS=OFF -B build -G Ninja
	cmake --build build --parallel
	ninja -C build install
	cd ..
fi

if [ "$SKIP_LIBJPEG" != true ]; then
	echo "Building libjpeg..."
	rm -fr "libjpeg-turbo-$LIBJPEGTURBO"
	tar xf "libjpeg-turbo-$LIBJPEGTURBO.tar.gz"
	cd "libjpeg-turbo-$LIBJPEGTURBO"
	cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DENABLE_STATIC=OFF -DENABLE_SHARED=ON -B build -G Ninja
	cmake --build build --parallel
	ninja -C build install
	cd ..
fi

if [ "$SKIP_ZSTD" != true ]; then
	echo "Building Zstandard..."
	rm -fr "zstd-$ZSTD"
	tar xf "zstd-$ZSTD.tar.gz"
	cd "zstd-$ZSTD"
	cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DBUILD_SHARED_LIBS=ON -DZSTD_BUILD_SHARED=ON -DZSTD_BUILD_STATIC=OFF -DZSTD_BUILD_PROGRAMS=OFF -B build -G Ninja build/cmake
	cmake --build build --parallel
	ninja -C build install
	cd ..
fi

if [ "$SKIP_LIBWEBP" != true ]; then
	echo "Building WebP..."
	rm -fr "libwebp-$LIBWEBP"
	tar xf "libwebp-$LIBWEBP.tar.gz"
	cd "libwebp-$LIBWEBP"
	cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -B build -G Ninja \
		-DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF \
		-DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF -DBUILD_SHARED_LIBS=ON
	cmake --build build --parallel
	ninja -C build install
	cd ..
fi

if [ "$SKIP_LIBZIP" != true ]; then
	echo "Building libzip..."
	rm -fr "libzip-$LIBZIP"
	tar xf "libzip-$LIBZIP.tar.xz"
	cd "libzip-$LIBZIP"
	cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -B build -G Ninja \
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
		tar xf "freetype-$FREETYPE.tar.xz"
		cd "freetype-$FREETYPE"
		cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DBUILD_SHARED_LIBS=ON -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON -DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_BROTLI=TRUE -DFT_DISABLE_HARFBUZZ=TRUE -B build -G Ninja
		cmake --build build --parallel
		ninja -C build install
		cd ..

		echo "Building HarfBuzz..."
		rm -fr "harfbuzz-$HARFBUZZ"
		tar xf "harfbuzz-$HARFBUZZ.tar.gz"
		cd "harfbuzz-$HARFBUZZ"
		cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DBUILD_SHARED_LIBS=ON -DHB_BUILD_UTILS=OFF -B build -G Ninja
		cmake --build build --parallel
		ninja -C build install
		cd ..
	fi

	echo "Building FreeType with HarfBuzz..."
	rm -fr "freetype-$FREETYPE"
	tar xf "freetype-$FREETYPE.tar.xz"
	cd "freetype-$FREETYPE"
	cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DBUILD_SHARED_LIBS=ON -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON -DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_BROTLI=TRUE -DFT_REQUIRE_HARFBUZZ=TRUE -B build -G Ninja
	cmake --build build --parallel
	ninja -C build install
	cd ..
fi

echo "Building SDL..."
rm -fr "SDL3-$SDL3"
tar xf "SDL3-$SDL3.tar.gz"
cd "SDL3-$SDL3"
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DBUILD_SHARED_LIBS=ON -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

if [ "$SKIP_QT" != true ]; then
	# Couple notes:
	# -fontconfig is needed otherwise Qt Widgets render only boxes.
	# -qt-doubleconversion avoids a dependency on libdouble-conversion.
	# ICU avoids pulling in a bunch of large libraries, and hopefully we can get away without it.
	# OpenGL is needed to render window decorations in Wayland, apparently.
	echo "Building Qt Base..."
	rm -fr "qtbase-everywhere-src-$QT"
	tar xf "qtbase-everywhere-src-$QT.tar.xz"
	cd "qtbase-everywhere-src-$QT"
	patch -p1 < "$SCRIPTDIR/qtbase-disable-pcre2-jit.patch"
	mkdir build
	cd build
	../configure -prefix "$INSTALLDIR" -release -dbus-linked -gui -widgets -fontconfig -qt-doubleconversion -ssl -openssl-runtime -opengl desktop -qpa xcb,wayland -xkbcommon -xcb -gtk -- -DFEATURE_cups=OFF -DFEATURE_dbus=ON -DFEATURE_icu=OFF -DFEATURE_sql=OFF -DFEATURE_system_png=ON -DFEATURE_system_jpeg=ON -DFEATURE_system_zlib=ON -DFEATURE_system_freetype=ON -DFEATURE_system_harfbuzz=ON
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
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -B build -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building SPIRV-Cross..."
cd SPIRV-Cross
rm -fr build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DSPIRV_CROSS_SHARED=ON -DSPIRV_CROSS_STATIC=OFF -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_ENABLE_GLSL=ON -DSPIRV_CROSS_ENABLE_HLSL=OFF -DSPIRV_CROSS_ENABLE_MSL=OFF -DSPIRV_CROSS_ENABLE_CPP=OFF -DSPIRV_CROSS_ENABLE_REFLECT=OFF -DSPIRV_CROSS_ENABLE_C_API=ON -DSPIRV_CROSS_ENABLE_UTIL=ON -B build -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building cpuinfo..."
rm -fr "cpuinfo-$CPUINFO"
tar xf "cpuinfo-$CPUINFO.tar.gz"
cd "cpuinfo-$CPUINFO"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DCPUINFO_LIBRARY_TYPE=shared -DCPUINFO_RUNTIME_TYPE=shared -DCPUINFO_LOG_LEVEL=error -DCPUINFO_LOG_TO_STDIO=ON -DCPUINFO_BUILD_TOOLS=OFF -DCPUINFO_BUILD_UNIT_TESTS=OFF -DCPUINFO_BUILD_MOCK_TESTS=OFF -DCPUINFO_BUILD_BENCHMARKS=OFF -DUSE_SYSTEM_LIBS=ON -B build -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building discord-rpc..."
rm -fr "discord-rpc-$DISCORD_RPC"
tar xf "discord-rpc-$DISCORD_RPC.tar.gz"
cd "discord-rpc-$DISCORD_RPC"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DBUILD_SHARED_LIBS=ON -B build -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building plutosvg..."
rm -fr "plutosvg-$PLUTOSVG"
tar xf "plutosvg-$PLUTOSVG.tar.gz"
cd "plutosvg-$PLUTOSVG"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DBUILD_SHARED_LIBS=ON -DPLUTOSVG_ENABLE_FREETYPE=ON -DPLUTOSVG_BUILD_EXAMPLES=OFF -B build -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

echo "Building soundtouch..."
rm -fr "soundtouch-$SOUNDTOUCH"
tar xf "soundtouch-$SOUNDTOUCH.tar.gz"
cd "soundtouch-$SOUNDTOUCH"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON -B build -G Ninja
cmake --build build --parallel
ninja -C build install
cd ..

if [ "$SKIP_CLEANUP" != true ]; then
	echo "Cleaning up..."
	cd ..
	rm -fr deps-build
fi
