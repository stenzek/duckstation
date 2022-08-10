#!/bin/sh

set -e

DEPS=$HOME/deps

if [ "$#" -ne 1 ]; then
  echo "Syntax: $0 <path to source directory>"
  exit 1
fi

# no realpath...
SOURCEDIR="$1"

echo "Build x64..."
mkdir build-x64
cd build-x64
export MACOSX_DEPLOYMENT_TARGET=10.14
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_NOGUI_FRONTEND=OFF -DBUILD_QT_FRONTEND=ON -DUSE_SDL2=ON -DCMAKE_PREFIX_PATH="$DEPS" -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON -G Ninja "../$SOURCEDIR"
cmake --build . --parallel
cd ..

echo "Build arm64..."
mkdir build-arm64
cd build-arm64
export MACOSX_DEPLOYMENT_TARGET=11.00
cmake -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release -DBUILD_NOGUI_FRONTEND=OFF -DBUILD_QT_FRONTEND=ON -DUSE_SDL2=ON -DENABLE_OPENGL=ON -DCMAKE_PREFIX_PATH="$DEPS" -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON -G Ninja "../$SOURCEDIR"
cmake --build . --parallel
cd ..

echo "Combine binary..."
unset MACOSX_DEPLOYMENT_TARGET
BINPATH=bin/DuckStation.app/Contents/MacOS/DuckStation
lipo -create "build-x64/$BINPATH" "build-arm64/$BINPATH" -o "build-x64/$BINPATH"

echo "Grab app..."
mv build-x64/bin/DuckStation.app .
rm -fr build-x64 build-arm64

echo "Sign binary with self-signed cert..."
codesign -s - --deep -f -v DuckStation.app
