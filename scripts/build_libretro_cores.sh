#!/usr/bin/env bash
SCRIPTDIR=$(dirname $(realpath "${BASH_SOURCE[0]}"))
ROOTDIR=$SCRIPTDIR/..
BUILDDIR=$SCRIPTDIR/../build-libretro
set -e

make_build() {
  PLATFORM=$1
  OPTIONS=$2
  SUFFIX=$3
  ZIPFILE=../duckstation_libretro_${PLATFORM}.zip

  echo Building for ${PLATFORM}...

  cd $BUILDDIR

  rm -fr $PLATFORM
  mkdir -p $PLATFORM
  cd $PLATFORM
  cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_LIBRETRO_CORE=ON $OPTIONS $ROOTDIR

  ninja
  rm -f $ZIPFILE
  zip -j $ZIPFILE duckstation_libretro${SUFFIX}.so
  cd ..
}
  
echo Creating build directory...
mkdir -p $BUILDDIR
cd $BUILDDIR || exit $?
rm -f duckstation_libretro_android_aarch64.zip duckstation_libretro_android_armv7.zip duckstation_libretro_linux_x64.zip duckstation_libretro_linux_aarch64.zip duckstation_libretro_linux_armv7.zip

echo Building...
make_build linux_x64 "" ""
make_build linux_aarch64 "-DCMAKE_TOOLCHAIN_FILE=$ROOTDIR/CMakeModules/aarch64-cross-toolchain.cmake" ""
make_build linux_armv7 "-DCMAKE_TOOLCHAIN_FILE=$ROOTDIR/CMakeModules/armv7-cross-toolchain.cmake" ""

make_build android_aarch64 "-DANDROID_ABI=arm64-v8a -DCMAKE_TOOLCHAIN_FILE=/home/user/Android/ndk-bundle/build/cmake/android.toolchain.cmake" "_android"
make_build android_armv7 "-DANDROID_ABI=armeabi-v7a -DANDROID_ARM_NEON=ON -DCMAKE_TOOLCHAIN_FILE=/home/user/Android/ndk-bundle/build/cmake/android.toolchain.cmake" "_android"

