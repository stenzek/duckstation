#!/bin/bash

set -e

merge_binaries() {
  X86DIR=$1
  ARMDIR=$2
  echo "Merging ARM64 binaries from $ARMDIR into fat binaries at $X86DIR..."

  IFS="
"
  pushd "$X86DIR"
  for X86BIN in $(find . -type f \( -name '*.dylib' -o -name '*.a' -o -perm +111 \)); do
    if file "$X86DIR/$X86BIN" | grep "Mach-O " >/dev/null; then
      ARMBIN="${ARMDIR}/${X86BIN}"
      echo "Merge $ARMBIN to $X86BIN..."
      lipo -create "$X86BIN" "$ARMBIN" -o "$X86BIN"
    fi
  done
  popd
}

if [ "$#" -ne 1 ]; then
    echo "Syntax: $0 <output directory>"
    exit 1
fi

export MACOSX_DEPLOYMENT_TARGET=11.0
INSTALLDIR="$1"
NPROCS="$(getconf _NPROCESSORS_ONLN)"
SDL=SDL2-2.30.0
QT=6.6.0
MOLTENVK=1.2.6
ZSTD=1.5.5
PNG=1.6.43
JPEG=9f
WEBP=1.3.2

mkdir -p deps-build
cd deps-build

export PKG_CONFIG_PATH="$INSTALLDIR/lib/pkgconfig:$PKG_CONFIG_PATH"
export LDFLAGS="-L$INSTALLDIR/lib -dead_strip $LDFLAGS"
export CFLAGS="-I$INSTALLDIR/include -Os $CFLAGS"
export CXXFLAGS="-I$INSTALLDIR/include -Os $CXXFLAGS"

cat > SHASUMS <<EOF
36e2e41557e0fa4a1519315c0f5958a87ccb27e25c51776beb6f1239526447b0  $SDL.tar.gz
b6a3d179aa9c41275ed0e35e502e5e3fd347dbe5117a0435a26868b231cd6246  v$MOLTENVK.tar.gz
9c4396cc829cfae319a6e2615202e82aad41372073482fce286fac78646d3ee4  zstd-$ZSTD.tar.gz
6a5ca0652392a2d7c9db2ae5b40210843c0bbc081cbd410825ab00cc59f14a6c  libpng-$PNG.tar.xz
04705c110cb2469caa79fb71fba3d7bf834914706e9641a4589485c1f832565b  jpegsrc.v$JPEG.tar.gz
2a499607df669e40258e53d0ade8035ba4ec0175244869d1025d460562aa09b4  libwebp-$WEBP.tar.gz
039d53312acb5897a9054bd38c9ccbdab72500b71fdccdb3f4f0844b0dd39e0e  qtbase-everywhere-src-$QT.tar.xz
e1542cb50176e237809895c6549598c08587c63703d100be54ac2d806834e384  qtimageformats-everywhere-src-$QT.tar.xz
33da25fef51102f564624a7ea3e57cb4a0a31b7b44783d1af5749ac36d3c72de  qtsvg-everywhere-src-$QT.tar.xz
4e9feebc142bbb6e453e1dc3277e09ec45c8ef081b5ee2a029e6684b5905ba99  qttools-everywhere-src-$QT.tar.xz
a0d89a236f64b810eb0fe4ae1e90db22b0e86263521b35f89e69f1392815078c  qttranslations-everywhere-src-$QT.tar.xz
EOF

curl -L \
  -O "https://libsdl.org/release/$SDL.tar.gz" \
  -O "https://github.com/KhronosGroup/MoltenVK/archive/refs/tags/v$MOLTENVK.tar.gz" \
  -O "https://github.com/facebook/zstd/releases/download/v$ZSTD/zstd-$ZSTD.tar.gz" \
  -O "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$WEBP.tar.gz" \
  -O "https://downloads.sourceforge.net/project/libpng/libpng16/$PNG/libpng-$PNG.tar.xz" \
  -O "https://ijg.org/files/jpegsrc.v$JPEG.tar.gz" \
  -O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qtbase-everywhere-src-$QT.tar.xz" \
  -O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qtsvg-everywhere-src-$QT.tar.xz" \
  -O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qttools-everywhere-src-$QT.tar.xz" \
  -O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qtimageformats-everywhere-src-$QT.tar.xz" \
  -O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qttranslations-everywhere-src-$QT.tar.xz"

