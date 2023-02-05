#!/bin/bash

set -e

export MACOSX_DEPLOYMENT_TARGET=10.14
INSTALLDIR="$HOME/deps"
NPROCS="$(getconf _NPROCESSORS_ONLN)"
SDL=SDL2-2.26.2
QT=6.4.2
MOLTENVK=1.2.2
CURL=7.87.0

mkdir deps-build
cd deps-build

export PKG_CONFIG_PATH="$INSTALLDIR/lib/pkgconfig:$PKG_CONFIG_PATH"
export LDFLAGS="-L$INSTALLDIR/lib -dead_strip $LDFLAGS"
export CFLAGS="-I$INSTALLDIR/include -Os $CFLAGS"
export CXXFLAGS="-I$INSTALLDIR/include -Os $CXXFLAGS"

cat > SHASUMS <<EOF
95d39bc3de037fbdfa722623737340648de4f180a601b0afad27645d150b99e0  $SDL.tar.gz
8065a10c2d70b561f48475dedb118e643176527b162d6e439fa127270c2a07dd  v$MOLTENVK.tar.gz
8a063d664d1c23d35526b87a2bf15514962ffdd8ef7fd40519191b3c23e39548  curl-$CURL.tar.gz
a88bc6cedbb34878a49a622baa79cace78cfbad4f95fdbd3656ddb21c705525d  qtbase-everywhere-src-$QT.tar.xz
b746af3cb1793621d8ed7eae38d9ad5a15541dc2742031069f2ae3fe87590314  qtsvg-everywhere-src-$QT.tar.xz
a31387916184e4a5ef522d3ea841e8e931cc0f88be0824a7a354a572d5826c68  qttools-everywhere-src-$QT.tar.xz
bbe0291502c2604b72fef730e1935bd22f8b921d8c473250f298a723b2a9c496  qttranslations-everywhere-src-$QT.tar.xz
EOF

curl -L \
  -O "https://libsdl.org/release/$SDL.tar.gz" \
  -O "https://github.com/KhronosGroup/MoltenVK/archive/refs/tags/v$MOLTENVK.tar.gz" \
  -O "https://curl.se/download/curl-$CURL.tar.gz" \
  -O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qtbase-everywhere-src-$QT.tar.xz" \
  -O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qtsvg-everywhere-src-$QT.tar.xz" \
  -O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qttools-everywhere-src-$QT.tar.xz" \
  -O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qttranslations-everywhere-src-$QT.tar.xz"

shasum -a 256 --check SHASUMS

echo "Installing SDL..."
tar xf "$SDL.tar.gz"
cd "$SDL"

# Patch clang wrappers to require 10.14 for x64.
patch -u build-scripts/clang-fat.sh <<EOF
--- clang-fat.bak	2023-02-05 13:22:17.032581300 +1000
+++ clang-fat.sh	2023-02-05 13:23:15.668561400 +1000
@@ -6,12 +6,12 @@
 
 DEVELOPER="\`xcode-select -print-path\`/Platforms/MacOSX.platform/Developer"
 
-# Intel 64-bit compiler flags (10.9 runtime compatibility)
-CLANG_COMPILE_X64="clang -arch x86_64 -mmacosx-version-min=10.9 \\
--DMAC_OS_X_VERSION_MIN_REQUIRED=1070 \\
+# Intel 64-bit compiler flags (10.14 runtime compatibility)
+CLANG_COMPILE_X64="clang -arch x86_64 -mmacosx-version-min=10.14 \\
+-DMAC_OS_X_VERSION_MIN_REQUIRED=101400 \\
 -I/usr/local/include"
 
-CLANG_LINK_X64="-mmacosx-version-min=10.9"
+CLANG_LINK_X64="-mmacosx-version-min=10.14"
 
 # ARM 64-bit compiler flags (11.0 runtime compatibility)
 CLANG_COMPILE_ARM64="clang -arch arm64 -mmacosx-version-min=11.0 \\
EOF

patch -u build-scripts/clang++-fat.sh << EOF
--- clang++-fat.bak	2023-02-05 13:22:23.744491600 +1000
+++ clang++-fat.sh	2023-02-05 13:23:27.160575900 +1000
@@ -6,11 +6,11 @@
 
 DEVELOPER="\`xcode-select -print-path\`/Platforms/MacOSX.platform/Developer"
 
