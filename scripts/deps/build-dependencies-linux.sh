#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
# SPDX-License-Identifier: CC-BY-NC-ND-4.0

set -e

if [ "$#" -lt 1 ]; then
    echo "Syntax: $0 [-system-freetype] [-system-harfbuzz] [-system-libjpeg] [-system-libpng] [-system-libwebp] [-system-libzip] [-system-zstd] [-system-qt] [-skip-download] [-skip-cleanup] <output directory>"
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
	fi
done

SCRIPTDIR=$(realpath $(dirname "${BASH_SOURCE[0]}"))
NPROCS="$(getconf _NPROCESSORS_ONLN)"
INSTALLDIR="$1"
if [ "${INSTALLDIR:0:1}" != "/" ]; then
	INSTALLDIR="$PWD/$INSTALLDIR"
fi

FREETYPE=2.13.3
HARFBUZZ=10.1.0
LIBBACKTRACE=86885d14049fab06ef8a33aac51664230ca09200
LIBJPEGTURBO=3.0.4
LIBPNG=1.6.44
LIBWEBP=1.4.0
LIBZIP=1.11.2
SDL2=2.30.9
QT=6.8.1
ZSTD=1.5.6

CPUINFO=7524ad504fdcfcf75a18a133da6abd75c5d48053
DISCORD_RPC=144f3a3f1209994d8d9e8a87964a989cb9911c1e
LUNASVG=9af1ac7b90658a279b372add52d6f77a4ebb482c
SHADERC=1c0d3d18819aa75ec74f1fbd9ff0461e1b69a4d6
SOUNDTOUCH=463ade388f3a51da078dc9ed062bf28e4ba29da7
SPIRV_CROSS=vulkan-sdk-1.3.290.0

mkdir -p deps-build
cd deps-build

if [ "$SKIP_DOWNLOAD" != true ]; then
	curl -C - -L \
		-O "https://github.com/ianlancetaylor/libbacktrace/archive/$LIBBACKTRACE.tar.gz" \
		-O "https://github.com/libsdl-org/SDL/releases/download/release-$SDL2/SDL2-$SDL2.tar.gz" \
		-o "cpuinfo-$CPUINFO.tar.gz" "https://github.com/stenzek/cpuinfo/archive/$CPUINFO.tar.gz" \
		-o "discord-rpc-$DISCORD_RPC.tar.gz" "https://github.com/stenzek/discord-rpc/archive/$DISCORD_RPC.tar.gz" \
		-o "lunasvg-$LUNASVG.tar.gz" "https://github.com/stenzek/lunasvg/archive/$LUNASVG.tar.gz" \
		-o "shaderc-$SHADERC.tar.gz" "https://github.com/stenzek/shaderc/archive/$SHADERC.tar.gz" \
		-o "soundtouch-$SOUNDTOUCH.tar.gz" "https://github.com/stenzek/soundtouch/archive/$SOUNDTOUCH.tar.gz"
fi

cat > SHASUMS <<EOF
baf8aebd22002b762d803ba0e1e389b6b4415159334e9d34bba1a938f6de8ce6  $LIBBACKTRACE.tar.gz
24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4  SDL2-$SDL2.tar.gz
e1351218d270db49c3dddcba04fb2153b09731ea3fa6830e423f5952f44585be  cpuinfo-$CPUINFO.tar.gz
3eea5ccce6670c126282f1ba4d32c19d486db49a1a5cbfb8d6f48774784d310c  discord-rpc-$DISCORD_RPC.tar.gz
3998b024b0d442614a9ee270e76e018bb37a17b8c6941212171731123cbbcac7  lunasvg-$LUNASVG.tar.gz
3826d86f8a13564be1c047ac105041a3c5d0dc0bf826fe47cc582fe17a2ce7b1  shaderc-$SHADERC.tar.gz
fe45c2af99f6102d2704277d392c1c83b55180a70bfd17fb888cc84a54b70573  soundtouch-$SOUNDTOUCH.tar.gz
EOF

if [ "$SKIP_FREETYPE" != true ]; then
	if [ "$SKIP_DOWNLOAD" != true ]; then
		curl -C - -L -o "freetype-$FREETYPE.tar.xz" "https://sourceforge.net/projects/freetype/files/freetype2/$FREETYPE/freetype-$FREETYPE.tar.xz/download"
	fi
	cat >> SHASUMS <<EOF
0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289  freetype-$FREETYPE.tar.xz
EOF
fi
if [ "$SKIP_HARFBUZZ" != true ]; then
	if [ "$SKIP_DOWNLOAD" != true ]; then
		curl -C - -L -o "harfbuzz-$HARFBUZZ.tar.gz" "https://github.com/harfbuzz/harfbuzz/archive/refs/tags/$HARFBUZZ.tar.gz"
	fi
	cat >> SHASUMS <<EOF
c758fdce8587641b00403ee0df2cd5d30cbea7803d43c65fddd76224f7b49b88  harfbuzz-$HARFBUZZ.tar.gz
EOF
fi
if [ "$SKIP_LIBJPEG" != true ]; then
	if [ "$SKIP_DOWNLOAD" != true ]; then
		curl -C - -L -O "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$LIBJPEGTURBO/libjpeg-turbo-$LIBJPEGTURBO.tar.gz"
	fi
	cat >> SHASUMS <<EOF
