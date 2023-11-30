#!/bin/bash

set -e

export MACOSX_DEPLOYMENT_TARGET=11.0
INSTALLDIR="$HOME/deps"
NPROCS="$(getconf _NPROCESSORS_ONLN)"
SDL=SDL2-2.28.5
QT=6.6.1
MOLTENVK=1.2.6

mkdir -p deps-build
cd deps-build

export PKG_CONFIG_PATH="$INSTALLDIR/lib/pkgconfig:$PKG_CONFIG_PATH"
export LDFLAGS="-L$INSTALLDIR/lib -dead_strip $LDFLAGS"
export CFLAGS="-I$INSTALLDIR/include -Os $CFLAGS"
export CXXFLAGS="-I$INSTALLDIR/include -Os $CXXFLAGS"

cat > SHASUMS <<EOF
332cb37d0be20cb9541739c61f79bae5a477427d79ae85e352089afdaf6666e4  $SDL.tar.gz
b6a3d179aa9c41275ed0e35e502e5e3fd347dbe5117a0435a26868b231cd6246  v$MOLTENVK.tar.gz
450c5b4677b2fe40ed07954d7f0f40690068e80a94c9df86c2c905ccd59d02f7  qtbase-everywhere-src-$QT.tar.xz
ac4ed08950072e375be662cfa64fdb447dd6e935cf29c56a4128d1500492188f  qtimageformats-everywhere-src-$QT.tar.xz
248deb56d26a463cf3162f530358ccf90cfb654bbf518bb35ddf81b205e09228  qtsvg-everywhere-src-$QT.tar.xz
4939105a7345ab4e19e7caee8654a836e65bd41910359623e0f233f3aff0914a  qttools-everywhere-src-$QT.tar.xz
668702e822ad7150b27e7caa2158595fd9b3b77ffbc8262e6509872a3920ee88  qttranslations-everywhere-src-$QT.tar.xz
EOF

curl -L \
  -O "https://libsdl.org/release/$SDL.tar.gz" \
  -O "https://github.com/KhronosGroup/MoltenVK/archive/refs/tags/v$MOLTENVK.tar.gz" \
  -O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qtbase-everywhere-src-$QT.tar.xz" \
  -O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qtsvg-everywhere-src-$QT.tar.xz" \
  -O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qttools-everywhere-src-$QT.tar.xz" \
  -O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qtimageformats-everywhere-src-$QT.tar.xz" \
  -O "https://download.qt.io/official_releases/qt/${QT%.*}/$QT/submodules/qttranslations-everywhere-src-$QT.tar.xz"

shasum -a 256 --check SHASUMS

echo "Installing SDL..."
tar xf "$SDL.tar.gz"
cd "$SDL"

# Patch clang wrappers to require 11.0 for x64.
patch -u build-scripts/clang-fat.sh <<EOF
--- clang-fat.bak	2023-02-05 13:22:17.032581300 +1000
+++ clang-fat.sh	2023-02-05 13:23:15.668561400 +1000
@@ -6,12 +6,12 @@
 
 DEVELOPER="\`xcode-select -print-path\`/Platforms/MacOSX.platform/Developer"
 
-# Intel 64-bit compiler flags (10.9 runtime compatibility)
-CLANG_COMPILE_X64="clang -arch x86_64 -mmacosx-version-min=10.9 \\
--DMAC_OS_X_VERSION_MIN_REQUIRED=1070 \\
+# Intel 64-bit compiler flags (11.0 runtime compatibility)
+CLANG_COMPILE_X64="clang -arch x86_64 -mmacosx-version-min=11.0 \\
+-DMAC_OS_X_VERSION_MIN_REQUIRED=101400 \\
 -I/usr/local/include"
 
-CLANG_LINK_X64="-mmacosx-version-min=10.9"
+CLANG_LINK_X64="-mmacosx-version-min=11.0"
 
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
+# Intel 64-bit compiler flags (11.0 runtime compatibility)
+CLANG_COMPILE_X64="clang++ -arch x86_64 -mmacosx-version-min=11.0 \\
 -I/usr/local/include"
 
-CLANG_LINK_X64="-mmacosx-version-min=10.7"
+CLANG_LINK_X64="-mmacosx-version-min=11.0"
 
 # ARM 64-bit compiler flags (11.0 runtime compatibility)
 CLANG_COMPILE_ARM64="clang++ -arch arm64 -mmacosx-version-min=11.0 \\
EOF

CC="${PWD}/build-scripts/clang-fat.sh" CXX="${PWD}/build-scripts/clang++-fat.sh" ./configure --prefix "$INSTALLDIR" --without-x
make "-j$NPROCS"
make install
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

echo "Cleaning up..."
cd ..
rm -fr deps-build