shasum -a 256 --check SHASUMS

echo "Installing SDL..."
tar xf "$SDL.tar.gz"
cd "$SDL"

# SDL seems fine with dual architectures.
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" -DSDL_X11=OFF -DBUILD_SHARED_LIBS=ON
make -C build "-j$NPROCS"
make -C build install
cd ..

# MoltenVK already builds universal binaries, nothing special to do here.
echo "Installing MoltenVK..."
tar xf "v$MOLTENVK.tar.gz"
cd "MoltenVK-${MOLTENVK}"
./fetchDependencies --macos
make macos
cp Package/Latest/MoltenVK/dylib/macOS/libMoltenVK.dylib "$INSTALLDIR/lib/"
cd ..

echo "Installing Qt Base..."
tar xf "qtbase-everywhere-src-$QT.tar.xz"
cd "qtbase-everywhere-src-$QT"

# since we don't have a direct reference to QtSvg, it doesn't deployed directly from the main binary
# (only indirectly from iconengines), and the libqsvg.dylib imageformat plugin does not get deployed.
# We could run macdeployqt twice, but that's even more janky than patching it.
patch -u src/tools/macdeployqt/shared/shared.cpp <<EOF
--- shared.cpp
+++ shared.cpp
@@ -1119,14 +1119,8 @@
         addPlugins(QStringLiteral("networkinformation"));
     }
 
-    // All image formats (svg if QtSvg is used)
-    const bool usesSvg = deploymentInfo.containsModule("Svg", libInfix);
-    addPlugins(QStringLiteral("imageformats"), [usesSvg](const QString &lib) {
-        if (lib.contains(QStringLiteral("qsvg")) && !usesSvg)
-            return false;
-        return true;
-    });
-
+    // All image formats
+    addPlugins(QStringLiteral("imageformats"));
     addPlugins(QStringLiteral("iconengines"));
 
     // Platforminputcontext plugins if QtGui is in use
EOF

mkdir build
cd build
cmake -G Ninja -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DCMAKE_BUILD_TYPE=Release -DFEATURE_optimize_size=ON -DFEATURE_dbus=OFF -DFEATURE_framework=OFF -DFEATURE_icu=OFF -DFEATURE_opengl=OFF -DFEATURE_printsupport=OFF -DFEATURE_sql=OFF -DFEATURE_gssapi=OFF ..
cmake --build . --parallel
ninja install
cd ../../

echo "Installing Qt SVG..."
tar xf "qtsvg-everywhere-src-$QT.tar.xz"
cd "qtsvg-everywhere-src-$QT"
mkdir build
cd build
"$INSTALLDIR/bin/qt-configure-module" ..
cmake --build . --parallel
ninja install
cd ../../

echo "Installing Qt Tools..."
tar xf "qttools-everywhere-src-$QT.tar.xz"
cd "qttools-everywhere-src-$QT"
# Linguist relies on a library in the Designer target, which takes 5-7 minutes to build on the CI
# Avoid it by not building Linguist, since we only need the tools that come with it
patch -u src/linguist/CMakeLists.txt <<EOF
--- src/linguist/CMakeLists.txt
+++ src/linguist/CMakeLists.txt
@@ -14,7 +14,7 @@
 add_subdirectory(lrelease-pro)
 add_subdirectory(lupdate)
 add_subdirectory(lupdate-pro)
-if(QT_FEATURE_process AND QT_FEATURE_pushbutton AND QT_FEATURE_toolbutton AND TARGET Qt::Widgets AND NOT no-png)
+if(QT_FEATURE_process AND QT_FEATURE_pushbutton AND QT_FEATURE_toolbutton AND TARGET Qt::Widgets AND TARGET Qt::PrintSupport AND NOT no-png)
     add_subdirectory(linguist)
 endif()