-# Intel 64-bit compiler flags (10.7 runtime compatibility)
-CLANG_COMPILE_X64="clang++ -arch x86_64 -mmacosx-version-min=10.7 \\
+# Intel 64-bit compiler flags (10.14 runtime compatibility)
+CLANG_COMPILE_X64="clang++ -arch x86_64 -mmacosx-version-min=10.14 \\
 -I/usr/local/include"
 
-CLANG_LINK_X64="-mmacosx-version-min=10.7"
+CLANG_LINK_X64="-mmacosx-version-min=10.14"
 
 # ARM 64-bit compiler flags (11.0 runtime compatibility)
 CLANG_COMPILE_ARM64="clang++ -arch arm64 -mmacosx-version-min=11.0 \\
EOF

CC="${PWD}/build-scripts/clang-fat.sh" CXX="${PWD}/build-scripts/clang++-fat.sh" ./configure --prefix "$INSTALLDIR" --without-x
make "-j$NPROCS"
make install
cd ..

echo "Installing curl..."
tar xf "curl-$CURL.tar.gz"
cd "curl-$CURL"
mkdir build-x64
cd build-x64
../configure --prefix "$INSTALLDIR" --with-secure-transport --without-brotli
make "-j$NPROCS"
make install
cd ..

# Build arm64, but don't install it, instead just add the arm64 binary into the existing x64 dylib.
mkdir build-arm64
cd build-arm64
CFLAGS="-arch arm64" ../configure --host x86_64-apple-darwin --prefix "$INSTALLDIR" --with-secure-transport --without-brotli
make "-j$NPROCS"
lipo -create "$INSTALLDIR/lib/libcurl.4.dylib" "lib/.libs/libcurl.4.dylib" -o "$INSTALLDIR/lib/libcurl.4.dylib"
cd ../..

# MoltenVK already builds universal binaries, nothing special to do here.
echo "Installing MoltenVK..."
tar xf "v$MOLTENVK.tar.gz"
cd "MoltenVK-${MOLTENVK}"
./fetchDependencies --macos
make macos
cp Package/Latest/MoltenVK/dylib/macOS/libMoltenVK.dylib $HOME/deps/lib/
cd ..

echo "Installing Qt Base..."
tar xf "qtbase-everywhere-src-$QT.tar.xz"
cd "qtbase-everywhere-src-$QT"
mkdir build
cd build
cmake -G Ninja -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DCMAKE_BUILD_TYPE=Release -DFEATURE_optimize_size=ON -DFEATURE_dbus=OFF -DFEATURE_framework=OFF -DFEATURE_icu=OFF -DFEATURE_opengl=OFF -DFEATURE_printsupport=OFF -DFEATURE_sql=OFF -DFEATURE_gssapi=OFF ..
cmake --build . --parallel
cmake --install .
cd ../../

echo "Installing Qt SVG..."
tar xf "qtsvg-everywhere-src-$QT.tar.xz"
cd "qtsvg-everywhere-src-$QT"
mkdir build
cd build
cmake -G Ninja -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DCMAKE_BUILD_TYPE=MinSizeRel ..
cmake --build . --parallel
cmake --install .
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
cmake -G Ninja -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DCMAKE_BUILD_TYPE=Release -DFEATURE_assistant=OFF -DFEATURE_clang=OFF -DFEATURE_designer=OFF -DFEATURE_kmap2qmap=OFF -DFEATURE_pixeltool=OFF -DFEATURE_pkg_config=OFF -DFEATURE_qev=OFF -DFEATURE_qtattributionsscanner=OFF -DFEATURE_qtdiag=OFF -DFEATURE_qtplugininfo=OFF ..
cmake --build . --parallel
cmake --install .
cd ../../

echo "Installing Qt Translations..."
tar xf "qttranslations-everywhere-src-$QT.tar.xz"
cd "qttranslations-everywhere-src-$QT"
mkdir build
cd build
cmake -G Ninja -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" -DCMAKE_PREFIX_PATH="$INSTALLDIR" -DCMAKE_INSTALL_PREFIX="$INSTALLDIR" -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --parallel
cmake --install .
cd ../../

echo "Cleaning up..."
cd ..
rm -fr deps-build