99130559e7d62e8d695f2c0eaeef912c5828d5b84a0537dcb24c9678c9d5b76b  libjpeg-turbo-$LIBJPEGTURBO.tar.gz
EOF
fi
if [ "$SKIP_LIBPNG" != true ]; then
	if [ "$SKIP_DOWNLOAD" != true ]; then
		curl -C - -L -O "https://downloads.sourceforge.net/project/libpng/libpng16/$LIBPNG/libpng-$LIBPNG.tar.xz"
	fi
	cat >> SHASUMS <<EOF
60c4da1d5b7f0aa8d158da48e8f8afa9773c1c8baa5d21974df61f1886b8ce8e  libpng-$LIBPNG.tar.xz
EOF
fi
if [ "$SKIP_LIBWEBP" != true ]; then
	if [ "$SKIP_DOWNLOAD" != true ]; then
		curl -C - -L -O "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$LIBWEBP.tar.gz"
	fi
	cat >> SHASUMS <<EOF
61f873ec69e3be1b99535634340d5bde750b2e4447caa1db9f61be3fd49ab1e5  libwebp-$LIBWEBP.tar.gz
EOF
fi
if [ "$SKIP_LIBZIP" != true ]; then
	if [ "$SKIP_DOWNLOAD" != true ]; then
		curl -C - -L -O "https://github.com/nih-at/libzip/releases/download/v$LIBZIP/libzip-$LIBZIP.tar.xz"
	fi
	cat >> SHASUMS <<EOF
5d471308cef4c4752bbcf973d9cd37ba4cb53739116c30349d4764ba1410dfc1  libzip-$LIBZIP.tar.xz
EOF
fi
if [ "$SKIP_ZSTD" != true ]; then
	if [ "$SKIP_DOWNLOAD" != true ]; then
		curl -C - -L -O "https://github.com/facebook/zstd/releases/download/v$ZSTD/zstd-$ZSTD.tar.gz"
	fi
	cat >> SHASUMS <<EOF
8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1  zstd-$ZSTD.tar.gz
EOF
fi
if [ "$SKIP_QT" != true ]; then
	if [ "$SKIP_DOWNLOAD" != true ]; then
		curl -C - -L \
			-O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qtbase-everywhere-src-$QT.tar.xz" \
			-O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qtimageformats-everywhere-src-$QT.tar.xz" \
			-O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qtsvg-everywhere-src-$QT.tar.xz" \
			-O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qttools-everywhere-src-$QT.tar.xz" \
			-O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qttranslations-everywhere-src-$QT.tar.xz" \
			-O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qtwayland-everywhere-src-$QT.tar.xz" 
	fi
	cat >> SHASUMS <<EOF
40b14562ef3bd779bc0e0418ea2ae08fa28235f8ea6e8c0cb3bce1d6ad58dcaf  qtbase-everywhere-src-$QT.tar.xz
138cc2909aa98f5ff7283e36eb3936eb5e625d3ca3b4febae2ca21d8903dd237  qtimageformats-everywhere-src-$QT.tar.xz
3d0de73596e36b2daa7c48d77c4426bb091752856912fba720215f756c560dd0  qtsvg-everywhere-src-$QT.tar.xz
9d43d409be08b8681a0155a9c65114b69c9a3fc11aef6487bb7fdc5b283c432d  qttools-everywhere-src-$QT.tar.xz
635a6093e99152243b807de51077485ceadd4786d4acb135b9340b2303035a4a  qttranslations-everywhere-src-$QT.tar.xz
2226fbde4e2ddd12f8bf4b239c8f38fd706a54e789e63467dfddc77129eca203  qtwayland-everywhere-src-$QT.tar.xz
EOF
fi

shasum -a 256 --check SHASUMS

# Have to clone with git, because it does version detection.
if [ "$SKIP_DOWNLOAD" != true ]; then
	if [ ! -d "SPIRV-Cross" ]; then
		git clone https://github.com/KhronosGroup/SPIRV-Cross/ -b $SPIRV_CROSS --depth 1
	fi
fi

echo "Building libbacktrace..."
rm -fr "libbacktrace-$LIBBACKTRACE"
tar xf "$LIBBACKTRACE.tar.gz"
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

echo "Building SDL2..."
rm -fr "SDL2-$SDL2"
tar xf "SDL2-$SDL2.tar.gz"
cd "SDL2-$SDL2"
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DBUILD_SHARED_LIBS=ON -DSDL_SHARED=ON -DSDL_STATIC=OFF -G Ninja
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
@@ -14,12 +14,12 @@
 # Presumably because 6.0 ClangConfig.cmake files are not good enough?
 # In any case explicitly request a minimum version of 8.x for now, otherwise
 # building with CMake will fail at compilation time.
-qt_find_package(WrapLibClang 8 PROVIDED_TARGETS WrapLibClang::WrapLibClang)
+#qt_find_package(WrapLibClang 8 PROVIDED_TARGETS WrapLibClang::WrapLibClang)
 # special case end

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

echo "Building lunasvg..."
rm -fr "lunasvg-$LUNASVG"
tar xf "lunasvg-$LUNASVG.tar.gz"
cd "lunasvg-$LUNASVG"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DBUILD_SHARED_LIBS=ON -DLUNASVG_BUILD_EXAMPLES=OFF -B build -G Ninja
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