EOF
mkdir build
cd build
"$INSTALLDIR/bin/qt-configure-module" .. -- -DFEATURE_assistant=OFF -DFEATURE_clang=OFF -DFEATURE_designer=OFF -DFEATURE_kmap2qmap=OFF -DFEATURE_pixeltool=OFF -DFEATURE_pkg_config=OFF -DFEATURE_qev=OFF -DFEATURE_qtattributionsscanner=OFF -DFEATURE_qtdiag=OFF -DFEATURE_qtplugininfo=OFF ..
cmake --build . --parallel
ninja install
cd ../../

echo "Installing Qt Image Formats..."
tar xf "qtimageformats-everywhere-src-$QT.tar.xz"
cd "qtimageformats-everywhere-src-$QT"
mkdir build
cd build
"$INSTALLDIR/bin/qt-configure-module" ..
cmake --build . --parallel
ninja install
cd ../../

echo "Installing Qt Translations..."
tar xf "qttranslations-everywhere-src-$QT.tar.xz"
cd "qttranslations-everywhere-src-$QT"
mkdir build
cd build
"$INSTALLDIR/bin/qt-configure-module" ..
cmake --build . --parallel
ninja install
cd ../../

# Bit lame, but a custom install path breaks Qt's rcc :/
echo "Installing Zstd..."
tar xf "zstd-$ZSTD.tar.gz"
cd "zstd-$ZSTD"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DCMAKE_OSX_ARCHITECTURES="x86_64" -DBUILD_SHARED_LIBS=ON -DZSTD_BUILD_PROGRAMS=OFF -B build-dir build/cmake
make -C build-dir "-j$NPROCS"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DCMAKE_OSX_ARCHITECTURES="arm64" -DBUILD_SHARED_LIBS=ON -DZSTD_BUILD_PROGRAMS=OFF -B build-dir-arm64 build/cmake
make -C build-dir-arm64 "-j$NPROCS"
merge_binaries $(realpath build-dir) $(realpath build-dir-arm64)
make -C build-dir install
cd ..

echo "Installing libpng..."
rm -fr "libpng-$PNG"
tar xf "libpng-$PNG.tar.xz"
cd "libpng-$PNG"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DCMAKE_OSX_ARCHITECTURES="x86_64" -DBUILD_SHARED_LIBS=ON -DPNG_TESTS=OFF -DPNG_STATIC=OFF -DPNG_SHARED=ON -DPNG_TOOLS=OFF -B build
make -C build "-j$NPROCS"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DCMAKE_OSX_ARCHITECTURES="arm64" -DBUILD_SHARED_LIBS=ON -DPNG_TESTS=OFF -DPNG_STATIC=OFF -DPNG_SHARED=ON -DPNG_TOOLS=OFF -DPNG_ARM_NEON=on -B build-arm64
make -C build-arm64 "-j$NPROCS"
merge_binaries $(realpath build) $(realpath build-arm64)
make -C build install
cd ..

echo "Installing libjpeg..."
rm -fr "jpeg-$JPEG"
tar xf "jpegsrc.v$JPEG.tar.gz"
cd "jpeg-$JPEG"
mkdir build
cd build
../configure --prefix="$INSTALLDIR" --disable-static --enable-shared --host="x86_64-apple-darwin" CFLAGS="-arch x86_64"
make "-j$NPROCS"
cd ..
mkdir build-arm64
cd build-arm64
../configure --prefix="$INSTALLDIR" --disable-static --enable-shared --host="aarch64-apple-darwin" CFLAGS="-arch arm64"
make "-j$NPROCS"
cd ..
merge_binaries $(realpath build) $(realpath build-arm64)
make -C build install
cd ..

echo "Installing WebP..."
tar xf "libwebp-$WEBP.tar.gz"
cd "libwebp-$WEBP"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DCMAKE_OSX_ARCHITECTURES="x86_64" -B build \
  -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF \
  -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF -DBUILD_SHARED_LIBS=ON
make -C build "-j$NPROCS"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DCMAKE_OSX_ARCHITECTURES="arm64" -B build-arm64 \
  -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF \
  -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF -DBUILD_SHARED_LIBS=ON
make -C build-arm64 "-j$NPROCS"
merge_binaries $(realpath build) $(realpath build-arm64)
make -C build install
cd ..

echo "Cleaning up..."
cd ..
rm -fr deps-build
